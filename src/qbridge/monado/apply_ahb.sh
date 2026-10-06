#!/bin/bash
# Make a Linux Monado build share swapchains as AHardwareBuffers (Monado's
# Android path), backed by libhybris. Needed because the Quest's Vulkan driver
# can't export/import colour images as opaque fds. Idempotent.
# usage: apply_ahb.sh <monado_src_dir>
set -e
M=${1:?monado dir}; Q=$(cd "$(dirname "$0")" && pwd)
X=$M/src/xrt

# Minimal NDK headers for <android/hardware_buffer.h> (don't put the whole
# android-headers tree on the include path: it has its own linux/ dir).
H=/opt/hybris/include/ahb/android
mkdir -p $H
# Drop the bionic API-level guards/attributes: under glibc everything is "available".
sed -e '/^#if __ANDROID_API__ >= [0-9]*/d' -e '/^#endif \/\/ __ANDROID_API__/d' -e 's/ __INTRODUCED_IN([0-9]*)//' \
  /opt/hybris/include/android/android/hardware_buffer.h > $H/hardware_buffer.h
cp $(find /opt/hybris/include/android -name rect.h -path "*android/rect.h" | head -1) $H/

cp $Q/../linux/hybris_ahb.c $X/auxiliary/util/u_hybris_ahb.c

if ! grep -q XRT_HYBRIS_AHB $M/CMakeLists.txt; then
  sed -i 's|^if(ANDROID)\n\tset(VK_USE_PLATFORM_ANDROID_KHR TRUE)||' $M/CMakeLists.txt
  sed -i 's|^include(CompilerFlags.cmake)|option(XRT_HYBRIS_AHB "Share swapchains as AHardwareBuffers through libhybris" ON)\nif(XRT_HYBRIS_AHB AND NOT ANDROID)\n\tset(VK_USE_PLATFORM_ANDROID_KHR TRUE)\n\tadd_compile_definitions(XRT_HYBRIS_AHB)\n\tinclude_directories(SYSTEM /opt/hybris/include/ahb /opt/hybris/include)\nendif()\n\n&|' $M/CMakeLists.txt
fi

# Handle type selection.
sed -i 's|^#if defined(XRT_OS_ANDROID) \&\& defined(XRT_OS_ANDROID_USE_AHB) \&\& (__ANDROID_API__ >= 26)$|#if (defined(XRT_OS_ANDROID) \&\& defined(XRT_OS_ANDROID_USE_AHB) \&\& (__ANDROID_API__ >= 26)) \|\| defined(XRT_HYBRIS_AHB)|' $X/include/xrt/xrt_handles.h
sed -i 's|^#elif defined(XRT_OS_ANDROID) \&\& !defined(XRT_OS_ANDROID_USE_AHB) \|\| defined(XRT_OS_LINUX) \|\| defined(XRT_OS_OSX)$|#elif defined(XRT_OS_ANDROID) \&\& !defined(XRT_OS_ANDROID_USE_AHB) \|\| (defined(XRT_OS_LINUX) \&\& !defined(XRT_HYBRIS_AHB)) \|\| defined(XRT_OS_OSX)|' $X/include/xrt/xrt_handles.h

python3 - $X/include/xrt/xrt_handles.h <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
cond = "|| defined(XRT_HYBRIS_AHB)\n"
inc = "#ifdef XRT_HYBRIS_AHB\n#include <android/hardware_buffer.h>\n#endif\n"
if inc not in s:
    s = s.replace(cond, cond + inc, 1)
    open(p, "w").write(s)
PY

# AHB allocator + forwarding shim in aux_util (linked by everything).
if ! grep -q u_hybris_ahb.c $X/auxiliary/util/CMakeLists.txt; then
  cat >> $X/auxiliary/util/CMakeLists.txt <<'C'

if(XRT_HYBRIS_AHB AND NOT ANDROID)
	target_sources(aux_util PRIVATE u_hybris_ahb.c ../android/android_ahardwarebuffer_allocator.c)
	target_link_libraries(aux_util PUBLIC -L/opt/hybris/lib -lhybris-common -Wl,-rpath,/opt/hybris/lib)
endif()
C
fi
grep -n "XRT_HYBRIS_AHB" $X/include/xrt/xrt_handles.h $M/CMakeLists.txt | head
echo "hybris AHB patch applied"
