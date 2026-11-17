#!/usr/bin/python3
# q1anchord: keeps Meta's tracking map across restarts, like the stock OS (Guardian) does, through
# trackingservice's binder service "TrackingEnvironment" (docs/boundary.md). Run by
# android-q1anchord.service inside the tracking stack's environment.
#
#  * once the headset has been worn for a while (the map has grown), persists the world-origin anchor
#    (placeAnchor + persistAnchor): the engine promotes the current map to persistent and writes it to
#    /vision/insideout/mapdb; the map uuid is saved in /data/misc/tracking/world-map.uuid
#  * after a restart the engine loads that map and relocalizes by itself (current map uuid = saved one)
#  * writes /run/q1track/frame-state for driver_quest1 (chaperone_frame.cpp):
#      static  the saved map was seen as the current one in this engine session (or no map is saved yet): the
#              world frame is the persisted one, use it as is. It stays static after a tracking loss: another
#              current map is then the engine's temporary one, and switching to "anchor" re-anchored the room
#              on the head (the whole room turned and moved) and again when the saved map came back; the
#              engine snaps back into the saved frame when it relocalizes (map-state hides the room meanwhile).
#      unknown a map is saved but this engine session has not shown it as the current one yet (relocalization
#              normally takes 1-3 s). Never `static` on q1anchord's start alone.
#      anchor  a map is saved but not found after ~2 minutes worn (relocalization failed): the driver
#              anchors the raw space on the head at put-on instead.
#  * writes /run/q1track/map-state for driver_quest1: unknown | relocalizing | tracking | bad | lost, from the
#    engine's map events (the lines fakelogd copies to /var/log/quest1-map.log) and the current map uuid. The
#    driver shows the playspace only in `tracking` (and `bad` for a few seconds), like Guardian, which hides the
#    boundary while the headset relocalizes or has lost tracking. The file is rewritten every 3 s.
#  * `touch /tmp/quest1-clear-map` (SteamVR settings, Oculus Quest page: "Clear") forgets everything: removeAllMaps,
#    saved uuids deleted. "Clear playspace" only resets the boundary (vrserver writes a default 1x1 m one, the map stays).
#  * each time the user saves a playspace (vrserver rewrites chaperone_info.vrchap) the map is saved at once: persisted
#    then if none is yet (without waiting 45 s worn), and mapdb copied as soon as it has been quiet a few seconds
#    (instead of a minute), so the room the playspace was drawn in is on disk right away.
#  * the engine's MapDB checks its entries (CRC) and deletes the ones it finds corrupted: on 2026-11-07
#    ("MapDB: Deleting 3 corrupted entrees", most likely writes cut by a reboot) that emptied mapdb, while
#    the saved uuid stayed, so the frame stayed "anchor" for good and no map was ever saved again. Now:
#      - once the persisted map is the current one and mapdb has not changed for a minute, mapdb is copied
#        to /data/misc/tracking/mapdb.bak (same device, never off it);
#      - `q1anchord restore` (android-q1anchord-restore.service, before trackingservice) puts that copy back when
#        mapdb is empty;
#      - a mapdb empty for 30 s with a saved uuid (nothing to restore) is a lost map: the uuids are forgotten and
#        a new map is persisted after the next 45 s worn (the engine empties and rewrites mapdb within a second
#        when it saves it: a single empty read is not a loss).
#
# Map files hold camera-derived data: they stay under /vision and /data, never in the repo.
import os, re, struct, subprocess, sys, time

SVC = ['unshare', '-m', 'sh', '-c',
       'mount --bind /run/q1track/__properties__ /dev/__properties__ 2>/dev/null; '
       'exec env ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime '
       '/system/bin/service "$@"', 'svc']
MAPUUID = '/data/misc/tracking/world-map.uuid'
ANCHORUUID = '/data/misc/tracking/world-anchor.uuid'
ROOMUUIDS = '/data/misc/tracking/room-anchors.uuid'  # anchors the room is tied to (q1trackd, src/q1track/q1room.h)
ROOMREFS = '/data/misc/tracking/room-anchors.bin'  # their reference poses (written by q1trackd)
STATE = '/run/q1track/frame-state'
MAPSTATE = '/run/q1track/map-state'
CLEAR = '/tmp/quest1-clear-map'
PANELS_OFF = '/tmp/quest1-panels-off'
CHAP = '/home/steamos/.local/share/Steam/config/chaperone_info.vrchap'
WORN_BEFORE_PERSIST = 45.0
MAPDB = '/vision/insideout/mapdb'
MAPBAK = '/data/misc/tracking/mapdb.bak'
BACKUP_QUIET = 60.0  # mapdb unchanged this long before it is copied
BACKUP_QUIET_SAVED = 5.0  # the same just after the user saved a playspace
SAVED_URGENT = 120.0  # how long after a playspace save the quick saving applies
EMPTY_LOST = 30.0  # mapdb empty this long: the map is gone
UNFOUND_WORN = 120.0  # worn this long without the saved map becoming current: relocalization failed, anchor on the head
BAD_WINDOW = 10.0  # "bad" while bad-tracking lines of weight BAD_COUNT came within this (an "all submaps" one counts more)
BAD_COUNT = 3
ROOM_ANCHOR_EVERY = 10.0  # worn, tracking and the map saved: placeAnchor this often (it re-uses a nearby anchor)
ROOM_ANCHOR_MAX = 8
PERSIST_QUIET = 20.0  # tracking state and no bad map tracking this long before a map is persisted
SAVED_URGENT_PERSIST = 300.0  # how long after a playspace save a persist may wait for a good tracking moment
MAPDB_STAT_EVERY = 60.0


MAPLOG = '/var/log/quest1-map.log'  # shared with fakelogd (the engine's map events), outlives the boot


def log(msg):
    print(msg, flush=True)
    try:
        with open(MAPLOG, 'a') as f:
            f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
    except OSError:
        pass


def svc(*a):
    r = subprocess.run(SVC + list(a), capture_output=True, text=True, timeout=20)
    words = []
    for line in r.stdout.splitlines():
        m = re.match(r'\s*0x[0-9a-f]+: ((?:[0-9a-f]{8} ?)+)', line)
        if m:
            words += [int(w, 16) for w in m.group(1).split()]
        elif 'Parcel(' in line:  # short form: Parcel(00000000 00000000   '..')
            m = re.search(r'Parcel\(((?:[0-9a-f]{8} ?)+)', line)
            if m:
                words += [int(w, 16) for w in m.group(1).split()]
    return words


def string16(words):  # status, length, utf-16 data
    if len(words) < 2 or words[0] != 0:
        return ''
    n = words[1]
    if n <= 0 or n > 64:
        return ''
    b = b''.join(struct.pack('<I', w) for w in words[2:])
    return b[:2 * n].decode('utf-16le', 'replace')


def uuid_words(u):  # text uuid -> 4 signed ints for `service call`
    b = bytes.fromhex(u.replace('-', ''))
    return [struct.unpack('<i', b[i:i + 4])[0] for i in range(0, 16, 4)]


def uuid_from_words(ws):
    b = b''.join(struct.pack('<I', w) for w in ws[:4])
    h = b.hex()
    return '%s-%s-%s-%s-%s' % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def read(p):
    try:
        return open(p).read().strip()
    except OSError:
        return ''


def write_atomic(p, s):
    t = p + '.tmp'
    with open(t, 'w') as f:
        f.write(s + '\n')
    os.replace(t, p)


def mapdb_sig(d=MAPDB):  # (name, size, mtime) of every file, or () when there is none
    out = []
    for root, _, files in os.walk(d):
        for f in files:
            try:
                st = os.stat(os.path.join(root, f))
            except OSError:
                continue
            out.append((os.path.relpath(os.path.join(root, f), d), st.st_size, int(st.st_mtime)))
    return tuple(sorted(out))


def backup():
    tmp = MAPBAK + '.tmp'
    subprocess.run(['rm', '-rf', tmp], check=False)
    if subprocess.run(['cp', '-a', MAPDB, tmp]).returncode != 0:
        return False
    subprocess.run(['sync'], check=False)
    subprocess.run(['rm', '-rf', MAPBAK], check=False)
    os.rename(tmp, MAPBAK)
    log('q1anchord: map database copied (%d files)' % len(mapdb_sig(MAPBAK)))
    return True


def restore():  # before trackingservice starts
    if read(MAPUUID) and not mapdb_sig() and mapdb_sig(MAPBAK):
        subprocess.run(['cp', '-a', MAPBAK + '/.', MAPDB + '/'], check=False)
        subprocess.run(['sync'], check=False)
        log('q1anchord: mapdb was empty: restored the copy of map %s' % read(MAPUUID))


def chap_mtime():  # vrserver rewrites the chaperone file when a playspace is committed (or cleared)
    try:
        return os.stat(CHAP).st_mtime
    except OSError:
        return None


def forget():
    svc('call', 'TrackingEnvironment', '6')  # removeAllMaps
    drop_saved()
    log('q1anchord: all maps removed')


def drop_saved():
    for p in (MAPUUID, ANCHORUUID, ROOMUUIDS, ROOMREFS):
        try:
            os.unlink(p)
        except OSError:
            pass
    subprocess.run(['rm', '-rf', MAPBAK], check=False)


def persist():
    r = svc('call', 'TrackingEnvironment', '10')  # placeAnchor -> [status, nonnull, handle(version,index), ...]
    if len(r) < 4 or r[0] != 0 or r[1] != 1:
        return False
    h = svc('call', 'TrackingEnvironment', '13', 'i32', '1', 'i32', str(struct.unpack('<i', struct.pack('<I', r[2]))[0]),
            'i32', str(struct.unpack('<i', struct.pack('<I', r[3]))[0]))  # persistAnchor(handle) -> uuid
    if len(h) < 6 or h[0] != 0 or h[1] != 1:
        return False
    u = uuid_from_words(h[2:6])
    if u.startswith('00000000-0000'):
        return False
    time.sleep(2)
    m = string16(svc('call', 'TrackingEnvironment', '4'))
    if not m:
        return False
    os.makedirs(os.path.dirname(MAPUUID), exist_ok=True)
    write_atomic(ANCHORUUID, u)
    write_atomic(MAPUUID, m)
    log('q1anchord: persisted map %s anchor %s' % (m, u))
    return True


def room_uuids():
    return [l for l in read(ROOMUUIDS).splitlines() if l.strip()]


def place_room_anchor(handles):
    """placeAnchor returns the anchor nearby or makes one at the current submap; a new one is persisted and its uuid
    listed for q1trackd, which stores its pose in the room frame once the engine locates it (src/q1track/q1room.h).
    handles: the handles seen in this engine session (they restart with the engine)."""
    r = svc('call', 'TrackingEnvironment', '10')
    if len(r) < 4 or r[0] != 0 or r[1] != 1:
        return
    h = (r[2], r[3])
    if h in handles:
        return
    handles.add(h)
    u = persist_anchor(h)
    if not u or u == read(ANCHORUUID):
        return
    known = room_uuids()
    if u in known or len(known) >= ROOM_ANCHOR_MAX:
        return
    with open(ROOMUUIDS, 'a') as f:
        f.write(u + '\n')
    log('q1anchord: room anchor %d placed and persisted (handle %d)' % (len(known) + 1, h[0]))


def persist_anchor(h):  # persistAnchor(handle) -> uuid text or ''
    r = svc('call', 'TrackingEnvironment', '13', 'i32', '1', 'i32', str(struct.unpack('<i', struct.pack('<I', h[0]))[0]),
            'i32', str(struct.unpack('<i', struct.pack('<I', h[1]))[0]))
    if len(r) < 6 or r[0] != 0 or r[1] != 1:
        return ''
    u = uuid_from_words(r[2:6])
    return '' if u.startswith('00000000-0000') else u


class Engine:
    """What the engine's map events (quest1-map.log, written by fakelogd) say about tracking in this session."""

    def __init__(self):
        self.reset()
        self.f, self.ino = None, None
        try:
            st = os.stat(MAPLOG)
            self.f, self.ino = open(MAPLOG), st.st_ino
            self.f.seek(0, os.SEEK_END)  # only what comes from now on: older lines belong to older sessions
        except OSError:
            pass

    def reset(self):  # a new engine process (trackingservice restart): nothing is known yet
        self.reloc = False  # relocalizing into a saved map (isRelocalizing)
        self.lost = False  # map tracking lost ("Switching from Tracking to NoTracking")
        self.bad = []  # (time, weight) of bad map tracking lines
        self.last_bad = 0.0
        self.good_since = None  # since when the state has been `tracking`

    def feed(self, now):
        try:
            st = os.stat(MAPLOG)
            if self.f is None or st.st_ino != self.ino:  # first creation, or rotated by fakelogd
                if self.f:
                    self.f.close()
                self.f, self.ino = open(MAPLOG), st.st_ino
            data = self.f.read()
        except OSError:
            return
        for line in data.splitlines():
            if 'q1anchord:' not in line:
                self.event(line, now)

    def event(self, line, now):
        if 'Bad map tracking' in line or 'All submaps have bad map tracking' in line:
            m = re.search(r'Throttled count=(\d+)', line)
            w = 1 + (int(m.group(1)) if m else 0)
            if 'All submaps' in line:
                w += BAD_COUNT
            self.bad.append((now, w))
            self.last_bad = now
        elif 'Initializing a new Map Context' in line or 'isRelocalizing transitions false -> true' in line or \
                'Set isRelocalizing true' in line:
            self.reloc = True
        elif 'isRelocalizing transitions true -> false' in line or 'Relocalization + ViT succeeded' in line or \
                'Switching Map Context' in line:
            self.reloc = False
            self.lost = False
        elif 'Gained map tracking' in line or 'Starting map tracking on map' in line:
            self.lost = False
        elif 'Map tracking lost' in line or 'to NoTracking' in line:
            self.lost = True

    def is_bad(self, now):
        self.bad = [b for b in self.bad if now - b[0] < BAD_WINDOW]
        return sum(w for _, w in self.bad) >= BAD_COUNT


def engine_pid():
    r = subprocess.run(['pidof', 'trackingservice'], capture_output=True, text=True)
    return r.stdout.split()[0] if r.returncode == 0 and r.stdout.split() else ''


def mapdb_stats():  # file count and bytes only, never contents
    n = b = 0
    for root, _, files in os.walk(MAPDB):
        for f in files:
            try:
                b += os.stat(os.path.join(root, f)).st_size
                n += 1
            except OSError:
                pass
    return n, b


def main():
    worn_since = None
    chap_seen = chap_mtime()
    urgent_until = 0.0  # a playspace was just saved: save the map quickly until then
    urgent_persist = 0.0  # ... and persist it at the first good tracking moment until then
    last = ''
    sig, sig_since, backed = None, 0.0, None
    found, lost_logged = '', ''  # the saved map was the current one in this run; the map logged as temporary
    empty_since = None  # since when mapdb has been empty
    handles, last_place = set(), 0.0  # room anchors: handles placed this engine session
    eng, pid = Engine(), engine_pid()
    mstate, last_mstate, unfound_worn, last_stat, last_t, last_cur = 'unknown', '', 0.0, 0.0, time.time(), ''
    while True:
        try:
            if os.path.exists(CLEAR):
                os.unlink(CLEAR)
                forget()
                # the new map must grow for 45 s worn before it is persisted: the time worn before the clear
                # persisted it 3 s after (2026-11-12), when it held a few views from one spot
                worn_since = None
            now = time.time()
            p = engine_pid()
            if p != pid:  # trackingservice restarted: a new session, the saved map has to be found again
                pid, found, lost_logged, unfound_worn = p, '', '', 0.0
                handles.clear()
                last_place = now
                eng.reset()
                log('q1anchord: tracking engine restarted: the saved map has to be found again')
            eng.feed(now)
            m = chap_mtime()
            if m is not None and m != chap_seen:
                log('q1anchord: playspace saved by SteamVR: saving the map now')
                urgent_until = now + SAVED_URGENT
                urgent_persist = now + SAVED_URGENT_PERSIST
                backed = None  # copy mapdb again even if unchanged since the last copy
            chap_seen = m
            urgent = now < urgent_until
            saved = read(MAPUUID)
            cur = string16(svc('call', 'TrackingEnvironment', '4'))
            if cur.startswith('00000000-0000'):
                cur = ''
            s = mapdb_sig()
            # the engine rewrites every file of mapdb at once (deletes, then writes them again within a second): an
            # empty mapdb is a lost map only once it has stayed empty for a while (2026-11-07: one read during such a
            # rewrite dropped a good map and its copy)
            empty_since = (empty_since or now) if not s else None
            if saved and empty_since and now - empty_since > EMPTY_LOST:
                log('q1anchord: map %s is gone from mapdb (deleted by the engine?): a new one will be saved' % saved)
                drop_saved()
                saved = ''
            if s != sig:
                sig, sig_since = s, now
            elif saved and cur == saved and s and s != backed and \
                    now - sig_since > (BACKUP_QUIET_SAVED if urgent else BACKUP_QUIET):
                if backup():
                    backed = s
            if saved and cur == saved:
                found = saved
            worn = not os.path.exists(PANELS_OFF)
            if saved and cur and cur != saved and found != saved and worn:
                unfound_worn += now - last_t
            if not saved or found == saved:
                state = 'static'  # nothing to relocalize in, or the saved map was the current one in this session
            elif unfound_worn > UNFOUND_WORN:
                state = 'anchor'
            else:
                state = 'unknown'
            if saved and cur and cur != saved and found == saved and lost_logged != cur:
                log('q1anchord: current map %s is not the saved one (tracking lost?): frame stays static' % cur[:8])
                lost_logged = cur
            # map state: whether the engine's frame can be trusted right now
            if not cur:
                mstate = 'unknown'
            elif saved and cur != saved:
                mstate = 'lost' if eng.lost else 'relocalizing'
            else:
                if saved and last_cur != saved:
                    eng.reloc = False  # the saved map just became the current one
                if eng.lost:
                    mstate = 'lost'
                elif eng.reloc and saved:  # with no saved map the engine's reloc flag means nothing
                    mstate = 'relocalizing'
                elif eng.is_bad(now):
                    mstate = 'bad'
                else:
                    mstate = 'tracking'
            last_cur = cur
            eng.good_since = (eng.good_since or now) if mstate == 'tracking' else None
            if mstate != last_mstate:
                write_atomic(MAPSTATE, mstate)
                log('q1anchord: map state %s (current map %s, saved %s)' % (mstate, cur[:8] or '-', saved[:8] or '-'))
                last_mstate = mstate
            else:
                os.utime(MAPSTATE)  # the driver ignores a state that is no longer refreshed (q1anchord dead)
            if now - last_stat > MAPDB_STAT_EVERY:
                last_stat = now
                n, b = mapdb_stats()
                log('q1anchord: mapdb %d files, %d bytes' % (n, b))
            last_t = now
            if state != last:
                write_atomic(STATE, state)
                log('q1anchord: frame state %s (current map %s, saved %s)' % (state, cur or '-', saved or '-'))
                last = state
            if not os.path.exists(PANELS_OFF):
                worn_since = worn_since or now
            else:
                worn_since = None
            # A map is persisted only after tracking has been good for a while: one saved while tracking failed holds a
            # drifted frame (2026-11-12) and the room drawn in it is wrong after every boot. Never when a map is
            # already saved (the engine keeps it up to date itself).
            good = mstate == 'tracking' and eng.good_since and now - eng.good_since > PERSIST_QUIET and \
                now - eng.last_bad > PERSIST_QUIET
            if not saved and cur and good and (now < urgent_persist or
                                               (worn_since and now - worn_since > WORN_BEFORE_PERSIST)):
                persist()
            # room anchors: once the saved map is the current one and tracking is good, keep placing anchors where
            # the user is (a new one only when none is near)
            if saved and cur == saved and good and worn_since and now - last_place > ROOM_ANCHOR_EVERY and                     len(room_uuids()) < ROOM_ANCHOR_MAX:
                last_place = now
                place_room_anchor(handles)
        except Exception as e:  # keep running
            log('q1anchord: %r' % (e,))
        time.sleep(3)


if __name__ == '__main__':
    if sys.argv[1:] == ['restore']:
        restore()
    else:
        main()
