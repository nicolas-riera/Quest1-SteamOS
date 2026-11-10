#!/bin/sh
# Build Mesa's native OpenGL driver in a separate staging directory.
# This is not an installer and does not change the running graphics stack.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 MESA_SOURCE BUILD_DIR STAGE_DIR" >&2
    exit 2
fi

source_dir=$(cd "$1" && pwd -P)
build_dir=$2
stage_dir=$3
mesa_commit=769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then
    echo "This build requires an aarch64 Linux host." >&2
    exit 1
fi
if [ "$(git -C "$source_dir" rev-parse HEAD)" != "$mesa_commit" ]; then
    echo "Mesa source must be at commit $mesa_commit (mesa-24.3.4)." >&2
    exit 1
fi
if [ -n "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]; then
    echo "Mesa source has tracked modifications; use a clean checkout." >&2
    exit 1
fi
for dir in "$build_dir" "$stage_dir"; do
    if [ -e "$dir" ]; then
        echo "Refusing to overwrite existing path: $dir" >&2
        exit 1
    fi
done
mkdir -p "$build_dir" "$stage_dir"
build_dir=$(cd "$build_dir" && pwd -P)
stage_dir=$(cd "$stage_dir" && pwd -P)

meson setup "$build_dir" "$source_dir" \
    --prefix=/usr --libdir=lib --buildtype=release \
    -Dplatforms=x11 -Dgallium-drivers=freedreno -Dvulkan-drivers= \
    -Dfreedreno-kmds=msm -Dglx=dri -Degl=enabled -Dgbm=enabled \
    -Dllvm=disabled -Dgallium-va=disabled -Dgallium-vdpau=disabled
meson compile -C "$build_dir"
DESTDIR="$stage_dir" meson install -C "$build_dir"
printf '%s\n' "Experimental OpenGL files staged in $stage_dir"
printf '%s\n' 'This build does not provide Vulkan or enable the Quest kernel DRM driver.'
