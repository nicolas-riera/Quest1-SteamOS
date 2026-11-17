#!/bin/sh
# Cross-build libq1trackedcam.so (WSL Debian, g++-aarch64-linux-gnu, OpenVR SDK in /root/openvr-sdk).
# Install: copy it to /usr/local/lib/libq1trackedcam.so on the headset; device/holo-steamvr.sh preloads it into vrwebhelper.
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -fPIC -shared -I/root/openvr-sdk/headers -o libq1trackedcam.so q1_trackedcam_shim.cpp -ldl
