# Partial render and out-of-memory handling (SPM)

Geometry consumes **Parameter Buffer (PB)** pages from a **freelist**
(`rogue_fwif_freelist`). When the freelist runs out in the middle of geometry, the PM
hardware signals an out-of-memory event. The firmware's out-of-memory / partial-render
handling (trace group `ROGUE_FWIF_LOG_TYPE_GROUP_SPM`, here simply "SPM") decides what to do
so that the frame is not lost.

> **Provenance.** Protocol and command names: **P** (`pvr_rogue_fwif.h`, `pvr_free_list.c`,
> `pvr_ccb.c`). States: **P** (public trace strings) + **O** (seen in the trace). Ordering
> rules: **D**.

## Decision on OOM

On an out-of-memory event during geometry (trace SPM id 7 `OOM TA_cmd=…, U-FL …, N-FL …`),
the firmware chooses:

1. **Use the ready pages** of the freelist (`rogue_fwif_freelist.ready_pages`, pages
   reserved for OOM), if any: the geometry resumes (trace SPM id 44 `OOM: Resumed TA with
   ready pages, …`).
2. **Grow the freelist** (preferred when possible):
   - send FWCCB `ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW` to the host, mark
     `rogue_fwif_freelist.grow_pending` and enter the *wait for grow* state (trace SPM id 34,
     id 41 `Wait for pending grow on Freelist …`);
   - the host allocates more pages and answers with KCCB
     `ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE` (trace SPM id 54 `Received grow update, …`);
   - the geometry resumes.
3. **Partial render** (when growing is not possible):
   - run the `ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR` command of the context's cCCB: a partial
     render of what has already been tiled, which frees PB pages;
   - **ZS/MSAA** buffers that are needed are requested on demand through FWCCB
     `ROGUE_FWIF_FWCCB_CMD_ZSBUFFER_BACKING` (the host confirms with KCCB
     `ROGUE_FWIF_KCCB_CMD_ZSBUFFER_BACKING_UPDATE`);
   - after the partial render, geometry **restarts** where it stopped.

## States and flags

The public trace strings name the states the SPM goes through (trace group SPM):

| Trace id | State |
|---|---|
| 32 | none |
| 33 | PR blocked |
| 34 | wait for grow |
| 35 | wait for HW |
| 36 | PR running |
| 38 | PR executed |

- The SPM state is kept per geometry context / freelist. The scheduler respects it: it does
  not schedule the final fragment while a partial render is pending (interlocks in the
  inter-DM overlap rules, see [scheduler](scheduler.md)).
- In the *wait for HW* state, with no 3D context running, the partial render is started
  and DM 3 is marked pending.
- The UFO checks that decide whether a PR may run are traced as SPM id 66 `OOM TA/3D PR
  Check: …`.
- The OS runtime flags (`rogue_fwif_os_runtime_flags`) include `fl_grow_pending` (bit 4):
  a freelist grow is in progress.

## Freelist reconstruction after HWR

After a hardware recovery that corrupted freelists, the firmware sends FWCCB
`ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION` and the host answers with KCCB
`ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE` (failures flagged with
`ROGUE_FWIF_FREELISTS_RECONSTRUCTION_FAILED_FLAG`). See
[hardware-recovery](hardware-recovery.md).

## Fidelity notes (observed behaviour)

- The FREELIST_GROW protocol is a **strict request ↔ confirmation pair**. The firmware
  keeps the context blocked until the confirmation arrives.
- Freelist cleanup requests must return the **correct return-slot bits**:
  `ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED` alone when the freelist was cleaned, and
  `CMD_EXECUTED | ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY` when it is still in use. Reporting
  "busy" wrongly makes the host retry and give up, leaving the freelist stuck.
