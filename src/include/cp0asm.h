/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * microMIPS MFC0/MTC0 encodings.  The LLVM assembler only accepts these in
 * microMIPS R6 mode, so they are emitted by hand:
 *   POOL32A | rt | rs | 00 | sel | 00011 (MFC0) / 01011 (MTC0) | 111100
 * A 32-bit microMIPS instruction is stored as two halfwords, high first.
 */
#pragma once

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
