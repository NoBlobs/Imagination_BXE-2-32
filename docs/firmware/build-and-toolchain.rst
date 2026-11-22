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
   make -C src -B EXP="-DEXP_TA"                    # experimental geometry/fragment build
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
   * - ``EXP_TA``
     - Enable experimental geometry (``src/geom.c``) and fragment
       (``src/frag.c``) kicks (:exp:`E028`–:exp:`E040`)
   * - ``EXP_DUMP_RENDER``
     - Dump host ``GEOM``/``FRAG`` render structures to debug trace
       (:exp:`E027`)
   * - ``EXP_CANARY``
     - With ``EXP_TA``: write canary VAs to unassigned address registers
       (:exp:`E028`)
   * - ``EXP_HS_MODE=n``
     - With ``EXP_TA``: PM handshakes ``0x0250``/``0x0258`` (``0`` none,
       ``1`` after PM writes [default], ``2`` around them [faults at 0];
       :exp:`E032`)
   * - ``EXP_PM_PAUSE``, ``EXP_PM_BCAST``, ``EXP_CLK_ON``, ``EXP_SP32``
     - With ``EXP_TA``: PM load variants tested in :exp:`E032`
   * - ``EXP_NO_PM_FLAGS``, ``EXP_20F8_EDGE``, ``EXP_0388``,
       ``EXP_TOP_FIRST``
     - With ``EXP_TA``: first-load PM flag variants tested in :exp:`E033`
   * - ``EXP_F320``, ``EXP_UNIT_EXTRA``, ``EXP_IDX_AFTER``
     - With ``EXP_TA``: Q16i variants tested in :exp:`E034`
   * - ``EXP_NO_FL_LOAD``
     - With ``EXP_TA``: omit free-list load commands (:exp:`E035`)
   * - ``EXP_SP_LOAD_CMD=0``, ``EXP_SP_LOAD_CMD2=0``,
       ``EXP_GEOM_SP_CMD_A=0``, ``EXP_GEOM_SP_CMD_M=0``
     - With ``EXP_TA``: omit ALIST/MLIST SP load commands (:exp:`E037`)
   * - ``EXP_MEM_FREE_US``
     - With ``EXP_TA``: wait timeout in µs for ``PM_3D_MEM_FREE``
       (default ``3000``; :exp:`E038`)
   * - ``EXP_FLAGS_AFTER_SP``, ``EXP_FRAG_RGN``, ``EXP_FRAG_ISP_RENDER=v``
     - With ``EXP_TA``: fragment/PM sequencing knobs (:exp:`E036`,
       :exp:`E037`)

Trace ID generation
===================

``src/include/sfid.h`` is generated by ``tools/gen_sfids.py`` from the public
Linux header ``pvr_rogue_fwif_sf.h``. Each entry becomes
``#define SFID_<GROUP>_<id> 0x7nngiiii /* nparams: "format" */``, allowing
mainline ``pvr_fw_trace.c`` to decode this firmware's traces without changes.
