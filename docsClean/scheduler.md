# Scheduler and data masters

The scheduler decides **which context runs on which data master (DM), and when**. It is
driven by interrupts (see [execution-environment](execution-environment.md)): the BG task
re-evaluates what is blocked, and the IRQ task reacts to completions and reschedules.

## Data masters (DM)

A **data master** is a GPU work unit with its own queue. DM indices are the public
`PVR_FWIF_DM_*` values (`fwcommoncontext.dm`):

| DM | `PVR_FWIF_DM_*` | Unit |
|---|---|---|
| 0 | `GP` | general purpose |
| 1 | `TDM` | transfer (TDM) |
| 2 | `GEOM` | geometry (TA) |
| 3 | `FRAG` | fragment (3D), also transfer through 3D |
| 4 | `CDM` | compute |
| 6 | `GEOM2` | second geometry (multicore only; not used on this single-core SoC) |

## State the scheduler must keep

The internal organisation is free. The behaviour requires, per DM:

- a **run list**: **ready** contexts (dependencies resolved), ordered by `priority`;
- a **wait list**: contexts **blocked** on a fence, re-evaluated on every BG task;
- the list nodes are the ABI fields `run_node` and `waiting_node` of
  `rogue_fwif_fwcommoncontext` (see [data-structures](data-structures.md)). Lists of
  different DMs are **independent**: each DM has its own;
- an **execution state**: running context, offset of the running command, kick time,
  deadline (watchdog) and lockup flags. It is used for preemption and HWR.

And globally:

- a **"DM has ready work"** mark per DM, set when a context enters the run list, plus a
  **deferred work** mark (with a deferral count);
- a **DM visiting order**, a permutation that balances geometry, fragment and compute. It
  can be reordered by deferral count and returns to the default when nothing was deferred.

## Scheduling cycle

Triggered by completion (IRQ task) or re-evaluation (BG task):

```
for each DM, in visiting order, if pending or deferred:
    if the DM already runs a context and nothing of higher priority waits: skip
    if the DM is held (lockup / freelist / power): log and skip
    take the context at the top of the run list
    resolve the remaining dependencies (fences); if ready:
        if the inter-DM overlap rules block it: defer
        if a higher-priority context waits behind the running one:
            request a context store of the running one (preemption) and defer
        else: record the DM's execution state and dispatch
              (see client-ccb → "Dispatching a task")
```

## Preemption (context store)

When a **higher-priority** context waits behind a lower-priority one running on a DM, the
firmware asks the running context to **save its state** and yields the DM:

- fragment → 3D store; compute → CDM store;
- **geometry is not preempted** (observed: the higher-priority request waits for the TA to
  finish);
- an **HCS** (Hard Context Switch) watchdog is armed. If the store does not complete in the
  configured time, the firmware forces the switch.

Preemption only happens on DMs where the OS config flags enable context switching
(`ROGUE_FWIF_INICFG_OS_CTXSWITCH_{FRAG,CDM,GEOM,TDM}_EN`). With the flags at 0 there is no
context switch. Observed sequences in [observed-sequences](observed-sequences.md).

### Observed store and resume per DM (answers Q24, O)

Black-box observation of the open v1.1 image on the board.
- **Enabling CSW:** the mainline driver leaves the OS config flags at 0, so the host was
  started with the flags `GEOM_EN | FRAG_EN | CDM_EN` set in
  `rogue_fwif_osdata.fw_os_config_flags`.
- **Workload:** two contexts of different priority (`vkfwtest pre-3d`, `pre-ta`,
  `pre-cdm`).
- **Data captured:** the public trace, plus reads of the contexts' `fw_com_ctx_flags`.

**When the firmware decides to store.** When a context of higher priority becomes ready on a
DM that is running a lower-priority context. Trace CSW id 38: `Higher priority context
scheduled for DM <n>. Prios (OSid, OSid Prio, Context Prio): Current: 0, 0, 0 New: 0, 0, 1.
Hard Context Switching: 0`. In all three runs the decision came from **priority**. No store
was triggered by a deadline or by the host.

**Fragment (DM 3): stored and resumed (10 times in `pre-3d`).**
```
"Higher priority context scheduled for DM 3 …"
"*** 3D context store start"
"*** 3D context store complete"        ~7 k timer ticks later, together with the 3D
                                        completion event ("3D finished, HWRTData0State=3")
"3D FWCtx <ctx> needs resume"           fw_com_ctx_flags of the stored context = 0x1
                                        (ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME)
… the higher-priority 3D runs …
"*** 3D FWCtx <ctx> resume"
"Kick 3D: FWCtx <ctx> @ <same cCCB offset>, … CSW resume:1"
```
- **Same command:** the stored command is **not** retired. The **same command** (same cCCB
  offset) is kicked again with `CSW resume:1`, after its PM context 1 state is reloaded
  ("Loading stack-pointers … on context 1", "3D RTData … ready on HW context 1").
- **Store completion:** the store finishes through the normal fragment completion event; no
  separate completion event was seen.
- **Context-state buffer:** the context's `context_state_addr` stayed the same across all
  the stores.

**Geometry (DM 2): the store completes only when the TA finishes (`pre-ta`).**
```
"Higher priority context scheduled for DM 2 …"
"*** TA context store start"
"TA finished"                          ~170 ms later
"*** TA context store complete"
… higher-priority geometry runs; the stored context's fragment then runs normally
  (CSW resume:0) …
```
No geometry resume (`CSW resume:1` on a TA kick) was observed. The geometry is not
interrupted mid-way: the store request is satisfied at the end of the TA.

**Compute (DM 4): the store faults on this stack (`pre-cdm`).**
```
"Higher priority context scheduled for DM 4 …"
"*** CDM FWCtx store start"
"MMU fault event: 0x00000200"
"BIF0 page fault detected (Core 0 MMU Status: 0x…1041 Req Status: 0x…1000000000d0)"
"GPU-0 has locked up" → HWR
"Analysis DM4: Lockup FWCtx: <low-priority ctx>. Need to skip to next command"
"Recovery DM4: FWCtx … skipped to command @ …"   (the stored compute command is discarded)
"Kick Compute: <high-priority ctx> …"             (runs and finishes)
```
- With the original firmware, mainline host and Mesa, a CDM context store ends in a page
  fault at a low address (`0xd0`) and an HWR. The preempted command is discarded, not
  resumed.
- Whether this comes from host-side setup (for example the compute context's
  context-switch PDS programs or state buffer) or is inherent is not established. For
  mainline interoperability, **CDM context switching is effectively unusable**; the
  mainline default (flags 0) avoids it.

`PENDING`: the register-level store and resume sequences per DM, and the layout of the data
written into the context-state buffers (buffer A/B selection with
`ROGUE_FWIF_CTX_USING_BUFFER_A/B`). The observation shows the protocol, not the register
writes.

## Inter-DM overlap rules

Before starting a context on a DM, the firmware checks whether it may run **now**:

- blocked while **HWR is active**;
- blocked if **DM overlap is disabled** (`ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP`) and
  another DM is running;
- blocked according to the **TA/3D/CDM interlocks** of the running contexts and per-DM
  "serialise" flags, to respect pipeline and partial-render dependencies;
- for compute, priorities are compared with the running compute context.

## Partial render / SPM

Interactions with the partial-render subsystem (geometry out of PM memory) are in
[partial-render-spm](partial-render-spm.md). The scheduler respects the SPM state: for
example, it does not schedule the final fragment while a partial render is pending.

## Full path of a command (summary)

```
host writes the cCCB ─► KCCB KICK ─► context check: process fences
     ├─ blocked → DM wait list (re-evaluated every BG)
     └─ ready   → run list (priority) + DM marked pending
scheduling cycle (in DM visiting order)
     └─ dispatch by command type:
          ├─ GEOM  → geometry kick     ├─ FRAG → fragment kick
          ├─ TQ_3D → transfer kick     └─ CDM  → compute kick
GPU finishes → EVENT_STATUS → IRQ task
     └─ DM completion handling
          └─ retire the command → process updates → IRQ to the host
```
