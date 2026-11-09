# Data master kicks

A **kick** programs the GPU registers to start a unit of work and starts the unit. Each
task type has its kick, triggered by dispatch ([client-ccb](client-ccb.md)). This page
describes at a high level what each kick does. Register addresses are in
[registers](registers.md).

> **Full kick detail:** the registers to program and the ordering dependencies are in
> [kick-geometry-ta](kick-geometry-ta.md) (geometry) and
> [kick-compute-and-3d](kick-compute-and-3d.md) (compute, fragment and transfer).

## Steps common to every kick

1. **Activate the context's memory context (BIF)** (below).
2. **Program the context's register set** in the GPU register bank (XPU broadcast on
   multicore).
3. **Load the command parameters** (the kick structure, mostly host-prepared register
   values, see [command-structs](command-structs.md)) into the unit's registers.
4. **Start** the unit and record the DM's execution state (context, offset, kick time,
   deadline).
5. Emit the kick's trace entry.

## Memory context (BIF) activation

Each `rogue_fwif_fwmemcontext` (page catalogue `pc_dev_paddr`, breakpoint) is bound to one
of the **slots 1–7** of `ROGUE_CR_BIF_CAT_BASE` on every kick. Slot 0 belongs to the
firmware itself.

Behaviour requirements:

- if the chosen slot **already holds the same page catalogue**, do **not** invalidate the
  MMU (this is what makes consecutive kicks of the same context cheap);
- when changing a slot's page catalogue, program the slot and invalidate the MMU as needed
  (`ROGUE_CR_BIF_CTRL_INVAL`, see [registers](registers.md));
- a slot in use by a running DM **must not** be reused by another context;
- the **policy** for choosing the slot to reuse is free (LRU, round-robin, refcount…);
- the slot occupancy state is also walked in the **power teardown** and in HWR. The chosen
  structure must allow walking it in finite time (see
  [power-management](power-management.md)).

## Geometry kick (TA / DM 2 and 6)

- Loads the geometry buffers from `rogue_fwif_cmd_geom` (0x70 B): VHeap, TPC, TE region
  headers, list stack pointers (ALIST/MLIST), Parameter Buffer base.
- Binds the HWRTData to the HW context and configures the local/global freelist.
- On multicore, geometry runs on the **primary core**. The second geometry pipe (DM 6) uses
  the second core of the active core list.
- Starts the geometry unit (TA).

## Fragment kick (3D / DM 3)

- Loads `rogue_fwif_cmd_frag` (0x1E0 B): ISP configuration, tiles in flight, dusts,
  partition mask, ZS/MSAA buffers.
- Binds the HWRTData already tiled by geometry.
- Starts the 3D (fragment) unit. It can be a **partial render** (`FRAG_PR`) started by the
  SPM (see [partial-render-spm](partial-render-spm.md)).

## Transfer kick (TQ)

- `rogue_fwif_cmd_transfer` (0xC0 B). Blit/resolve through the 3D pipeline (TQ_3D), the TDM
  (TQ_TDM) or 2D (TQ_2D), depending on the type.

## Compute kick (CDM / DM 4)

- `rogue_fwif_cmd_compute` (0x60 B): compute PDS/USC state, work-group configuration.
- Activates the memory context, programs the register set and starts the CDM.

## Completion

The GPU signals completion in `ROGUE_CR_EVENT_STATUS` (HW2, IRQ task). The firmware handles
the completion of the corresponding DM:

- updates the HWRTData and freelist state (geometry), flushes the TPC, etc.;
- **flushes the DM's caches** (front-end, USC/PDS, TPU, MCU fence, then SLC) **before**
  retiring, so the host never sees the job done before its output is written back (see
  "Completion: per-job cache flush" in [kick-compute-and-3d](kick-compute-and-3d.md));
- retires the command from the cCCB and processes the updates (see
  [client-ccb](client-ccb.md));
- re-evaluates scheduling for the DM, to see whether more work is ready.
