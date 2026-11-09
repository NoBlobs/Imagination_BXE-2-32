<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E016 — how `COMPUTE_FINISHED` reaches the firmware: polling, not the MTS IRQ task (yet)

> **Superseded by [E018](E018-irq-task-delivery.md):** the IRQ task does fire on every
> completion. Here the 1 ms poll won the race and consumed the event first.

**Question.** The spec says `MTS_DM4_INTERRUPT_ENABLE = 0x04` routes `COMPUTE_FINISHED`
to the IRQ task (HW2). The firmware also polls `EVENT_STATUS` every 1 ms. Which path
completed the jobs in E013?

**Set-up (build `8c200280…`).** `irq_dispatch` now writes counters to scratch registers:

| Register | Content |
|---|---|
| `0x1A40` | timer interrupts (HW0) |
| `0x1A48` | MTS BG tasks (HW1) |
| `0x1A50` | MTS IRQ tasks (HW2) |
| `0x1A58` | `EVENT_STATUS` seen by the last IRQ task |
| `0x1A60` | OR of every event handled by `kick_events()` |

**Observation** (fresh boot, after `empty_shader`):

```
1a40=00000b45   ~2900 timer ticks
1a48=00000043   67 BG tasks (one per KCCB batch: the host's MTS_SCHEDULE works)
1a50=00000001   ONE IRQ task in total
1a58=00000400   ... and it saw only POWER_COMPLETE (bit 10)
1a60=00000404   COMPUTE_FINISHED (bit 2) was handled -> by sched_poll() on the timer
```

**Conclusions.**
- **BG tasks work.** Every KCCB command written by the host raises HW1.
- **The IRQ task is not raised by `COMPUTE_FINISHED`** with the routing as programmed:
  `MTS_DM4_INTERRUPT_ENABLE = 0x04`, `INTCTX_THREAD0_DM_ASSOC = 0xFFFF`. The only HW2
  came with `POWER_COMPLETE`, which belongs to none of the programmed DM masks. This
  suggests the routing needs something more, for example
  `MTS_GARTEN_WRAPPER_CONFIG` (`0x0B50`), a schedule-enable bit, or acknowledging HW2
  differently.
- The 1 ms poll keeps compute working, at the cost of up to 1 ms of latency per job.
  This is question Q9.

**Clean-room note.** Own firmware only.
