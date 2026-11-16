"""Steam Link VR controller test, run on the PC (python -P) during a session: presses each Touch control on the headset
(driver_quest1's simulated press, /tmp/quest1-ctrl-press over ssh: works with the controllers asleep on a table) and
checks what the PC's SteamVR shows of it, through the PC's render model of that controller (the component that moves,
or turns touched / pressed): the controller a game gets.

  python -P vrlink-buttons.py [CONTROL ...]     e.g. "right trigger" "left x"; all of them by default
  QHOST=root@192.168.137.249 to reach the headset over Wi-Fi (default: USB, root@192.168.77.1)

Exits 1 if a control does not show on the PC's controller as expected (docs/streaming.md item 14)."""
import ctypes as C
import os
import subprocess
import sys
import time

HOST = os.environ.get("QHOST", "root@192.168.77.1")
OPENVR = r"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openvr_api.dll"

# control on the headset -> the PC's render model component expected to react, and how (moved, T touched, P pressed);
# None: nothing expected on the PC (the right Oculus button stays on the headset); "-": not checked, the PC's model
# draws the button's press but not its touch
TESTS = [
    ("right", "a", "button_a", "P"), ("right", "a-touch", "button_a", "-"),
    ("right", "b", "button_b", "P"), ("right", "b-touch", "button_b", "-"),
    ("right", "trigger", "trigger", "moved"), ("right", "trigger-touch", "trigger", "T"),
    ("right", "grip", "button_grip", "moved"),
    ("right", "stick-x", "thumbstick", "moved"), ("right", "stick-y", "thumbstick", "moved"),
    ("right", "stick", "thumbstick", "P"), ("right", "stick-touch", "thumbstick", "T"),
    ("left", "x", "button_x", "P"), ("left", "x-touch", "button_x", "-"),
    ("left", "y", "button_y", "P"), ("left", "y-touch", "button_y", "-"),
    ("left", "system", "button_enter", "P"),
    ("left", "trigger", "trigger", "moved"), ("left", "trigger-touch", "trigger", "T"),
    ("left", "grip", "button_grip", "moved"),
    ("left", "stick-x", "thumbstick", "moved"), ("left", "stick-y", "thumbstick", "moved"),
    ("left", "stick", "thumbstick", "P"), ("left", "stick-touch", "thumbstick", "T"),
    ("right", "system", None, None),  # last: it opens the SteamVR dashboard (closed again at the end)
]
DELAY, HOLD = 1.5, 1.5  # both controllers stay connected for the whole run ("connected" below)

dll = C.CDLL(OPENVR)
dll.VR_InitInternal2.restype = C.c_uint32
dll.VR_InitInternal2.argtypes = [C.POINTER(C.c_int), C.c_int, C.c_char_p]
dll.VR_GetGenericInterface.restype = C.c_void_p
dll.VR_GetGenericInterface.argtypes = [C.c_char_p, C.POINTER(C.c_int)]
err = C.c_int(0)
dll.VR_InitInternal2(C.byref(err), 3, None)  # background
if err.value:
    sys.exit(f"VR_Init: error {err.value} (is the PC's SteamVR running?)")


def table(name, n):
    p = dll.VR_GetGenericInterface(b"FnTable:" + name, C.byref(err))
    if not p:
        sys.exit(f"{name}: error {err.value}")
    return (C.c_void_p * n).from_address(p)


# openvr_capi.h function tables
fs, fi, fr = table(b"IVRSystem_022", 40), table(b"IVRInput_010", 8), table(b"IVRRenderModels_006", 16)
U64 = C.c_uint64
dev_class = C.CFUNCTYPE(C.c_int, C.c_uint32)(fs[19])
connected = C.CFUNCTYPE(C.c_bool, C.c_uint32)(fs[20])
get_string = C.CFUNCTYPE(C.c_uint32, C.c_uint32, C.c_int, C.c_char_p, C.c_uint32, C.POINTER(C.c_int))(fs[27])
poll_event = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.c_uint32)(fs[29])
source_handle = C.CFUNCTYPE(C.c_int, C.c_char_p, C.POINTER(U64))(fi[3])
component_count = C.CFUNCTYPE(C.c_uint32, C.c_char_p)(fr[9])
component_name = C.CFUNCTYPE(C.c_uint32, C.c_char_p, C.c_uint32, C.c_char_p, C.c_uint32)(fr[10])


class ComponentState(C.Structure):
    _fields_ = [("to_model", C.c_float * 12), ("to_local", C.c_float * 12), ("props", C.c_uint32)]


component_state = C.CFUNCTYPE(C.c_bool, C.c_char_p, C.c_char_p, U64, C.c_void_p,
                              C.POINTER(ComponentState))(fr[13])
event = C.create_string_buffer(256)
mode = C.create_string_buffer(8)


def prop(i, p):
    b = C.create_string_buffer(256)
    e = C.c_int(0)
    get_string(i, p, b, 256, C.byref(e))
    return b.value if not e.value else b""


def controller(hand):
    for i in range(16):
        if dev_class(i) == 2 and connected(i) and (b"Left" in prop(i, 1001)) == (hand == "left"):
            return i
    return None


def snapshot(hand, path):
    i = controller(hand)
    if i is None:
        return None
    model = prop(i, 1003)
    out = {}
    for k in range(component_count(model)):
        nb = C.create_string_buffer(128)
        component_name(model, k, nb, 128)
        cs = ComponentState()
        if component_state(model, nb.value, path, mode, C.byref(cs)):
            out[nb.value.decode()] = (tuple(round(v, 3) for v in cs.to_local), cs.props)
    return out


def run(hand, control):
    path = U64()
    source_handle(f"/user/hand/{hand}".encode(), C.byref(path))
    subprocess.run(["ssh", HOST, f"echo '{hand} {control} 1 {HOLD} {DELAY}' > /tmp/quest1-ctrl-press"], check=True)
    t0 = time.time()
    rest, seen = None, {}
    while time.time() - t0 < DELAY + HOLD + 1:
        while poll_event(event, 64):
            pass
        s = snapshot(hand, path.value)
        if s:
            if rest is not None and time.time() - t0 > DELAY + 0.3:
                for n, (loc, props) in s.items():
                    r = rest.get(n)
                    if r is None:
                        continue
                    if loc != r[0]:
                        seen.setdefault(n, set()).add("moved")
                    for flag, bit in (("T", 4), ("P", 8)):
                        if props & bit and not r[1] & bit:
                            seen.setdefault(n, set()).add(flag)
            if time.time() - t0 < DELAY - 0.3:
                rest = s  # the last sample before the press
        time.sleep(0.02)
    time.sleep(0.5)
    return rest is not None, seen


# SteamVR keeps the controllers for its dashboard while it is open (vrlink's GetControllerState fails, nothing reaches
# the PC): closed first as a player would, with the right controller's system button. Whether it is open is read from
# vrlink's log (the "system flags" it sends the PC: 1 1 1 open, 0 0 0 closed), not asked from SteamVR: a new OpenVR
# client (vrdash) opens the dashboard again. Both controllers stay connected for the whole run, as when held.
def dashboard_open():
    out = subprocess.run(["ssh", HOST, "grep VTE_DRIVER_SYSTEM_FLAGS /home/steamos/.local/share/Steam/logs/vrlink.txt"
                          " | tail -1"], capture_output=True, text=True).stdout
    return not out.strip().endswith("0 0 0")


def press_file(text):
    subprocess.run(["ssh", HOST, f"printf '{text}' > /tmp/quest1-ctrl-press"], check=True)


def close_dashboard():
    if dashboard_open():
        press_file("right system 1 0.2 0.5\n")
        time.sleep(3)
    return not dashboard_open()


press_file("left connected 1800\nright connected 1800\n")
time.sleep(5)  # both on the PC (a new controller takes ~3 s)
if not close_dashboard():
    press_file("left connected 0\nright connected 0\n")
    sys.exit("the SteamVR dashboard stays open: nothing would reach the PC")
wanted = [t for t in TESTS if len(sys.argv) < 2 or f"{t[0]} {t[1]}" in sys.argv[1:]]
failed = 0
for hand, control, comp, how in wanted:
    present, seen = run(hand, control)
    got = ", ".join(f"{n} {'/'.join(sorted(f))}" for n, f in sorted(seen.items())) or "nothing"
    if not present:
        ok, got = False, "controller not on the PC"
    elif how == "-":
        ok, got = not seen, got + " (its touch is not drawn: not checked)"
    elif comp is None:
        ok = not seen
    else:
        ok = how in seen.get(comp, set()) and all(n == comp for n in seen)
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {hand:5} {control:14} -> {got}" +
          ("" if ok else f"   (expected {comp} {how})" if comp else "   (expected nothing)"), flush=True)
close_dashboard()
press_file("left connected 0\nright connected 0\n")
dll.VR_ShutdownInternal()
print(f"{len(wanted) - failed}/{len(wanted)} as expected")
sys.exit(1 if failed else 0)
