<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E035 — free-list load commands: the PM allocates and the first TA job finishes

**Question.** The firmware writes 1 after the value registers, then waits for bit 0 to
return to 0. The command clears itself within a few µs, which is why no sampling ever
saw it. Does it unblock the TA?

**Registers tested**:

| Free list | PM context | Value registers written first | Load command |
|---|---|---|---|
| local | 0 (geometry) | base `0x0210`/`0x0214`; stack top in the high word of `0x0220`/`0x0224` (low word 0); allocated PB / MMU page counts `0x0390`/`0x0394` | `0x0208` |
| local | 1 (fragment) | base `0x0218`/`0x021C`; stack top in `0x0230`/`0x0234` from bit 22; counts `0x0398`/`0x039C` | `0x0200` |
| global | 0 (geometry) | bank `0x20B8`: base `+0x00`, top `+0x08`, counts `+0x18`/`+0x1C` | `0x20B0` |
| global | 1 (fragment) | bank `0x2078`: base, top `0x2080`, counts `0x2090`/`0x2094` | `0x20A8` |

Rules: value registers first, then the command, then the poll (bit 0 = 0) must finish
before the next free list; the local free list goes before the global one.

The answer also **corrects** the earlier register table: the local counts of PM context 0
are `0x0390`/`0x0394`. `0x0398`/`0x039C`, which we had been writing since E029, belong to
PM context 1. The poll of `0x20B0` was inferred by the dirty team from the other three
commands.

**Change in `src/geom.c`** (`-DEXP_TA -DEXP_HS_MODE=0 -DEXP_NO_F320`, build `cac790a8…`;
knob `EXP_NO_FL_LOAD` turns the commands off):
1. local: `0x0210`, `0x0224` (64-bit write with the top in the high word),
   `0x0390`/`0x0394`, then `0x0208 = 1` and poll bit 0 = 0;
2. global: `0x20B8`, `0x20C0`, `0x20D0`/`0x20D4`, then `0x20B0 = 1` and poll bit 0 = 0;
3. the rest as before (E033/E034 flags, `0x02B0 = 3`, MLIST, ALIST/MLIST SPs, phase C/D,
   VDM kick). No `0x0250`/`0x0258` handshakes, no `0xF320` write.

**Result** (`simple_draw_triangle_list`; firmware trace reads right after each poll, then
host samples):

```
0xE0280B50  poll ok = 1   0x0348 = 0x1F     <- local live SP loaded
0xE0280B51  poll ok = 1   0x20C8 = 0x1BF    <- global live SP loaded
Kick TA: … First kick:1 …
  … 1.3 ms later: ALIST SP 0x0270 = 2, MLIST SP 0x02C0 = 9, VDM 0x0410 = …13F040,
                  0x0348 = 0x10, PM_CAT_BASE_VCE0 = 0x84DD4001 (valid)
TA finished
```

| Register | Before (E028–E034) | Now |
|---|---|---|
| `0x0348` local live SP | 0 | `0x1F` at load, `0x10` after the job (15 pages taken) |
| `0x20C8` global live SP | 0 | `0x1BF` |
| `PM_CAT_BASE_VCE0` | 0 | valid page (bit 0 set), filled by the PM |
| `0x0270` / `0x02C0` (ALIST / MLIST SP) | 0 / 0 | `2` / `9`: the values the spec reports for this draw |
| `0x0D00` | `0x0F` (stalled) | back to `0x3F` (idle) |
| geometry | stalled until the 30 s deadline, then HWR | **`TA finished` after ~1.3 ms** |

Both polls succeed on the first read, as for the other self-clearing commands we use.

**Conclusions.**
- **The load commands are the missing step of E028–E034.** With them, the PM loads both
  free lists, fills its page catalogues and the ALIST/MLIST, and the geometry runs to the
  end. The values match the spec's observation of the original exactly (`ALIST = 0x2`,
  `MLIST = 0x9`, local stack top `0x1F` → `0x10`).
- The live copy needs no handshake (`0x0250`/`0x0258`), no `PM_PAGE_MANAGEOP` pause and no
  `0xF320` write. `0x20F8`, `0x03D0`/`0x03D4` and `0x0240` are still written as observed;
  whether any of them is required is not tested yet.
- The dEQP case still fails, because fragment commands are retired without running. The
  next step is the fragment kick, with the PM context 1 load done through the same commands
  (`0x0200`, `0x20A8`).

**Clean-room note.** Own firmware, own measurements and own register reads, the spec's
published observations.