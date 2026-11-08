<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E011 — `0x0128` is the event enable, and it drives the host IRQ line

**Question.** In E009 run 2 the CDM went idle (`0x03C0 = 0x7F`) but `EVENT_STATUS`
stayed 0. Do GPU events have to be enabled before they latch?

## Part A: writable bits of `0x0120` and `0x0128`

The method was a one-off write of all ones, read back, then restore, with the GPU
powered:

```
0x0120  orig 0x0  mask 0x0000000F
0x0128  orig 0x0  mask 0x0008FFFE
```

`0x0008FFFE` is exactly bits 1–15 plus bit 19 of the public `EVENT_STATUS` field list:
KERNEL_FINISHED, COMPUTE_FINISHED, PIXELBE_END_RENDER, ISP_END_MACROTILE,
TA_FINISHED, TA_TERMINATE, PM_OUT_OF_MEMORY, PM_3D_MEM_FREE, MMU_PAGE_FAULT,
POWER_COMPLETE, POWER_ABORT, GPIO_REQ, GPIO_ACK, ZLS_FINISHED, USC_TRIGGER, and bit 19
SLAVE_REQ. **`0x0128` is the GPU event enable register** for the implemented events
of this core. It is 0 after the host's boot.

## Part B: firmware sets `EVENT_ENABLE = 0x0008FFFE` at boot

Set-up:
- build `EXP="-DEXP_PDS_REG=0x610 -DEXP_EVENT_ENABLE=0x8FFFE"`, sha256 `f7361b25…`;
- driver reloaded (`modprobe -r`, then `modprobe`), so the device is `minor 1`;
- same dEQP case, with serial capture (`data/serial-e009-e011.txt`).

```
[211.467] [drm] Initialized powervr 1.0.0 for cac00000.gpu on minor 1
[211.549] irq 30: nobody cared (try booting with the "irqpoll" option)
          handlers: pvr_device_irq_handler [powervr]
[211.549] Disabling IRQ #30
gpuregs 0.00s  0130=00008008 0128=0008fffe 12b0=00000000 1a78=00000000 03c0=7f
gpuregs 2.39s  0130=00008208 0128=0008fffe 12b0=00001041 12b8=00007b80 1a78=00000010 03c0=7a 0bb0=00000200
(then the SoC hung: no ping, no serial)
```

Observations:
1. As soon as the enable was set, events latched: `EVENT_STATUS = 0x8008`
   (USC_TRIGGER + PIXELBE_END_RENDER) before any work was submitted. These are left
   over from reset or boot. With enable = 0 they never appeared in E006–E009, except
   the MMU fault.
2. **The enabled events go to the host interrupt line** (Linux IRQ 30 = GIC/PLIC
   source 75). The host handler only accepts interrupts the firmware raises through
   `MIPS_WRAPPER_IRQ_STATUS`. It returned `IRQ_NONE` until the kernel disabled the
   line. From then on, the host no longer saw *any* firmware interrupt.
3. The compute kick then faulted again, at a **new address**:
   `BIF_FAULT_BANK0_REQ_STATUS.ADDRESS = 0x7B80`, cat base 1. With the PDS base in
   place, the CDM got past the PDS fetch at `0x26C0` and failed at the next unit that
   needs a base.

## Conclusions

- `ROGUE_CR_EVENT_ENABLE` = `0x0128` (mask `0x0008FFFE` on BXE-2-32).
- **A MIPS firmware must leave it at 0.** It routes events to the host IRQ, which the
  mainline host does not handle. The firmware learns about events from
  `EVENT_STATUS`, either by polling or through the MTS IRQ task. This answers part of Q1.
- Events, other than the MMU fault, are only latched in `EVENT_STATUS` when enabled
  here. **How `COMPUTE_FINISHED` reaches the firmware without the host enable is still
  open.** The MTS path (`MTS_DMn_INTERRUPT_ENABLE`, `MTS_GARTEN_WRAPPER_CONFIG`) is the
  next candidate.
- The second fault at `0x7B80` points to the USC code base. The candidate from E008 is
  `0x0FC8` (bits 39:34). The knob is `EXP_USC_REG`.
- The SoC hang again followed a driver reload, which supports the E009 hypothesis.

**Clean-room note.** Own firmware, public field list, black-box register probe.
