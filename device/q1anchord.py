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
#      static  the world frame is the persisted one (or no map is saved yet): use it as is
#      anchor  a map is saved but the current one is another (relocalization failed): the driver
#              anchors the raw space on the head at put-on instead. Only until the saved map has been
#              found once in this run: after that, another current map is the engine's temporary one
#              after a tracking loss, and switching to "anchor" re-anchored the room on the head (the
#              whole room turned and moved) and again when the engine found the saved map back. It stays
#              "static" then: the engine snaps back into the saved frame when it relocalizes.
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
STATE = '/run/q1track/frame-state'
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
    for p in (MAPUUID, ANCHORUUID):
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


def main():
    worn_since = None
    chap_seen = chap_mtime()
    urgent_until = 0.0  # a playspace was just saved: save the map quickly until then
    last = ''
    sig, sig_since, backed = None, 0.0, None
    found, lost_logged = '', ''  # the saved map was the current one in this run; the map logged as temporary
    empty_since = None  # since when mapdb has been empty
    while True:
        try:
            if os.path.exists(CLEAR):
                os.unlink(CLEAR)
                forget()
            now = time.time()
            m = chap_mtime()
            if m is not None and m != chap_seen:
                log('q1anchord: playspace saved by SteamVR: saving the map now')
                urgent_until = now + SAVED_URGENT
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
            if saved and cur and cur != saved and found != saved:
                state = 'anchor'
            else:
                state = 'static'
            if saved and cur and cur != saved and found == saved and lost_logged != cur:
                log('q1anchord: current map %s is not the saved one (tracking lost?): staying static' % cur)
                lost_logged = cur
            if state != last:
                write_atomic(STATE, state)
                log('q1anchord: frame state %s (current map %s, saved %s)' % (state, cur or '-', saved or '-'))
                last = state
            if not os.path.exists(PANELS_OFF):
                worn_since = worn_since or now
            else:
                worn_since = None
            if not saved and cur and (urgent or (worn_since and now - worn_since > WORN_BEFORE_PERSIST)):
                persist()
        except Exception as e:  # keep running
            log('q1anchord: %r' % (e,))
        time.sleep(3)


if __name__ == '__main__':
    if sys.argv[1:] == ['restore']:
        restore()
    else:
        main()
