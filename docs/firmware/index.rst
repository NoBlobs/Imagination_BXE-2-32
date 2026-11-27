.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

========
Firmware
========

Implementation of the free PowerVR Rogue **BXE-2-32** GPU firmware in
``src/``. It is written in freestanding GNU C23 (``-std=c23``, clang) and
microMIPS32r2 assembly, linked into a four-segment ELF32 image and packed
into ``build/rogue_36.29.52.182_v1.fw`` by ``tools/mkfw.py``.

These pages document the source layout, interrupt-driven runtime, toolchain
and compile-time experiment knobs, boot and TLB refill path, client CCB
scheduler and data-master kicks, power/HWR/statistics modules, and the
current bring-up status and experiment log.

.. toctree::
   :maxdepth: 1

   architecture
   build-and-toolchain
   boot-and-runtime
   scheduler-and-kicks
   power-hwr-and-stats
   test-harness
   status-and-experiments
