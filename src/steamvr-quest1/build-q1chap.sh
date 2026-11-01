#!/bin/sh
# Cross-build q1chap (chaperone setup utility) for the headset. Needs the device's libopenvr_api.so
# (scp root@192.168.77.1:/opt/steamvr/bin/linuxarm64/libopenvr_api.so /root/q1chap-lib/).
set -e
cd "$(dirname "$0")"
mkdir -p quest1/bin/linuxarm64
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -I/root/openvr-sdk/headers -I/root/stb -o quest1/bin/linuxarm64/q1chap q1chap.cpp \
	-L/root/q1chap-lib -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -static-libstdc++ -static-libgcc
ls -la quest1/bin/linuxarm64/q1chap
