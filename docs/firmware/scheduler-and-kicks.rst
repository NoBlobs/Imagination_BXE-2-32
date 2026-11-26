.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

===================
Scheduler and kicks
===================

This page describes client CCB scheduling in ``src/sched.c``, BIF catalogue
management and compute/transfer kicks in ``src/kick.c``, and the experimental
geometry/fragment render kicks in ``src/geom.c`` and ``src/frag.c``.

Data master support summary
===========================

.. list-table::
   :header-rows: 1
   :widths: 12 8 28 52

   * - DM
     - ID
     - cCCB task types
     - Implementation state
   * - ``GP``
     - 0
     - —
     - Firmware internal (BG context)
   * - ``TDM``
     - 1
     - ``TQ_TDM`` (206), ``TQ_2D`` (208)
     - Retired immediately (``MAIN_172``); not emitted by Mesa on BXE-2-32
   * - ``GEOM``
     - 2
     - ``GEOM`` (201)
     - Experimental with ``-DEXP_TA`` (``src/geom.c``); retired immediately
       in the default build
   * - ``FRAG``
     - 3
     - ``TQ_3D`` (202), ``FRAG`` (203), ``FRAG_PR`` (204)
     - ``TQ_3D`` **works** in default build (:exp:`E023`). ``FRAG``
       experimental with ``-DEXP_TA`` (``src/frag.c``, :exp:`E038`);
       ``FRAG_PR`` retired immediately
   * - ``CDM``
     - 4
     - ``CDM`` (205)
     - **Works** (:exp:`E013`, :exp:`E021`, :exp:`E023`)

Unimplemented tasks are retired immediately so their trailing updates still
run and host fences signal without hanging the driver.

Context table and client CCB walk (``src/sched.c``)
===================================================

``sched_kick()`` (called from ``KCCB_KICK``) finds or allocates an entry in
``ctxs[64]`` keyed by ``context_fw_addr`` (``rogue_fwif_fwcommoncontext``),
increments ``submitted_commands`` on up to 3 cleanup control blocks (queuing
them in an 8-entry per-context FIFO), updates ``write_offset`` and
``wrap_mask`` in the context's cCCB control block, marks the context pending,
and invokes ``sched_run()``.

Each cCCB command begins with a ``0x28``-byte ``rogue_fwif_ccb_cmd_header``
(magic ``0x2ABC`` in high half, bit ``0x8000`` marking a task, ``cmd_size``
bytes of payload; next offset ``(roff + 0x28 + cmd_size) & wrap_mask``):

.. code-block:: text

   ctx_process() loop (at most 64 commands per pass):
       roff == woff                 -> context idle (pending = false, dep_offset = roff)
       a task of this context runs  -> stop
       bad magic                    -> trace MAIN_171, mark context dead
       TASK:
           DM busy or forced idle   -> stop (retry later)
           kick_task() == running   -> stop (command stays at read_offset)
           kick_task() == done      -> retire now (pop cleanup FIFO), continue
       FENCE / FENCE_PR             -> all UFOs satisfied? else stop & record last_failed_ufo
       UPDATE / RMW / UNFENCED_*    -> write (or add) values, notify host
       PRIORITY                     -> update ctx.priority
       PADDING / POST_TIMESTAMP     -> skip

UFO semantics:

* ``FENCE``: ``*addr == value``;
* ``FENCE_PR``: signed ``*addr - value >= 0`` (used by Mesa for timeline
  dependencies);
* ``UPDATE``: ``*addr = value``;
* ``RMW_UPDATE``: ``*addr += value``;
* Sync checkpoint (bit 0 of UFO address set): fence passes on ``SIGNALED``
  or ``ERRORED``; update writes ``SIGNALED``.

``sched_run()`` walks pending contexts round-robin for up to 8 passes while
progress is made, updates ``pow_state``, and notifies the host once if any
visible state changed.

Completion and cleanup
----------------------

Active kicks store ``{ctx, cmd_off, event_mask, finish_sfid, kick_time,
bif_slot}`` in ``dms[dm]`` and set bit ``dm`` in scratch ``0x1A78``. When an
``EVENT_STATUS`` bit in ``event_mask`` fires (via HW2 IRQ task or timer
poll), ``kick_events()`` clears the event, runs ``dm_cache_flush(dm)``
(:doc:`../hardware/kicks-compute-3d-transfer`), clears the busy bit, traces the
finish SFID and calls ``sched_dm_done()`` to advance ``read_offset``, bump
``executed_commands`` and re-schedule trailing updates in the same pass.

``KCCB_CLEANUP`` returns ``CLEANUP_BUSY`` while a ``FWCOMMONCONTEXT`` has a
running task or non-empty cCCB (unless dead), or while a running DM still
references an ``HWRTDATA``, ``FREELIST`` or ``ZSBUFFER``; otherwise it frees
the entry and returns ``0``.

BIF catalogue slot management (``src/kick.c``)
==============================================

Before kicking a DM, the context's ``fwmemcontext.pc_dev_paddr`` is bound to
one of ``BIF_CAT_BASE1..7`` (slot 0 is never used for clients):

* An already-bound context reuses its slot.
* Otherwise a free slot (not used by a busy DM) is chosen round-robin, the
  SLC is flushed (:exp:`E015`), ``BIF_CAT_BASEn = pc & 0xFF_FFFF_F000`` is
  written, ``BIF_CTRL_INVAL = 0xF`` invalidates PT/PD/PC/TLB, and the slot
  index is stored back into ``fwmemcontext.page_cat_base_reg_set``.
* The DM's field in ``BIF_CAT_BASE_INDEX`` (``0x1240``) is pointed to the
  slot. ``bif_rebuild()`` restores all active bindings after HWR.

Compute kick (``kick_cdm``)
===========================

Follows :dc:`kick-compute-and-3d`:

1. Read ``MULTICORE_SYSTEM`` (``0xF308``); write
   ``BCAST[0xF330] = (execute_count << 8) | 1``.
2. Bind BIF slot and set ``CAT_BASE_INDEX`` CDM field (bits 18:16).
3. Write 64-bit ``BCAST[0x0480]`` (``cdm_ctrl_stream_base``),
   ``BCAST[0x17A8]`` (``tpu_border_colour_table``) and ``BCAST[0x0498]``
   (``cdm_context_state_base_addr``).
4. Emit trace ``MAIN_122``.
5. Write ``CDM_CONTEXT_PDS0/1`` and ``CDM_TERMINATE_PDS/1``
   (``0x04A8..0x04C0``) from ``fwcomputecontext.ctxswitch_regs``.
6. Clear ``COMPUTE_FINISHED``, mark DM 4 busy, and trigger
   ``BCAST[0x0478] = 1`` (always the last write).

Unused ``rogue_fwif_compute_regs`` fields (``tpu``, ``cdm_item``,
``compute_cluster``, ``tpu_tag_cdm_ctrl``) are not written because their
gating features/BRNs are absent on BXE-2-32.

Transfer kick (``kick_tq3d``)
=============================

Follows the transfer table in :dc:`kick-compute-and-3d` (verified in
:exp:`E014`, :exp:`E017`, :exp:`E020`, :exp:`E022`, :exp:`E023`):

1. Write ``MULTICORE_FRAGMENT_CTRL_COMMON (0xF310) = 1`` and bind the BIF
   slot for DM 3 (``CAT_BASE_INDEX`` bits 10:8) **before** any ISP/PBE write.
2. Program transfer, ISP and PBE registers:

   * Only when ``FAST_2D`` (``(isp_render & 3) == 2``): write ``0x0F20``
     (``isp_mtile_base``), ``0x0F28`` (``isp_rgn``) and ``0x0FC8/0x0FCC =
     0/0xE4`` (writing them unconditionally caused PBE writes to address 0;
     :exp:`E022`).
   * Write ``0x0FA8/0x0FAC = 0x88888888`` only if ``isp_aa & 3``; write
     ``0x0388 = 0``; leave ``0x06B0`` (``PDS_BGRND2``) untouched.
   * Set tiles-in-flight ``0x0FD8 = min(ISP_CTL[15:12] + 1, 2)``.
   * Program PBE in two planar banks: ``WORD0_MRTn = 0x1510 + 8n`` and
     ``WORD1_MRTn = 0x1550 + 8n``.
   * Program end-of-tile PDS ``event_pixel_pds_info/code/data`` into
     ``0x0628`` / ``0x0618`` / ``0x0620``.

3. Launch with ``ISP_RENDER = isp_render`` immediately followed by
   ``BCAST[0x0F00] = 1`` (writing ``ISP_RENDER`` earlier lost the first
   tile of multi-tile transfers; :exp:`E023`), then poke ``MTS_SCHEDULE = 0``.
4. Completion: ``PIXELBE_END_RENDER``, per-DM cache flush and trace
   ``MAIN_4``.

Geometry and fragment kicks
===========================

On by default since :exp:`E041` (``-DEXP_NO_RENDER`` retires them unrun, the
old behaviour); developed in :exp:`E027`–:exp:`E041`:

* **Geometry** (``src/geom.c``): loads PM context 0 (local/global free-list
  staging registers + load commands ``0x0208``/``0x20B0``, ALIST/MLIST SP
  load commands ``0x0288``/``0x0190``, skipping the PM load on non-first
  kicks per :exp:`E039`), binds BIF slot for TA after PM writes
  (:exp:`E034`), writes TE/PPP/VDM registers (including ``0x0CB0`` TA
  context-state base found via canary in :exp:`E028`), enables geometry
  context ``0x0C68``, triggers ``0x0400 = 1``, and writes back PM state on
  completion.
* **Fragment** (``src/frag.c``): loads PM context 1 (local/global free-list
  load commands ``0x0200``/``0x20A8``, ALIST/MLIST SP load commands
  ``0x0288``/``0x0190``; :exp:`E036`, :exp:`E037`), programs ZLS depth/stencil
  registers (``0x0F48..0x0F68``), ``0x0F28 = 4 * mtile_stride``, multisample
  words ``0x0FA0``/``0x0FA8``, ISP and PBE banks, launches via
  ``ISP_RENDER`` + ``0x0F00 = 1``, waits for ``PIXELBE_END_RENDER`` and
  ``PM_3D_MEM_FREE`` (:exp:`E038`), and writes back updated free-list counts.
* **PM contexts** (``src/pm.c``, :exp:`E041`): ``pm_slots[]`` records which
  HWRTData and free lists each PM context holds. Before another HWRTData is
  loaded into a context, its live free-list state is stored back
  (``pm_slot_evict``), but only while the context is *dirty*: the fragment
  load (context 0) and the end of the fragment (context 1) already store it.
  Without the store, two render contexts that interleave hung the SoC;
  without the dirty flag, a stale store overwrote the freed pages. Every load
  and store emits the reference's ``Load Freelist`` / ``Store Freelist``
  trace lines.
* **Out-of-memory** (``src/pm.c``, :exp:`E041`): ``PM_OUT_OF_MEMORY`` while
  the geometry runs adds ready pages inside a ``PM_PAGE_MANAGEOP`` pause,
  requests a grow (FWCCB ``FREELIST_GROW``), or arms a partial render
  (``FRAG_PR``). The TA does not resume yet (Q28).
* **Known gap**: the second render on a device stalls the TA (Q29).
