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
- Output/PBE: blocks `BCAST[0x1510 … 0x1564]` = `cmd+0xD0 …` (the `pbe_word` area of
  `regs`), one register pair per render target (render-target arrays supported).

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

> **This is the fix for the clean team's Q8.** Setting `ISP_RENDER` to `0x13` (host
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

### B. Transfer configuration (broadcast)

| Register | Data |
|---|---|
| `BCAST[0x0F88]` | `cmd+0x30` |
| `BCAST[0x06A0 … 0x06BC]` | `cmd+0x78 … +0x8C` |
| `BCAST[0x0F20/0x0F24]` | `cmd+0x90/0x94` (when applicable) |
| `BCAST[0x0F28]` | `cmd+0x6C`; `BCAST[0x0FC8] = 0`, `BCAST[0x0FCC] = 0xE4` |

### C. ISP and output

- ISP (same registers as 3D): `BCAST[0x4070]` = control value,
  `BCAST[0x4078/0x4080/0x4088/0x4090]` = `cmd+0x38 … +0x44`, `BCAST[0x3E40]` = `cmd+0x70`,
  `BCAST[0x0F18]` = `cmd+0x48`, `BCAST[0x0F10]` = `cmd+0x4C`, `BCAST[0x0FD8]`,
  `BCAST[ROGUE_CR_ISP_CTL]` (`0x0F38`), `BCAST[0x0F30]` = `cmd+0x58`.
- **No ZS** (transfer has no depth/stencil): `BCAST[0x0FA8] = BCAST[0x0FAC] = 0x88888888`;
  `BCAST[0x0F48] = 0`, `BCAST[0x0F4C] = 0`.
- Output/PBE: `BCAST[0x0628/0x0618/0x0620]` = `cmd+0x5C/0x60/0x64`; blocks
  `BCAST[0x1510 … 0x1564]` = `cmd+0x98 …` (destinations).

### D. Trace, watchdog and trigger

As for 3D: trace MAIN id 121 `Kick 3D TQ: FWCtx … @ …, CSW resume:…. (PID:…, …)`,
watchdog, then the **two-write trigger** — `BCAST[ROGUE_CR_ISP_RENDER] |= 0x10`, then
`BCAST[0x0F00] = 1` (the real launch). `0x06B0` is **not** written by the transfer kick.

**Completion (Q8):** a TQ_3D ends on **`PIXELBE_END_RENDER`** (`ROGUE_CR_EVENT_STATUS` bit 3),
the same event as a normal 3D render; it is handled by the shared 3D-completion path.

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
