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
	/*
	 * The host rewrote page tables in DRAM: flush/invalidate the SLC first so
	 * that the MMU does not walk stale lines (E015), then the MMU caches.
	 */
	if (inval || (f & MMUCACHE_PMTLB))
		slc_flush_inval();
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

/*
 * Access to client GPU memory (kick-compute-and-3d.md "Render-target
 * arrays", answer to Q35): walk the render context's GPU MMU (PC -> PD -> PT,
 * public entry formats in pvr_rogue_mmu_defs.h) to a bus address, then map
 * that page through a scratch entry of the MIPS page table. The scratch page
 * sits in the host's reserved 1 MiB of the heap window, past the firmware
 * data object, where the host allocates nothing.
 */
#define SCRATCH_VA      0xC0FC0000u     /* even page of a TLB pair; the odd one stays empty */
#define PTE_UNCACHED_VG ((2u << 3) | 2u | 1u)   /* C = uncached, V, G; D clear: read */
#define PTE_UNCACHED_DVG ((2u << 3) | 4u | 2u | 1u)

static volatile uint32_t *scratch_pte(void)
{
	return (volatile uint32_t *)(FW_PT_VA + ((SCRATCH_VA & 0x00FFF000u) >> 10));
}

/* Map the 4 KiB bus page holding `pa`; returns its address, NULL if busy. */
static volatile uint8_t *phys_map(uint64_t pa, bool write)
{
	volatile uint32_t *pte = scratch_pte();

	if (*pte)
		return NULL;    /* not ours: never overwrite a host mapping */
	*pte = (uint32_t)((pa >> 12) << 6) | (write ? PTE_UNCACHED_DVG : PTE_UNCACHED_VG);
	write_barrier();
	tlb_flush_dynamic();
	return (volatile uint8_t *)SCRATCH_VA + (pa & 0xFFFu);
}

static void phys_unmap(void)
{
	write_barrier();
	*scratch_pte() = 0;
	write_barrier();
	tlb_flush_dynamic();
}

/* Read a 32- or 64-bit page-table entry (naturally aligned: never crosses the page). */
static bool phys_rd(uint64_t pa, uint64_t *v, bool wide)
{
	volatile uint8_t *p = phys_map(pa, false);

	if (!p)
		return false;
	*v = *(volatile uint32_t *)p;
	if (wide)
		*v |= (uint64_t)*(volatile uint32_t *)(p + 4) << 32;
	phys_unmap();
	return true;
}

/* GPU VA -> bus address in the memory context `memctx`; 0 if not mapped. */
static uint64_t gpu_va_to_pa(fwaddr_t memctx, uint64_t va)
{
	static const uint8_t page_shift[6] = { 12, 14, 16, 18, 20, 21 };
	static const uint8_t pt_base_shift[6] = { 12, 10, 8, 6, 5, 5 };
	const struct fwmemcontext *mc = fwptr(memctx);
	uint64_t pc, pd, pt, pte, e;
	uint32_t size;

	if (!mc)
		return 0;
	pc = mc->pc_dev_paddr & 0xFFFFFFF000ull;
	if (!pc || !phys_rd(pc + 4 * ((va >> 30) & 0x3FF), &e, false) || !(e & 1))
		return 0;
	pd = (e >> 4) << 12;
	if (!phys_rd(pd + 8 * ((va >> 21) & 0x1FF), &e, true) || !(e & 1))
		return 0;
	size = (uint32_t)(e >> 1) & 7;
	if (size > 5)
		return 0;
	pt = e & 0xFFFFFFFFFFull & ~((1ull << pt_base_shift[size]) - 1);
	if (!phys_rd(pt + 8 * ((va & 0x1FFFFFull) >> page_shift[size]), &pte, true) || !(pte & 1))
		return 0;
	return (pte & 0xFFFFFFFFFFull & ~((1ull << page_shift[size]) - 1)) |
	       (va & ((1ull << page_shift[size]) - 1));
}

/* Copy (buf != NULL) or zero (buf == NULL, write) `len` bytes at GPU VA `va`. */
static bool gpu_access(fwaddr_t memctx, uint64_t va, uint8_t *buf, uint32_t len, bool write)
{
	while (len) {
		uint32_t n = 0x1000u - (uint32_t)(va & 0xFFFu);
		uint64_t pa = gpu_va_to_pa(memctx, va);
		volatile uint8_t *p;

		if (n > len)
			n = len;
		if (!pa || !(p = phys_map(pa, write)))
			return false;
		for (uint32_t i = 0; i < n; i++) {
			if (write)
				p[i] = 0;
			else
				buf[i] = p[i];
		}
		phys_unmap();
		va += n;
		len -= n;
		if (buf)
			buf += n;
	}
	return true;
}

bool gpu_read(fwaddr_t memctx, uint64_t va, void *buf, uint32_t len)
{
	return gpu_access(memctx, va, buf, len, false);
}

bool gpu_zero(fwaddr_t memctx, uint64_t va, uint32_t len)
{
	return gpu_access(memctx, va, nullptr, len, true);
}
