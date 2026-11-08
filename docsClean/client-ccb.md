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

The KCCB `FORCE_UPDATE` command forces checkpoints still `PVR_SYNC_CHECKPOINT_ACTIVE`
(`0xAC1`) in a blocked fence to errored (`0xEFF`), unblocking it.

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
