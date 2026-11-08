/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Exception page (EBase = 0x9FC02000, KSEG0) and C runtime entry.
 *
 *   +0x000 TLB refill     -> dynamic mapping of the firmware heap
 *   +0x180 general        -> fatal exception report
 *   +0x400 HW0 timer      -> irq_dispatch(0)
 *   +0x500 HW1 BG task    -> irq_dispatch(1)
 *   +0x600 HW2 IRQ task   -> irq_dispatch(2)
 */
#include "layout.h"
#include "regs.h"
#include "cp0asm.h"

	.set	micromips
	.set	noreorder
	.set	noat

	.section .exc, "ax"
	.globl	_exc_base
_exc_base:
	.org	0x000
	b	refill_main
	nop
	.org	0x180
	b	general_main
	nop
	.org	0x200
	li	$26, 8
	b	int_common
	nop
	.org	0x300
	li	$26, 9
	b	int_common
	nop
	.org	0x400
	li	$26, 0
	b	int_common
	nop
	.org	0x500
	li	$26, 1
	b	int_common
	nop
	.org	0x600
	li	$26, 2
	b	int_common
	nop
	.org	0x700
	li	$26, 3
	b	int_common
	nop
	.org	0x800
	li	$26, 4
	b	int_common
	nop
	.org	0x900
	li	$26, 5
	b	int_common
	nop

	.org	0xA00
/*
 * TLB refill (EXL=1, only $26/$27 free).  The TLB is always identity
 * (PFN = VA >> 12); the real 36-bit address goes to the wrapper remap entry
 * of the chosen slot (entry slot for the even page, slot+16 for the odd).
 * Pages without a valid host PTE, or outside the heap window, are sent to
 * the host fault page.
 */
refill_main:
	lui	$27, 0xBFC0
	ori	$27, $27, 0x1000
	sw	$8, (BD_SAVE + 0)($27)
	sw	$9, (BD_SAVE + 4)($27)
	sw	$10, (BD_SAVE + 8)($27)
	sw	$11, (BD_SAVE + 12)($27)
	sw	$12, (BD_SAVE + 16)($27)
	sw	$13, (BD_SAVE + 20)($27)
	sw	$1, (BD_SAVE + 24)($27)

	lw	$8, BD_REFILL_COUNT($27)
	addiu	$8, $8, 1
	sw	$8, BD_REFILL_COUNT($27)
	li	$1, FW_REGS_VA
	sw	$8, CR_DBG_REFILLS($1)

	MFC0	26, 8, 0			/* BadVAddr */
	sw	$26, CR_DBG_LASTVA($1)
	li	$8, ~0x1FFF
	and	$26, $26, $8		/* VA of the even page */

	srl	$8, $26, 24
	li	$9, 0xC0
	bne	$8, $9, 1f
	nop
	li	$9, 0x00FFF000
	and	$8, $26, $9
	srl	$8, $8, 10		/* PTE index * 4 */
	lui	$9, 0xCF00
	addu	$8, $8, $9
	lw	$10, 0($8)		/* PTE even */
	lw	$11, 4($8)		/* PTE odd */
	b	2f
	nop
1:	li	$10, 0
	li	$11, 0
2:
	/* choose slot */
	lw	$8, BD_TLB_NEXT($27)
	li	$9, TLB_NUM_WIRED + 1
	sltu	$12, $8, $9
	beqz	$12, 3f
	nop
	move	$8, $9
3:	li	$9, TLB_NUM_ENTRIES
	sltu	$12, $8, $9
	bnez	$12, 4f
	nop
	li	$8, TLB_NUM_WIRED + 1
4:	addiu	$9, $8, 1
	sw	$9, BD_TLB_NEXT($27)

	li	$1, FW_REGS_VA

	/* even page */
	andi	$12, $10, 2		/* PTE valid */
	bnez	$12, 5f
	nop
	lw	$12, BD_FAULT_HI($27)
	lw	$13, BD_FAULT_COUNT($27)
	addiu	$13, $13, 1
	sw	$13, BD_FAULT_COUNT($27)
	MFC0	13, 8, 0
	sw	$13, BD_LAST_BADVADDR($27)
	b	6f
	nop
5:	srl	$12, $10, 6
	li	$13, 0x00FFFFFF
	and	$12, $12, $13
	sll	$12, $12, 4
6:	sll	$13, $8, REMAP_ENTRY_SHIFT
	or	$13, $13, $26
	ori	$13, $13, REMAP_ENABLE
	sw	$13, CR_MIPS_ADDR_REMAP_RANGE_CONFIG($1)
	sw	$12, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)

	/* odd page */
	andi	$12, $11, 2
	bnez	$12, 7f
	nop
	lw	$12, BD_FAULT_HI($27)
	b	8f
	nop
7:	srl	$12, $11, 6
	li	$13, 0x00FFFFFF
	and	$12, $12, $13
	sll	$12, $12, 4
8:	addiu	$13, $8, 16
	sll	$13, $13, REMAP_ENTRY_SHIFT
	or	$13, $13, $26
	li	$9, 0x1000
	or	$13, $13, $9
	ori	$13, $13, REMAP_ENABLE
	sw	$13, CR_MIPS_ADDR_REMAP_RANGE_CONFIG($1)
	sw	$12, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)
	lw	$12, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)	/* flush posted writes */

	/* identity TLB entry, uncached, dirty, valid, global */
	srl	$12, $26, 12
	sll	$12, $12, 6
	ori	$12, $12, (2 << 3) | 7
	MTC0	12, 2, 0
	addiu	$12, $12, (1 << 6)
	MTC0	12, 3, 0
	MTC0	0, 5, 0
	MTC0	8, 0, 0
	ehb
	tlbwi
	ehb

	lw	$8, (BD_SAVE + 0)($27)
	lw	$9, (BD_SAVE + 4)($27)
	lw	$10, (BD_SAVE + 8)($27)
	lw	$11, (BD_SAVE + 12)($27)
	lw	$12, (BD_SAVE + 16)($27)
	lw	$13, (BD_SAVE + 20)($27)
	lw	$1, (BD_SAVE + 24)($27)
	eret
	nop

/* General exception: dump state into the boot-data page, call C, stop. */
general_main:
	lui	$27, 0xBFC0
	ori	$27, $27, 0x1000
	MFC0	26, 13, 0
	sw	$26, BD_LAST_CAUSE($27)
	MFC0	26, 14, 0
	sw	$26, BD_LAST_EPC($27)
	MFC0	26, 8, 0
	sw	$26, BD_LAST_BADVADDR($27)
	li	$1, FW_REGS_VA
	sw	$26, CR_DBG_EXC_BADVA($1)
	lw	$26, BD_LAST_CAUSE($27)
	sw	$26, CR_DBG_EXC_CAUSE($1)
	lw	$26, BD_LAST_EPC($27)
	sw	$26, CR_DBG_EXC_EPC($1)
	lw	$26, BD_IN_EXCEPTION($27)
	bnez	$26, 9f			/* nested fault: just stop */
	nop
	li	$26, 1
	sw	$26, BD_IN_EXCEPTION($27)
	sw	$29, (BD_EXC_REGS + 29 * 4)($27)
	sw	$31, (BD_EXC_REGS + 31 * 4)($27)
	li	$29, FW_STACK_VA + FWL_STACK_SIZE - 16
	MFC0	26, 12, 0
	li	$27, ~0x7		/* EXL=0, IE=0, ERL=0: refills work in C */
	and	$26, $26, $27
	MTC0	26, 12, 0
	ehb
	la	$25, fatal_exception
	ori	$25, $25, 1
	jalr	$25
	nop
9:	wait
	b	9b
	nop

/* Interrupt entry, $26 = source id. Runs on the wired stack with EXL=1. */
int_common:
	addiu	$29, $29, -96
	sw	$1, 0($29)
	sw	$2, 4($29)
	sw	$3, 8($29)
	sw	$4, 12($29)
	sw	$5, 16($29)
	sw	$6, 20($29)
	sw	$7, 24($29)
	sw	$8, 28($29)
	sw	$9, 32($29)
	sw	$10, 36($29)
	sw	$11, 40($29)
	sw	$12, 44($29)
	sw	$13, 48($29)
	sw	$14, 52($29)
	sw	$15, 56($29)
	sw	$24, 60($29)
	sw	$25, 64($29)
	sw	$31, 68($29)
	mfhi	$8
	sw	$8, 72($29)
	mflo	$8
	sw	$8, 76($29)
	MFC0	8, 14, 0
	sw	$8, 80($29)
	MFC0	8, 12, 0
	sw	$8, 84($29)
	/*
	 * Run the handler with EXL=0 and IE=0: a TLB miss in C then takes the
	 * normal refill vector and EPC stays meaningful.
	 */
	li	$9, ~0x7
	and	$8, $8, $9
	MTC0	8, 12, 0
	ehb
	move	$4, $26
	la	$25, irq_dispatch
	ori	$25, $25, 1
	jalr	$25
	nop
	di	$0
	ehb
	lw	$8, 84($29)
	ori	$8, $8, 0x2		/* EXL back on for ERET */
	li	$9, ~0x1
	and	$8, $8, $9		/* IE restored by ERET clearing EXL */
	lw	$9, 84($29)
	andi	$9, $9, 1
	or	$8, $8, $9
	MTC0	8, 12, 0
	ehb
	lw	$8, 80($29)
	MTC0	8, 14, 0
	lw	$8, 72($29)
	mthi	$8
	lw	$8, 76($29)
	mtlo	$8
	lw	$1, 0($29)
	lw	$2, 4($29)
	lw	$3, 8($29)
	lw	$4, 12($29)
	lw	$5, 16($29)
	lw	$6, 20($29)
	lw	$7, 24($29)
	lw	$8, 28($29)
	lw	$9, 32($29)
	lw	$10, 36($29)
	lw	$11, 40($29)
	lw	$12, 44($29)
	lw	$13, 48($29)
	lw	$14, 52($29)
	lw	$15, 56($29)
	lw	$24, 60($29)
	lw	$25, 64($29)
	lw	$31, 68($29)
	addiu	$29, $29, 96
	ehb
	eret
	nop

/*
 * void tlb_flush_dynamic(void)
 * Invalidate every non-wired TLB entry (distinct KSEG0 VPN, EntryLo = 0)
 * and disable its two remap entries.  Lives in the unmapped exception page
 * so that it never invalidates the page it is executing from.
 */
	.globl	tlb_flush_dynamic
	.type	tlb_flush_dynamic, @function
	.ent	tlb_flush_dynamic
tlb_flush_dynamic:
	di	$12
	ehb
	MFC0	8, 6, 0			/* Wired */
	MFC0	13, 10, 0		/* EntryHi */
	li	$1, FW_REGS_VA
	li	$9, TLB_NUM_ENTRIES
	move	$10, $8
	lui	$14, 0x8000
1:	MTC0	10, 0, 0
	sll	$11, $10, 13
	or	$11, $11, $14
	MTC0	11, 10, 0
	MTC0	0, 2, 0
	MTC0	0, 3, 0
	MTC0	0, 5, 0
	ehb
	tlbwi
	ehb
	sll	$11, $10, REMAP_ENTRY_SHIFT
	sw	$11, CR_MIPS_ADDR_REMAP_RANGE_CONFIG($1)
	sw	$0, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)
	addiu	$11, $10, 16
	sll	$11, $11, REMAP_ENTRY_SHIFT
	sw	$11, CR_MIPS_ADDR_REMAP_RANGE_CONFIG($1)
	sw	$0, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)
	addiu	$10, $10, 1
	bne	$10, $9, 1b
	nop
	lw	$11, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)($1)
	MTC0	13, 10, 0
	lui	$11, 0xBFC0
	ori	$11, $11, 0x1000
	sw	$8, BD_TLB_NEXT($11)
	andi	$12, $12, 1
	beqz	$12, 2f
	nop
	ei
	ehb
2:	jr	$31
	nop
	.end	tlb_flush_dynamic

/* ------------------------------------------------------------------ */
	.text
	.globl	fw_entry
	.ent	fw_entry
fw_entry:
	/* clear .bss */
	la	$8, __bss_start
	la	$9, __bss_end
1:	sltu	$10, $8, $9
	beqz	$10, 2f
	nop
	sw	$0, 0($8)
	addiu	$8, $8, 4
	b	1b
	nop
2:	li	$29, FW_STACK_VA + FWL_STACK_SIZE - 16
	la	$25, fw_main
	ori	$25, $25, 1
	jalr	$25
	nop
3:	wait
	b	3b
	nop
	.end	fw_entry
