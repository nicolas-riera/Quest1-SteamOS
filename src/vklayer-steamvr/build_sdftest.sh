#!/bin/sh
# Cross-build sdftest (WSL Debian: glslang-tools, gcc-aarch64-linux-gnu, Vulkan-Headers in /root/Vulkan-Headers).
set -e
cd "$(dirname "$0")"
glslangValidator -V --vn sdftest_cs -o sdftest_cs.h sdftest.comp >/dev/null
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -I/root/Vulkan-Headers/include -o sdftest sdftest.c -ldl
