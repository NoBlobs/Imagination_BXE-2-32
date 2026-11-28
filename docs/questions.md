<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
following the question flow in `docsClean/clean-room-process.md`. Until a question is
answered, the implementation uses the stated assumption.

## Questions, newest first

**State on 2026-11-27: no question is open.**
- Every question below has been answered in `docsClean` or resolved by an experiment; each
  heading says where.
- With the Mesa and kernel fixes of E048, the firmware passes the whole vkfwtest battery
  (48 pass / 2 skip) and gives the manufacturer firmware's results on every test (E049).
- **Still to implement (answered in the spec, no question pending):**
  - preemption, i.e. context store and resume (Q24, `steps.md` phase 8, optional);
  - on-demand `ZSBUFFER_BACKING` (Q23, Q30 item 4), which no test exercises.

Field names are those of the public `pvr_rogue_fwif*.h`. `CR[x]` is the primary-core bank and
`BCAST[x]` the broadcast bank, as in the spec.

### After 2026-11-09: the PM, partial renders, render-target arrays and the vkfwtest battery

#### Q39 — What gives the background object its render target index (layered loads and partial renders)? (answered in `1524931`: only `0x03D8`; `FRAG_PR` runs the layer loop; implemented in E049)

With render-target arrays:
- **The end-of-tile program gets the current layer.** It reads `RENDER_TGT_ID` from
  `BCAST[0x03D8]` (Q33).
- **The background object does not.** Mesa's load-op program for a layered attachment
  (`rta_load_clear`, `pvr_usc.c`) adds `RENDER_TGT_ID × layer_size` to the texture address, and
  on the board it always loads layer 0. Seen with our firmware and with the manufacturer's (Q37).
  We now write `pds_bgnd0..3`, `isp_bgobjvals`, `isp_bgobjdepth` and `0x0F10` again on every
  layer, as the Q36 answer says, with no change.
- **Consequences:**
  - a `LOAD` render over several layers loads layer 0 into every layer. Mesa now avoids that
    case by deferring the clears of the other layers to transfers;
  - a partial render of a layered render cannot work either: the final render of layer *k*
    must reload layer *k*'s partial result through the background object (Mesa's SPM), and it
    gets layer 0's. `vkfwtest bigpb-layers` page-faults at the partial render with both
    firmwares.
1. Is there a register, or a field of one the firmware already writes, that gives
   `RENDER_TGT_ID` to the PDS/USC background task (normal and partial-render background)? If
   so, which one, and when is it written on each layer (before the `0x0F00` launch)?
2. If the hardware never passes the layer to the background task on this core, does the
   manufacturer firmware avoid layered partial renders some other way (for example by
   rendering only layer 0 in a `FRAG_PR`, or by refusing the partial render)?

#### Q38 — `vkfwtest stress`: intermittent lockups with four contexts (both firmwares) (answered in `1524931`: cross-context PM free-list sync; implemented in E049, `stress` 8/8; the remaining ~4 % failures were a bug in our sync, fixed in E050: 400/400)

[E047](logRefs/E047-vkfwtest-investigation-tests.md). `stress` runs `multiframe`, `pre-ta`,
`pre-cdm` and `xfer` on four devices at once.
- **With our firmware** it fails about half the time: 3 of 6 runs, with Mesa RTA off and on.
- **The failing runs** show `Guilty overrunning` and `Guilty lockup` resets. The firmware
  trace shows `Lockup detected on DM3` and a BIF0 page fault (MMU status `0x9041`, request
  `0x0000ed0000001180`), and a later frame of one device reads wrong pixels.
- **Our rerun of the manufacturer firmware** failed `stress` too, with RTA off and on. The
  `9d74761` baselines record a passing run.
1. How often does `stress` fail on the manufacturer firmware (for example 10 runs of
   `vkfwtest -k stress`)?
2. In a failing run, which job locks up, and what does the manufacturer firmware's trace show
   around the first `Guilty overrunning`: which DM, which context, what was running on the
   other DMs?
3. Does the manufacturer firmware do anything when several contexts have work queued at once
   that our scheduler does not: preemption, context-switch requests, or ordering between DM 2
   and DM 3 of different contexts?

#### Q37 — Render-target array with load op LOAD: the background load reads layer 0 (answered by data, vkfwtest baselines `9d74761`: the manufacturer firmware fails step 3 the same way; E047)

[E047](logRefs/E047-vkfwtest-investigation-tests.md). Most vkfwtest failures, including the
step-3 failure of Q36 and the `msaa` failure of Q34, were a Mesa synchronisation bug. Mesa
ignores render-pass dependencies to `VK_SUBPASS_EXTERNAL`, so the read-back copy raced the
render. vkfwtest now puts an explicit barrier before every read-back.
- **With that barrier, `msaa` with RTA on passes.** Q34 was the race, not the PBE resolve.
- **`rta` step 3 changes.** The setup:
  - Mesa `5d2dc45` with RTA on;
  - a render pass with load op `LOAD` over a 6-layer 2D array view;
  - `vkCmdClearAttachments` on layer 1 (whole area) and on an inner rect of layer 5.
- **What happens:**
  - the bitmap marks layers 0, 1 and 5, and our firmware renders `{0, 1, 5}`, writing
    `BCAST[0x03D8]` = 0, 1, 5;
  - the clears land in the right layers (layer 1 passes, layer 5's inner rect is right);
  - **outside the rect, layer 5 holds layer 0's contents**: the background load read layer 0.
- **Mesa's load-op program** (`pvr_usc.c`, `rta_load_clear`, set for a single view with
  `layer_count > 1`) adds `layer_id × layer_size` to the texture address. `layer_id` is
  `RENDER_TGT_ID`. The end-of-tile program sees the right `RENDER_TGT_ID`; the background
  program behaves as if it saw 0.
1. With the manufacturer firmware v1.1, the RTA-enabled Mesa and **the current vkfwtest** (with
   the read-back barrier), does `vkfwtest rta` step 3 pass? The Q36 run was made before the
   barrier, so the race hid this case.
2. Does the background object (`pds_bgnd`) get `RENDER_TGT_ID` from `BCAST[0x03D8]`, or from
   another register or state that must be set per layer? If another, which one, and when is it
   written (before each layer's `0x0F00` kick)?
3. With the current vkfwtest, which of the other tests fail on the manufacturer firmware? We
   pass 42 of 50, and the 6 failures left are the Mesa-bug detectors (`second-*`). Running
   `vkfwtest -k -B <baseline> all` from the current vkfwtest gives both answers.

#### Q36 — Render-target array with load op LOAD: the clears of the touched layers are lost (answered in `88e48e7`: the manufacturer firmware fails the same way, Mesa side; bit 0 comes from the hardware/VDM stream)

[E045](logRefs/E045-rta-fw-1.1.md), `vkfwtest rta` step 3 (vkfwtest after this change):
- the 6-layer colour image is filled by earlier render passes;
- a render pass with load op `LOAD` and **no draw** runs `vkCmdClearAttachments` on layer 1
  (whole area) and layer 5 (an inner rect).

With the RTA-enabled Mesa (`5d2dc45`, `gs_rta_support` on):
- the geometry marks layers 0, 1 and 5 (bitmap `0x23`, `CR[0x0D20]` = 3);
- our firmware renders exactly those (`valid_render_targets[] = {0, 1, 5}`, `RENDER_TGT_ID` =
  layer, region headers at `rgn_header + stride × layer`, `CR[0x03E0]` 0/0/1);
- layers 1 and 5 come back with their **old** contents: the load works, the clears are missing;
- layer 0 is unchanged, which is right.

The other layered renders of the same test pass with RTA on: 6 layers with load op `CLEAR`
(colour + D32), 6 stencil layers (D24S8), and a 3D image viewed as a 2D array. With RTA off in
Mesa, step 3 passes too.
1. With the manufacturer firmware v1.1 and the same RTA-enabled Mesa, does `vkfwtest rta`
   step 3 pass?
2. Does the fragment kick do anything per layer for the background object of a `LOAD` render
   (`pds_bgnd`, `isp_bgobjvals`, `0x0F10`) that the spec does not list?
3. Why is layer 0 marked in the RTC bitmap when only layers 1 and 5 get geometry?

#### Q34 — MSAA resolve in the PBE (`downscale`) writes nothing (`msaa` with `gs_rta_support`; answered in `b50fabe`; E047: it was the Mesa read-back race, `msaa` with RTA on now passes)

[E045](logRefs/E045-rta-fw-1.1.md). When Mesa sees firmware 1.1 it turns `gs_rta_support` on, and
with it it resolves MSAA attachments in the PBE at the end of the render
(`attachment->is_pbe_downscalable`, PBE `REG_WORD0.downscale`, bit 56), instead of a separate
transfer. With the old Mesa (no RTA) `vkfwtest msaa` passes. With RTA on, the render runs
(one layer, no RTA loop, no fault, `HWRTData0State=5`) but every resolved pixel is 0.
1. Does the fragment kick do anything different when a PBE word has `downscale` set: another
   register, a sample-count field, a different `0x0F30` (`ISP_AA`) or `0x0FA0` (`ISP_MSCTL`)
   value, or a step at `PIXELBE_END_RENDER`?
2. Or does the firmware pass the PBE words through unchanged, so that the problem is on the
   Mesa side?

#### Q35 — `CR[0x0D20]` reads 0 at TA finished of the last kick (render-target arrays; answered in `b50fabe`, implemented in E045: bitmap scan at the last kick through a GPU-MMU walk)

[E045](logRefs/E045-rta-fw-1.1.md). Six-layer render in two geometry kicks (`FIRSTKICK`, then
`LASTKICK`), RTC present:
- **First kick:** `CR[0x0D20]` reads 6 at TA finished.
- **Last kick:** we write `CR[0x0D20] = 6` before the kick, then it reads 0 at TA finished.

The spec reads `CR[0x0D20]` only at TA finished of a non-last kick, and at the last kick it
scans the RTC bitmap (`rtc_dev_addr + 0x200`). We cannot map client memory yet, so we keep the
count of the earlier kicks and list layers `0..max_rts-1` when the count equals `max_rts`.
1. Does `CR[0x0D20]` count the layers set for the first time in the current kick, or is it reset
   by `CR[0x0CB8] = 1`?
2. At TA finished of the last kick, does the firmware read `CR[0x0D20]` at all, or does it take
   the count from the bitmap scan only?
3. How does the firmware map `rtc_dev_addr + 0x200`: which MMU context (the render context's
   memory context?), and which window (remap slot, TLB entry)?

#### Q31 — Who fills `rta_ctl.active_render_targets` and `valid_render_targets[]`? (render-target arrays; answered in `dd37220`, implemented in E045)

The Q27 answer runs the layer loop only when `rta_ctl.active_render_targets` (`+0xD8`) is
non-zero. The mainline kernel creates the HWRTData with `active_render_targets = 0`,
`render_target_index = 0` and a `valid_render_targets` array of `max_rts` u32 entries that it
does not initialise. Mesa never writes them. So something in the firmware sets them.
1. When are `active_render_targets` and `valid_render_targets[]` written (geometry kick,
   `TA finished`, fragment kick), and from what (a register read back after the TA, e.g.
   `0x0D20`, the RTC, the command)? What is `cumul_active_render_targets` (`+0xDC`) used for?
2. With `max_rts = 6` and geometry in only some layers, does the reference render all six
   layers (each needs its clear/store) or only the layers with geometry?
3. Does the fragment kick reset `render_target_index`/`current_render_target` at the start of
   each render, and `active_render_targets` at the end?
4. Are `rgnhdr_single_rt_size` / `rgnhdr_scratch_offset` (fragment regs `+440`/`+444`) used?
5. Does a partial render (`FRAG_PR`) of a layered render loop over the layers too
   (`rta_num_partial_renders_fw_addr`)?

Our choice until answered (E045): at the fragment kick, if `active_render_targets == 0`
and `max_rts > 1`, fill `valid[i] = i`, set `active_render_targets = max_rts`, and clear it
again after the last layer; `make EXP=-DEXP_RTA_ACTIVE_ONLY` turns this off.

#### Q32 — Occlusion queries: which register takes `isp_oclqry_base`? (answered in `dd37220`: `CR[0x0FC0/0x0FC4]`; `queries` passes, E045)

With the manufacturer firmware, `vkfwtest queries` reads an occlusion count of `960`; with
ours it reads `0` (E044). Our fragment kick does not write `rogue_fwif_frag_regs`
`isp_oclqry_base` (`+112`, 64-bit) nor `isp_oclqry_stride` (`+80`); the register table of
`kick-compute-and-3d.md` does not list them, and no public header gives their address.
1. Which register(s) does the fragment kick write from `isp_oclqry_base` (and
   `isp_oclqry_stride`, `isp_zls_pixels` `+84`, `isp_xtp_pipe_enable` `+48`, `tpu` `+56`,
   `pixel_phantom` `+64`, `fb_cdc_zls` `+144`, `isp_dummy_stencil/depth_store_base`
   `+424`/`+432`), and under which condition?
2. Does the firmware do anything at the end of the render for the query counters (a flush,
   a write-back), before the DM 3 cache flush?

#### Q33 — Who supplies the render-target id to the end-of-tile program? (answered in `dd37220`: `BCAST[0x03D8]`; `rta` passes, E045)

[E045](logRefs/E045-rta-fw-1.1.md). With firmware 1.1 and a public Mesa that enables
`gs_rta_support`, the layer loop runs (`Kick RTA render 2 of 6` … `6 of 6`). But every layer
is written to layer 0: a 6-layer colour image shows the layer-5 clear colour in layer 0.
- **Public Mesa:** for an image that is not 3D, the end-of-tile program computes the PBE
  address as `state + rta_element_stride × layer_id` (`pvr_usc.c`, `nir_umad64_32`).
  `layer_id` is the USC special register `RENDER_TGT_ID` (`PCO_SR_RENDER_TGT_ID`).
- **The spec** (`kick-compute-and-3d.md`, "Render-target arrays") adds `layer` to bits
  `27:14` of PBE word 1 only when `(word0_hi & 3) == 2`, which is `TWIDDLE_3D`. A 2D array
  image has `memlayout` 1 (twiddled) or 0 (linear), so that step does nothing for it. In our
  trace `word0_hi` is `0x0200007D` and `max_rts` is 6: no MRT is selected.
1. What gives `RENDER_TGT_ID` a value other than 0 in the end-of-tile program? Is it a
   register the firmware writes per layer (which one, which value: the layer index or
   `valid_render_targets[index]`), or does the hardware take it from the region header?
2. If it is a register, is it written once per layer next to `0x0F20/0x0F24`, or at the
   first layer only?
3. Is the `RENDER_TGT_ID` for a 3D image (`TWIDDLE_3D`) the same value, with the `zslice`
   add as an extra?

#### Q30 — Restart after the second partial render on the same HWRTData (answered in `50055ae` and `partial-render-spm.md`; implemented in E049, `bigpb` passes; item 4, on-demand ZS backing, not implemented)

[E043](logRefs/E043-pm-mmu-inval-first-kick-reset-hwr.md), `vkfwtest bigpb` frame 3 (4 M
triangles). The partial render now runs:
- `FENCE_PR` is passed through the SPM failing-UFO check without being consumed;
- the `FRAG_PR` is kicked with PM context 1 = the geometry's current catalogues and SPs,
  `0x0338 = 1`, `pds_pr_bgnd`, no ZLS.

**The first one works**:
- `PR executed`;
- the restart: `0x0338 = 0`, slot 1 dropped, state 1, context-0 select, `0x0290`,
  `0x0198`, `0x1348`, `0x01B8` + poll, `0x0328`, `0x0CA8`;
- the TA continues until the next OOM.

The **second** partial render on the same HWRTData runs (14 ms). At its restart:
- the `0x01B8` poll times out;
- a BIF0 page fault follows (MMU status `0x1021`, request `0x0011f08001008ff0`).

With the full PM-MMU invalidation in the restart (SLC `0x10`, `BIF_CTRL_INVAL = 4`,
`0x1348`, each polled) instead of `0x1348` alone, **the board hung**.

1. The exact restart sequence after a partial render, with polls and order. In
   particular:
   - what "save the 3D PM context's catalogue bases" writes, and where;
   - is the PM-MMU invalidation in the restart the full three-step one, or `0x1348` only?
2. Before the second partial render's `FRAG_PR`, does PM context 1 take the geometry's
   catalogues again, or does it continue from the first partial render's state?
3. What should `0x01B8` wait for, and what does the reference do when it does not clear?
4. Partial renders of a render pass with a depth attachment: which ZLS values does the
   `FRAG_PR` use (the reference's `ZSBUFFER_BACKING`)? Is it the command's ZLS registers
   with load/store forced, or nothing?

#### Q29b — The second HWRTData of a render-target dataset still fails (answered in `d244503`, fixed in E043)

[E042](logRefs/E042-spec-fab1fc9-tpc-oom-init.md). The Q29 answer is implemented:
- the TA-finished sequence: VHEAP store, TPC flush `0x0C50` with its poll, RTC flush;
- the full geometry cache flush;
- RTC programming (`0x0CB0 = 0`, `0x0CE0 = 1/3`, `0x0CB8 = 1`, `0x0D20`);
- `0x0C68` on first kicks only.

None of the new polls times out. Repeated renders on **one** HWRTData work (many per
device in `basic_draw`). Every render on the **second** HWRTData of a dataset fails:
- `vkfwtest pre-3d` render 2 (`RTD 0xc0138000`): the TA takes 21 local pages and stalls:
  - `0x0D00 = 0x0F`;
  - no `PM_OUT_OF_MEMORY`;
- `vkfwtest bigpb` frame 1 (`RTD 0xc00bc000`): BIF0 page fault in the TA at once:
  - MMU status `0x8001`;
  - request status `0x00006e0002b3a000`;
- `renderpass1…d32_sfloat.input.clear.dont_care.draw`: the TA of the second HWRTData
  finishes, then the 3D faults on the IPF read at `0x2000` (request
  `0x0010250000002000`).

With `CONTEXT_PB_BASE = 0` and no free-list reload, as the reference does for the second
HWRTData, `pre-3d` render 2 still stalls.

Per HWRTData we write:
- MLIST base `0x02D8`;
- `PM_CAT_BASE_*0` = the HWRTData's `*_cat_base` (0 from the host);
- list SPs 0;
- free lists;
- region headers `0x0C38` (`+0x80` from HWRTData 0, as the reference);
- TPC `0x0C40` (shared);
- VHeap table `0x0248` (shared);
- VDM stream base `0x0408`;
- `te_psg`, `ppp_ctrl` from the command.

1. Is anything per HWRTData written that we do not write: a register, or a firmware write
   to HWRTData / HWRTData-common / region-header memory before the first kick of
   HWRTData 1?
2. Does the reference select which HWRTData of the pair is active (the trace prints
   `HWRTData0State` / `HWRTData1State`)? Where does it get the pair from, and does it
   write the index anywhere (`te_psg`, `0x0D20`, a PM register)?
3. In `pre-3d` on the reference: the trace lines and the register writes from `3D
   finished` of render 1 to `Kick TA` of render 2 (RTD of HWRTData 1).
4. After a lockup and HWR on a render, every later TA on the device stalls, also on
   HWRTData 0. What does recovery reset in the PM/TA besides the `SOFT_RESET` pulse
   (`Zero RTC for FWCtx`, free-list reconstruction, `0x0D20`, …)?

#### Q29 — Why does the second render on a device stall the TA? (answered in `fab1fc9` and `d244503`; fixed in E042/E043, see Q29b; the later "frame 1" failures were a Mesa bug, E047)

[E041](logRefs/E041-render-default-and-spm.md), `vkfwtest pre-3d`. One device draws the same
quad several times, and Mesa alternates the two HWRTData of the render-target dataset.

Render 1 (HWRTData 0) works:
- `Load Freelist` local `SP 0x1f`, global `0x1bf`;
- `TA finished`, the 3D, `PM_3D_MEM_FREE`;
- `Store Freelist` back to `0x1f`/`0x1bf`.

Render 2 (HWRTData 1) loads **exactly the same free-list values**:
- `CONTEXT_PB_BASE = 3`;
- both free lists reloaded;
- list SPs and catalogues from 0;
- `0x0C38`/`0x0C40`/`0x0408` from HWRTData 1 and its command.

The TA then takes 21 local pages and stops:
- `0x0D00 = 0x0F`;
- no `PM_OUT_OF_MEMORY`;
- lockup after 30 s.

From then on **every** geometry kick stalls, also:
- on HWRTData 0 again;
- on another device's first render;
- after the GPU was powered off and on between the renders.

`basic_draw` (one render per device) passes 203/331.

1. What does the reference do at `TA finished` and before the next first geometry kick
   that we do not do? In particular the **TPC flush** (Q19b item 1): register, value, poll,
   and whether it runs at every `TA finished`.
2. Does the reference write anything to memory between two renders: TPC contents,
   region headers, the RTC, `rtc_dev_addr` / `macrotile_array_dev_addr`, HWRTData fields?
3. In `pre-3d` on the reference, the trace lines from `TA finished` of render 1 to
   `Kick TA` of render 2, with the register writes in between (same sampling as the
   Q16m answer).
4. For render 2 the reference may print `CONTEXT_PB_BASE set to 0x0` (shared free lists,
   as for the second HWRTData of a render pass). Is a reload with `0x3` and identical
   values expected to work?

#### Q27 — What does firmware 1.1 change for render-target arrays (`gs_rta_support`)? (answered in `fab1fc9`; implemented in E045, with Q31–Q39)

Mesa MR 40973 enables `gs_rta_support` only for firmware 1.1 or later, and the acceptance
oracle expects `FW version v1.1`. We keep reporting 1.0 until we implement what 1.1
changes. In the same form as the other answers:
1. how the geometry and fragment kicks use `rogue_fwif_hwrtdata.rta_ctl`,
   `rgnhdr_single_rt_size`, `rgnhdr_scratch_offset`, `zls_stride` and `sls_stride`;
2. per-layer region headers and PBE addressing, if the firmware takes part in them;
3. any other firmware-visible difference between the v1.0 and v1.1 open images.

#### Q16n — Input-attachment render: state outside the dumped ranges (resolved by the Q29b answer: `d32_sfloat.input.*` 72/72 in E043)

Our second render now matches `input-attachment-dump.md` in every dumped range
([E040](logRefs/E040-input-attachment-dump-diff.md)), and the shared-PB rule
(`CONTEXT_PB_BASE` 3 → 0, no reload) is in place. It still faults on an IPF control read at
`0x2000`.
1. During that second render in the reference: the non-zero registers in `0x0800–0x0BFF`,
   `0x1000–0x14FF` and `0x1800–0x3FFF`, especially BIF/PM (`0x1200–0x12FC`, `0x0200–0x03FC`,
   `0x2000–0x20FC`).
2. Does the reference use the HWRTData's `macrotile_array_dev_addr` or `rtc_dev_addr`
   (geometry or fragment kick)? Our kicks write neither.
3. Is anything written to memory by the firmware between the two renders (region headers,
   HWRTData fields such as the `*_last_cat_base`, `pm_alist_stack_pointer`)?

#### Q16m — Input-attachment renders, `FRAG_PR`, and multi-kick geometry (answered; see E039; the input case was fixed with Q29b, E043)

With the Q16l answer applied ([E038](logRefs/E038-zls-mem-free-and-depth.md)):
- `basic_draw` passes 203/331, with no failures (the rest is not supported);
- `PM_3D_MEM_FREE` is seen after every render;
- depth load and store work.

Open:
1. **Renders that read an input attachment** (`renderpass1.suballocation.formats.*.input.*`,
   36 of 81 for `d32_sfloat`). The second subpass's render (colour only, `flags` `0x8000`,
   `isp_ctl` `0x80011000`) faults on a **read at GPU VA `0x2000`**: BIF0 MMU status
   `0xD001`, request `0x00102500_00002000`. During such a render in the original, which
   fragment-kick registers hold values that ours does not write? A full-range sample of
   `0x0600–0x07FC`, `0x0F00–0x0FFC`, `0x1500–0x17FC` and `0x4000–0x40FC` during that
   render would let us diff.
2. **`FRAG_PR`** (cCCB type `0x2ABC80CC`) is queued before every fragment command. We
   retire it without running it. What does the original do with it when no partial render
   is needed? Does it stay unrun, or does it have side effects (UFO updates, HWRTData
   state)?
3. **Multi-kick geometry, OOM, grow and partial renders.** Which registers does the
   firmware write for a geometry kick that is not the first (`First kick:0`) of the same
   HWRTData, and for the OOM paths of `partial-render-spm.md` (pause, free-list update,
   resume, PR)? Register list and order, in the same form as the load commands.

#### Q16l — Remaining render items after the first working draws (answered, see E038)

Since [E037](logRefs/E037-pm-list-sp-load-commands.md) the clean firmware renders:
- `simple_draw` 4/4 and `api.smoke` 8/8;
- `basic_draw` 201/331 (2 fail, 128 not supported);
- no faults.

Open points, in priority order:
1. **`PM_3D_MEM_FREE` (`EVENT_STATUS` bit 8) is never seen on our firmware**, although the
   pages do come back (`0x0350` → `0x1F`). Is an enable needed (a register, or a bit in
   `0x03E0`/`0x0388`), or a clear of a previous state? What does the firmware wait on
   before the next load of the same PM context?
2. The **`0x0C68` context-enable poll** (write 1, poll bit 0 = 1) fails on the second and
   later geometry kicks of a process; the TA still runs. Does the original clear `0x0C68`
   (or something else) at geometry completion?
3. **ZLS**: which registers take `isp_zlsctl`, `isp_zload_store_base` and
   `isp_stencil_load_store_base` (and `fb_cdc_zls`), and under which `flags` bits
   (`DEPTHBUFFER`, `STENCILBUFFER`)?
4. **`0x0F28` in the 3D kick** ("render target state"): its source field and value for a
   normal render.
5. `basic_draw.draw.point_list.45` and `basic_draw.misc.flat_b_sat_error` fail on image
   comparison. If the original passes them, which fragment-kick register differs?

#### Q16k — Loading the ALIST/MLIST stack pointers of PM context 1, and freeing the pages after the 3D (answered + E037: commands `0x0288`/`0x0190`)

The geometry finishes, and **the first draw of a run renders correctly** 
([E035](logRefs/E035-pm-freelist-load-commands.md),
[E036](logRefs/E036-first-3d-render.md)). At the fragment kick we load PM context 1:
- local `0x0218`/`0x0230`/`0x0398`, then the command `0x0200`;
- global `0x2078` bank, then the command `0x20A8`;
- `0x02B0 = 0`, `0x02E0`, `PM_CAT_BASE_*1` = the catalogues the geometry built;
- the geometry's ALIST/MLIST SPs written to `0x0278`/`0x02C8`.

Problems:
- **`0x0280`/`0x02D0` stay 0.** They are not writable.
- After the 3D, the local live SP of context 1 (`0x0350`) **stays `0x10`**, where the
  original climbs back to `0x1F`. The pages are never freed.
- The next draw's geometry then writes at GPU address `0x10` and faults.

1. How are the ALIST/MLIST stack pointers of a PM context loaded (trace RTD 12, `Loading
   stack-pointers for 1 (0:MidTA,1:3D) on context 1, MLIST = …, ALIST = …`)? Which value
   registers, which **load command** register (like `0x0208`/`0x20B0`), and which poll?
   Is the same mechanism used for context 0 at a geometry kick that is not the first?
2. How does the PM free the render's pages after the 3D (Q20)? Is it automatic once
   context 1 holds the right ALIST/MLIST, or does the firmware issue a command (at the
   kick or at `PIXELBE_END_RENDER`)? What does the firmware wait for before it reads the
   free lists back?
3. Is there a **store** command that captures a context's live free-list state (`Store
   Freelist … for PMDM0` at the fragment kick, and at HWRTData cleanup), or does the
   firmware read the live registers directly? If it reads them, which registers hold the
   local live allocated-page counts?
4. Must anything be reset in PM context 0 or 1 between two renders (for example
   `PM_CAT_BASE_*`, `0x03D0`/`0x03D4`, or the HWRTData state) before the next geometry
   load?

#### Q16j — The exact register writes from `Activate MemCtx` to `Load Freelist` (answered: free-list load commands, E035)

With the Q16i answer applied ([E034](logRefs/E034-pm-copy-q16i-variants.md)), our register
state at the geometry kick matches every value the spec observed in the original:
- `0xF320` = 0;
- the unit values of the first load (`0x1500`, `0x1780`, `0x1788`, `0x178C`, `0x0FA0`,
  `0x17B8`);
- `CAT_BASE1` and the TA field of `CAT_BASE_INDEX`, written before or after the PM writes;
- `0x20F8`, `0x0240`, `0x03D0`/`0x03D4`;
- local and global free lists, base first;
- `0x02B0`.

The live stack pointers `0x0348`/`0x20C8` still stay 0. Sampling has shown that the missing
step changes no register value. Sampling cannot settle this; it needs the original's
**write sequence**, in the spec's usual form (offset, value or value source, order, polls):
1. Every register write and poll from `PM running primary config` (trace PM 30) to
   `CONTEXT_PB_BASE set to …` (RTD 3) at a first geometry load. Include **writes of a value
   the register already holds**, writes to registers outside the sampled ranges, and any
   read whose result is waited on.
2. In particular, what the "primary config" selection step writes (PM 30 / PM 31), and
   whether the free-list registers are written through a different bank or alias (for
   example a per-config window) rather than at `0x0210`/`0x20B8` directly.
3. If the copy is triggered by a write to a register we already write (a "commit" or
   "load" bit), which register, which bit, and in what order relative to `+0x00`/`+0x08`.

#### Q16i — What action copies the loaded free list into the PM's live stack pointers? (answered in part, E034; resolved with Q16j/Q16k)

With the Q16h answer applied ([E033](logRefs/E033-pm-first-load-flags.md)), our PM state
after init equals the original's: the same non-zero list, with `0x20F8` going 0 → 1 at the
load. At the first geometry load we write, in the observed order: `0x20F8 = 1`, `0x0240`,
`0x03D0 = 0x100`, `0x03D4 = 1`, `0x0210`/`0x0224`, `0x20B8`/`0x20C0`/`0x20D0`/`0x20D4`,
`0x02B0 = 3`, `0x0398`/`0x039C`, MLIST, VHeap. Every value lands, but **`0x0348` and
`0x20C8` stay 0**: read by our firmware right after the writes, and still 0 seconds later.
Also tried: `0x0388 = 1`, handshakes before, around and after the writes, a
`PM_PAGE_MANAGEOP` pause, the broadcast bank, clocks ON, 32-bit writes, and the bases
rewritten after the tops.

The Q16h answer says the copy is **a separate step** (5–13 µs after the `+0x08` write in 48
of 136 loads), so it is something that leaves no value change in 6 µs sampling.
1. In the 48 delayed loads, **which register changed in the same sample as the late live
   copy**, or in the sample just before it? Any register at all, inside or outside the PM
   ranges: `0x0D00`, EVENT_STATUS, MTS, `0x0420`, `0x0C68`, BIF/MMU (`0x1200–0x12FC`), SLC.
2. Do `0x03D0`/`0x03D4` change in the **same** sample as the local live copy (`0x0348`) in
   later loads, or always one sample before? Which one comes first decides whether they are
   a command (written by the firmware) or a status (set by the PM after the copy).
3. Is a **PM TLB / MMU invalidation** (`0x1348` "PM TLB", or `MMU_CTRL_INVAL` with the PM
   or all-contexts bit) or an **SLC flush** issued between the free-list writes and the
   live copy? An operation like that would leave no value change.
4. If register sampling cannot catch it, **which firmware trace lines** (group and id, from
   the public trace list) appear between `UFL-TA-Base`/`FL-TA-Base` and `Kick TA` at a
   first load?

#### Q16h — What makes the PM copy `+0x08`/`0x0224` into its live stack pointers? (answered: first-load registers, see E033)

With the Q16g answer applied ([E032](logRefs/E032-pm-live-sp-load-variants.md)): `0x0240 =
0x80_0000_0000`, local `0x0210`/`0x0224`, geometry bank `0x20B8`/`0x20C0`/`0x20D0`/`0x20D4`,
`0x02B0 = 3`, MLIST, VHeap. The values land, but **`0x20C8` and `0x0348` stay 0**, read by
the firmware itself right after the writes. Tried without effect: handshakes after the
writes, no handshakes, `PM_PAGE_MANAGEOP` pause around the writes (`0x02A8` acks it), the
broadcast bank, all clocks forced ON, 32-bit writes. With the PM writes **between**
`0x0250 = 1` and `0x0258 = 1`, the `0x0258` poll fails and a read at address 0 faults
(request status `0x0011B000_00000000`: `TAG_SB 4`, `TAG_ID 0x1B`).

1. Before the **first** geometry load after boot, does the original write anything to the
   PM that is not in the per-kick list: in GPU init, at the first kick, or at a freelist
   `FREELISTS_RECONSTRUCTION`/init command? A register sweep of `0x0200–0x03FC` and
   `0x2000–0x20FC` **right after boot, before any job**, compared with ours (we can send
   ours), would show it.
2. Is there a PM register that must be non-zero for the live registers to load, for example
   a PM context select, a "PM enable", the PB size (`0x2090`… total pages), or the
   `0x02F8` counter the answer saw start counting?
3. Is the `0x20C8` change in the same sample as the `0x20C0` change also seen when
   `0x0250`/`0x0258` are skipped? What is the requester with `TAG_SB 4` / `TAG_ID 0x1B`
   (which unit reads at the base address)?

### A. Geometry (TA) kick (2026-11-09; all answered, see each heading)

The questions from here down were written on 2026-11-09, when nothing could be drawn yet.
Compute, transfers, power, HWR and statistics worked then; the geometry and fragment kicks,
the parameter-buffer handling and preemption were missing. All of them were answered
afterwards, partly by `docsClean` and partly by experiments (E027–E043).

#### Q16g — How does the firmware load the free lists into the PM's live state? (resolved: free-list load commands, E035/E037)

After the Q16f answer ([E031](logRefs/E031-pm-live-stack-pointers.md)):
- The live global stack pointer `0x2088` and the live counter `0x2098` **read 0 although
  we write them**. `0x2088` is also not writable in our E008 probe.
- `PM_CAT_BASE_{VCE,TE,ALIST}0` **stay 0** for the whole job. There is no MMU fault in
  any bank.
- So the PM has taken no page at all: its live stack pointers are 0, and the free lists
  look empty to it.

Written before the PM handshakes:
- local: `0x0210` base, `0x0224` loaded top, `0x0398`/`0x039C` counts;
- global bank: `0x2078` base, `0x2090`/`0x2094` counts;
- `0x02B0 = 3`, VHeap `0x0248`, MLIST `0x02D8`;
- then `0x0250 = 1` / poll bit 0 = 1, `0x0258 = 1` / poll bit 0 = 0.

Both polls succeed.

1. What copies the loaded values into the live stack pointers: the `0x0250`/`0x0258`
   handshakes, or another register?
   - What is `0x2080` (`+0x08` of the bank: `0x20`, `0x108`, `0x109`)? Is it the source of
     the live SP, a page count, or a command?
   - Is there an equivalent for the local free list?
2. Does the PM read anything from **memory** at load time? For example the free list's
   page stack at the base address, or a header the host prepares that our firmware should
   update first.
3. Right after a geometry load in the original, before the TA trigger, what do `0x2088`
   and the (unidentified) live local stack pointer read? If they are already the stack top
   by then, which register write made them so?

#### Q16f — PM page catalogues on a first kick (resolved: catalogue bases and the first-kick PM reset, E037/E043)

With every PM register the spec identified now programmed as observed
([E030](logRefs/E030-ta-global-freelist-pb-base.md)): the local FL (`0x0210`, `0x0224`,
`0x0398/0x039C`), the global FL bank (`0x2078`, `0x2090/0x2094/0x20A0`), `CONTEXT_PB_BASE`
`0x02B0 = 3`, MLIST `0x02D8`, VHeap, the TA state `0x0CB0` and the constants. Geometry still
waits forever for its first page. `0x0D00` reads `0x0F` (units 0–3 idle, 4–5 busy), the
inverse of the original's `0x30–0x35` while running.

The answer to Q16b says `PM_CAT_BASE_{VCE,TE,ALIST}0` receive **new valid entries (bit 0
set) at every geometry kick**, while the host leaves the HWRTData catalogue bases at 0. We do
not write guessed values there: they are physical addresses used without the MMU.
1. **Who writes `PM_CAT_BASE_*0` before or at a first kick, and with what?** Does the
   firmware take pages from a free list (which one, and how is the stack popped), or does
   the PM do it after an enable? Which enable or register makes it do so?
2. Are the `PM_CAT_BASE_*0` values related to the HWRTData (`pm_mlist_dev_addr`,
   `vheap_table_dev_addr`, `macrotile_array_dev_addr`) or to the free lists? A few
   example values with the matching free-list base and stack top from one run would let
   us check the relation without the code.
3. Which register holds the global free list's **stack pointer** (`SP = …` in the trace)?
   We use `0x2080` (22-bit, the bank-1 pair is `0x20C0`) as a guess.
4. What do bits 4 and 5 of `0x0D00` report, given that ours are busy (0) while the
   original's are idle (1) during geometry?

#### Q16e — The PM still allocates nothing (resolved: the PM allocates since E037)

With the Q16b/Q16c answers applied ([E029](logRefs/E029-ta-pm-registers-from-answers.md)),
the geometry runs (`PERF_TA_CYCLE` counts; `0x0420` counts the kick; `0x0D00` goes
`0x3F` → `0x0F`). It still stops after the first 64 bytes of the control stream: local
stack top stays `0x1F`, ALIST/MLIST stay 0. Programmed for PM context 0:
- `0x0210` local base, `0x0224` stack top, `0x0398`/`0x039C` counts;
- `0x0248` VHeap, `0x02D8` MLIST base;
- `PM_CAT_BASE_*0` = 0 (first kick), handshakes `0x0250`/`0x0258`;
- `0x0CB0` TA state base, constants of Q16c.

Questions:
1. **Global free list:** which registers hold its base, stack top, total and counts for
   PM context 0? The 0x2000 block (`0x2078` + `0x2090/0x2094`) was tried with no effect.
   The answer said they are outside `0x0200–0x043F`. Could the dirty team sample
   `0x0440–0x0800` and `0x2000–0x2200` the same way?
2. **PM start:** must the firmware write `PM_PAGE_MANAGEOP` (`0x02A0`) or another register
   to start or unpause allocation for a first kick? (It reads 0 during our stalled job.)
3. **What does `0x0D00 = 0x0F` mean** (bits 4 and 5 clear while the TA runs)? Which units
   are those?
4. Is anything else written between the PM handshakes and the VDM trigger that the
   register sampling cannot show, for example a write that self-clears? A write-ordered
   list for one first kick would settle this, even without field names.

#### Q16b — PM free-list registers (answered in part; resolved with Q16e–Q16k)

With the TA kick of [E028](logRefs/E028-ta-kick-bringup.md), geometry starts: the TA
cycle counter runs and the VDM fetches its first 64 bytes. The PM then hands out no page:
the stack top stays `0x1F`, and ALIST/MLIST stay 0. Programmed so far for PM context 0:
- `0x0248` = VHeap;
- `0x0210` = local free list address;
- `0x0220` = `current_pages` (low 22 bits) | stack top (`0x0224`);
- `PM_CAT_BASE_{VCE,TE,ALIST}0` = HWRTData cat bases (0 on a first kick);
- `0x0278`/`0x02C8` = ALIST/MLIST SP;
- `0x02D8` = `pm_mlist_dev_addr` (guess);
- handshakes `0x0250`/`0x0258`.

Questions:
- Which registers receive each value the public "Load Freelist" trace prints, for the
  local and the global free list and for each PM context: TotalPMPages, FL-addr, stacktop,
  Alloc Page Count, Alloc MMU Page Count? The E008 masks show 22-bit fields at
  `0x0220` (low/high), `0x02B8` and `0x02C8`, 33-bit fields at `0x0268`/`0x0278`, a 16-bit
  field at `0x0298`, and 40/44-bit addresses at `0x0218`, `0x0230`, `0x0240`, `0x02D8` and
  `0x02E0`.
- Is the low field of `0x0220` the total page count?
- Which register is `PM_MLIST0_BASE`, and is `pm_mlist_dev_addr` loaded on a first kick?
- On a first kick the host leaves the HWRTData's `vce/te/alist_cat_base` at 0. Does the
  firmware write anything to `PM_CAT_BASE_*0`, or does the PM fill them itself?
- Is any other PM enable needed: `PM_PARTIAL_RENDER_ENABLE`, a PM "context select" or
  "start", the macrotile array (`PM_MTILE_ARRAY`)?

#### Q16c — geometry registers still unmatched (resolved: geometry kick complete, E028–E042)

After a working geometry job the spec saw `0x0C30/0x0C34 = 2/3`, `0x0C48 = 8`,
`0x0C70 = 3`, `0x0C90` (64-bit) = `0x3089705F`, `0x0CA0 = 0x10`, `0x0CE0 = 1` and
`0x0D00 = 0x3F`. Which of these are written by the firmware, and with what (constants,
command fields, `hwrtdata_common` fields)? E027 matched the others by value (`0x0C98`,
`0x0C18`, `0x0C08`, `0x0C10`, `0x0C20`, `0x0C78`, `0x0C80`, `0x0418`). Please confirm those.

#### Q16d — context-state registers (resolved: VDM context-state base, E028)

[E028](logRefs/E028-ta-kick-bringup.md) found by experiment that the hardware reads
`[0x0CB0] + 0x200` when the geometry context is enabled, and that
`0x0CB0 = geom_reg_ta_context_state_base_addr` removes the fault. Please confirm, and give:
- the register for `geom_reg_vdm_context_state_base_addr` (`0x0428` is a candidate);
- the meaning of `0x0CE0`, `0x0CB8`, `0x0C48` and `0x0D20` for a new kick.

#### Q15 — What is a "register set"? (answered, see the Answered table)

`kick-geometry-ta.md` writes `CR[set+0xC00…0xC98]`, `CR[set+0xCB0…0xCE0]`,
`CR[set+0x4098]` and the run register `CR[set+0x400]`, and says the set's displacement is
part of the offset. The trace says "Setup register set=1 DM=…", which matches the BIF slot
the firmware picks.
- Is the register set the BIF catalogue slot (1–7) chosen by the memory-context
  activation, or a separate index?
- How is `set` turned into an address: base offset and stride per set? For set 1, what is
  the absolute offset of `set+0x400` and of `set+0xC00`?
- Do compute and transfer use register sets too? (Today they only use `CAT_BASE_INDEX`
  and that works.)

#### Q16 — Loading the HWRTData into the PM, geometry phase (answered in `kick-geometry-ta.md` phase B and Q16b–Q16m; implemented E033–E043)

Phase B says "the command's HWRTData is loaded into the PM, for the geometry buffer and
for the list (MLIST)", then gives only the handshakes `CR[0x250]`/`CR[0x258]`. The sequence
itself is needed:
- **Which registers** receive which fields of `rogue_fwif_hwrtdata`?
  - `pm_mlist_dev_addr`, `pm_mlist_stack_pointer`, `pm_alist_stack_pointer`;
  - `vce_cat_base[4]`, `te_cat_base[4]`, `alist_cat_base` and their `*_last_cat_base`.
    The public `ROGUE_CR_BIF_PM_CAT_BASE_VCE0/TE0/ALIST0/VCE1/TE1/ALIST1` are at
    `0x1248–0x1280`: which of the two banks, and how do the 4 entries map?
  - `vheap_table_dev_addr`, `tail_ptrs_dev_addr`, `macrotile_array_dev_addr`,
    `rgn_header_dev_addr`, `rtc_dev_addr`.
- **Free lists:** from `freelists_fw_addr[]` (local and global), which `rogue_fwif_freelist`
  fields (`freelist_dev_addr`, `current_dev_addr`, `current_stack_top`, `current_pages`,
  `ready_pages`, `pm_global_pb`) go to which PM registers?
- **First kick vs later kicks** of the same render (`ROGUE_GEOM_FLAGS_FIRSTKICK`,
  state `KICK_GEOM_FIRST`/`KICK_GEOM`): what is loaded only on the first kick, and what is
  reloaded from the HWRTData on later kicks?
- **Order:** where do these writes go relative to the `0x250`/`0x258` handshakes?

#### Q17 — Register-set values that come from state, not from the command (answered in `kick-geometry-ta.md`; implemented E027/E028)

The phase-C table names their sources only as "state":
- `CR[set+0xC28]`, "context RTData address in the PM": which value exactly (an address
  from the HWRTData, an index)?
- `CR[set+0xC38]`/`CR[set+0xC40]`: what is written before the poll, and which value is
  awaited?
- `CR[set+0xC88…0xC98]`: the spec says the split of `vdm_ctrl_stream_base` and the control
  field can be finished "from public field widths". Please give the exact words written to
  `+0xC88`, `+0xC90`, `+0xC98` for a first kick, and which command flag selects
  `tpu_border_colour_table` versus `vdm_draw_indirect0` in `+0xC80`.
- `CR[set+0x4098]` (byte): which command field?
- `rogue_fwif_geom_regs.ppp_ctrl`: the spec says it is consumed by the PM/HWRTData load
  path. Which register receives it?
- **`rogue_fwif_hwrtdata_common`** (`screen_pixel_max`, `multi_sample_ctl`, `tpc_stride`,
  `tpc_size`, `te_screen`, `mtile_stride`, `teaa`, `te_mtile1/2`, `isp_merge_*`,
  `rgn_header_size`, `isp_mtile_size`): which of these does the firmware write, to which
  registers, and in the TA kick, the 3D kick, or both?

#### Q18 — TA state buffer and context registers for a new (non-resumed) kick (answered in `kick-geometry-ta.md`; implemented E028)

Phase D lists `CR[set+0xCE0]`, `CR[set+0xCB0]` (poll), `CR[set+0xCB8] = 1`,
`CR[set+0xC48]` and `CR[0xD20]` = state buffer address, without values.
- What is written to each for a **new** kick, and from which fields?
  `rogue_fwif_geom_registers_caswitch` (`geom_reg_vdm_context_state_base_addr`,
  `…_resume_addr`, `geom_reg_ta_context_state_base_addr`) and
  `rogue_fwif_geom_ctx_state_per_geom` (`geom_reg_vdm_call_stack_pointer(_init)`) look
  related.
- What does the `CR[set+0xCB0]` poll wait for?
- Is `CR[0x40A8] = 1` (primary core) needed on a single-core GPU?
- Which value is written to `BCAST[0x0408]`/`0x040C` and `CR[0x418]` (the free-list/ALIST
  broadcast of phase E)?

#### Q19 — TA completion (answered, Q19b in the Answered table; implemented E041/E042)

- Events: completion is `TA_FINISHED` (bit 5, routed by `MTS_DM2`). Must the firmware also
  handle `TA_TERMINATE` (bit 6), and when does it occur?
- **Write-back:** after the TA, which PM registers are read back into the HWRTData
  (`pm_alist_stack_pointer`, `pm_mlist_stack_pointer`, …) and into each
  `rogue_fwif_freelist` (`current_stack_top`, `current_dev_addr`, `allocated_page_count`)?
  How does `state` move (`GEOM_FINISHED`; first/last kick)?
- **Cache flush:** the completion-flush table gives the geometry variant only partly
  (`0x0688 = 1`, `0x0668 = 5`, SLC `DM_TA`). Please give the full ordered geometry
  sequence, as for fragment and compute: steps 1–9, the `0x1608`/`0x1720` values, and the
  MCU fence. Where does the "TPC flush" fit?
- Is a PM "unload" or pause needed before the next TA of another HWRTData?

### B. Fragment (3D) kick (2026-11-09; all answered)

#### Q20 — Loading the HWRTData into the PM, fragment phase, and PM deallocation (answered in `kick-compute-and-3d.md` and Q16k; implemented E037/E038, PM sync E049)

Phase A says "Load the HWRTData into the PM (fragment phase)", and "pause the PM
deallocation" for a partial render.
- Which registers and fields does the fragment-phase load write (MLIST, region headers,
  macrotile array, free lists for deallocation)?
- How is "pause the PM deallocation" done?
- **After the 3D:** how are the PB pages returned to the free list? Is it automatic
  (event `PM_3D_MEM_FREE`, bit 8), or does the firmware drive it? Which `rogue_fwif_freelist`
  fields does the firmware update, and how does `state` move (`FRAG_FINISHED`)?

#### Q21 — Sources of the 3D registers (answered in `kick-compute-and-3d.md`; implemented E038–E040)

- **"Render target state":** where do `BCAST[0x3E40]`, `BCAST[0x0F18]`, `BCAST[0x0F28]`,
  the ZS pair `BCAST[0x0FA8/0x0FAC]`, and the 64-bit `BCAST[0x0F20/0x0F24]` of phase B come
  from? Fields of `rogue_fwif_hwrtdata(_common)` (for example `rgn_header_dev_addr` or
  `macrotile_array_dev_addr` for `0x0F20`), or something else?
- `BCAST[0x40A0]` (byte) = `cmd+0x7C`. That is `rogue_fwif_frag_regs.view_idx` (regs + 68).
  Correct?
- The remaining `rogue_fwif_frag_regs` fields the table does not place: `isp_bgobjdepth`,
  `isp_bgobjvals`, `isp_aa`, `isp_ctl`, `tpu`, `pixel_phantom`, `isp_scissor_base`,
  `isp_dbias_base`, `isp_oclqry_base`, `isp_zlsctl`, `isp_zload_store_base`,
  `isp_stencil_load_store_base`, `tpu_border_colour_table`, `pds_bgnd[3]`, `pds_pr_bgnd[3]`,
  `isp_dummy_stencil/depth_store_base`, `rgnhdr_single_rt_size`, `rgnhdr_scratch_offset`.
  Which register receives each, and is any of them conditional?
  - The phase-C `cmd+0x98…+0xA4` → `0x0FB0…0x0FBC` looks like `isp_zlsctl` and
    `isp_zload_store_base`. Please confirm.
- `rogue_fwif_cmd_frag.flags` (`DEPTHBUFFER`, `STENCILBUFFER`, `GET_VIS_RESULTS`,
  `SCRATCHBUFFER`, `PREVENT_CDM_OVERLAP`…), `zls_stride`, `sls_stride`: what does each
  change in the kick?
- Phase F says a normal 3D kick writes `CR[0x0298] = 0` and `CR[0x01C8] = 0`. Where in
  the sequence?

#### Q22 — Combined geometry + fragment kick and interlocks (answered, see the Answered table)

- For KCCB `COMBINED_GEOM_FRAG_KICK` (117), and for separate kicks on one render context:
  must the fragment wait for the TA of the same HWRTData to finish (state
  `GEOM_FINISHED`)? Is that wait done through the cCCB fences, or by the firmware itself?
- With DM overlap enabled (`INICFG_DISABLE_DM_OVERLAP` clear on this GPU), which DMs may
  run at the same time (TA + 3D of different renders, 3D + compute)? What does
  `ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP` enforce?

### C. Parameter buffer exhaustion (SPM) (2026-11-09; answered)

#### Q23 — Out-of-memory, free-list grow, partial render, ZS backing (answered in `partial-render-spm.md`, Q28, Q30; implemented E037–E049 except on-demand ZS backing)

`partial-render-spm.md` gives the protocol but no registers:
- **Detecting OOM:** `PM_OUT_OF_MEMORY` (bit 7, `MTS_DM2`). How does the firmware tell
  which free list (local or global) ran out, and read how many pages were used?
- **Using the ready pages:** which PM registers are changed to hand `ready_pages` to the
  TA, and how is the TA resumed (a register write, or a new kick)?
- **Grow:** after KCCB `FREELIST_GROW_UPDATE` (`delta_pages`, `new_pages`, `ready_pages`),
  which PM registers change before resuming?
- **Partial render** (`FRAG_PR`): how does it differ from a normal 3D kick (`pds_pr_bgnd`,
  `pr_buffer_fw_addr[]`, no ZS load/store)? How is the TA restarted afterwards, and which
  HWRTData fields are updated (`HWRTDATA_PARTIAL_RENDERED`, state `PARTIALRENDERFINISHED`)?
- **ZS backing:** when does the firmware send `ZSBUFFER_BACKING`, and what does it do on
  `ZSBUFFER_BACKING_UPDATE`?
- **Free-list reconstruction after HWR:** what does the firmware compute from
  `freelist_hwr_snapshot[]` and send in `FREELISTS_RECONSTRUCTION`, and what does it do
  on the reply?

### D. Preemption (2026-11-09; answered, not implemented)

#### Q24 — Context store and resume (CDM, TA, 3D) (answered in `scheduler.md` and `kick-compute-and-3d.md`; not implemented: `steps.md` phase 8, optional)

- **When** does the firmware decide to store a running context (priority of a newly
  ready context, deadline, host request)?
- **Store sequence per DM:**
  - 3D: `BCAST[0x1000] = 1` while `(ISP_RENDER & 3) != 3`;
  - CDM: through `CDM_CONTEXT_STORE_STATUS` `0x04A0` and the `cdm_terminate_pds`
    programs?
  - TA: through the VDM store tasks in `geom_registers_caswitch`?

  Which event or poll says the store is complete? Where is the state saved
  (`context_state_addr`, buffer A/B with `ROGUE_FWIF_CTX_USING_BUFFER_A/B`)?
- **Resume sequence per DM:** for 3D the spec gives `CR[0x1020]`, `ISP_RENDER |= 0x10`,
  then `CR[0x0298]`/`CR[0x01C8]`. Please give the CDM and TA equivalents and how
  `fw_com_ctx_flags` (`NEED_RESUME`) is maintained.

## Answered

| Q | Answer | Where |
|---|---|---|
| Q29 / Q29b second HWRTData | PM-MMU invalidation (SLC `0x10`, `BIF_CTRL_INVAL = 4`, `0x1348`) at every first kick, HWRTData switch and 3D finished; first-kick `0x0258` + PM reset pulses `0x0290`/`0x0198`; full HWR teardown. **Implemented: `pre-3d`, `pre-ta` pass.** | `docsClean/kick-geometry-ta.md`, `hardware-recovery.md`; [E043](logRefs/E043-pm-mmu-inval-first-kick-reset-hwr.md) |
| Q19b TPC flush, geometry flush, cache zeroing | TA finished: `0x0260` VHEAP store, `0x0C50 = 0x40000000` + poll, RTC flush `0x0CB8 = 2`; full DM 2 flush column; zeroing via CPU + `0x0258`/`0x0250`. **Implemented** (CPU zeroing replaced by an SLC flush). | `docsClean/kick-geometry-ta.md`; [E042](logRefs/E042-spec-fab1fc9-tpc-oom-init.md) |
| Q25 NPOT copies | GPU-init step 9 TPU/PBE/ISP defaults (`0x17B8` mode, `0x1780`, …). **Implemented: `copy_and_blit` 23/23.** | `docsClean/boot-gpu-init.md`; [E042](logRefs/E042-spec-fab1fc9-tpc-oom-init.md) |
| Q28 OOM resume | `0x0328 = 1` after the paused update; stop with `0x0320`/`0x0CF0`, resume with `0x0328` + `0x0CF8`; PR restart sequence; always the global list. **Implemented: `bigpb` frame 0 completes.** | `docsClean/partial-render-spm.md`; [E042](logRefs/E042-spec-fab1fc9-tpc-oom-init.md) |
| Q1 MTS init / event routing | `MTS_DMn_INTERRUPT_ENABLE` (DM0 `0x89200`, DM2 `0xA0`, DM3 `0x08`, DM4 `0x04`), `EVENT_ENABLE = 0`, `INTCTX = 0xFFFF`, `BGCTX = 1`. **Implemented**, but see Q9. | `docsClean/boot-gpu-init.md` B/C; [E011](logRefs/E011-event-enable-0x0128.md) |
| Q2 device info | values of the open image (SLC 16 KiB, 6 ERNs, `LAYOUT_MARS`, `XE_MEMORY_HIERARCHY`, …). **Implemented** in `tools/mkfw.py`. | `docsClean/memory-model.md`; [E001](logRefs/E001-sys-bus-secure-reset.md) |
| Q3 remap order | low word, high word, read back the high word. **Implemented.** | `docsClean/memory-model.md` |
| Q4 geometry register set | `rogue_fwif_geom_regs` field → `CR[set+0xC..]` mapping | `docsClean/kick-geometry-ta.md` (not implemented yet) |
| Q5 3D start | RMW `ISP_RENDER \| 0x10`, readiness `(ISP_RENDER & 3) == 3`. Tried for TQ, see Q8. | `docsClean/kick-compute-and-3d.md` |
| Q6 dust power | firmware-controlled: `0x0050` mask, `0x0038` request plus `POWER_COMPLETE` poll. **Implemented** at boot; per job only with `POW_RASCALDUST`, which mainline does not set. | `docsClean/boot-gpu-init.md` D |
| Q8 TQ_3D start | second launch write `BCAST[0x0F00] = 1`, mandatory BIF gate, `0x06B0` not written, completion on `PIXELBE_END_RENDER`. **Implemented: the pipe starts.** Output is still wrong, see Q12. | `docsClean/kick-compute-and-3d.md`; [E017](logRefs/E017-tq3d-launch-fault-at-zero.md) |
| Q9 IRQ task | section-B routing is enough; end with `0x0B08 = 2` and a read-back; no "which DM" register. **Confirmed**: HW2 fires on every completion; E016 was a race. | `docsClean/boot-gpu-init.md` B′; [E018](logRefs/E018-irq-task-delivery.md) |
| Q10 zero workgroups | no special case: one kick per command. **Confirmed**: `empty_workgroup_x/y/z` pass once no stuck job is left. | `docsClean/kick-compute-and-3d.md`; [E019](logRefs/E019-hwr-and-compute-basic-67.md) |
| Q12 TQ address 0 | PBE two banks, `0x0FD8`, conditional `0x0F20`. **Implemented**, but the fault remains: see Q13. | `docsClean/kick-compute-and-3d.md`; [E020](logRefs/E020-tq3d-pbe-two-banks.md) |
| Q13 TQ destination | the destination is in the end-of-tile program data; complete table with conditions. **Implemented**: no more address-0 fault, TQs complete. See Q14. | `docsClean/kick-compute-and-3d.md`; [E022](logRefs/E022-tq3d-completes-first-tile-lost.md) |
| Q14 first tile lost | launch = `ISP_RENDER = isp_render` then `0x0F00`, with `\|= 0x10` only on resume; per-DM cache flush before retiring. **Implemented: no failures in five groups.** | `docsClean/kick-compute-and-3d.md`; [E023](logRefs/E023-tq-launch-order-and-dm-flush.md) |
| Q15 register set | no per-set addressing: fixed offsets in the geometry core's bank. Trace "register set N" = `CAT_BASE{N+1}`. **Used in E028.** | `docsClean/kick-geometry-ta.md` |
| Q22 geometry → fragment | a `FENCE_PR` in the fragment cCCB on the geometry timeline UFO, both started by `COMBINED_GEOM_FRAG_KICK`. The extra HWRTData-state hold is still `TO CONFIRM`. | `docsClean/client-ccb.md` |
| Q25 transfer fields | the Q13 table is the whole kick; the remaining failures are host-side | `docsClean/kick-compute-and-3d.md` |
| Q26 KCCB no-ops | the mainline host never sends them; consuming them with `CMD_EXECUTED` is enough | `docsClean/kernel-ccb.md` |
| Q11 HWR reset | SLC idle → partial renders off → SOFT_RESET pulse `0x13FFFC1D/0xFFEFFE00` → units re-init and BIF rebuild → discard → resume. **Implemented and working** (~0.28 ms). | `docsClean/hardware-recovery.md`; [E019](logRefs/E019-hwr-and-compute-basic-67.md) |
| Q7 GPU units init, exec bases | PDS `0x0610` (found by experiment, [E009](logRefs/E009-pds-exec-base-0x0610.md)), USC `0x4008/0x4010/0x4028`; the full section-A sequence. `0x0FC8` (E012) is an ISP register. **Implemented: compute works.** | `docsClean/boot-gpu-init.md` A; [E013](logRefs/E013-gpu-units-init-compute-pass.md) |
