<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E012 — `usc_exec_base` → `0x0FC8`, and the CDM stays active

**Question.** Does `0x0FC8` (E008 candidate, bits 39:34) take the USC code base, and
does the empty compute shader then complete?

**Set-up.**
- Build `EXP="-DEXP_PDS_REG=0x610 -DEXP_USC_REG=0xFC8"`, sha256 `58b53c57…`.
- Fresh boot (power cycle, then `img-gpu`), so there was no driver reload. Serial
  capture ran throughout.
- Test case: `dEQP-VK.compute.pipeline.basic.empty_shader`, 40 s timeout.

**Observation.**

```
after load:  0610/0614 = 00000000/000000da   0fc8/0fcc = 00000000/000000e0
             1a30 = da, 1a38 = e0            (firmware read-backs)
trace:       "Kick Compute: FWCtx 0xc0056000 @ 0 …"   (no further DM trace)
during run:  0130 EVENT_STATUS = 0, 12b0 MMU fault = 0, 1a78 busy DMs = 0x10,
             03c0 = 0x7f, 0bb0 = 0
after run:   6030 PERF_COMPUTE_CYCLE: 0xea1bc17e -> 0xedcbfc0a in 0.2 s  (~309 MHz)
             6018 PERF_COMPUTE_PHASE = 0, 6020 TA_CYCLE = 0, 6028 3D_CYCLE = 0
             1008 = 0x00030208        (E009: 0x0001000a; after the E006 fault: 0x00030008)
dEQP:        no result before the timeout, the system stays up
```

**Analysis.**
- `0x0FC8` holds `0xE0_0000_0000 = ROGUE_USCCODE_HEAP_BASE` exactly, and the
  firmware's read-back confirms the write. Whether the hardware *uses* it cannot be
  seen yet, because the task neither faults nor completes, with or without it.
- `PERF_COMPUTE_CYCLE` counts at the core clock (307.2 MHz configured) while the TA and
  3D cycle counters stay at 0. The compute pipeline is therefore **active and stuck**.
  It has not finished with a lost completion event. (Caveat: the counter's value
  before the kick was not sampled in this run.)
- There is still no MMU fault. The `0x7B80` fault of E011 came after a driver reload
  with a stuck task, so it may not belong to this kick.

**Conclusions.**
- With the PDS base set, the CDM runs, but something it waits on is not configured.
  Candidates: the remaining `rogue_fwif_compute_regs` fields (`tpu`, `cdm_item`,
  `compute_cluster`, `tpu_tag_cdm_ctrl`, `cdm_resume_pds1`), USC/PDS unit
  initialisation, or the context-store state.
- The reliable observable for the CDM's state is `PERF_COMPUTE_CYCLE` (0x6030), which
  is public.
- No SoC hang on a fresh boot, which again points to the driver reload as the cause of
  the hangs in E009 run 1 and E011.

**Clean-room note.** Own firmware, public register names, black-box observation.
