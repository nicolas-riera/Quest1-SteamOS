#!/bin/sh
# The Quest 1 edits to SteamVR (the Steam Frame's arm64 SteamVR, copied by the user to /opt/steamvr: nothing
# of Valve's is shipped here). Each edit is anchored on a line of Valve's file and applied once, so this can
# run again after a SteamVR update replaces the files.
#  - bin/linuxarm64/vrcompositor-launcher.sh: vrcompositor alone gets the Vulkan shim and the SteamVR compat
#    layer (src/vklayer-steamvr): simulated display, timeline semaphores...
#  - bin/vrwebhelper/linuxarm64/vrwebhelper.sh: X display :1 (Xvfb, Steam's), vrclient on the Adreno like
#    vrcompositor (it shares the overlay textures with it), CEF remote debugging when /tmp/q1-webdebug exists,
#    and the dashboard's CEF processes at nice 10 so they yield to vrcompositor, Monado and sshd
#  - resources/safe_mode_driver_whitelist.json: "quest1", so safe mode after a crash keeps our driver
#  - /usr/local/bin/steamvr-q1 (the environment every SteamVR program runs with) and the /etc/quest1-direct-*
#    and /etc/quest1-warp-lens defaults
#  - the right controller's render model with the Oculus logo on its Oculus button (src/steamvr-quest1/mkrendermodel.py,
#    into driver_quest1's resources)
# Usage (root): holo-steamvr.sh install [steamvr dir, default $ROOT/opt/steamvr]
# ROOT=<dir> works on a system image from outside (the installer's build): only python3 is needed on the host.
set -e
[ "$1" = install ] || { echo "usage: $0 install [steamvr dir]" >&2; exit 1; }
R=${2:-$ROOT/opt/steamvr}
[ -x "$R/bin/linuxarm64/vrserver" ] || { echo "no SteamVR in $R" >&2; exit 1; }
python3 - "$R" <<'PY'
import json, os, sys
R = sys.argv[1]

def edit(rel, marker, steps):
    """steps: (anchor line, 'before'|'after'|'replace', text). Skipped when marker is already in the file."""
    p = os.path.join(R, rel)
    s = open(p, newline="").read()
    if marker in s:
        print("already done:", rel); return
    if not os.path.exists(p + ".orig"):
        open(p + ".orig", "w", newline="").write(s)
    lines = s.split("\n")
    for anchor, how, text in steps:
        i = next((i for i, l in enumerate(lines) if l.strip() == anchor), None)
        if i is None:
            sys.exit("%s: line %r not found (SteamVR changed?), nothing written" % (rel, anchor))
        new = text.split("\n")
        if how == "before": lines[i:i] = new
        elif how == "after": lines[i + 1:i + 1] = new
        else: lines[i:i + 1] = new
    st = os.stat(p)
    open(p, "w", newline="").write("\n".join(lines))
    os.chmod(p, st.st_mode)
    print("edited:", rel)

edit("bin/linuxarm64/vrcompositor-launcher.sh", "QUEST1_STEAMVR_COMPAT", [
    ('exec "$ROOT/vrcompositor-launcher" "$@"', "before",
     "# Quest 1: only vrcompositor needs the compat layer (simulated display, timeline semaphores...)\n"
     "export QUEST1_STEAMVR_COMPAT=1\n"
     "export LD_LIBRARY_PATH=/usr/local/lib/quest1-vk${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"),
])

edit("bin/vrwebhelper/linuxarm64/vrwebhelper.sh", "QUEST1_STEAMVR_COMPAT", [
    ('cd "${SCRIPT_DIR}"', "after",
     "\n# Quest 1: Steam and its X server (Xvfb) are on :1\n"
     "export DISPLAY=${DISPLAY:-:1}\n"
     "# Quest 1: vrclient in here shares the overlay textures with vrcompositor (OPAQUE_FD memory and\n"
     "# timeline semaphores), so it must run on the Adreno like vrcompositor: libhybris (inherited from\n"
     "# vrserver), the Vulkan shim with the SteamVR compat layer, and X11 surfaces from the shim for ANGLE.\n"
     "export LD_PRELOAD=/opt/hybris/lib/libbionictls.so\n"
     "export LD_LIBRARY_PATH=/usr/local/lib/quest1-vk-steam:/opt/hybris/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\n"
     "export QUEST1_STEAMVR_COMPAT=1 QUEST1_VKSHIM_WSI=1"),
    ('if ! timeout 1s xset q &>/dev/null; then', "before",
     "# Quest 1: debug hook, /tmp/q1-webdebug enables CEF remote debugging (127.0.0.1:9333)\n"
     "[ -e /tmp/q1-webdebug ] && set -- \"$@\" --remote-debugging-port=9333 --remote-allow-origins=*\n"),
    ('exec ./vrwebhelper "$@"', "replace",
     "\t# Quest 1: the dashboard CEF processes yield to vrcompositor, Monado and sshd\n"
     "\texec nice -n 10 ./vrwebhelper \"$@\""),
])

p = os.path.join(R, "resources/safe_mode_driver_whitelist.json")
s = open(p, newline="").read()
if "quest1" not in json.loads(s)["drivers"]:
    i = s.index("[", s.index('"drivers"')) + 1
    s = s[:i] + (' "quest1",' if s[i] == " " else '"quest1", ') + s[i:]
    json.loads(s)
    open(p, "w", newline="").write(s)
    print("edited: resources/safe_mode_driver_whitelist.json")
PY

# steamvr-q1: how every SteamVR program is started here (quest1-steamvr, -vrsession and -frame-dashboard units)
mkdir -p "$ROOT/usr/local/bin"
cat > "$ROOT/usr/local/bin/steamvr-q1" <<'Q1EOF'
#!/bin/sh
# steamvr-q1 [program args]: run a SteamVR program (default vrserver) on the Quest 1:
# hybris GPU env, Monado as the OpenXR runtime for driver_quest1, Vulkan through quest1_vkshim
# + the compat layer (timeline semaphores etc.)
S=/opt/steamvr/bin/linuxarm64
export QUEST1_STEAMVR_COMPAT=1
export QUEST1_COMPAT_SPIRV=${QUEST1_COMPAT_SPIRV:-}
# vrcompositor-launcher.sh puts quest1-vk (shim + compat layer) first for vrcompositor only
case "${1:-vrserver}" in
vrcompositor*) VK=/usr/local/lib/quest1-vk: ;;
*) VK= ;;
esac
export LD_LIBRARY_PATH=$VK$S:/opt/hybris/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so${LD_PRELOAD:+:$LD_PRELOAD}
export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-/opt/monado/share/openxr/1/openxr_monado.json}
# the Monado client looks for $XDG_RUNTIME_DIR/monado_comp_ipc
if [ "$(id -u)" = 0 ]; then
	export XDG_RUNTIME_DIR=/run/monado
else
	export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
	ln -sf /run/monado/monado_comp_ipc "$XDG_RUNTIME_DIR/monado_comp_ipc"
fi
export DISPLAY=${DISPLAY:-:1}
# direct scanout (quest1_scanout.c): vrcompositor distorts for the lenses and its 2880x1600 output goes
# straight to the panels; Monado only tracks. /etc/quest1-direct-scanout holds the default (0/1).
export QUEST1_DIRECT_SCANOUT=${QUEST1_DIRECT_SCANOUT:-$(cat /etc/quest1-direct-scanout 2>/dev/null || echo 0)}
# orientation knobs of the direct distortion (driver_quest1 DirectFlip), /etc/quest1-direct-flip
export QUEST1_DIRECT_FLIP=${QUEST1_DIRECT_FLIP:-$(cat /etc/quest1-direct-flip 2>/dev/null || echo 0)}
[ "$QUEST1_DIRECT_SCANOUT" = 1 ] && export QUEST1_DISPLAY_SIZE=${QUEST1_DISPLAY_SIZE:-2880x1600}
# vrcompositor output size, read by driver_quest1 and the simulated display (must agree)
export QUEST1_DISPLAY_SIZE=${QUEST1_DISPLAY_SIZE:-1836x1020}
[ -f /etc/quest1-direct-nofull ] && export QUEST1_DIRECT_NO_FULL_PASS=1 # debug
[ -f /etc/quest1-spirv-dump ] && export QUEST1_COMPAT_SPIRV=2 # debug
prog=${1:-vrserver}; [ $# -gt 0 ] && shift
# the "Oculus Quest" page of SteamVR's settings: vrserver reads its settings schema from VR_SETTINGSSCHEMA_FILE when
# set (a path relative to its resources directory); this SteamVR's own schema with driver_quest1's section added, made
# again at each start (SteamVR updates)
if [ "$prog" = vrserver ]; then
	Q=/opt/quest1-steamvr/quest1/resources/settings/quest1_settingsschema.json
	rel=$(python3 - "$S/../../resources" "$Q" "$XDG_RUNTIME_DIR/quest1-settingsschema.vrsettings" <<'PY'
import json, os, sys
res = os.path.realpath(sys.argv[1])
schema = json.load(open(os.path.join(res, "settingsschema.vrsettings")))
ours = json.load(open(sys.argv[2]))
schema = [s for s in schema if s.get("controller") != ours["controller"]]
at = next((i for i, s in enumerate(schema) if s.get("controller") == "startupshutdown_settings"), len(schema))
schema.insert(at, ours)
tmp = sys.argv[3] + ".tmp"
json.dump(schema, open(tmp, "w"), indent=1)
os.replace(tmp, sys.argv[3])
print(os.path.relpath(os.path.realpath(sys.argv[3]), res))
PY
) && export VR_SETTINGSSCHEMA_FILE=$rel
fi
exec $S/$prog "$@"
Q1EOF
chmod 755 "$ROOT/usr/local/bin/steamvr-q1"
# display defaults read by steamvr-q1 and quest1-frame-settings, kept if already set:
# direct scanout on (vrcompositor's output goes straight to the panels), its orientation flags (driver_quest1
# DirectFlip), and vrcompositor (not Monado's warp) doing the lens distortion
[ -f "$ROOT/etc/quest1-direct-scanout" ] || echo 1 > "$ROOT/etc/quest1-direct-scanout"
[ -f "$ROOT/etc/quest1-direct-flip" ] || echo 19 > "$ROOT/etc/quest1-direct-flip"
[ -f "$ROOT/etc/quest1-warp-lens" ] || echo 0 > "$ROOT/etc/quest1-warp-lens"
# the right controller's render model, with the Oculus logo on its Oculus button instead of the menu glyph
# (src/steamvr-quest1/mkrendermodel.py, made from this SteamVR's own models; driver_quest1 uses it when present)
M=$(dirname "$0")/../src/steamvr-quest1/mkrendermodel.py
[ -f "$M" ] || M=/usr/local/lib/holo/mkrendermodel.py
if [ -f "$M" ]; then python3 "$M" "$R" "$ROOT/opt/quest1-steamvr/quest1"; else echo "no mkrendermodel.py: Oculus button glyph left as is" >&2; fi
