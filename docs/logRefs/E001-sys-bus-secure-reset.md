<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E001 — the MIPS executes nothing until `SYS_BUS_SECURE_RESET` is advertised

**Question.** The first builds were accepted by the host ("FW version v1.0"), but
`firmware_started` never became 1 and the driver reported "Firmware failed to boot".
Was the boot code running at all?

**Set-up.**
- A boot-stage marker was written at the first instruction of `boot.S`, both to the
  boot-data page and to scratch `0x1A00`.
- `tools/gpu-probe.sh` sampled the registers every 0.25 s during `modprobe`.

**Observation.**

```
STAGE=00000000 REFILLS=00000000 ... MIPS_EXC_ST=00000003 REMAP_UNMAPPED=00000000
SOFT_RESET=00000000 EVENT_ST=00000000 MTS_SCHED_EN=000000ff
[  101.99] powervr cac00000.gpu: [drm] *ERROR* Firmware failed to boot
```

- The very first marker never appeared.
- `MIPS_EXCEPTION_STATUS = 3` throughout: the core was held in an exception and
  error state (EXL|ERL) without fetching.
- In the public `pvr_fw_start()`, the host writes `ROGUE_CR_SYS_BUS_SECURE = 0`
  ("disable the default sys_bus_secure protection") **only if** the device info
  carries `SYS_BUS_SECURE_RESET`.

**Change.** Add `SYS_BUS_SECURE_RESET` to `DEV_FEATURES` in `tools/mkfw.py`.

**Result.**

```
STAGE=c0de0006 REFILLS=00000009 ... MIPS_EXC_ST=0000002a
[  262.71] [drm] Initialized powervr 1.0.0 for cac00000.gpu on minor 0
```

**Conclusion.** On the K1 the GPU leaves reset with the secure-bus protection on, and
the MIPS cannot fetch from DRAM until the host clears it. A BXE-2-32 device info must
therefore include `SYS_BUS_SECURE_RESET`. This answers part of question Q2.

**Clean-room note.** Only our own firmware and the public host source were observed.
