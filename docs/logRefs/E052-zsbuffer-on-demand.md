<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E052 — On-demand ZS/MSAA buffer backing for partial renders

**Question.** Implement the last SPM piece left out (Q23, Q30 item 4): before a partial
render, ask the host to back the ZS and MSAA buffers that it creates unbacked.

## Sources

- `partial-render-spm.md`: the buffers are requested through FWCCB `ZSBUFFER_BACKING` and
  confirmed with KCCB `ZSBUFFER_BACKING_UPDATE`. The request is only made when
  `cmd_geom.pr_buffer_fw_addr[]` is non-zero.
- Public `pvr_rogue_fwif*.h`:
  - `rogue_fwif_prbuffer` (`buffer_id`, `on_demand`, `state` with `UNBACKED`, `BACKED`,
    `BACKING_PENDING`, `UNBACKING_PENDING`);
  - `rogue_fwif_fwccb_cmd_zsbuffer_backing_data` (`zs_buffer_id`);
  - `rogue_fwif_zsbuffer_backing_data` (`zs_buffer_fw_addr`, `done`);
  - `pr_buffer_fw_addr[]` at offset 8 of `cmd_geom_frag_shared`.
- Public trace strings: `SPM` ids 21–31, 42 and 46–52, and `CLEANUP` ids 8 and 9.
- The points the spec leaves open are Q41. The code follows its stated assumptions.

## Firmware changes

- **`src/geom.c`:** the geometry kick passes `pr_buffer_fw_addr[]` to
  `spm_geom_kick()`.
- **`src/pm.c`, when a partial render is armed** (`start_partial_render()`):
  `prb_back_all()` checks each buffer:
  - none: `SPM_21` / `SPM_46`;
  - not on demand, or already backed: `SPM_23` / `SPM_47`;
  - unbacked: the state becomes `BACKING_PENDING`, `FWCCB_ZSBUFFER_BACKING` is sent
    (`SPM_26` / `SPM_49`), and the trace says the partial render is waiting (`SPM_30` /
    `SPM_31`);
  - pending: `SPM_28` / `SPM_51`.

  If a buffer is missing, the SPM enters the new state `SPM_WAIT_BACKING` instead of
  `SPM_WAIT_HW`.
- **`src/pm.c`, `spm_zsbuffer_update()`** (from `src/kccb.c`, both update commands):
  - records the new state (`SPM_22`, `SPM_42`, `SPM_24`);
  - arms the partial render once every buffer is backed, then runs the scheduler;
  - a failed backing leaves the TA stopped.
- **`src/frag.c` → `spm_zsbuffer_release()`:** when the HWRTData's final render finishes,
  the buffers backed for its partial renders are unbacked (`FWCCB_ZSBUFFER_UNBACKING`,
  `SPM_27` / `SPM_50`).
- **`src/sched.c`:** `CLEANUP_ZSBUFFER` is busy while a request is pending or the buffer is
  still in use (`CLEANUP_9`), and frees it otherwise (`CLEANUP_8`).

The build has no warnings; `check-trace-args` and `fw-preflight` pass. Firmware `e9474a9b…`.

## Results

| Run | Result |
|---|---|
| `bigpb` | pass |
| `bigpb-layers` | pass: 140 free-list grows and no partial render, so the new path did not run |
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** |
| `stress` | **50/50**, no reset |

**Conclusion.** The protocol is in place and causes no regression, but **it is not tested**:
- the mainline kernel never fills `pr_buffer_fw_addr[]`, and Mesa does not either;
- the kernel does not handle `FWCCB_ZSBUFFER_BACKING`;
- with the current Mesa the free list always grows, so no partial render runs.

A test needs host support (a ZS buffer object, the FWCCB handler and a uAPI) and the
answers to Q41.
