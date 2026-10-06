#!/bin/sh
# Cross-build the SteamVR compatibility layer for the headset (WSL Debian, g++-aarch64-linux-gnu).
# Install on the headset: the .so in /usr/local/lib, the JSON in /usr/share/vulkan/implicit_layer.d/.
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation \
	-I/root/Vulkan-Headers/include -o libVkLayer_quest1_steamvr_compat.so quest1_compat.c -lpthread
aarch64-linux-gnu-objdump -T libVkLayer_quest1_steamvr_compat.so | grep -E "vkNegotiate|GLIBC_2\.(3[4-9]|4)" | sort -u
ls -la libVkLayer_quest1_steamvr_compat.so
