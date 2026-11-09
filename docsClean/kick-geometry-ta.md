# Geometry (TA) kick — registers and ordering dependencies

Details the **geometry kick** (DM 2/6) at register level: **which registers must be
programmed**, **with what data**, and **which ordering dependencies are mandatory**. These
are hardware/behaviour facts, not a routine to copy: the code organisation is free as long
as the dependencies are respected. Complements [data-masters](data-masters.md) and
[command-structs](command-structs.md).

## Register notation

- **`CR[off]`**: register at offset `off` in a **core's** bank, in the window
  `0xCF800000 + core·0x80000` (see [memory-model](memory-model.md)).
- **`BCAST[off]`**: broadcast write (all enabled cores, bank `0xCFA00000`).

> **On the `CR[set+off]` notation used below (answers Q15).** The geometry registers below
> are at **fixed offsets** (`0xC00`, `0xC88`, `0x400`, …) in the bank of the core that runs
> the geometry (the **primary** core for DM 2; the second core of the active list for DM 6).
> There is **no** extra per-"set" displacement or stride beyond the per-core bank stride
> (`0x80000`): on this single-core SoC every such write lands in the primary core's bank.
> Read `CR[set+0xC00]` below as "offset `0xC00` in the geometry core's bank". The trace
> `Setup register set=N` reports the **BIF page-catalogue slot** chosen by the
> memory-context activation, not an addressing base for these registers. Observed (**O**):
> after `Setup register set=0 DM=2`, the context's page catalogue is in
> `ROGUE_CR_BIF_CAT_BASE1` and `ROGUE_CR_BIF_CAT_BASE_INDEX = 0x101` (`TA` and `PIXEL`
> fields = 1). So trace set *N* means catalogue register `CAT_BASE{N+1}` and index-field
> value *N+1*; `CAT_BASE0` belongs to the firmware. Compute and transfer bind the same way through
> `ROGUE_CR_BIF_CAT_BASE_INDEX` and do not use a separate addressing base either.
- Public names, where they exist, are from `pvr_rogue_cr_defs.h`. The public header names
  only a subset of the registers; the others are given by offset.

> **Provenance.** Offsets and dependencies: **D** (firmware analysis), confirmed by **O**
> (the milestones appear in the trace: `Activate MemCtx`, `Setup register set`, `Kick TA`).
> Values come from `rogue_fwif_cmd_geom` (public header) and from the context state.

## Inputs

The context (`rogue_fwif_fwcommoncontext`: memory context, register set), the geometry
command (`rogue_fwif_cmd_geom`, right after the 0x28-byte header in the cCCB) and the
command offset (used in the trace entry and on retirement).

## Phases and dependencies

The phases below have a **mandatory order**: each depends on the previous one.

### A. Preconditions

- The power domain must be **on** (wake the GPU if needed).
- On the first kick after a GPU-units initialisation, if the context's `rf_cmd_addr` is
  non-zero, the firmware first runs a compute job from it (see the transfer kick in
  [kick-compute-and-3d](kick-compute-and-3d.md)). The mainline host leaves it 0.
- The **memory context** is bound to a `ROGUE_CR_BIF_CAT_BASE` slot (1–7), which selects
  the **register set** to use. Trace: BIF id 14, `Activate MemCtx=… DM=… secure=…`. Must
  happen before any write to the register set.

> **Observation run (O) behind phases B, C and E.** Black-box observation on the board of
> one first-kick geometry job (`FIRSTKICK | LASTKICK`, from
> `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list`, 256×256 target). The
> method compares the values in the geometry command (read from the cCCB while the job was
> live) and the addresses printed by the public trace with a read-only snapshot of the
> geometry core's registers, taken while the GPU was held powered. Firmware: the open image
> **v1.1, build 6976702** (the one this spec describes); the same mapping was also
> observed with the open image v1.0, build 6734358. Board DRAM at 1066 MT/s.

### B. Load the render target into the PM and synchronise

**The two PM contexts (O, public trace).** Geometry uses **PM context 0** (trace "HW
context 0", "PMDM0"). Fragment uses **PM context 1** ("HW context 1", "PMDM1"). The observed
order is:

1. **Before the geometry kick:** load the HWRTData's **local** and **global** freelists into
   PM context 0. Trace PM: `Load  Freelist … type: 0 (0:local,1:global) for PMDM0: FL Total
   Pages …, FL-addr = …, stacktop = …, Alloc Page Count = …`, then the same for type 1.
2. Load the HWRTData on HW context 0: RTD id 15, `TA RTData … loaded on HW context 0`.
3. **Before the fragment kick of the same HWRTData:**
   - **store** the freelists from PM context 0 (`Store Freelist … for PMDM0`). This is
     where the freelist state left by the geometry is captured; the trace shows
     `stacktop` 0x1F → 0x10 and `Alloc Page Count` 0 → 6.
   - **load** the freelists into PM context 1 (`Load Freelist … for PMDM1`).
   - load the 3D stack pointers (RTD id 12, `Loading stack-pointers for 1 (0:MidTA,1:3D) on
     context 1, MLIST = …, ALIST = …`).
   - `3D RTData … ready on HW context 1`.
4. **On HWRTData cleanup:** store the freelists back from both PM contexts.

**Registers observed holding the loaded PM state (O):**

| Register | Observed value | Matches |
|---|---|---|
| `0x0248`/`0x024C` | `0x80_0013_A000` | `rogue_fwif_hwrtdata.vheap_table_dev_addr` (trace "VHeap") |
| `0x0210`/`0x0214`, `0x0218`/`0x021C` | `0x80_0013_9000` (both) | local freelist `freelist_dev_addr` (trace "FL-addr"), one register pair per PM context (`TO CONFIRM` which is which) |
| `0x0224` | `0x1F` | local freelist stack top at rest (32 pages, trace "stacktop") |
| `0x0270`, `0x0278`, `0x0280` | `0x2` | ALIST stack pointer (trace "ALIST = 0x2") |
| `0x02C0`, `0x02C8`, `0x02D0` | `0x9` | MLIST stack pointer (trace "MLIST = 0x9") |
| `ROGUE_CR_BIF_PM_CAT_BASE_{VCE,TE,ALIST}0` and `…1` | the same three values in both banks | page-catalogue entries of the HWRTData, bank 0 = PM context 0, bank 1 = PM context 1 |

The global freelist's address (`0x80_0004_8900` in this run) was not found in the sampled
ranges. How the 4-entry arrays `vce_cat_base[4]`/`te_cat_base[4]` map to the single entry
per bank is `TO CONFIRM`.

#### PM freelist registers (answers Q16b, O)

Two observation runs on the open v1.1 image:
- the draw above;
- `bigpb`: a 1024×1024 target with millions of triangles, out-of-memory, freelist grows and
  partial renders.

In both, register snapshots were compared with the values printed by the public
`Load`/`Store Freelist` trace lines. Results:

| Register | Holds | Evidence |
|---|---|---|
| `0x0210`/`0x0214` | **local** freelist base (`freelist_dev_addr`) of **PM context 0** | changes at the geometry load; equals the trace "FL-addr" |
| `0x0218`/`0x021C` | the same for **PM context 1** | changes only at the fragment load, to the same address |
| `0x0224` | local freelist **stack top** | `0x1F` at rest (32 pages → total − 1), `0` when exhausted; equals the trace "stacktop" |
| `0x0398` | local freelist **allocated page count** | `6` and `20` in the two runs = trace "Alloc Page Count" |
| `0x039C` | local freelist **allocated MMU page count** | `9` and `11` = trace "Alloc MMU Page Count" |
| `0x02D8`/`0x02DC` (PM context 0), `0x02E0`/`0x02E4` (PM context 1) | a per-HWRTData address | alternates between the two HWRTData of `bigpb` at each geometry kick, and is copied to the context 1 pair at the fragment kick. Consistent with `pm_mlist_dev_addr` (`TO CONFIRM`: the HWRTData contents could not be read to match the value) |
| `0x0270/0x0278/0x0280`, `0x02C0/0x02C8/0x02D0` | ALIST / MLIST stack pointers | the first register of each triple follows the geometry, the other two take its value at the fragment kick |

Other answers:
- **`0x0220`** read back as **0** in every snapshot of the original firmware, including
  while freelists were loaded. Whether its low field is the total page count is
  `TO CONFIRM`; the observed stack top is at `0x0224`.
- **No register in `0x0200–0x043F` holds a value of the global freelist** (address, total,
  stack top, counts), although these values changed many times during `bigpb`. The global
  freelist's PM registers are outside the sampled range (`PENDING`).
- **`PM_CAT_BASE_*` on a first kick:** bank 0 (`…0`) receives new, valid (bit 0 = 1)
  entries at **every** geometry kick, including first kicks where the host left the
  HWRTData catalogue bases at 0. Bank 1 (`…1`) takes the same values at the fragment kick.
  The PM fills bank 0 itself during the geometry, from the local freelist (see Q16f
  below). The second word of
  `VCE0`/`TE0` (`0x124C`/`0x1254`) becomes non-zero only around partial renders.
- **Other PM enables:** `ROGUE_CR_PM_PARTIAL_RENDER_ENABLE` (`0x0338`) read 0 in every
  snapshot, and `0x03E0` read 1 throughout. A separate PM "context select" or "start"
  register was not identified (`PENDING`).

#### PM allocation follow-up (answers Q16e, O)

A second `bigpb` run on the open v1.1 image sampled `0x0200–0x0800` and `0x2000–0x2200` at
every freelist, OOM, grow and kick event. The global freelist's base, size and stack pointer
change hundreds of times in that run, which makes matching possible.

**1. Global freelist registers.** They are in the `0x2000` block, as **two banks with a
stride of `0x40`**: PM context 0 at `0x2078`, PM context 1 at `0x20B8`. The trace lines used
are `UFL-TA-Base`/`UFL-3D-Base`, `Freelist update completed` and `OOM: Resumed TA`.

| Bank offset | PM ctx 0 | PM ctx 1 | Holds | Evidence |
|---|---|---|---|---|
| `+0x00`/`+0x04` | `0x2078`/`0x207C` | `0x20B8`/`0x20BC` | **global freelist base** (64-bit) | equals the trace base in 24 of 37 snapshots each; the misses are snapshots taken during a base change |
| `+0x08` | `0x2080` | `0x20C0` | not identified (small values: `0x20`, `0x108`, `0x109`) | — |
| `+0x10` | `0x2088` | `0x20C8` | **stack pointer** (live; the trace `SP = …`) | see the identity below; `0xFFFF` = 65535 with all 65536 pages free, `9` at the partial render where the trace prints `SP = 9` (corrected by Q16f) |
| `+0x18` | `0x2090` | `0x20D0` | PB page count at the last freelist update/store | equals the trace "4PB" at those points (11212 at a grow update), then holds its value |
| `+0x1C` | `0x2094` | `0x20D4` | MMU page count at the same points | equals the trace "4PT" (19, 26, 124…) |
| `+0x20` | `0x2098` | `0x20D8` | **allocated PB pages** (live) | `65400` at the partial render where the trace prints `4PB = 65400` |
| `+0x28` | `0x20A0` | `0x20E0` | **allocated MMU pages** (live) | `126` at the same point (`4PT = 126`) |

In every snapshot of two runs, for both banks:
**stack pointer + allocated PB pages + allocated MMU pages + 1 = total pages** of the
freelist (for example `0xAEA + 0x2102 + 0x13 + 1 = 11264`). The stack pointer counts down as
the PM hands out pages.

The **local** freelist has no copy in the `0x2000` block. Its registers are the
`0x0210`/`0x0218`, `0x0224` and `0x0398`/`0x039C` given above.

**2. Starting the PM.** `PM_PAGE_MANAGEOP` (`0x02A0`) read **0 in all 37 snapshots**,
including while the geometry was running and allocating. It goes to 1 only briefly, around
freelist updates (`PM pause/unpause TA ALLOC`, see
[partial-render-spm](partial-render-spm.md)). Nothing indicates that the firmware writes it
to start allocation on a first kick: 0 means "not paused".

The public trace prints `CONTEXT_PB_BASE set to 0x…` when freelists are loaded:
- `0x3` at the geometry load, together with `FL different between TA/3D: local: 1, global: 1`;
- `0x0` at the fragment load, when TA and 3D freelists are the same.

Register **`0x02B0`** held this same value in **73 of 75 snapshots** across three runs. It is
therefore the register behind `CONTEXT_PB_BASE` (`TO CONFIRM` the bit meaning: the observed
pairing suggests bit 0 = local and bit 1 = global freelist differ between the two PM
contexts).

**3. `0x0D00`.** With the original firmware the values seen were `0x30`, `0x34` and `0x35`
while geometry was running, `0x3F` after jobs, and 0/1/4/5 at other moments. **`0x0F` was
never observed**: in the original, bits 4 and 5 are set while the geometry runs. Which units
these bits stand for is not established.

**4. Write-ordered list.** Not provided. The complete ordered sequence of the original's
register writes would be a transcription of its kick routine, which the clean-room rules
exclude, and the observation cannot produce it anyway. What this spec gives is the
registers, their values and sources, and the ordering constraints the hardware imposes.
Registers that are written and then self-clear during the kick are, by construction,
invisible to these snapshots (`PENDING`).

#### PM page catalogues on a first kick (answers Q16f, O)

A third `bigpb` run on the open v1.1 image read, at each geometry kick, the `PM_CAT_BASE_*`
registers, both freelists' page stacks and the HWRTData. All reads were done from the host,
through the GPU page tables of the application's memory context. Six geometry kicks were
examined; they include first kicks of both HWRTData.

**1. Who fills `PM_CAT_BASE_*0`.** The **PM**, during the geometry, with pages it takes from
the **local** freelist. Evidence:
- At the first kick, the HWRTData's `vce_cat_base[]`, `te_cat_base[]` and `alist_cat_base`
  are all 0 (the host leaves them at 0).
- While that geometry runs, `PM_CAT_BASE_VCE0`, `TE0` and `ALIST0` are valid (bit 0 = 1).
  Their `ADDR` fields are pages of the local freelist's stack: entries **29, 18 and 13**,
  with the stack top loaded at 31 (trace `FL-TA-Base … SP = 31`).
- The positions were the **same in all six kicks**, although the stack's contents were in a
  different order each time (pages come back in a different order).
- The three pages are **not consecutive** stack entries: other pages were handed out between
  them. This is what on-demand allocation by the PM looks like, not a preamble that takes
  pages before the kick.

Nothing was found that the firmware writes to make this happen besides the freelist and PM
context registers already described (local and global freelist, `CONTEXT_PB_BASE`, VHeap,
MLIST). No separate enable was identified. Whether the firmware also writes
`PM_CAT_BASE_*0` (for example to 0) before a first kick is `TO CONFIRM`: such a write would
be overwritten before any snapshot.

Two corrections to the reading of the earlier answers:
- **`0x0224` does not move while the PM allocates.** In the original it stays `0x1F` during
  geometry, even after the PM has taken all 32 local pages (stores report `SP = 0`). It holds
  the stack top **as loaded**, not the live one. A stalled `0x0224 = 0x1F` is therefore not
  in itself the symptom. The live local stack pointer was not identified: it reaches 0
  quickly, and the snapshots only list non-zero registers.
- Bank 1 (`…1`) at a geometry kick still holds the **previous** HWRTData's catalogue pages,
  which may already have been freed and reused for other data.

**2. Relation to HWRTData and freelists.** The catalogue pages come from the **local
freelist**, not from the HWRTData's own buffers. In this run, `pm_mlist_dev_addr`,
`vheap_table_dev_addr`, `macrotile_array_dev_addr` and `rgn_header_dev_addr` translate to
bus pages `0x84E86000`/`0x84E87000`, `0x84E83000`, `0x84E84000` and
`0x84E88000`/`0x84E89800`. These are outside the freelist and differ from every
`PM_CAT_BASE` value.

Example from the first kick (bus addresses; freelist entries are page numbers = bus address
>> 12, as the host writes them):

| Item | Value |
|---|---|
| local freelist base / stack top at load | `0x80_00AB_D000` / 31 |
| local freelist entries 0…31 | `0x84E63`, `0x84E64`, … `0x84E82` (consecutive at the first kick) |
| global freelist base / stack top at load | `0x80_0004_8900` / 447 |
| `PM_CAT_BASE_VCE0` | `0x0000_0000_84E8_0001` = entry 29 |
| `PM_CAT_BASE_TE0` | `0x0000_0000_84E7_5001` = entry 18 |
| `PM_CAT_BASE_ALIST0` | `0x0000_0000_84E7_0001` = entry 13 |
| `VCE1`, `TE1`, `ALIST1` | 0 (no fragment yet) |

**What the catalogue pages contain.** While the geometry runs:
- The first 32-bit word of each catalogue page is `(page number << 4) | 1`. The page number is
  another page of the local freelist: `0x0084E7F1` in the `VCE0` page points to page
  `0x84E7F`.
- Further entries in later snapshots are 64-bit, `bus address | 0x11`, and point to pages
  **outside** the local freelist (`0x8F796011`, `0x96F85011`, …). These are consistent with
  parameter-buffer pages from the global freelist (`TO CONFIRM`).
- The `ALIST1` page holds a list of 64-bit `bus address | 1` entries for local-freelist
  pages.

The high word of `PM_CAT_BASE_*1` (the public `INIT_PAGE` field, bits 59:40) was non-zero
only after partial renders (`0x3EF4`, `0x7DE8` in `VCE1`; `0xDD`, `0x1BA` in `TE1`).

**3. Global freelist stack pointer:** `0x2088` (PM context 0) and `0x20C8` (PM context 1); see
the table under Q16e above. `0x2080` is not the stack pointer.

**4. `0x0D00` bits 4 and 5.** Not established. Observation only shows the following:
- At the first kick of this run, `0x0D00` read `0x04`, with the catalogue bases already
  valid; it then read `0x34`/`0x35` while the geometry kept running.
- The bits are therefore not a direct "PM has pages" indicator.

- **PM handshakes** (**D**, not observed directly):
  - `CR[0x250] = 1`, then **poll** `CR[0x250]` until bit 0 = 1;
  - `CR[0x258] = 1`, then **poll** `CR[0x258]` until bit 0 = 0.

  The polls must complete before phase C.

### C. Program the geometry registers (command data)

**Observed (O)**, for a first kick. Each command field is matched by value with the register
that held the same value:

| Register (geometry core bank) | Observed value | Source |
|---|---|---|
| `0x0C88` | `0x202` | `rogue_fwif_geom_regs.ppp_ctrl` |
| `0x0C28` | `0x0002_0000` | `rogue_fwif_geom_regs.te_psg` |
| `0x0C38`/`0x0C3C` | `0x80_0013_E000` | `rogue_fwif_hwrtdata.rgn_header_dev_addr` (trace "TE Region headers base") |
| `0x0C40`/`0x0C44` | `0x80_0013_B000` | `rogue_fwif_hwrtdata.tail_ptrs_dev_addr` (trace "TPC") |
| `0x0408`/`0x040C` | `0x80_0013_F000` | `rogue_fwif_geom_regs.vdm_ctrl_stream_base` |
| `0x17A0`/`0x17A4` | `0x20_0001_C400` | `rogue_fwif_geom_regs.tpu_border_colour_table` (the fragment kick writes the same register) |

The other non-zero values in the geometry bank are analysed in "Unmatched geometry
registers" below. `0x0410`/`0x0414` held `vdm_ctrl_stream_base` plus a small offset (`+0x40`,
`+0x3C`) after each job, which reads like the VDM's current position in the control stream
(`TO CONFIRM`).

#### Unmatched geometry registers (answers Q16c, O)

The `bigpb` run renders to a 1024×1024 target, against 256×256 for the draw. Comparing the
two separates the values that depend on the target size from the constant ones:

| Register | 256×256 | 1024×1024 | Reading |
|---|---|---|---|
| `0x0C98` | `0x00FF_00FF` | `0x03FF_03FF` | **x/y maximum in pixels** (width − 1, height − 1) |
| `0x0C18` | `0x0000_F00F` | `0x0003_F03F` | **x/y maximum in 16×16 tiles** (tiles − 1) |
| `0x0C08`, `0x0C10` | `0x0010_0000` | `0x0040_0000` | scales ×4 when each side grows ×4 (a per-row size or stride; `TO CONFIRM` which `hwrtdata_common` field) |
| `0x0C20` | `0x10` | `0x100` | scales ×16, with the area (`TO CONFIRM`) |
| `0x0C30`, `0x0C34` | `2`, `3` | same | constant |
| `0x0C48` | `8` | same | constant |
| `0x0C70` | `3` | same | constant |
| `0x0C78`, `0x0C80` | `0x88` | same | constant |
| `0x0C90`/`0x0C94` | `0x3089_705F` ×2 | same | constant 64-bit value (candidate: `multi_sample_ctl`, which is 64-bit and fixed per sample count; `TO CONFIRM`) |
| `0x0CA0` | `0x10` | same | constant |
| `0x0CE0` | `1` | same | constant |
| `0x0D00` | `0x3F` | 0…`0x3F`, varying | hardware status that changes during the job, not configuration |
| `0x0420` | `1` | 2, 3, … 9 | increments by one at each geometry kick (a running count) |
| `0x0418`/`0x041C` | `0x80_0006_D000` | same | **`rogue_fwif_geom_ctx_state_per_geom.geom_reg_vdm_call_stack_pointer_init`** of the context's `context_state_addr` buffer (equal by value) |

"Constant" means the value did not change across the two targets. The analysis does not
show whether the firmware writes it as a constant or copies it from a field that happened
to be equal in both runs.

`vdm_context_resume_task0_size` (`0x2001_0000` in this job) was **not** found in any
sampled register. It is probably used only when resuming a stored context (`TO CONFIRM`).
`tpu`, `pds_ctrl` and `view_idx` were 0 in this job, so their registers could not be
identified by value.

> **Correction.** Earlier versions of this phase had a table derived from firmware analysis
> that placed `te_psg` at `0xC18`, `tpu` at `0xC20`, `pds_ctrl` at `0xC10`, the resume sizes
> at `0xC00`/`0xC08`, `tpu_border_colour_table` at `0xC80`, the VDM control-stream base at
> `0xC88…0xC98`, and listed `0xC28`, `0xC38` and `0xC40` as context state or polls. Where
> that table and the observation disagree, the **observation above wins**: `0xC28` is
> `te_psg`, `0xC38`/`0xC40` are the region-header and TPC addresses, `0xC88` is `ppp_ctrl`,
> and the VDM control-stream base is at `0x0408`. Rows that are neither confirmed nor
> contradicted are dropped. Trace: BIF id 19 `Setup register set=… DM=…`, RTD id 25/33
> `TA Buffers: …`, RTD id 24 `TE Region headers base …`.

### D. Context-switch state and lists

- A **new kick** and a **resume** (after preemption) program the TA *state buffer* and the
  stack pointers differently: `CR[set+0xCE0]`, `CR[set+0xCB0]` (poll),
  `CR[set+0xCB8] = 1`, `CR[set+0xC48]`, and `CR[0xD20]` = state buffer address (0 if
  none).
- On **resume**, the TA state buffer is **flipped** relative to the last store.
- After an **HWR**, the RTC cache and the TA caches are cleared before the kick
  (`rogue_fwif_hwrtdata.geom_caches_need_zeroing`).
- `CR[0x40A8] = (primary core) & 1` (core/TE selection).

**Context-state registers (Q16d, O — partial).** For the draw and `bigpb` runs, the
render context's `static_render_context_state.ctxswitch_regs[0]` (at render-context offset
`0x130`, public layout) held:
- `geom_reg_vdm_context_state_base_addr = 0x80_0006_E000`;
- `geom_reg_ta_context_state_base_addr = 0x80_0006_F000`;
- the `geom_state[]` store/resume task words.

**None of these values was found in any register sampled after a job.** `0x0CB0`, `0x0CB8`,
`0x0428` and `0x0D20` read 0, and `0x0CE0` read 1. So the observation neither confirms nor
contradicts `0x0CB0 = geom_reg_ta_context_state_base_addr` or `0x0428 =
geom_reg_vdm_context_state_base_addr`: either these registers are only meaningful during the
job, or they are cleared at its end. Settling it needs a view of the writes during the kick
(`PENDING`).

### E. Enable the context and broadcast the configuration

- `CR[set+0xC68] = 1`, then **poll** `CR[set+0xC68]` until bit 0 = 1 (context
  acceptance). Must complete before the trigger.
- `0x0408`/`0x040C` receive the VDM control-stream base (`vdm_ctrl_stream_base`, observed
  in phase C), not a freelist/ALIST configuration as earlier versions said. The pair
  `0x0418`/`0x041C` holds `geom_reg_vdm_call_stack_pointer_init` (see Q16c above).

### F. Trace, timing and watchdog (before the trigger)

- Trace entry MAIN id 119: `Kick TA: FWCtx … @ …, RTD …, First kick:…, Last kick:…, CSW
  resume:…. (PID:…, prio:…, frame:…, ext:…, int:…)`.
- Read `ROGUE_CR_TIMER` (`0x0160`), arm the DM's **deadline watchdog** (see
  [hardware-recovery](hardware-recovery.md)) and record the DM's execution state
  (context, offset, kick time, deadline).

### G. Trigger

- **`CR[set+0x400] = 1`**: the TA *run* register. It is **the last write**: everything
  above must be programmed and accepted before it.

## Completion

The GPU signals in `ROGUE_CR_EVENT_STATUS` (HW2/IRQ). Geometry completion handling (trace
MAIN id 8, `TA finished`) updates the HWRTData and freelist, flushes the TPC, retires the
command and processes the updates (see [client-ccb](client-ccb.md) and
[scheduler](scheduler.md)).

**Which events reach the firmware (part of Q19).** The boot routing for DM 2
(`ROGUE_CR_MTS_DM2_INTERRUPT_ENABLE = 0xA0`, see [boot-gpu-init](boot-gpu-init.md)) enables
only `TA_FINISHED` (bit 5) and `PM_OUT_OF_MEMORY` (bit 7). `TA_TERMINATE` (bit 6, public
name) is **not** routed to the IRQ task, so a firmware that follows this routing completes
geometry on `TA_FINISHED` alone. When the hardware raises `TA_TERMINATE` is not
established by this spec.

**Freelist write-back (part of Q19, O).** The freelist state reached by the geometry is
captured when the freelists are **stored from PM context 0 at the fragment kick** of the same
HWRTData (phase B, step 3), not at `TA finished`. The fields involved are the ones the trace
prints: stack top, current pages, allocated page count. Geometry completion itself is traced
as `TA finished` then `TA RTData … finished on HW context 0`.

`PENDING`: the registers the PM state is read back from, the HWRTData fields updated after
the TA, the context registers of phase D, and the full geometry variant of the completion
cache flush.
## Confidence and pending items

- **High confidence:** the phases and their dependencies (preconditions → PM → register
  set → CSW → enable → watchdog → **trigger last**), the listed offsets and the mandatory
  polls. The `rogue_fwif_geom_regs` field → register mapping in Phase C (Q4) is resolved
  above.
- The VDM control pair (`CR[set+0xC88]`…`+0xC98`) mixes the host `vdm_ctrl_stream_base`
  with firmware context state; the exact split of the 64-bit base across `+0xC90`/`+0xC98`
  is a detail the implementer can finish from the public field widths.

The 3D, transfer and compute kicks follow the same skeleton. See
[kick-compute-and-3d](kick-compute-and-3d.md).
