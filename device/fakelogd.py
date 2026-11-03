# Minimal logd stand-in: receives liblog datagrams on /dev/socket/logdw and prints them (journal).
# Installed as /usr/local/lib/holo/fakelogd.py (fakelogd.service).
#
# Also the Touch controllers' battery levels: trackingservice's IInputHub keeps 100 % (in Android
# Meta's Java services forward the level to it), but the sensors HAL logs them: its cache dump
# when a controller connects ("id : <16 hex>" ... "battery state : 60%") and each change
# ("Controller <8 hex>xxxxxxxx battery level changed: 50% -> 40%"). They go to
# /run/q1track/ctlbattery, "<first 8 hex of the device id> <percent>" per line, for q1trackd.
import os, re, socket

p = '/dev/socket/logdw'
os.makedirs('/dev/socket', exist_ok=True)
try: os.unlink(p)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(p); os.chmod(p, 0o666)
prio = 'VDIWEF'

BAT = '/run/q1track/ctlbattery'
cache_re = re.compile(r'id\s*:\s*([0-9a-f]{8})[0-9a-f]{8}\b.*?battery state\s*:\s*(\d+)%', re.S)
change_re = re.compile(r'Controller ([0-9a-f]{8})\w* battery level changed: \d+% -> (\d+)%')
dead_re = re.compile(r'\[[lr]con \(\s*\w+\) ([0-9a-f]{8})\w*\].*-> paired_dead_battery')
levels = {}


def battery(msg):
    m = cache_re.search(msg) or change_re.search(msg) or dead_re.search(msg)
    if not m:
        return
    pct = int(m.group(2)) if m.re is not dead_re else 0
    if levels.get(m.group(1)) == pct:
        return
    levels[m.group(1)] = pct
    try:
        os.makedirs(os.path.dirname(BAT), exist_ok=True)
        with open(BAT + '.tmp', 'w') as f:
            f.writelines('%s %d\n' % kv for kv in levels.items())
        os.rename(BAT + '.tmp', BAT)
    except OSError:
        pass


while True:
    d = s.recv(65536)
    # header: u8 log_id, u16 tid, u32 sec, u32 nsec (11 bytes), then u8 prio, tag\0, msg\0
    body = d[11:]
    if len(body) < 2: continue
    pr = body[0]; rest = body[1:].split(b'\0')
    tag = rest[0].decode(errors='replace'); msg = rest[1].decode(errors='replace') if len(rest) > 1 else ''
    if tag.startswith('SyncBoss') and 'batter' in msg:
        battery(msg)
    print('%s/%s: %s' % (prio[pr - 2] if 2 <= pr <= 7 else '?', tag, msg.rstrip()), flush=True)
