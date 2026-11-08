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
