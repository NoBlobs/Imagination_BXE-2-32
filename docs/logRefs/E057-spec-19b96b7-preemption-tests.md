<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E057 — Board tests of E056, `docsClean` `19b96b7` (CDM store, `0x0F28`), 3D preemption working

**Question.** Test the E056 implementation of Q40–Q42 on the board, and apply `docsClean`
`19b96b7`:
- TA and CDM context store and resume (`scheduler.md`);
- the resolved `TO CONFIRM` items (`kick-compute-and-3d.md`, `kick-geometry-ta.md`).

## Changes from `19b96b7`

- **`0x0F28` = `hwrtdata_common.rgn_header_size` (`+0x4C`):** was `4 × mtile_stride`
  (Q16l, E038). It is now written from the field (`src/frag.c`).
- **`CONTEXT_PB_BASE`, bit 1 = local, bit 0 = global:** confirmed. `pm.c` already wrote it
  that way.
- **CDM context store and resume** (`src/kick.c`):
  - gated by `ROGUE_FWIF_INICFG_OS_CTXSWITCH_CDM_EN` (bit 3), or by `EXP_CSW_FORCE`;
  - **request:** `BCAST[0x0488] = 1` (`CSW_5`) when a higher-priority compute waits;
  - **end** at `COMPUTE_FINISHED`: poll `CR[0x04A0]` bit 0. Bit 1 means stored: the context
    flags get the core mask (`CSW_4`, `CSW_1`) and the command stays at the head of its queue;
  - **resume:** `CR[0x04D8]` = the A/B resume state, `CR[0x04E0] = cdm_resume_pds1`,
    `BIF_CTRL_INVAL = 0x1C`, trigger `CR[0x0490] = 1` instead of `0x0478` (`CSW_2`);
  - **assumptions:**
    - the A/B selector is `rogue_fwif_compute_ctx_state.ctx_state_flags` bit 0, and the
      firmware never flips it;
    - "the A/B resume state buffer address" is `cdm_resume_pds0` or `cdm_resume_pds0_b`;
    - an HWR that retires a compute also clears its resume flag.
  - The stored context yields to the one that preempted it, as for the 3D.
- **TA context store: not implemented.** The register list names "task0/1/2_size", which
  match no field of the public `rogue_fwif_geom_registers_caswitch`. On this stack the
  reference satisfies a TA store only at the end of the TA anyway (Q24). → Q43.

## Fix found on the board: 3D store detection (E056 bug)

`kick_events()` clears `PIXELBE_END_RENDER` (bit 3 of `CR_EVENT_STATUS`) before calling
`frag_csw_end()`. That function then read the register and saw the bit clear, so every
stored render was taken for a normal end and retired half-drawn. `prio-3d` failed with
61440/65536 wrong pixels in the low job.

`frag_csw_end()` now receives the event bits that ended the render. It also traces
`CSW_10` (event status, `CR[0x1038]`, `CR[0x1008]`). A typical store reads `0x418, 0x2,
0x40005000`.

A second gap: only a waiting `FRAG`/`FRAG_PR` requested a store. The probe's read-back copy
(`TQ_3D`, a separate transfer context) then waited behind the resumed low job for up to
1.4 s. Now any higher-priority task waiting for DM 3 requests the store, as the spec says
("a context of higher priority becomes ready on a DM").

## Results (board, kernel `7.4.0-rc1-img`, Mesa `~/mesa-dev`)

**Default build** (`4d09a808…`; `.text`/`.data` identical to the tested `69bc1384…`):

| Run | Result |
|---|---|
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** |
| `stress` | **30/30** |
| `prio-3d`, `prio-ta`, `prio-cdm` (`-p low`) | pass, no preemption (flags 0), no reset |
| E053 HWR scenario (`multiframe` beside `hwr-frag`) | **1 wrong frame** (the one lost in the HWR), **2 resets**, **no page fault, no second HWR** |

**Q42 confirmed.** After the HWR, the trace shows the orphaned 3D discarded:

```text
Discarded cmd on DM3 (reason=1) HWRTData=0xc00bb000 (st: 9), FWCtx 0xc0079098 @ 30168
```

Before (E053/E054) there were 3 wrong frames, 3–5 resets and a page fault at address 0.

**`EXP_CSW_FORCE` build, 3D preemption (Q40):**

```text
Higher priority context scheduled for DM 3 … Current: 0, 0, 0 New: 0, 0, 1
*** 3D context store start
3D context store pipe state: 0x00000418 0x00000002 0x40005000
*** 3D context store complete
3D FWCtx 0xc00f6098 needs resume
3D finished, HWRTData0State=6, …
Kick 3D: FWCtx 0xc0173098 … (prio:1)                       ← the higher priority runs
*** 3D FWCtx 0xc00f6098 resume
Kick 3D: FWCtx 0xc00f6098 @ 4168 … CSW resume:1 (prio:0)  ← same command, resumed
```

| `prio-3d -p low`, 3 runs | Result |
|---|---|
| Stores requested / completed / resumed | 36 / 35 / 35 (one request came too late: normal end) |
| Low job's pixels | right |
| Probe latency during the low job | **avg 64–70 ms, worst 116–122 ms** (no preemption: avg 152 ms, worst 1430 ms). The vendor stack measures 89–138 ms |
| Resets / faults | 0 / 0 |

With this build, `-k all` gives 48/2, `stress` 20/20, and `prio-ta` passes.

**CDM preemption:** every store ends in a page fault at `0xd0` and an HWR; the stored
command is discarded. This is **exactly what `scheduler.md` reports for the original
firmware on this stack**: "CDM context switching is effectively unusable" with the mainline
host and Mesa, probably because of the host's context-switch PDS programs. It stays off by
default (`OS_CTXSWITCH_CDM_EN` = 0).

## Conclusion

- 3D preemption works and matches the reference protocol. It is off by default, since the
  mainline kernel cannot set `OS_CTXSWITCH_FRAG_EN`.
- The Q42 discard removes the second HWR.
- The CDM store behaves as the original firmware does (unusable on this host).
- The TA store waits for Q43.
