/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * microMIPS ``MFC0`` / ``MTC0`` encodings, for C and assembly.
 *
 * The LLVM assembler only accepts these instructions in microMIPS R6 mode,
 * so they are emitted by hand. A 32-bit microMIPS instruction is stored as
 * two halfwords, high first. Assembly uses the ``MFC0 rt, rs, sel`` and
 * ``MTC0 rt, rs, sel`` macros defined here; C uses ``mfc0()`` and ``mtc0()``
 * from ``mips.h``.
 */
/**/
#pragma once

/**
 * Encode a microMIPS ``MFC0`` / ``MTC0`` instruction:
 * ``POOL32A | rt | rs | 00 | sel | op | 111100``.
 *
 * Args:
 *     op: ``0x03`` for ``MFC0``, ``0x0B`` for ``MTC0``.
 *     rt: General-purpose register.
 *     rs: CP0 register.
 *     sel: Select field.
 *
 * Returns:
 *     The 32-bit instruction word.
 */
#define __CP0_WORD(op, rt, rs, sel) \
	(((rt) << 21) | ((rs) << 16) | ((sel) << 11) | ((op) << 6) | 0x3C)

#ifdef __ASSEMBLER__
	.macro	MFC0 rt, rs, sel=0
	.hword	(__CP0_WORD(0x03, \rt, \rs, \sel) >> 16), (__CP0_WORD(0x03, \rt, \rs, \sel) & 0xFFFF)
	.endm
	.macro	MTC0 rt, rs, sel=0
	.hword	(__CP0_WORD(0x0B, \rt, \rs, \sel) >> 16), (__CP0_WORD(0x0B, \rt, \rs, \sel) & 0xFFFF)
	.endm
#endif
