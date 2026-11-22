.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=================================
Power, HWR, statistics and trace
=================================

Power management (``src/power.c``)
==================================

``update_pow_state()`` keeps ``sysdata.pow_state`` accurate: ``ON`` while any
DM is busy or any context has pending commands, ``FORCED_IDLE`` under a host
forced-idle request, and ``IDLE`` otherwise.

``pow_handle()`` handles ``KCCB_POW`` requests and signals completion by
writing ``1`` to ``*fwptr(osdata.power_sync_fw_addr)``:

* ``FORCED_IDLE_REQ`` (force/timeout): sets ``g.forced_idle = true``,
  ``pow_state = FORCED_IDLE``, sync = ``1``.
* ``FORCED_IDLE_REQ`` (cancel): clears ``g.forced_idle``, restores
  ``pow_state = IDLE``, sync = ``1``.
* ``OFF_REQ``: traces ``POW_59`` and ``MAIN_145``/``MAIN_144``, calls
  ``tlb_flush_dynamic()``, sets ``pow_state = OFF``, masks the CP0 timer
  interrupt, sets ``g.powered_off = true``, sync = ``1``.
* ``NUM_UNITS_CHANGE`` / ``APM_LATENCY_CHANGE``: acknowledges with sync =
  ``1``.

Per-job dust power (``POW_RASCALDUST``) is not needed because mainline
``powervr`` does not enable it; dusts are powered on at boot and after HWR.

Hardware recovery (``src/hwr.c``)
=================================

Triggered by either:

* ``EVENT_STATUS.MMU_PAGE_FAULT`` (``kick_events()`` first traces ``HWR_80``
  with ``BIF_FAULT_BANK0_MMU_STATUS`` and ``REQ_STATUS``), or
* ``hwr_watchdog()`` on the 1 ms timer tick whenever a DM has been busy for
  longer than its context's ``max_deadline_ms`` (30 s from mainline host,
  30 s fallback; :exp:`E021`).

``hwr_recover()`` executes the reset sequence (:dc:`hardware-recovery`,
verified in :exp:`E019` at ~0.28 ms):

1. Clear ``HARDWARE_OK`` and set ``RESET_IN_PROGRESS`` in
   ``sysdata.hwr_state_flags``.
2. Flush and invalidate the SLC and wait for idle.
3. Clear ``PM_PARTIAL_RENDER_ENABLE = 0``.
4. Pulse ``SOFT_RESET`` via broadcast: ``0x13FFFC1D`` / ``0xFFEFFE00`` →
   ``0`` (leaves the MIPS core and MTS wrapper running).
5. Re-run ``gpu_power_on()``, ``gpu_units_init()`` and ``bif_rebuild()``.
6. Clear all GPU events and discard the running command on every busy DM
   (its trailing UFO updates still execute so host fences complete).
7. Record a ``rogue_hwrinfo`` entry and queue a FWCCB
   ``CONTEXT_RESET_NOTIFICATION`` for each discarded command (:exp:`E025`).
8. Restore ``HARDWARE_OK | DM_RUNNING_OK``, increment ``CR_DBG_HWR_COUNT``,
   and reschedule.

Traces emitted during recovery: ``HWR_32``, ``HWR_28``, ``HWR_61``,
``MAIN_27``, ``HWR_14``, ``HWR_25`` and ``HWR_62``.

Statistics and HWR reporting (``src/stats.c``)
==============================================

Verified in :exp:`E025`:

* **Time correlation**: appends an entry to ``gpu_util_fwcb.time_corr[]`` at
  every boot and on ``KCCB_CORECLKSPEEDCHANGE``, then bumps
  ``time_corr_seq_count``.
* **Utilisation**: accumulates elapsed nanoseconds into
  ``stats_counters[ACTIVE | BLOCKED | IDLE]``, updates
  ``last_word = time | state``, updates ``sysdata.start_idle_time`` on idle,
  and traces ``POW_2``, ``POW_22`` and ``DBG_20``.
* **HWR log & FWCCB**: populates ``rogue_hwrinfo`` (first 8 entries kept,
  last 8 ring-buffered, plus per-DM counters) and sends FWCCB
  ``CONTEXT_RESET_NOTIFICATION`` (105).

Trace points emitted (``src/util.c``)
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - SFID
     - Event
   * - ``DBG_4``
     - Pre-init MTS register snapshot; ``0xDEAD00cc, EPC, BadVAddr`` on fatal
       exception
   * - ``POW_7``, ``MAIN_69``
     - Dust power on; ``GPU Units init (# mask: …)``
   * - ``POW_23``, ``MAIN_176``
     - End of firmware init
   * - ``MTS_19`` / ``MTS_17``
     - KCCB command picked / return value
   * - ``MAIN_14``
     - ``KICK``: new write offset, DM, context
   * - ``MTS_9``
     - Task dispatched to a DM
   * - ``BIF_14``, ``BIF_19``
     - Memory context bound to a BIF slot
   * - ``MAIN_122`` / ``MAIN_6``
     - Compute kick / compute finished
   * - ``MAIN_4``
     - 3D transfer (``TQ_3D``) finished
   * - ``MAIN_22``
     - UFO update
   * - ``MAIN_171`` / ``MAIN_172``
     - Bad cCCB command / unimplemented task retired immediately
   * - ``MAIN_93``
     - Clock speed change
   * - ``POW_2``, ``POW_22``, ``POW_19``, ``POW_59``, ``POW_6``,
       ``MAIN_145``, ``MAIN_144``
     - Idle timer and power-state transitions
   * - ``HWR_13``
     - ``cr_poll32`` timeout (also stored in ``sysdata.cr_poll_*``)
   * - ``HWR_80``, ``HWR_32``, ``HWR_28``, ``HWR_61``, ``MAIN_27``,
       ``HWR_14``, ``HWR_25``, ``HWR_62``
     - MMU fault dump and HWR sequence
   * - ``MAIN_90``
     - FWCCB ring full
