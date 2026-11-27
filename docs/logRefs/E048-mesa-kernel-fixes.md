<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E048 — Mesa and kernel fixes for the vkfwtest battery

**Goal.** Fix the bugs in Mesa (`NoBlobs/mesa`, branch `pvr-bxe-2-32`) and in the kernel driver
(`NoBlobs/linux`, branch `IMG-BXE-2-32`) so that every vkfwtest test passes with our firmware.

**Board.**
- BPI-F3, Arch Linux RISC-V, kernel `7.4.0-rc1-img`, our firmware v1.1.
- Mesa cross-compiled on the PC from the repository, installed in `~/mesa-dev`.
- The kernel's `powervr.ko` rebuilt from the repository and installed in place; the original
  is kept in `~/powervr.ko.orig`.

**Starting point** (repository Mesa, which had a local patch assuming firmware 1.1):
**39 pass / 9 fail / 2 skip**.

| Failing test | Cause |
|---|---|
| `second-*` (6) | Mesa ignores external subpass dependencies (E047) |
| `rta` step 3 | a layered `LOAD` loads layer 0 into every layer |
| `bigpb-layers` | partial render of a layered render |
| `stress` | intermittent |

## Kernel: UAPI 1.1.0 and `DRM_PVR_DEV_QUERY_FW_INFO_GET`

Mesa asks for the firmware version only when the driver reports DRM 1.1.0. Without the
answer it assumes firmware 1.0 and turns `gs_rta_support` off (E045).

The changes:
- `include/uapi/drm/pvr_drm.h`: `struct drm_pvr_dev_query_fw_info` and
  `DRM_PVR_DEV_QUERY_FW_INFO_GET`, as in Mesa's copy of the UAPI;
- `pvr_drv.c`: `pvr_dev_query_fw_info_get()` returns `pvr_dev->fw_version.major/minor`;
- `pvr_drv.h`: `PVR_DRIVER_MINOR` = 1, and the type is added to `PVR_UOBJ_MIN_SIZE`.

**Result:** dmesg shows `Initialized powervr 1.1.0`. An unpatched public Mesa (`5d2dc45`)
turns RTA on by itself: no "Missing FW support" warning, and the layer loop runs. The Mesa
repository's local FW-1.1 patch was reverted.

## Mesa 1: dependencies on `VK_SUBPASS_EXTERNAL`

The changes:
- `pvr_pass.h`: the render pass stores its external dependencies;
- `pvr_arch_pass.c` (`CreateRenderPass2`): they are filled in, from the `VkMemoryBarrier2` in
  `pNext` when there is one;
- `pvr_arch_cmd_buffer.c`: `pvr_cmd_buffer_external_deps()` applies them through
  `CmdPipelineBarrier2`. Dependencies from outside run before the render pass begins; those to
  outside run after it ends, once the render-pass state is cleared.

**Result:** all 8 `second-*` tests pass, without vkfwtest's read-back barrier.

## Mesa 2: layered `LOAD` without the background layer id

- **The problem:** the background object that runs a render's load-op program does not get the
  render target index: `RENDER_TGT_ID` reads 0 there. Only the end-of-tile program gets the
  current layer, from `BCAST[0x03D8]`. So a render-target-array render that loads a layered
  attachment loads layer 0 into every layer.
- **Same with the manufacturer firmware (Q37).** Rewriting the background registers on every
  layer, as the Q36 answer describes, changed nothing (firmware `3175577…`).
- **The fix** (`rogue/pvr_blit.c`, `pvr_clear_use_rta()`):
  - a render whose colour attachment is loaded from a view with more than one layer clears the
    layers other than 0 with the deferred transfer clears, which is the path Mesa uses without
    `gs_rta_support`;
  - no geometry then reaches the other layers, so only layer 0 is rendered, and its load is
    right;
  - other renders keep the in-render RTA clears.

**Result:** `rta` passes, step 3 included, and `msaa`, `multiview`, `secondary` and `mrt` still
pass.

## Firmware

`frag_rta_next()` now writes `pds_bgnd0/1/3`, `isp_bgobjdepth`, `isp_bgobjvals` and `0x0F10`
again for every layer, as the Q36 answer says the manufacturer firmware does. No test changed.

## Result

**46 pass / 2 fail / 2 skip.** Skipped: `local-read` and `sparse`, which the driver does not
support.

| Still failing | Cause | Next |
|---|---|---|
| `bigpb-layers` | a partial render of a layered render: the final render of each layer must reload that layer's partial result through the background object, which always gets layer 0. Mesa's SPM also handles layer 0 only. | **Q39**: is there a register that gives the background task its render target index? |
| `stress` | a 3D kick on one of four contexts (the second render of a device, on its second HWRTData) never raises `PIXELBE_END_RENDER`. Lockup detected ~36 s later, then HWR. Some runs page-fault at `0x1180`. The manufacturer firmware fails too. | **Q38** |
