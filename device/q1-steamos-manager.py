#!/usr/bin/python3
"""q1-steamos-manager: the part of Valve's steamos-manager D-Bus API that Steam's quick menu uses, on the Quest 1.

Steam (steamui.so, gen.com.steampowered.SteamOSManager1.c) looks for the name com.steampowered.SteamOSManager1 and
shows "Manual GPU Clock" in the Performance tab only when the GpuPerformanceLevel1 interface is there (log line
"SteamOSManager: daemon not present" otherwise). The Steam Frame's real daemon (a Rust binary, not ours to copy)
is configured for its Snapdragon 8 Gen 3 by /usr/share/steamos-manager/devices/steam-frame.toml: a devfreq GPU
(min_freq 500 MHz, sysfs_path of the GPU's devfreq directory) and three custom performance profiles that cap the
CPU clusters (cpufreq max per policy) and the GPU clock. This does the same on the Adreno 540's KGSL devfreq
(/sys/class/kgsl/kgsl-3d0/devfreq) and the Kryo clusters (cpu0 = little, cpu4 = big).

Interfaces, all on /com/steampowered/SteamOSManager1 (Steam's gdbus object manager client; org.freedesktop.DBus.
ObjectManager is also served on / and /com/steampowered, in case the client roots it there):
  com.steampowered.SteamOSManager1.GpuPerformanceLevel1   AvailableGpuPerformanceLevels (as), GpuPerformanceLevel (s,
        rw: auto, low, high, manual), ManualGpuClock (u, rw, MHz), ManualGpuClockMin / ManualGpuClockMax (u, MHz)
  com.steampowered.SteamOSManager1.PerformanceProfile1    AvailablePerformanceProfiles (as), PerformanceProfile (s,
        rw), SuggestedDefaultPerformanceProfile (s)
The property types are those of upstream's XML (steamos-manager, com.steampowered.SteamOSManager1.xml) as far as the
Valve binary's strings show them; Steam's gdbus proxy would warn in its log if one were wrong.

Runs as steamos on Steam's session bus (the bus Steam's own process uses: DBUS_SESSION_BUS_ADDRESS in steam.service);
QUEST1_SM_BUS=system claims the name on the system bus instead (needs /usr/share/dbus-1/system.d/
com.steampowered.SteamOSManager1.conf, installed too). The sysfs files it writes are handed to steamos by
q1-steamos-manager-perms (ExecStartPre=+ of the unit). Settings are not persisted: Steam writes them again at each
start ("Set ..." lines in its steamui_system.txt).

device/holo-frame-perf.sh installs it. Config: /etc/quest1/steamos-manager.json (optional, see CONF_KEYS).
"""
import json
import os
import signal
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

NAME = "com.steampowered.SteamOSManager1"
PATH = "/com/steampowered/SteamOSManager1"
GPU = "/sys/class/kgsl/kgsl-3d0/devfreq"
CPU = "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq"
CONF = "/etc/quest1/steamos-manager.json"

# Defaults, overridden by CONF (same keys). The GPU's "auto" range is Android's: min_freq 257 MHz (the power HAL's
# reset to 180 MHz is masked, docs/tracking-6dof.md), max the top of available_frequencies (710 MHz). The profile
# caps follow the Frame's custom profiles in spirit (Performance = no cap; Balanced and Powersave = lower GPU and
# big-cluster ceilings); the values are a first guess, not measured (docs/frame-features.md).
CONF_KEYS = {
    "gpu_auto_min_mhz": 257,
    "profiles": {
        # name: {gpu_max_mhz or None, cpu policy: max kHz or None}
        "Performance": {"gpu_max_mhz": None, "cpu0": None, "cpu4": None},
        "Balanced": {"gpu_max_mhz": 596, "cpu0": None, "cpu4": 1958400},
        "Powersave": {"gpu_max_mhz": 515, "cpu0": 1478400, "cpu4": 1574400},
    },
    "default_profile": "Performance",
}

LEVELS = ["auto", "low", "high", "manual"]

XML_GPU = """<node><interface name="com.steampowered.SteamOSManager1.GpuPerformanceLevel1">
<property name="AvailableGpuPerformanceLevels" type="as" access="read"/>
<property name="GpuPerformanceLevel" type="s" access="readwrite"/>
<property name="ManualGpuClock" type="u" access="readwrite"/>
<property name="ManualGpuClockMin" type="u" access="read"/>
<property name="ManualGpuClockMax" type="u" access="read"/>
</interface></node>"""
XML_PROFILE = """<node><interface name="com.steampowered.SteamOSManager1.PerformanceProfile1">
<property name="AvailablePerformanceProfiles" type="as" access="read"/>
<property name="PerformanceProfile" type="s" access="readwrite"/>
<property name="SuggestedDefaultPerformanceProfile" type="s" access="read"/>
</interface></node>"""
XML_OM = """<node><interface name="org.freedesktop.DBus.ObjectManager">
<method name="GetManagedObjects"><arg name="objpath_interfaces_and_properties" type="a{oa{sa{sv}}}" direction="out"/></method>
<signal name="InterfacesAdded"><arg name="object_path" type="o"/><arg name="interfaces_and_properties" type="a{sa{sv}}"/></signal>
<signal name="InterfacesRemoved"><arg name="object_path" type="o"/><arg name="interfaces" type="as"/></signal>
</interface></node>"""

I_GPU = "com.steampowered.SteamOSManager1.GpuPerformanceLevel1"
I_PROFILE = "com.steampowered.SteamOSManager1.PerformanceProfile1"


def log(msg):
    print("q1-steamos-manager: " + msg, flush=True)


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
        return True
    except OSError as e:
        log("cannot write %s to %s: %s" % (value, path, e))
        return False


def load_conf():
    conf = json.loads(json.dumps(CONF_KEYS))
    try:
        with open(CONF) as f:
            user = json.load(f)
        conf.update({k: v for k, v in user.items() if k in conf})
    except (OSError, ValueError):
        pass
    return conf


class Manager:
    def __init__(self):
        self.conf = load_conf()
        freqs = sorted(int(x) for x in (read(GPU + "/available_frequencies") or "").split())
        # Hz -> MHz; without the table (not a KGSL device) the service still runs, with the A540's range
        self.freqs = [f // 1000000 for f in freqs] or [257, 342, 414, 515, 596, 670, 710]
        self.gpu_min = max(self.conf["gpu_auto_min_mhz"], self.freqs[0])
        self.gpu_max = self.freqs[-1]
        self.level = "auto"
        self.manual = self.gpu_max
        self.profile = self.conf["default_profile"]
        self.conn = None
        self.dirty = False  # nothing is written until Steam asks for something: the clocks stay as they are

    # --- policy: what the clocks should be ------------------------------------------------------------------
    def snap(self, mhz):
        mhz = max(self.gpu_min, min(self.gpu_max, int(mhz)))
        return min((f for f in self.freqs if f >= self.gpu_min), key=lambda f: abs(f - mhz))

    def gpu_range(self):
        cap = self.conf["profiles"].get(self.profile, {}).get("gpu_max_mhz")
        top = min(self.gpu_max, cap) if cap else self.gpu_max
        if self.level == "manual":
            f = self.snap(self.manual)
            return f, f
        if self.level == "low":
            return self.gpu_min, self.gpu_min
        if self.level == "high":
            return top, top
        return self.gpu_min, max(self.gpu_min, self.snap(top))

    def apply(self):
        self.dirty = True
        lo, hi = self.gpu_range()
        cur_lo = int(read(GPU + "/min_freq") or 0) // 1000000
        lo_hz, hi_hz = lo * 1000000, hi * 1000000
        # devfreq refuses min > max and max < min: move the bound that does not cross first
        if hi >= cur_lo:
            write(GPU + "/max_freq", hi_hz)
            write(GPU + "/min_freq", lo_hz)
        else:
            write(GPU + "/min_freq", lo_hz)
            write(GPU + "/max_freq", hi_hz)
        prof = self.conf["profiles"].get(self.profile, {})
        for policy in (0, 4):
            path = CPU % policy
            full = read(os.path.dirname(path) + "/cpuinfo_max_freq")
            cap = prof.get("cpu%d" % policy) or (int(full) if full else None)
            if cap:
                write(path, cap)
        log("level %s manual %d MHz profile %s -> GPU %d-%d MHz" % (self.level, self.manual, self.profile, lo, hi))

    # --- D-Bus ----------------------------------------------------------------------------------------------
    def props(self, iface):
        if iface == I_GPU:
            return {
                "AvailableGpuPerformanceLevels": GLib.Variant("as", LEVELS),
                "GpuPerformanceLevel": GLib.Variant("s", self.level),
                "ManualGpuClock": GLib.Variant("u", self.manual),
                "ManualGpuClockMin": GLib.Variant("u", self.gpu_min),
                "ManualGpuClockMax": GLib.Variant("u", self.gpu_max),
            }
        return {
            "AvailablePerformanceProfiles": GLib.Variant("as", list(self.conf["profiles"])),
            "PerformanceProfile": GLib.Variant("s", self.profile),
            "SuggestedDefaultPerformanceProfile": GLib.Variant("s", self.conf["default_profile"]),
        }

    def changed(self, iface, names):
        p = self.props(iface)
        self.conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              GLib.Variant("(sa{sv}as)", (iface, {n: p[n] for n in names}, [])))

    def get_property(self, conn, sender, path, iface, name):
        return self.props(iface)[name]

    def set_property(self, conn, sender, path, iface, name, value):
        try:
            if iface == I_GPU and name == "GpuPerformanceLevel":
                if value.get_string() not in LEVELS:
                    raise ValueError("unknown level %s" % value.get_string())
                self.level = value.get_string()
                names = ["GpuPerformanceLevel"]
            elif iface == I_GPU and name == "ManualGpuClock":
                self.manual = self.snap(value.get_uint32())
                names = ["ManualGpuClock"]
            elif iface == I_PROFILE and name == "PerformanceProfile":
                prof = value.get_string() or self.conf["default_profile"]
                if prof not in self.conf["profiles"]:
                    raise ValueError("unknown profile %s" % prof)
                self.profile = prof
                names = ["PerformanceProfile"]
            else:
                return False
        except ValueError as e:
            log(str(e))
            return False
        self.apply()
        GLib.idle_add(self.changed, iface, names)
        return True

    def managed_objects(self):
        # PerformanceProfile1 is not exported (user, 2026-11-17: on the Quest 1 the headset stays in its performance
        # profile; the quick menu's profile slider is hidden that way): only the manual GPU clock
        return GLib.Variant("(a{oa{sa{sv}}})", ({PATH: {I_GPU: self.props(I_GPU)}},))

    def om_call(self, conn, sender, path, iface, method, params, invocation):
        if method == "GetManagedObjects":
            invocation.return_value(self.managed_objects())
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", method)

    def on_bus(self, conn, name):
        self.conn = conn
        for xml, iface in ((XML_GPU, I_GPU),):
            info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
            conn.register_object(PATH, info, None, self.get_property, self.set_property)
        om = Gio.DBusNodeInfo.new_for_xml(XML_OM).interfaces[0]
        for p in ("/", "/com/steampowered", PATH):
            conn.register_object(p, om, self.om_call, None, None)

    def on_name(self, conn, name):
        log("name %s acquired (GPU %d-%d MHz, profiles %s)" % (name, self.gpu_min, self.gpu_max,
                                                               ", ".join(self.conf["profiles"])))
        # systemd (Type=notify): ready once the name is on the bus, so steam.service, ordered after us, finds it
        sock = os.environ.get("NOTIFY_SOCKET")
        if sock:
            import socket
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as u:
                u.connect(chr(0) + sock[1:] if sock.startswith("@") else sock)
                u.send(b"READY=1")

    def on_lost(self, conn, name):
        log("name %s lost or not available: exiting (systemd restarts)" % name)
        loop.quit()


loop = GLib.MainLoop()


def main():
    m = Manager()
    bus = Gio.BusType.SYSTEM if os.environ.get("QUEST1_SM_BUS") == "system" else Gio.BusType.SESSION
    Gio.bus_own_name(bus, NAME, Gio.BusNameOwnerFlags.NONE, m.on_bus, m.on_name, m.on_lost)
    for s in (signal.SIGTERM, signal.SIGINT):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, s, loop.quit)
    loop.run()
    # back to the defaults when the service stops: a pinned clock must not outlive the menu that set it
    if m.dirty:
        m.level, m.profile = "auto", m.conf["default_profile"]
        m.apply()


if __name__ == "__main__":
    sys.exit(main())
