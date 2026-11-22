.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=================================================
Compute, fragment (3D) and transfer kicks
=================================================

Register-level hardware specification for the **Compute (CDM, DM 4)**,
**Fragment (3D, DM 3)** and **Transfer** (``TQ_3D``, DM 3) kicks, per-DM
completion cache flushes, and context-switch (preemption) behaviour
(:dc:`kick-compute-and-3d`, :dc:`scheduler`, :dc:`input-attachment-dump`).

Common kick skeleton
====================

.. code-block:: text

   1. Ensure GPU power domain and GPU units are initialised
   2. [First kick after units init] Run rf_cmd_addr compute job if non-zero (no-op on mainline)
   3. Program multicore control (BCAST ..._CTRL_COMMON)
   4. Bind memory context to BIF slot 1..7 and BIF_CAT_BASE_INDEX (BEFORE any unit writes)
   5. [TA / 3D] Load HWRTData into PM context 0 / 1 (+ load commands / handshakes)
   6. Program unit register block (mostly broadcast, from host-prepared command regs)
   7. [Preemption only] Reload saved state from snapshot / context-state buffer
   8. Trace entry + arm watchdog + record DM state
   9. TRIGGER (last write):
        TA:   CR[0x0400] = 1
        CDM:  BCAST[0x0478] = 1
        3D:   BCAST[0x0F00] = 1   (ISP_RENDER left at 0; |= 0x10 on resume only)
        TQ:   BCAST[0x0F08] = isp_render ; then BCAST[0x0F00] = 1
  10. ON COMPLETION:
        clear EVENT_STATUS bit -> per-DM cache flush -> retire cCCB command -> UFO updates -> host IRQ

Compute kick (CDM / DM 4)
=========================

Verified in :exp:`E013`, :exp:`E019`, :exp:`E021`, :exp:`E023`:

.. list-table::
   :header-rows: 1
   :widths: 12 34 54

   * - Phase
     - Register(s)
     - Data / action
   * - A
     - ``0xF308``, ``BCAST[0xF330]``
     - Read ``MULTICORE_SYSTEM``; write ``MULTICORE_COMPUTE_CTRL_COMMON =
       (execute_count << 8) | 1``; bind DM 4 BIF slot (``CAT_BASE_INDEX``
       bits 18:16)
   * - B
     - ``BCAST[0x0480/84]``, ``[0x17A8/AC]``, ``[0x0498/9C]``
     - 64-bit ``cdm_ctrl_stream_base``, ``tpu_border_colour_table``,
       ``cdm_context_state_base_addr`` (from ``rogue_fwif_cmd_compute``)
   * - C
     - Snapshot buffer ``[spec]``
     - Context-switch resume only (unused when CSW flags are 0)
   * - D
     - Trace ``MAIN_122``, ``ROGUE_CR_TIMER``
     - Arm DM 4 watchdog; poke ``MTS_SCHEDULE = 0``
   * - E
     - ``BCAST[0x04A8/AC]``, ``[0x04B0]``, ``[0x04B8/BC]``, ``[0x04C0]``
     - ``CDM_CONTEXT_PDS0/1`` and ``CDM_TERMINATE_PDS/1`` from
       ``fwcomputecontext.ctxswitch_regs``
   * - F
     - ``BCAST[0x0478] = 1``
     - **CDM run trigger** (last write); completes on
       ``EVENT_STATUS.COMPUTE_FINISHED`` (bit 2)

* **Unused fields on BXE-2-32**: ``tpu`` (BRN 49927), ``cdm_item``
  (``COMPUTE_MORTON_CAPABLE``), ``compute_cluster`` (``CLUSTER_GROUPING``),
  ``tpu_tag_cdm_ctrl`` (``TPU_DM_GLOBAL_REGISTERS``) and ``cdm_cb*``
  (``CDM_USER_MODE_QUEUE``) are all gated by absent features and ignored.
* **Single-kick / zero-workgroup rule** (Q10): the firmware issues **one**
  kick per ``rogue_fwif_cmd_compute`` and never inspects workgroup
  dimensions; the CDM walks the full control stream itself.

Fragment kick (3D / DM 3)
=========================

Verified for colour and depth/stencil renders under ``-DEXP_TA``
(:exp:`E036`–:exp:`E040`, :dc:`kick-compute-and-3d`):

A. Preconditions
----------------

* ``BCAST[ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON] (0xF310) = 1``.
* Bind DM 3 BIF slot (``CAT_BASE_INDEX`` bits 10:8).
* Load the HWRTData into **PM context 1** (store freelists from PM ctx 0,
  load into PM ctx 1, load ALIST/MLIST SPs via ``0x0288``/``0x0190``, copy
  ``PM_CAT_BASE_*0`` → ``*1``, set ``0x0388 = 1``;
  :doc:`parameter-manager-and-spm`). On a partial render with pending
  deallocation, pause PM deallocation first and set
  ``PM_PARTIAL_RENDER_ENABLE = 1``.

B–D. ISP, ZLS, MSAA and PBE registers (broadcast)
-------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 34 36

   * - Register(s)
     - Data source
     - Notes
   * - ``0x4070``
     - ``usc_pixel_output_ctrl``
     - USC pixel output control
   * - ``0x4078``, ``4080``, ``4088``, ``4090``
     - ``usc_clear_register[0..3]``
     - Clear colour registers
   * - ``0x40A0`` (byte)
     - ``view_idx``
     - Multiview index
   * - ``0x0F20`` / ``0x0F24``
     - ``hwrtdata.rgn_header_dev_addr``
     - 64-bit region headers base
   * - ``0x3E40`` / ``0x0F18``
     - ``screen_pixel_max`` / ``isp_mtile_size``
     - Target pixel max & macrotile size (do **not** put ``mtile_stride`` in
       ``0x0F18``, :exp:`E039`)
   * - ``0x0F28``
     - ``4 * hwrtdata_common.mtile_stride``
     - ``0x10`` for 64×64, ``0x40`` for 256×256 (:exp:`E038`, ``[spec]`` Q16l)
   * - ``0x0F30`` / ``0x0F38``
     - ``isp_aa`` / ``isp_ctl``
     - ``0x0F38`` reverts to ``0x21000`` after render
   * - ``0x0F80`` / ``0x0F88``
     - ``isp_bgobjdepth`` / ``isp_bgobjvals``
     - ``0x0F88`` reverts to ``0x200`` after render
   * - ``0x0FA0`` / ``0x0FA8``
     - ``multi_sample_ctl`` / ``flipped_multi_sample_ctl``
     - Observed ``0x88``; required for ``point_list.45`` (:exp:`E038`)
   * - ``0x0FB0/B4`` / ``0x0FB8/BC``
     - ``isp_scissor_base`` / ``isp_dbias_base``
     - 64-bit scissor & depth-bias bases
   * - ``0x0F48`` / ``0x0F4C``
     - ``isp_zlsctl`` (64-bit, Mesa ``ISP_ZLSCTL``)
     - Omitted / 0 on partial renders & colour-only; reverts to 0 after
       render (:exp:`E038`)
   * - ``0x0F50/54`` & ``0x0F58/5C``
     - ``isp_zload_store_base`` (both pairs)
     - Depth load and store bases (0 without depth)
   * - ``0x0F60/64`` & ``0x0F68/6C``
     - ``isp_stencil_load_store_base`` (both pairs)
     - Stencil load and store bases (0 without stencil)
   * - ``0x17A0`` / ``0x17A4``
     - ``tpu_border_colour_table``
     - Shared with geometry kick
   * - ``0x0F08`` / ``0x0F10`` / ``0x0FC8`` / ``0x0FCC``
     - ``0`` / ``0`` / ``0`` / ``0``
     - Pipe reset (``ISP_RENDER`` returns to ``3`` after render)
   * - ``0x0618`` / ``0x0620`` / ``0x0628``
     - ``0x80`` / ``event_pixel_pds_data`` / ``event_pixel_pds_info``
     - End-of-tile pixel-event PDS program (``frag_regs`` has no code field,
       so ``0x0618 = 0x80``)
   * - ``0x1510 + 8n`` / ``0x1550 + 8n``
     - ``pbe_word[n][0]`` / ``pbe_word[n][1]``
     - Two planar PBE banks (up to 8 MRTs, 2 words per target)

F. Trigger and resume
---------------------

* Normal kick: write ``CR[0x0298] = 0``, ``CR[0x01C8] = 0``, then launch
  with ``BCAST[0x0F00] = 1`` (``ISP_RENDER`` remains ``0`` from pipe
  reset).
* Resuming a stored context (``ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME``):
  reload per-core saved state into ``CR[0x1020]``, set
  ``ROGUE_CR_ISP_RENDER |= 0x10``, reload ``CR[0x0298]``/``0x01C8``, then
  launch with ``BCAST[0x0F00] = 1`` ``[spec]``.
* **Unused** ``FRAG_PR``: when geometry finishes without OOM, the ``FRAG_PR``
  entry sitting ahead of ``FRAG`` in the fragment cCCB is skipped without
  launching the 3D pipe — its ``read_offset`` advances and its trailing UFO
  updates still run (``[spec]`` Q16m).

Input-attachment subpasses ``[open]``
-------------------------------------

Multi-subpass render passes (such as
``dEQP-VK.renderpasses.renderpass1…d32_sfloat.input.clear.dont_care.draw``)
use **two distinct HWRTData** (the second sharing VHeap/TPC and placing its
region headers at ``+0x80``, with its own MLIST, VDM stream and PM
catalogues, using ``CONTEXT_PB_BASE = 0`` shared-PB mode). A fault at VA
``0x2000`` decodes as ``TAG_ID = 0x25`` (**IPF control request**) reading an
invalid region-header, MLIST or catalogue base (:dc:`input-attachment-dump`,
:exp:`E039`, :exp:`E040`).

Transfer kick (``TQ_3D`` / DM 3)
================================

``TQ_3D`` executes blits and resolves through the 3D pixel pipe (verified in
:exp:`E020`, :exp:`E022`, :exp:`E023`, :exp:`E026`). Binding the DM 3 BIF
slot is a mandatory gate before writing any ISP/PBE register.

Complete broadcast register table (:dc:`kick-compute-and-3d`, Q13/Q14/Q25):

.. list-table::
   :header-rows: 1
   :widths: 28 36 36

   * - Register(s)
     - Value (from ``rogue_fwif_transfer_regs``)
     - Condition
   * - ``0xF310`` ``MULTICORE_FRAGMENT_CTRL_COMMON``
     - ``1`` (single-core GPU)
     - Always
   * - ``0x0F88``
     - ``isp_bgobjvals``
     - Always
   * - ``0x06A0/A4``, ``0x06A8/AC``, ``0x06B8/BC``
     - ``pds_bgnd0_base``, ``pds_bgnd1_base``, ``pds_bgnd3_sizeinfo``
     - Always (``0x06B0`` is **never** written)
   * - ``0x0F20/24``, ``0x0F28``, ``0x0FC8/CC``
     - ``isp_mtile_base``, ``isp_rgn``, ``0`` / ``0xE4``
     - **Only if** ``(isp_render & 3) == 2`` (``FAST_2D``; writing outside
       this condition faults at address 0, :exp:`E022`)
   * - ``0x4070``, ``0x4078..0x4090``
     - ``usc_pixel_output_ctrl``, ``usc_clear_register0..3``
     - Always
   * - ``0x3E40``, ``0x0F18``, ``0x0F10``
     - ``frag_screen``, ``isp_mtile_size``, ``isp_render_origin``
     - Always
   * - ``0x0FD8``
     - ``min(((isp_ctl >> 12) & 0xF) + 1, 2)``
     - Always (tiles in flight)
   * - ``0x0F38`` ``ISP_CTL``, ``0x0F30``
     - ``isp_ctl``, ``isp_aa``
     - Always
   * - ``0x0FA8`` / ``0x0FAC``
     - ``0x88888888`` / ``0x88888888``
     - **Only if** ``(isp_aa & 3) != 0``
   * - ``0x0F48`` / ``0x0F4C``
     - ``0`` / ``0``
     - Always
   * - ``0x0628``, ``0x0618``, ``0x0620``
     - ``event_pixel_pds_info``, ``_code``, ``_data``
     - Always (see pixel-event layout below)
   * - ``0x1510 + 8n`` / ``0x1550 + 8n``
     - ``pbe_wordx_mrty[2n]`` / ``[2n + 1]``
     - Targets ``n = 0..2`` (two planar PBE banks, format/stride/clip only)
   * - ``0x0388``
     - ``0``
     - Always (suppresses ``PM_3D_MEM_FREE`` on transfers)
   * - ``0x0F08`` ``ISP_RENDER``
     - ``isp_render``
     - **Immediately before launch** (writing earlier drops the first tile of
       multi-tile transfers; :exp:`E023`)
   * - ``0x0F00``
     - ``1``
     - **Always last write** (pixel-pipe launch; completes on
       ``PIXELBE_END_RENDER``)

Pixel-event PDS registers and destination address (Q13)
-------------------------------------------------------

The destination surface address is **not** in ``pbe_wordx_mrty`` (which holds
only PBE register words: format, stride, swizzle, clip). The host places PBE
**state words** (including ``address_low``/``address_high``) in the data
segment of the end-of-tile pixel-event PDS program:

* ``0x0618``: pixel-event PDS **code** offset (bits 31:4, 128-bit granular,
  relative to ``PDS_EXEC_BASE`` ``0x0610``);
* ``0x0620``: pixel-event PDS **data** offset (bits 31:4, 128-bit granular);
* ``0x0628``: pixel-event **info** (``const_size`` bits 4:0, ``temp_stride``
  bits 8:5, ``usc_sr_size`` bits 14:9). If ``const_size == 0`` or
  ``usc_sr_size == 0``, the pixel-event task is skipped and
  ``PIXELBE_END_RENDER`` never arrives.

Per-DM completion cache flush
=============================

Runs after clearing the completion event and **before** retiring the command
so host memory is coherent when fences signal (:dc:`kick-compute-and-3d` Q14,
verified in :exp:`E023`). All writes are broadcast:

.. list-table::
   :header-rows: 1
   :widths: 8 46 46

   * - Step
     - Fragment / ``TQ_3D`` (DM 3)
     - Compute (DM 4)
   * - 1
     - ``0x4000 = 1``
     - ``0x4000 = 1``
   * - 2
     - ``0x0688 = 2``, poll bit 1
     - ``0x0688 = 4``, poll
   * - 3
     - ``0x0668 = 3``, poll bit 0
     - ``0x0668 = 9``, poll bit 0
   * - 4
     - ``0x1608 = 0x38001``, poll bit 0
     - ``0x1608 = 0x10001``, poll bit 0
   * - 5
     - ``0x1480 = 1``, ``0x1488 = 1``
     - ``0x1488 = 1``
   * - 6
     - ``MCU_FENCE = 0x180_00000000`` (``DM_PIXEL``)
     - ``MCU_FENCE = 0x280_00000000`` (``DM_COMPUTE``)
   * - 7
     - ``0x1720 = 0x2B``, poll ``0x1728`` bit 0
     - ``0x1720 = 0x33``, poll ``0x1728`` bit 0
   * - 8
     - MIPS write barrier
     - —
   * - 9
     - ``SLC_CTRL_FLUSH_INVAL = 6`` (``DM_TA | DM_PIXEL``), poll
       ``SLC_STATUS0`` bit 2
     - ``SLC_CTRL_FLUSH_INVAL = 8`` (``DM_COMPUTE``), poll ``SLC_STATUS0``
       bit 2

A geometry variant also exists (``0x0688 = 1``, ``0x0668 = 5``, SLC
``DM_TA``) ``[spec]``.

Context switching (preemption) and overlap rules ``[spec]``
===========================================================

Preemption only occurs when OS config flags enable it
(``ROGUE_FWIF_INICFG_OS_CTXSWITCH_{FRAG,CDM,GEOM,TDM}_EN``; mainline leaves
all at ``0``). Observed behaviour on BXE-2-32 (:dc:`scheduler` Q24):

* **Fragment (DM 3)**: store requested via ``BCAST[0x1000] = 1`` while
  ``(ISP_RENDER & 3) != 3``; completes on normal ``PIXELBE_END_RENDER``, sets
  ``ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME`` without retiring the cCCB command,
  and resumes later by restoring PM ctx 1, ``0x1020`` and
  ``ISP_RENDER |= 0x10``.
* **Geometry (DM 2)**: **not preempted mid-pass** — the store completes only
  when ``TA_FINISHED`` arrives naturally.
* **Compute (DM 4)**: on mainline + Mesa, a CDM store faults at low VA
  ``0xD0`` and triggers HWR (making CDM CSW unusable on this stack).
* **Inter-DM overlap rules**: scheduling blocks a DM while HWR is active,
  when ``DISABLE_DM_OVERLAP`` is set and another DM runs, or while a partial
  render is pending (final fragment cannot start ahead of ``FRAG_PR``).
