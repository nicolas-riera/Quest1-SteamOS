#!/bin/sh
# "Frame mode" on the native Quest 1: boot straight into Steam's VR shell like the Steam Frame, with
# the Frame's dashboard settings and system power actions. See docs/frame-mode.md.
#   quest1-steamvr.service     SteamVR (vrserver + compositor) as steamos once Monado runs; before
#                              starting it applies the Frame dashboard settings (quest1-frame-settings)
#   quest1-vrsession.service   keep-alive OpenVR client (vrcmd --waitforquit), as vrstart.sh does
#   quest1-frame-dashboard.service  vrdashboard (SteamVR's dashboard process: power menu, systemui),
#                              then opens the dashboard (Steam's VR UI is the main panel)
#   power: logind lets steamos power off / reboot / "suspend" (polkit); suspend is replaced by
#   quest1-standby (no kernel suspend: SteamVR gets PrepareForSleep, /run/quest1-standby is set,
#   a power button press wakes); a short power button press = Sleep, a long press = power off
#   (logind's default for the Quest's qpnp_pon power key was an immediate power off).
# Steam itself (steam.service, steam-q1 -gamepadui) already serves its VR UI as SteamVR overlays
# (valve.steam.gamepadui.main/.bar/.frame.menu), the same non-gamescope path as the Frame's
# BServeVRGamepadUIOverlay; nothing to change there.
# Usage (on the headset, as root): holo-frame-mode.sh install|enable|disable|uninstall
#   install   writes everything, enables the boot units (takes effect at the next boot or with
#             quest1-frame restart); disable/enable only toggle the boot units; uninstall removes
#             all of it (steamvr.vrsettings keeps the dashboard keys, its pre-frame backup is in
#             ~steamos/.local/share/Steam/config/steamvr.vrsettings.pre-frame)
set -e
UNITS="quest1-steamvr.service quest1-vrsession.service quest1-frame-dashboard.service"

case "${1:-install}" in
enable)  systemctl enable $UNITS; exit 0 ;;
disable) systemctl disable $UNITS; exit 0 ;;
uninstall)
	systemctl disable --now $UNITS 2>/dev/null || true
	rm -f /etc/systemd/system/quest1-steamvr.service /etc/systemd/system/quest1-vrsession.service \
		/etc/systemd/system/quest1-frame-dashboard.service \
		/etc/systemd/system/systemd-suspend.service.d/quest1-standby.conf \
		/etc/systemd/system/reboot.target.d/quest1-timeout.conf \
		/etc/systemd/system/poweroff.target.d/quest1-timeout.conf \
		/etc/systemd/logind.conf.d/quest1-power.conf /etc/polkit-1/rules.d/50-quest1-power.rules \
		/usr/local/sbin/quest1-frame-settings /usr/local/sbin/quest1-standby /usr/local/sbin/quest1-frame
	systemctl daemon-reload
	echo "logind keeps its power key settings until restarted: systemctl restart systemd-logind"
	exit 0 ;;
install) ;;
*) echo "usage: $0 install|enable|disable|uninstall" >&2; exit 1 ;;
esac

# --- Frame dashboard settings ---------------------------------------------------------------------
cat > /usr/local/sbin/quest1-frame-settings <<'EOF'
#!/usr/bin/python3
# Applies the Steam Frame's dashboard settings (frame_hmd_additional.vrsettings of the Frame image)
# to steamvr.vrsettings. Run only while SteamVR is stopped (vrserver rewrites the file). Also clears
# SteamVR's safe mode after a crash, which would otherwise block driver_quest1 at the next start.
import glob, json, os, shutil, sys

P = "/home/steamos/.local/share/Steam/config/steamvr.vrsettings"
FRAME = {
    "dashboard": {
        # power menu (Steam's VR UI shows Sleep / Restart / Shut down; SteamVR calls logind)
        "allowSystemSleep": True,
        "allowSystemRestart": True,
        "allowSystemShutdown": True,
        "allowExitVR": True,
        "expectSteam": True,          # the dashboard waits for Steam instead of showing SteamVR's
        "preferGamepadMode": True,    # Steam's UI driven by the controllers' gamepad inputs
        "allowFreeTransform": True,
        "allowIncognitoMode": True,
        "allowQuickLaunchInVRGamepadUI": True,
        "showOnAppExit": True,
        "maxInlineTabs": 3,
        "scaleSliderMin": 0.5,
        "scaleSliderMax": 2.5,
        "laserRayWidthScale": 0.8,
        "allowPassthroughPortalManager": False,
        # the Frame does not render Steam's VR UI at the headset's refresh rate (CPU)
        "steamMatchesHMDFramerate": False,
    },
    "steamvr": {
        # no desktop-window webhelpers (SteamVR settings/bindings windows nobody sees here)
        "omitDesktopWebhelpers": True,
        "showDashboardSettings": True,
        "laserMouseAutoHideTimeout": 7.0,
    },
}

def main():
    if os.path.exists(P) and not os.path.exists(P + ".pre-frame"):
        shutil.copy2(P, P + ".pre-frame")
    d = json.load(open(P)) if os.path.exists(P) else {}
    for sec, kv in FRAME.items():
        d.setdefault(sec, {}).update(kv)
    # safe mode after a crash (same as /root/vrunsafe.py)
    s = d.setdefault("steamvr", {})
    s["enableSafeMode"] = False
    for k in [k for k in s if "crash" in k.lower() or ("safemode" in k.lower() and k != "enableSafeMode")]:
        del s[k]
    d.setdefault("driver_quest1", {})["blocked_by_safe_mode"] = False
    for f in glob.glob("/home/steamos/.local/share/Steam/logs/vrhw_*.vdf"):
        os.remove(f)
    tmp = P + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        json.dump(d, f, indent=3)
    st = os.stat(os.path.dirname(P))
    os.chown(tmp, st.st_uid, st.st_gid)
    os.replace(tmp, P)

if __name__ == "__main__":
    sys.exit(main())
EOF
chmod 755 /usr/local/sbin/quest1-frame-settings

# --- SteamVR at boot ------------------------------------------------------------------------------
ENV='Environment=HOME=/home/steamos USER=steamos XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus'

cat > /etc/systemd/system/quest1-steamvr.service <<EOF
[Unit]
Description=SteamVR (Frame mode: vrserver + vrcompositor with driver_quest1)
Requires=monado.service
Wants=steam.service quest1-vrsession.service quest1-frame-dashboard.service
After=monado.service xvfb.service steam.service

[Service]
User=steamos
$ENV
Environment=QUEST1_COMPAT_SPIRV=
ExecStartPre=+/usr/bin/install -d -o steamos -g steamos -m 700 /run/user/1000
ExecStartPre=+/usr/local/sbin/quest1-frame-settings
ExecStartPre=/bin/sh -c 'rm -f /home/steamos/.local/share/Steam/logs/vr*.txt'
ExecStart=/usr/local/bin/steamvr-q1 vrserver --keepalive
# vrcompositor, vrwebhelpers etc. are vrserver's children: stopping the unit stops all of SteamVR
KillMode=control-group
TimeoutStopSec=15
Restart=no

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/quest1-vrsession.service <<EOF
[Unit]
Description=SteamVR keep-alive client (vrcmd --waitforquit)
Requires=quest1-steamvr.service
After=quest1-steamvr.service
PartOf=quest1-steamvr.service

[Service]
User=steamos
$ENV
ExecStartPre=/bin/sleep 8
ExecStart=/usr/local/bin/steamvr-q1 vrcmd --waitforquit
Restart=no

[Install]
WantedBy=quest1-steamvr.service
EOF

cat > /etc/systemd/system/quest1-frame-dashboard.service <<EOF
[Unit]
Description=SteamVR dashboard process (Steam's VR UI as main panel), opened at start
Requires=quest1-steamvr.service
After=quest1-steamvr.service quest1-vrsession.service
PartOf=quest1-steamvr.service

[Service]
User=steamos
$ENV
Nice=5
# vrcompositor must be up first
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 30); do pgrep -u steamos -x vrcompositor >/dev/null && exit 0; sleep 2; done; exit 1'
ExecStartPre=/bin/sleep 5
ExecStart=/usr/local/bin/steamvr-q1 vrdashboard
ExecStartPost=-/bin/sh -c 'sleep 25; LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 20 /usr/local/bin/vrdash show'
Restart=no

[Install]
WantedBy=quest1-steamvr.service
EOF

# --- power menu / power button ---------------------------------------------------------------------
mkdir -p /etc/polkit-1/rules.d
cat > /etc/polkit-1/rules.d/50-quest1-power.rules <<'EOF'
// Frame mode: Steam and SteamVR (user steamos, no seat session) may power off, reboot and "suspend"
// (suspend is replaced by quest1-standby, see systemd-suspend.service.d/quest1-standby.conf).
polkit.addRule(function(action, subject) {
	if (subject.user != "steamos")
		return polkit.Result.NOT_HANDLED;
	if (/^org\.freedesktop\.login1\.(power-off|reboot|suspend)(-multiple-sessions|-ignore-inhibit)?$/.test(action.id))
		return polkit.Result.YES;
	return polkit.Result.NOT_HANDLED;
});
EOF

cat > /usr/local/sbin/quest1-standby <<'EOF'
#!/usr/bin/python3
# Replaces the kernel suspend (systemd-suspend.service) on the Quest 1: suspend-to-RAM of this
# Android 4.4 kernel without Android is untested. Instead, while this runs:
#  - logind has sent PrepareForSleep(true) (SteamVR's vrserver listens for it) and will send
#    PrepareForSleep(false) when we exit; logind ignores other power actions meanwhile
#  - /run/quest1-standby exists (for the display side: panels off until wake)
# A power button press (qpnp_pon KEY_POWER) wakes. QUEST1_STANDBY_MAX (seconds) bounds it.
import os, select, struct, sys, time

KEY_POWER, EV_KEY = 116, 1
EVENT = struct.Struct("llHHi")
FLAG = "/run/quest1-standby"

def devices():
    out = []
    for ev in sorted(os.listdir("/sys/class/input")):
        try:
            with open(f"/sys/class/input/{ev}/device/name") as f:
                if f.read().strip() == "qpnp_pon" and ev.startswith("event"):
                    out.append(f"/dev/input/{ev}")
        except OSError:
            pass
    return out

def main():
    t0 = time.monotonic()
    limit = float(os.environ.get("QUEST1_STANDBY_MAX", "0") or 0)
    with open(FLAG, "w") as f:
        f.write(f"{int(time.time())}\n")
    print("quest1-standby: sleeping until the power button is pressed", flush=True)
    fds = [os.open(p, os.O_RDONLY | os.O_NONBLOCK) for p in devices()]
    try:
        while True:
            left = None if limit <= 0 else max(0.0, limit - (time.monotonic() - t0))
            if left == 0.0:
                print("quest1-standby: time limit", flush=True)
                break
            ready, _, _ = select.select(fds, [], [], left)
            woke = False
            for fd in ready:
                try:
                    data = os.read(fd, EVENT.size * 64)
                except BlockingIOError:
                    continue
                for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    _, _, t, code, value = EVENT.unpack_from(data, off)
                    # ignore the press that put us to sleep: only presses 1 s after start count
                    if t == EV_KEY and code == KEY_POWER and value == 1 and time.monotonic() - t0 > 1.0:
                        woke = True
            if woke:
                print("quest1-standby: power button, waking up", flush=True)
                break
    finally:
        try:
            os.unlink(FLAG)
        except OSError:
            pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
EOF
chmod 755 /usr/local/sbin/quest1-standby

mkdir -p /etc/systemd/system/systemd-suspend.service.d
cat > /etc/systemd/system/systemd-suspend.service.d/quest1-standby.conf <<'EOF'
# Frame mode: "Sleep" is a standby, not a kernel suspend (see /usr/local/sbin/quest1-standby)
[Service]
ExecStart=
ExecStart=/usr/local/sbin/quest1-standby
TimeoutStartSec=infinity
EOF

# a stuck shutdown (seen once natively: loop-mounted root) ends in a forced reboot/power off
for t in reboot poweroff; do
	mkdir -p /etc/systemd/system/$t.target.d
	printf '[Unit]\nJobTimeoutSec=90s\nJobTimeoutAction=%s-force\n' $t > /etc/systemd/system/$t.target.d/quest1-timeout.conf
done

mkdir -p /etc/systemd/logind.conf.d
cat > /etc/systemd/logind.conf.d/quest1-power.conf <<'EOF'
# Frame mode: short press of the power button = Sleep (quest1-standby), long press = power off.
# (logind's default was an immediate power off on any press of the Quest's power key.)
[Login]
HandlePowerKey=suspend
HandlePowerKeyLongPress=poweroff
EOF

# --- control script --------------------------------------------------------------------------------
cat > /usr/local/sbin/quest1-frame <<'EOF'
#!/bin/sh
# quest1-frame start|stop|restart|status: the Frame-mode SteamVR units (Steam and Monado untouched).
# Restarts that others may be relying on should go through /root/q1restart.sh while developing.
U="quest1-steamvr.service quest1-vrsession.service quest1-frame-dashboard.service"
case "$1" in
start)   systemctl start quest1-steamvr.service ;;
stop)    systemctl stop $U ;;
restart) systemctl stop $U; pkill -x vrcompositor; pkill -x vrserver; sleep 2; systemctl start quest1-steamvr.service ;;
status)  systemctl --no-pager status $U | grep -E "^.? quest1|Active:" ;;
*) echo "usage: quest1-frame start|stop|restart|status" >&2; exit 1 ;;
esac
EOF
chmod 755 /usr/local/sbin/quest1-frame

systemctl daemon-reload
systemctl enable $UNITS
echo "Frame mode installed. logind picks up the power key settings at its next start (boot, or"
echo "systemctl restart systemd-logind); SteamVR starts at boot, or now with: quest1-frame start"
