<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Experiment log (logRefs)

This folder records the experiments on the board that produced a new finding: a
behaviour the spec did not describe, an assumption confirmed or refuted, or a hardware
fact found by observation. Each file covers one experiment and has these parts:

- **Question**: what we wanted to know
- **Set-up**: firmware build, kernel, commands
- **Observation**: raw values, with large dumps in `data/`
- **Conclusion**: what changed in the code or the docs
- **Clean-room note**: why the method is acceptable

Clean-room rule for every experiment: we observe **our own firmware** and the GPU
hardware it drives, through the public host driver and `/dev/mem`. The vendor firmware
is never run, read or disassembled, and no dirty-team material is used.

| Id | Date | Topic | Result |
|---|---|---|---|
| [E001](E001-sys-bus-secure-reset.md) | 2026-11-08 | MIPS executes nothing after reset | `SYS_BUS_SECURE_RESET` must be in the device info |
| [E002](E002-exl-in-c-handlers.md) | 2026-11-08 | TLB miss inside an interrupt handler | C handlers must run with `EXL = 0` |
| [E003](E003-tlb-flush-from-unmapped-page.md) | 2026-11-08 | TLB flush kills the firmware | the flush must run from an unmapped page |
| [E004](E004-register-read-power-off-hang.md) | 2026-11-08 | SoC hang during register sampling | never read RGX registers with the domain off |
| [E005](E005-runtime-pm-cycle.md) | 2026-11-08 | Runtime PM and warm boot | full cycle works with the full boot path at every power-on |
| [E006](E006-compute-kick-mmu-fault.md) | 2026-11-08 | First compute kick | CDM starts, MMU fault at VA `0x26C0`: exec base missing |
| [E007](E007-register-survey.md) | 2026-11-08 | Read-only register survey `0x0000–0x2000` | reset and boot values, fault state |
| [E008](E008-writable-mask-probe.md) | 2026-11-08 | Writable-bit map | `0x0610` (40-bit, 1 MiB) and `0x0FC8` (bits 39:34) are exec-base candidates |
| [E009](E009-pds-exec-base-0x0610.md) | 2026-11-08 | `pds_exec_base` → `0x0610` | the `0x26C0` fault disappears: `0x0610` = `PDS_EXEC_BASE`; task still never completes; SoC hangs after a driver reload |
| [E010](E010-firmware-fallback.md) | 2026-11-08 | Truncated firmware file after a hang | kernel falls back to `/lib/firmware/powervr/` |
| [E011](E011-event-enable-0x0128.md) | 2026-11-08 | `0x0128` event enable | it is `EVENT_ENABLE` (mask `0x8FFFE`); it routes events to the host IRQ, so it must stay 0; next fault at `0x7B80` (USC base?) |
| [E012](E012-usc-base-0x0fc8.md) | 2026-11-08 | `usc_exec_base` → `0x0FC8` | value accepted; no fault, no completion; `PERF_COMPUTE_CYCLE` shows the CDM active and stuck |

The raw data is in [data/](data/).
