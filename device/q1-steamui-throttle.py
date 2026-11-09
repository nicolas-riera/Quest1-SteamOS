#!/usr/bin/python3
"""q1-steamui-throttle.py hide|show|status: unmap/map Steam's Big Picture window (the screen-sized steamwebhelper top-level) on the Xvfb display.

While the SteamVR dashboard is hidden nobody sees Steam's UI, yet Chromium keeps animating it (GPU +
CPU). An unmapped X window makes Chromium treat the page as hidden (no frames); mapping it again
repaints it. Driven by q1-dashwatch (OpenVR dashboard events)."""
import ctypes, os, sys

x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                         ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                         ctypes.POINTER(ctypes.c_uint)]
x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XUnmapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

class Attr(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("w", ctypes.c_int), ("h", ctypes.c_int),
                ("border", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                ("root", ctypes.c_ulong), ("klass", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
                ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate", ctypes.c_long),
                ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]
x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(Attr)]

cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
d = x.XOpenDisplay(os.environ.get("DISPLAY", ":1").encode())
if not d:
    sys.exit("cannot open display")
root = x.XDefaultRootWindow(d)
import re, subprocess
out = subprocess.run(["xwininfo", "-root", "-children"], capture_output=True, text=True).stdout
scr = x.XDefaultScreen(d)
size = "%dx%d" % (x.XDisplayWidth(d, scr), x.XDisplayHeight(d, scr))
for m in re.finditer(r'^\s+(0x[0-9a-f]+) "[^"]*": \("steamwebhelper" "steam"\)\s+' + size, out, re.M):
    win = int(m.group(1), 16)
    a = Attr()
    x.XGetWindowAttributes(d, win, ctypes.byref(a))
    was = "mapped" if a.map_state else "unmapped"
    if cmd == "hide":
        x.XUnmapWindow(d, win)
    elif cmd == "show":
        x.XMapWindow(d, win)
    x.XSync(d, 0)
    print("window 0x%x was %s -> %s" % (win, was, cmd))
