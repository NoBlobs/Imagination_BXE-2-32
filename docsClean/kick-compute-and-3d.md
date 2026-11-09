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

- Power domain **on**. If the configuration requires it, the USC local memory is cleared
  first (a compute job run by the firmware itself).
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
- Power domain on (and local memory cleared, if configured).
- For a **partial render** with a pending deallocation, **pause the PM deallocation**
  first.
- **Memory context** bound to a BIF slot (DM 3).
- **Load the HWRTData into the PM** (fragment phase). If that fails, undo the memory
  context binding and abort the kick.

### B. ISP configuration (broadcast)

| Register | Data |
|---|---|
| `BCAST[0x4070/0x4078/0x4080/0x4088/0x4090]` | `cmd+0x38 … +0x48` (start of `regs`: pixel output control and clear registers) |
| `BCAST[0x40A0]` (byte) | `cmd+0x7C` |
| `BCAST[0x0F20/0x0F24]` | 64-bit address |
| `BCAST[0x3E40]`, `BCAST[0x0F18]` | render target state |

### C. ZS load/store

A partial render has no ZS load/store.

- `BCAST[0x0FA8/0x0FAC]`: ZS pair (from the render target state);
- `BCAST[0x0FB0 … 0x0FBC]` = `cmd+0x98 … +0xA4`; `BCAST[0x0F80]` = `cmd+0x5C`;
  `BCAST[0x0F30]` = `cmd+0x64`; `BCAST[0x0F28]` = render target state.

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

### F. Trigger (two writes — corrects Q5/Q8)

The pixel pipeline needs **two** final writes, in order (**D**/**O**):

1. `BCAST[ROGUE_CR_ISP_RENDER] = ROGUE_CR_ISP_RENDER | 0x10` — read-modify-write, set bit 4.
   The readiness precondition checked earlier is `(ROGUE_CR_ISP_RENDER & 3) == 3`. (A
   context **store** uses `ROGUE_CR_ISP_RENDER` the other way — a stop request — not `| 0x10`.)
2. **`BCAST[0x0F00] = 1`** (no public name) — this is the actual **pixel-pipe launch**, the
   true last write. On multicore it is issued once the fragment core mask covers all cores;
   single-core it is always issued.

> **This answers Q8.** Setting `ISP_RENDER` to `0x13` (host
> `isp_render 0x3` then `| 0x10`) is **not** enough: without the following
> `BCAST[0x0F00] = 1`, the pipe never starts, `PERF_3D_CYCLE` stays 0 and no
> `PIXELBE_END_RENDER` arrives — exactly the reported symptom. The reset write
> `ROGUE_CR_ISP_RENDER = 0` in phase D is not a trigger.

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
> `vulkan/pvr_arch_job_transfer.c`). Field names marked `TO CONFIRM` are inferred from the
> position of the value in the public layout and were not checked independently.

| Register | Value | Written when |
|---|---|---|
| `ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON` | control value | always |
| `0x0F88` | `isp_bgobjvals` (`TO CONFIRM`) | always |
| `0x06A0`/`0x06A4` | `pds_bgnd0_base` (64-bit) | always |
| `0x06A8`/`0x06AC` | `pds_bgnd1_base` (64-bit) | always |
| `0x06B8`/`0x06BC` | `pds_bgnd3_sizeinfo` (64-bit) | always |
| `ROGUE_CR_ISP_RENDER` | `isp_render` | always (see ordering) |
| `0x0F20`/`0x0F24` | `isp_mtile_base` (64-bit) | only if `(isp_render & 3) == 2` |
| `0x0F28` | `isp_rgn` (`TO CONFIRM`) | only if `(isp_render & 3) == 2` |
| `0x0FC8` / `0x0FCC` | `0` / `0xE4` | only if `(isp_render & 3) == 2` |
| `0x4070` | `usc_pixel_output_ctrl` | always |
| `0x4078`, `0x4080`, `0x4088`, `0x4090` | `usc_clear_register0..3` | always |
| `0x3E40` | `frag_screen` | always |
| `0x0F18` | `isp_mtile_size` | always |
| `0x0F10` | `isp_render_origin` | always |
| `0x0FD8` | tiles in flight = `min(((isp_ctl >> 12) & 0xF) + 1, 2)` | always |
| `ROGUE_CR_ISP_CTL` | `isp_ctl` | always |
| `0x0F30` | `isp_aa` (`TO CONFIRM`) | always |
| `0x0FA8` / `0x0FAC` | `0x88888888` / `0x88888888` | only if `(value written to 0x0F30) & 3 != 0` |
| `0x0F48` / `0x0F4C` | `0` / `0` | always |
| `0x0628` | `event_pixel_pds_info` | always |
| `0x0618` | `event_pixel_pds_code` | always |
| `0x0620` | `event_pixel_pds_data` | always |
| `0x1510 + 8·n` / `0x1550 + 8·n` | `pbe_wordx_mrty[2n]` / `pbe_wordx_mrty[2n+1]` | per render target *n* |
| `0x0388` | `0` (no public name; role unknown) | always |

A register listed with a condition is **not written** when the condition is false. This
analysis does not establish what happens if such a register is written anyway.

**Ordering.** Only these constraints are required:

- the DM 3 memory-context activation comes first, and if it fails nothing in the table is
  written and the kick is skipped;
- the last two writes are `ROGUE_CR_ISP_RENDER |= 0x10` and then `0x0F00 = 1` (see 3D
  phase F).

Everything else in the table may be written in any order. The analysed firmware writes
`ROGUE_CR_ISP_RENDER = isp_render` once before the other configuration registers and then
does the `| 0x10` at launch. Whether that early write is required has **not** been
established.

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

**Completion:** a TQ_3D ends on **`PIXELBE_END_RENDER`** (`ROGUE_CR_EVENT_STATUS` bit 3), the
same event as a normal 3D render. `0x06B0` is **not** written by the transfer kick.

---

## Common skeleton (TA, 3D, TQ, CDM)

```
make sure the power domain is on (and clear local memory, if configured)
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
  - 3D/TQ:  BCAST[ROGUE_CR_ISP_RENDER] |= 0x10 ; then BCAST[0x0F00] = 1  (0x0F00 is the launch)
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
  the broadcast offsets, and the trigger sequence (3D/TQ: `ISP_RENDER |= 0x10` then
  `0x0F00 = 1`; CDM: `0x0478 = 1`).
- **`PENDING`:** the field name of each remaining `cmd+0xNN` (read the public header and
  its check file).
