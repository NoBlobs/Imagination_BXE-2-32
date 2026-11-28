<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E051 — Probe of the 3D context store on our firmware

**Question.** Before implementing preemption (`steps.md` phase 8, Q24): does a 3D store
requested by our firmware happen, and which registers show it?

**Board.**
- BPI-F3, kernel `7.4.0-rc1-img`, `powervr` 1.1.0 (no `ctx_switch` parameter: the OS config
  flags stay 0).
- Mesa `pvr-bxe-2-32` (`~/mesa-dev`). Mesa refuses `VK_QUEUE_GLOBAL_PRIORITY_HIGH`
  (`VK_ERROR_NOT_PERMITTED`), so `vkfwtest prio-3d` runs its long job at LOW (firmware
  priority 0) and its probes at MEDIUM (priority 1).
- Firmware built with `EXP=-DEXP_CSW_PROBE` (`1a8f5b1a…` and a rebuild).

## Probe

- **Request (`frag_csw_probe_request()`, called by the scheduler when a 3D or `FRAG_PR`
  command finds the fragment DM busy):** if the running task is a render (not a partial
  render) of lower priority, trace `CSW_38`, then, if `(ISP_RENDER & 3) != 3`, write
  `BCAST[0x1000] = 1` and trace `CSW_11` (`scheduler.md`, "Preemption").
- **Completion (`frag_csw_probe_done()`, at every fragment completion):** trace
  `ISP_RENDER`, `0x1000`, `0x1020`, `0x0298`, `0x01C8` and `EVENT_STATUS`.
- The stored render is then retired as if it had finished: the probe only observes.

Two runs. The first one never requested a store: the waiting command at the head of the
high-priority queue is the `FRAG_PR` (behind its `FENCE_PR`), not the `FRAG`. The hook now
accepts both.

## Results

```
[1805845] TA finished                                   (high-priority geometry)
[1806212] Higher priority context scheduled for DM 3 ... Current: 0, 0, 0 New: 0, 0, 1
[1806228] 0xe0510001 ISP_RENDER=0 0x1000=0 0x1020=0 0x0298=0 0x01C8=0
[1806241] *** 3D context store start
[1846743] 0xe0510102 ISP_RENDER=0 0x1000=0 0x1020=0 0x0298=0 0x01C8=0 EVENT_STATUS=0x400
[1850708] 0xe0410d01                                    PM_3D_MEM_FREE never came
[1850753] Store Freelist ... DM3: stacktop = 0x10, Alloc Page Count = 6
```

- **The store happens:** the low-priority render ends about 40 k timer ticks after the
  request, instead of running for ~1.7 s, with the normal `PIXELBE_END_RENDER`. There is no
  fault and no reset.
- **The stored render frees nothing:** `PM_3D_MEM_FREE` does not come, and the free list keeps
  its 6 pages. A normal render frees all of them.
- **No register read tells a store from a normal end:** the five registers are 0 in both
  cases, and `EVENT_STATUS` is `0x400` in both. `0x1020` is 0 after the store, so the value
  the resume writes into it (`scheduler.md`: `0x25003`, `0x2E007`, …) comes from a register
  the spec does not name.
- `vkfwtest prio-3d` reports the long job's image as wrong. That is expected, since the probe
  retired the stored render.

## Conclusion

The store request works on our firmware. The resume cannot be written from the spec: what to
save into `rogue_fwif_frag_ctx_state`, from which registers, and how to recognise a stored
render. The questions are in Q40. Until they are answered the request stays behind
`EXP_CSW_PROBE`, and the default build has no preemption.
