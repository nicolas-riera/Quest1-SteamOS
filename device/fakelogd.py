# Minimal logd stand-in: receives liblog datagrams on /dev/socket/logdw and prints them (journal).
# Installed as /usr/local/lib/holo/fakelogd.py (fakelogd.service).
#
# Also the Touch controllers' battery levels: trackingservice's IInputHub keeps 100 % (in Android
# Meta's Java services forward the level to it), but the sensors HAL logs them: its cache dump
# when a controller connects ("id : <16 hex>" ... "battery state : 60%") and each change
# ("Controller <8 hex>xxxxxxxx battery level changed: 50% -> 40%"). They go to
# /run/q1track/ctlbattery, "<first 8 hex of the device id> <percent>" per line, for q1trackd.
#
# And the paired controllers: the sensors HAL logs "Syncboss input <16 hex> hardware type N, subtype
# 1|2, revision <rev>" for each, the revision empty until the controller is connected; they go to
# /run/q1track/controllers, "<id> <1 left|2 right> <1 connected|0 not yet>" per line, for ovrstub's
# OVRRemoteService stand-in, without which trackingservice reports no controller. ovrstub announces a controller
# once it is connected: trackingservice ignores every announcement after a first one it could not act on.
#
# The journal gets warnings and errors only, at most BURST lines per tag every 10 s (then a count of
# what was dropped): Meta's tracking logs ~300 lines/s (CONSTELLATION, VioInit...), which cost
# journald and this script CPU and pushed everything else out of the journal within minutes.
# FAKELOGD_LEVEL=V|D|I|W|E|F sets the lowest level printed (W by default).
#
# The tracking engine's map events (map context switches, map tracking gained/lost, relocalization; info lines, a
# few per minute at most) are always printed, and also appended to MAPLOG with the wall-clock time, which outlives
# the boot (the journal is volatile): what happened to the room's map when the headset was put on.
import os, re, socket, time

p = '/dev/socket/logdw'
os.makedirs('/dev/socket', exist_ok=True)
try: os.unlink(p)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(p); os.chmod(p, 0o666)
prio = 'VDIWEF'
min_pr = 2 + prio.index(os.environ.get('FAKELOGD_LEVEL', 'W')[:1].upper() or 'W')
BURST, WINDOW = 20, 10.0
window = {}  # tag -> [window start, lines printed, lines dropped]

MAPLOG = '/var/log/quest1-map.log'  # shared with q1anchord
MAPLOG_MAX = 256 * 1024
map_re = re.compile(r'Map Context|map context|[Mm]ap tracking|promoted|demoted|[Rr]elocaliz|MapDB|placeAnchor|'
                    r'persistAnchor|loadMap|removeAllMaps|Anchor(Import|Export)')


def maplog(line):
    try:
        if os.path.getsize(MAPLOG) > MAPLOG_MAX:
            os.replace(MAPLOG, MAPLOG + '.1')
    except OSError:
        pass
    try:
        with open(MAPLOG, 'a') as f:
            f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    except OSError:
        pass


BAT = '/run/q1track/ctlbattery'
cache_re = re.compile(r'id\s*:\s*([0-9a-f]{8})[0-9a-f]{8}\b.*?battery state\s*:\s*(\d+)%', re.S)
change_re = re.compile(r'Controller ([0-9a-f]{8})\w* battery level changed: \d+% -> (\d+)%')
dead_re = re.compile(r'\[[lr]con \(\s*\w+\) ([0-9a-f]{8})\w*\].*-> paired_dead_battery')
levels = {}

# The controllers' connection as the sensors HAL and trackingservice see it (a few lines per connection, info
# level): always printed, to follow a controller that never reaches SteamVR (2026-11-09).
ctl_re = re.compile(r'Syncboss input|stateChangeNotifierFunc|preparePairedControllerInfo|ControllerTrackingGlue|'
                    r'handleDevice(Connected|Disconnected)')

CTL = '/run/q1track/controllers'
input_re = re.compile(r'Syncboss input ([0-9a-f]{16}) hardware type \d+, subtype ([12]), revision ?(\S*)')
ctls = {}  # id -> (subtype, connected)


def write_atomic(path, lines):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + '.tmp', 'w') as f:
            f.writelines(lines)
        os.rename(path + '.tmp', path)
    except OSError:
        pass


def controller(msg):
    m = input_re.search(msg)
    if not m:
        return
    v = (m.group(2), 1 if m.group(3) else 0)
    if ctls.get(m.group(1)) != v:
        ctls[m.group(1)] = v
        write_atomic(CTL, sorted('%s %s %d\n' % (k, sub, c) for k, (sub, c) in ctls.items()))


def battery(msg):
    m = cache_re.search(msg) or change_re.search(msg) or dead_re.search(msg)
    if not m:
        return
    pct = int(m.group(2)) if m.re is not dead_re else 0
    if levels.get(m.group(1)) == pct:
        return
    levels[m.group(1)] = pct
    write_atomic(BAT, ['%s %d\n' % kv for kv in levels.items()])


while True:
    d = s.recv(65536)
    # header: u8 log_id, u16 tid, u32 sec, u32 nsec (11 bytes), then u8 prio, tag\0, msg\0
    body = d[11:]
    if len(body) < 2: continue
    pr = body[0]; rest = body[1:].split(b'\0')
    tag = rest[0].decode(errors='replace'); msg = rest[1].decode(errors='replace') if len(rest) > 1 else ''
    if tag.startswith('SyncBoss') and 'batter' in msg:
        battery(msg)
    if 'Syncboss input' in msg:
        controller(msg)
    if ctl_re.search(msg):
        print('%s/%s: %s' % (prio[pr - 2] if 2 <= pr <= 7 else '?', tag, msg.rstrip()), flush=True)
        continue
    if map_re.search(msg):
        line = '%s/%s: %s' % (prio[pr - 2] if 2 <= pr <= 7 else '?', tag, msg.rstrip())
        print(line, flush=True)
        maplog(line)
        continue
    if pr < min_pr:
        continue
    now = time.monotonic()
    w = window.get(tag)
    if w is None or now - w[0] >= WINDOW:
        if w and w[2]:
            print('fakelogd: %d lines from %s dropped in %.0f s' % (w[2], tag, now - w[0]), flush=True)
        w = window[tag] = [now, 0, 0]
    if w[1] >= BURST:
        w[2] += 1
        continue
    w[1] += 1
    print('%s/%s: %s' % (prio[pr - 2] if 2 <= pr <= 7 else '?', tag, msg.rstrip()), flush=True)
