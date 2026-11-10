# Clean-room specification — Rogue BXE-2-32 GPU firmware (SpacemiT K1)

This tree is the **functional clean-room specification** of the firmware that runs on the
internal microcontroller of the **PowerVR Rogue BXE-2-32** GPU (BVNC `36.29.52.182`) used in
the **SpacemiT Key Stone K1** SoC. Its purpose is to let a **clean team** reimplement the
firmware **from scratch**, reading **only** these documents.

> **Scope.** This describes the *open* firmware variant (upstream, compatible with the
> mainline `powervr` driver + Mesa `pvr`). A *closed* variant, shipped with the vendor's
> proprietary driver, also exists and is out of scope. See [3].

---

## How to use this documentation (clean-room rule)

Read **[1] Clean-room process** first. In short:

- The clean team implements **only** from these `.md` files. Nobody on the clean team may
  read the original binary, its disassembly, decompiler output, or the vendor's DDK/driver
  source.
- This spec describes **behaviour, interfaces, formats and algorithms** — the **facts**
  needed to interoperate with the RGX hardware and the host driver. It contains no code
  from the original firmware.
- Any gap found during implementation goes back as a question to the spec maintainer (the
  "dirty team"), who answers by **extending the spec** — never by handing over original
  code.

---

## Index

1. [Clean-room process](clean-room-process.md) — the firewall, what may and may not enter
   the spec, question flow, acceptance criteria.
2. [Firmware overview](overview.md) — what it is, what it is for, BVNC, coprocessor role,
   host ↔ firmware ↔ hardware relationship, spec conventions.
3. [Open vs. closed — sources and corroboration](open-vs-closed.md) — why the open
   variant is the target; clean-room source hierarchy (public header > observed behaviour
   > analysis); facts corroborated by both variants; traceability (P/O/D provenance).
4. [Execution environment (MIPS core)](execution-environment.md) — microMIPS ISA/ABI, boot
   sequence, vectors, interrupt model, timers, caches and TLB, write barrier.
5. [Memory model](memory-model.md) — MIPS address space, remap registers, host-managed
   page tables, firmware heap window, per-OS virtualisation, fault page, `.fw` container.
6. [Host ↔ firmware interface](host-interface.md) — shared-memory contract, boot
   handshake, structure size checks, visibility barrier, host↔FW interrupts.
7. [Shared data structures](data-structures.md) — layouts of `rogue_fwif_sysinit`,
   `osinit`, `sysdata`, `osdata`, `fwcommoncontext`, `fwmemcontext`, trace/HWR buffers,
   time correlation.
8. [Kernel CCB (host → firmware)](kernel-ccb.md) — host command ring, command list,
   processing, return slots.
9. [Firmware CCB (firmware → host)](firmware-ccb.md) — firmware notification ring,
   command list, slot acquisition.
10. [Client CCB and command dispatch](client-ccb.md) — per-context ring, command types,
    fences/UFOs, `dep_offset`, dispatch to data masters, retirement and updates.
11. [Kick command structures](command-structs.md) — sizes and role of the geometry,
    fragment, transfer and compute commands and of the command header; authoritative
    source (public header) and how the firmware consumes them.
12. [Scheduler and data masters](scheduler.md) — per-DM queues and state, run/wait lists,
    scheduling order, preemption (context store), inter-DM overlap rules.
13. [Data master kicks](data-masters.md) — overview of the geometry (TA), fragment (3D),
    transfer (TQ) and compute (CDM) kicks; memory context (BIF) activation.
14. [Geometry (TA) kick — registers and ordering](kick-geometry-ta.md) — step-by-step
    detail of the geometry kick: which registers, data sources, mandatory poll handshakes
    and the trigger register.
15. [Compute (CDM), fragment (3D) and transfer (TQ) kicks — registers and ordering](kick-compute-and-3d.md)
    — the compute, 3D and transfer kicks at the same level of detail: multicore control,
    CDM PDS, ISP/PBE configuration, and each unit's trigger register.
16. [Partial render and out-of-memory (SPM)](partial-render-spm.md) — PM OOM, freelist
    growth, partial render, on-demand ZS/MSAA buffers.
17. [Power management](power-management.md) — power states, APM, power-off and teardown,
    forced idle, power sync with the host, core clock and DVFS.
18. [Hardware recovery (HWR)](hardware-recovery.md) — fault/lockup detection, register
    polls, reset, command discard/recovery, freelist reconstruction, cleanup.
19. [Observed sequences (behaviour reference)](observed-sequences.md) — sequences
    validated on the board (and some in emulation): host notification, job cycle, context
    store/resume, OOM/grow, power, HWR, USC/GPIO events, 3D timeout; reference latencies.
    Acceptance reference for the reimplementation.
20. [RGX registers used by the firmware](registers.md) — map of relevant control registers
    (`ROGUE_CR_*`): events, MTS, BIF, SLC, MMU, timer, MIPS wrapper, XPU broadcast.
21. [Firmware trace and HWPerf](hwperf-trace.md) — circular trace buffer and its entry
    format, log groups, HWPerf status.
22. [Firmware trace IDs](trace-ids.md) — the public trace-ID table, key events per
    subsystem, log groups and `log_type`.
23. [GPU bring-up and boot register sequence](boot-gpu-init.md) — the GPU-side register
    programming at every power-on before the first kick: GPU-units init, PDS/USC execution
    bases, GPU-event → IRQ-task routing, MTS task-context association, the firmware-started
    handshake, and per-job dust/unit power.
24. [Glossary](glossary.md) — acronyms and terms (DM, cCCB, KCCB, UFO, SLC, BIF, MTS, HWR,
    SPM, PM, RTData, PB, USC, TA, CDM, …).
25. [Input-attachment render dump](input-attachment-dump.md) — observed register values
    during an input-attachment render pass (Q16m).

---

## Status of this spec

Living document. The behaviour described here was **validated on hardware** on the K1
(3D, compute and transfer running correctly, with pixel-checked dEQP tests).

- **Control plane** (boot, memory, shared-memory contract, CCBs, scheduler, power, HWR):
  covered well enough to implement.
- **Data plane** (kicks): all four kicks (geometry, compute, fragment, transfer) are
  described at register level, with the mandatory ordering dependencies, in
  [kick-geometry-ta](kick-geometry-ta.md) and [kick-compute-and-3d](kick-compute-and-3d.md).
  Command structures point to the **upstream public headers**
  ([command-structs](command-structs.md)); observed sequences are in
  [observed-sequences](observed-sequences.md).
- Items marked `PENDING` (matching each command field to its name in the public header;
  some register names) can be closed by reading the public ABI, or answered by the spec
  maintainer through the question flow.

Sections marked `PENDING` or `TO CONFIRM` are points to detail in a later iteration.
