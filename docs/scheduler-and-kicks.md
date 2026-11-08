<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Scheduler, client CCBs and data-master kicks

## Data masters

| DM | id | Work | cCCB task types | State |
|---|---|---|---|---|
| GP | 0 | firmware internal | – | – |
| TDM | 1 | transfer | `TQ_TDM` (206), `TQ_2D` (208) | retired at once (`MAIN_172`) |
| GEOM | 2 | geometry / TA | `GEOM` (201) | retired at once |
| FRAG | 3 | fragment / 3D | `FRAG` (203), `FRAG_PR` (204), `TQ_3D` (202) | retired at once |
| CDM | 4 | compute | `CDM` (205) | **kicked on hardware**. It faults until the exec bases are known (Q7). |

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
`tpu_tag_cdm_ctrl`, `cdm_resume_pds1`) are **not written yet**: their offsets are
open question Q7.

### Why compute does not work yet

The PDS addresses in `CDM_CONTEXT_PDS0`/`TERMINATE_PDS` are **offsets inside the PDS
code/data heap**, not full virtual addresses. The hardware adds a global *PDS
execution base*. The upstream header `pvr_rogue_heap_config.h` states that the heap
bases "will be programmed, on a global basis, into `ROGUE_CR_PDS_EXEC_BASE` and
`ROGUE_CR_USC_CODE_BASE_*`", but no public source gives those registers' offsets.
With the base left at 0, the CDM fetches from VA `0x26C0`, which lies in the reserved
first 4 MiB and is unmapped, and takes an MMU fault
([E006](logRefs/E006-compute-kick-mmu-fault.md)). The experiments that look for the
registers are [E007](logRefs/E007-register-survey.md)–[E009](logRefs/E009-pds-exec-base-0x0610.md).
