.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

===================
Build and toolchain
===================

Requirements
============

* The GNU toolchain for ``mipsel-elf``: GCC and binutils. The default is
  GCC 16.2.0 with binutils 2.42 in ``/opt/rgxcross``; another one is
  selected with ``make CROSS=<prefix> GCC=<gcc>``. C23 needs GCC 14 or
  later.
* ``python3`` (``tools/mkfw.py``, ``tools/gen_sfids.py``)
* No libc or libgcc: the firmware is freestanding (``memset``, ``memcpy``,
  ``__udivdi3`` and ``__umoddi3`` live in ``src/util.c``).

Build commands
==============

.. code-block:: sh

   make -C src                                      # -> build/rogue_36.29.52.182_v1.fw
   make -C src -B EXP="-DEXP_NO_RENDER"             # retire geometry/fragment commands unrun
   make -C src sfid SFH=<path>/pvr_rogue_fwif_sf.h  # regenerate src/include/sfid.h
   make -C src clean
   make -C src CROSS=/usr/bin/mipsel-elf- GCC=/usr/bin/mipsel-elf-gcc
                                                    # another GCC (14 or later)

Outputs in ``build/`` (git-ignored):

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Artifact
     - Description
   * - ``build/obj/*.o``
     - Compiled object files
   * - ``build/fw.elf``
     - Linked firmware ELF32 (four ``PT_LOAD`` segments)
   * - ``build/fw.dis``
     - microMIPS disassembly (``mipsel-elf-objdump -d``)
   * - ``build/rogue_36.29.52.182_v1.fw``
     - Packed firmware container loaded by ``powervr.ko``

The build is deterministic: identical sources produce a byte-identical
``.fw`` file.

.. note::

   Always pass ``-B`` when changing ``EXP=…``, because ``make`` does not
   track macro changes. Verify that the build succeeded and that the ``.fw``
   SHA-256 changed before deploying — a failed build leaves the previous
   ``.fw`` on disk (:exp:`E023`).

Compiler flags
==============

.. code-block:: text

   -EL -march=mips32r2 -mmicromips -mabi=32 -msoft-float
   -std=gnu23 -O2 -g -ffreestanding -fno-builtin -fno-pic -mno-abicalls -G0
   -mlong-calls -fno-common -fno-stack-protector -fno-jump-tables
   -fno-delete-null-pointer-checks -mno-check-zero-division
   -Wall -Wextra -Werror -Wno-unused-parameter

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Flag
     - Reason
   * - ``-mmicromips``
     - Target ISA of the MIPS wrapper core; ``Config3.ISAOnExc = 1`` enters
       exceptions in microMIPS mode, and function pointers carry bit 0
       (ISA bit).
   * - ``-mlong-calls``
     - Code spans three distant regions (``0x9FC02000``, ``0xBFC00000``,
       ``0xC0000000``), so cross-region calls must go through ``jalr``.
   * - ``-G0 -fno-pic -mno-abicalls``
     - Disables ``$gp``-relative small data and PIC; the image links at
       fixed virtual addresses.
   * - ``-fno-jump-tables``
     - Prevents ``switch`` statements from emitting absolute-address tables
       in ``.rodata``.
   * - ``-msoft-float``
     - The microcontroller has no FPU.
   * - ``-std=gnu23``
     - GNU C23: ISO C23 (``nullptr``, ``constexpr``, ``static_assert``,
       ``[[attributes]]``, ``typeof``, ``__STDC_VERSION__ == 202311L``) plus
       the GNU extensions the code uses (statement expressions, ``__asm__``,
       ``register`` variables).
   * - ``-fno-delete-null-pointer-checks``
     - Without it, GCC at ``-O2`` assumes a null pointer is never
       dereferenced and turns such paths into a ``teq`` trap
       (``-fisolate-erroneous-paths-dereference``). In this firmware a trap is
       a fatal exception, while address 0 merely faults through the TLB.
   * - ``-mno-check-zero-division``
     - Without it, GCC follows every division with a ``teq`` trap on a zero
       divisor. Every division in the firmware is already guarded.

The GNU toolchain since November 2026
=====================================

Until November 2026 the firmware was built with clang and ``ld.lld``. Moving
to GCC and binutils removed two LLVM workarounds:

* LLVM's assembler accepts microMIPS ``mfc0`` / ``mtc0`` only for R6, so
  ``cp0asm.h`` encoded them by hand as ``.hword`` pairs. GNU ``as`` accepts
  them, and ``boot.S``, ``vectors.S`` and ``mips.h`` now use the real
  instructions (the encodings are identical).
* ``ld.lld`` range-checked microMIPS ``J``/``JAL`` relocations above
  ``0x80000000`` as if they were PC-relative, so the link needed
  ``--noinhibit-exec`` and ``tools/ld-wrap.sh`` to filter the false errors.
  GNU ``ld`` links without warnings.

The code GCC 16 generates is about 25 % smaller (``.text`` 27 KiB instead of
36 KiB).

Compile-time experiment knobs (``EXP=``)
========================================

Passed via ``make -C src -B EXP="-DNAME[=value]"``:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Knob
     - Effect
   * - ``EXP_NO_EVENT_POLL``
     - Disable timer polling of ``EVENT_STATUS`` so completions come only
       from MTS IRQ tasks (:exp:`E018`)
   * - ``EXP_SLC_FLUSH_PER_KICK``
     - Flush and invalidate SLC before every ``TQ_3D`` kick (:exp:`E020`)
   * - ``EXP_DM_FLUSH_PER_KICK``
     - Run the full DM 3 cache flush before every ``TQ_3D`` kick
       (:exp:`E026`)
   * - ``EXP_TIF=<n>``
     - Override the TQ tiles-in-flight register value (:exp:`E022`)
   * - ``EXP_SLC_ONLY_FLUSH``
     - Use SLC-only completion flush instead of per-DM flush (:exp:`E023`)
   * - ``EXP_NO_RENDER``
     - Retire ``GEOM``/``FRAG``/``FRAG_PR`` commands without running them
       and ignore ``PM_OUT_OF_MEMORY`` (the behaviour before :exp:`E041`;
       rendering is on by default since then)
   * - ``EXP_DUMP_RENDER``
     - Dump host ``GEOM``/``FRAG`` render structures to debug trace
       (:exp:`E027`)
   * - ``EXP_SHARED_PB``
     - Shared free lists between the PM contexts (``CONTEXT_PB_BASE = 0``,
       no reload) when PM context 1 holds the same free lists; off by
       default since it hung the board in :exp:`E040` (it no longer does
       in :exp:`E042`, but does not fix Q29b either)
   * - ``EXP_PR_FULL_INVAL``
     - Full PM-MMU invalidation (SLC, BIF, PM TLB) in the restart after a
       partial render, as the spec reads; it hung the board in
       :exp:`E043`, so the default writes ``0x1348`` only (Q30)
   * - ``EXP_MEM_FREE_US``
     - Wait timeout in µs for ``PM_3D_MEM_FREE`` after a render (default
       ``3000``; :exp:`E038`)

The knobs of the PM bring-up (:exp:`E028`–:exp:`E040`: ``EXP_TA``,
``EXP_CANARY``, ``EXP_HS_MODE``, ``EXP_PM_*``, ``EXP_CLK_ON``, ``EXP_SP32``,
``EXP_20F8_EDGE``, ``EXP_0388``, ``EXP_RESUME`` (:exp:`E042`), ``EXP_TOP_FIRST``, ``EXP_F320``,
``EXP_UNIT_EXTRA``, ``EXP_IDX_AFTER``, ``EXP_NO_FL_LOAD``, ``EXP_*_SP_CMD*``,
``EXP_FLAGS_AFTER_SP``, ``EXP_FRAG_*``, ``EXP_MTILE_STRIDE``,
``EXP_FRAG_DUMP``) were removed in :exp:`E041`, when the working sequence
became the code. The experiment pages describe what each one did.

Test harness
============

``tools/deploy.sh vkfwtest`` builds the ``vkfwtest`` Vulkan harness (repository
``../vkfwtest``) on the board; see :doc:`test-harness`.

Build-time checks
=================

* ``tools/check-trace-args.py`` runs before every link: each
  ``TRACE(SFID_x, …)`` must pass the argument count of its format string
  (the ``cr_poll32`` trace bug of :exp:`E037`).
* ``tools/fw-preflight.py`` checks the ``.fw`` container
  (``board-testing-oracle`` section 2); ``tools/deploy.sh fw`` refuses to
  copy a file that fails it.

Trace ID generation
===================

``src/include/sfid.h`` is generated by ``tools/gen_sfids.py`` from the public
Linux header ``pvr_rogue_fwif_sf.h``. Each entry becomes
``#define SFID_<GROUP>_<id> 0x7nngiiii``, under a documentation comment that
holds its format string and parameter count, allowing
mainline ``pvr_fw_trace.c`` to decode this firmware's traces without changes.
