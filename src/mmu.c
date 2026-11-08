// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/* GPU MMU cache maintenance and the MIPS dynamic TLB. */
#include "fw.h"
#include "mips.h"

#define MMUCACHE_PT        0x1u
#define MMUCACHE_PD        0x2u
#define MMUCACHE_PC        0x4u
#define MMUCACHE_TLB       0x8u
#define MMUCACHE_PMTLB     0x10u
#define MMUCACHE_INTERRUPT 0x4000000u

#define CR_PM_TLB_INVAL    0x1348 /* no public name (spec) */

void mmu_cache_op(const struct kccb_cmd *cmd)
{
	uint32_t f = cmd->data.mmucache.cache_flags;
	uint32_t inval = 0;
	volatile uint32_t *sync;

	if (f & MMUCACHE_PT)
		inval |= 0x1;
	if (f & MMUCACHE_PD)
		inval |= 0x2;
	if (f & MMUCACHE_PC)
		inval |= 0x4;
	if (f & MMUCACHE_TLB)
		inval |= 0x8;
	if (inval)
		cr_write32(CR_BIF_CTRL_INVAL, inval);
	if (f & MMUCACHE_PMTLB)
		cr_write32(CR_PM_TLB_INVAL, 1);

	/* The host may have changed firmware mappings too. */
	tlb_flush_dynamic();

	sync = fwptr(cmd->data.mmucache.sync_fw_addr);
	if (sync) {
		write_barrier();
		*sync = cmd->data.mmucache.sync_value;
	}
}
