#!/bin/sh
# Cross-build ovrstub for the headset (WSL Debian, gcc-aarch64-linux-gnu). Plain glibc C, no
# Android libraries. Natively in Holo: gcc -O2 -Wall -o ovrstub ovrstub.c
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -o ovrstub ovrstub.c
ls -la ovrstub
