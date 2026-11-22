.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

============================
Host ↔ firmware protocol
============================

This page describes the runtime protocol between mainline ``powervr.ko`` and
the firmware across boot, KCCB/FWCCB rings, interrupts, runtime PM,
statistics/HWR reporting and the debugfs trace buffer (:dc:`host-interface`).

Start-up handshake
==================

1. The host loads the code/data sections, populates ``BOOT_DATA``,
   ``sysinit`` and ``osinit``, sets ``firmware_started = 0`` and releases the
   MIPS core from reset.
2. The firmware discovers ``sysdata``, ``runtime_cfg``, ``tracebuf``,
   ``osdata``, KCCB/return slots and FWCCB via ``sysinit`` (``0xC0FF0000``)
   and ``osinit`` (``0xC0FE0000``).
3. It reads the core clock from ``runtime_cfg.core_clock_speed`` (falling
   back to ``sysinit.initial_core_clock_speed`` and then 307.2 MHz), sets
   ``sysdata.config_flags`` and
   ``sysdata.hwr_state_flags = HARDWARE_OK | DM_RUNNING_OK``, completes MTS
   and GPU unit bring-up, sets ``pow_state = IDLE``, and emits ``POW_23`` and
   ``MAIN_176``.
4. It writes ``firmware_started_timestamp``, executes a write barrier, and
   sets ``firmware_started = 1``. The host polls this word and then prints
   ``[drm] Initialized powervr 1.0.0 for cac00000.gpu``.

Kernel CCB (host → firmware)
============================

The KCCB is a ring of 88-byte ``rogue_fwif_kccb_cmd`` entries controlled by
``(write_offset, read_offset, wrap_mask)`` plus a parallel array of 32-bit
return slots (:dc:`kernel-ccb`).

After queuing a command, the host writes ``ROGUE_CR_MTS_SCHEDULE`` to raise
HW1 (BG task) and waits for the entry's return slot to have
``ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED`` (bit 0) set (plus
``CLEANUP_BUSY``, bit 1, if a cleanup must be retried).

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - KCCB command
     - Host / firmware contract
   * - ``KICK`` (101)
     - Updates a context's client CCB ``write_offset`` / ``wrap_mask``,
       increments cleanup ``submitted_commands``, schedules work
   * - ``COMBINED_GEOM_FRAG_KICK`` (117)
     - Dispatched as two ``KICK`` operations (geometry then fragment)
   * - ``MMUCACHE`` (102)
     - SLC flush/inval (:exp:`E015`), BIF invalidation, PM TLB inval,
       dynamic MIPS TLB flush, then writes the command's sync value
   * - ``CLEANUP`` (106)
     - Frees ``FWCOMMONCONTEXT`` / ``HWRTDATA`` / ``FREELIST`` / ``ZSBUFFER``
       when idle, otherwise returns ``CLEANUP_BUSY`` so the host retries
   * - ``POW`` (107)
     - Power-state transitions acknowledged via ``osdata.power_sync_fw_addr``
   * - ``FORCE_UPDATE`` (116)
     - Marks a context's sync checkpoints errored at the fence and re-runs
       the context
   * - ``CORECLKSPEEDCHANGE`` (204)
     - Reloads core clock from ``runtime_cfg``, appends a time-correlation
       entry, traces ``MAIN_93``
   * - ``STATEFLAGS_CTRL`` (209)
     - Sets, ORs or clears ``config_flags`` mirrored to ``sysdata``
   * - ``LOGTYPE_UPDATE`` (206)
     - Acknowledged; ``log_type`` is read live from the trace control block
   * - ``SLCFLUSHINVAL`` (105)
     - Full SLC flush/inval (:exp:`E026`); not sent by mainline ``powervr``
   * - ``HEALTH_CHECK``, ``BP``, freelist/ZS updates, ``REGCONFIG``, HWPerf,
       PDVFS, ``PHR_CFG``, ``WDG_CFG``, …
     - Acknowledged with ``CMD_EXECUTED``

Mainline ``powervr.ko`` command subset (Q26, ``[public]``): upstream
``drivers/gpu/drm/imagination/*.c`` only ever sends ``KICK`` (101),
``COMBINED_GEOM_FRAG_KICK`` (117), ``MMUCACHE`` (102), ``CLEANUP`` (106),
``POW`` (107), ``FREELIST_GROW_UPDATE`` (110),
``FREELISTS_RECONSTRUCTION_UPDATE`` (112), ``HEALTH_CHECK`` (115) and
``LOGTYPE_UPDATE`` (206). All other KCCB IDs (``BP``, ``REGCONFIG``,
``NOTIFY_WRITE_OFFSET_UPDATE``, ``OS_ONLINE_STATE_CONFIGURE``, ``PHR_CFG``,
``WDG_CFG``, HWPerf, PDVFS) are never emitted by mainline.

Paired geometry + fragment submission (Q22)
-------------------------------------------

To express that a fragment render depends on its geometry pass, mainline
``powervr.ko`` (``pvr_queue.c``, ``pvr_cccb.c``) queues a ``FENCE_PR``
command in the fragment cCCB referencing the geometry queue's timeline UFO
at the geometry completion value, sets the geometry command's
``partial_render_geom_frag_fence`` to one below that value (for SPM OOM
checks), and submits both cCCBs together in a single
``COMBINED_GEOM_FRAG_KICK`` (117). If no OOM occurred, the ``FRAG_PR``
command preceding ``FRAG`` in the fragment cCCB is retired without running
the 3D pipe (:dc:`client-ccb`).

Host IRQ notification and visibility barrier
--------------------------------------------

To wake the host driver (IRQ 75):

1. Execute ``write_barrier()``: write uncached ``BD_BARRIER``, execute
   ``EHB`` + ``SYNC``, read ``BD_BARRIER`` back, read ``CORE_REVISION``.
2. Increment ``osdata.interrupt_count[0]`` and barrier again.
3. Write ``ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS = 1`` (``0x08A8``).

The Linux IRQ handler compares ``osdata.interrupt_count[0]`` against its last
observed count to determine whether the firmware raised the interrupt.

Firmware CCB (firmware → host)
==============================

The FWCCB is a ring of ``0x58``-byte firmware-to-host notifications
(:dc:`firmware-ccb`). If the ring is full, the firmware notifies the host,
traces ``MAIN_90`` once, and spins in 10 µs steps until a slot frees up.

Currently used for ``CONTEXT_RESET_NOTIFICATION`` (105) after HWR
(:exp:`E025`), carrying ``server_common_context_id``, reset reason
(``GUILTY_LOCKUP`` or ``GUILTY_OVERRUNING``), DM, ``ext_job_ref``, page
catalogue and (on MMU fault) the ``PF`` flag with fault address. Mainline
``powervr`` decodes and logs it:

.. code-block:: text

   [drm] Received context reset notification on context 2
   [drm]   Reset reason=1 (Guilty lockup)  Data Master=4 (Compute)  Job ref=1
   [drm]   Page fault occurred, fault address=26c0

Full upstream FWCCB command table (``ROGUE_FWIF_FWCCB_CMD_*``,
:dc:`firmware-ccb`): ``101 ZSBUFFER_BACKING``, ``102 ZSBUFFER_UNBACKING``,
``103 FREELIST_GROW``, ``104 FREELISTS_RECONSTRUCTION``,
``105 CONTEXT_RESET_NOTIFICATION``, ``106 DEBUG_DUMP``, ``107 UPDATE_STATS``,
``108 CORE_CLK_RATE_CHANGE``, ``109 REQUEST_GPU_RESTART``,
``112 CONTEXT_FW_PF_NOTIFICATION``. Each asynchronous request
(``FREELIST_GROW``, ``ZSBUFFER_BACKING``, ``FREELISTS_RECONSTRUCTION``)
pairs with a matching KCCB ``*_UPDATE`` confirmation from the host.

Runtime power management and warm boot
======================================

When ``sysdata.pow_state == IDLE`` and the KCCB is empty for
``active_pm_latency_ms``, ``powervr.ko`` suspends the GPU using
``osdata.power_sync_fw_addr`` (host writes ``0``, sends ``KCCB_POW``, polls
for non-zero):

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - ``KCCB_POW`` request
     - Firmware response
   * - ``FORCED_IDLE_REQ`` (force/timeout)
     - Stop dispatching new tasks, ``pow_state = FORCED_IDLE``, sync = ``1``
   * - ``FORCED_IDLE_REQ`` (cancel)
     - Resume scheduling, ``pow_state = IDLE``, sync = ``1``
   * - ``OFF_REQ``
     - Trace ``POW_59`` / ``MAIN_145`` / ``MAIN_144``, flush dynamic TLB,
       ``pow_state = OFF``, mask timer IRQ, sync = ``1``
   * - ``NUM_UNITS_CHANGE``, ``APM_LATENCY_CHANGE``
     - Sync = ``1``

Once ``OFF_REQ`` completes, the kernel powers off ``K1_PD_GPU`` in the APMU.
When new work arrives, it powers the domain back on, rewrites ``BOOT_DATA``
without reloading the firmware image, and releases the MIPS reset vector. The
firmware executes a complete cold boot and rebuilds all state from
``sysinit``/``osinit`` (:exp:`E005`).

Statistics, HWR log and debugfs trace
=====================================

* **Time correlation** (``gpu_util_fwcb.time_corr[]``): populated by the
  firmware at every boot and on ``CORECLKSPEEDCHANGE`` with
  ``cr_timestamp = CR_TIMER``, ``os_timestamp`` in ns,
  ``cr_delta_to_os_delta_kns = (256e9 << 20) / core_clock``, and
  ``time_corr_seq_count++`` (:exp:`E025`).
* **GPU utilisation**: tracks ``ACTIVE``, ``BLOCKED`` and ``IDLE`` durations
  in ``stats_counters[]``, publishes ``last_word = time | state``, and sets
  ``sysdata.start_idle_time`` when going idle.
* **HWR log** (``osinit.hwr_info_buf_ctl_fw_addr``): records
  ``rogue_hwrinfo`` entries (first 8 preserved, last 8 as a ring) plus per-DM
  recovery counters.
* **Trace buffer** (``/sys/kernel/debug/dri/N/pvr_fw/trace_0``): circular
  buffer of 32-bit words decoded by ``pvr_fw_trace.c`` using the public SFID
  table in ``pvr_rogue_fwif_sf.h`` (:dc:`hwperf-trace`, :dc:`trace-ids`).

Trace entry layout:

.. code-block:: text

   word 0   SFID  = 0x70000000 | (nparams << 16) | (group << 12) | id
   word 1   timestamp high (ROGUE_CR_TIMER, 48-bit)
   word 2   timestamp low
   word 3.. parameters (32-bit each)

Enable all trace groups from the host with:

.. code-block:: sh

   echo 0xffffffff > /sys/kernel/debug/dri/0/pvr_fw/trace_mask

Mapping from SFID group number to ``log_type`` enable bit (bit 0 = global
trace enable):

.. list-table::
   :header-rows: 1

   * - Group
     - 0 NULL
     - 1 MAIN
     - 2 CLEANUP
     - 3 CSW
     - 4 PM
     - 5 RTD
     - 6 SPM
     - 7 MTS
     - 8 BIF
     - 9 MISC
     - 10 POW
     - 11 HWR
     - 12 HWP
     - 13 RPM
     - 14 DMA
     - 15 DBG
   * - Bit
     - —
     - 1
     - 3
     - 4
     - 6
     - 7
     - 8
     - 2
     - 5
     - 14
     - 9
     - 10
     - 11
     - 12
     - 13
     - 31
