.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

======================
Status and experiments
======================

Board: Banana Pi BPI-F3 (SpacemiT K1), kernel ``7.4.0-rc1-img`` (branch
``IMG-BXE-2-32``), mainline ``powervr``, Mesa ``pvr``.

What works
==========

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Feature
     - Verification
   * - ``.fw`` container & device info
     - ``FW version v1.0 (build 1 OS)``; SLC 16 KiB, 6 ERNs, ``LAYOUT_MARS``,
       ``SYS_BUS_SECURE_RESET`` (:exp:`E001`)
   * - MIPS boot, wired/dynamic TLB, wrapper remap
     - Boot markers in ``0x1A00``, refill count in ``0x1A08``
   * - ``firmware_started`` handshake & Vulkan enum
     - ``[drm] Initialized powervr 1.0.0``; ``PowerVR B-Series BXE-2-32 MC1``
   * - GPU bring-up (``gpu_init.c``)
     - Dust power, SLC bypass, PDS/USC exec bases, unit defaults, init
       handshakes, MTS association (:exp:`E013`)
   * - KCCB, return slots, MTS BG & IRQ tasks
     - BG task per KCCB batch (:exp:`E016`), HW2 IRQ task on every
       completion (:exp:`E018`)
   * - Runtime PM & warm boot
     - Full ``IDLE → FORCED_IDLE → OFF → power off → warm boot`` cycle
       (:exp:`E005`)
   * - Compute (``CDM``) jobs
     - ``compute.pipeline.basic`` **73/73** supported, ``indirect_dispatch``
       **24/24**, ``builtin_var`` **11/11** (:exp:`E021`, :exp:`E023`)
   * - Transfer (``TQ_3D``) jobs
     - ``api.fill_and_update_buffer`` **72/72**, ``buffer_to_buffer``
       **8/8**, image compute cases pass (:exp:`E023`); most of
       ``api.copy_and_blit.core`` passes (:exp:`E026`)
   * - Per-DM completion flush
     - Front-end, USC/PDS, TPU, MCU fence and SLC flush before retirement
       (:exp:`E023`)
   * - Hardware recovery (HWR)
     - MMU fault or watchdog (``max_deadline_ms``) in ~0.28 ms; 13
       recoveries in one dEQP run without a hang (:exp:`E019`)
   * - Statistics, HWR log & reset notifications
     - Utilisation, time correlation, HWR log, FWCCB context-reset
       notification decoded by host (:exp:`E025`)
   * - Experimental 3D rendering (``-DEXP_TA``)
     - ``simple_draw`` **4/4**, ``api.smoke`` **8/8**, ``basic_draw``
       **203 pass / 0 fail** (128 not supported), depth load/store
       (:exp:`E038`)

Open items and next steps
=========================

* **Geometry and fragment rendering** (``-DEXP_TA``): input-attachment
  renders still fault and caused a board hang during ``basic_draw`` in
  :exp:`E040` (Q16n); ``FRAG_PR``, multi-kick geometry, OOM/freelist grow
  and partial renders remain to be completed before enabling ``GEOM``/``FRAG``
  in the default build.
* **Stability run**: repeat ``tools/stab-run.sh`` (interrupted by external
  reboot in :exp:`E026`).
* **Offset/NPOT/mirror transfer edge cases**: investigate
  ``buffer_to_buffer_with_offset`` (host-side / Q25).

Clean-room rules
================

Every experiment in :lr:`docs/logRefs/ <>` observes **this firmware** and
the GPU hardware it drives through the public host driver and ``/dev/mem``:

* Never read, run or disassemble the vendor firmware (``rogue_*.fw``,
  ``rgx.fw*``), its dumps or captures.
* Never read vendor DDK / proprietary driver sources or headers.
* Never modify :dc:`docsClean/ <README>`; send specification gaps back via
  ``docs/questions.md``.

Experiment index (``docs/logRefs/``)
====================================

.. list-table::
   :header-rows: 1
   :widths: 10 14 26 50

   * - ID
     - Date
     - Topic
     - Finding
   * - :exp:`E001`
     - 2026-11-08
     - MIPS stalls after reset
     - ``SYS_BUS_SECURE_RESET`` required in device info
   * - :exp:`E002`
     - 2026-11-08
     - TLB miss inside IRQ handler
     - C handlers must clear ``EXL`` before running
   * - :exp:`E003`
     - 2026-11-08
     - TLB flush crashes FW
     - ``tlb_flush_dynamic`` must execute from unmapped KSEG0
   * - :exp:`E004`
     - 2026-11-08
     - SoC hang on register read
     - Never read RGX registers with the GPU power domain off
   * - :exp:`E005`
     - 2026-11-08
     - Runtime PM & warm boot
     - Full boot runs on every power-on; ``pow_state`` must track idle
   * - :exp:`E006`
     - 2026-11-08
     - First compute kick
     - CDM starts, faults at ``0x26C0`` (PDS exec base missing)
   * - :exp:`E007`
     - 2026-11-08
     - Register survey ``0x0000–0x2000``
     - Boot/idle register values (:lr:`data/survey-0-2000.txt`)
   * - :exp:`E008`
     - 2026-11-08
     - Writable-bit probe
     - Mapped 40-bit address registers; found ``0x0610`` and ``0x0FC8``
   * - :exp:`E009`
     - 2026-11-08
     - ``pds_exec_base`` → ``0x0610``
     - ``0x26C0`` fault gone (``0x0610 = PDS_EXEC_BASE``); reload hang hazard
   * - :exp:`E010`
     - 2026-11-08
     - Truncated ``.fw`` after hang
     - Kernel falls back silently to ``/lib/firmware/powervr/``
   * - :exp:`E011`
     - 2026-11-08
     - ``0x0128`` event enable
     - ``EVENT_ENABLE`` routes to host IRQ; must remain ``0``
   * - :exp:`E012`
     - 2026-11-08
     - ``usc_exec_base`` → ``0x0FC8``
     - ``0x0FC8`` is an ISP register, not the USC exec base
   * - :exp:`E013`
     - 2026-11-08
     - Full GPU units bring-up
     - **First compute jobs pass** (``empty_shader``, ``copy_ssbo_*``, etc.)
   * - :exp:`E014`
     - 2026-11-08
     - First ``TQ_3D`` kick
     - ``0x06A0..0x06B8 = PDS_BGRND0..3``; launch trigger missing
   * - :exp:`E015`
     - 2026-11-08
     - Fault in new memory context
     - Flush and invalidate SLC before MMU invalidation
   * - :exp:`E016`
     - 2026-11-08
     - Completion delivery counters
     - BG task confirmed; IRQ conclusion later superseded by :exp:`E018`
   * - :exp:`E017`
     - 2026-11-08
     - ``TQ_3D`` launch ``0x0F00 = 1``
     - 3D pipe starts, then faults writing to address 0
   * - :exp:`E018`
     - 2026-11-08
     - MTS IRQ task delivery
     - HW2 fires on every completion; timer poll only raced it
   * - :exp:`E019`
     - 2026-11-08
     - HWR + ``compute.pipeline.basic``
     - Recovery in ~0.28 ms; 67/82 pass (6 image fails pending TQ)
   * - :exp:`E020`
     - 2026-11-08
     - ``TQ_3D`` two-bank PBE
     - Two PBE banks & ``event_pixel_pds_*`` verified; addr-0 fault remains
   * - :exp:`E021`
     - 2026-11-08
     - Watchdog & wider compute
     - Uses ``max_deadline_ms``; ``indirect_dispatch`` 24/24, ``builtin_var`` 11/11
   * - :exp:`E022`
     - 2026-11-09
     - Full Q13 ``TQ_3D`` table
     - Conditional regs fixed addr-0 fault; multi-tile lost first tile
   * - :exp:`E023`
     - 2026-11-09
     - TQ launch order + per-DM flush
     - ``ISP_RENDER`` right before ``0x0F00``; **0 failures** across 5 groups
   * - :exp:`E024`
     - 2026-11-09
     - ``vkfwtest`` workloads
     - Compute/transfer clean; graphics waiting on TA/3D kicks
   * - :exp:`E025`
     - 2026-11-09
     - Stats, HWR log, FWCCB reset
     - Time correlation, utilisation, HWR log and reset notifications verified
   * - :exp:`E026`
     - 2026-11-09
     - ``SLCFLUSHINVAL``, ``copy_and_blit``
     - Real SLC flush added; ``copy_and_blit`` mostly passes
   * - :exp:`E027`
     - 2026-11-09
     - Render structures by value
     - Mapped ``hwrtdata_common`` & context state to observed geometry regs
   * - :exp:`E028`
     - 2026-11-09
     - First TA kick (``-DEXP_TA``)
     - Canary found ``0x0CB0`` TA context-state base; VDM runs, PM idle
   * - :exp:`E029`
     - 2026-11-09
     - Q16b/Q16c TA registers
     - ``0x0420`` counts kicks; PM still allocates nothing
   * - :exp:`E030`
     - 2026-11-09
     - Global FL bank & ``CONTEXT_PB_BASE``
     - ``0x0D00 = 0x0F``; PM catalogue setup remaining gap
   * - :exp:`E031`
     - 2026-11-09
     - Live PM stack pointers
     - Live SPs and counters are read-only and stay 0
   * - :exp:`E032`
     - 2026-11-09
     - PM live SP load variants
     - ``0x02A8`` acks ``PM_PAGE_MANAGEOP``; writes inside ``0x0250`` fault
   * - :exp:`E033`
     - 2026-11-09
     - First-load PM flags
     - Idle PM state matches reference; live SP copy still missing
   * - :exp:`E034`
     - 2026-11-09
     - Q16i PM copy variants
     - ``0xF320 = 0``, catalogue index after PM writes; load trigger missing
   * - :exp:`E035`
     - 2026-11-09
     - Free-list load commands
     - ``0x0208``/``0x0200``/``0x20B0``/``0x20A8`` load live SPs; **TA finishes**
   * - :exp:`E036`
     - 2026-11-09
     - First 3D render
     - **First correct triangle rendered**; PM pages not yet freed
   * - :exp:`E037`
     - 2026-11-09
     - ALIST/MLIST SP load commands
     - ``0x0288``/``0x0190`` fix page freeing; ``simple_draw`` 4/4, ``smoke`` 8/8
   * - :exp:`E038`
     - 2026-11-10
     - ZLS, MSAA & ``PM_3D_MEM_FREE``
     - ``basic_draw`` **203/331 (0 fails)**, depth works; input attachments fault
   * - :exp:`E039`
     - 2026-11-10
     - Input-attachment diff
     - Non-first kicks skip PM load; ``0x0F18`` stays ``isp_mtile_size``
   * - :exp:`E040`
     - 2026-11-10
     - Input-attachment dump diff
     - Dumped ranges match; shared-PB rule added; still faults / board hang
