/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * MIPS32r2 CP0 access, interrupt masking and barrier helpers for C.
 */
/**/
#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>

#include "cp0asm.h"

/**
 * Read a CP0 register.
 *
 * The instruction is emitted by hand through ``$2`` (see :c:macro:`__CP0_WORD`
 * for the encoding).
 *
 * Args:
 *     reg: CP0 register number (``C0_*``).
 *     sel: Select field.
 *
 * Returns:
 *     The register value (``uint32_t``).
 */
#define mfc0(reg, sel) ({ register uint32_t __v __asm__("$2"); \
	__asm__ volatile(".hword %1, %2" : "=r"(__v) \
			 : "i"(__CP0_WORD(0x03, 2, reg, sel) >> 16), \
			   "i"(__CP0_WORD(0x03, 2, reg, sel) & 0xFFFF)); __v; })
/**
 * Write a CP0 register, followed by ``EHB``.
 *
 * Args:
 *     reg: CP0 register number (``C0_*``).
 *     sel: Select field.
 *     val: The value.
 */
#define mtc0(reg, sel, val) do { register uint32_t __v __asm__("$2") = (uint32_t)(val); \
	__asm__ volatile(".hword %1, %2\n\tehb" :: "r"(__v), \
			 "i"(__CP0_WORD(0x0B, 2, reg, sel) >> 16), \
			 "i"(__CP0_WORD(0x0B, 2, reg, sel) & 0xFFFF) : "memory"); } while (0)

/** CP0 ``Index``. */
#define C0_INDEX    0
/** CP0 ``EntryLo0``. */
#define C0_ENTRYLO0 2
/** CP0 ``EntryLo1``. */
#define C0_ENTRYLO1 3
/** CP0 ``PageMask``. */
#define C0_PAGEMASK 5
/** CP0 ``Wired``. */
#define C0_WIRED    6
/** CP0 ``BadVAddr``. */
#define C0_BADVADDR 8
/** CP0 ``Count`` (core clock / 2). */
#define C0_COUNT    9
/** CP0 ``EntryHi``. */
#define C0_ENTRYHI  10
/** CP0 ``Compare`` (timer interrupt). */
#define C0_COMPARE  11
/** CP0 ``Status``. */
#define C0_STATUS   12
/** CP0 ``Cause``. */
#define C0_CAUSE    13
/** CP0 ``EPC``. */
#define C0_EPC      14

/**
 * Mask interrupts.
 *
 * Returns:
 *     The previous ``Status``; pass it to :c:func:`irq_restore`.
 */
static inline uint32_t irq_save(void)
{
	uint32_t s;

	__asm__ volatile("di %0\n\tehb" : "=r"(s) :: "memory");
	return s;
}

/**
 * Unmask interrupts if they were enabled before :c:func:`irq_save`.
 *
 * Args:
 *     s: The value :c:func:`irq_save` returned.
 */
static inline void irq_restore(uint32_t s)
{
	if (s & 1)
		__asm__ volatile("ei\n\tehb" ::: "memory");
}

/** ``SYNC``: order memory accesses. */
static inline void mips_sync(void) { __asm__ volatile("sync" ::: "memory"); }
/** ``EHB``: clear execution hazards (after CP0 writes). */
static inline void mips_ehb(void) { __asm__ volatile("ehb" ::: "memory"); }
/** ``WAIT``: sleep until an interrupt. */
static inline void mips_wait(void) { __asm__ volatile("wait" ::: "memory"); }
#endif
