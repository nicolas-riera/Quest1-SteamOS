#!/bin/sh
# Cross-build trackedcam_test (WSL Debian, g++-aarch64-linux-gnu, OpenVR SDK in /root/openvr-sdk).
# openvr_api is loaded at run time: LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers -o trackedcam_test trackedcam_test.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined
