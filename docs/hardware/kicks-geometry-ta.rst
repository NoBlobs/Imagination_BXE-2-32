.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=============================
Geometry (TA) kick registers
=============================

Register-level hardware specification of the **Geometry / Tiling Accelerator
(TA / VDM, DM 2)** kick (:dc:`kick-geometry-ta`, experimentally verified in
:exp:`E027`–:exp:`E040` under ``-DEXP_TA``).

Register-bank addressing note: geometry registers sit at fixed offsets
(``0x0400``, ``0x0C00``, …) in the geometry core's bank
(``0xCF800000 + core * 0x80000``; on single-core BXE-2-32, always the
primary core bank). The trace's ``Setup register set = N`` refers to BIF
catalogue slot ``N + 1`` (``CAT_BASE{N+1}``), not an extra register offset.

Mandatory kick phases (A–G)
===========================

Each phase depends on the completion of the previous phase:

A. Preconditions
----------------

* GPU power domain on and GPU units initialised (:doc:`gpu-bringup`).
* If this is the first kick after GPU units init and ``rf_cmd_addr != 0``,
  run the firmware compute job first (no-op on mainline ``powervr``).
* Bind the memory context to a BIF slot (1–7) and select it in
  ``ROGUE_CR_BIF_CAT_BASE_INDEX`` TA field (bits 2:0).
* ``ROGUE_CR_MULTICORE_GEOMETRY_CTRL_COMMON`` (``0xF320``) remains ``0`` on
  single-core BXE-2-32 (``[spec]`` Q16i, :exp:`E034`).

B. Load render target into PM context 0
---------------------------------------

* **First kick of an HWRTData** (``GEOM_FLAGS_FIRSTKICK``):

  * Load local and global freelists into PM context 0 (bases, stack tops,
    counts + load commands ``0x0208`` and ``0x20B0``;
    :doc:`parameter-manager-and-spm`).
  * Program ``0x0240/0x0244 = 0x80_0000_0000``, ``PM_VHEAP_TABLE``
    (``0x0248``), ``PM_MLIST0_BASE`` (``0x02D8``), ``CONTEXT_PB_BASE``
    (``0x02B0 = 3`` for separate freelists, ``0`` for shared-PB subpasses;
    :exp:`E040`), and ``0x03D0 = 0x100`` / ``0x03D4 = 1``.
  * Load zero ALIST/MLIST stack pointers via ``0x0278``/``0x02C8`` and
    commands ``0x0288``/``0x0190`` (:exp:`E037`), and clear
    ``PM_CAT_BASE_*0`` so the PM allocates fresh catalogue pages from the
    local freelist.

* **Non-first kick on the same HWRTData** (``First kick:0``, e.g. layered
  renders): **skip** the PM load — PM context 0 keeps its live stack
  pointers, catalogues and freelist state (:exp:`E039`, ``[spec]`` Q16m).
* **PM handshakes** (after PM writes, never around them; :exp:`E032`):
  ``CR[0x0250] = 1`` (poll bit 0 = ``1``); ``CR[0x0258] = 1`` (poll bit 0 =
  ``0``).

C. Geometry (TE / PPP / VDM) registers
--------------------------------------

Compiled from ``rogue_fwif_cmd_geom`` (``0x70`` B),
``rogue_fwif_hwrtdata``, ``rogue_fwif_hwrtdata_common`` and
``rogue_fwif_geom_ctx_state_per_geom`` (:dc:`kick-geometry-ta`, :exp:`E027`,
:exp:`E028`, :exp:`E029`):

.. list-table::
   :header-rows: 1
   :widths: 22 44 34

   * - Offset (core bank)
     - Data source / value
     - Notes & scaling
   * - ``0x0408`` / ``0x040C``
     - ``geom_regs.vdm_ctrl_stream_base`` (64-bit)
     - VDM control stream base
   * - ``0x0410`` / ``0x0414``
     - Read-only VDM current stream pointer
     - Observed at ``base + 0x3C / +0x40`` (:exp:`E028`)
   * - ``0x0418`` / ``0x041C``
     - ``geom_ctx_state.geom_reg_vdm_call_stack_pointer_init`` (64-bit)
     - ``VDM_CALL_STACK_POINTER`` (:exp:`E027`)
   * - ``0x0420``
     - Hardware geometry kick counter (``+1`` per kick)
     - Read-only telemetry (:exp:`E029`)
   * - ``0x0428``
     - Candidate ``VDM_CONTEXT_STATE_BASE`` ``[open]``
     - 40-bit 16 B mask (:exp:`E008`)
   * - ``0x0C08`` / ``0x0C10``
     - ``hwrtdata_common.te_mtile1`` / ``te_mtile2``
     - Scales ×4 per side (``0x100000`` at 256×256, ``0x400000`` at
       1024×1024)
   * - ``0x0C18``
     - ``hwrtdata_common.te_screen``
     - Max X/Y in 16×16 tiles (``0x0000_F00F`` for 256×256, ``0x0003_F03F``
       for 1024×1024)
   * - ``0x0C20``
     - ``hwrtdata_common.mtile_stride``
     - Scales with area (``0x10`` for 256×256, ``0x100`` for 1024×1024)
   * - ``0x0C28``
     - ``geom_regs.te_psg``
     - ``TE_PSG`` (observed ``0x0002_0000``)
   * - ``0x0C30`` / ``0x0C34``
     - Constants ``2`` / ``3``
     - Unchanged across 256×256 and 1024×1024
   * - ``0x0C38`` / ``0x0C3C``
     - ``hwrtdata.rgn_header_dev_addr`` (64-bit)
     - ``TE_PSGREGION_ADDR`` (region headers base)
   * - ``0x0C40`` / ``0x0C44``
     - ``hwrtdata.tail_ptrs_dev_addr`` (64-bit)
     - ``TE_TPC_ADDR`` (tail pointer cache)
   * - ``0x0C48`` / ``0x0C70`` / ``0x0CA0``
     - Constants ``8`` / ``3`` / ``0x10``
     - Fixed geometry unit constants (:exp:`E029`)
   * - ``0x0C78`` / ``0x0C80``
     - ``hwrtdata_common.multi_sample_ctl`` / ``flipped_multi_sample_ctl``
     - Observed ``0x88`` (:exp:`E027`)
   * - ``0x0C88``
     - ``geom_regs.ppp_ctrl``
     - ``PPP_CTRL`` (observed ``0x202``)
   * - ``0x0C90`` / ``0x0C94``
     - ``0x3089_705F`` / ``0x3089_705F``
     - Constant 64-bit pattern (:exp:`E029`)
   * - ``0x0C98``
     - ``hwrtdata_common.screen_pixel_max``
     - ``PPP_SCREEN``: ``(width - 1) | ((height - 1) << 16)``
   * - ``0x0CB0``
     - ``geom_reg_ta_context_state_base_addr`` (64-bit)
     - TA context-state base; read at ``+0x200`` on context enable (found
       via canary probe in :exp:`E028`)
   * - ``0x0CE0``
     - ``1`` for a new kick
     - Context-switch control
   * - ``0x17A0`` / ``0x17A4``
     - ``geom_regs.tpu_border_colour_table`` (64-bit)
     - Shared register with fragment kick

D. Context-switch state and post-HWR zeroing ``[spec]``
-------------------------------------------------------

* New kick vs. resume programs ``0x0CE0``, ``0x0CB0``, ``0x0CB8 = 1``,
  ``0x0C48`` and ``0x0D20`` (state buffer address, ``0`` if none); on resume
  the TA state buffer flips between A and B.
* After HWR, if ``hwrtdata.geom_caches_need_zeroing`` is set, the first kick
  zeroes the TPC (``tail_ptrs_dev_addr``, ``tpc_size`` bytes) and the RTC
  (``rtc_dev_addr``, ``0x300`` bytes) with the CPU, through the GPU MMU
  (``geom_zero_caches()``, E053).
* ``CR[0x40A8] = (primary_core & 1)`` selects core/TE routing.

E. Context enable (mandatory poll)
----------------------------------

Write ``CR[0x0C68] = 1`` and poll ``CR[0x0C68]`` until bit 0 = ``1``
(context acceptance, must succeed before trigger). On hardware, ``0x0C68``
reads ``1`` for ≤ 20 µs while ``0x0D00`` drops from ``0x3F`` to ``0x1F``,
then returns to ``0`` about 60 µs before ``0x0420`` increments
(``[spec]`` Q16l, :exp:`E028`, :exp:`E038`).

F. Trace, timing and watchdog
-----------------------------

Emit trace ``MAIN_119`` (``Kick TA``), sample ``ROGUE_CR_TIMER``, arm the
DM 2 watchdog deadline and record DM 2 execution state.

G. Trigger (last write)
-----------------------

Write ``CR[0x0400] = 1`` (TA/VDM run trigger). Everything in phases A–F must
be complete before this store.

Geometry completion
===================

* ``MTS_DM2_INTERRUPT_ENABLE = 0xA0`` routes only ``TA_FINISHED`` (bit 5)
  and ``PM_OUT_OF_MEMORY`` (bit 7) to HW2; ``TA_TERMINATE`` (bit 6) is not
  routed.
* Geometry completion updates ``hwrtdata.state``, flushes the TA caches
  (``0x0688 = 1``, ``0x0668 = 5``, ``SLC_CTRL_FLUSH_INVAL = DM_TA``
  ``[spec]``), retires the cCCB command and applies UFO updates.
* The freelist state reached by geometry is stored back from PM context 0 at
  the subsequent **fragment kick** of the same HWRTData (not at
  ``TA_FINISHED``).
