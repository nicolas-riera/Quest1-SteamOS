#!/bin/sh
# Cross-build the feasibility probe ptres_test (WSL Debian, same toolchain as build_overlay.sh).
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers -o ptres_test ptres_test.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined
aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers -o ptprovider ptprovider.cpp \
	-L/opt/q1libs -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined
