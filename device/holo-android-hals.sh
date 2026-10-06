#!/bin/sh
# Install systemd units that run the few Android HAL daemons native Holo needs
# (bionic binaries from the read-only system_a mount, no Android init):
#   hwservicemanager - HIDL service registry on /dev/hwbinder
#   graphics allocator - gralloc buffers (AHardwareBuffer) for the Monado target and swapchains
#   configstore - libvulkan asks it for surfaceflinger settings and waits forever without it
set -e
U=/etc/systemd/system

cat > $U/android-hwservicemanager.service <<'EOF'
[Unit]
Description=Android hwservicemanager (HIDL registry)
Requires=holo-android-blobs.service
After=holo-android-blobs.service

[Service]
Environment=ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime
ExecStart=/system/bin/hwservicemanager
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

hal() { # name description binary
cat > $U/android-$1.service <<EOF
[Unit]
Description=Android HAL: $2
Requires=android-hwservicemanager.service
After=android-hwservicemanager.service

[Service]
Environment=ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime
# hwservicemanager.ready is preset in the property area: give the registry time to start
ExecStartPre=/bin/sleep 0.5
ExecStart=$3
Restart=on-failure
RestartSec=1

[Install]
WantedBy=multi-user.target
EOF
}
hal gralloc "graphics allocator" /system/vendor/bin/hw/android.hardware.graphics.allocator@2.0-service
hal configstore "configstore" /system/vendor/bin/hw/android.hardware.configstore@1.1-service

systemctl daemon-reload
systemctl enable android-hwservicemanager.service android-gralloc.service android-configstore.service
