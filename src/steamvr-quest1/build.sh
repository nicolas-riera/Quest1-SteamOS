#!/bin/sh
# Cross-build driver_quest1.so for the headset (run in WSL Debian with g++-aarch64-linux-gnu).
# Headers: OpenVR SDK (/root/openvr-sdk), OpenXR-SDK (/root/OpenXR-SDK), Vulkan-Headers (/root/Vulkan-Headers).
set -e
cd "$(dirname "$0")"
mkdir -p quest1/bin/linuxarm64
aarch64-linux-gnu-g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wno-missing-field-initializers \
	-I/root/openvr-sdk/headers -I/root/OpenXR-SDK/include -I/root/Vulkan-Headers/include \
	-o quest1/bin/linuxarm64/driver_quest1.so driver_quest1.cpp -ldl -lpthread \
	-static-libstdc++ -static-libgcc -Wl,--no-undefined
aarch64-linux-gnu-objdump -T quest1/bin/linuxarm64/driver_quest1.so | grep -E "HmdDriverFactory|GLIBC_2\.(3[5-9]|4)" | sort -u
ls -la quest1/bin/linuxarm64/driver_quest1.so
