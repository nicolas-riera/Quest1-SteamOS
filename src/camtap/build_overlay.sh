#!/bin/sh
# Cross-build camoverlay (WSL Debian, g++-aarch64-linux-gnu, OpenVR SDK in /root/openvr-sdk).
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers -o camoverlay camoverlay.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined
