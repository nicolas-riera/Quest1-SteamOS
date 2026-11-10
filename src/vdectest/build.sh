#!/bin/sh
# Cross-build vdectest for the headset (run in WSL Debian with gcc-aarch64-linux-gnu).
# Natively in Holo: gcc -O2 -o vdectest vdectest.c -ldl
# vkahbtest (the GPU sampling a decoder buffer) is built on the headset only: see the top of vkahbtest.c.
# Run as root on the headset (the decoder node and /dev/ion), e.g.
#   ./vdectest -c h264 -s 1920x1080 -m split -o /tmp/dec.nv12 test.264
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -o vdectest vdectest.c -ldl
aarch64-linux-gnu-objdump -T vdectest | grep -E "GLIBC_2\.(3[5-9]|4)" | sort -u || true
ls -la vdectest
