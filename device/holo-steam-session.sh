#!/bin/sh
# Steam session on the native Quest 1: the arm64 Steam client (Big Picture) on a virtual X server,
# shown in the headset by xscreen (an OpenXR quad layer on Monado) and controllable from the PC over VNC.
#   xvfb.service      Xvfb :1, 1280x720 (the Frame's gamescope screen), framebuffer exported as an XWD file in /run/xvfb
#   steam.service     steam-q1 -gamepadui -steamos3 -steamdeck -steamframe (Frame identity: ON_FRAME, Frame OOBE; docs/frame-mode.md) as steamos (restarts itself after client self-updates)
#   x11vnc.service    VNC on port 5900, password in /etc/x11vnc/passwd
#   steam-screen.service  shows the Steam screen in VR (xscreen); the Monado target keeps the
#                     panels off while the proximity sensor says the headset is not worn
# The webhelper's GPU process (Chromium) renders on the Adreno: ANGLE Vulkan through libhybris, with
# X11 presentation by quest1_vkshim (docs/steam-gpu.md). QUEST1_STEAM_GPU=0 in steam.service's
# environment goes back to Mesa llvmpipe.
# Remote Play encodes on the headset's venus encoder: /dev/video-enc0 (the Frame's node name) and
# libquest1_v4l2.so preloaded into the client, which turns Steam's mainline V4L2 calls into what the
# downstream msm_vidc driver takes (src/v4l2shim, docs/streaming.md). The same library has the decoder
# (q1-hwdec <program> preloads it).
# Usage (on the headset): [VKSRC=<src/vklayer-steamvr copy>] [V4L2SRC=<src/v4l2shim copy>] [RPSRC=<src/rpspectator copy>]
#   [SCSRC=<src/q1screencast copy>] holo-steam-session.sh install
#   VKSRC: build and install the Vulkan shim + dlopen redirect into /usr/local/lib/quest1-vk-steam
#   V4L2SRC: build and install /usr/local/lib/libquest1_v4l2.so (the image has it from tools/build-native.sh)
#   RPSRC: build and install /usr/local/lib/libquest1_rpspectator.so (src/rpspectator; same)
#   SCSRC: build and install /usr/local/bin/q1screencast (src/q1screencast; same)
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
# Steam plays its startup movie at every start: unseen in Xvfb, but heard. Empty overrides in
# config/uioverrides (not one of Steam's verified files) fail to play, which Steam takes as the movie's end.
mkdir -p $S/config/uioverrides/movies
for m in $S/steamui/movies/*startup*.webm; do
	o=$S/config/uioverrides/movies/${m##*/}
	[ -e "$m" ] && [ ! -e "$o" ] && : > "$o"
done
# webhelper GPU process on the Adreno (ANGLE Vulkan via libhybris, docs/steam-gpu.md);
# QUEST1_STEAM_GPU=0 goes back to Mesa llvmpipe. Steam's xcomposite workaround re-composites
# every CEF window with GLX on llvmpipe (~2 cores while the UI animates): off by default here,
# QUEST1_STEAM_CEF_ARGS= (empty) keeps it.
if [ "${QUEST1_STEAM_GPU:-1}" = 1 ]; then
	export STEAM_CEF_GPU_CMD_PREFIX=steam-webhelper-gpu
	set -- -cef-disable-gpu-sandbox ${QUEST1_STEAM_CEF_ARGS--cef-disable-xcomposite-workaround} "$@"
fi
# Steam's VR UI: the webhelper browser process hands it to SteamVR (vrclient: OPAQUE_FD textures
# and timeline semaphores shared with vrcompositor), so its Vulkan must be the Adreno's: libhybris
# TLS, quest1_vkshim with the SteamVR compat layer, Steam's bundled libvulkan redirected to the
# shim. Without it vkCreateInstance fails (-9), every SetOverlayTexture fails and the SteamVR
# dashboard panel stays empty. Set for the whole client: Steam verifies its files
# (BVerifyInstalledFiles), so steamwebhelper.sh cannot carry it. QUEST1_STEAM_VR=0 turns it off.
if [ "${QUEST1_STEAM_VR:-0}" = 1 ]; then
	V=/usr/local/lib/quest1-vk-steam
	export LD_LIBRARY_PATH=$V:$LD_LIBRARY_PATH
	export LD_PRELOAD=/opt/hybris/lib/libbionictls.so:$V/libquest1_vkredirect.so${LD_PRELOAD:+:$LD_PRELOAD}
	export QUEST1_VK_REDIRECT=$V/libvulkan.so.1 QUEST1_VKSHIM_WSI=1 QUEST1_STEAMVR_COMPAT=1
fi
# Remote Play's hardware encoder (CGameStreamVideoStageV4L2 on /dev/video-enc0) through the msm_vidc translation
[ -f /usr/local/lib/libquest1_v4l2.so ] && export LD_PRELOAD=/usr/local/lib/libquest1_v4l2.so${LD_PRELOAD:+:$LD_PRELOAD}
# Remote Play from the headset is a spectator view: the PC's mouse, keyboard, cursor and gamepads stay out
# (src/rpspectator)
[ -f /usr/local/lib/libquest1_rpspectator.so ] &&
	export LD_PRELOAD=/usr/local/lib/libquest1_rpspectator.so${LD_PRELOAD:+:$LD_PRELOAD}
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
# Stencil-based drawing is broken on it too: Skia's stencil-then-cover filled SVG icons over their
# bounding box (most Steam UI icons of 32 px and more showed as squares/blobs); Chromium's
# avoid_stencil_buffers workaround makes Skia draw them from coverage masks instead.
# GPU memory: Chromium's default budget made this process map ~1.5 GB of KGSL memory, which the
# 3.8 GB shared CPU/GPU RAM cannot afford next to SteamVR (swap storms, SSH timeouts): capped. At 192 MB Skia could not
# keep its scratch textures and allocated ~2.7 new full-page (12 MB) ones a frame of Steam's VR pages; at 384 under one.
# Readback: Chromium reads every frame of Steam's off-screen (VR) pages back; the blob's "cached coherent" memory is
# write-combined (~220 MB/s CPU reads), QUEST1_VKSHIM_CACHED_READBACK gives ANGLE its really cached type instead.
# QUEST1_VKSHIM_MAX_INFLIGHT: at most 4 submissions waiting for the GPU (quest1_wsi_x11.c): without it 17-20 waited
# behind vrcompositor, Steam reacted seconds late and the GPU memory held for them ran the RAM out (2026-11-08).
# Extra GPU process args: QUEST1_STEAM_GPU_ARGS. See docs/steam-gpu.md.
V=/usr/local/lib/quest1-vk-steam
export LD_LIBRARY_PATH=$V${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so:$V/libquest1_vkredirect.so${LD_PRELOAD:+:$LD_PRELOAD}
export QUEST1_VKSHIM_WSI=1 QUEST1_VKSHIM_LAYER=none QUEST1_VK_REDIRECT=$V/libvulkan.so.1 QUEST1_VKSHIM_CACHED_READBACK=1 \
	QUEST1_VKSHIM_MAX_INFLIGHT=4
exec "$@" --use-angle=vulkan --disable-angle-features=supportsExtendedDynamicState \
	--enable-angle-features=disableFlippingBlitWithCommand --avoid_stencil_buffers \
	--force-gpu-mem-available-mb=384 --force-gpu-mem-discardable-limit-mb=64 $QUEST1_STEAM_GPU_ARGS
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

if [ -n "$V4L2SRC" ]; then
	gcc -std=gnu11 -O2 -Wall -fPIC -shared -fvisibility=hidden -o /usr/local/lib/libquest1_v4l2.so \
		"$V4L2SRC/quest1_v4l2.c" "$V4L2SRC/quest1_vdec.c" -lpthread
fi
# Hardware video decoding for a program that uses FFmpeg's V4L2 decoders (h264_v4l2m2m, hevc_v4l2m2m): the same
# library turns their calls into what the venus decoder takes (src/v4l2shim/quest1_vdec.c). For example
#   q1-hwdec ffmpeg -c:v h264_v4l2m2m -i film.mp4 ...    q1-hwdec vlc --avcodec-codec=h264_v4l2m2m film.mp4
cat > /usr/local/bin/q1-hwdec <<'EOF'
#!/bin/sh
exec env LD_PRELOAD=/usr/local/lib/libquest1_v4l2.so${LD_PRELOAD:+:$LD_PRELOAD} "$@"
EOF
chmod 755 /usr/local/bin/q1-hwdec
# VLC's launcher: the hardware decoder when the file it is opened with is one venus decodes (8-bit progressive
# H.264 or HEVC), plain VLC otherwise. VLC's --avcodec-codec names one decoder and libavcodec does not fall back
# from one decoder to another, so the choice is made here, before VLC starts. A stream of another codec played later
# in the same VLC is decoded in software (VLC ignores the name then).
cat > /usr/local/bin/q1-vlc <<'EOF'
#!/bin/bash
hw=
if [ -w /dev/video32 ] && [ -f /usr/local/lib/libquest1_v4l2.so ]; then
	for a in "$@"; do
		case $a in -*) continue ;; esac
		f=${a#file://}
		[ "$f" != "$a" ] && printf -v f '%b' "${f//%/\\x}" # a file URL: percent-decoded
		[ -f "$f" ] || continue
		IFS=, read -r codec w h pix field < <(ffprobe -v error -select_streams v:0 \
			-show_entries stream=codec_name,width,height,pix_fmt,field_order -of csv=p=0 "$f" 2>/dev/null)
		case $pix in yuv420p | yuvj420p) ;; *) break ;; esac
		case $field in progressive | unknown | "") ;; *) break ;; esac
		[ "${w:-0}" -le 4096 ] && [ "${h:-0}" -le 2304 ] || break
		case $codec in h264 | hevc) hw=${codec}_v4l2m2m ;; esac
		break
	done
fi
[ -n "$hw" ] && exec /usr/local/bin/q1-hwdec /usr/bin/vlc --avcodec-codec=$hw "$@"
exec /usr/bin/vlc "$@"
EOF
chmod 755 /usr/local/bin/q1-vlc
if [ -f /usr/share/applications/vlc.desktop ]; then
	mkdir -p /usr/local/share/applications
	sed 's|^Exec=/usr/bin/vlc|Exec=/usr/local/bin/q1-vlc|' /usr/share/applications/vlc.desktop \
		> /usr/local/share/applications/vlc.desktop
fi
if [ -n "$RPSRC" ]; then
	gcc -std=gnu11 -O2 -Wall -fPIC -shared -fvisibility=hidden -o /usr/local/lib/libquest1_rpspectator.so \
		"$RPSRC/quest1_rpspectator.c" -ldl
fi
if [ -n "$SCSRC" ]; then
	gcc -std=gnu11 -O3 -Wall -o /usr/local/bin/q1screencast "$SCSRC/q1screencast.c" \
		$(pkg-config --cflags --libs libpipewire-0.3 libsystemd x11 xext xdamage xfixes)
fi
# Remote Play's capture: the ScreenCast portal backend for :1 (src/q1screencast), so Steam takes the screen through
# PipeWire instead of reading it back with OpenGL on llvmpipe. Started by D-Bus when a client asks the portal; the
# portal (xdg-desktop-portal, XDG_CURRENT_DESKTOP unset) reads /etc/xdg-desktop-portal/portals.conf, where only
# ScreenCast changes: the rest stays on gtk, as before (no "*": it would wake the KDE backend).
cat > /usr/share/xdg-desktop-portal/portals/quest1.portal <<'EOF'
[portal]
DBusName=org.freedesktop.impl.portal.desktop.quest1
Interfaces=org.freedesktop.impl.portal.ScreenCast;
EOF
mkdir -p /etc/xdg-desktop-portal
cat > /etc/xdg-desktop-portal/portals.conf <<'EOF'
[preferred]
default=gtk
org.freedesktop.impl.portal.ScreenCast=quest1
EOF
cat > /usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.quest1.service <<'EOF'
[D-BUS Service]
Name=org.freedesktop.impl.portal.desktop.quest1
Exec=/usr/local/bin/q1screencast
SystemdService=q1screencast.service
EOF
cat > /etc/systemd/user/q1screencast.service <<'EOF'
[Unit]
Description=ScreenCast portal backend for the Steam client's X screen (:1)

[Service]
Type=dbus
BusName=org.freedesktop.impl.portal.desktop.quest1
Environment=DISPLAY=:1
ExecStart=/usr/local/bin/q1screencast
EOF
# the venus encoder under the Frame's name (msm_vidc's nodes are video32 = decoder, video33 = encoder; their
# sysfs name is empty); the video group (steamos) already has it
cat > /etc/udev/rules.d/70-quest1-video.rules <<'EOF'
SUBSYSTEM=="video4linux", KERNEL=="video33", SYMLINK+="video-enc0"
EOF
ln -sfn video33 /dev/video-enc0

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
ExecStart=/usr/sbin/Xvfb :1 -screen 0 1280x720x24 -fbdir /run/xvfb -nolisten tcp
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/steam.service <<'EOF'
[Unit]
Description=Steam client (arm64, Big Picture)
Requires=xvfb.service
Wants=user@1000.service
After=xvfb.service network-online.target user@1000.service

[Service]
User=steamos
# steamos needs gid 3003 (aid_inet) for IP sockets: the Android kernel has PARANOID_NETWORK
Environment=DISPLAY=:1 HOME=/home/steamos USER=steamos XDG_RUNTIME_DIR=/run/user/1000
# the user's session bus, as in a desktop session: the apps Steam starts ("+" menu) inherit Steam's environment, and
# without it every Flatpak failed ("Failed to get portal proxy": flatpak looks for a bus with dbus-launch)
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
ExecStartPre=+/usr/bin/install -d -o steamos -g steamos -m 700 /run/user/1000
# Steam's VR UI (SteamVR overlays from the webhelper's browser process, steam-q1 above). Steam's xcomposite composer
# is kept (QUEST1_STEAM_CEF_ARGS empty). The browser's OpenGL is Mesa llvmpipe: vrclient must not see its
# GL_EXT_memory_object/semaphore (llvmpipe can't import the Adreno's memory), so it takes its CPU fallback.
Environment=QUEST1_STEAM_VR=1 QUEST1_STEAM_CEF_ARGS=
Environment="MESA_EXTENSION_OVERRIDE=-GL_EXT_memory_object -GL_EXT_memory_object_fd -GL_EXT_semaphore -GL_EXT_semaphore_fd"
# -pipewire: Remote Play captures the screen through the ScreenCast portal (q1screencast above), not with OpenGL
ExecStart=/usr/local/bin/steam-q1 -gamepadui -steamos3 -steamdeck -steamframe -pipewire
# Chromium makes its audio threads SCHED_RR 8 itself when RLIMIT_RTPRIO allows it (desktops grant it through rtkit).
# At 0 Steam's "Realtime AudioW" stayed a normal thread that waited longer for a CPU than it ran (0.99 s against
# 0.29 s six minutes after boot, up to 24 ms at once): its sounds crackled while the headset was busy (2026-11-08).
# Below Meta's tracking, SteamVR and Monado (48 and up); games started by Steam inherit it, as rtkit would allow.
LimitRTPRIO=10
# Steam exits with status 0 on its own (e.g. when SteamVR restarts); like a console session it must come back
Restart=always
RestartSec=5
# Steam saves and quits within ~2 s of SIGTERM; when part of it crashes on the way out, its crash
# reporter tries to upload the dump with the network already down and held power off and restart
# for 30 s (2026-11-03)
TimeoutStopSec=8

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

# Steam lists the system's desktop entries in its "+" menu: hide the desktop tools that come with
# the image or our x11vnc build (Avahi browsers, CMake, IBus preferences, X11VNC Server) and show
# nothing in the headset. Entries in /usr/local/share/applications take precedence over /usr/share
# ones; Hidden=true removes them.
mkdir -p /usr/local/share/applications
for a in avahi-discover bssh bvnc cmake-gui org.freedesktop.IBus.Setup x11vnc; do
	printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true\n' "$a" > /usr/local/share/applications/$a.desktop
done

# The user's own services (xdg-desktop-portal-gtk: file choosers, settings for apps) open their windows on Steam's X
# display, like the apps Steam starts; q1apps shows them in the dashboard. Without DISPLAY the GTK portal failed
# ("cannot open display").
mkdir -p /etc/environment.d
echo DISPLAY=:1 > /etc/environment.d/quest1-display.conf

mkdir -p /etc/x11vnc
[ -f /etc/x11vnc/passwd ] || /usr/local/bin/x11vnc -storepasswd quest1 /etc/x11vnc/passwd
chmod 600 /etc/x11vnc/passwd
systemctl daemon-reload
systemctl enable xvfb.service steam.service x11vnc.service steam-screen.service
