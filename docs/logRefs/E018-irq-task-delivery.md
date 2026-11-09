<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E018 — the MTS IRQ task does deliver completions (E016 was a race)

**Question.** The spec answer to Q9 says that the section-B routing alone raises HW2 on
completions. E016 saw only one HW2 per test. Is the routing broken, or the measurement?

**Set-up.**
- Build with `-DEXP_NO_EVENT_POLL`: the 1 ms timer no longer polls `EVENT_STATUS`.
  The BG task still calls `sched_poll()`.
- New counters:

| Register | Content |
|---|---|
| `0x1A18` | CDM kicks issued |
| `0x1A20` | HW2 entries whose `EVENT_STATUS` had `COMPUTE_FINISHED` |
| `0x1A28` | `MTS_INTCTX` at the last HW2 entry |
| `0x1A38` | `MTS_BGCTX` at the last HW1 entry |
| `0x1A70` | HW1 (BG) entries with a DM completion already pending |

**Observation** (cumulative after each test):

```
test                          CDM kicks  HW2 w/ COMPUTE  HW2 total  BG w/ event
empty_shader                       1           0             1          0
copy_ssbo_single_invocation        2           1             5          0
copy_ssbo_multiple_groups          3           1             9          0
concurrent_compute                 5           3            11          0
(all four pass; MTS_INTCTX / MTS_BGCTX read 0 at entry)
```

In a longer run (`copy_ssbo_*` with TQs): 19 HW2, events handled `0x61C` (COMPUTE,
PIXELBE_END_RENDER, ISP_END_MACROTILE, MMU fault, POWER_COMPLETE), and 0 BG tasks that
entered with a completion pending.

**Analysis.**
- Every job is followed by an HW2. With the timer poll disabled, completions still
  arrive within the same ~0.25 ms as before, so they come from HW2 or from the BG
  task's poll.
- When an HW2 arrives *without* the completion bit, the event was consumed first by
  `sched_poll()` in a BG task that was running when the job ended. Interrupts are
  masked inside a handler, so the HW2 is taken right after, and it finds the bit
  already cleared. With the 1 ms timer poll enabled (E016), this race was won by the
  poll even more often. That is why E016 counted a single HW2.
- `MTS_INTCTX`/`MTS_BGCTX` read 0 at entry. As the spec says, they do not tell which DM
  raised the task: the handler has to read `EVENT_STATUS`.

**Conclusion.** Event delivery through `MTS_DMn_INTERRUPT_ENABLE` works as specified.
The polls stay as a harmless backstop, since an event is handled by whichever path sees
it first. E016's conclusion is withdrawn.

**Clean-room note.** Own firmware only.
