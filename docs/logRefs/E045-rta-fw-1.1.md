<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E045 — Render-target arrays (firmware 1.1) and the frame-1 failures

**Question.** `docsClean` `kick-compute-and-3d.md` ("Render-target arrays", answer to Q27)
describes the layer loop that public Mesa expects from firmware 1.1 (`gs_rta_support`). Does
our implementation of it work on the board? And can `indirect` and `vertex-ext`, which pass with
the manufacturer firmware, be made to pass?

**Board.**
- BPI-F3 16 GB at `192.168.0.110`, kernel `7.4.0-rc1-img`.
- GPU clock 307.2 MHz (`clk_summary`). The reference results in `docsClean` used 819.2 MHz.
- Mesa `/opt/mesa-pvr`, `27.0.0-devel (git-f7c93a1352)`, 2026-10-21.

## Firmware

- `src/frag.c`:
  - `rta_setup()` programs the first layer at the fragment kick:
    - region headers;
    - PBE word 1 of array MRTs;
    - ZLS/SLS bases.
  - `frag_rta_next()` at `PIXELBE_END_RENDER` advances `render_target_index` and re-kicks DM 3,
    before the flush and the retire.
  - Partial renders stay on layer 0.
- `tools/mkfw.py`: version 1.1.
- `active_render_targets` and `valid_render_targets[]` are filled by our choice (Q31). The
  experiment `EXP_RTA_ACTIVE_ONLY` turns that choice off.
- Build: no warnings. `fw-preflight` PASS.

## Result: the layer loop is never reached with this Mesa

- The kernel loads the firmware and reports `FW version v1.1 (build 1 OS)`.
- `vkfwtest rta` passes (64×64 × 6 layers, colour + D32). But the trace has one `Kick 3D` per
  frame and no `MAIN_39`/`MAIN_40`.
- A temporary trace at `rta_setup()` shows why: `max_rts = 1`, `valid_render_targets_fw_addr = 0`.
  Mesa created the dataset with one layer.
- **Cause, in public Mesa:**
  - at `f7c93a1352`, `has_gs_rta_support` is commented out in
    `common/device_info/bxe-2-32.h`;
  - `eccb5c88d` "pvr: re-enable gs_rta_support" (2026-11-10) turns it back on;
  - from that commit on, the winsys turns the feature off again when the firmware is older than
    1.1;
  - the earlier commits `a3a4e2e3b`, `f8cb0ca03`, `be6650987` and `6a552e0e5` fix RTA clears.
- So the board's Mesa clears each layer as a separate render. The `rta` test exercises our
  per-layer path only with Mesa `eccb5c88d` or later.
- The same holds for the manufacturer-firmware results in `docsClean` `50055ae`: same Mesa, so
  the reference never ran its RTA path in `vkfwtest` either.
- **To exercise the path:** a newer public Mesa is needed. See "Newer Mesa" below.

## Full battery with firmware 1.1 (old Mesa)

`vkfwtest -k -L 300 -T 40 all`: **16 pass / 20 fail / 0 timeout / 0 lost / 3 skip**.

The failures are the same tests with the same pixel counts as E044. Firmware 1.1 regresses
nothing.

## `indirect` frame 1 and `vertex-ext`

- **What the `indirect` trace shows:** frames 0 and 1 have the same sequence (one TA kick, one
  3D kick, `HWRTData0State=5`). Each TA uses 21 local free-list pages. There is no fault and no
  HWR.
- **What fails:** frame 1 repeats frame 0 with new objects, and 13056 = 204 × 64 of its pixels
  are wrong (the drawn cells). Frame 0 passes.
- **Cache experiment:** a full per-DM flush plus an SLC flush-invalidate at every fragment kick
  (`EXP_DM_FLUSH_PER_KICK` + `EXP_SLC_FLUSH_PER_KICK`). `indirect`, `multiframe`, `vertex-ext`,
  `secondary` and `mrt` fail the same way, so the frame-1 failure is not a stale cache.
- **Open:**
  - why the manufacturer firmware passes `indirect` frame 1 but fails `multiframe` frame 1;
  - whether the 307.2 MHz clock matters.

## Newer Mesa (`5d2dc45`, cross-compiled on the PC)

The board now runs Arch Linux RISC-V (kernel `7.4.0-rc1-img`, glibc 2.44; the Debian system
and `/opt/mesa-pvr` are on the SD card and were not used).
- **Build:**
  - riscv64 cross build of the Vulkan `imagination` driver, `-Dmesa-clc=system`,
    `-Dprecomp-compiler=system`;
  - `mesa_clc`, `pco_clc` and `pdsc_precomp` built natively on the PC first;
  - `libdrm`, `expat`, `zlib` and the Vulkan loader taken from the board (`pacman`);
  - installed in `~/mesa-rta` on the board. `vkfwtest` was cross-built the same way.
- **The kernel hides the firmware version.** Mesa asks the firmware version with
  `DRM_PVR_DEV_QUERY_FW_INFO_GET`, but only if the DRM API version is at least 1.1.0. Our
  kernel reports `powervr 1.0.0` and has no such query, so Mesa assumes firmware 1.0 and
  prints `Missing FW support for GS RTA support, feature disabled`. The firmware's 1.1 is
  never seen. Fix: the kernel needs the 1.1.0 UAPI and the query (not done).
- **Test patch (Mesa copy in the scratchpad only):** default firmware version 1.1 when the
  query does not exist. With it the layer loop runs: `RTA render target 0`, then
  `Kick RTA render 2 of 6` … `6 of 6`.

### Result: every layer lands in layer 0 (Q33)

`rta` fails: layer 0 holds the layer-5 clear colour (`0x417049`).
- For an image that is not 3D, Mesa does not use the PBE `zslice` field. The end-of-tile
  program adds `layer_id × layer_size` to the PBE address, with `layer_id` read from the USC
  special register `RENDER_TGT_ID`.
- The spec only adds `layer` to PBE word 1 for `memlayout == TWIDDLE_3D`. The trace shows
  `word0_hi = 0x0200007D`, so no MRT of the test qualifies.
- Nothing in the spec gives `RENDER_TGT_ID` a value, so it stays 0 and the 6 renders
  overwrite layer 0. Opened as **Q33**. We do not guess a register.

### Full battery with the new Mesa

| Mesa | pass | fail | notes |
|---|---|---|---|
| `f7c93a1352`, firmware 1.1 | 16 | 20 | same as E044 |
| `5d2dc45`, RTA on (test patch) | not totalled | not totalled | run test by test, the list was cut after `prio-cdm`. Compared with the old Mesa, `msaa` and `rta` fail; `load-store-none` runs and fails; `queries` aborts in Mesa (core dump); `prio-3d`/`prio-ta` fail calibration |
| `5d2dc45`, RTA off | not totalled | not totalled | only 7 tests run: `msaa` and `rta` pass; `load-store-none`, `multiview`, `mrt`, `indirect`, `vertex-ext` fail as before |

- With RTA on, `msaa` fails although it renders a single layer: the regression comes from
  Mesa's RTA paths (clears, passthrough vertex shader), not from the firmware's layer loop.
- The frame-1 failures (`multiframe`, `indirect`, `mrt`, `multiview`, `secondary`) do not
  depend on the Mesa version.
- `queries` still needs Q32. With the new Mesa it aborts before reading anything.

## Answers in `docsClean` `dd37220` (Q31, Q32, Q33), implemented

The submodule pointer is still at `50055ae`; the answers were read from the `dd37220` object.

### What changed in the firmware

- **Q31, `src/geom.c`.**
  - Geometry first kick with an RTC: `active_render_targets = 0`, and the per-layer
    partial-render counters are zeroed after a prior partial render. Without an RTC:
    `active = 0` on a first kick and `1` on a later kick, `render_target_index = 0`,
    `current_render_target = 0`.
  - TA finished: `active_render_targets = CR[0x0D20]` (after the RTC flush). At the last kick
    the layers are listed in `valid_render_targets[]` and `current_render_target` is set to the
    first one.
  - Our own choice (Q31 "fill every layer") is gone.
- **Q33, `src/frag.c`.**
  - `BCAST[0x03D8] = current_render_target` on every 3D kick and every layer. This is the
    `RENDER_TGT_ID` of the end-of-tile program.
  - `CR[0x03E0]` is 0 on a non-last layer and 1 on the last layer (or a one-layer render).
  - The PBE `zslice` add is kept for `TWIDDLE_3D` targets only.
  - After the last layer only `render_target_index` returns to 0.
- **Q32, `src/frag.c`.** When `flags & ROGUE_FRAG_FLAGS_GET_VIS_RESULTS` (bit 5), the kick
  writes `isp_oclqry_base` (`frag_regs + 112`) to `CR[0x0FC0/0x0FC4]`.
- Build: no warnings. `fw-preflight` PASS. `check-trace-args` clean.

### Two places where we differ from the spec (Q35)

1. **`CR[0x0D20]` after the last kick.** In `rta` (two geometry kicks) it reads 6 after the
   first kick and **0** after the last, although 6 was written before the last kick. Using the
   value read at the last kick, no layer loop ran and layer 1 stayed empty. The firmware now
   keeps the larger of the stored count and the value read.
2. **No RTC bitmap scan.** The spec lists the layers from the bitmap at `rtc_dev_addr + 0x200`.
   Our firmware cannot map client memory yet. When the count equals `max_rts` the layers can
   only be `0..max_rts-1`, so we list those. Otherwise we list every layer and trace
   `0xE0450002`. The same applies to zeroing the RTC on OOM.
3. **Not implemented:** the layer loop for `FRAG_PR` and the per-layer partial-render counters
   (`rta_num_partial_renders[]`, per-layer `has_pr`). A partial render stays on one layer.

### Result on the board (Arch, firmware `8ce49335…`, Mesa `5d2dc45`)

- **`rta` passes with RTA on.** The trace shows `RTC with RTA present, 6 active render
  targets`, `RTA render target 0`, and `Kick RTA render 2 of 6` … `6 of 6`, in both frames.
  All 6 colour layers and all 6 D32 layers are checked pixel by pixel, so the per-layer ZLS
  bases work too. There is no fault and no HWR.
- **`queries` passes** with both Mesa builds: occlusion counts are exact (960 / 0 / 0, on the
  host and in the GPU copy).
  - Before that, it was a vkfwtest bug: the test wrote timestamps although the queue family
    reports `timestampValidBits = 0`. The old Mesa ignored this; `5d2dc45` traps
    (`UNREACHABLE` in `CmdWriteTimestamp2`).
  - The test now uses timestamps only when the queue has valid bits.
- **`msaa` still fails with RTA on** (every resolved pixel 0), and passes with RTA off.
  - With `gs_rta_support` Mesa resolves in the PBE (`downscale`) instead of a separate
    transfer.
  - That render has one layer and no RTA loop. Opened as **Q34**.
- **Rest of the battery** (RTA on, run test by test):
  - pass: `rta`, `queries`, `pre-cdm`, `resources`, `compute-dims`, `subgroups`, `xfer`,
    `buffers`, `ds-copy`, `blit-mips`, `image-copy`, `texture2`, `sync`, `sync2`, `memory`,
    `robust`, `prio-cdm`;
  - fail: the same frame-1 / second-render failures as E044 (`multiframe`, `pre-ta`, `bigpb`,
    `input-attach`, `zs-load`, `mrt`, `sizes`, `vertex`, `indirect`, `formats`, `secondary`,
    `multiview`, `raster`, `vertex-ext`, `texture`, `idle-resume`, `stress`, `soak`, `churn`,
    `prio-3d`, `prio-ta`), plus `msaa` and `load-store-none`.
  - `queries` was run again after the vkfwtest fix; the other tests were not.

## Answers in `docsClean` `b50fabe` (Q34, Q35), implemented

### Q34: nothing to do in the firmware

The firmware passes the PBE words through. The manufacturer firmware v1.1 with the RTA-enabled
Mesa fails `msaa` the same way. The failure is on the Mesa side.

### Q35: the RTC bitmap is now read

- **`src/mmu.c`, access to client GPU memory.**
  - Walk the render context's GPU MMU (`fwmemcontext.pc_dev_paddr`; PC → PD → PT; 4 KiB to
    2 MiB pages; public entry formats from `pvr_rogue_mmu_defs.h`) to a bus address.
  - Map that page through a scratch entry of the MIPS page table, `0xC0FC0000`, uncached.
    The page lies in the host's reserved 1 MiB `[0xC0ED0000, 0xC0FD0000)`, past the firmware
    data object, where the kernel allocates nothing (`pvr_fw.c`, allocations are limited to
    `fw_heap_info.size`). The entry must be empty before use.
  - After each access: write barrier, clear the entry, invalidate the dynamic TLB.
  - Helpers: `gpu_read()` and `gpu_zero()`.
- **`src/geom.c`.**
  - `CR[0x0D20]` sets `active_render_targets` only at TA finished of a **non-last** kick.
  - At the **last kick, before the TA starts**: SLC flush, then read the 256-byte bitmap at
    `rtc_dev_addr + 0x200`, and write the first `active` set bits to
    `valid_render_targets[]`.
  - Diagnostic traces: `0xE0450002` (bitmap not readable: every layer), `0xE0450004` (fewer
    bits than counted), `0xE0450005` (more bits than counted).
  - Our earlier workaround (keep the larger count) is gone.
- **Not done:** zeroing `rtc_dev_addr + 0x000..0x1FF` on OOM, and the layer loop for `FRAG_PR`.
  Both sit on the partial-render path, which already fails (Q30).

### vkfwtest `rta`, extended

New steps, all checked pixel by pixel:
- **Step 3, some layers only:** load op `LOAD`, then a clear of layer 1 (whole area) and
  layer 5 (an inner rect); the other layers must keep their contents.
- **Step 4, per-layer stencil:** D24S8 or D32S8, a clear of each layer to its own value.
- **Step 5, a 3D image viewed as a 6-layer 2D array** (Vulkan 1.1): `TWIDDLE_3D` and the PBE
  `zslice` add.
- Steps 3 to 5 all run, and the first failure is reported. A colour check now reports every
  wrong layer.

On the PC (GTX 1060) it passes, with gl_Layer, D24S8 and the 3D image.

### Result on the board (firmware `f45474ed…`)

| Step | Mesa, RTA on | Mesa, RTA off |
|---|---|---|
| 1 colour + D32, 6 layers, load op CLEAR | pass (bitmap `0x3F`, 6 layers) | pass |
| 3 `LOAD`, clears of layers 1 and 5 | **fail**: layers 1 and 5 keep their old contents | pass |
| 4 stencil D24S8, 6 layers | pass | pass |
| 5 3D image as 2D array | pass | pass |
| `queries` | pass | pass |

- **Step 3:** the bitmap is `0x23`. The firmware lists `{0, 1, 5}` and renders exactly those 3
  layers, so the layer selection works. The clear primitives of layers 1 and 5 do not reach
  the image. Opened as **Q36**: we need the manufacturer firmware's result on the same test.
- **Steps 4 and 5:** the per-layer stencil base (SLS) and the `TWIDDLE_3D` `zslice` path work.

## Answer in `docsClean` `88e48e7` (Q36): the step-3 failure is in Mesa

- **Observed with the manufacturer firmware v1.1** and the same RTA-enabled Mesa: `vkfwtest
  rta` step 3 fails the same way (layers 1 and 5 keep their old contents, no fault). The lost
  clears are on the Mesa `pvr` side.
- **Background-object registers:** the firmware does nothing per layer to `pds_bgnd0..3`
  (`0x06A0..0x06BC`), `isp_bgobjvals` (`0x0F88`) or `0x0F10`. They are written as the command
  gives them. Our layer re-kick leaves them as the first kick wrote them, which is the same.
- **Bit 0 of the RTC bitmap:** the firmware never writes the bitmap. The bit comes from the
  hardware or the VDM stream (the initial render target is 0).

### State of render-target arrays on the firmware side

With the RTA-enabled Mesa, our firmware gives the same result as the manufacturer firmware on
every step of `vkfwtest rta`:
- pass: 6 layers colour + D32, per-layer stencil, 3D image;
- fail: step 3, a Mesa bug.

`msaa` with RTA on fails on both firmwares too (Q34).

Still open on the firmware side, all on the partial-render path, which already fails for one
layer (Q30):
- the layer loop for `FRAG_PR`;
- the per-layer partial-render counters (`rta_num_partial_renders[]`, per-layer `has_pr`);
- zeroing `rtc_dev_addr + 0x000..0x1FF` on OOM.

Outside the firmware: the kernel must report UAPI 1.1.0 and answer
`DRM_PVR_DEV_QUERY_FW_INFO_GET`. Without that, Mesa never turns RTA on.
