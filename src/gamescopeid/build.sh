#!/bin/sh
# Cross-build libquest1_gamescopeid.so (WSL Debian, gcc-aarch64-linux-gnu). tools/build-native.sh builds it for the image.
set -e
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc -O2 -Wall -shared -fPIC -o libquest1_gamescopeid.so gamescopeid.c -ldl
aarch64-linux-gnu-strip libquest1_gamescopeid.so
