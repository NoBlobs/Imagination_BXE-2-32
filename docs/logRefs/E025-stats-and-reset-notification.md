<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E025 — utilisation statistics, HWR log and context-reset notification

**Question.** Do the host-visible statistics and the robustness path work with the
mainline driver?
- GPU utilisation and time correlation (`rogue_fwif_gpu_util_fwcb`);
- the HWR log (`rogue_fwif_hwrinfobuf`);
- the FWCCB `CONTEXT_RESET_NOTIFICATION`.

## What the mainline host consumes (public source)

- `pvr_fw.c` allocates `gpu_util_fwcb` (initial `last_word = IDLE`) and `hwrinfobuf`,
  and passes their addresses in sysinit and osinit. No code in the driver reads them back
  today. They are the host's view for debugging and future use.
- `pvr_ccb.c`: `CONTEXT_RESET_NOTIFICATION` → `pvr_dump_context_reset_notification()`
  prints the context, reason, DM, job ref and page-fault address. `UPDATE_STATS` is
  accepted and ignored.
- HWPerf: the driver allocates no HWPerf buffer and reads no packets. The HWPerf KCCB
  commands are acknowledged with `CMD_EXECUTED`, which `hwperf-trace.md` allows for a
  mainline-compatible firmware.

## Implementation (`src/stats.c`, build `7d597e9c…`)

**Time base.** The mainline host writes no time-correlation entries, so the firmware
publishes its own:
- `time_corr[seq]` gets `cr_timestamp = CR_TIMER`, `os_timestamp` in ns derived from it,
  and `cr_delta_to_os_delta_kns = K`;
- `K = (256·10⁹ << 20) / core clock`, so that `delta_ns = (delta_cr·K) >> 20`
  (`ROGUE_FWIF_GET_DELTA_OSTIME_NS`);
- this happens at every (warm) boot and on `CORECLKSPEEDCHANGE`.

**Utilisation.** `stats_set_state()` is called from the scheduler's power-state update:
- ACTIVE while any DM is busy or any context has pending commands;
- BLOCKED in that same situation while the host holds forced idle;
- IDLE otherwise, and at power-off.

On each change it adds the elapsed ns to `stats_counters[old state]` and writes
`last_word = time | state`. It never subtracts across a backwards jump of
`CR_TIMER`, which restarts after a power-off. Going idle also writes
`sysdata.start_idle_time`.

**Events.**
- Trace `POW_2` "GPU idle (might be powered down)" when going idle and `POW_22` "Cancel
  idle timer" when leaving idle.
- `POW_20` when the host cancels forced idle.
- Debug group `DBG_20`: the idle/active/blocked totals in µs.

**HWR log.** For every command discarded by `hwr_recover()`, `stats_hwr_log()` writes a
`rogue_hwrinfo` entry. Each entry carries:
- the BIF request and MMU status for an MMU fault;
- the page catalogue and PID;
- `EVENT_STATUS`, the DM and `hwr_type` (`BIF0FAULT` for a fault, `OVERRUN` for the
  watchdog);
- the kick, reset-start and reset-finish `CR_TIMER` times.

The first 8 entries are kept, and the last 8 form a ring (`write_index`). The buffer also
keeps `hwr_counter` and the per-DM locked-up, overran and recovered counters. All layouts
are checked with `static_assert` against the public `pvr_rogue_fwif_check.h`.

**Notification.** Per discarded command, `CONTEXT_RESET_NOTIFICATION` is sent with:
- `server_common_context_id` of the context;
- reason `GUILTY_LOCKUP` (MMU fault) or `GUILTY_OVERRUNING` (watchdog);
- the DM and `reset_job_ref` (`ext_job_ref` of the running command);
- the page catalogue;
- for a fault, flag `PF` and `fault_address` = `BIF_FAULT_BANK0_REQ_STATUS.ADDRESS`.

## Results

**Normal operation** (default build, trace mask `0x80007FFE`):
- `compute.pipeline.basic` 73/73 and `fill_and_update_buffer` 72/72, as in E023.
- The utilisation totals grow across jobs and warm boots. Example sequence, in µs:
  idle 7 738 792 / active 126 823 / blocked 0 → 7 747 177 / 128 299 / 0.
- "GPU idle" and "Cancel idle timer" appear around every job.
- No reset notification during normal runs.

**Overrun path** (`-DEXP_WATCHDOG_MS=1`, `vkfwtest pre-cdm`, long compute jobs):

```
powervr cac00000.gpu: [drm] Received context reset notification on context 2
powervr cac00000.gpu: [drm]   Reset reason=3 (Guilty overrunning)
powervr cac00000.gpu: [drm]   Data Master=4 (Compute)
powervr cac00000.gpu: [drm]   Job ref=1
… (29 notifications, one per discarded job; trace "Finished hardware reset (HWR Counter=28)")
```

**Page-fault path** (`-DEXP_NO_PDS_BASE`, which reproduces the E006 fault):

```
powervr cac00000.gpu: [drm] Received context reset notification on context 2
powervr cac00000.gpu: [drm]   Reset reason=1 (Guilty lockup)
powervr cac00000.gpu: [drm]   Data Master=4 (Compute)
powervr cac00000.gpu: [drm]   Job ref=1
powervr cac00000.gpu: [drm]   Page fault occurred, fault address=26c0
```

The address matches E006 exactly.

**Notes.**
- The mainline host only logs the notification. It does not mark the context as lost,
  so a Vulkan application is not told about device loss. That is a host limitation; the
  firmware side is complete.
- `-DEXP_TQ_FORCE_FAULT` (the E017 conditional writes) no longer causes a fault. With
  the E023 launch order those writes are harmless, so the address-0 fault needed both
  bugs together.
- The 2.4 s "large dispatch" of E021 does not happen anymore: every
  `builtin_var.global_invocation_id` kick now ends in ~0.3 ms. The long run in E021 was
  probably a completion that was found late, before the E023 completion flush existed.

**Clean-room note.** Public structures and host source, own firmware.
