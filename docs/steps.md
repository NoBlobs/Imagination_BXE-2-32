<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Steps to finish the firmware

The roadmap from the current state to a firmware that passes the acceptance oracle of
[`docsClean/board-testing-oracle.md`](../docsClean/board-testing-oracle.md). Phases are in
dependency order. Each step gives:
- **Do:** the work to implement;
- **Spec:** the `docsClean/` section it rests on;
- **Code:** the files involved;
- **Done when:** the acceptance check;
- **Ask:** the question for the dirty team, when the spec has a gap.

Written against `docsClean` `2d4b018` ("General improvements"). Clean-room rules are
unchanged: sources are `docsClean/` and our own experiments only.

## Next, by priority (E044, `docsClean` `50055ae`)

`docsClean/vkfwtest-results.md` gives the manufacturer open firmware's results on the same
mainline + Mesa `pvr` stack: 18 pass / 18 fail / 3 skip. Ours: 16 / 20 / 3. The two agree on
36 of 39 tests (E044, "Comparison with the manufacturer firmware"). The work is ordered by
that comparison:

1. **`queries`: done (E045).** `CR[0x0FC0]` from Q32 (`dd37220`), plus a vkfwtest fix:
   no timestamps without `timestampValidBits`. The test passes with exact counts.
2. **`indirect` frame 1 and `vertex-ext` `divisor 4`**: renders after the first on a device
   that pass with the reference and fail with ours.
   - Do: first check the GPU clock in our DTB (the reference uses 819.2 MHz; 1 GHz gives
     intermittent errors). Then compare the second render of these two tests with the
     `multiframe` frame 1 (which fails on both) to find what our firmware does differently.
   - Done when: both pass. Then the 18-test baseline of `docsClean` section 4 holds.
3. **Keep the other 18 failures clean**: they fail the same way with the reference
   (`multiframe` frame 1, `pre-ta`/`bigpb`/`texture` frame 0, `raster` 1152/4096, …), so
   they are not firmware work. They must complete with no timeout, device lost or kernel
   fault, which already holds. Lower priority than before: the second-HWRTData, post-OOM and
   after-transfer leads of E044.
4. **Stage E without `VK_ERROR_DEVICE_LOST`**: the reference and the vendor stack do not
   return it either, so phase 5 needs only a clean recovery (which holds) until Mesa changes.
5. **Render-target arrays (E045): done on the firmware side for normal renders.** Same result as
   the manufacturer firmware on every `vkfwtest rta` step. The `LOAD` subset step and `msaa`
   with RTA on fail on both firmwares, in Mesa (Q34, Q36). Open:
   - the kernel must report UAPI 1.1.0 and answer `DRM_PVR_DEV_QUERY_FW_INFO_GET`, or Mesa never
     turns RTA on;
   - the layer loop for `FRAG_PR`, the per-layer partial-render counters and the RTC zeroing on
     OOM, after Q30.

The phases below keep their content; their acceptance checks are read against this baseline.

## Progress (E045, 2026-11-27)

| Phase | State |
|---|---|
| 1 | Done (E041–E043): PM slots, TA-finished TPC/RTC flush, full geometry flush, RTC programming, PM-MMU invalidation, first-kick PM reset pulses. **The second HWRTData works** (`pre-3d`, `pre-ta` pass). CPU zeroing of TPC/RTC after HWR not done (SLC flush instead). |
| 2 | Stages A and C pass. Stage B runs the pixel-checking `vkfwtest` (E044): `pre-cdm`, `xfer` pass; `multiframe`, `pre-ta`, `bigpb` fail on wrong pixels, **the same way as with the manufacturer firmware** (`docsClean` `50055ae`). Full battery 16/20/3 vs the reference's 18/18/3: only `queries`, `indirect`, `vertex-ext` differ. Stage D (`vkfwtest xfer` after suspend) passes after stage A; it fails after compute/sync suites (E043). |
| 3 | Non-first kicks, `owner_geom`, HWRTData states done; no workload exercising them yet. |
| 4 | Ready pages, grow, TA stop/resume, `FRAG_PR` via the SPM failing-UFO check: **the first partial render of a frame runs and the TA restarts**; the second restart faults (Q30). ZS backing not done. |
| 5 | Full HWR teardown done (E043). `vkfwtest hwr-lockup` (E044): recovery after 30 s and compute works afterwards, but **the guilty fence signals normally: no `VK_ERROR_DEVICE_LOST`**. The manufacturer firmware on Mesa and the vendor stack behave the same, so this is not a firmware gap. |
| 6 | BIF/PM state reset at `POW_OFF`, `kick_init()` at boot; barriers already in place. Stage D passes. |
| 7 | Firmware reports 1.1. RTA implemented (E045): RTC count and bitmap scan (client memory read through a GPU-MMU walk), `valid_render_targets[]`, per-layer region headers, `RENDER_TGT_ID` (`0x03D8`), ZLS/SLS, `zslice`, `CR[0x03E0]`, layer loop at `PIXELBE_END_RENDER`. With RTA on, every `vkfwtest rta` step gives the manufacturer firmware's result (the `LOAD` subset step fails on both, Mesa side, Q36). Open: kernel FW_INFO query, `FRAG_PR` loop (after Q30). |
| 8 | Not started (optional). |
| 9 | Trace argument check done; others open. |
| 10 | `fw-preflight.py`, `oracle.sh` (stage B/D on `vkfwtest`, `new_trace`), `check-trace-args.py`, **`vkfwtest`** (own repository, `tools/deploy.sh vkfwtest`) done. |

---

## 0. Where we are

**Works (default build):**
- boot, wrapper remap and TLB;
- GPU units init;
- KCCB/FWCCB, client CCBs and UFO fences;
- MTS background and IRQ tasks;
- runtime PM with warm boot;
- compute (`compute.pipeline.basic` 73/73);
- `TQ_3D` transfers;
- the per-DM completion flush;
- HWR (MMU fault and watchdog);
- statistics, the HWR log and reset notifications.

**Works (experimental, `make EXP=-DEXP_TA`):**
- geometry and fragment kicks, with the free-list and list-SP load commands
  (E035, E037);
- depth load and store;
- `PM_3D_MEM_FREE` seen after every render;
- `simple_draw` 4/4, `api.smoke` 8/8, `basic_draw` 203 pass / 0 fail.

**Open:**
- renders with two HWRTData in one render pass (input attachments) fault on an IPF read
  at `0x2000` (E039, E040);
- the shared-PB rule hung the board (E040) and is now off by default;
- no OOM, grow or partial-render handling;
- `FRAG_PR` is consumed without running;
- firmware version 1.0.

---

## 1. Renders with several HWRTData (input attachments)

**Do:**
1. **Full geometry completion flush, including the TPC.**
   - The spec says geometry completion "flushes the TPC". Our geometry variant only does
     `0x4000`, `0x0688 = 1`, `0x0668 = 5`, SLC `DM_TA`.
   - The second HWRTData of an input-attachment pass **shares the TPC** with the first. A
     stale TPC is the main suspect for the IPF fault.
2. **`CR[0x40A8] = (primary core) & 1`** in the geometry kick (phase D). We never write it.
3. **`geom_caches_need_zeroing`** (HWRTData `+0x114`, common `+0x00`): clear the RTC and
   TA caches before a geometry kick when it is set (after HWR).
4. **PM slot tracking.**
   - Replace `pm_ctx1_fl[]` with a per-slot record: which HWRTData and free lists are
     loaded in PM context 0 and PM context 1.
   - Invalidate the context-1 slot after every partial render and every HWR (spec rule).
   - Re-enable the shared-PB rule (`CONTEXT_PB_BASE = 0`, no reload) only on top of this
     tracking, and only after reproducing and explaining the E040 hang. Run it on a single
     test first.

**Spec:**
- `kick-geometry-ta.md`: phase D, "Completion";
- `kick-compute-and-3d.md`: "Input-attachment renders";
- `input-attachment-dump.md`;
- `partial-render-spm.md`: "Fidelity notes" (invalidate the 3D PM slot);
- `data-structures.md`: the HWRTData layout.

**Code:** `src/kick.c` (`dm_cache_flush`), `src/geom.c`, `src/frag.c`.

**Done when:**
- `dEQP-VK.renderpasses.renderpass1.suballocation.formats.*` passes with no fault and no
  HWR, each format group run on a fresh module load;
- `basic_draw` stays 203/0.

**Ask, Q19b:**
- Which register and value flush the TPC at geometry completion?
- What is the full geometry variant of the completion flush, in the same form as the
  fragment and compute table?

---

## 2. Rendering in the default build

**Do:**
1. Drop the `EXP_TA` gate on `CCB_CMD_GEOM` and `CCB_CMD_FRAG` in `kick.c`.
2. Keep only the working sequence as code; the knobs that are no longer needed go away:
   - `EXP_HS_MODE`, the canaries, `EXP_PM_PAUSE`, `EXP_PM_BCAST`, `EXP_CLK_ON`,
     `EXP_SP32`, `EXP_20F8_EDGE`, `EXP_TOP_FIRST`, `EXP_IDX_AFTER`, `EXP_MTILE_STRIDE`;
   - the dump and trace helpers stay behind one `EXP_DEBUG`.
3. Keep `FRAG_PR` consumed without running (correct until phase 4, per the Q16m answer).
4. Document the final geometry and fragment sequences in
   `docs/firmware/scheduler-and-kicks.rst` and `docs/hardware/kicks-geometry-ta.rst`.

**Code:** `src/kick.c`, `src/geom.c`, `src/frag.c`, `docs/firmware/*.rst`,
`docs/firmware/build-and-toolchain.rst` (knob table).

**Done when:** the oracle's stage C passes on the default build:
- `api.smoke` 8/8;
- `simple_draw` 4/4;
- `compute.pipeline.basic` 73/82;
- `copy_and_blit image_to_image.simple_tests` 23/23;
- `synchronization.basic` 24/29;
- `api.buffer` 1030/3076;
- 0 kernel faults.

---

## 3. Multi-kick geometry and render state

**Do:**
1. `First kick:0` and `Last kick`: no PM reload on a later kick (done in E039; verify with
   a workload that sends it). Defer the fragment until the last kick.
2. HWRTData states:
   - `KICK_GEOM`, `KICK_GEOM_FIRST`, `GEOM_FINISHED`, `KICK_FRAG`, `FRAG_FINISHED`;
   - `owner_geom_not_used_by_host` (`+0x110`);
   - `cleanup_state` (`+0x140`) for the cleanup command.
3. Geometry → fragment interlock: `FENCE_PR` on the geometry timeline UFO, the
   `COMBINED_GEOM_FRAG_KICK` path, and the HWRTData-state hold (Q22, `TO CONFIRM`).

**Spec:** `kick-geometry-ta.md`, `client-ccb.md`, `command-structs.md`,
`data-structures.md`.

**Code:** `src/geom.c`, `src/frag.c`, `src/sched.c`.

**Done when:** a geometry-heavy renderpass (oracle stage B, row 2) passes with
`Kick TA >= 1`, `Kick 3D >= 1`, `Discarded cmd == 0` and no fault.

---

## 4. Parameter-buffer exhaustion (SPM)

**Do:**
1. **`PM_OUT_OF_MEMORY`** (event bit 7, already routed for DM 2): implement the SPM state
   machine (`none`, `wait for grow`, `PR blocked`, `wait for HW`, `PR running`,
   `PR executed`).
2. **Ready pages, no host round trip.**
   - Pause the PM: `0x02A0 = 1`, poll `0x02A8` bit 0 = 1.
   - Move the free list base down 4 bytes per added page, then write the new stack top to
     bank `+0x08` and issue the load command.
   - Unpause (`0x02A0 = 0`) and resume the TA.
3. **Grow.** FWCCB `FREELIST_GROW` (103) to the host; KCCB `FREELIST_GROW_UPDATE` (110)
   back, which is a no-op in `kccb.c` today. Hold the context until the update arrives,
   then apply it as in item 2.
4. **Partial render.**
   - Arm it with a non-mutating evaluation of `FENCE_PR` (the live `dep_offset` must not
     change).
   - Run `FRAG_PR` with `pds_pr_bgnd`: load PM context 1 from the geometry's current
     ALIST/MLIST SPs and catalogues, `PM_PARTIAL_RENDER_ENABLE` (`0x0338`) = 1, then 0 at
     the end.
   - Reset context 0's list SPs to 0 and restart the geometry with new catalogues
     (`INIT_PAGE` in the high word of `PM_CAT_BASE_VCE0`), without a new `Kick TA`.
   - Invalidate the context-1 slot (phase 1 item 4).
5. **ZS buffers on demand:** FWCCB `ZSBUFFER_BACKING`/`UNBACKING` (101/102) ↔ KCCB
   (108/109), and the `PRBUFFER` (`pr_buffer_fw_addr`).

**Spec:** `partial-render-spm.md` (all sections), `kick-geometry-ta.md` (Q16m answer:
multi-kick, OOM, partial render), `firmware-ccb.md`, `kernel-ccb.md`,
`observed-sequences.md` (OOM and grow).

**Code:** new `src/spm.c`, `src/geom.c`, `src/frag.c`, `src/kccb.c`, `src/sched.c`,
`src/include/fwif.h`.

**Done when:** the oracle's stage B "Parameter-Buffer OOM render" passes: `rc == 0`, a
pixel check, several partial renders, `Discarded cmd == 0`, no fault or HWR.

**Ask, if needed:** the exact register sequence of the context-0 restart after a partial
render (catalogue reset, `INIT_PAGE` source), in the form of the load commands.

---

## 5. HWR with rendering

**Do:**
1. Free-list reconstruction after HWR:
   - snapshot in `freelist_hwr_snapshot` (`+0xBC`);
   - FWCCB `FREELISTS_RECONSTRUCTION` (104) ↔ KCCB `FREELISTS_RECONSTRUCTION_UPDATE`
     (112).
2. Set the HWRTData state `HWR` and `geom_caches_need_zeroing`.
3. Invalidate the PM slots on HWR.
4. Return the right cleanup bits (`CMD_EXECUTED`, or `| CLEANUP_BUSY` while in use) for
   HWRTData, free lists and ZS buffers.
5. The PM 3D timeout sequence (`0x03E0`, `0x0388`, `0x0298`, `0x01C8`, event bit 8).

**Spec:** `hardware-recovery.md`, `partial-render-spm.md` ("Freelist reconstruction after
HWR"), `observed-sequences.md` ("PM 3D timeout").

**Code:** `src/hwr.c`, `src/sched.c`, `src/kccb.c`, `src/geom.c`.

**Done when:** a forced fault during a render recovers, the next render passes, and the
host sees a single context reset.

---

## 6. Power with rendering

**Do:**
1. Check that every MMIO access is `volatile` and that a write barrier comes before
   `MIPS_WRAPPER_IRQ_STATUS = 1` and before `power_sync = 1`.
2. Walk the BIF slot table in the `POW_OFF` teardown, in finite time and without touching
   active slots.
3. Keep the `pow flags` trace word as our own encoding (now explicitly free).

**Spec:** `power-management.md`, `data-masters.md` (BIF slot rules),
`execution-environment.md`.

**Code:** `src/power.c`, `src/kick.c` (BIF slots), `src/include/fw.h`.

**Done when:** the oracle's stage D passes with a renderpass as well as a transfer
workload (suspend, then warm boot from `suspended`), with no `error -22` or
`device lost`.

---

## 7. Firmware 1.1 and `gs_rta_support`

**Do:**
1. Layered rendering:
   - `rta_ctl` (HWRTData `+0xD0`);
   - `rgnhdr_single_rt_size` and `rgnhdr_scratch_offset` (fragment regs);
   - `zls_stride` and `sls_stride` (fragment command `+0x1D4`/`+0x1D8`);
   - per-layer region headers and PBE addressing.
2. Bump `fw_version_minor` to 1 in `tools/mkfw.py` **only after** RTA works. Mesa MR
   40973 enables `gs_rta_support` for firmware 1.1 or later, and the oracle's stage A
   expects `FW version v1.1`.

**Spec:** `command-structs.md`, `data-structures.md`; the RTA behaviour is not described
yet.

**Code:** `src/geom.c`, `src/frag.c`, `tools/mkfw.py`.

**Done when:** layered-rendering `dEQP-VK` groups (geometry shader `layered.*`, multiview)
pass with Mesa's `gs_rta_support` on.

**Ask, Q27:** what firmware 1.1 does differently for render-target arrays (geometry and
fragment kick, region headers per layer, `rta_ctl`), compared with 1.0.

---

## 8. Preemption (optional)

The mainline host leaves context switching off, so this comes last.

**Do:**
1. 3D store and resume:
   - store: `0x1000 = 1` while `(ISP_RENDER & 3) != 3`;
   - resume: `0x1020` per core from `context_state_addr`, `ISP_RENDER |= 0x10`, then
     `0x0298`/`0x01C8` from the saved state.
2. TA store, satisfied at the end of the TA.
3. Leave the CDM store out: it faults on the reference too.

**Spec:** `scheduler.md` ("Preemption", "Observed store and resume per DM"),
`kick-compute-and-3d.md` (phase F), `command-structs.md` (CSW structures).

**Code:** `src/sched.c`, `src/frag.c`, `src/geom.c`.

**Done when:** a two-priority workload with `fw_os_config_flags` `FRAG_EN` runs stores
and resumes without faults.

---

## 9. Remaining items

- **Q25:** the transfer cases that still fail (`buffer_to_buffer_with_offset`, NPOT,
  mirror). The answer says they are host-side; confirm against the current Mesa.
- **`0x0C68`:** the wait for the return to 0 sometimes times out (E038). Decide whether
  to keep, shorten or drop it.
- **Q26:** the remaining KCCB commands are consumed with `CMD_EXECUTED`, as answered;
  keep a test for each.
- **Trace hygiene:** pass every `TRACE` the argument count of its SFID. The `cr_poll32`
  bug of E037 came from this; a build-time check in `gen_sfids.py` would catch it.
- **Clock:** the oracle runs the GPU at 819.2 MHz. Check the board's clock tree, since
  1 GHz shows errors at stock voltage.

---

## 10. Validation and release

1. **`tools/fw-preflight.py`.** Check the container before any copy to the board:
   - size a multiple of 4096;
   - `info_version == 3`, BVNC, `OPEN_SOURCE` flag;
   - layout entries that don't overlap;
   - ELF `PT_LOAD` segments that fit their entries, with flags `0x72001001`.

   Run it from `tools/deploy.sh`.
2. **`tools/oracle.sh`.** Run stages A–D of the oracle with its hard-failure patterns:
   - `HW poll .* failed`, `BIF.*fault`, `MMU.*fault`, `FW fault`, `Lockup`;
   - `Discarded cmd`, `FWCCB.*112`, `device lost`, `error -22`.

   Save a summary in `docs/logRefs/data/`.
3. **Stability:** `tools/stab-run.sh` with rendering included, plus wider groups
   (`draw.*`, `renderpasses.*`). Never run `memory.allocation.basic.*` unguarded.
4. **Docs:** update the `.rst` pages (status, kicks, PM/SPM), the README and
   `questions.md`. Record every step as an experiment in `docs/logRefs/`.
5. **Release:** tag a version once stages A–D pass on the default build.

---

## Appendix: open questions for the dirty team

| Q | Phase | Question |
|---|---|---|
| Q19b | 1 | The TPC flush and the full geometry completion flush |
| Q16n | 1 | State outside the dumped ranges during the input-attachment render (still open; may be answered by Q19b) |
| Q27 | 7 | What firmware 1.1 changes for render-target arrays (`gs_rta_support`) |
| — | 4 | Context-0 restart after a partial render, if the spec is not enough |

The spec is now a public repository with issues. The clean team opens questions there but
only reads the published documents, not the issue threads: those can carry
non-clean material.
