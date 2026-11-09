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
  `isp_zlsctl`/`isp_zload_store_base`; the ZLS fields are elsewhere (`TO CONFIRM`).
- `BCAST[0x0F80]` = `isp_bgobjdepth` (**O**); `BCAST[0x0F30]` = `isp_aa` (`cmd+0x64`);
  `BCAST[0x0F28]` = render target state.
- `BCAST[0x17A0]`/`[0x17A4]` = `tpu_border_colour_table` (**O**; the same register the
  geometry kick writes).

> Source of the field names: their offsets in the public `rogue_fwif_frag_regs` (check
> header), plus a black-box observation of one fragment job (open firmware v1.1, confirmed
> with v1.0; see [kick-geometry-ta](kick-geometry-ta.md)). Fields not listed here are `PENDING`. The
> observation could not place them because the read-back transfer that follows a render
> overwrites part of the 3D registers.

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
> and, on multicore, a condition on the launch. Re-checked against the open variant: the
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
| `0x0388` | `0` (no public name; role unknown) | always |
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
> against the open variant (the target): `ISP_RENDER = isp_render` is written just before
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
