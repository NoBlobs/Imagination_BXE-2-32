# Compute (CDM), fragment (3D) and transfer (TQ) kicks — registers and ordering

Details the **compute**, **fragment** and **transfer** kicks in the same format as the
[geometry (TA) kick](kick-geometry-ta.md): registers to program, data sources and mandatory
ordering dependencies. The code organisation is free.

Notation: **`CR[off]`** = primary-core register; **`BCAST[off]`** = broadcast write (all
cores, bank `0xCFA00000`); `ROGUE_CR_*` names from `pvr_rogue_cr_defs.h` where public
(the header names only a subset; the rest are given by offset). **`cmd+0xNN`** = offset from
the **start of the command header** (payload offset = `0xNN − 0x28`, see
[command-structs](command-structs.md)).

> **Provenance.** **D** (firmware analysis) confirmed by **O** (trace milestones:
> `Activate MemCtx`, `Kick Compute`, `Kick 3D`, `Kick 3D TQ`). Field names given below come
> from the public check headers (**P**).

Compute and 3D program almost everything through **broadcast**, with data from the command
(`rogue_fwif_cmd_compute`, `rogue_fwif_cmd_frag`, `rogue_fwif_cmd_transfer`, whose `regs`
blocks are prepared by the host) and from the context.

---

## Compute kick (CDM / DM 4)

### A. Preconditions

- Power domain **on**. On the first kick after a GPU-units initialisation, the
  `rf_cmd_addr` job runs first if it is non-zero (see "Pre-kick firmware job" in the
  transfer kick; a no-op on mainline).
- **Multicore control**: read `ROGUE_CR_MULTICORE_SYSTEM` (`0xF308`) and write
  `BCAST[ROGUE_CR_MULTICORE_COMPUTE_CTRL_COMMON]` (`0xF330`).
- **Memory context** bound to a BIF slot (DM 4). Whether compute may overlap other DMs is
  decided here (see overlap rules in [scheduler](scheduler.md)).

### B. Command parameters (broadcast)

| Register | Data |
|---|---|
| `BCAST[0x0480]`, `BCAST[0x0484]` | `cmd+0x50/0x54` = `regs.cdm_ctrl_stream_base` (64-bit) |
| `BCAST[0x17A8]`, `BCAST[0x17AC]` | `cmd+0x30/0x34` = `regs.tpu_border_colour_table` (64-bit) |
| `BCAST[0x0498]`, `BCAST[0x049C]` | `cmd+0x58/0x5C` = `regs.cdm_context_state_base_addr` (64-bit) |

### C. Context-switch resume (only after preemption)

Reloads the saved state from the *snapshot buffer*: address and size go to the context's
register set, core by core. Trace: CSW id 48 `Multicore context resume on DM…`, CSW id 3
`CDM FWCtx shared alloc size load …`, CSW id 29 `*** CDM FWCtx … resume from snapshot
buffer …, shader state …`.

### D. Trace and watchdog

- Trace MAIN id 122: `Kick Compute: FWCtx … @ …. (PID:…, prio:…, ext:…, int:…)`.
- Read `ROGUE_CR_TIMER`, arm the deadline watchdog and record the execution state of DM 4.
- `ROGUE_CR_MTS_SCHEDULE = 0` to reschedule the BG task.

### E. CDM PDS programs

The values come from the compute context's context-switch register block
(`rogue_fwif_cdm_registers_cswitch`, which follows the 0x98-byte common context; host
prepared):

| Register | Data |
|---|---|
| `BCAST[ROGUE_CR_CDM_CONTEXT_PDS0]` (`0x04A8`), `+4` | `cdmreg_cdm_context_pds0` (64-bit) |
| `BCAST[ROGUE_CR_CDM_CONTEXT_PDS1]` (`0x04B0`) | `cdmreg_cdm_context_pds1` |
| `BCAST[ROGUE_CR_CDM_TERMINATE_PDS]` (`0x04B8`), `+4` | `cdmreg_cdm_terminate_pds` |
| `BCAST[ROGUE_CR_CDM_TERMINATE_PDS1]` (`0x04C0`) | `cdmreg_cdm_terminate_pds1` |

### F. Trigger

- **`BCAST[0x0478] = 1`**: the CDM *run* register (no public name). Last write; A–E must
  be complete.

### Which `rogue_fwif_compute_regs` fields are used (answers Q7)

On this GPU (features/BRNs from the [device-info block](memory-model.md#device-information-block)),
only the always-present fields are programmed:

- **Used:** `tpu_border_colour_table` (B), `cdm_ctrl_stream_base` (B),
  `cdm_context_state_base_addr` (B), and the CDM PDS registers from the context's
  `rogue_fwif_cdm_registers_cswitch` (E), including `cdm_resume_pds1`.
- **Not programmed** (their gating feature/BRN is absent, so the host leaves them unused):
  `tpu` (needs BRN 49927), `cdm_item` (needs `COMPUTE_MORTON_CAPABLE`), `compute_cluster`
  (needs `CLUSTER_GROUPING`), `tpu_tag_cdm_ctrl` (needs `TPU_DM_GLOBAL_REGISTERS`), and the
  `cdm_cb*` user-mode-queue fields (need `CDM_USER_MODE_QUEUE`). A reimplementation for this
  GPU can ignore them.

> **Prerequisite:** the PDS/USC execution bases must have been programmed at boot, or the
> CDM faults on its first PDS/USC fetch. See [boot-gpu-init](boot-gpu-init.md).

---

## Fragment kick (3D / DM 3)

The kick with the **most registers** (ISP, ZS and output configuration).

### A. Preconditions

- `BCAST[ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON]` (`0xF310`) = control value.
- Power domain on (and the `rf_cmd_addr` job on the first kick after a units init, as above).
- For a **partial render** with a pending deallocation, **pause the PM deallocation**
  first.
- **Memory context** bound to a BIF slot (DM 3).
- **Load the HWRTData into the PM** (fragment phase). If that fails, undo the memory
  context binding and abort the kick.

### B. ISP configuration (broadcast)

| Register | Data |
|---|---|
| `BCAST[0x4070]` | `usc_pixel_output_ctrl` (**O**: observed equal) |
| `BCAST[0x4078/0x4080/0x4088/0x4090]` | `usc_clear_register[0..3]` |
| `BCAST[0x40A0]` (byte) | `view_idx` (`cmd+0x7C` = `regs+68`, public offset) |
| `BCAST[0x0F20/0x0F24]` | `rogue_fwif_hwrtdata.rgn_header_dev_addr` (**O**: observed equal to the trace "TE Region headers base") |
| `BCAST[0x3E40]`, `BCAST[0x0F18]` | render target state. Observed `0x3E40 = 0x00FF_00FF` (255/255 for a 256×256 target) and `0x0F18 = 0x0010_0010`; sources `TO CONFIRM` (`rogue_fwif_hwrtdata_common` candidates) |

### C. ZS load/store

A partial render has no ZS load/store.

- `BCAST[0x0FA8/0x0FAC]`: ZS pair (from the render target state);
- `BCAST[0x0FB0]`/`[0x0FB4]` = `isp_scissor_base` and `BCAST[0x0FB8]`/`[0x0FBC]` =
  `isp_dbias_base` (`cmd+0x98 … +0xA4`; **O**: both observed equal). These are **not**
  `isp_zlsctl`/`isp_zload_store_base`; the ZLS registers are `0x0F48`–`0x0F6C` (see
  "Observed during the render" below).
- `BCAST[0x0F80]` = `isp_bgobjdepth` (**O**); `BCAST[0x0F30]` = `isp_aa` (`cmd+0x64`);
  `BCAST[0x0F28]` = render target state (observed: 4 × `mtile_stride`, see Q16l below).
- `BCAST[0x17A0]`/`[0x17A4]` = `tpu_border_colour_table` (**O**; the same register the
  geometry kick writes).

> Source of the field names: their offsets in the public `rogue_fwif_frag_regs` (check
> header), plus a black-box observation of one fragment job. Fields not listed here are
> `PENDING`. The observation could not place them because the read-back transfer that
> follows a render overwrites part of the 3D registers.

### Observed during the render (answers Q16l, O)

Black-box runs of the board. The workloads were:
- `dEQP-VK.renderpasses.renderpass1.suballocation.formats.{d16_unorm,d32_sfloat,d24_unorm_s8_uint}.{load,clear}.draw`;
- `dEQP-VK.draw.renderpass.simple_draw.*` and `basic_draw.*`;
- `vkfwtest bigpb` and `xfer`.

All passed, and there was no recovery. Registers were read from the host, with no writes:
- full-range snapshots at each `Kick 3D`;
- tight sampling (≤ 10 µs) of `0x0F00–0x0FFC`, `0x1700–0x17FC` and `0x4000–0x40FC` while
  the job ran.

Command fields are at their public `rogue_fwif_frag_regs` offsets. The client CCB command
header in front of the command is 40 bytes on this ABI.

**Several ISP registers are only valid while the render runs.** The firmware, or the
hardware, puts them back when the job ends:

| Register | During the render | After the render |
|---|---|---|
| `ROGUE_CR_ISP_RENDER` (`0x0F08`) | 0 | 3 |
| `0x0F38` | `isp_ctl` | `0x21000` |
| `0x0F48` | `isp_zlsctl` | 0 |
| `0x0F88` | `isp_bgobjvals` | `0x200` |

A snapshot taken after a short render misses them; this is why earlier answers listed the
ZLS fields as not found.

#### ZS load/store (item 3)

| Command field | Register | Evidence |
|---|---|---|
| `isp_zlsctl` (64-bit) | **`0x0F48`/`0x0F4C`** | equal to the field during the render in every depth test: `0x180AA01E` (d32 load), `0x180A201C` (d32 clear), `0x1A8EE01E` (d24s8 load). It reads 0 after the job, and in a colour-only render it stays 0. The field layout is Mesa's public `ISP_ZLSCTL` |
| `isp_zload_store_base` | **`0x0F50`/`0x0F54` and `0x0F58`/`0x0F5C`** (the same address in both pairs) | equal in every depth test; written with **0** in a colour-only render. Mesa's public names point to a load base and a store base |
| `isp_stencil_load_store_base` | **`0x0F60`/`0x0F64` and `0x0F68`/`0x0F6C`** | equal in the d24s8 tests. A d32 (no stencil) render writes **0** there. With packed d24s8, depth and stencil have the same address |
| `zls_stride` (`cmd+468`) | `0x17B0` in one of the depth runs | `TO CONFIRM` (only one match) |
| `fb_cdc_zls` | not used | this core does not have the public `REQUIRES_FB_CDC_ZLS_SETUP` feature (Mesa device info for 36.29.52.182), and the field was 0 in every command |
| `isp_ctl` | `0x0F38` | `0x80031000` / `0x80131000` during the render (the command values), `0x21000` after |
| `isp_bgobjvals` | `0x0F88` | `0x1FF` / `0x3FF` during the render (the command values) |

**The `flags` bits.** The commands carried:
- `0x8080` with depth only (`ROGUE_FRAG_FLAGS_DEPTHBUFFER`, bit 7);
- `0x8180` with depth and stencil (bit 8, `STENCILBUFFER`, added);
- `0x8000` for colour only.

In every case the ZLS registers held the command's field values, which are 0 when the
corresponding buffer is absent. The observation cannot tell whether the firmware skips the
writes when a flag is clear or writes the zero fields (`TO CONFIRM`). Writing the field
values unconditionally gives the observed register state.

#### `0x0F28` (item 4)

| Render | `0x0F18` | `0x0C20` (geometry, `mtile_stride`) | `0x0F28` |
|---|---|---|---|
| 64×64 (depth tests) | `0x0004_0004` | 4 | **`0x10`** |
| 256×256 (`simple_draw`, `basic_draw`) | `0x0010_0010` | `0x10` | **`0x40`** |

- `0x0F28` is not a copy of any word of the fragment command or the HWRTData.
- In both sizes it equals **4 × `mtile_stride`** (`rogue_fwif_hwrtdata_common.mtile_stride`,
  the value the geometry kick puts in `0x0C20`). This holds in the two sizes seen
  (`TO CONFIRM` with more sizes and with MSAA).
- `0x0F18` follows the macrotile layout of the render target (`TO CONFIRM`: candidate
  `isp_mtile_size`).

#### `PM_3D_MEM_FREE` (item 1)

- **`ROGUE_CR_EVENT_ENABLE` (`0x0128`) reads 0 for the whole run.** No event enable is
  needed for the bit to appear in `ROGUE_CR_EVENT_STATUS`.
- **`0x0388`** is **1 during every render that follows a geometry pass**, and **0 during
  transfer jobs**:
  - it goes 0 → 1 at the fragment kick, when the local live stack pointer `0x0350` still
    holds the pages (`0x10`);
  - it goes 1 → 0 at the next transfer job.
- **`PM_3D_MEM_FREE` (bit 8) followed every `PIXELBE_END_RENDER` of a render with
  `0x0388` = 1**, and **never** one of a transfer (`0x0388` = 0). Transfers end with
  `EVENT_STATUS` `0x18` only.
- Timing:
  - in small renders the bit came 7 µs after `PIXELBE_END_RENDER`; in `bigpb` it came
    0.4–2.5 ms after;
  - in every case it came after `0x0350` and the global live stack pointer were back at
    their full value;
  - the firmware clears bit 8 within 3–7 µs, then bit 3.
- `0x03E0` read 1 throughout.
- **Before the next load of the same PM context:** in every observed sequence, the next
  geometry kick (and its PM load) came only after `PM_3D_MEM_FREE` had been seen and
  cleared.

So an implementation that sees the pages return but never sees bit 8 should check that
`0x0388` is 1 while the render runs. The spec's transfer table sets it to 0, and the fragment
kick must set it back to 1 (see [kick-geometry-ta](kick-geometry-ta.md), Q16k experiment
D).

#### Failing image comparisons (item 5)

`dEQP-VK.draw.renderpass.basic_draw.draw.point_list.45` and
`dEQP-VK.draw.renderpass.basic_draw.misc.flat_b_sat_error` **pass** with the original
firmware on this stack.

Registers during the `point_list.45` render:
- `0x0F18` = `0x100010`, `0x0F20` = `0x13E000`, `0x0F24` = `0x80`, `0x0F28` = `0x40`;
- `0x0F38` = `0x80031000`, `0x0F80` = `0x3F800000`, `0x0F88` = `0x200`;
- `0x0FA0`/`0x0FA8` = `0x88`, `0x0FB0` = `0x53240`, `0x0FB4` = `0x80`, `0x0FD8` = 2;
- `0x4078` = `0xFF000000` (only while the render runs).

For `flat_b_sat_error` the capture caught only its read-back transfer (`PENDING`).

The command fields of these renders map as in the tables above and in phase B.

#### Input-attachment renders (answers Q16m item 1, O, P)

`dEQP-VK.renderpasses.renderpass1.suballocation.formats.d32_sfloat.input.clear.dont_care.draw`
**passes** on the board. From the trace and register sampling:

- **The two subpasses are two renders on two HWRTData** (`RTD …c000`, then `RTD …d000`).
  Each has its own geometry and fragment kick.
  - The second HWRTData's region headers are the same buffer at **`+0x80`**
    (`TE Region headers base 0x80_000D_4080`).
  - Its MLIST base (`0x02D8`/`0x02E0`), VDM stream base (`0x0408`) and catalogue pages are
    its own.
  - VHeap (`0x0248`) and TPC (`0x0C40`) are shared with the first.
- **Second render** (input attachment, colour only), register values during the render:

  | Register | Value | Source |
  |---|---|---|
  | `0x0F20`/`0x0F24` | `0x80_000D_4080` | the second HWRTData's `rgn_header_dev_addr` (not the first's) |
  | `0x0F38` | `0x80011000` | `isp_ctl` |
  | `0x0F48`, `0x0F50`–`0x0F5C` | 0 | no depth |
  | `0x0FB0`/`0x0FB4` | `0x80_0005_B480` | `isp_scissor_base` (moved from `0x532C0` of the first render) |
  | `0x1510`/`0x1514` | `0x6880_0000` / `0x0200_007D` | PBE word 0 of target 0 |
  | `0x1550`/`0x1554` | `0x3F` / `0x3F` | PBE word 1 |
  | `0x0618`, `0x0620`, `0x0628` | `0x80`, `0x2BC0`, `0x201` | pipe-reset values of phase D |
  | `PM_CAT_BASE_*1`, `0x02E0` | the second HWRTData's catalogues / MLIST | fragment PM load |
  | `0x0388` | 1 | |

  `0x06A0`–`0x06BC` (`PDS_BGRND*`, written by transfers) read 0 during both renders.
- **The fault reported for this render decodes as an ISP parameter fetch (P).** Request
  status `0x0010_2500_0000_2000` has, in the public vendor kernel driver's tables
  (SpacemiT `linux-6.6` `img-rogue`, XE memory-hierarchy layout): read, `TAG_ID` `0x25` =
  **IPF control request**, side-band 0, address `0x2000`.
  - The IPF reads the region headers and the control streams they point to.
  - A read at `0x2000` points to a region-header, MLIST or catalogue base that is 0, or to
    the first HWRTData's state, while the second HWRTData renders.
  - Compare `0x0F20`, `0x02E0`, `PM_CAT_BASE_*1` and the context-1 ALIST/MLIST stack
    pointers of the second render with the table above (`TO CONFIRM` which one).

### D. ISP pipe and output

- Pipe reset: `BCAST[ROGUE_CR_ISP_RENDER]` (`0x0F08`) `= 0`, `BCAST[0x0618] = 0x80`,
  `BCAST[0x0620]` = `cmd+0x80`, `BCAST[0x0628]` = `cmd+0x74`, `BCAST[0x0F10] = 0`,
  `BCAST[0x0FC8] = 0`, `BCAST[0x0FCC] = 0`.
- Output/PBE: same **two-bank** layout as the transfer kick (see "Output / PBE state"
  below) — **WORD0 of render target *n* → `BCAST[0x1510 + 8·n]`, WORD1 → `BCAST[0x1550 + 8·n]`**
  (8 targets; 16 writable 64-bit registers `0x1510…0x1588`). The source is the frag command's
  `pbe_word[n][…]`; the firmware writes 2 words per target. Do **not** interleave as
  `0x1510 + 8·(3n+k)`.

### E. Trace and watchdog

Trace MAIN id 120: `Kick 3D: FWCtx … @ …, RTD …, Partial render:…, CSW resume:…. (PID:…,
…)`. Read `ROGUE_CR_TIMER`, arm the watchdog and record the execution state of DM 3. PM 3D
timeouts are in [observed-sequences](observed-sequences.md).

### F. Trigger

- **`BCAST[0x0F00] = 1`** (no public name) **launches the pixel pipeline.** It is the last
  write of the kick and is issued unconditionally (**D**/**O**).
- In a normal (non-resumed) 3D kick, `ROGUE_CR_ISP_RENDER` is written **only** by the phase D
  pipe reset (`= 0`). Nothing else writes it before the launch.
- **`ROGUE_CR_ISP_RENDER |= 0x10` is part of resuming a stored context, not of the launch.**
  It is done only when the context's `fw_com_ctx_flags` has
  `ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME` set. In that case the firmware first reloads the
  per-core state saved by the context store into `CR[0x1020]` of each core (from the
  context's `context_state_addr` buffer), then sets bit 4, and then reloads `CR[0x0298]`
  and `CR[0x01C8]` from the same buffer. A normal kick writes `CR[0x0298] = 0` and
  `CR[0x01C8] = 0` instead.
- `(ROGUE_CR_ISP_RENDER & 3)` is **not** a kick precondition. It is tested when the firmware
  decides whether to **request a 3D context store**: a store is requested (by writing
  `BCAST[0x1000] = 1`) only while `(ROGUE_CR_ISP_RENDER & 3) != 3`.

> **Correction (Q14).** Earlier versions of this phase described the trigger as
> "`ISP_RENDER |= 0x10`, then `0x0F00 = 1`", with an `(ISP_RENDER & 3) == 3` readiness check
> and, on multicore, a condition on the launch. Re-checked against the board: the
> launch is `0x0F00 = 1` alone, `| 0x10` belongs to the resume path, and the `& 3` test
> belongs to the context-store request. The point of the Q8 answer still holds: without
> `0x0F00 = 1` the pipe never starts.

---

## Transfer kick (TQ_3D / DM 3)

**Transfer through the 3D pipeline** (blit/resolve) uses the fragment pipe. It is a
**sub-case of the 3D kick**, with its own configuration.

### A. Preconditions

Power domain on; `BCAST[ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON]` = control value (also used
for the ISP configuration below); memory context bound to a BIF slot (DM 3, according to a
flag at `cmd+0xE0` bit 0).

> **Mandatory gate (Q8).** Binding the memory context to the BIF slot (DM 3) **must
> succeed** before any ISP/PBE register is written: the whole register-programming block and
> the trigger are conditional on it (if the BIF activation returns "not done", the firmware
> skips the kick entirely and the pipe never starts). This is the same
> `ROGUE_CR_BIF_CAT_BASE_INDEX` (PIXEL field) activation as for a 3D kick — a transfer is a
> sub-case of 3D and needs it too.

### B–D. Registers written by the transfer kick

All writes go through the broadcast bank. Unless stated, a value is the host's
`rogue_fwif_transfer_regs` field, written unchanged.

> **Provenance.** Registers, values and conditions: **D**. Field roles: **P** (public
> header, and public Mesa: `csbgen/rogue/cr.xml`, `include/hwdef/rogue_hw_defs.h`,
> `vulkan/pvr_arch_job_transfer.c`). Field names are matched by their offsets in the public
> `rogue_fwif_transfer_regs` layout.

| Register | Value | Written when |
|---|---|---|
| `ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON` | `1` on a single-core GPU (this SoC), or when `flags` has `ROGUE_TRANSFER_FLAGS_SINGLE_CORE`; otherwise `((1 << cores) − 1) \| 0x100` | always |
| `0x0F88` | `isp_bgobjvals` | always |
| `0x06A0`/`0x06A4` | `pds_bgnd0_base` (64-bit) | always |
| `0x06A8`/`0x06AC` | `pds_bgnd1_base` (64-bit) | always |
| `0x06B8`/`0x06BC` | `pds_bgnd3_sizeinfo` (64-bit) | always |
| `0x0F20`/`0x0F24` | `isp_mtile_base` (64-bit) | only if `(isp_render & 3) == 2` |
| `0x0F28` | `isp_rgn` | only if `(isp_render & 3) == 2` |
| `0x0FC8` / `0x0FCC` | `0` / `0xE4` | only if `(isp_render & 3) == 2` |
| `0x4070` | `usc_pixel_output_ctrl` | always |
| `0x4078`, `0x4080`, `0x4088`, `0x4090` | `usc_clear_register0..3` | always |
| `0x3E40` | `frag_screen` | always |
| `0x0F18` | `isp_mtile_size` | always |
| `0x0F10` | `isp_render_origin` | always |
| `0x0FD8` | tiles in flight = `min(((isp_ctl >> 12) & 0xF) + 1, 2)` | always |
| `ROGUE_CR_ISP_CTL` | `isp_ctl` | always |
| `0x0F30` | `isp_aa` | always |
| `0x0FA8` / `0x0FAC` | `0x88888888` / `0x88888888` | only if `(value written to 0x0F30) & 3 != 0` |
| `0x0F48` / `0x0F4C` | `0` / `0` | always |
| `0x0628` | `event_pixel_pds_info` | always |
| `0x0618` | `event_pixel_pds_code` | always |
| `0x0620` | `event_pixel_pds_data` | always |
| `0x1510 + 8·n` / `0x1550 + 8·n` | `pbe_wordx_mrty[2n]` / `pbe_wordx_mrty[2n+1]` | per render target *n* |
| `0x0388` | `0` (no public name; `PM_3D_MEM_FREE` was only seen after renders with `0x0388` = 1, see Q16l) | always |
| `CR[0x1020]` of each core, then `ROGUE_CR_ISP_RENDER \|= 0x10` | per-core state saved by the context store, from `context_state_addr` | only when resuming (`fw_com_ctx_flags` has `ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME`) |
| `ROGUE_CR_ISP_RENDER` | `isp_render` | always, immediately before the launch |
| `0x0F00` | `1` (pixel-pipe launch) | always, last write |

A register listed with a condition is **not written** when the condition is false. This
analysis does not establish what happens if such a register is written anyway.

**Ordering.** Only these constraints are required:

- the DM 3 memory-context activation comes first, and if it fails nothing in the table is
  written and the kick is skipped;
- the last two writes are `ROGUE_CR_ISP_RENDER = isp_render` and then `0x0F00 = 1`.

Everything else in the table may be written in any order. After the launch, the firmware
records the kick time and requests a BG pass (`ROGUE_CR_MTS_SCHEDULE = 0`).

> **Correction (Q14).** Earlier versions of this section placed the `isp_render` write
> before the configuration registers and launched with `ISP_RENDER |= 0x10`. Re-checked
> against the board (the target): `ISP_RENDER = isp_render` is written just before
> `0x0F00 = 1`, and `| 0x10` is done only when resuming, as in the 3D kick (phase F).

**Pre-kick firmware job.** After every GPU-units initialisation (cold boot, warm boot, HWR
re-init), the first kick on any DM (geometry, fragment, transfer or compute) checks the
context's `rf_cmd_addr`. If it is
non-zero, the firmware first runs a compute job from that address on DM 4, waits for
`COMPUTE_FINISHED` and flushes the compute caches. If that job fails, the kick is skipped.
The mainline host never sets `rf_cmd_addr`, so on mainline nothing runs and the kick
proceeds.

#### Output / PBE state

| Register | Value | Content |
|---|---|---|
| `0x1510 + 8·n` (`+0`/`+4`) | `pbe_wordx_mrty[2n]` | PBE register word 0 of render target *n* |
| `0x1550 + 8·n` (`+0`/`+4`) | `pbe_wordx_mrty[2n+1]` | PBE register word 1 of render target *n* |

A transfer uses **2 PBE register words per render target**, stored as consecutive pairs
(public Mesa `ROGUE_NUM_PBESTATE_REG_WORDS_FOR_TRANSFER = 2`). The remaining entries of the
9-entry `pbe_wordx_mrty` array are not written. These register words hold format, stride,
swizzle and clip. **They do not hold the destination address** (see below).

#### Pixel-event program and destination address (answers Q13)

1. **Where the destination address is.** It is **not** in `pbe_wordx_mrty`. The PBE has two
   kinds of words. The **register** words above carry format, stride, swizzle and clip. The
   **state** words carry the surface address (`address_low`/`address_high`). The host places
   the state words in the **data segment of the end-of-tile pixel-event program** it builds
   for the transfer, and points `event_pixel_pds_code`/`event_pixel_pds_data` at that
   program (**P**, public Mesa). So the host does leave the address to the end-of-tile emit.
   The firmware never sees the address and programs no destination register.
   *This corrects the Q12 answer, which said the address was in the PBE register words.*
2. **`0x0618`/`0x0620`/`0x0628`.** The transfer kick writes all three, each with the host's
   32-bit value unchanged: `0x0628 ← event_pixel_pds_info`, `0x0618 ← event_pixel_pds_code`,
   `0x0620 ← event_pixel_pds_data`. Their roles, from the public Mesa field layouts:

   | Register | Role | Fields |
   |---|---|---|
   | `0x0618` | pixel-event PDS **code** address | `addr` bits 31:4, 128-bit granular |
   | `0x0620` | pixel-event PDS **data** address | `addr` bits 31:4, 128-bit granular |
   | `0x0628` | pixel-event **info** | `const_size` 4:0, `temp_stride` 8:5, `usc_sr_size` 14:9 |

   The pixel-event task is **skipped** when `const_size` or `usc_sr_size` is 0. That explains
   why no `PIXELBE_END_RENDER` arrives when these registers are left unprogrammed.

   `0x0618` is the code address, not a control register, and the code address is not derived
   from data + size. The 3D kick writes a fixed `0x80` there because `rogue_fwif_frag_regs`
   has **no** `event_pixel_pds_code` field. It does have `event_pixel_pds_info` and
   `event_pixel_pds_data` (public offsets 60 and 72), and the 3D kick writes them to `0x0628`
   and `0x0620`, which is consistent with the roles above.

   The code and data values are **offsets** into the PDS heap: they resolve against the PDS
   execution base (`0x0610/0x0614`, see [boot-gpu-init](boot-gpu-init.md)). The USC
   end-of-tile program that the PDS program starts lives in the USC heap and resolves
   against the USC execution base (**P**, public Mesa).

   The reported host values decode as follows: `info = 0x202` gives `const_size = 2`,
   `temp_stride = 0` and `usc_sr_size = 1`, so the task runs.
3. **Anything else the pixel-event task needs.** The table above is the complete set of
   registers the transfer kick writes:
   - There is **no** per-kick shared-register allocation register. The pixel-event task's
     shared-register size comes from `usc_sr_size` in `event_pixel_pds_info`, and the
     unit-level shared/temp register setup is done once at boot (`0x06D8`, `0x4050` in
     [boot-gpu-init](boot-gpu-init.md)).
   - **`0x4070`** is the USC pixel-output control, loaded from `usc_pixel_output_ctrl`. It is
     the same register the 3D kick loads from `rogue_fwif_frag_regs.usc_pixel_output_ctrl`.
   - **`0x0F10`** is the render origin. The transfer kick writes the host's
     `isp_render_origin`; the 3D kick writes `0` because `rogue_fwif_frag_regs` has no such
     field. Both are correct.

**Open point.** The analysis does not show which value makes the end-of-tile write land on
address 0. The emit takes the address from the pixel-event program's data at run time, so a
write to 0 means the PBE state words, as the GPU reads them, are zero. Hypotheses, consistent
with the observations but unverified:

- the PDS data address does not resolve to the host's upload (check the PDS execution base
  plus `event_pixel_pds_data`, against the address the host gave);
- the host's upload is not visible to the GPU when the task runs;
- a conditional register from the table was written outside its condition.

**Fields the transfer kick reads (answers Q25).** The table above is the whole kick. In
particular the firmware does **not** read `isp_xtp_pipe_enable` (feature
`S7_TOP_INFRASTRUCTURE` is absent), reads no `flags` bit except bit 1
(`ROGUE_TRANSFER_FLAGS_SINGLE_CORE`) and bit 0 (no public name; it changes how the DM 3
memory context is bound, `TO CONFIRM` in what way), and always writes the PBE words of render targets 0–2 (six of
the nine `pbe_wordx_mrty` entries), whatever the number of targets in use. A transfer that
completes but produces wrong bytes is therefore not explained by an ignored transfer field.
Look at what the host puts in the fields and in the end-of-tile program instead.

**Completion:** a TQ_3D ends on **`PIXELBE_END_RENDER`** (`ROGUE_CR_EVENT_STATUS` bit 3), the
same event as a normal 3D render. `0x06B0` is **not** written by the transfer kick.

---

## Completion: per-job cache flush (answers Q14)

When a fragment, transfer or compute job finishes, the firmware **flushes the caches of that
DM's pipeline before it retires the command**. Retiring is the step that applies the fence
updates the host waits on. So the host never sees a job as finished before its output has
been written back (**D**).

The order on completion is:

1. clear the completion event in `ROGUE_CR_EVENT_CLEAR` (`PIXELBE_END_RENDER` for 3D and TQ,
   `COMPUTE_FINISHED` for compute);
2. run the flush sequence below;
3. retire the command (fence updates, then the IRQ to the host).

The flush depends on the context's DM (`rogue_fwif_fwcommoncontext.dm`). Transfer contexts
use `PVR_FWIF_DM_FRAG`, so a TQ_3D gets the fragment variant. All writes are broadcast.
"Poll" means: wait on every enabled core until the written bit clears.

| Step | Fragment / transfer (`dm` = FRAG) | Compute (`dm` = CDM) |
|---|---|---|
| 1 | `0x4000 = 1` | `0x4000 = 1` |
| 2 | `0x0688 = 2`, poll bit 1 | `0x0688 = 4`, poll |
| 3 | `0x0668 = 3`, poll bit 0 | `0x0668 = 9`, poll bit 0 |
| 4 | `0x1608 = 0x38001`, poll bit 0 | `0x1608 = 0x10001`, poll bit 0 |
| 5 | `0x1480 = 1`, `0x1488 = 1` | `0x1488 = 1` |
| 6 | `ROGUE_CR_MCU_FENCE = 0x00000180_00000000` (`DM` field = `DM_PIXEL`) | `ROGUE_CR_MCU_FENCE = 0x00000280_00000000` (`DM` field = `DM_COMPUTE`) |
| 7 | `0x1720 = 0x2B`, poll `0x1728` bit 0 | `0x1720 = 0x33`, poll `0x1728` bit 0 |
| 8 | write barrier (see [execution-environment](execution-environment.md)) | — |
| 9 | `ROGUE_CR_SLC_CTRL_FLUSH_INVAL = 6` (`DM_TA \| DM_PIXEL`), poll `ROGUE_CR_SLC_STATUS0` bit 2 | `ROGUE_CR_SLC_CTRL_FLUSH_INVAL = 8` (`DM_COMPUTE`), poll `ROGUE_CR_SLC_STATUS0` bit 2 |

None of `0x4000`, `0x0688`, `0x0668`, `0x1608`, `0x1480`, `0x1488`, `0x1720`, `0x1728` has a
public name. They are steps of the front-end, USC/PDS, TPU and MCU cache flush that precedes
the SLC flush. Their individual roles are inferred, not established. A geometry variant
exists as well (`0x0688 = 1`, `0x0668 = 5`, SLC `DM_TA`).

**Answers to the three Q14 questions:**

1. **Nothing per kick is needed before the first tile can emit.** The transfer kick does
   **not** do the 3D phase D pipe-reset writes (`ISP_RENDER = 0`, `0x0618 = 0x80`,
   `0x0F10 = 0`); those belong to the 3D kick only. No USC/PDS flush happens at kick time.
   The only firmware action before a kick is the `rf_cmd_addr` job after a units
   initialisation, which does nothing on mainline.
2. **DM 3 state between jobs.** The firmware carries no pixel-pipeline state from one job to
   the next, except the context-switch state of a context being resumed. It relies on every
   job flushing its DM's caches at completion, as above. A compute job that ran just before
   is flushed by the compute variant when it completes, before it is retired.
3. **Yes, there is a firmware action at the end of a TQ that comes before the SLC flush.** It
   is steps 1–8 of the fragment variant above, which run before step 9 (the SLC flush) and
   before the command is retired. An SLC flush alone (step 9) is not the whole sequence.

> **Status.** That this flush is what the first tile of a multi-tile transfer is missing is
> **consistent with the observations but not proven**. The symptoms that fit are a lost
> output that varies from run to run, with neither an SLC flush nor a delay after
> `PIXELBE_END_RENDER` fixing it. The analysis shows that the firmware always runs this
> sequence before retiring a fragment or transfer job. It does not show which step a
> missing first tile depends on.

---

## Common skeleton (TA, 3D, TQ, CDM)

```
make sure the power domain is on
[first kick after a units init] run the rf_cmd_addr job if non-zero (no-op on mainline)
multicore control (BCAST …_CTRL_COMMON)
bind the memory context to a BIF slot               ── before any register-set write
[TA/3D] load the HWRTData into the PM (+ poll handshakes)
program the register set / BCAST with the command fields
[preemption] resume from the snapshot buffer
trace entry (Kick …) + deadline watchdog
record the DM's execution state
TRIGGER (last write):
  - TA:     CR[set+0x400] = 1
  - CDM:    BCAST[0x0478] = 1
  - 3D:     BCAST[0x0F00] = 1                       (ISP_RENDER left at 0 from phase D)
  - TQ:     BCAST[ROGUE_CR_ISP_RENDER] = isp_render ; then BCAST[0x0F00] = 1
  - [resume only, 3D/TQ] ISP_RENDER |= 0x10 after reloading the saved per-core state
ON COMPLETION:
  clear the completion event → per-DM cache flush (see "Completion") → retire the command
```

## Compute: one kick per command, zero workgroups (answers Q10)

The compute kick points the CDM at the host-built **control stream**
(`cmd_ctrl_stream_base` → `BCAST[0x0480/0x0484]`) and issues **exactly one** kick
(`BCAST[0x0478] = 1`) per `rogue_fwif_cmd_compute`. The firmware does **not** inspect the
work-group dimensions and does **not** special-case a zero dimension (it neither skips the
kick nor loops): the CDM hardware walks the whole control stream in that single run, and any
multiple dispatches are encoded **inside that one stream** (no "several CDM runs" per
command). So for `empty_workgroup_{x,y,z}` the firmware behaves exactly as for
`empty_workgroup_all` — it kicks once and waits for `COMPUTE_FINISHED`. A difference in
outcome between them is therefore **not** a firmware per-dimension rule; look at the CDM
control stream the host/Mesa builds (how a zero-dimension dispatch is encoded and
terminated) rather than at the kick path.

## Confidence and pending items

- **High confidence:** the phases and dependencies, the named registers
  (`ROGUE_CR_CDM_*`, `ROGUE_CR_ISP_RENDER`/`ISP_CTL`, `ROGUE_CR_MULTICORE_*_CTRL_COMMON`),
  the broadcast offsets, and the triggers (3D: `0x0F00 = 1`; TQ: `ISP_RENDER = isp_render`
  then `0x0F00 = 1`; CDM: `0x0478 = 1`; `ISP_RENDER |= 0x10` only when resuming).
- **`PENDING`:** the field name of each remaining `cmd+0xNN` (read the public header and
  its check file).
