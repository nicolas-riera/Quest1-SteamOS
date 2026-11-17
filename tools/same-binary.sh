#!/bin/sh
# same-binary.sh [REV] (in WSL Debian as root, from the repository: wsl -u root -e sh tools/same-binary.sh): builds the
# SteamVR compatibility layer (src/vklayer-steamvr, one translation unit) from REV (default HEAD) and from the working
# tree with the same flags as its build.sh, and says whether the two are the same byte for byte. For changes that must
# not change the code: moving functions between the files of one unit, comments, splitting a long file into parts.
set -e
rev=${1:-HEAD}
repo=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
F="-std=gnu11 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
-Wno-format-truncation -I/root/Vulkan-Headers/include -I../monado/src/xrt/compositor/main/msm
-I../monado/src/xrt/compositor/main"
git -C "$repo" archive "$rev" src/vklayer-steamvr src/monado/src/xrt/compositor/main | tar -x -C "$t"
(cd "$t/src/vklayer-steamvr" && aarch64-linux-gnu-gcc $F -o "$t/before.so" quest1_compat.c -lpthread)
(cd "$repo/src/vklayer-steamvr" && aarch64-linux-gnu-gcc $F -o "$t/after.so" quest1_compat.c -lpthread)
md5sum "$t/before.so" "$t/after.so" | sed "s|$t/||"
if cmp -s "$t/before.so" "$t/after.so"; then
	echo "same binary as $rev"
else
	echo "DIFFERENT from $rev"
	exit 1
fi
