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

## Observed sequences (answers Q23, O)

Black-box observation of the open v1.1 image on the board running `vkfwtest bigpb`: one
1024×1024 render target, millions of triangles per frame, so geometry runs out of PB pages
again and again. The quoted lines are public trace strings.

**Freelists involved.** The trace names both freelists on every OOM: `U-FL` (the
**global** freelist) and `N-FL` (the HWRTData's **local** freelist). In this run the local
freelist had `max = total = 32` (grow size 0) and was never extended. **Every extension
and grow went to the global freelist.** When the geometry kick loads them, the trace
prints `UFL-TA-Base: <global base> (SP = …)` and `FL-TA-Base: <local base> (SP = …)`.

**1. OOM with ready pages: no host round trip.**
```
"Out of memory! Context …, HWRTData …"
"PM pause TA ALLOC: PM_PAGE_MANAGEOP set to 0x1"
"PM unpause TA ALLOC: PM_PAGE_MANAGEOP set to 0x0"
"Freelist update completed [global]: old total pages 0x1c0, new total pages 0x200,
 new DevVirtAddr 0x…48800"                      (was 0x…48900)
"OOM: Resumed TA with ready pages, FL addr: 0x…48800, current pages: 512, SP : 511"
```
- The firmware adds the freelist's `ready_pages` (64 here) to its total.
- The freelist's base address moves **down by 4 bytes per added page**: the page stack grows
  toward lower addresses.
- The geometry then **resumes**; no new `Kick TA` appears.
- Allocation is paused through `PM_PAGE_MANAGEOP` (`CR[0x02A0]`) while the PM state is
  changed.

**2. OOM without ready pages: grow.**
```
"Out of memory! …"
"OOM TA_cmd=…, U-FL <global>, N-FL <local>"
"OOM TA/3D PR Check: [<ufo>] is 0x0 requires 0x0"
"Wait for pending grow on Freelist <global>"
"SPM State = wait for grow"                     (FWCCB FREELIST_GROW to the host)
"Received grow update, FL addr: …, new pages: 768, ready pages: 96"   (KCCB FREELIST_GROW_UPDATE)
"PM pause TA ALLOC …" / "PM unpause TA ALLOC …"
"Freelist update completed [global]: old total 0x200, new total 0x2a0, new DevVirtAddr …"
"SPM State = PR avoided"
"Resume TA without partial render"
"SPM State = none"
```
After a grow update with `new pages = 768` and `ready pages = 96`, the new current total is
`768 − 96 = 672` (`0x2a0`). The ready pages are **held back** and handed out at the next OOM
(path 1: `0x2a0 → 0x300`).

**3. OOM that cannot grow: partial render.** This happens once the global freelist has
reached its maximum (65536 pages in this run).
```
"Out of memory! …"
"OOM TA_cmd=…, U-FL …, N-FL …"
"OOM TA/3D PR Check: [<ufo>] is 0x3 requires 0x3"     (geometry-fragment fence passes)
"No deferred partial render FW (Type=0) Buffer provided"
"No deferred partial render FW (Type=1) Buffer provided"
"SPM State = PR blocked"
"SPM State = wait for HW"                       (wait until the fragment DM is free)
fragment-side load: "Loading stack-pointers for 1 (0:MidTA,1:3D) on context 1, MLIST = …,
                     ALIST = …", "3D RTData … ready on HW context 1"
"UFL-3D-Base: <global> (SP = 9, …)", "ALIST1 SP = …, MLIST1 SP = … (VCE …, TE …, ALIST …)"
"Kick 3D: … Partial render:1 …"
"SPM State = PR running"
"3D finished, …"
"UFL-3D-Base: <global> (SP = 65527, …)"         (pages returned to the global freelist)
"Partial Render finished"
"SPM State = PR executed"
"Restarting TA after partial render, HWRTData0State=0x8, HWRTData1State=0x8"
"Restart TA after partial render"
"SPM State = none"
```
- The partial render is the context's `FRAG_PR` command run on the fragment DM, with PM
  context 1 loaded from the geometry's current ALIST/MLIST stack pointers and page
  catalogues.
- When it finishes, the pages come back (SP 9 → 65527), and the geometry is **restarted
  without a new `Kick TA`**.
- The `PR Check` line is the test of `partial_render_geom_frag_fence` (see
  [client-ccb](client-ccb.md)).
- "No deferred partial render FW Buffer provided" appears when the geometry command's
  `pr_buffer_fw_addr[]` entries are 0.

**The "4PB/4PT" counters.** The `UFL-…-Base`/`FL-…-Base` lines print `SP`, `4PB` and `4PT`
per freelist, for example `SP = 9, 4PB = 65402, 4PT = 124` near exhaustion. These are the
stack pointer and the pages handed out for parameter-buffer data and for page tables. They
correspond to the `Alloc Page Count` and `Alloc MMU Page Count` of the `Load`/`Store
Freelist` lines (local freelist registers in [kick-geometry-ta](kick-geometry-ta.md), Q16b).

**Not exercised in this run (`PENDING`):** on-demand ZS backing
(`ZSBUFFER_BACKING`/`_UPDATE`; no `pr_buffer` was provided), and freelist reconstruction
after HWR (no HWR occurred). The global freelist's registers (base and page counters, one
bank per PM context: `0x20B8` geometry, `0x2078` fragment) are given in
[kick-geometry-ta](kick-geometry-ta.md) (Q16e–Q16g), including the live stack pointers
(global `0x20C8`/`0x2088`, local `0x0348`/`0x0350`).

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
