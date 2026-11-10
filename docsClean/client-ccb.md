# Client CCB and command dispatch

The **Client CCB** (cCCB) is the **per-context** work ring: the host queues the context's
render/compute commands here (geometry, fragment, transfer, compute), plus auxiliary
commands (fences, updates, timestamps). The firmware consumes this ring, resolves
dependencies and dispatches the work to the data masters.

## Control block `rogue_fwif_cccb_ctl`

```
{ write_offset, read_offset, dep_offset, wrap_mask, read_offset2, read_offset3, read_offset4 }
```

- **`write_offset`** — how far the host has written (advanced by KICK).
- **`read_offset`** — next command to consume.
- **`dep_offset`** — boundary between commands whose **dependencies are resolved** (ready
  for dispatch) and those still blocked by a fence. Fence processing advances
  `dep_offset`.
- **`wrap_mask`** — ring wrap mask.
- **`read_offset2..4`** — additional read cursors used on the synchronisation path.

Exact layout: public header (`pvr_rogue_fwif.h`).

## Command header and command types

Every command starts with a **header** `rogue_fwif_ccb_cmd_header` (**0x28 B**):
`cmd_type`, `cmd_size` (payload size), `ext_job_ref`, `int_job_ref`, `work_est_kick_data`.
The payload follows the header. **`cmd_type` is the first word of the header** and has the
form `id | (ROGUE_CMD_MAGIC_DWORD << 16)`; **bit 15** (`ROGUE_CCB_TYPE_TASK`, `0x8000`)
marks a **task**, which the firmware dispatches to a data master. The others are
auxiliary commands consumed during fence/update processing.

| id | `ROGUE_FWIF_CCB_CMD_TYPE_*` | Class | Handling |
|---|---|---|---|
| 201 | `GEOM` | task | geometry kick (TA) |
| 202 | `TQ_3D` | task | transfer kick (via 3D) |
| 203 | `FRAG` | task | fragment kick (3D) |
| 204 | `FRAG_PR` | task | started by SPM, not by dispatch |
| 205 | `CDM` | task | compute kick |
| 206 | `TQ_TDM` | task | transfer (TDM) |
| 207 | `FBSC_INVALIDATE` | task | FBSC invalidate |
| 208 | `TQ_2D` | task | 2D transfer |
| 209 | `PRE_TIMESTAMP` | task | write the GPU time to the address in the command |
| 210 | `NULL` | task | null kick; restarts periodic sampling |
| 211 | `ABORT` | task | log only |
| 212 | `FENCE` | auxiliary | wait on UFO(s) |
| 213 | `UPDATE` | auxiliary | signal UFO(s) |
| 214 | `RMW_UPDATE` | auxiliary | increment a UFO |
| 215 | `FENCE_PR` | auxiliary | partial-render fence |
| 216 | `PRIORITY` | auxiliary | change priority |
| 217 | `POST_TIMESTAMP` | auxiliary | write the host time |
| 218/219 | `UNFENCED_(RMW_)UPDATE` | auxiliary | update without fence |
| 221 | `PADDING` | auxiliary | wrap padding |

So a geometry task has `cmd_type = 0x2ABC80C9` and a compute task `0x2ABC80CD`.

A type outside this table is an invalid command. The firmware logs it (public trace: group
MAIN ids 171/172, `Unknown Client CCB Command …`) and treats it as a context failure.

## Fences and UFOs

A **UFO** (*Updated Fence Object*, `rogue_fwif_ufo`) is a pair `{ addr, value }`:

- if **bit 0 of the address** is set (`ROGUE_FWIF_UFO_ADDR_IS_SYNC_CHECKPOINT`), it is a
  **sync checkpoint**: satisfied when memory holds `PVR_SYNC_CHECKPOINT_SIGNALED` (`0x519`)
  or `PVR_SYNC_CHECKPOINT_ERRORED` (`0xEFF`);
- otherwise it is a **counter**: compared for **equality** (`FENCE`) or with **signed
  `>=`** (`FENCE_PR`).

At most `ROGUE_FWIF_CCB_CMD_MAX_UFOS` (**64**) UFOs per command. The UFO that failed is
stored in `fwcommoncontext.last_failed_ufo` so it can be re-evaluated cheaply on the next
BG task.

### Paired geometry + fragment jobs (answers Q22)

How the mainline host expresses "the fragment must wait for its geometry" (**P**,
`pvr_queue.c`, `pvr_cccb.c`):

- The fragment job's cCCB gets a **`FENCE_PR`** command whose UFO is the geometry queue's
  timeline UFO, with the geometry job's completion value. So the wait is a **cCCB fence**
  (signed `>=`, see above), not a dependency the firmware has to discover by itself.
- The geometry command's `partial_render_geom_frag_fence` is set to the same timeline UFO
  with the value **one below** the geometry job's completion value. The firmware uses it
  for the partial-render check (trace SPM id 66 `OOM TA/3D PR Check: … requires …`, see
  [partial-render-spm](partial-render-spm.md)).
- Both cCCBs are then started together with one KCCB **`COMBINED_GEOM_FRAG_KICK`** (117),
  which carries a geometry kick and a fragment kick (`fill_cmd_kick_data` for each). For a
  partial-render fragment the host attaches no cleanup resources to the fragment half.

`TO CONFIRM` (not established by the analysis): whether the firmware, in addition to this
fence, holds the fragment kick on the HWRTData `state` while the geometry of the same
HWRTData is running.

The KCCB `FORCE_UPDATE` command forces checkpoints still `PVR_SYNC_CHECKPOINT_ACTIVE`
(`0xAC1`) in a blocked fence to errored (`0xEFF`), unblocking it.

### `FRAG_PR` when no partial render is needed (answers Q16m item 2, O)

Public trace of a single draw on the board. The fragment context's cCCB holds:
1. at offset 0, the `FENCE_PR` check on the geometry-fragment fence;
2. the `FRAG_PR` command;
3. the regular fence/`FRAG` commands after them.

When the geometry finished **without** running out of memory:
- `UFO PR-Check: [<geom-frag fence>] is 0x1 requires >= 0x1` passes once the geometry's
  update has written the fence.
- The firmware takes the next command (`Client command header DM = 3, … cmd = <ccb>+0x30`,
  the `FRAG_PR`). **No `Kick 3D` follows for it.**
- The firmware applies that command's **update UFOs** (`UFO Updates for FWCtx … @ 568`,
  `UFO Update: [<frag job fence>] = 0x1`).
- It moves on to the following commands: `UFO Checks … @ 616`, then `Kick 3D … @ 664,
  Partial render:0`. After the real render, the same fence is updated to `0x2`.

So a `FRAG_PR` that is not needed is **consumed without running**:
- the read offset passes it;
- its update UFOs are signalled;
- no HWRTData state or PM register changes go with it (none were seen in the register
  sampling).

It runs (`Kick 3D … Partial render:1`) only from the out-of-memory path of
[partial-render-spm](partial-render-spm.md).

## cCCB processing flow

When a context is considered for running (on KICK or when re-evaluating the wait list):

1. **Process fences**: advance `dep_offset` over consecutive `FENCE`/`FENCE_PR`/`PRIORITY`
   commands.
   - For each FENCE, evaluate the UFOs. If any is not satisfied, **stop**: the context is
     blocked (wait list) and the failed UFO is stored.
   - PRIORITY adjusts the context's `priority`.
2. If a **task** (bit 15) is reached, the context is **ready**: it enters the DM's **run
   list** (ordered by `priority`) and the DM is marked as pending for scheduling.
3. The scheduler ([scheduler](scheduler.md)) picks the context and dispatches it.

### Dispatching a task

- The **command to dispatch** is the one at **`read_offset`** of the context's cCCB control
  block: cCCB base + `read_offset`.
- The cCCB base and control-block addresses come from the `rogue_fwif_fwcommoncontext`
  (`ccb_fw_addr`, `ccbctl_fw_addr`) with the window mask applied (see
  [memory-model](memory-model.md)).
- The **type** is `cmd_type` in the command header. The firmware dispatches by **id**
  (table above): GEOM, TQ_3D, FRAG and CDM go to the corresponding data master kick (see
  [data-masters](data-masters.md)). PRE_TIMESTAMP, NULL and ABORT are handled immediately.

Commands that **complete immediately** (timestamp, null, abort) are **retired at once**. A
hardware kick **stays** at `read_offset` until the data master finishes, and only then is
it retired.

## Retirement and updates

When the work finishes (completion interrupt, see [scheduler](scheduler.md) and
[data-masters](data-masters.md)):

1. **Retire the command**: advance `read_offset` past the completed task.
2. **Process updates**: consume the following `UPDATE`/`RMW_UPDATE`/`POST_TIMESTAMP`.
   - `UPDATE` writes the values into the UFOs or signals checkpoints (`SIGNALED`).
   - `RMW_UPDATE` increments a UFO.
   - `POST_TIMESTAMP` writes the host time.
3. **Raise an IRQ to the host** (after the write barrier), to release the frame and wake
   dependent work.

Consuming the auxiliary commands advances `read_offset`. The context's next dispatch starts
from the resulting `read_offset`, which points to the next task.
