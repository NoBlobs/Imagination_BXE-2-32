.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=================================================
Power, HWR, auxiliary events and latencies
=================================================

This page documents the hardware side of Active Power Management (APM),
Hardware Recovery (HWR), auxiliary GPU events (USC trigger and GPIO
handshake), and reference timing on BXE-2-32 (:dc:`power-management`,
:dc:`hardware-recovery`, :dc:`observed-sequences`).

Active Power Management (APM) and teardown
==========================================

``rogue_fwif_sysdata.pow_state`` tracks the GPU power state:
``POW_OFF (0)``, ``POW_ON (1)``, ``POW_FORCED_IDLE (2)``, ``POW_IDLE (3)``.
Mainline ``powervr.ko`` considers the GPU idle when
``pow_state == POW_IDLE`` **and** the KCCB is empty, then drives:

.. code-block:: text

   KCCB POW(FORCED_IDLE_REQ) -> power_sync = 1
   KCCB POW(OFF_REQ, forced=1) -> teardown -> power_sync = 1
   host stops MIPS + powers off K1 APMU GPU domain (K1_PD_GPU)
   ... next job ...
   host powers on K1_PD_GPU + rewrites BOOT_DATA + releases MIPS at 0xBFC00000 (warm boot)

Observed teardown order before writing ``1`` to ``power_sync`` ``[spec]``:

1. Turn off GPU dusts and units (``GPU units deinit`` / ``GPU deinit``).
2. Disable GPIO events.
3. Write back MIPS D-cache (if enabled) and flush dynamic MIPS TLB + remap
   entries (:exp:`E003`, :exp:`E005`).
4. Invalidate MMU caches and release/reset BIF catalogue slot tracking.
5. Mask the timer interrupt, mark ``pow_state = POW_OFF``, and signal
   ``*power_sync = 1`` **only after every teardown step has completed**.

Core clock and DVFS
-------------------

The host supplies the initial core clock in
``sysinit.initial_core_clock_speed`` and updates it via KCCB
``CORECLKSPEEDCHANGE`` (``runtime_cfg.core_clock_speed``). Timer ticks per
ms, APM deadlines and watchdogs must be recomputed from the active clock;
the firmware must never run with a ``0`` clock (fallback 307.2 MHz).

Hardware recovery (HWR) reset sequence
======================================

HWR is triggered by ``EVENT_STATUS.MMU_PAGE_FAULT`` (bit 9), a failed
hardware register poll (recorded in ``sysdata.cr_poll_addr/mask/count``,
``ROGUE_HWRTYPE_POLLFAILURE``), or a DM deadline watchdog
(``ROGUE_HWRTYPE_OVERRUN``).

Complete ordered reset sequence (:dc:`hardware-recovery` Q11, verified on
silicon in :exp:`E019` at ~0.28 ms, reference ~0.43 ms):

1. Hold scheduling: clear ``ROGUE_FWIF_HWR_HARDWARE_OK`` (bit 0) and set
   ``RESET_IN_PROGRESS`` (bit 1) in ``sysdata.hwr_state_flags``.
2. Flush and invalidate the SLC (``SLC_CTRL_FLUSH_INVAL``) and poll
   ``SLC_STATUS0`` bit 2 (``FLUSH_INVAL_PENDING``) until clear.
3. Disable partial renders: ``ROGUE_CR_PM_PARTIAL_RENDER_ENABLE (0x0338) = 0``.
4. Pulse ``ROGUE_CR_SOFT_RESET`` via **XPU broadcast**:

   * Assert: low word (``0x0100``) = ``0x13FFFC1D``, high word (``0x0104``)
     = ``0xFFEFFE00``;
   * Release: both words = ``0``.

   This mask resets the GPU pipelines **without resetting the MIPS core or
   the MTS (Garten) wrapper** (under ``LAYOUT_MARS = 1``, MIPS sits in its
   own MARS domain). ``SOFT_RESET2`` is not used in HWR, and the full
   power-on mask ``0xF3FFFC1D`` / ``0xFFEFFFF8`` must **not** be used.
5. Re-run GPU power-on setup, full GPU units init (:doc:`gpu-bringup`) and
   rebuild BIF catalogue slots ``BIF_CAT_BASE1..7``.
6. Clear pending GPU events, discard each busy DM's guilty command (letting
   trailing UFO updates run so host fences do not hang), record
   ``rogue_hwrinfo`` and send FWCCB ``CONTEXT_RESET_NOTIFICATION``
   (:exp:`E025`), and request ``FREELISTS_RECONSTRUCTION`` if freelists were
   affected.
7. Restore ``hwr_state_flags = HARDWARE_OK | DM_RUNNING_OK``, poke
   ``ROGUE_CR_MTS_SCHEDULE`` and reschedule.

``hwr_state_flags`` bit definitions (``[public]``): bit 0 ``HARDWARE_OK``,
bit 1 ``RESET_IN_PROGRESS``, bit 3 ``GENERAL_LOCKUP``, bit 4
``DM_RUNNING_OK``, bit 5 ``DM_STALLING``, bit 6 ``FW_FAULT``, bit 7
``RESTART_REQUESTED``.

Auxiliary GPU events ``[spec]``
===============================

USC trigger event (``EVENT_STATUS`` bit 15)
-------------------------------------------

.. code-block:: text

   ROGUE_CR_EVENT_CLEAR = 0x8000
   osdata.interrupt_count[0]++
   write_barrier()
   ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS = 1

If ``GPIO_REQ`` (bit 12) is simultaneously pending and no power request is
in progress, GPIO is serviced first.

GPIO handshake (``EVENT_STATUS`` bits 12 / 13)
----------------------------------------------

Unpublished offsets: ``0x0150`` (8-bit GPIO input), ``0x0158`` (ack output),
``0x0B90`` (event re-arm/mask):

.. code-block:: text

   on EVENT_STATUS bit 12:
     v = CR[0x0150]
     EVENT_CLEAR = 0x1000 ; poll bit 12 == 0
     CR[0x0158] = 1
     poll bit 12 == 1 ; EVENT_CLEAR = 0x1000 ; poll bit 12 == 0
     BCAST[0x0B90] = 0x1000
   on any timeout:
     EVENT_CLEAR = 0x3000 ; BCAST[0x0B90] = 0x1000
     clear bit 12 in MTS_DM0_INTERRUPT_ENABLE   # "GPIO disabled"

Reference latencies ``[spec]``
==============================

Observed median timings for a transfer job on the reference board
(:dc:`observed-sequences`):

.. list-table::
   :header-rows: 1
   :widths: 34 22 22 22

   * - Stack / core clock
     - IRQ → completion
     - Completion → next kick
     - Kick → completion (GPU)
   * - Vendor stack, 614 MHz
     - 10.8 µs
     - 53.8 µs
     - 876 µs
   * - Mainline (open), 307 MHz
     - 14.2 µs
     - 180.8 µs
     - 2225 µs
   * - Mainline (open), 614 MHz
     - 8.8 µs
     - 118.8 µs
     - 1044 µs
