#!/bin/bash
# Build Oculus Quest 4.4 kernel in WSL. usage: kbuild.sh <outdir-name> [make targets...]
set -o pipefail
O=$HOME/q1/${1:-out}; shift
cd ~/q1/kernel
export ARCH=arm64 CROSS_COMPILE=$HOME/q1/tc/gcc49/bin/aarch64-linux-android- CROSS_COMPILE_ARM32=$HOME/q1/tc/gcc49arm/bin/arm-linux-androideabi-
cp -n verity.x509.pem $O/ 2>/dev/null
make O=$O DTC=/usr/bin/dtc -j$(nproc) "${@:-Image.gz-dtb}" > $O.log 2>&1
rc=$?; echo "rc=$rc"; grep -E "error:|Error [0-9]|FATAL" $O.log | head -20; tail -3 $O.log
ls -la $O/arch/arm64/boot/Image.gz-dtb 2>/dev/null
