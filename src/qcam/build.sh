#!/bin/sh
# Cross-build qcam for the headset (run in WSL Debian with gcc-aarch64-linux-gnu).
# libhybris is NOT needed at build time: qcam dlopen()s /opt/hybris/lib/libhybris-common.so.1 at
# run time (or uses android_dlopen directly when linked with -lhybris-common, as sbimu does).
# Natively in Holo: gcc -O2 -o qcam qcam.c -ldl -lpthread
# Copy to the headset and run as root:
#   LD_LIBRARY_PATH=/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./qcam --stage 2
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -o qcam qcam.c -ldl -lpthread
aarch64-linux-gnu-objdump -T qcam | grep -E "GLIBC_2\.(3[5-9]|4)" | sort -u || true
ls -la qcam
