#!/bin/sh
# Cross-build libframeid.so for the headset (run in WSL: wsl -d Debian -u root -- sh build.sh from this dir)
set -e
aarch64-linux-gnu-gcc -O2 -Wall -shared -fPIC -o libframeid.so frameid.c -ldl
aarch64-linux-gnu-strip libframeid.so
ls -la libframeid.so
