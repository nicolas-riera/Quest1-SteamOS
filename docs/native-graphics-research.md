# Quest 1 native graphics bring-up

This is experimental research, not a working replacement graphics stack. It was
prepared from Quest1-SteamOS commit `595f091714d9cd7d1f48a7af2061e53b082e5495`.
No Quest 1 was available for testing.

## Existing path

The device uses Meta's 4.4 kernel with `CONFIG_QCOM_KGSL=y`,
`CONFIG_DRM` disabled, and the MSM framebuffer display driver. Steam and
SteamVR use the Android Adreno driver via libhybris and the project's Vulkan
shim and compatibility layer. The direct scanout path commits to `/dev/fb0`;
it is coupled to gralloc buffers, panel power state, and fence handling. See
`steam-gpu.md`, `steamvr-port.md`, and `direct-scanout.md`.

## Firmware inspection

The OTA `q1_49845030443200410.zip` matched the SHA-256 in
`installer/sources.json` and its metadata identifies the `monterey` build
`49845030443200410`. The extracted `system` and `modem` partition hashes
matched the OTA manifest. The archive hash provides identity against the
project's pin, not an independent signature verification.

The 64-bit GPU files include `vulkan.msm8998.so`, `libgsl.so`,
`gralloc.msm8998.so`, `libEGL_adreno.so`, and `libGLESv2_adreno.so`.
The Vulkan library directly depends on `libgsl.so`, `libadreno_utils.so`,
`libllvm-glnext.so`, Android platform libraries, and the C runtime. The system
partition includes `a530_pfp.fw`, `a530_pm4.fw`, and `a540_gpmu.fw2`; the
modem image contains `a540_zap` files. These proprietary files are not included
in this repository. The modem FAT image is shorter than the size stated by its
filesystem header; 7-Zip reported an unexpected end of archive while extracting
the zap files, so their integrity has not been independently established.

## Kernel and Mesa findings

The project pins Meta kernel commit `6929f734ce0e602018790ff3a52dc7bad646af60`.
Its MSM DRM source has an A540 implementation and recognizes the KGSL GPU
device-tree compatible string. However, its Makefile excludes that GPU code
when `CONFIG_QCOM_KGSL=y`; the DRM driver's GPU load and registration functions
are also disabled. Its initialization requires a working KMS implementation.
These are constraints, not proof that disabling KGSL will produce a bootable
headset. GPU and display resource ownership, device tree, firmware loading,
buffer sharing, synchronization, and tracking dependencies all need testing.

`tools/build-freedreno-experimental.sh` builds an isolated native Mesa 24.3.4
Freedreno OpenGL staging tree for aarch64 Linux. It does not install files into
the system, enable DRM, provide a Vulkan ICD, or change the default boot path.
It targets MSM DRM. Neither kernel ABI compatibility nor runtime behavior has
been verified on Quest 1. The existing Mesa Turnip driver does not implement
Adreno 5xx Vulkan support, so the build explicitly excludes Vulkan.

To build on an aarch64 Linux development machine with Mesa build dependencies:

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git mesa
git -C mesa checkout --detach 769e51468b49b2a42f0a0eaf71cf9eed5ff4e5de
sh tools/build-freedreno-experimental.sh mesa mesa-build mesa-stage
```

The separate CI workflow lists the required Ubuntu 24.04 packages. A successful
build only establishes that userspace compiles; it does not establish hardware
support. Do not deploy it as a replacement until the kernel and display paths
have been implemented and tested using a recoverable boot on real hardware.
