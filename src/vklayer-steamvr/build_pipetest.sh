#!/bin/sh
# Cross-build pipetest (WSL Debian, gcc-aarch64-linux-gnu, Vulkan-Headers in /root/Vulkan-Headers).
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -I/root/Vulkan-Headers/include -o pipetest pipetest.c -ldl
