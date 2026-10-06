#!/bin/sh
# Install the Quest 1 Monado build (headset tree /root/monado/build) as the system OpenXR runtime,
# started at boot. The MDP compositor target keeps the panels off and the GPU at its normal
# governor whenever no app is showing frames.
set -e
B=/root/monado/build
P=/opt/monado

install -m755 $B/src/xrt/targets/service/monado-service $P/bin/monado-service
install -m755 $B/src/xrt/targets/cli/monado-cli $P/bin/monado-cli
install -m755 $B/src/xrt/targets/openxr/libopenxr_monado.so $P/lib/libopenxr_monado.so
mkdir -p /etc/xdg/openxr/1
ln -sf $P/share/openxr/1/openxr_monado.json /etc/xdg/openxr/1/active_runtime.json

cat > /etc/systemd/system/monado.service <<'EOF'
[Unit]
Description=Monado OpenXR runtime (Quest 1 native: SyncBoss IMU, MDP panels)
Requires=android-gralloc.service android-configstore.service
After=android-gralloc.service android-configstore.service

[Service]
# libhybris needs bionic's TLS layout; the IPC socket goes to $XDG_RUNTIME_DIR
Environment=LD_PRELOAD=/opt/hybris/lib/libbionictls.so
Environment=XDG_RUNTIME_DIR=/run/monado HOME=/root
# graphics path (layers rendered at 85 %, then mesh distortion): 5.7 ms GPU for a quad layer vs
# 17.7 ms with the compute path, which misses every vsync on the Adreno 540
Environment=XRT_NO_STDIN=1 XRT_COMPOSITOR_SCALE_PERCENTAGE=85 XRT_COMPOSITOR_COMPUTE=0
RuntimeDirectory=monado
RuntimeDirectoryMode=0755
ExecStart=/opt/monado/bin/monado-service
# The target blanks the panels itself; make sure they end up off whatever happens.
ExecStopPost=/bin/sh -c 'echo msm-adreno-tz > /sys/class/kgsl/kgsl-3d0/devfreq/governor; echo 4 > /sys/class/graphics/fb0/blank'
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

# OpenXR apps: same socket directory, the hybris Vulkan, bionic TLS
cat > /usr/local/bin/xr-run <<'EOF'
#!/bin/sh
# xr-run <command...>: run an OpenXR app against the native Quest Monado service
export XDG_RUNTIME_DIR=/run/monado LD_LIBRARY_PATH=/opt/hybris/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so${LD_PRELOAD:+:$LD_PRELOAD}
exec "$@"
EOF
chmod 755 /usr/local/bin/xr-run

systemctl daemon-reload
systemctl enable monado.service
