/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/* MIPS32r2 CP0 helpers. */
#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>

#include "cp0asm.h"

/* CP0 access through $2 (v0), see cp0asm.h for the encoding. */
#define mfc0(reg, sel) ({ register uint32_t __v __asm__("$2"); \
	__asm__ volatile(".hword %1, %2" : "=r"(__v) \
			 : "i"(__CP0_WORD(0x03, 2, reg, sel) >> 16), \
			   "i"(__CP0_WORD(0x03, 2, reg, sel) & 0xFFFF)); __v; })
#define mtc0(reg, sel, val) do { register uint32_t __v __asm__("$2") = (uint32_t)(val); \
	__asm__ volatile(".hword %1, %2\n\tehb" :: "r"(__v), \
			 "i"(__CP0_WORD(0x0B, 2, reg, sel) >> 16), \
			 "i"(__CP0_WORD(0x0B, 2, reg, sel) & 0xFFFF) : "memory"); } while (0)

#define C0_INDEX    0
#define C0_ENTRYLO0 2
#define C0_ENTRYLO1 3
#define C0_PAGEMASK 5
#define C0_WIRED    6
#define C0_BADVADDR 8
#define C0_COUNT    9
#define C0_ENTRYHI  10
#define C0_COMPARE  11
#define C0_STATUS   12
#define C0_CAUSE    13
#define C0_EPC      14

static inline uint32_t irq_save(void)
{
	uint32_t s;

	__asm__ volatile("di %0\n\tehb" : "=r"(s) :: "memory");
	return s;
}

static inline void irq_restore(uint32_t s)
{
	if (s & 1)
		__asm__ volatile("ei\n\tehb" ::: "memory");
}

static inline void mips_sync(void) { __asm__ volatile("sync" ::: "memory"); }
static inline void mips_ehb(void) { __asm__ volatile("ehb" ::: "memory"); }
static inline void mips_wait(void) { __asm__ volatile("wait" ::: "memory"); }
#endif
