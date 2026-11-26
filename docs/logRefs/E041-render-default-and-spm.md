<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E041 — rendering by default, the PM module, out-of-memory handling, tools; first runs on the 16 GB board

**Question.** `docs/steps.md` phases 1–10 against `docsClean` `2d4b018`. How much can be
implemented from the spec alone, and what does the board say?

**Board.** From this experiment on: **Banana Pi BPI-F3 16 GB** at `192.168.0.110`.
- Mainline SD card, kernel `7.4.0-rc1-img`, GRUB entry 3.
- Serial console on `/dev/ttyUSB1`.
- The vendor system on its eMMC is never used.
- GRUB's default entry is the vendor eMMC, so `tools/boot-entry.py` must select entry 3.
- `tools/deploy.sh` now defaults to this address.

## What changed

**`src/pm.c` + `src/include/pm.h` (new)** — the Parameter Manager in one place:
- **Free-list load/store per PM context and per list:** one table of value registers,
  live registers and load commands (E035, Q16b–Q16k).
- **List loads:** catalogues, MLIST base and the ALIST/MLIST SPs with the `0x0288`/`0x0190`
  commands (E037); context selection through `0x03D0`/`0x03D4`.
- **PM slot tracking:** which HWRTData and free lists each PM context holds. It replaces
  `pm_ctx1_fl[]` and is invalidated on HWR, after a partial render and at power-off.
- **SPM** (`partial-render-spm.md`, the Q16m register changes):
  - **ready pages:** the PM pause (`0x02A0`/`0x02A8`), base − 4·N, live SP + N, then the
    load command and unpause. Then `FREELIST_GROW` to the host in the background;
  - **grow:** FWCCB `FREELIST_GROW` (103) and KCCB `FREELIST_GROW_UPDATE` (110). An
    update for a geometry that waits is applied at once; otherwise the pages are held for
    the next out-of-memory event (see below);
  - **partial render:**
    - the firmware signals the geometry's `partial_render_geom_frag_fence`, so the
      fragment context's `FENCE_PR` passes;
    - `FRAG_PR` then runs as a partial render: PM context 1 takes the geometry's *current*
      catalogues and list SPs, `PM_PARTIAL_RENDER_ENABLE = 1`, `pds_pr_bgnd`, no ZS;
    - at its end: `0x0338 = 0`, context 0 reloaded with zero lists, context-1 slot
      invalidated;
    - a `FRAG_PR` that was not armed is consumed without running, as before;
  - **HWR:** HWRTData state `HWR`, `geom_caches_need_zeroing = 1`, and FWCCB
    `FREELISTS_RECONSTRUCTION` (104) with the free-list IDs.

**`src/geom.c`, `src/frag.c` rewritten.** The working sequence is now the code, and all
bring-up knobs are gone (list in `docs/firmware/build-and-toolchain.rst`). New:
- `CR[0x40A8] = 0` (TE core selection, phase D);
- `owner_geom` is set at every kick;
- `geom_caches_need_zeroing` is honoured: SLC flush and the flag cleared, since the spec
  does not name the clear registers;
- **non-first kicks** reload the PM only if context 0 holds another HWRTData;
- `HWRTData` offsets come from the public layout in `pm.h`;
- the fragment kick has a `partial` mode;
- frag completion traces `3D finished`.

**`src/kick.c`.**
- Geometry, fragment and `FRAG_PR` run **by default**. `EXP_NO_RENDER` brings back the
  old "retire unrun" behaviour.
- `PM_OUT_OF_MEMORY` is routed to the SPM while the geometry runs, also from the polling
  backstop.

**Other code.**
- `src/hwr.c`: the PM step above; a debug trace of the PM state at each recovery.
- `src/power.c`: `POW_OFF` forgets the BIF slots and the PM state (the registers are
  lost).
- `src/main.c`: `kick_init()` is now called at boot. It was defined but never called;
  the slot table only worked because `.bss` starts at 0.

**Tools.**
- **`tools/fw-preflight.py`:** checks the `.fw` container (oracle section 2); run by
  `deploy.sh fw`. The current build passes every check (code `0x66CC` of `0x10000`).
- **`tools/oracle.sh`:** stages A–D of the oracle with its hard-failure patterns.
- **`tools/check-trace-args.py`:** run before every link; it finds no mismatch in the
  current sources and does catch a wrong count in a test.

## Results on the board (build `e4ef5484…` and before)

| Check | Result |
|---|---|
| Oracle stage A (cold boot) | **pass**: `FW version v1.0`, initialised, no hard failure |
| `api.smoke.*` | **8/8** |
| `draw.renderpass.simple_draw.*` | **4/4** |
| `compute.pipeline.basic.*` | **73/82** (9 not supported), 0 fail |
| `draw.renderpass.basic_draw.*` | **203/331**, 0 fail (128 not supported), no fault |
| `renderpass1…d32_sfloat.input.clear.dont_care.draw` | still fails: IPF read at `0x2000` (Q19b) |
| `vkfwtest bigpb` | first OOM handled exactly as in the reference (below); the TA never resumes → lockup and HWR each frame (Q28) |
| Oracle stages C, D, B in one run | **the board hung** (no network, no serial) during stage C; needs a power cycle |

**Out-of-memory trace** (first frame of `bigpb`):

```
OOM TA_cmd=0xc0079000, U-FL 0xc003e000, N-FL 0xc00b9000
PM pause TA ALLOC: PM_PAGE_MANAGEOP set to 0x1
PM unpause TA ALLOC: PM_PAGE_MANAGEOP set to 0x0
Freelist update completed [0xc003e000]: old total pages 0x1c0, new total pages 0x200, new DevVirtAddr 0x8000048800
OOM: Resumed TA with ready pages, FL addr: 0x8000048800, current pages: 512, SP : 71
Received grow update, FL addr: 0xc003e000, new pages: 768, ready pages: 96
… nothing until "Lockup detected on DM2" 36 s later
```

The update itself matches the reference exactly: the same totals, base `…48800`, and the
live SP 7 → `0x47`. But at the lockup the live global SP is still `0x47` and the local one
2. The PM took no more pages and raised no new `PM_OUT_OF_MEMORY`, so the TA did not
resume.

| Variant | Result |
|---|---|
| grow update applied to the running PM (first build) | same stall; holding the update until the next OOM is closer to the reference and kept |
| clear `PM_OUT_OF_MEMORY` again after the update (`EXP_RESUME=1`, default) | same stall |
| also re-issue the TA run register `0x0400` (`EXP_RESUME=2`) | same stall |

The missing step changes no sampled register, as the load commands did. This is Q28.

**Board hang.** The oracle (`C D B`) hung the SoC during stage C, whose suites are
`api.smoke`, `simple_draw`, `compute.pipeline.basic`,
`copy_and_blit.image_to_image.simple_tests`, `synchronization.basic` and `api.buffer`. The
first three had just passed on their own. The summary was lost: `/tmp` is in RAM. The
next run must:
- read `journalctl -b -1` if the journal is persistent;
- run the stages one at a time, writing to disk.

The oracle also warns that marginal DRAM on 16 GB boards causes bit flips that look like
firmware bugs. This board's DDR setting has not been checked.

## Oracle stage by stage (after the power cycle, build `e4ef5484…`)

The persistent journal of the hung boot ends ~15 minutes before the hang: the last
entries never reached the disk. It shows two kernel WARNINGs at `modprobe -r powervr`,
right after a faulted input-attachment render:
- `drm_mm_takedown` in `pvr_fw_fini`;
- `pvr_context_device_fini`.

The host still held allocations and a context. So a faulted render leaves host state
behind.

| Stage | Result | Reference |
|---|---|---|
| C `copy_and_blit…image_to_image.simple_tests` | 19/23: the four `partial_image_npot_*` fail, no fault | 23/23 |
| C `synchronization.basic` | **24/29**, 5 not supported | 24/29 |
| C `api.buffer` | **1030/3076**, 2046 not supported, no fault | 1030/3076 |
| D runtime-PM suspend → render after warm boot | **pass** | pass |
| B `xfer` | **pass** | pass |
| B `pre-cdm` | **pass** (no preemption: probes wait for the long job) | pass |
| B `pre-3d` | **fail** (rc ≠ 0), then **the board hung** during `pre-3d`/`pre-ta` | pass |

The NPOT copies were blamed on the host in the Q25 answer, but the reference passes them
on the same stack, so they are ours (Q25 reopened).

**Likely cause of the hangs.** `pre-3d` and `pre-ta` render from two Vulkan contexts at
once. When geometry B loaded PM context 0, the firmware did not store geometry A's live
free-list state first. Fragment A then found context 0 holding B, skipped the store, and
loaded PM context 1 from A's stale free-list struct. The PM can then hand out pages still
in use and write page-catalogue entries, which are physical addresses used without the
MMU, into memory it does not own. That can corrupt RAM and hang the SoC, which fits a
hang with no output on the serial line.

**Fix** (build `96f558c3…`): `pm_slot_evict()` stores a PM context's free
lists before another HWRTData is loaded into it. The fragment stores PM context 0's state
only when context 0 still holds the same HWRTData.

## After the second power cycle (builds `96f558c3…`, `9646126b…`, `f6d2f959…`)

**No more board hangs.** `pre-3d` ran three times, about 10 minutes in all, with a lockup
every 30 s. The SoC never stopped responding. The interleaved-context fix holds.

**Eviction bug, fixed (`9646126b…`).** The first eviction build stored PM context 0's
registers at the next geometry, even after the fragment had taken the free lists over and
written back the freed pages. That overwrote the free lists with stale "pages in use"
state. The slots now have a `dirty` flag:
- set when a context is loaded;
- cleared when its state is stored (fragment load for context 0, end of the fragment for
  context 1);
- eviction and `fl_add_pages` only act on a dirty context.

**`Load Freelist` / `Store Freelist` traces (`f6d2f959…`).** The reference's own trace lines
(RTD 6 / RTD 13) are now emitted at every free-list load and store.

**`pre-3d`: the second render's TA stalls.** `pre-3d` calibrates with renders on one device.
Mesa alternates the two HWRTData of the render-target dataset (`0xc0137000`,
`0xc0138000`).

| Render | Free lists loaded into context 0 | Result |
|---|---|---|
| 1, HWRTData 0 | local `SP 0x1f`, global `0x1bf`, allocated 0 | TA finished; local `0x1f`→`0x08`, global `0x1bf`→`0x1b9`; the 3D frees everything back to `0x1f`/`0x1bf` |
| 2, HWRTData 1 | **the same values** | **TA stalls**: local live SP `0x0a` (21 pages taken), global unchanged, `0x0D00 = 0x0F`, no `PM_OUT_OF_MEMORY`; lockup after 30 s |

So the free-list state is not the cause.

After that first lockup, **every** geometry kick stalls:
- HWRTData 0 again;
- the second device's first render (a fresh render-target dataset);
- also with `autosuspend_delay_ms = 0`, where the trace shows the GPU powered off and on
  between render 1 and render 2.

Something that render 1 leaves in memory, or the first recovery leaves behind, breaks
every later TA. Candidates, all named by the spec but without registers:
- the TPC flush at geometry completion (Q19b). The two HWRTData share the TPC (`0x0C40`);
- the RTC / TA cache zeroing (Q19b item 3);
- `Zero RTC for FWCtx` (trace HWR 69) in recovery.

The public `pvr_rogue_cr_defs.h` and Mesa's `rogue_cr.xml` name only `TE_TPC_ADDR`, no flush
register. This is the same family as the input-attachment fault (the second HWRTData of one
render pass). Asked as **Q29**.

**Regression** (`f6d2f959…`): stage A passes. `api.smoke` 8/8, `simple_draw` 4/4,
`basic_draw` 203/331 with 0 failures and no fault.

`pre-ta` was not run: it renders more than once per device, so it hits the same stall.

**Not done** (needs the spec): the TPC flush and cache-zeroing registers (Q19b), the OOM
resume (Q28), RTA / firmware 1.1 (Q27), and 3D preemption (optional).

**Clean-room note.** Own firmware and measurements; spec sections named above; host side
from the public mainline `pvr_free_list.c`.
