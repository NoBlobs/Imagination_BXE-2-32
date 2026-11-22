.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

==================
Kernel integration
==================

The mainline ``powervr`` driver requires no architectural modifications to run
the free firmware. Three small commits (~205 lines total) on branch
``IMG-BXE-2-32`` (based on ``v7.4-rc1``) wire up the SpacemiT K1 GPU power
domain, device-tree node and experimental BVNC entry.

Commits on branch ``IMG-BXE-2-32``
==================================

1. ``pmdomain: spacemit: add K1 APMU power domain driver (GPU domain)``
-----------------------------------------------------------------------

* Adds ``drivers/pmdomain/spacemit/k1-apmu-pd.c``, an auxiliary driver
  (``spacemit_ccu.k1-apmu-pd``) spawned by the APMU clock controller via a
  new ``pd_name`` field in the common CCU code.
* Registers a ``genpd`` onecell provider on the APMU syscon node. Domain IDs
  live in ``include/dt-bindings/power/spacemit,k1-power.h`` (VPU 1, **GPU 2**,
  LCD 3, ISP 4, AUDIO 5, GNSS 6, HDMI 7); only **GPU** is implemented.
* Controls the GPU domain through APMU ``+0xD0`` (control) and APMU ``+0xF0``
  bit 0 (status):

  * **Power on**: set bit 2 (``SLEEP1``), wait 20 µs; set bit 3 (``SLEEP2``),
    wait 20 µs; set bit 1 (``ISO`` release); poll status bit 0.
  * **Power off**: perform the same steps in reverse.

* Mainline lacked a K1 power-domain driver; the sequence was derived by
  observing the SoC APMU registers on the vendor kernel while the GPU was
  active (observing the SoC's power controller, not the GPU firmware).

2. ``riscv: dts: spacemit: k1: add the IMG BXE-2-32 GPU node``
--------------------------------------------------------------

Adds the GPU node under ``multimedia-bus`` (whose ``dma-ranges`` map the
GPU's DMA view ``0x80000000 → 0x1_00000000``) and enables it in
``k1-bananapi-f3.dts``:

.. code-block:: dts

   gpu: gpu@cac00000 {
       compatible = "spacemit,k1-gpu", "img,img-bxe-2-32", "img,img-rogue";
       reg = <0x0 0xcac00000 0x0 0x80000>;
       interrupts = <75>;
       clocks = <&syscon_apmu CLK_GPU>;
       clock-names = "core";
       resets = <&syscon_apmu RESET_GPU>;
       power-domains = <&syscon_apmu K1_PD_GPU>;
       status = "disabled";
   };

3. ``drm/imagination: add BXE-2-32 (36.29.52.182) as experimental``
-------------------------------------------------------------------

* Adds BVNC ``36.29.52.182`` to the driver's GPU table with
  ``PVR_GPU_EXPERIMENTAL`` (requiring ``exp_hw_support=1`` when loading the
  module).
* Adds the matching ``MODULE_FIRMWARE`` entry for
  ``powervr/rogue_36.29.52.182_v1.fw``.

Building the kernel
===================

Start from a working mainline RISC-V kernel configuration for the K1 board
and enable ``CONFIG_SPACEMIT_K1_APMU_PD=y`` and ``CONFIG_DRM_POWERVR=m``:

.. code-block:: sh

   make -C <linux-tree> ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- LOCALVERSION= \
       -j$(nproc) Image modules dtbs

The resulting kernel release is ``7.4.0-rc1-img``.

Debug boot-dump patch
---------------------

``tools/linux-fw-boot-debug-dump.patch`` (uncommitted in the kernel tree)
makes ``powervr`` dump the ``BOOT_DATA`` page whenever firmware boot times
out; it was used during early ``boot.S`` bring-up.

GRUB entry ``img-gpu``
======================

``tools/deploy.sh kernel`` installs the kernel, modules, DTB and initramfs,
backs up ``/boot/efi/EFI/BOOT/grub.cfg`` to ``grub.cfg.bak-img``, and appends
a non-default boot entry with ``powervr`` blacklisted:

.. code-block:: text

   menuentry "Debian 13 - Linux 7.4.0-rc1-img (GPU clean-room FW, powervr blacklisted)" --id img-gpu
       linux /vmlinuz-7.4.0-rc1-img root=UUID=… … modprobe.blacklist=powervr
       devicetree /spacemit/7.4.0-rc1-img/k1-bananapi-f3.dtb

Because ``powervr`` is blacklisted at boot, a broken firmware image can never
prevent the board from booting; every test begins with a manual:

.. code-block:: sh

   sudo modprobe powervr exp_hw_support=1

Firmware search path and fallback hazard
========================================

The firmware is installed to
``/lib/firmware/updates/powervr/rogue_36.29.52.182_v1.fw``, which the Linux
firmware loader searches before ``/lib/firmware/powervr/``.

.. warning::

   If the file under ``/lib/firmware/updates/powervr/`` is truncated (for
   example after an unclean power cycle following a SoC hang), the kernel
   silently falls back to ``/lib/firmware/powervr/`` (:exp:`E010`). Always
   check the SHA-256 hash and verify that ``dmesg`` prints
   ``FW version v1.0 (build 1 OS)``.
