# GPU bring-up and boot register sequence

This document specifies the **GPU-side register programming** the firmware must perform at
boot (cold boot and warm boot) before the first kick, and how GPU events are routed to the
firmware's IRQ task. It answers what [execution-environment](execution-environment.md) and
[overview](overview.md) summarise as "the firmware initialises its subsystems (… GPU …)"
and the trace milestone `GPU Units init` (trace MAIN id 69).

> **Provenance.** Register offsets and the ordering: **D** (firmware analysis), confirmed by
> **O** (the sequence reproduces on the board; `GPU Units init` / `Initialised Firmware`
> appear in the trace, and omitting the exec bases reproduces the MMU fault the clean team
> observed). Public names are from `pvr_rogue_cr_defs.h`; registers the public header does
> not name are given by offset ("no public name"). The **values** below are hardware
> configuration constants required by this GPU, not firmware logic.
>
> `CR[off]` = primary-core register; `BCAST[off]` = XPU-broadcast write (see
> [registers](registers.md)). Unless stated, all writes in the GPU-units init are
> **broadcast**.

## When this runs

The whole sequence runs on **every** power-on of the GPU domain: cold boot and every warm
boot after an APM power-off (see [power-management](power-management.md)). It must complete
**before** the first kick of any DM; otherwise the first kick faults (an MMU page fault
when the exec bases are missing — see "PDS/USC execution bases").

## A. GPU units init (before any kick)

Done once the power domain is up, from values in `rogue_fwif_sysinit`. Order matters only
where a poll is shown.

1. **Clock gating.** If `ROGUE_FWIF_INICFG_DISABLE_CLKGATING_EN` is set, force all clocks
   on (write the clock-control registers — no public name, offsets in the `0x...` clock
   bank). Otherwise leave gating enabled.
2. **Power up the units/dusts.** Power on `power_units` dusts (see
   [power-management](power-management.md) and section D) and poll the unit-ready bits.
3. **SLC bypass.** For a small SLC (`SLC_SIZE_IN_KILOBYTES` 0 or ≤ 32 KiB) **set** the SLC
   bypass; otherwise clear it. On this GPU the SLC is 16 KiB (see
   [device-info](memory-model.md#device-information-block)), so the bypass is **set**:
   `BCAST[ROGUE_CR_SLC_CTRL_BYPASS] |= 0x01000000`, and the high word `|= 0x2000`.
4. **Reset pulse.** Pulse `BCAST[0x6000] = 1` then `= 0` (no public name).
5. **PDS/USC execution bases** (see the dedicated section below).
6. **Fixed block of unit-default writes.** A block of FW-only unit configuration (registers
   with no public name), required for the USC/TPU/MCU/VDM units to work. Required values
   (**D**/**O**):

   | Register(s) | Value |
   |---|---|
   | `BCAST[0x18C0]`, `[0x18C8]`, `[0x18D0]`, `[0x18E0]` | `0x007FC000` |
   | `BCAST[0x0600]` | `0x403F3F00` |
   | `BCAST[0x0604]` | `0x00800000` |
   | `BCAST[0x4050]` | over-allocation temp-register count `| 0x2000` |
   | `BCAST[0x06D8]` | over-allocation shared-register count |
   | `BCAST[0x0720]` | `0x0000000F` |
   | `BCAST[0x0640]` | `0x9`; `BCAST[0x0698]` = `0xB`; `BCAST[0x0630]` = `1`; `BCAST[0x0658]` = `0xB`; `BCAST[0x06C0]` = `0xD`; `BCAST[0x0648]` = `1`; `BCAST[0x0608]` = `1` |
   | `BCAST[0x18E8]`, `[0x18F0]`, `[0x18F8]`, `[0x1900]` | `0xF` (open variant) |
   | `BCAST[0x1498]`, `[0x0700]` | `1` |
   | `BCAST[0x0358]` | `5` |
   | `BCAST[0x20F8]` | `1`; `BCAST[0x2010]` = `0`; `BCAST[0x2100]` = `0` |
   | `BCAST[0x13F8]` | `7` |

7. **Init-done handshakes.** `BCAST[0x4068] = 1`, then **poll** `0x4068` until bit 0 = 0;
   `BCAST[0x0660] = 1`; `BCAST[0x0670] = 1`; `BCAST[0x0CD8] = 1`, then **poll** `0x0CD8`
   until bit 0 = 0. (No public names; both polls must succeed before the first kick.)
8. **TPU trace/filter mode** from `sysinit.filter_flags` (register `0x17B8`, no public
   name).
9. **Per-core index registers** for multicore (`0xF318`/`0xF338`, no public name) — a single
   core on this SoC.
10. Mark GPU units initialised (internal state) and emit trace MAIN id 69 `GPU Units init`.

### PDS/USC execution bases (answers the clean team's Q7)

The hardware needs the base of the PDS and USC code heaps so that the **offsets** carried in
the kick commands (e.g. `rogue_fwif_cdm_registers_cswitch.cdm_context_pds0`) resolve to real
device addresses. Without them the first kick's PDS/USC fetch takes an MMU page fault.

| Register (no public name) | Value | Notes |
|---|---|---|
| `BCAST[0x0610]` / `BCAST[0x0614]` | `sysinit.pds_exec_base` (low / high) | 64-bit PDS execution base. Confirms the clean team's probe (bits 39:20 writable). |
| `BCAST[0x4008]` / `BCAST[0x400C]` | `sysinit.usc_exec_base` (low / high) | USC execution base — **written to three register pairs** |
| `BCAST[0x4010]` / `BCAST[0x4014]` | `sysinit.usc_exec_base` (low / high) | second USC base pair |
| `BCAST[0x4028]` / `BCAST[0x402C]` | `sysinit.usc_exec_base` (low / high) | third USC base pair |

**Correction for the clean team:** the USC execution base is **not** register `0x0FC8`
(that offset is an ISP/3D register, written in the fragment kick — see
[kick-compute-and-3d](kick-compute-and-3d.md)). Setting `0x0FC8` removed the PDS fault only
because the PDS base at `0x0610` was already correct; the empty compute shader then never
completed because the real USC base registers (`0x4008`, `0x4010`, `0x4028`) were still 0.
Program all three USC pairs from `sysinit.usc_exec_base` and the empty compute shader
completes.

## B. Event routing (answers Q1)

GPU events (`ROGUE_CR_EVENT_STATUS`) reach the firmware's **IRQ task (HW2)** through the
**per-DM MTS interrupt-enable** registers — **not** through `ROGUE_CR_EVENT_ENABLE`. The
boot sequence programs:

| Register | Value | Routed event bits |
|---|---|---|
| `ROGUE_CR_EVENT_CLEAR` | `0x41FC` | clear pending events first |
| `ROGUE_CR_MTS_DM0_INTERRUPT_ENABLE` (`0x0B58`) | `0x89200` | `MMU_PAGE_FAULT` (b9), `GPIO_REQ` (b12), `USC_TRIGGER` (b15), `SLAVE_REQ` (b19) |
| `ROGUE_CR_MTS_DM2_INTERRUPT_ENABLE` (TA, `0x0B68`) | `0xA0` | `TA_FINISHED` (b5), `PM_OUT_OF_MEMORY` (b7) |
| `ROGUE_CR_MTS_DM3_INTERRUPT_ENABLE` (3D, `0x0B70`) | `0x08` | `PIXELBE_END_RENDER` (b3) |
| `ROGUE_CR_MTS_DM4_INTERRUPT_ENABLE` (CDM, `0x0B78`) | `0x04` | `COMPUTE_FINISHED` (b2) |
| `ROGUE_CR_EVENT_ENABLE` (`0x0128`) | `0` | **must stay 0** on a MIPS firmware |

- This is how **`COMPUTE_FINISHED` reaches the firmware**: through
  `MTS_DM4_INTERRUPT_ENABLE = 0x04`. Each `MTS_DMn_INTERRUPT_ENABLE` selects which
  `EVENT_STATUS` bits raise the IRQ task tied to DM *n*. The IRQ task then reads
  `EVENT_STATUS` to see what happened.
- **Confirming the clean team's E011 (and why):** on this **single-core** GPU
  `ROGUE_CR_EVENT_ENABLE` (`0x0128`) stays **0**. The firmware does later write
  `EVENT_ENABLE = 0x8EAC` — but **only on multicore** (`ROGUE_CR_MULTICORE_SYSTEM ≥ 2`), as
  part of broadcasting the primary core's event-enable/catalogue state to the secondary
  cores; the code path returns early on a single core. With `MULTICORE_SYSTEM = 1` here it
  is never taken, so `EVENT_ENABLE = 0` is correct and completions reach the firmware purely
  through the `MTS_DMn_INTERRUPT_ENABLE` path above. (`EVENT_ENABLE` drives the GPU's
  external event-IRQ line, which the mainline host does not service — hence `nobody cared`
  when it was set.)

## B′. How the IRQ task (HW2) is delivered and consumed (answers Q9)

- **Nothing more than section B is needed to *route* completions to HW2.**
  `ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG` (`0x0B50`) is programmed by the **host**
  (`= IDLE_CTRL_META`, in the MIPS boot path), **not** by the firmware — if the BG task
  (HW1) is being delivered, the Garten wrapper is already configured. `MTS_SCHEDULE_ENABLE`
  (`0x0BC8`) is **not written by the firmware or the host**; leave it at reset. So the only
  firmware-side requirement for event-driven HW2 is the per-DM `MTS_DMn_INTERRUPT_ENABLE`
  values of section B (e.g. DM4 = `0x04` for `COMPUTE_FINISHED`).
- **Ending the IRQ task:** write `0x0B08 = 2` then **read it back** (the read posts the
  write). No `MTS_INTCTX` clear or counted-task acknowledgement is needed. (The BG task ends
  with `0x0B08 = 0` + read-back.)
- **Which DM raised it:** there is **no "which DM" register**. The IRQ task reads the
  **core-local** `ROGUE_CR_EVENT_STATUS` (`CR[0x130]` in the primary core bank, not the
  broadcast/host view) and tests the per-DM completion bits: `KERNEL_FINISHED` (b1) → DM0,
  `COMPUTE_FINISHED` (b2) → DM4, `PIXELBE_END_RENDER` (b3) → DM3, `TA_FINISHED` (b5) → DM2,
  `PM_OUT_OF_MEMORY` (b7) → SPM, `MMU_PAGE_FAULT` (b9) → HWR. It clears what it handled via
  `ROGUE_CR_EVENT_CLEAR`. On multicore the per-core events are accumulated through the XPU
  event registers (`0x08F0`) — not relevant on this single-core SoC.
- **MIPS side:** HW2 is MIPS interrupt **IM4**; it must be unmasked (`Status.IM2..IM4`,
  i.e. `0x1C00`) with `Cause.IV = 1` and `IntCtl.VS = 256 B`, vectoring to `EBASE + 0x600`
  (HW0 timer `+0x400`, HW1 BG `+0x500`). See [execution-environment](execution-environment.md).
  If HW2 only ever fires when the firmware self-schedules it (`MTS_SCHEDULE = 0x20`) but not
  on completions, re-check that `EVENT_STATUS`/`EVENT_CLEAR` are read on the **primary core
  bank**, that IM4 stays unmasked, and that the handler re-enables interrupts on exit —
  the event→HW2 routing itself is just the section-B per-DM enables.

## C. MTS task-context association and the firmware-started handshake

| Register | Value | Meaning |
|---|---|---|
| `ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC` (`0x0B40`) | `0xFFFF` | the IRQ (interrupt) context serves all DMs |
| `ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC` (`0x0B30`) | `1` | the BG context serves DM0 |

- The host clears both `*_THREAD0_DM_ASSOC` registers when it stops the firmware, so these
  are firmware-set, as the clean team suspected. The boot sequence does **not** need to
  touch `ROGUE_CR_MTS_SCHEDULE_ENABLE` (`0x0BC8`).
- A **background** task is kicked with `ROGUE_CR_MTS_SCHEDULE = 0` (BGCTX, DM0); the
  **interrupt** task is kicked with `ROGUE_CR_MTS_SCHEDULE = 0x20` (INTCTX). After the
  firmware-started handshake, if work is already pending the firmware kicks the IRQ context
  with `MTS_SCHEDULE = 0x20`.
- **XPU broadcast / secure.** Early in power-on the firmware sets the XPU broadcast mask to
  all cores (`ROGUE_CR_XPU_BROADCAST`) and writes `ROGUE_CR_SYS_BUS_SECURE = 0`.
- **BIF fault page.** `sysinit.fault_phys_addr` is programmed into the BIF fault-read
  register so faulting units read a dummy page instead of stalling the bus (see
  [memory-model](memory-model.md)).
- **Firmware-started handshake.** Finally the firmware records
  `sysinit.firmware_started_timestamp = ROGUE_CR_TIMER`, flushes, then sets
  `sysinit.firmware_started = 1` — the flag the host's boot poll waits on (see
  [host-interface](host-interface.md)). The host polls it for ~5 s.

## D. Per-job dust/unit power (answers Q6)

The **external GPU power domain** (the whole island) is controlled by the **host** (it
powers the domain off/on around APM suspend/resume). The **dust/unit gating inside the GPU**
is controlled by the **firmware**, per job, when `ROGUE_FWIF_INICFG_POW_RASCALDUST` (BIT 4)
is enabled — which it is on this board (the trace shows `Changing number of dusts` and `HW
Request On/Off` on every job).

- **Active-dust mask:** `BCAST[0x0050] = (1 << num_dusts) − 1` (8-bit; no public name).
- **Power-island request:** bracket the request with `CR[ROGUE_CR_XPU_BROADCAST] = 1`, then
  `CR[0x0038] = req` (bit 0 = on/off), then `CR[0x0038] = req | 2` (bit 1 = request pulse);
  poll `ROGUE_CR_EVENT_STATUS` for `POWER_COMPLETE` (b10) / `POWER_ABORT` (b11); restore
  `CR[ROGUE_CR_XPU_BROADCAST] = 1`. Trace: POW id 7 `HW Request On(1)/Off(0): …, Units: …`,
  POW id 9 `Changing number of dusts …`. `0x0038` has no public name.

So touching `0x0038`/`0x0050` **is required** for a mainline-compatible firmware on this
GPU; it is not fully host-controlled.

## Summary — minimum to make the first kick complete

1. Power domain up (host) → GPU units init (section A), **including the three USC exec-base
   pairs and the PDS exec base**.
2. Event routing (section B), with `EVENT_ENABLE = 0`.
3. MTS context association + firmware-started handshake (section C).
4. Per-job dust power (section D) as part of the kick's power step.
Only then program the DM's register set and trigger it (see
[kick-geometry-ta](kick-geometry-ta.md), [kick-compute-and-3d](kick-compute-and-3d.md)).
