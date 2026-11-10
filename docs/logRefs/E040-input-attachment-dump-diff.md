<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E040 — input-attachment render: full register diff, shared PB rule, and a board hang

**Question.** The spec now has a register dump of the reference firmware during
`formats.d32_sfloat.input.clear.dont_care.draw` (`docsClean/input-attachment-dump.md`,
the dirty team's host-side observation). Which of our registers differ in the second
render, where our firmware faults on an IPF control read at `0x2000`?

**Method.** Knob `EXP_FRAG_DUMP`: just before each fragment launch, the firmware traces
every non-zero register in the dump's ranges (`0x0600–0x07FC`, `0x0F04–0x0FFC`,
`0x1500–0x17F0`, `0x4000–0x40F8`). A script compares our second render with the last
"during the render" value of the spec's table.

**Differences found** (second render):

| Register | Reference | Ours | Note |
|---|---|---|---|
| `0x0F18` | `0x20002` during the render | `0x20002` | **confirms `isp_mtile_size`** (E039's revert was right) |
| `0x1790`/`0x1794` | `0x20_0001C400` | 0 | same value as the border-colour table `0x17A0` |
| `0x06D0` | `0x7F` | `0x1F` | constant in the reference |
| `0x0708` | 1 | 0 | constant in the reference |
| `0x1500`, `0x1780`, `0x1788`, `0x178C`, `0x17B8` | `0x80`, `0x21`, `0x20`, `0x20`, `0xA` | 0, `0x40`, 0, 0, 0 | the first-load unit values of Q16i |
| `0x06A0`–`0x06BC` | set | 0 | `PDS_BGRND*`; the earlier answer says 0 during both renders, so they come from a transfer in between |
| PBE `0x1510`/`0x1550`, `0x0F88`, `0x40D8`/`0x40E0` | — | — | the table's last value is the read-back transfer's or a counter's; ours match the second render's values in `kick-compute-and-3d.md` |

**Experiments** (each on a fresh module load):

| Change | Result |
|---|---|
| `0x1790` ← `tpu_border_colour_table` (now default; `EXP_NO_PIXEL_BORDER` removes it) | still faults |
| plus `0x06D0 = 0x7F`, `0x0708 = 1` and the unit values (`EXP_FRAG_UNIT_EXTRA`) | still faults; these ranges now match the reference |
| **shared-PB rule** (`EXP_SHARED_PB`; off by default after the hang) | the rule behaves as designed (`0x02B0` = 3 at the first HWRTData, **0** at the second, no reload); still faults |

**Shared-PB rule.** The reference's trace line `CONTEXT_PB_BASE set to 0x3, FL different
between TA/3D: local: 1, global: 1`, together with the Q16m answer, gives:
- bit 0 = the geometry's local free list differs from the one PM context 1 (3D) holds;
- bit 1 = the same for the global free list;
- only a differing free list is loaded into context 0.

`frag.c` records the free lists it loads into context 1 (`pm_ctx1_fl[]`), and the geometry
kick compares against them.

**Board hang.** With the shared-PB rule, `simple_draw` 4/4 and `api.smoke` 8/8 passed. Then
**the SoC stopped responding during `basic_draw`**: no network, no serial output. It needs a
power cycle. Until the board is checked, the shared-PB rule is **suspect**:
- it skips the context-0 load when context 1 holds the same free lists;
- after HWR or a transfer, context 0's live state may not be what the rule assumes.

The installed firmware at the time was this experimental build. The `img-gpu` kernel
entry blacklists `powervr`, so the board still boots.

**After the power cycle.** The same `basic_draw` group with the rule turned off (build
`687ff605…`, `EXP_NO_SHARED_PB` at the time): **203/331 pass, 0 fail, no reset, no hang**.
The rule is therefore the most likely cause of the hang. It is now **off by default**:
`EXP_SHARED_PB` turns it on, and `CONTEXT_PB_BASE` is 3 with both free lists loaded at
every first geometry kick, as before. The `0x1790` write stays.

**Conclusions.**
- In the ranges the reference dump covers, our second render now matches. The fault comes
  from something outside them: the PM state, or memory the IPF reads (region headers or
  control streams).
- Q16n asks for:
  - the IPF-related registers outside these ranges (`0x0800–0x0BFF`, `0x1000–0x14FF`,
    `0x1800–0x3FFF`);
  - whether the reference uses the HWRTData's `macrotile_array_dev_addr` / `rtc_dev_addr`,
    which our kicks never write.

**Clean-room note.** Own firmware and measurements; reference values from the spec's
observation.
