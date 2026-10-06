#!/bin/sh
# Steam session on the native Quest 1: the arm64 Steam client (Big Picture) on a virtual X server,
# shown in the headset by xscreen (an OpenXR quad layer on Monado) and controllable from the PC over VNC.
#   xvfb.service      Xvfb :1, 1280x800, framebuffer exported as an XWD file in /run/xvfb
#   steam.service     steam-q1 -gamepadui as steamos (restarts itself after client self-updates)
#   x11vnc.service    VNC on port 5900, password in /etc/x11vnc/passwd
#   steam-screen.service  shows the Steam screen in VR (xscreen); the Monado target keeps the
#                     panels off while the proximity sensor says the headset is not worn
# The webhelper's GPU process (Chromium) renders on the Adreno: ANGLE Vulkan through libhybris, with
# X11 presentation by quest1_vkshim (docs/steam-gpu.md). QUEST1_STEAM_GPU=0 in steam.service's
# environment goes back to Mesa llvmpipe.
# Usage (on the headset): [VKSRC=<src/vklayer-steamvr copy>] holo-steam-session.sh install
#   VKSRC: build and install the Vulkan shim + dlopen redirect into /usr/local/lib/quest1-vk-steam
set -e

# --- Steam launcher -------------------------------------------------------------------------------
cat > /usr/local/bin/steam-q1 <<'EOF'
#!/bin/bash
# Launch the arm64 Steam client the way the Frame RUNSTEAM.sh does (links + env), with our args.
S=$HOME/.local/share/Steam
mkdir -p ~/.steam
ln -sTfn $S ~/.steam/steam; ln -sTfn $S ~/.steam/root
ln -sTfn $S/linux32 ~/.steam/sdk32; ln -sTfn $S/linux64 ~/.steam/sdk64
ln -sTfn $S/linuxarm64 ~/.steam/sdkarm64; ln -sTfn $S/steamrtarm64 ~/.steam/binarm64
ln -sTfn $S/ubuntu12_32 ~/.steam/bin32; ln -sTfn $S/ubuntu12_64 ~/.steam/bin64
export LD_LIBRARY_PATH=$S/steamrtarm64 STEAM_RUNTIME=$S/ubuntu12_32/steam-runtime
# webhelper GPU process on the Adreno (ANGLE Vulkan via libhybris, docs/steam-gpu.md);
# QUEST1_STEAM_GPU=0 goes back to Mesa llvmpipe. Steam's xcomposite workaround re-composites
# every CEF window with GLX on llvmpipe (~2 cores while the UI animates): off by default here,
# QUEST1_STEAM_CEF_ARGS= (empty) keeps it.
if [ "${QUEST1_STEAM_GPU:-1}" = 1 ]; then
	export STEAM_CEF_GPU_CMD_PREFIX=steam-webhelper-gpu
	set -- -cef-disable-gpu-sandbox ${QUEST1_STEAM_CEF_ARGS--cef-disable-xcomposite-workaround} "$@"
fi
# exit status 42 = "restart me" (after a self-update), as steam.sh handles it
while :; do
	$S/steamrtarm64/steam "$@"
	rc=$?
	[ $rc -eq 42 ] || exit $rc
	echo "steam-q1: client asked for a restart"
done
EOF
chmod 755 /usr/local/bin/steam-q1

# --- webhelper GPU process on the Adreno ----------------------------------------------------------
cat > /usr/local/bin/steam-webhelper-gpu <<'EOF'
#!/bin/sh
# GPU process launcher for the Steam webhelper (STEAM_CEF_GPU_CMD_PREFIX, set by steam-q1; Steam
# quotes the value, so it is found through PATH as the symlink literally named "'steam-webhelper-gpu'").
# Chromium/ANGLE on the Adreno 540 via libhybris: ANGLE's Vulkan backend, X11 presentation by
# quest1_vkshim (QUEST1_VKSHIM_WSI), ANGLE's dlopen of its bundled libvulkan redirected to the shim.
# ANGLE features for the Adreno 512.555 blob: its vertex-binding-stride dynamic state draws nothing
# (supportsExtendedDynamicState off), flipped vkCmdBlitImage is wrong (disableFlippingBlitWithCommand).
# Extra GPU process args: QUEST1_STEAM_GPU_ARGS. See docs/steam-gpu.md.
V=/usr/local/lib/quest1-vk-steam
export LD_LIBRARY_PATH=$V${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so:$V/libquest1_vkredirect.so${LD_PRELOAD:+:$LD_PRELOAD}
export QUEST1_VKSHIM_WSI=1 QUEST1_VKSHIM_LAYER=none QUEST1_VK_REDIRECT=$V/libvulkan.so.1
exec "$@" --use-angle=vulkan --disable-angle-features=supportsExtendedDynamicState \
	--enable-angle-features=disableFlippingBlitWithCommand $QUEST1_STEAM_GPU_ARGS
EOF
chmod 755 /usr/local/bin/steam-webhelper-gpu
ln -sfn steam-webhelper-gpu "/usr/local/bin/'steam-webhelper-gpu'"

if [ -n "$VKSRC" ]; then
	V=/usr/local/lib/quest1-vk-steam
	mkdir -p $V
	# -z nodelete: Chromium unloads Vulkan after probing it and loads it again for ANGLE; a second
	# copy of the shim would find the hybris loader already initialized by the first one
	gcc -std=gnu11 -O2 -Wall -Wno-unused-parameter -fPIC -shared -fvisibility=hidden \
		-Wl,-soname,libvulkan.so.1 -Wl,-z,nodelete -o $V/libvulkan.so.1 \
		"$VKSRC/quest1_vkshim.c" "$VKSRC/quest1_wsi_x11.c" -ldl -lpthread -lxcb -lxcb-shm
	gcc -std=gnu11 -O2 -Wall -fPIC -shared -o $V/libquest1_vkredirect.so "$VKSRC/quest1_vkredirect.c" -ldl
fi

# the GPU (kgsl) and ion for the render group (steamos); binder nodes are opened too (gralloc)
cat > /etc/udev/rules.d/70-quest1-gpu.rules <<'EOF'
# Adreno GPU (Android blob via libhybris) for members of render, e.g. the Steam webhelper GPU process
KERNEL=="kgsl-3d0", GROUP="render", MODE="0660"
KERNEL=="ion", GROUP="render", MODE="0660"
EOF
if [ ! -f /etc/udev/rules.d/60-quest1-binder.rules ]; then # also written by holo-monado.sh
	echo 'KERNEL=="binder|hwbinder|vndbinder", MODE="0666"' > /etc/udev/rules.d/60-quest1-binder.rules
	chmod 666 /dev/binder /dev/hwbinder /dev/vndbinder
fi
chgrp render /dev/kgsl-3d0 /dev/ion && chmod 660 /dev/kgsl-3d0 /dev/ion

cat > /etc/systemd/system/xvfb.service <<'EOF'
[Unit]
Description=Virtual X server for the Steam client (:1)

[Service]
ExecStartPre=/usr/bin/mkdir -p /run/xvfb
ExecStart=/usr/sbin/Xvfb :1 -screen 0 1280x800x24 -fbdir /run/xvfb -nolisten tcp
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/steam.service <<'EOF'
[Unit]
Description=Steam client (arm64, Big Picture)
Requires=xvfb.service
After=xvfb.service network-online.target

[Service]
User=steamos
# steamos needs gid 3003 (aid_inet) for IP sockets: the Android kernel has PARANOID_NETWORK
Environment=DISPLAY=:1 HOME=/home/steamos USER=steamos XDG_RUNTIME_DIR=/run/user/1000
ExecStartPre=+/usr/bin/install -d -o steamos -g steamos -m 700 /run/user/1000
ExecStart=/usr/local/bin/steam-q1 -gamepadui
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/x11vnc.service <<'EOF'
[Unit]
Description=VNC access to the Steam screen (:1)
Requires=xvfb.service
After=xvfb.service

[Service]
ExecStart=/usr/local/bin/x11vnc -display :1 -rfbauth /etc/x11vnc/passwd -forever -shared -rfbport 5900 -noxdamage -nowf -noscr
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

cat > /usr/local/bin/steam-screen <<'EOF'
#!/bin/sh
# steam-screen [width m] [distance m] [timeout s]: show the Steam screen in the headset.
# kill -USR1 $(pidof xscreen) recenters it in front of the current gaze.
exec /usr/local/bin/xr-run /usr/local/bin/xscreen /run/xvfb/Xvfb_screen0 "${1:-1.6}" "${2:-1.4}" "${3:-0}"
EOF
chmod 755 /usr/local/bin/steam-screen

cat > /etc/systemd/system/steam-screen.service <<'EOF'
[Unit]
Description=Steam screen in VR (OpenXR quad layer on Monado)
Requires=monado.service xvfb.service
After=monado.service xvfb.service steam.service

[Service]
ExecStart=/usr/local/bin/steam-screen
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

mkdir -p /etc/x11vnc
[ -f /etc/x11vnc/passwd ] || /usr/local/bin/x11vnc -storepasswd quest1 /etc/x11vnc/passwd
chmod 600 /etc/x11vnc/passwd
systemctl daemon-reload
systemctl enable xvfb.service steam.service x11vnc.service steam-screen.service
