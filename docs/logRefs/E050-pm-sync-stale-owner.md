<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E050 — The remaining `stress` failures: a geometry's free list lost by the PM sync

**Question.** After E049, `stress` still failed about 1 run in 25 (3 of 72 runs with the
clang, GCC 12 and GCC 16 builds), while the manufacturer firmware passes it. The failing runs
show `Guilty overrunning` or `Guilty lockup` resets and a page fault at address 0. What causes
them?

**Board.**
- BPI-F3, Arch Linux RISC-V, kernel `7.4.0-rc1-img` with `powervr` 1.1.0.
- Mesa `pvr-bxe-2-32` (`~/mesa-dev`).
- Our firmware, built with GCC 16.2.0 (`-std=gnu23`).

## Method

- `stress` runs in a loop with the firmware trace on (mask `0x80007ffe`). A snapshot of
  `trace_0` is saved every second, and another one at each reset in dmesg.
- A checker (`flcheck.py`, kept out of the tree) replays the `Load/Store Freelist` and
  `Kick TA` / `TA finished` lines. It flags any free-list load whose memory copy is older than
  allocations that a geometry made on that list and that were never stored.

## The failure

Run 17 of the second loop failed ("frame 4: 65536 of 65536 pixels wrong", reset reasons
`Guilty lockup` and a page fault at address 0). The checker found exactly one stale load in
38 s of traces, in that run:

1. A 3D of HWRTData `0xc01bd000` loads free list `0xc01bb000` into PM context 1. The context
   stores it at its end, and `pm_slots[1].fl[0]` keeps `0xc01bb000`.
2. The geometry of HWRTData `0xc01be000` (same render context, same list) loads it into PM
   context 0 and allocates about 6 pages. `TA finished` stores nothing: the live state stays in
   the registers, with `pm_slots[0].dirty` set.
3. A geometry of **another** render context takes PM context 0. In
   `pm_sync_load_freelists()`, the list context 0 held is also the one context 1 holds, so the
   rule "the other context owns it: no store from this context" skips the store. But context 1
   finished long before: its copy is the old one.
4. The 3D of `0xc01be000` loads the stale copy (stack top `0x1f`, 0 pages allocated) and frees
   the geometry's 6 pages. Its store at the end shows `Alloc Page Count = 4194298` (-6 in
   22 bits). The PM then hands out pages still in use: wrong pixels, lockup, page fault.

The interleaving needs a geometry of another context to start between the geometry and the
3D of one render, so it only happens when four devices share the GPU.

## Fix

`src/pm.c`, `pm_sync_load_freelists()`:
- every load is numbered (`pm_slot.seq`);
- the other PM context owns a list only while it runs, or when it loaded the list after this
  one did. Otherwise this context's registers are the newest copy, and they are stored before
  the new load.

The rule for a running context, from the Q38 answer, is unchanged.

## Results

Firmware `f2da21a8…`:

| Run | Result |
|---|---|
| `stress`, trace on | **300/300**, no reset; the checker finds 0 stale loads in 479 s of traces |
| `stress`, trace off | **100/100**, no reset |
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** |

Before the fix, the same build gave 39/40 untraced, and 100/100 then 16/17 traced.

**Conclusion.** The intermittent `stress` failure was ours, not Mesa's: the cross-context PM
sync dropped a geometry's free-list state when the other PM context held an old copy of the
same list.
