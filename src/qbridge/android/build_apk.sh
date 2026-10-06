#!/bin/bash
# Build qbridge.apk without Gradle (run in WSL). Output: build/qbridge.apk
set -e
SRC=${SRC:-$(cd "$(dirname "$0")" && pwd)}
SDK=$HOME/android; NDK=$SDK/ndk/27.2.12479018; BT=$SDK/build-tools/34.0.0
JAR=$SDK/platforms/android-32/android.jar
OUT=$HOME/q1/qbridge-build; mkdir -p $OUT
CMAKE=$SDK/cmake/3.22.1/bin/cmake
$CMAKE -S $SRC -B $OUT/cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 >/dev/null
$CMAKE --build $OUT/cmake -j$(nproc) 2>&1 | grep -E "error|warning: unused|Linking" || true
test -f $OUT/cmake/libqbridge.so
rm -rf $OUT/apk && mkdir -p $OUT/apk/lib/arm64-v8a
cp $OUT/cmake/libqbridge.so $SRC/../third_party/openxr_aar/jni/arm64-v8a/libopenxr_loader.so $OUT/apk/lib/arm64-v8a/
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-debug $OUT/apk/lib/arm64-v8a/libqbridge.so
$BT/aapt2 link -o $OUT/base.apk --manifest $SRC/AndroidManifest.xml -I $JAR --min-sdk-version 29 --target-sdk-version 29
(cd $OUT/apk && zip -q -r -0 ../base.apk lib)
$BT/zipalign -f -p 4 $OUT/base.apk $OUT/aligned.apk
KS=$HOME/q1/debug.keystore
[ -f $KS ] || keytool -genkeypair -keystore $KS -storepass android -keypass android -alias debug \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=qbridge debug" >/dev/null 2>&1
$BT/apksigner sign --ks $KS --ks-pass pass:android --out $OUT/qbridge.apk $OUT/aligned.apk
mkdir -p /mnt/d/Documents/Projets/Quest1-SteamOS/build && cp $OUT/qbridge.apk /mnt/d/Documents/Projets/Quest1-SteamOS/build/
ls -la /mnt/d/Documents/Projets/Quest1-SteamOS/build/qbridge.apk
