.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=============
Test platform
=============

All board experiments in :lr:`docs/logRefs/ <>` run on a Banana Pi BPI-F3
based on the SpacemiT K1 SoC.

Reference board
===============

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Item
     - Value
   * - Board
     - Banana Pi BPI-F3, SpacemiT K1 (8× X60 RISC-V), DRAM trained at
       1066 MT/s by the FSBL
   * - GPU MMIO & IRQ
     - ``0xCAC00000`` (512 KiB), IRQ 75
   * - Serial console
     - UART console at 115200 baud
   * - OS & boot chain
     - Debian 13 (RISC-V) with mainline Linux kernels booted via U-Boot /
       GRUB EFI
   * - Display
     - None in mainline: all Vulkan and dEQP tests run headless (off-screen)
   * - Mesa
     - Mesa ``pvr`` Vulkan ICD (``powervr_mesa_icd.riscv64.json``), requires
       ``PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1``
   * - Vulkan CTS
     - Khronos Vulkan CTS (``deqp-vk``) built for ``riscv64``

Bench safety rules
==================

These hardware hazards were discovered on silicon and must be respected by
every script and manual test:

* **Never read GPU registers while the GPU power domain is off.** Accessing
  ``0xCAC00000`` while the APMU GPU domain is powered down hangs the SoC bus
  and requires a physical power cycle (:exp:`E004`). Register-sampling tools
  (``tools/gpu-regs.py``, ``tools/reg-survey.py``) verify
  ``runtime_status == active`` and force ``power/control = on`` before
  touching ``/dev/mem``.
* **Reloading** ``powervr`` **with a stuck job can hang the SoC** (:exp:`E009`,
  :exp:`E011`). Always ensure HWR has completed or the GPU is idle before
  unloading the module.
* **Verify the installed** ``.fw`` **file after any hang or hard reset.** A
  truncated file in ``/lib/firmware/updates/powervr/`` causes the kernel to
  silently fall back to ``/lib/firmware/powervr/``, which contains the vendor
  firmware that the clean team must never run (:exp:`E010`). Always verify
  the SHA-256 hash and check for ``FW version v1.0 (build 1 OS)`` in
  ``dmesg``.
* **Run risky address-fetch experiments only when a power cycle is available.**
  Experiments that make GPU units fetch from new or unknown addresses can
  hang the SoC (:exp:`E009`, :exp:`E040`).
