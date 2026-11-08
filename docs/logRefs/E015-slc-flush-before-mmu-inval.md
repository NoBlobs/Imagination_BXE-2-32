<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E015 — MMU fault in a new memory context: flush the SLC before MMU invalidation

**Question.** After E013, a later run of `copy_ssbo_multiple_groups` failed with an MMU
fault. In E013 the same case had passed. Why?

**Observation** (build `8c200280…`):

```
[5360359] : Setup register set=2 DM=4, PC address=0x0000000082f97000, OSid=0, NewPCRegRequired=1
[5360385] : Kick Compute: FWCtx 0xc0056000 @ 0. …
[5360661] : BIF0 page fault detected (Core 0 MMU Status: 0x0000000000002001
            Req Status: 0x00121fe00004b080)
```

- MMU status `0x2001`: cat base 2 (the slot just bound to a **new** page catalogue).
- Request: read, address `0xE0_0004_B080`, which is inside the **USC code heap**
  (`0xE0_0000_0000`). The shader code of the new context was not visible to the MMU.
- The first kick in that context took the fault. The same test with an earlier context
  (slot 1) had passed.

**Hypothesis.** The host writes the new context's page tables to DRAM with the CPU. The
MMU walks them through the SLC, which can still hold stale lines for those addresses.
The firmware invalidated the BIF caches (`BIF_CTRL_INVAL`) on `MMUCACHE` and on binding
a new catalogue, but it never invalidated the SLC; it only flushed it after DM
completion.

**Change (build `cc8fc482…`).** `slc_flush_inval()` (`SLC_CTRL_FLUSH_INVAL = ALL`, then
poll `SLC_STATUS0` bit 2) now runs:
- in `mmu_cache_op()`, before the BIF and PM TLB invalidation;
- in `bif_activate()`, before writing a new `BIF_CAT_BASEn`.

**Result.**

```
copy_ssbo_multiple_groups      Pass
copy_ssbo_multiple_invocations Pass
copy_ssbo_single_invocation    Pass
(and the whole basic group as in E013: data/deqp-compute-basic-e015.txt)
```

No MMU fault in the runs that followed.

**Conclusion.** It is likely, though not proven since the fault was intermittent, that
the MMU needs the SLC invalidated after the host changes page tables. Flushing the SLC
before every MMU invalidation is cheap, so it is now the default.

**Clean-room note.** Own firmware, public register names.
