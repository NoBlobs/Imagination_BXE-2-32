.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

===================
Build and toolchain
===================

Requirements
============

* ``clang`` and ``ld.lld`` with the ``Mips`` target (tested with LLVM on
  Arch Linux), plus ``llvm-objdump``
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
     - microMIPS disassembly (``llvm-objdump -d --mattr=+micromips``)
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

   --target=mipsel-unknown-elf -march=mips32r2 -mmicromips -mabi=32 -msoft-float
   -std=c23 -O2 -g -ffreestanding -fno-builtin -fno-pic -mno-abicalls -G0
   -mlong-calls -fno-common -fno-stack-protector -fno-jump-tables
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

LLVM microMIPS quirks and workarounds
=====================================

``MFC0`` / ``MTC0`` in microMIPS32r2
------------------------------------

LLVM's assembler rejects microMIPS ``mfc0`` / ``mtc0`` mnemonics unless the
target is R6. Because the instruction encoding is identical in R2 and R6,
``src/include/cp0asm.h`` emits the 32-bit encoding directly:

.. code-block:: text

   word = (rt << 21) | (rs << 16) | (sel << 11) | (op << 6) | 0x3C
          op: MFC0 = 0x03, MTC0 = 0x0B
   emitted as two .hword values, high half first (microMIPS 32-bit order)

``src/include/mips.h`` exposes the same encoding to C as ``mfc0(reg, sel)``
and ``mtc0(reg, sel, v)`` via ``$2``. The bit pattern was verified against
LLVM's ``-mips32r6 -mmicromips`` output.

``ld.lld`` and ``R_MICROMIPS_26_S1``
------------------------------------

``ld.lld`` range-checks microMIPS ``J``/``JAL`` relocations as if they were
PC-relative, causing bogus "relocation out of range" errors for code above
``0x80000000`` even though the emitted region-relative value is correct
(verified in ``build/fw.dis``). ``src/Makefile`` therefore links with
``--noinhibit-exec`` and filters only those specific warning lines via
``tools/ld-wrap.sh`` while preserving all other diagnostics.

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
       default since it hung the board in :exp:`E040`
   * - ``EXP_MEM_FREE_US``
     - Wait timeout in µs for ``PM_3D_MEM_FREE`` after a render (default
       ``3000``; :exp:`E038`)
   * - ``EXP_RESUME=n``
     - What the firmware does after adding pages at an out-of-memory event:
       ``1`` clear ``PM_OUT_OF_MEMORY`` again (default), ``2`` also
       re-issue the TA run register; neither resumes the TA (:exp:`E041`)

The knobs of the PM bring-up (:exp:`E028`–:exp:`E040`: ``EXP_TA``,
``EXP_CANARY``, ``EXP_HS_MODE``, ``EXP_PM_*``, ``EXP_CLK_ON``, ``EXP_SP32``,
``EXP_20F8_EDGE``, ``EXP_0388``, ``EXP_TOP_FIRST``, ``EXP_F320``,
``EXP_UNIT_EXTRA``, ``EXP_IDX_AFTER``, ``EXP_NO_FL_LOAD``, ``EXP_*_SP_CMD*``,
``EXP_FLAGS_AFTER_SP``, ``EXP_FRAG_*``, ``EXP_MTILE_STRIDE``,
``EXP_FRAG_DUMP``) were removed in :exp:`E041`, when the working sequence
became the code. The experiment pages describe what each one did.

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
``#define SFID_<GROUP>_<id> 0x7nngiiii /* nparams: "format" */``, allowing
mainline ``pvr_fw_trace.c`` to decode this firmware's traces without changes.
