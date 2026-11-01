#!/bin/sh
# Build libq1camtap.so on the headset (Holo's gcc) as a bionic shared object: no NDK, linked
# against the runtime APEX's bionic libc/libdl (same recipe as src/q1track/build.sh).
# Install as /data/local/tmp/libq1camtap.so (the bionic default namespace permits /data) and
# preload it into trackingservice: LD_PRELOAD=/data/local/tmp/libq1camtap.so.
set -e
cd "$(dirname "$0")"
B=/apex/com.android.runtime/lib64/bionic
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
	-shared -fPIC -mno-outline-atomics -nostdlib -fno-stack-protector -U_FORTIFY_SOURCE -fvisibility=hidden \
	-Wl,-z,now -Wl,--no-undefined -Wl,--allow-shlib-undefined \
	-o libq1camtap.so camtap.c $B/libc.so $B/libdl.so
ls -la libq1camtap.so
