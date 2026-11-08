# Firmware overview

## What it is

The **PowerVR Rogue BXE-2-32** GPU contains a **dedicated microcontroller** (a MIPS core)
that runs firmware. That firmware is the GPU's **conductor**: the host driver (on the K1's
RISC-V CPU) does not program the GPU directly to draw — it **submits work** into command
queues in shared memory, and the firmware:

- reads the commands,
- resolves the dependencies between them,
- schedules the work on the various *data masters* (geometry, fragment, compute and
  transfer units),
- programs the GPU registers for each kick,
- handles completion (GPU interrupts) and signals the host back,
- performs power management and fault recovery.

The host **does not** see the GPU; it sees the **firmware**. All interaction goes through
the shared-memory contract (see [host-interface](host-interface.md)).

## Identification (BVNC)

- **BVNC `36.29.52.182`** — Branch 36, Version 29, Number of scalable units 52, Config 182.
- Firmware core: **MIPS32r2 / microMIPS**, **o32** ABI, **soft-float**, little-endian.
- Two variants of the same firmware exist:
  - **open** (upstream): ships with the mainline `drm/imagination` driver (`powervr`) and
    Mesa `pvr`. **It is the target of this spec.** Command numbering and structures follow
    the upstream `pvr_rogue_fwif*.h` headers.
  - **closed**: ships with the vendor's proprietary driver, with other layouts and other
    numbering. Out of scope (see [open-vs-closed](open-vs-closed.md)).

## Coprocessor role — the control loop

```
   Host CPU (RISC-V)                  Firmware (MIPS in the GPU)          GPU hardware
   -----------------                  --------------------------          ------------
   driver builds commands
   in the context's Client CCB
   driver writes KICK into ───────►   BG task reads the Kernel CCB
   the Kernel CCB + wakes FW          resolves Client CCB fences
                                      puts the context on the run list
                                      schedules the DM       ──────────►  programs registers,
                                                                          starts the unit
                                      (GPU works)            ◄──────────  finishes → EVENT_STATUS (IRQ)
                                      IRQ task handles completion
                                      retires the command, does updates
   driver gets IRQ        ◄─────────  signals the host (UFO/IRQ)
   and releases the frame
```

None of these steps has a "main loop": after boot the firmware sits in `WAIT` and **all**
work happens in **three interrupt contexts** (timer, background task, IRQ task). See
[execution-environment](execution-environment.md) and [scheduler](scheduler.md).

## Shared memory

Host and firmware share DRAM. The firmware addresses it through a **virtual window
`0xC0xxxxxx`** in its MIPS address space; the host addresses the same memory through the
GPU MMU. Control structures (CCBs, `sysdata`/`osdata` blocks, contexts) live there. The
firmware's VA→physical mapping is set up by the **host** (MIPS page tables managed by the
driver). See [memory-model](memory-model.md).

## Subsystems (mental map)

| Subsystem | Responsibility | Document |
|---|---|---|
| Boot / runtime | vectors, interrupts, caches/TLB, barriers | [execution-environment](execution-environment.md) |
| Memory | VA, remap, page tables, heap, virtualisation | [memory-model](memory-model.md) |
| Host↔FW contract | shared structures, handshake | [host-interface](host-interface.md), [data-structures](data-structures.md) |
| Command queues | KCCB, FWCCB, Client CCB | [kernel-ccb](kernel-ccb.md), [firmware-ccb](firmware-ccb.md), [client-ccb](client-ccb.md) |
| Scheduling | DMs, run/wait lists, preemption | [scheduler](scheduler.md) |
| Work execution | TA/3D/TQ/CDM kicks | [data-masters](data-masters.md) |
| PM memory | partial render, OOM, freelist | [partial-render-spm](partial-render-spm.md) |
| Power | APM, power-off, DVFS | [power-management](power-management.md) |
| Robustness | HWR, cleanup | [hardware-recovery](hardware-recovery.md) |
| Observability | trace, HWPerf | [hwperf-trace](hwperf-trace.md) |

## Conventions of this spec

- **Addresses `0x....`** without qualification are in the **firmware MIPS core's** address
  space.
- **`ROGUE_CR_*`** = GPU control register (see [registers](registers.md)), names from the
  public `pvr_rogue_cr_defs.h`.
- **`rogue_fwif_*`** / **`ROGUE_FWIF_*`** = structure / constant of the host↔firmware
  contract, names from the public `pvr_rogue_fwif*.h`.
- **Structure offsets** are in bytes from the start of the structure.
- **DM** = *data master*. DM indices are the public `PVR_FWIF_DM_*` values: `2` = geometry
  (`GEOM`), `3` = fragment (`FRAG`), `4` = compute (`CDM`), `6` = second geometry
  (`GEOM2`, multicore only), `1` = transfer (`TDM`). See [scheduler](scheduler.md) and
  [glossary](glossary.md).
- **Provenance**: **P** = public header/driver, **O** = observed on the board, **D** = from
  analysis (see [open-vs-closed](open-vs-closed.md)).
- Pseudocode is a **description of behaviour**, not the original code.
