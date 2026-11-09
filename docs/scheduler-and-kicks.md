<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Scheduler, client CCBs and data-master kicks

## Data masters

| DM | id | Work | cCCB task types | State |
|---|---|---|---|---|
| GP | 0 | firmware internal | – | – |
| TDM | 1 | transfer | `TQ_TDM` (206), `TQ_2D` (208) | retired at once (`MAIN_172`); not seen from Mesa on this GPU |
| GEOM | 2 | geometry / TA | `GEOM` (201) | retired at once |
| FRAG | 3 | fragment / 3D | `FRAG` (203), `FRAG_PR` (204), `TQ_3D` (202) | `FRAG`/`FRAG_PR` retired at once. `TQ_3D` is kicked: the pipe starts, but its output faults at address 0 and HWR discards it ([E017](logRefs/E017-tq3d-launch-fault-at-zero.md)). |
| CDM | 4 | compute | `CDM` (205) | **works**: `dEQP-VK.compute.pipeline.basic`, 67/82 pass ([E019](logRefs/E019-hwr-and-compute-basic-67.md)) |

A task that is not implemented is **retired immediately**: its updates still run and
the host's fences signal. The host therefore never hangs, but the work is not done.
Rendering tests fail on content, not on timeouts.

## Context table (`sched.c`)

A KICK names a context through `context_fw_addr`, a `rogue_fwif_fwcommoncontext`. The
firmware keeps up to 64 contexts in `ctxs[]`. Each entry has its address, its DM, a
`pending` flag (unread commands), a `dead` flag (bad cCCB) and a FIFO of up to 8 kicks'
cleanup control blocks.

### `sched_kick(kick)` (from `KCCB_KICK`)

1. Find or create the context entry.
2. For each of the kick's cleanup control blocks (up to 3), increment
   `submitted_commands`. Queue the blocks in the context's FIFO so that
   `executed_commands` can be incremented when that kick's task retires.
3. Copy `client_woff_update` and `client_wrap_mask_update` into the context's cCCB
   control block (`write_offset`, `wrap_mask`).
4. Mark the context pending and run the scheduler.

## Client CCB walk (`ctx_process`)

A cCCB is a byte ring of commands. Each command has a 0x28-byte
`rogue_fwif_ccb_cmd_header` (`cmd_type`, `cmd_size`, `ext_job_ref`, `int_job_ref`, work
estimation) followed by `cmd_size` bytes of payload. The next command starts at
`(roff + 0x28 + cmd_size) & wrap_mask`. Bit `0x8000` of the type marks a **task**,
and the high half must be the magic `0x2ABC`.

```
loop (at most 64 commands per pass):
    roff == woff                 → context idle (pending = false, dep_offset = roff)
    a task of this context runs  → stop
    bad magic                    → MAIN_171, context dead
    TASK:
        DM busy or forced idle   → stop (retry later)
        kick_task() == running   → stop, command stays at read_offset
        kick_task() == done      → retire now (cleanup FIFO pop), continue
    FENCE / FENCE_PR             → every UFO satisfied? else stop and record last_failed_ufo
    UPDATE / RMW / UNFENCED_*    → write (or add) values, notify host
    PRIORITY                     → ctx.priority
    PADDING / POST_TIMESTAMP     → skip
```

UFO semantics:

- **FENCE**: `*addr == value`.
- **FENCE_PR**: signed `*addr − value ≥ 0`. Mainline Mesa uses this for timeline
  dependencies.
- **UPDATE**: `*addr = value`.
- **RMW_UPDATE**: `*addr += value`.
- If bit 0 of the UFO address is set, the address points to a **sync checkpoint**.
  The fence is satisfied when it is `SIGNALED` or `ERRORED`, and an update writes
  `SIGNALED`.

`sched_run()` walks the pending contexts round-robin. It repeats while any context
made progress, up to 8 passes. It then updates `pow_state` and notifies the host once
if anything visible changed.

### Completion

The kick records `{ctx, cmd_off, event_mask, finish_sfid, kick_time, bif_slot}` in
`dms[dm]` and sets bit `dm` in scratch `0x1A78`. When an `EVENT_STATUS` bit in
`event_mask` appears, `kick_events()` runs. The event can arrive through the IRQ task
(HW2) or through `sched_poll()` on the timer. `kick_events()` does the following:

1. Clear the event and flush and invalidate the SLC: `SLC_CTRL_FLUSH_INVAL = 1`, then
   poll `SLC_STATUS0` bit 2.
2. Clear the busy bit, trace the finish SFID, and call `sched_dm_done()`.
3. `sched_dm_done()` advances `read_offset` past the task and pops the cleanup FIFO,
   which bumps `executed_commands`. It marks the context pending again, so that the
   updates after the task run in the same pass.

### Cleanup (`KCCB_CLEANUP`)

| Type | Answer |
|---|---|
| `FWCOMMONCONTEXT` | `CLEANUP_BUSY` while a task of the context runs, or while its cCCB is not empty (unless dead). Otherwise the entry is freed and 0 is returned. |
| `HWRTDATA`, `FREELIST`, `ZSBUFFER` | `CLEANUP_BUSY` while a running DM uses that render target, otherwise 0 |

The host retries `BUSY` cleanups.

## BIF catalogue slots (`kick.c`)

Each client memory context (`rogue_fwif_fwmemcontext.pc_dev_paddr`) must be bound to
one of the `ROGUE_CR_BIF_CAT_BASEn` registers (`0x1200 + 8n`) before a DM uses it:

- Slots 1..7 are used, and slot 0 is left alone. A context already bound keeps its
  slot. Otherwise the next slot not used by a busy DM is taken round-robin. It gets
  `CAT_BASEn = pc & 0xFF_FFFF_F000`, followed by `BIF_CTRL_INVAL = 0xF` (PT, PD, PC,
  TLB).
- The slot is written back into `fwmemcontext.page_cat_base_reg_set`.
- The DM is pointed at the slot through `ROGUE_CR_BIF_CAT_BASE_INDEX` (`0x1240`).

  | DM | Field shift in `CAT_BASE_INDEX` |
  |---|---|
  | GEOM (TA) | 0 |
  | FRAG (3D) | 8 |
  | CDM | 16 |
  | TDM | 19 |

On the board, the first compute kick produced `CAT_BASE1 = 0x860B3000` and
`CAT_BASE_INDEX = 0x00010000` (CDM → slot 1). The MMU fault report then named
**cat base 1**, so the binding itself works ([E006](logRefs/E006-compute-kick-mmu-fault.md)).

## Compute kick (`kick_cdm`)

It follows `docsClean/kick-compute-and-3d.md`. `BCAST[x]` is a write through the XPU
broadcast bank at `0xCFA00000`.

| Step | Write | Source |
|---|---|---|
| 1 | read `MULTICORE_SYSTEM` (`0xF308`); `BCAST[0xF330] = execute_count << 8 \| 1` | `cmd_compute.execute_count` |
| 2 | BIF slot, `CAT_BASE_INDEX` CDM field | `ctx.fw_mem_context_fw_addr` |
| 3 | `BCAST[0x0480]` (64-bit) control stream base | `cmd_compute.regs.cdm_ctrl_stream_base` |
| 4 | `BCAST[0x17A8]` (64-bit) TPU border colour table | `regs.tpu_border_colour_table` |
| 5 | `BCAST[0x0498]` (64-bit) context state base | `regs.cdm_context_state_base_addr` |
| 6 | trace `MAIN_122` | |
| 7 | `BCAST[0x04A8]` `CDM_CONTEXT_PDS0`, `[0x04B0]` `PDS1`, `[0x04B8]` `TERMINATE_PDS`, `[0x04C0]` `TERMINATE_PDS1` | `fwcomputecontext.ctxswitch_regs` |
| 8 | `EVENT_CLEAR = COMPUTE_FINISHED`; DM marked busy | |
| 9 | `BCAST[0x0478] = 1`: **CDM run**, always the last write | |

Completion is `EVENT_STATUS` bit 2 (`COMPUTE_FINISHED`).

The remaining `rogue_fwif_compute_regs` fields (`tpu`, `cdm_item`, `compute_cluster`,
`tpu_tag_cdm_ctrl`) are **not written**. Their gating feature or BRN is absent on this
GPU, so the host leaves them unused (`docsClean/kick-compute-and-3d.md`, answer to Q7).

### What made compute work

1. The PDS and USC execution bases and the rest of the GPU units init
   (see [boot-and-memory.md](boot-and-memory.md#gpu-bring-up-gpu_initc)). Without the
   PDS base the CDM faulted at `0x26C0` ([E006](logRefs/E006-compute-kick-mmu-fault.md)).
   Without the USC bases it stayed active forever
   ([E012](logRefs/E012-usc-base-0x0fc8.md)).
2. SLC flush and invalidate before MMU invalidation and before binding a new page
   catalogue. Without it the first job of a new memory context could fault on its
   USC code ([E015](logRefs/E015-slc-flush-before-mmu-inval.md)).

## Transfer kick TQ_3D

`kick_tq3d()` follows the transfer table of `kick-compute-and-3d.md`. The spec's `cmd+N`
offsets are relative to the cCCB header, so `cmd+0x30` = `regs+0`. The full field map is
in [E014](logRefs/E014-tq3d-kick-does-not-start.md):

1. Set `MULTICORE_FRAGMENT_CTRL_COMMON (0xF310) = 0x101` and bind the BIF slot for DM 3
   (`CAT_BASE_INDEX` bits 10:8). The binding is mandatory before any ISP or PBE write.
2. Program the transfer, ISP and PBE registers:
   - `0x06B0` (PDS_BGRND2) is not written.
   - `0x0F20` (`isp_mtile_base`) only when `(isp_render & 3) == 2`.
   - `0x0FD8` = `min(ISP_CTL[15:12] + 1, 2)` (tiles in flight).
   - PBE in two banks: `WORD0_MRTn = 0x1510 + 8n` and `WORD1_MRTn = 0x1550 + 8n`, from
     `pbe_wordx_mrty[2n]`, `[2n+1]`.
   - `event_pixel_pds_info/code/data → 0x0628/0x0618/0x0620` is still written; the knob
     `TQ_NO_EVENT_PDS` disables it ([E020](logRefs/E020-tq3d-pbe-two-banks.md)).
3. Launch with two writes: `ISP_RENDER = regs.isp_render`, then RMW `| 0x10`, then
   **`BCAST[0x0F00] = 1`**, the real launch and the last write.
4. Completion: `PIXELBE_END_RENDER`; finish trace `MAIN_4` "3D Transfer finished".

State ([E017](logRefs/E017-tq3d-launch-fault-at-zero.md), [E020](logRefs/E020-tq3d-pbe-two-banks.md)):
the pipe starts, then the PBE writes to address 0 (Q13). HWR discards the job.

## MMU faults and hardware recovery (`hwr.c`)

A recovery has two triggers:
- `EVENT_STATUS.MMU_PAGE_FAULT`, seen by the IRQ task or by a poll. `kick_events()`
  first emits `HWR_80` with the MMU and request status.
- The watchdog: `hwr_watchdog()`, run on every timer tick, finds a DM busy for longer
  than its context's `max_deadline_ms` (30 s from the mainline host; fallback 30 s).
  A fixed 2 s limit discarded legitimate long jobs ([E021](logRefs/E021-watchdog-deadline-wider-compute.md)).

`hwr_recover()` then runs the spec's reset sequence:
1. Hold scheduling: clear `HARDWARE_OK`, set `RESET_IN_PROGRESS`.
2. SLC flush and invalidate, wait for idle.
3. `PM_PARTIAL_RENDER_ENABLE = 0`.
4. Pulse `SOFT_RESET` through broadcast: `0x13FFFC1D`/`0xFFEFFE00`, then 0. This leaves
   the MIPS and the MTS wrapper alone.
5. `gpu_power_on()`, `gpu_units_init()`, `bif_rebuild()`.
6. Clear every event and discard the command of each busy DM. Its updates still run, so
   the host sees its fences.
7. `HARDWARE_OK | DM_RUNNING_OK`, then reschedule.

The traces are `HWR_32`, `HWR_28`, `HWR_61`, `MAIN_27`, `HWR_14`, `HWR_25` and `HWR_62`. A
recovery takes about 0.28 ms ([E019](logRefs/E019-hwr-and-compute-basic-67.md)). Not yet
done: the HWR info buffer for the host and the FWCCB context-reset notification.
