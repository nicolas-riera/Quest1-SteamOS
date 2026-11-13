# pcmarker.py [DOWN_M] [SIDE_M] (on the PC, `python -P`, while a Steam Link VR session runs): the PC side's q1marker.
# A world-locked SteamVR overlay with a random pattern (another one than q1marker's), 1.5 m ahead of the head as the
# PC sees it when started, DOWN_M below (0.85) and SIDE_M to the right (0.5). The PC draws it into the stream's frames:
# next to q1marker's (drawn by the headset) in the same snapshot, the two must stay together while the head moves,
# but for what the stream's own latency explains (tools/headset-dev/README.md). Runs until killed.
import ctypes as C, math, random, sys, time

DLL = r'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openvr_api.dll'
down = float(sys.argv[1]) if len(sys.argv) > 1 else 0.85
side = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5

dll = C.CDLL(DLL)
dll.VR_InitInternal.restype = C.c_void_p
dll.VR_InitInternal.argtypes = [C.POINTER(C.c_int), C.c_int]
dll.VR_GetGenericInterface.restype = C.c_void_p
dll.VR_GetGenericInterface.argtypes = [C.c_char_p, C.POINTER(C.c_int)]
err = C.c_int(0)
dll.VR_InitInternal(C.byref(err), 2)  # VRApplication_Overlay
if err.value:
    sys.exit('init %d' % err.value)


def table(names):
    for v in names:
        e = C.c_int(0)
        p = dll.VR_GetGenericInterface(b'FnTable:' + v, C.byref(e))
        if p and not e.value:
            return p
    sys.exit('no ' + names[0].decode())


def fn(tab, i, res, *args):
    return C.cast(C.c_void_p.from_address(tab + i * 8).value, C.CFUNCTYPE(res, *args))


class M34(C.Structure):
    _fields_ = [('m', C.c_float * 12)]


class Pose(C.Structure):
    _fields_ = [('m', C.c_float * 12), ('vel', C.c_float * 3), ('ang', C.c_float * 3), ('result', C.c_int),
                ('valid', C.c_bool), ('conn', C.c_bool)]


sysT = table([b'IVRSystem_023', b'IVRSystem_022', b'IVRSystem_021'])
ovT = table([b'IVROverlay_028', b'IVROverlay_027'])
get_pose = fn(sysT, 11, None, C.c_int, C.c_float, C.POINTER(Pose), C.c_uint32)
create = fn(ovT, 1, C.c_int, C.c_char_p, C.c_char_p, C.POINTER(C.c_uint64))
set_width = fn(ovT, 22, C.c_int, C.c_uint64, C.c_float)
set_abs = fn(ovT, 33, C.c_int, C.c_uint64, C.c_int, C.POINTER(M34))
show = fn(ovT, 43, C.c_int, C.c_uint64)
set_raw = fn(ovT, 62, C.c_int, C.c_uint64, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32)

h = C.c_uint64(0)
r = create(b'quest1.pcmarker', b'pcmarker', C.byref(h))
if r:
    sys.exit('CreateOverlay %d' % r)
W, H = 128, 64
rnd = random.Random(3)
px = bytearray()
for _ in range(W * H):
    v = 230 if rnd.random() < 0.5 else 20
    px += bytes((v, v, v, 255))
buf = (C.c_ubyte * len(px)).from_buffer(px)
print('raw', set_raw(h, C.addressof(buf), W, H, 4), 'width', set_width(h, 0.8))
pose = (Pose * 1)()
get_pose(1, 0.0, pose, 1)  # TrackingUniverseStanding
m = pose[0].m
fx, fz = -m[2], -m[10]
n = math.hypot(fx, fz)
fx, fz = fx / n, fz / n
yaw = math.atan2(-fx, -fz)  # facing the head
c, s = math.cos(yaw), math.sin(yaw)
rx, rz = -fz, fx  # the head's right, level
t = M34((C.c_float * 12)(c, 0, s, m[3] + 1.5 * fx + side * rx, 0, 1, 0, m[7] - down, -s, 0, c, m[11] + 1.5 * fz + side * rz))
print('transform', set_abs(h, 1, C.byref(t)), 'show', show(h), flush=True)
while True:
    time.sleep(1)
