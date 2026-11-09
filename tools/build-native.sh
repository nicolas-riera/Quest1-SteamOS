#!/bin/bash
# Build every arm64 binary of ours that the headset system runs, from public sources only, into one archive
# (prebuilt/native-arm64.tar.zst) that the installer unpacks over the image (installer/build_image.py, step
# "native"). Nothing of Meta's or Valve's is needed or included: bionic-linked tools link against generated
# stubs, SteamVR tools against the public OpenVR SDK.
#
# Runs as root inside a Holo aarch64 root with network (pacman installs the build tools):
#   CI:   .github/workflows/native.yml (GitHub's arm64 runner, a chroot, no emulation)
#   dev:  WSL 2 + qemu-user binfmt, a chroot of Holo's system.rootfs.zst (slow), or on the headset
# usage: build-native.sh <repo dir> <output .tar.zst>      (JOBS=n, default: the CPU count)
#
# Contents (paths in the image):
#   /opt/hybris        libhybris (+ android-headers) and libbionictls.so (src/bionictls)
#   /opt/monado        Monado @ MONADO_COMMIT + src/monado/quest1-monado.patch
#   /opt/quest1-steamvr/quest1   driver_quest1 (src/steamvr-quest1) and its resources
#   /usr/local/lib     libVkLayer_quest1_steamvr_compat.so (src/vklayer-steamvr), libvncserver,
#                      libquest1_v4l2.so (src/v4l2shim), libquest1_rpspectator.so, qt6/plugins/ksystemstats (src/ksysgpu)
#   /usr/local/lib/quest1-vk, quest1-vk-steam   the Vulkan shim (vrcompositor; Steam's webhelper + vkredirect)
#   /usr/local/lib/q1track/q1trackd, /data/local/tmp/libq1camtap.so   bionic tools (src/q1track, src/camtap)
#   /usr/local/lib/q1splash/q1splash, /usr/local/bin/{ovrstub,q1black,xscreen,x11vnc,q1-camoverlay,
#   q1-ptprovider,vrdash,q1desktop,q1apps}
# To check against the development headset (built by hand there, recipe not recorded until now):
#   the libhybris commit, Monado's CMake options, the vrcompositor shim (here the same build as Steam's).
set -eo pipefail
R=$(cd "$1" && pwd); OUT=$(realpath -m "$2")
J=${JOBS:-$(nproc)}
W=${W:-/var/tmp/q1native}; D=$W/dest; S=$W/src
HYBRIS=https://github.com/libhybris/libhybris;           HYBRIS_COMMIT=7079712a42ea2754adf747e70c6cc75764c8596e
AHEADERS=https://github.com/Halium/android-headers;     AHEADERS_COMMIT=217a38e92cc64a96b4d1ac7469f30827d84485d3 # halium-11.0
MONADO=https://gitlab.freedesktop.org/monado/monado.git; MONADO_COMMIT=cfa6078b8d7f368fa5296f540a436d59833e496c
OPENVR=https://github.com/ValveSoftware/openvr;          OPENVR_TAG=v2.15.6
VNCSERVER=https://github.com/LibVNC/libvncserver;        VNCSERVER_TAG=LibVNCServer-0.9.15
X11VNC=https://github.com/LibVNC/x11vnc;                 X11VNC_TAG=0.9.17

step() { echo "@step {\"step\": \"$1\"}"; echo "== $1"; }
fetch() { # url ref dir: a shallow checkout of one commit or tag
	[ -d "$3/.git" ] || { git init -q "$3"; git -C "$3" remote add origin "$1"; }
	git -C "$3" fetch -q --depth 1 origin "$2" && git -C "$3" checkout -q -f FETCH_HEAD && git -C "$3" clean -qfdx
}

step deps
grep -q '^DisableSandbox' /etc/pacman.conf || sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf
pacman -Sy --needed --noconfirm --disable-download-timeout $(sed 's/#.*//' "$R/tools/native-deps.txt")
rm -rf "$D"; mkdir -p "$D" "$S"
export PKG_CONFIG_PATH=$D/opt/hybris/lib/pkgconfig

step libhybris
fetch $AHEADERS $AHEADERS_COMMIT $S/android-headers
fetch $HYBRIS $HYBRIS_COMMIT $S/libhybris
# the Quest 1 runs Android 10: the halium-11.0 headers with their version set to 10
mkdir -p $D/opt/hybris/include/android /opt/hybris/include
cp -a $S/android-headers/. $D/opt/hybris/include/android/; rm -rf $D/opt/hybris/include/android/.git
sed -i 's/^#define ANDROID_VERSION_MAJOR .*/#define ANDROID_VERSION_MAJOR 10/' $D/opt/hybris/include/android/android-version.h
rm -rf /opt/hybris/include/android; ln -sfn $D/opt/hybris/include/android /opt/hybris/include/android
cd $S/libhybris/hybris
./autogen.sh --prefix=/opt/hybris --enable-arch=arm64 --enable-adreno-quirks --enable-wayland \
	--with-android-headers=/opt/hybris/include/android \
	--with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/apex/com.android.runtime/lib64/bionic >/dev/null
# GCC 14+: implicit declarations are errors; some of libhybris's tests (test_audio) rely on them
make -j$J CFLAGS="-g -O2 -DVK_ENABLE_BETA_EXTENSIONS -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types" >/dev/null
make install DESTDIR=$D >/dev/null
# the other builds find libhybris at its final path
rm -rf /opt/hybris; cp -a $D/opt/hybris /opt/hybris
# NDK-style <android/hardware_buffer.h> for Monado's AHardwareBuffer path (src/qbridge/monado/apply_ahb.sh)
H=$D/opt/hybris/include/ahb/android; mkdir -p $H
sed -e '/^#if __ANDROID_API__ >= [0-9]*/d' -e '/^#endif \/\/ __ANDROID_API__/d' -e 's/ __INTRODUCED_IN([0-9]*)//' \
	/opt/hybris/include/android/android/hardware_buffer.h > $H/hardware_buffer.h
cp "$(find /opt/hybris/include/android -name rect.h -path '*android/rect.h' | head -1)" $H/
rm -rf /opt/hybris; cp -a $D/opt/hybris /opt/hybris

step ours
cd "$R/src"
cc() { gcc -std=gnu11 -O2 -Wall -Wno-unused-parameter "$@"; }
# kernel uapi of the Quest's display driver, as <linux/msm_mdp*.h>
INC=$W/inc; mkdir -p $INC/linux; cp monado/src/xrt/compositor/main/msm/linux/msm_mdp*.h $INC/linux/
HY="-I$INC -I/opt/hybris/include -L/opt/hybris/lib -Wl,-rpath,/opt/hybris/lib -lhybris-common"
install -d $D/opt/hybris/lib $D/usr/local/bin $D/usr/local/lib/q1splash $D/usr/local/lib/q1track \
	$D/usr/local/lib/quest1-vk $D/usr/local/lib/quest1-vk-steam $D/data/local/tmp
cc -shared -fPIC -o $D/opt/hybris/lib/libbionictls.so bionictls/bionictls.c -ldl -lpthread
cc -fPIC -shared -fvisibility=hidden -Wextra -Wno-missing-field-initializers -Wno-format-truncation \
	-Imonado/src/xrt/compositor/main/msm -Imonado/src/xrt/compositor/main \
	-o $D/usr/local/lib/libVkLayer_quest1_steamvr_compat.so vklayer-steamvr/quest1_compat.c -lpthread
# -z nodelete: Chromium unloads Vulkan after probing it and loads it again for ANGLE
cc -fPIC -shared -fvisibility=hidden -Wl,-soname,libvulkan.so.1 -Wl,-z,nodelete \
	-o $D/usr/local/lib/quest1-vk-steam/libvulkan.so.1 vklayer-steamvr/quest1_vkshim.c vklayer-steamvr/quest1_wsi_x11.c \
	-ldl -lpthread -lxcb -lxcb-shm
cp $D/usr/local/lib/quest1-vk-steam/libvulkan.so.1 $D/usr/local/lib/quest1-vk/libvulkan.so.1
cc -fPIC -shared -o $D/usr/local/lib/quest1-vk-steam/libquest1_vkredirect.so vklayer-steamvr/quest1_vkredirect.c -ldl
# Remote Play hardware encoding: Steam's V4L2 encoder calls on msm_vidc (preloaded by steam-q1)
cc -fPIC -shared -fvisibility=hidden -Wextra -o $D/usr/local/lib/libquest1_v4l2.so v4l2shim/quest1_v4l2.c -lpthread
# Remote Play spectator: the PC's input, cursor and gamepads kept out of the headset (preloaded by steam-q1)
cc -fPIC -shared -fvisibility=hidden -Wextra -o $D/usr/local/lib/libquest1_rpspectator.so rpspectator/quest1_rpspectator.c -ldl
cc -Wextra -o $D/usr/local/bin/ovrstub ovrstub/ovrstub.c
gcc -O2 -Wall -o $D/usr/local/bin/q1black q1black/q1black.c $HY
gcc -O2 -Wall -o $D/usr/local/lib/q1splash/q1splash q1splash/q1splash.c $HY
gcc -O2 -o $D/usr/local/bin/xscreen xscreen/xscreen.c xscreen/gamepad.c -lopenxr_loader -lvulkan -lm

# bionic tools: linked against stubs of bionic's libc.so/libdl.so (the symbols they import; data objects are
# FILE pointers, 8 bytes as in bionic), resolved by Meta's real ones at run time
B=$W/bionic; mkdir -p $B
gcc -std=gnu11 -O2 -fPIE -fno-stack-protector -U_FORTIFY_SOURCE -c -o $B/q1trackd.o q1track/q1trackd.c
gcc -std=gnu11 -O2 -fPIC -mno-outline-atomics -fno-stack-protector -U_FORTIFY_SOURCE -fvisibility=hidden \
	-c -o $B/camtap.o camtap/camtap.c
nm -u $B/q1trackd.o $B/camtap.o | awk 'NF==2{print $2}' | sort -u > $B/syms
{ grep -E '^(stdin|stdout|stderr|environ)$' $B/syms | sed 's/.*/void *&;/'
  grep -vE '^(stdin|stdout|stderr|environ|dl[a-z_]*|android_dlopen_ext)$' $B/syms | sed 's/.*/void &(void) {}/'; } > $B/libc.c
grep -E '^(dl[a-z_]*|android_dlopen_ext)$' $B/syms | sed 's/.*/void &(void) {}/' > $B/libdl.c
gcc -w -shared -nostdlib -Wl,-soname,libc.so -o $B/libc.so $B/libc.c
gcc -w -shared -nostdlib -Wl,-soname,libdl.so -o $B/libdl.so $B/libdl.c
gcc -fPIE -pie -nostdlib -nostartfiles -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,now -Wl,--no-undefined \
	-Wl,--allow-shlib-undefined -o $D/usr/local/lib/q1track/q1trackd $B/q1trackd.o $B/libc.so $B/libdl.so
gcc -shared -nostdlib -Wl,-z,now -Wl,--no-undefined -Wl,--allow-shlib-undefined \
	-o $D/data/local/tmp/libq1camtap.so $B/camtap.o $B/libc.so $B/libdl.so

step steamvr
fetch $OPENVR $OPENVR_TAG $S/openvr
OVR="-I$S/openvr/headers -L$S/openvr/bin/linuxarm64 -l:libopenvr_api.so -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -Wl,--allow-shlib-undefined"
V=$D/opt/quest1-steamvr/quest1
install -d $V/bin/linuxarm64 $V/resources/input $V/resources/settings
cd "$R/src/steamvr-quest1"
g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wno-missing-field-initializers -I$S/openvr/headers \
	-o $V/bin/linuxarm64/driver_quest1.so driver_quest1.cpp chaperone_frame.cpp controllers.cpp battery.cpp camera.cpp \
	stereo_depth.cpp -ldl -lpthread -static-libstdc++ -static-libgcc -Wl,--no-undefined
cp quest1/driver.vrdrivermanifest $V/; cp quest1/resources/input/*.json $V/resources/input/
cp quest1/resources/settings/*.vrsettings quest1/resources/settings/*.json $V/resources/settings/
g++ -std=c++17 -O2 -o $D/usr/local/bin/vrdash vrdash.cpp $OVR
cd "$R/src/camtap"
g++ -std=c++17 -O2 -o $D/usr/local/bin/q1-camoverlay camoverlay.cpp $OVR
g++ -std=c++17 -O2 -o $D/usr/local/bin/q1-ptprovider ptprovider.cpp $OVR
# desktop mode: a nested Plasma's Xvfb as the SteamVR overlay gamescope would make (device/holo-desktop.sh)
cd "$R/src/q1desktop"
g++ -std=c++17 -O2 -Wall -o $D/usr/local/bin/q1desktop q1desktop.cpp $OVR -ldl -lX11 -lXdamage -lXtst
# apps started by Steam ("+" menu): each in its own overlay, Steam's X display covered (device/holo-desktop.sh)
g++ -std=c++17 -O2 -Wall -o $D/usr/local/bin/q1apps q1apps.cpp $OVR -ldl -lX11 -lX11-xcb -lxcb -lXext -lXdamage \
	-lXfixes -lXcomposite -lXtst -lxcb-shm
# System Monitor's GPU sensors (kgsl): replaces ksystemstats' DRM-only GPU plugin (device/holo-desktop.sh)
cmake -S "$R/src/ksysgpu" -B $W/ksysgpu -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
cmake --build $W/ksysgpu -j$J >/dev/null
DESTDIR=$D cmake --install $W/ksysgpu >/dev/null

step monado
fetch $MONADO $MONADO_COMMIT $S/monado
cd $S/monado
git apply "$R/src/monado/quest1-monado.patch"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/opt/monado \
	-DBUILD_TESTING=OFF -DBUILD_DOC=OFF >/dev/null
ninja -C build -j$J >/dev/null
DESTDIR=$D cmake --install build >/dev/null

step x11vnc
fetch $VNCSERVER $VNCSERVER_TAG $S/libvncserver
cmake -S $S/libvncserver -B $S/libvncserver/build -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF -DWITH_GNUTLS=OFF -DWITH_GCRYPT=OFF >/dev/null
make -C $S/libvncserver/build -j$J >/dev/null
make -C $S/libvncserver/build install >/dev/null            # for x11vnc's build
make -C $S/libvncserver/build install DESTDIR=$D >/dev/null
fetch $X11VNC $X11VNC_TAG $S/x11vnc
cd $S/x11vnc
autoreconf -fi >/dev/null 2>&1
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./configure --prefix=/usr/local >/dev/null
make -j$J >/dev/null
install -m755 src/x11vnc $D/usr/local/bin/x11vnc

step package
rm -rf $D/usr/local/include $D/usr/local/lib/pkgconfig $D/usr/local/lib/cmake $D/opt/monado/include
find $D -name '*.a' -delete; find $D -name '*.la' -delete
# no debug info in the image (Monado alone: 475 MB with it)
find $D -type f -exec sh -c 'for f; do head -c4 "$f" | grep -q ELF && strip --strip-debug "$f"; done' sh {} +
mkdir -p $D/usr/lib
cat >$D/usr/lib/quest1-native.json <<EOF
{"repo": "$(git -c safe.directory='*' -C "$R" rev-parse HEAD 2>/dev/null || echo unknown)",
 "libhybris": "$HYBRIS_COMMIT", "android-headers": "$AHEADERS_COMMIT", "monado": "$MONADO_COMMIT",
 "openvr": "$OPENVR_TAG", "libvncserver": "$VNCSERVER_TAG", "x11vnc": "$X11VNC_TAG"}
EOF
mkdir -p "$(dirname "$OUT")"
tar -C $D --numeric-owner --owner=0 --group=0 -cf - . | zstd -19 -T0 -q -f -o "$OUT"
echo "built $OUT ($(stat -c %s "$OUT") bytes)"
