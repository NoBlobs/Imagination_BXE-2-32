.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

=========================
Role of the Linux driver
=========================

Mainline Linux's ``drm/imagination`` driver (``drivers/gpu/drm/imagination/``,
module ``powervr.ko``) drives the PowerVR Rogue GPU from the host RISC-V CPUs
and treats the MIPS firmware as an autonomous coprocessor that owns the
internal GPU scheduling, cache flushes, power handshakes and hardware
recovery.

Responsibilities on the host side
=================================

Around firmware boot and normal operation, ``powervr.ko`` performs the
following tasks:

#. **SoC enablement**: powers on the K1 APMU GPU domain (``K1_PD_GPU``),
   enables the core clock (``CLK_GPU``), de-asserts ``RESET_GPU``, maps the
   512 KiB MMIO window at ``0xCAC00000`` and claims IRQ 75
   (:doc:`kernel-integration`).
#. **Firmware loading and parsing**: loads
   ``powervr/rogue_36.29.52.182_v1.fw`` via the kernel firmware loader
   (``/lib/firmware/updates/powervr/`` overrides ``/lib/firmware/powervr/``),
   parses the trailing 4 KiB header, layout table and device-info block, and
   prints ``FW version v1.0 (build 1 OS)`` (:doc:`fw-container-and-abi`).
#. **Device feature discovery**: constructs its entire feature, BRN and ERN
   view of the GPU from the firmware's device-info block. Missing flags change
   host behaviour directly — for example, ``SYS_BUS_SECURE_RESET`` is required
   for the host to clear ``ROGUE_CR_SYS_BUS_SECURE`` (:exp:`E001`), and
   ``LAYOUT_MARS = 1`` makes ``pvr_fw_stop()`` check ``MARS_IDLE``.
#. **Firmware heap allocation and mapping**: allocates the ``code`` and
   ``data`` firmware objects according to the 6-entry layout table, builds
   the 4-page MIPS page table covering the 16 MiB firmware heap at
   ``0xC0000000``, configures the fixed MIPS wrapper remaps
   (``MIPS_ADDR_REMAP1..5``), populates ``BOOT_DATA``, ``sysinit``
   (``0xC0FF0000``) and ``osinit`` (``0xC0FE0000``), and sets
   ``firmware_started = 0``.
#. **MIPS reset release and handshake**: clears ``SYS_BUS_SECURE`` and
   releases the MIPS core at ``0xBFC00000``, then polls
   ``firmware_started == 1`` before registering the DRM device
   (``[drm] Initialized powervr 1.0.0 for cac00000.gpu``).
#. **Command submission and completion**: writes Kernel CCB (KCCB) entries,
   pokes ``ROGUE_CR_MTS_SCHEDULE``, waits on KCCB return slots and GPU
   interrupts (IRQ 75, disambiguated via ``osdata.interrupt_count[0]``),
   consumes Firmware CCB (FWCCB) notifications, and decodes the firmware trace
   buffer in debugfs (:doc:`host-protocol`).
#. **Runtime PM**: after ``active_pm_latency_ms`` of idle time (when
   ``sysdata.pow_state == IDLE`` and KCCB is empty), runs the
   ``FORCED_IDLE_REQ`` → ``OFF_REQ`` handshake over ``KCCB_POW``, powers down
   the APMU domain, and performs a full warm boot on the next job
   (:exp:`E005`).

Userspace stack (Mesa ``pvr``)
==============================

Userspace Vulkan workloads run through Mesa's ``pvr`` ICD
(``powervr_mesa_icd.riscv64.json``) with
``PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1``:

* ``vulkaninfo`` enumerates ``PowerVR B-Series BXE-2-32 MC1``.
* Mesa submits compute (``CDM``), transfer (``TQ_3D``) and graphics
  (``GEOM`` / ``FRAG``) command buffers through DRM ioctls, which
  ``powervr.ko`` places into per-context client CCBs (cCCBs) with timeline
  ``FENCE_PR`` and sync-checkpoint UFOs.
* ``TDM`` / ``TQ_2D`` transfers are not emitted by Mesa on BXE-2-32; buffer
  copies and fills go through ``TQ_3D`` on the fragment data master.
* Mainline ``powervr.ko`` leaves GPU clock gating in ``AUTO``, does not
  allocate a HWPerf buffer, writes no time-correlation entries of its own,
  does not send ``KCCB_SLCFLUSHINVAL``, and logs FWCCB
  ``CONTEXT_RESET_NOTIFICATION`` events to ``dmesg`` (:exp:`E025`).
