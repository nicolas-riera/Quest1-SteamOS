# Adreno 540 Vulkan blob: extension gating, qgl_config.txt, timeline semaphores

Offline static analysis of `system/vendor/lib64/hw/vulkan.msm8998.so` (V@0555, 06/2021, Vulkan 1.1.128) and
`system/vendor/lib64/libgsl.so`, plus the KGSL sources of our 4.4 kernel (`~/q1/kernel/drivers/gpu/msm`).
Personal interoperability use only.

Tags: **[V]** verified (code read, and for the gating pipeline also emulated with unicorn and checked against
`recon/vulkaninfo_hybris.txt`), **[I]** inferred.
Addresses are virtual addresses. For this ELF they equal file offsets: every PT_LOAD has vaddr == offset.
Exported `qglinternal::vk*` functions have names. All other functions are anonymous: `.gnu_debugdata` only holds
hashed local names, but it still gives function bounds.

## 0. TL;DR

- Extension visibility is a 96-entry table driven by a per-process **settings** struct. Three things clear entries:
  1. the **Android SDK level** (`ro.build.version.sdk`, which is 29 on the Quest);
  2. a **hard-coded "A5xx device class" method** (`0xbe0b8`), which clears about 35 extensions unconditionally;
  3. **`qgl_config.txt` overrides**, which are applied **after** both of the above and **win**. [V]
- All 7 of the 8 SteamVR extensions that exist in the blob can be turned back on **without patching**. Use
  `/data/vendor/gpu/qgl_config.txt` (fallback `/data/misc/gpu/qgl_config.txt`) with one line per extension, keyed by
  the hash of the extension name (`0x4c45e5a6=1` = VK_KHR_timeline_semaphore). The matching feature bits follow
  automatically. [V]
- `VK_EXT_shader_viewport_index_layer` **does not exist** in the blob: there is no string, no table entry and no code. [V]
- Advertising an extension does not make it work:
  - **Timeline semaphores need KGSL ioctls 0x58–0x5d, which our 4.4 kernel lacks.** Without them,
    `vkCreateSemaphore(TIMELINE)` fails with VK_ERROR_INITIALIZATION_FAILED. There is no emulation fallback. [V]
  - **The blob can never export a timeline semaphore**, and it does not support OPAQUE_FD for any semaphore type.
    External semaphores are SYNC_FD only. [V]
  - SteamVR shares timeline semaphores as OPAQUE_FD (steamvr-port.md §3.4/§8.1). **The compatibility layer is
    therefore needed even with a kernel backport.**

## 1. Extension table and enumeration [V]

- **Name table:** `.data.rel.ro` 0x190510, 96 `const char*` entries, indexed by an internal enum.
  Index list: §6, or run `idx.py` from the method notes.
- **Settings struct:** `settings = *(ctx+0x80)`. In the device context the struct is inline at `ctx+0xc0`.
  It holds four parallel `u32[96]` arrays:

  | Offset in settings | Array | Meaning |
  |---|---|---|
  | `+0x1634 + 4*i` | `supported` | the entry is listed only if this is `== 1` |
  | `+0x17b4 + 4*i` | `hidden` | must be `== 0` |
  | `+0x1934 + 4*i` | `type` | 0 = device, 1 = instance |
  | `+0x1ab4 + 4*i` | `specVersion` | |

- **Enumeration function:** `0x82808` (`count*`, `props*`, `ctx`, `isInstance`).
  - `vkEnumerateDeviceExtensionProperties` (0x76cb8) passes `pd->[8]->[0x1208]->[0x80]`.
  - `vkEnumerateInstanceExtensionProperties` (0x76ce0) creates a throw-away *instance* context.
  - The instance context's vtable (0x192590) has stub methods and **skips the A5xx clearing**.
  - This explains why Monado saw `VK_EXT_debug_utils` listed at instance level but rejected at device level:
    the device class clears it (0xbe3e0).
- **Feature reporting:** `vkGetPhysicalDeviceFeatures2` (0x788c8) copies the feature bits from the physical-device
  object. Those bits are filled from the same `supported` flags in `0xa6f98` (physical-device/context init):

  | Feature | pd offset | Source flag | Code |
  |---|---|---|---|
  | `hostQueryReset` | +0x824 | sup[45] | 0xa8e40 |
  | `timelineSemaphore` | +0x828 | sup[37] | 0xa8e48 / 0xa982c |
  | `extendedDynamicState` | +0x870 | sup[25] | 0xa93a0 |
  | `customBorderColors` + `customBorderColorWithoutFormat` | +0x880 (2 bools) | sup[44] | 0xa93e4 |

  So an override of the extension also turns its feature on.

## 2. Gating pipeline (context creation `0x9cdb0`) [V]

1. **Parse `qgl_config.txt`** into a key list: 0x9d050–0x9d4dc (§3).
2. `property_list(cb=0x9e748)`: every system property *name* is hashed and looked up in a 626-entry hash table at
   0x17b884. The device class adds an 84-entry table at 0x18ad0c (0xbe900). On a hit, the value string is copied
   into the table.
   The property names were not recovered: none match `debug.*`, `vendor.*`, `persist.*` or `ro.*` combined with any
   string in the blob. The consumer is not identified, and these hashes are disjoint from the qgl_config keys. [I]
3. **Defaults** (0x9d6e8–0x9e130). Emulated result: every extension of interest defaults to `supported=1`, except
   swapchain, surface, copy_commands2, robustness2, fragment_shading_rate and a few QCOM entries.
4. **SDK gate** at 0x9e1a4: `atoi(property_get("ro.build.version.sdk"))`. This is the only reader of that property.

   | SDK | Extensions cleared |
   |---|---|
   | ≤ 29 | `uniform_buffer_standard_layout` (0x1784), `shader_subgroup_extended_types` (0x1700), `buffer_device_address` (0x1690), **`timeline_semaphore` (0x16c8)** |
   | ≤ 28 | also `driver_properties`, `depth_stencil_resolve`, `vulkan_memory_model` |
   | ≤ 27 | also `ANDROID_external_memory_android_hardware_buffer`, `create_renderpass2` |
   | ≥ 29 | sets `AHB specVersion = 2` |

5. **Device-class method** `vtable(0x192c40)+0x58` = **0xbe0b8**, called at 0x9e240.
   - It reads the GPU model from `devinfo+0x119c` (0x40021c = "540"; 0x4001fe = 510).
   - It **unconditionally** zeroes `supported[]` for:
     - depth_range_unrestricted, subgroup_size_control, shader_float16_int8, copy_commands2, spirv_1_4, astc_decode_mode;
     - buffer_device_address, **extended_dynamic_state**, shader_terminate_invocation, scalar_block_layout,
       vertex_attribute_divisor, primitives_generated_query;
     - **timeline_semaphore**, imageless_framebuffer, **custom_border_color**, **host_query_reset**;
     - rotated_copy_commands, shader_subgroup_extended_types, robustness2, separate_stencil_usage, IMG/EXT filter_cubic;
     - **image_format_list**, 16bit_storage, shader_demote, render_pass_transform, depth_clip_control, transform_feedback;
     - blend_operation_advanced, provoking_vertex, depth_stencil_resolve, shader_float_controls, vulkan_memory_model,
       descriptor_indexing;
     - fragment_shading_rate, debug_utils, draw_indirect_count, uniform_buffer_standard_layout,
       primitive_topology_list_restart, **sample_locations**.
   - Store sites: 0xbe350–0xbe400.
   - Only when the model is **not** 540 does it also clear fragment_density_map and fragment_density_map2
     (0xbe424/0xbe42c). FDM is kept on the Quest.
6. **Config overrides** `0x15a360` (17.6k instructions, called at 0x9e250).
   - For each of about 970 known key hashes, it searches the parsed list and stores the parsed value into its
     settings field.
   - It includes **one boolean key per extension** that writes `supported[i]` (0x168738–0x16b6b8).
7. Optional `libadreno_app_profiles.so!ApplyApplicationProfile` when `settings+0x15e8 == 1`.
   The Quest image has no such library. [V]
8. API-version fix-ups (0x9e3dc): the version is `settings+0x2ec/0x2f0/0x2f4` (major/minor/patch).
   If it is below 1.1, `spirv_1_4` is cleared.
9. If `settings+0x88` (a process-name filter string) is non-empty and differs from `__progname`, 0x174578 resets part
   of the settings. It does not touch the extension arrays. [V/I]

**Validation [V]:** I emulated steps 3+4+5 with unicorn (SDK=29, model 0x40021c). The predicted device list has
41 entries and matches `vulkaninfo_hybris.txt` exactly. Its 44 entries minus the 4 that Android's loader adds
(`KHR_swapchain`, `GOOGLE_display_timing`, `KHR_incremental_present`, `KHR_shared_presentable_image`) leave 40.
Add `ANDROID_native_buffer`, which the loader hides, and you get the same 41. I then emulated step 6 with a parsed list holding the 6 keys below (hex and named forms
mixed): all 6 extensions reappeared, and a `=0` key removed `KHR_multiview`.

### Why each SteamVR-relevant extension is hidden on A540

| Extension | idx | Default | Cleared by | Config key (hash) | Notes |
|---|---|---|---|---|---|
| VK_KHR_timeline_semaphore | 37 | 1 | SDK ≤ 29 (0x9e1e0) **and** A5xx class (0xbe3c4) | `0x4c45e5a6` | Also needs kernel support (§4) |
| VK_KHR_image_format_list | 59 | 1 | A5xx class (0xbe368) | `0xa0b0665d` | Pure API struct; should be harmless [I] |
| VK_EXT_host_query_reset | 45 | 1 | A5xx class (0xbe3ac) | `0x5c029c11` | CPU reset of query memory; likely OK [I] |
| VK_EXT_custom_border_color | 44 | 1 | A5xx class (0xbe3e4) | `0x5ad1c414` | A5xx sampler path untested by Qualcomm [I] |
| VK_EXT_extended_dynamic_state | 25 | 1 | A5xx class (0xbe3cc) | `0x2f4e28ec` | `vkCmdSet*EXT` entry points exist (exported). Whether the A5xx state emitter honours them is unknown [I] |
| VK_EXT_sample_locations | 92 | 1 | A5xx class (0xbe378) | `0xf9b17f24` | Programmable sample positions on A5xx are unverified [I] |
| VK_EXT_shader_viewport_index_layer | — | — | **not implemented** | — | No name or entry. `multiViewport=false`. Must be faked or rewritten in the layer |
| VK_KHR_driver_properties | 83 | 1 | only if SDK < 29 | `0xd6067eac` | **Already exposed** (SDK 29) |
| VK_EXT_extended_dynamic_state3 | — | — | not in blob | — | — |

## 3. qgl_config.txt [V]

- **Lookup order:** `/data/vendor/gpu/qgl_config.txt`, then `/data/misc/gpu/qgl_config.txt`.
  - Built at 0x9d0f0 and 0x9d55c with path helper 0xfc650 plus 0xfc9a8, then `..`/`/` normalisation, then `fopen("r")`.
  - Success logs `Found an QGL Settings File: %s` through liblog. fakelogd will capture it.
  - Under libhybris these are plain Linux paths, so create them in the Holo rootfs.
  - The file's mtime is recorded (difftime against 2013-01-01) but was not seen used as a gate. [I]
- **Line syntax:**
  - Lines are read with `fgets(512)`; the first character `;` marks a comment.
  - The line is cut at whitespace (`strtok_r " \t\n\r\f\v"`), and only the **first whitespace-free token** is used.
  - That token is then split on `" ="` into key and value.
  - **Write `KEY=VALUE` with no spaces**; a trailing `   ; comment` is fine.
- **Keys:**
  - The key is hashed case-insensitively: `h = 0x425534b3; for c: h = tolower(c) ^ ror32(h, 27)`.
  - A key written as `0x…` is taken literally as the hash (`strtoul(key, 0, 0)`).
- **Lock:**
  - While the file is "locked", **named keys** are replaced by `0xdeadbeef` (ignored), except the one whose hash is
    `0x8c292fb5` (name not recovered).
  - **Hex-hash keys bypass the lock** (0x9d388 → 0x9d3f8).
  - The line `0x0=0x8675309` sets the unlock flag (`*(cfg+0xe8)=1`). Every later named key is then accepted.
- **Boolean values** (per-extension keys):
  - The first character decides: `1`/`t`/`T` → 1, `0`/`f`/`F` → 0. Anything else is ignored.
  - Applied entries are flagged and dumped as `SETTING %s=%s` between `===== BEGIN/END DUMP OF OVERRIDDEN SETTINGS`
    (0x9e648). This is useful for checking in the journal.
- **Recognised keys:** about 970 hashes, in the sorted compare chain of `0x15a360`.
  - Named so far: the **96 extension names themselves**, `GmemSize`, `RenderMode`.
  - API-version keys exist: `0x7c0c0f03` → major, `0x7c2c8f03` → minor, `0x728fc3d3` → patch. Their names are
    unknown and the value parser was not checked. **Do not force 1.2**: the 1.2 core entry points and features are
    incomplete on A5xx. [I]
  - No global "disable gating" or "expose all" key was found [I]. Gating is per extension anyway.
- **System properties read by name** [V]:
  - `ro.build.version.sdk` (the gate above);
  - `ro.gfx.driver.0` / `ro.gfx.driver.1` (0xfbfd8/0xfc030; updatable-driver detection [I]);
  - `debug.vulkan.profiler`, `debug.graphics.gpu.profiler.perfetto`;
  - `vendor.debug.scope.{enable,frames,firstframe}`, `vendor.debug.fbodump.enabled`.
  - Plus the hashed `property_list` tables (§2 step 2).
  - Nothing matches `vendor.gfx.*` or `debug.vk.*` by name.

**Ready-to-use file** (no unlock needed):
```
; /data/vendor/gpu/qgl_config.txt  -- no spaces around '='
0x4c45e5a6=1   ; VK_KHR_timeline_semaphore (only works with KGSL timeline ioctls, see section 4)
0xa0b0665d=1   ; VK_KHR_image_format_list
0x5c029c11=1   ; VK_EXT_host_query_reset
0x5ad1c414=1   ; VK_EXT_custom_border_color
0x2f4e28ec=1   ; VK_EXT_extended_dynamic_state
0xf9b17f24=1   ; VK_EXT_sample_locations
```
The equivalent named form starts with `0x0=0x8675309`, then one line per extension, e.g. `VK_EXT_host_query_reset=1`.
The other 90 extension keys follow the same pattern; compute them with the hash above.
The file is global: it affects every Vulkan process using the blob.

## 4. Timeline semaphores

**Driver side [V]:**
- `vkCreateSemaphore` (0x75428) handles `VkSemaphoreTypeCreateInfo{TIMELINE}` in 0xb9838.
  - It calls gsl table slot +0x158 (`gsl_timeline_create`, profiling wrapper 0xad4f0) with the initial value.
  - It maps gsl errors through 0x3a2d8; most errors become **VK_ERROR_INITIALIZATION_FAILED**.
  - **There is no fallback emulation.**
- Host wait, signal and query go through `gsl_multiple_wait_timelines`, `gsl_timeline_signal` and
  `gsl_timeline_query`. GPU-side signalling goes through `gsl_command_issueib_timeline_sync`.
- **External handles:**
  - `vkGetPhysicalDeviceExternalSemaphoreProperties` (0x787c8) returns **0 for every handle type** when a
    `VkSemaphoreTypeCreateInfo` with type ≠ BINARY is chained.
  - For binary semaphores only `SYNC_FD` (0x10) is exportable or importable. `vkGetSemaphoreFdKHR` (0x7c340) and
    `vkImportSemaphoreFdKHR` (0x7c430) reject every other type with `VK_ERROR_INVALID_EXTERNAL_HANDLE`.
  - **OPAQUE_FD semaphores are unsupported altogether.**

**libgsl ioctls [V]** (`ioctl_kgsl_*`, KGSL type 0x09):

| libgsl function | ioctl |
|---|---|
| timeline_create | `0xc0100958` (nr 0x58, 16 B) |
| multiple_wait_timelines | `0x40280959` (0x59, 40 B) |
| timeline_query | `0xc010095a` (0x5a) |
| timeline_signal | `0x4010095b` (0x5b) |
| timeline_destroy | `0x4004095d` (0x5d) |
| gpu_signal_aux_command (`gpu_aux_signal_timeline(_sync)`, `sharedmem_bind_sync`) | `0xc0300957` (0x57, IOCTL_KGSL_GPU_AUX_COMMAND) |
| sharedmem_bind | `0xc0200956` (0x56, GPUMEM_BIND_RANGES) |

- These match CAF msm-5.4 `msm_kgsl.h` (FENCE_GET 0x5c is not used).
- `gsl_timeline_create` has no capability probe. It only short-circuits when a global flag bit (byte 45, bit 5) is set.

**Our 4.4 KGSL [V]:**
- It has **no timeline code**: the ioctl table ends at 0x57, there is no `kgsl_timeline.c`, and there is no
  `KGSL_CMD_SYNCPOINT_TYPE_TIMELINE`. Ioctls 0x58–0x5d return `-ENOIOCTLCMD` (ENOTTY), so creation fails cleanly.
- **Hazard:** 0x56/0x57 are Oculus additions here (`ALLOW_UID_HIGH_PRIORITY`, `ALLOW_TID_MAXIMUM_PRIORITY`).
  `kgsl_ioctl_helper` dispatches on `_IOC_NR` only.
  - A root caller (CAP_SYS_NICE) issuing libgsl's AUX command (0xc0300957) would hit
    `kgsl_ioctl_allow_tid_maximum_priority`.
  - That handler reads the first 4 bytes of the aux struct as a tid and **returns 0 without signalling anything**:
    a silent hang.
  - A backport must dispatch AUX by full command value (or renumber) and keep the Oculus ioctls working.
- **Backport estimate [I]:**
  - Upstream pieces: `kgsl_timeline.c` (about 550 lines) and `.h` (about 100); timeline syncpoints in
    `kgsl_drawobj.c` plus the AUX command (`kgsl_ioctl_gpu_aux_command`, timeline drawobj, dispatcher retire); uapi
    structs. About **1.3–1.5k lines**.
  - 4.4 has `struct fence` (`fence_init/signal`, `fence_wait_any_timeout`) and staging `sync_fence`/`sync_pt`.
  - 4.4 lacks `dma_fence` naming, `dma_fence_array` and `sync_file`. Adapt them via compat macros, per-fence waits
    and a staging `sync_timeline`, as `kgsl_sync.c` already does for context timestamps.
  - Effort: several days, with GPU-hang risk.
  - **This does not solve SteamVR**: the semaphores still cannot be exported as OPAQUE_FD. The CPU-side timeline
    emulation in the compatibility layer (steamvr-port.md §8.1: binary SYNC_FD and fences plus a memfd counter and
    futex) is needed either way. That layer works on the current kernel. **Recommendation: do not backport.**
    Leave `VK_KHR_timeline_semaphore` hidden in the blob and implement it in the layer.

## 5. Making hidden extensions appear

1. **No patch (preferred) [V]:** use the `qgl_config.txt` above. It overrides both the SDK gate and the A5xx clear.
   A fake `ro.build.version.sdk=30` (via `mkproparea.py -s`) is **not** needed, and would also change other Android
   libraries' behaviour.
2. **Binary patch (only if config loading is impossible):** NOP (`0xd503201f`) the clearing stores. File offset equals
   vaddr.
   - `0x9e1e0` (SDK clear of timeline)
   - `0xbe3c4` timeline
   - `0xbe368` image_format_list
   - `0xbe3ac` host_query_reset
   - `0xbe3e4` custom_border_color
   - `0xbe3cc` extended_dynamic_state
   - `0xbe378` sample_locations

   Risk: the same as the config route, because the extension's code path runs on A540 untested by Qualcomm. The copy
   is overlaid in tmpfs, so nothing on the device is modified.
3. **Practical advice:**
   - Enabling `image_format_list` and `host_query_reset` in the blob is low-risk and saves layer work.
   - `custom_border_color`, `extended_dynamic_state` and `sample_locations` should first be tested with a small
     Vulkan test program. Otherwise keep them faked in the layer (steamvr-port.md §8.2: SteamVR's default path uses no
     EDS state).
   - `timeline_semaphore` must stay in the layer (§4).
   - `shader_viewport_index_layer` must be handled in the layer, by SPIR-V rewrite or per-eye draws.

## 6. Method notes

- Tools: capstone and unicorn in WSL (`apt install python3-capstone python3-pyelftools python3-unicorn`, as root),
  plus `aarch64-linux-gnu-objdump`. The scripts are in the session scratchpad, not kept.
- Extension index order (name-table index = array index):
  - 0 depth_range_unrestricted, 25 extended_dynamic_state, 37 timeline_semaphore, 44 custom_border_color,
    45 host_query_reset, 59 image_format_list, 83 driver_properties, 92 sample_locations.
  - The array offset is `0x1634 + 4*i` (supported) in settings, or `0x16f4 + 4*i` relative to the device context.
- Per-extension key hash = `H(extension name)`. For example `H("VK_KHR_maintenance1") = 0x29391591`.
