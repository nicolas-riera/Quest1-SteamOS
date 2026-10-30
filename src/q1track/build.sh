#!/bin/sh
# Build q1trackd on the headset (Holo's gcc), as a bionic executable: no NDK, linked directly
# against the runtime APEX's bionic libc/libdl from the system_a mount (holo-android-blobs).
# Output: ./q1trackd (install as /usr/local/lib/q1track/q1trackd; holo-android-tracking runs it
# from /data/local/tmp inside its mount namespace).
set -e
cd "$(dirname "$0")"
B=/apex/com.android.runtime/lib64/bionic
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
	-fPIE -pie -nostdlib -nostartfiles -fno-stack-protector -U_FORTIFY_SOURCE \
	-Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,now -Wl,--no-undefined -Wl,--allow-shlib-undefined \
	-o q1trackd q1trackd.c $B/libc.so $B/libdl.so
ls -la q1trackd
