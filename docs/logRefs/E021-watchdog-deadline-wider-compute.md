<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E021 — watchdog uses the context deadline; wider compute groups pass

**Question.** How does the compute path behave beyond the `basic` group?

**First run** (build `5f3bc70a…`, 2 s watchdog):

| Group | Pass | Fail | Not supported |
|---|---|---|---|
| `dEQP-VK.compute.pipeline.indirect_dispatch.*` | 24 | 0 | 12 |
| `dEQP-VK.compute.pipeline.builtin_var.*` | 10 | **1** | 0 |

The failure was `builtin_var.global_invocation_id`. Eight sub-cases passed (up to
7392/7392 values). The ninth, a large dispatch, was cut short:

```
[29115073] : Kick Compute: FWCtx 0xc0056000 @ 184. …
[31515118] : Lockup detected on DM4, FWCtx: 0xc0056000     (exactly 2 s later)
```

The job was not hung. It took longer than the firmware's 2 s watchdog.

**Change (build `79910dad…`).** The watchdog limit is now the context's own
`rogue_fwif_fwcommoncontext.max_deadline_ms`. The mainline host sets 30 000 ms
(`pvr_queue.c`), and its own job timeout is 60 s. The fallback is 30 s.

**Second run.**

```
dEQP-VK.compute.pipeline.builtin_var.*   Passed: 11/11
```

**Conclusion.**
- The watchdog has to follow the host's per-context deadline. A fixed short limit
  discards legitimate long jobs.
- Supported-case totals: basic 67/73, the 6 failures all images (Q13);
  indirect_dispatch 24/24; builtin_var 11/11.

**Clean-room note.** Public host source (deadline value), own firmware.
