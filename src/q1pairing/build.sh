#!/bin/sh
# Cross-build q1pairing for the headset (WSL Debian, g++-aarch64-linux-gnu, OpenVR headers in /root/openvr-sdk,
# an arm64 libopenvr_api.so in /opt/q1libs: same as src/camtap/build_test.sh). At run time it uses SteamVR's own.
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -I/root/openvr-sdk/headers -o q1pairing q1pairing.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined \
	-static-libstdc++ -static-libgcc
ls -la q1pairing
