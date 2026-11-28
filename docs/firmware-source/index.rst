.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=================
Firmware (Source)
=================

Reference of the firmware source, generated from the documentation comments
in ``src/``: every file's purpose, and every function's behaviour, arguments
and return value.
The comments are written in the Google style (``Args:`` / ``Returns:``) and
extracted with `Hawkmoth <https://jnikula.github.io/hawkmoth/>`_, which
parses the sources with libclang using the firmware's own flags
(freestanding GNU C23 for ``mipsel``). Hawkmoth only reads the code;
the firmware itself is built with GCC.

The two assembly files, ``boot.S`` and ``vectors.S``, cannot be parsed; their
comments are extracted by a small directive in ``conf.py`` and shown on
the page :doc:`asm`.

For how the pieces fit together, see :doc:`/firmware/index`.

.. toctree::
   :maxdepth: 1
   :caption: Sources

   src-main
   src-kccb
   src-sched
   src-kick
   src-geom
   src-frag
   src-pm
   src-mmu
   src-power
   src-hwr
   src-stats
   src-gpu_init
   src-util
   asm

.. toctree::
   :maxdepth: 1
   :caption: Headers

   inc-fw
   inc-fwif
   inc-kick
   inc-pm
   inc-layout
   inc-regs
   inc-mips
   inc-sfid
