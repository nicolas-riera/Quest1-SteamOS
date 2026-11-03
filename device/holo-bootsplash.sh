#!/bin/sh
# Boot animation like the Steam Frame's: Steam's own Frame startup movie (steamframe_startup_450x450.webm,
# in the user's Steam install: nothing of Valve's is shipped here), drawn straight on the panels, with
# its sound, before Monado and SteamVR take the display.
# Run as root: sh holo-bootsplash.sh install <dir with q1splash.c and inc/ (kernel uapi msm_mdp_ext.h)>
#  - packages: GStreamer's VP9 (libvpx) and Matroska plugins
#  - /usr/local/lib/q1splash/q1splash: built from src/q1splash/q1splash.c
#  - /usr/local/sbin/q1splash-play, quest1-bootsplash.service (enabled), monado.service waits for it
# Before Steam has been installed (first boot) there is no movie and nothing is shown.
set -e
[ "$1" = install ] && [ -f "$2/q1splash.c" ] || { echo "usage: $0 install <q1splash source dir>" >&2; exit 2; }
cd "$2"
pacman -S --noconfirm --needed gst-plugins-base gst-plugins-good libvpx

mkdir -p /usr/local/lib/q1splash
gcc -O2 -Wall -o /usr/local/lib/q1splash/q1splash q1splash.c -Iinc -I/opt/hybris/include -L/opt/hybris/lib \
	-Wl,-rpath,/opt/hybris/lib -lhybris-common

cat > /usr/local/sbin/q1splash-play <<'EOF'
#!/bin/sh
# Plays the Steam Frame's startup movie on the panels (q1splash), its sound on PipeWire when the user
# session is up (else the video alone, still paced by GStreamer's clock).
M=/home/steamos/.local/share/Steam/steamui/movies/steamframe_startup_450x450.webm
[ -f "$M" ] || { echo "q1splash-play: no $M (Steam not installed yet)"; exit 0; }
P=/run/user/1000/pulse/native
for i in $(seq 1 20); do [ -S $P ] && break; sleep 0.25; done
if [ -S $P ]; then
	AUDIO="audioconvert ! audioresample ! pulsesink server=unix:$P"
else
	AUDIO="fakesink sync=true"
fi
gst-launch-1.0 -q filesrc location="$M" ! matroskademux name=d \
	d. ! queue ! vp9dec ! videoconvert ! video/x-raw,format=RGBA ! fdsink fd=1 sync=true \
	d. ! queue ! opusdec ! $AUDIO |
	LD_PRELOAD=/opt/hybris/lib/libbionictls.so /usr/local/lib/q1splash/q1splash 450 450
EOF
chmod 755 /usr/local/sbin/q1splash-play

cat > /etc/systemd/system/quest1-bootsplash.service <<'EOF'
[Unit]
Description=Boot animation on the panels (the Steam Frame's startup movie)
# gralloc for the panel buffers, the ADSP and PipeWire for the sound
After=android-gralloc.service holo-audio.service user@1000.service
Wants=android-gralloc.service
Before=monado.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/q1splash-play
TimeoutStartSec=20

[Install]
WantedBy=multi-user.target
EOF

# one committer on /dev/fb0: Monado starts once the animation is over
mkdir -p /etc/systemd/system/monado.service.d
cat > /etc/systemd/system/monado.service.d/quest1-bootsplash.conf <<'EOF'
[Unit]
After=quest1-bootsplash.service
EOF

systemctl daemon-reload
systemctl enable quest1-bootsplash.service
echo "holo-bootsplash: installed; the animation plays at the next boot (test now: systemctl stop monado first)"
