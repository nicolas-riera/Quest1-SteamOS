#!/bin/sh
# Cross-build q1-perfhud (WSL Debian, g++-aarch64-linux-gnu, OpenVR SDK in /root/openvr-sdk). tools/build-native.sh builds it
# for the image. Output: ./q1-perfhud (not committed).
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -I/root/openvr-sdk/headers -o q1-perfhud q1perfhud.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined
