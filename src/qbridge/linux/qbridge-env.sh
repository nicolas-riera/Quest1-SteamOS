# Source me inside the Holo chroot: environment for Monado + OpenXR apps on the
# Quest's own Vulkan driver (libhybris) displaying through the qbridge app.

# Quest Vulkan driver through libhybris; bionic code needs its TLS slot.
export LD_LIBRARY_PATH=/opt/hybris/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so${LD_PRELOAD:+:$LD_PRELOAD}

# Monado: qbridge builder + compositor target, graphics (not compute) path
# because the AHardwareBuffer-backed target images can't be storage images.
export QBRIDGE_ENABLE=1
export XRT_COMPOSITOR_COMPUTE=0
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"

# OpenXR apps: use Monado.
export XR_RUNTIME_JSON=/opt/monado/share/openxr/1/openxr_monado.json
export PATH=/opt/monado/bin:$PATH
