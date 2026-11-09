<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
following the question flow in `docsClean/clean-room-process.md`. Until a question is
answered, the implementation uses the stated assumption.

## Open — what is still missing to finish the clean firmware

Status when these were written (2026-11-09): compute, transfers through the 3D pipe (TQ_3D),
power, HWR, statistics and the robustness notification all work on the board. Every
supported case passes in `compute.pipeline.basic`/`indirect_dispatch`/`builtin_var`,
`api.fill_and_update_buffer` and `api.copy_and_blit.core.buffer_to_buffer`. **Nothing can be
drawn yet:** the geometry (TA) and fragment (3D) kicks are missing, and so are the
parameter-buffer handling (PM, free lists, partial renders) and preemption.

The questions below list what is not in `docsClean/` and not in any public source. Field
names are those of the public `pvr_rogue_fwif*.h`. `CR[x]` is the primary-core bank and
`BCAST[x]` the broadcast bank, as in the spec. They are ordered by how much they block.

### A. Geometry (TA) kick — blocks all rendering

> **2026-11-09, after the partial answer.** Q15, Q22 (fence), Q25 and Q26 are answered
> (see the "Answered" table). Q16–Q21 are partly answered by the spec's observation. E027
> and E028 filled in part of the rest by experiment, and the follow-up questions are
> **Q16b–Q16d** below. Q19 (full geometry flush), Q20, Q21 (ZLS, the remaining
> `frag_regs`), Q23 and Q24 are still fully open.

#### Q16j — The exact register writes from `Activate MemCtx` to `Load Freelist` (blocking)

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

#### Q16i — What action copies the loaded free list into the PM's live stack pointers? (answered in part, see E034)

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

#### Q16g — How does the firmware load the free lists into the PM's live state? (blocking)

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

#### Q16f — PM page catalogues on a first kick (blocking)

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

#### Q16e — The PM still allocates nothing (blocking)

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

#### Q16b — PM free-list registers (answered in part; see Q16e)

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

#### Q16c — geometry registers still unmatched

After a working geometry job the spec saw `0x0C30/0x0C34 = 2/3`, `0x0C48 = 8`,
`0x0C70 = 3`, `0x0C90` (64-bit) = `0x3089705F`, `0x0CA0 = 0x10`, `0x0CE0 = 1` and
`0x0D00 = 0x3F`. Which of these are written by the firmware, and with what (constants,
command fields, `hwrtdata_common` fields)? E027 matched the others by value (`0x0C98`,
`0x0C18`, `0x0C08`, `0x0C10`, `0x0C20`, `0x0C78`, `0x0C80`, `0x0418`). Please confirm those.

#### Q16d — context-state registers

[E028](logRefs/E028-ta-kick-bringup.md) found by experiment that the hardware reads
`[0x0CB0] + 0x200` when the geometry context is enabled, and that
`0x0CB0 = geom_reg_ta_context_state_base_addr` removes the fault. Please confirm, and give:
- the register for `geom_reg_vdm_context_state_base_addr` (`0x0428` is a candidate);
- the meaning of `0x0CE0`, `0x0CB8`, `0x0C48` and `0x0D20` for a new kick.

#### Q15 — What is a "register set"?

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

#### Q16 — Loading the HWRTData into the PM, geometry phase

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

#### Q17 — Register-set values that come from state, not from the command

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

#### Q18 — TA state buffer and context registers for a new (non-resumed) kick

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

#### Q19 — TA completion

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

### B. Fragment (3D) kick

#### Q20 — Loading the HWRTData into the PM, fragment phase, and PM deallocation

Phase A says "Load the HWRTData into the PM (fragment phase)", and "pause the PM
deallocation" for a partial render.
- Which registers and fields does the fragment-phase load write (MLIST, region headers,
  macrotile array, free lists for deallocation)?
- How is "pause the PM deallocation" done?
- **After the 3D:** how are the PB pages returned to the free list? Is it automatic
  (event `PM_3D_MEM_FREE`, bit 8), or does the firmware drive it? Which `rogue_fwif_freelist`
  fields does the firmware update, and how does `state` move (`FRAG_FINISHED`)?

#### Q21 — Sources of the 3D registers

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

#### Q22 — Combined geometry + fragment kick and interlocks

- For KCCB `COMBINED_GEOM_FRAG_KICK` (117), and for separate kicks on one render context:
  must the fragment wait for the TA of the same HWRTData to finish (state
  `GEOM_FINISHED`)? Is that wait done through the cCCB fences, or by the firmware itself?
- With DM overlap enabled (`INICFG_DISABLE_DM_OVERLAP` clear on this GPU), which DMs may
  run at the same time (TA + 3D of different renders, 3D + compute)? What does
  `ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP` enforce?

### C. Parameter buffer exhaustion (SPM)

#### Q23 — Out-of-memory, free-list grow, partial render, ZS backing

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

### D. Preemption

#### Q24 — Context store and resume (CDM, TA, 3D)

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

### E. Smaller items

#### Q25 — Transfer cases that still fail

These cases fail deterministically, and adding the full DM 3 flush before every TQ does
not change them ([E026](logRefs/E026-slcflush-copy-and-blit-stability.md)):
- `api.copy_and_blit.core.buffer_to_buffer_with_offset.*`: all 64, with off-by-one byte
  values;
- `image_to_image.simple_tests.partial_image_npot_*`;
- `image_to_buffer…buffer_offset_relaxed_r8_unorm`, `whole_unaligned`;
- `blit_image.simple_tests.mirror_subregions.*`.

Does the firmware use any `rogue_fwif_cmd_transfer` field that ours ignores? Candidates:
- `flags` bits other than `SINGLE_CORE`;
- `isp_xtp_pipe_enable`;
- the third PBE word per render target;
- a different number of render targets.

Or is a transfer kick fully described by the Q13 table, so that these failures come from
the host side?

#### Q26 — KCCB commands still treated as no-ops

What must the firmware do for:
- `NOTIFY_WRITE_OFFSET_UPDATE` (114): resume a context whose cCCB grew without a KICK?
- `OS_ONLINE_STATE_CONFIGURE` (118);
- `REGCONFIG` (200), `BP` (103, shader breakpoints);
- `PHR_CFG` (213), `WDG_CFG` (215)?

The mainline host may not send them today, but a complete firmware should handle them.

## Answered

| Q | Answer | Where |
|---|---|---|
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
