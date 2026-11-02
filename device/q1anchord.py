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
#              anchors the raw space on the head at put-on instead
#  * `touch /tmp/quest1-clear-map` (or the Playspace menu's "Clear playspace" through the chaperone
#    file watch below) forgets everything: removeAllMaps, saved uuids deleted.
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


def configured():  # chaperone data with a universe (playspace set up)
    return '"standing"' in read(CHAP)


def forget():
    svc('call', 'TrackingEnvironment', '6')  # removeAllMaps
    for p in (MAPUUID, ANCHORUUID):
        try:
            os.unlink(p)
        except OSError:
            pass
    print('q1anchord: all maps removed', flush=True)


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
    print('q1anchord: persisted map %s anchor %s' % (m, u), flush=True)
    return True


def main():
    worn_since = None
    chap_seen = configured()
    last = ''
    while True:
        try:
            if os.path.exists(CLEAR):
                os.unlink(CLEAR)
                forget()
            # "Clear playspace" in the dashboard deletes the chaperone data: forget the map too
            have = configured()
            if chap_seen and not have:
                forget()
            chap_seen = have
            saved = read(MAPUUID)
            cur = string16(svc('call', 'TrackingEnvironment', '4'))
            if cur.startswith('00000000-0000'):
                cur = ''
            if saved and cur and cur != saved:
                state = 'anchor'
            else:
                state = 'static'
            if state != last:
                write_atomic(STATE, state)
                print('q1anchord: frame state %s (current map %s, saved %s)' % (state, cur or '-', saved or '-'), flush=True)
                last = state
            now = time.time()
            if not os.path.exists(PANELS_OFF):
                worn_since = worn_since or now
            else:
                worn_since = None
            if not saved and cur and worn_since and now - worn_since > WORN_BEFORE_PERSIST:
                persist()
        except Exception as e:  # keep running
            print('q1anchord: %r' % (e,), flush=True)
        time.sleep(3)


if __name__ == '__main__':
    main()
