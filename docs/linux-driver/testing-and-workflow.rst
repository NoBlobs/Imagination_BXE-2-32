.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

====================
Testing and workflow
====================

Host and board scripts in ``tools/`` automate firmware deployment, kernel
installation, serial GRUB selection, safe register inspection and dEQP-VK
regression runs.

Scripts in ``tools/``
=====================

.. list-table::
   :header-rows: 1
   :widths: 26 10 64

   * - Script
     - Runs on
     - Purpose
   * - ``deploy.sh fw``
     - Host
     - Build ``rogue_36.29.52.182_v1.fw``, copy to
       ``/lib/firmware/updates/powervr/``, ``sync``, print SHA-256
   * - ``deploy.sh kernel``
     - Host
     - Build and install the kernel, modules, DTB and initramfs, append the
       ``img-gpu`` GRUB entry
   * - ``boot-entry.py``
     - Host
     - Reboot over SSH, select a GRUB menu index over the serial console with
       inter-key delays, and wait for ``login:``
   * - ``gpu-test.sh``
     - Board
     - ``modprobe powervr exp_hw_support=1``, dump ``dmesg`` and the decoded
       firmware trace from debugfs
   * - ``gpu-probe.sh``
     - Board
     - Like ``gpu-test.sh``, plus register sampling during load; **unloads
       the driver immediately if any firmware other than this build was
       loaded**
   * - ``gpu-regs.py <s> [step]``
     - Board
     - Sample selected registers through ``/dev/mem`` only while
       ``runtime_status == active`` after forcing ``power/control = on``
       (``EXTRA_REGS="0610 1a30"``, ``ONLY_EXTRA=1``)
   * - ``reg-survey.py read|probe <start> <end>``
     - Board
     - Black-box register survey: dump non-zero values (:exp:`E007`) or
       probe writable-bit masks with restore and deny list (:exp:`E008`)
   * - ``deqp.sh <pattern> [timeout] [lines]``
     - Board
     - Run ``deqp-vk`` cases against Mesa ``pvr`` and print pass/fail counts
   * - ``cab-run.sh``
     - Board
     - Run ``api.copy_and_blit.core`` subgroups sequentially (summary in
       ``/tmp/cab/summary.txt``)
   * - ``ta-sample.sh``
     - Board
     - Install ``/tmp/rogue_…fw``, reload ``powervr``, run a draw case while
       sampling geometry/PM registers (``REGS``, ``SAMPLE``, ``CASE``)
   * - ``stab-run.sh``
     - Board
     - Stability loop: ``REPS`` × compute basic followed by ``CYCLES``
       runtime-PM cycles (summary in ``/tmp/stab/summary.txt``)
   * - ``parse-dump.py``
     - Host
     - Parse render register dumps from firmware debug trace
   * - ``mkfw.py``, ``gen_sfids.py``, ``ld-wrap.sh``
     - Host
     - Build helpers (:doc:`../firmware/build-and-toolchain`)

SSH note: if the board alternates between different OS images (for example,
mainline on SD card and vendor kernel on eMMC) with different SSH host keys,
pass a dedicated ``known_hosts`` file via ``KH=<known_hosts>``
(``-o UserKnownHostsFile=$KH``). ``tools/deploy.sh`` reads ``KH`` from the
environment.

Booting the ``img-gpu`` GRUB entry
==================================

Select the 0-based GRUB menu index of the ``img-gpu`` entry over the board's
serial console:

.. code-block:: sh

   python3 tools/boot-entry.py <serial-device> <grub-index> <user>@<board-ip> \
       -o UserKnownHostsFile=$KH

Typical edit → deploy → test cycle
==================================

.. code-block:: sh

   make -C src
   KH=<known_hosts> tools/deploy.sh fw
   scp tools/*.sh tools/*.py <user>@<board-ip>:/tmp/
   ssh <user>@<board-ip> 'sudo modprobe -r powervr; bash /tmp/gpu-test.sh'
   ssh <user>@<board-ip> 'bash /tmp/deqp.sh "dEQP-VK.compute.pipeline.basic.empty_shader" 60'

Expected kernel log on a healthy load:

.. code-block:: text

   powervr cac00000.gpu: [drm] FW version v1.0 (build 1 OS)
   [drm] Initialized powervr 1.0.0 for cac00000.gpu on minor 0
