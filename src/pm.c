// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Parameter Manager: free-list and list loads, PM context bookkeeping, the
 * cross-context free-list synchronisation, the out-of-memory handling (SPM)
 * and the free-list reconstruction after a hardware recovery.
 *
 * Sources:
 *
 * - ``docsClean`` ``kick-geometry-ta.md`` phase B (answers to Q16b-Q16m):
 *   free-list banks, live stack pointers, OOM and partial-render register
 *   changes;
 * - ``docsClean`` ``partial-render-spm.md``: SPM decisions, states, fidelity
 *   notes;
 * - E035 and E037: the free-list and list stack-pointer load commands;
 * - mainline ``pvr_free_list.c``: the host side of grow and reconstruction.
 */
/**/
#include "kick.h"
#include "pm.h"

/* ------------------------------------------------------------ registers */
/**
 * ALIST stack-pointer load registers per PM context (``kick-geometry-ta.md``
 * B step 5); the live values are at ``0x0270`` / ``0x0280``.
 */
static const uint16_t alist_sp_load[2] = { 0x0268, 0x0278 };   /* live 0x0270 / 0x0280 */
/** MLIST stack-pointer load registers per PM context (live at ``0x02C0`` / ``0x02D0``). */
static const uint16_t mlist_sp_load[2] = { 0x02B8, 0x02C8 };   /* live 0x02C0 / 0x02D0 */
#define CR_PM_ALIST_SP_CMD       0x0288   /* E037: load commands, write 1, wait bit 0 = 0 */
#define CR_PM_MLIST_SP_CMD       0x0190
#define CR_PM_CTX_SEL_A          0x03D0   /* ctx0: bit 8, ctx1: bit 16 (Q16h) */
#define CR_PM_CTX_SEL_B          0x03D4   /* ctx0: bit 0, ctx1: bit 8 */
#define CR_PM_PAGE_MANAGEOP      0x02A0   /* 1 = pause TA allocation */
#define CR_PM_PAGE_MANAGEOP_ACK  0x02A8
#define CR_PM_TLB_INVAL          0x1348   /* PM TLB invalidate, self-clearing */
#define FREELIST_BASE_ALIGN      16u      /* ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE */

/** The registers of one free-list bank of a PM context. */
struct fl_regs {
	/** Base-address load register. */
	uint16_t base;
	/** Stack-top load register (64-bit). */
	uint16_t top;
	/** Page-count load registers (pages, then MMU pages at ``+4``). */
	uint16_t cnt;
	/** Load command: write 1, wait until bit 0 clears. */
	uint16_t cmd;
	/** Live stack pointer. */
	uint16_t live_sp;
	/** Live allocated page count. */
	uint16_t live_pb;
	/** Live allocated MMU page count. */
	uint16_t live_mmu;
	/** Position of the stack top in the 64-bit top register. */
	uint8_t top_shift;
};

/**
 * Free-list bank registers, indexed ``[PM context][local or global]`` (E035,
 * answers to Q16b-Q16k).
 */
static const struct fl_regs flr[2][2] = {
	[PM_CTX_GEOM] = {
		[PM_FL_LOCAL]  = { 0x0210, 0x0220, 0x0390, 0x0208, 0x0348, 0x03A0, 0x2000, 32 },
		[PM_FL_GLOBAL] = { 0x20B8, 0x20C0, 0x20D0, 0x20B0, 0x20C8, 0x20D8, 0x20E0, 0 },
	},
	[PM_CTX_FRAG] = {
		[PM_FL_LOCAL]  = { 0x0218, 0x0230, 0x0398, 0x0200, 0x0350, 0x03A8, 0x2008, 22 },
		[PM_FL_GLOBAL] = { 0x2078, 0x2080, 0x2090, 0x20A8, 0x2088, 0x2098, 0x20A0, 0 },
	},
};

/** MLIST base register per PM context. */
static const uint16_t mlist_base[2] = { 0x02D8, 0x02E0 };
/** VCE, TE and ALIST catalogue-base registers per PM context. */
static const uint16_t cat_base[2][3] = {
	{ CR_BIF_PM_CAT_BASE_VCE0, CR_BIF_PM_CAT_BASE_TE0, CR_BIF_PM_CAT_BASE_ALIST0 },
	{ CR_BIF_PM_CAT_BASE_VCE1, CR_BIF_PM_CAT_BASE_TE1, CR_BIF_PM_CAT_BASE_ALIST1 },
};

/** What each PM context holds (see :c:struct:`pm_slot`). */
struct pm_slot pm_slots[2];

/**
 * Run a PM load command: write 1 and wait (10 ms at most) until bit 0 clears.
 *
 * Args:
 *     reg: The command register.
 *
 * Returns:
 *     ``true`` when the command completed.
 */
static bool pm_cmd(uint32_t reg)
{
	cr_write32(reg, 1);
	return cr_poll32(reg, 1, 0, 10000);
}

/**
 * Select the PM context that the following loads go to (Q16h, E037).
 *
 * Args:
 *     ctx: ``PM_CTX_GEOM`` or ``PM_CTX_FRAG``.
 */
void pm_select(uint32_t ctx)
{
	uint32_t a = cr_read32(CR_PM_CTX_SEL_A), b = cr_read32(CR_PM_CTX_SEL_B);

	if (ctx == PM_CTX_GEOM) {
		a = (a & ~0x10000u) | 0x100u;
		b = (b & ~0x100u) | 0x1u;
	} else {
		a |= 0x10000u;
		b |= 0x100u;
	}
	cr_write32(CR_PM_CTX_SEL_A, a);
	cr_write32(CR_PM_CTX_SEL_B, b);
}

/**
 * Load a free list into a PM context.
 *
 * Writes the base, stack top and page counts, then runs the bank's load
 * command.
 *
 * Args:
 *     ctx: The PM context.
 *     which: ``PM_FL_LOCAL`` or ``PM_FL_GLOBAL``.
 *     fl: The free list.
 *
 * Returns:
 *     ``true`` when the load command completed.
 */
bool pm_load_freelist(uint32_t ctx, uint32_t which, const struct pm_freelist *fl)
{
	const struct fl_regs *r = &flr[ctx][which];

	cr_write64(r->base, fl->current_dev_addr);
	cr_write64(r->top, (uint64_t)fl->current_stack_top << r->top_shift);
	cr_write32(r->cnt, fl->allocated_page_count);
	cr_write32(r->cnt + 4, fl->allocated_mmu_page_count);
	TRACE(SFID_RTD_6, (uint32_t)(uintptr_t)fl, which, ctx ? DM_FRAG : DM_GEOM, fl->current_pages,
	      (uint32_t)(fl->current_dev_addr >> 32), (uint32_t)fl->current_dev_addr, 0u,
	      fl->current_stack_top, fl->allocated_page_count, fl->allocated_mmu_page_count);
	return pm_cmd(r->cmd);
}

/**
 * Copy a PM context's live free-list state back into the free list.
 *
 * Args:
 *     ctx: The PM context.
 *     which: ``PM_FL_LOCAL`` or ``PM_FL_GLOBAL``.
 *     fl: The free list that receives the stack top and page counts.
 */
void pm_store_freelist(uint32_t ctx, uint32_t which, struct pm_freelist *fl)
{
	const struct fl_regs *r = &flr[ctx][which];

	fl->current_stack_top = cr_read32(r->live_sp);
	fl->allocated_page_count = cr_read32(r->live_pb);
	fl->allocated_mmu_page_count = cr_read32(r->live_mmu);
	TRACE(SFID_RTD_13, (uint32_t)(uintptr_t)fl, which, ctx ? DM_FRAG : DM_GEOM, fl->current_pages,
	      (uint32_t)(fl->current_dev_addr >> 32), (uint32_t)fl->current_dev_addr, 0u,
	      fl->current_stack_top, fl->allocated_page_count, fl->allocated_mmu_page_count);
}

/**
 * Load the MLIST base and the catalogues of a PM context, without the
 * stack-pointer loads (a first geometry kick).
 *
 * Args:
 *     ctx: The PM context.
 *     mlist: MLIST device address.
 *     vce: VCE catalogue base.
 *     te: TE catalogue base.
 *     alist: ALIST catalogue base.
 */
void pm_load_bases(uint32_t ctx, uint64_t mlist, uint64_t vce, uint64_t te, uint64_t alist)
{
	cr_write64(mlist_base[ctx], mlist);
	cr_write64(cat_base[ctx][0], vce);
	cr_write64(cat_base[ctx][1], te);
	cr_write64(cat_base[ctx][2], alist);
}

/**
 * Load the MLIST base, the catalogues and the ALIST/MLIST stack pointers of
 * a PM context (every load except a first geometry kick).
 *
 * Args:
 *     ctx: The PM context.
 *     mlist: MLIST device address.
 *     vce: VCE catalogue base.
 *     te: TE catalogue base.
 *     alist: ALIST catalogue base.
 *     alist_sp: ALIST stack pointer.
 *     mlist_sp: MLIST stack pointer.
 */
void pm_load_lists(uint32_t ctx, uint64_t mlist, uint64_t vce, uint64_t te, uint64_t alist,
		   uint64_t alist_sp, uint32_t mlist_sp)
{
	pm_load_bases(ctx, mlist, vce, te, alist);
	cr_write64(alist_sp_load[ctx], alist_sp);
	cr_write32(mlist_sp_load[ctx], mlist_sp);
	pm_cmd(CR_PM_MLIST_SP_CMD);
	pm_cmd(CR_PM_ALIST_SP_CMD);
}

/**
 * Invalidate the PM's MMU and TLB.
 *
 * Needed at every first geometry kick, every HWRTData switch in a PM context
 * and every 3D finished (``kick-geometry-ta.md`` B step 3). Without it the PM
 * keeps catalogue and TLB entries of the previous HWRTData, and the second
 * HWRTData of a dataset stalls or faults (E041, E042). Invalidates the PM
 * lines of the SLC, the BIF page-catalogue cache and the PM TLB.
 */
void pm_mmu_inval(void)
{
	bcast_write32(CR_SLC_CTRL_FLUSH_INVAL, 0x10);
	cr_poll32(CR_SLC_STATUS0, SLC_STATUS_FLUSH_PENDING, 0, 10000);
	bcast_write32(CR_BIF_CTRL_INVAL, 4);
	cr_poll32(CR_BIF_CTRL_INVAL, 4, 0, 10000);
	bcast_write32(CR_PM_TLB_INVAL, 1);
	if (!cr_poll32(CR_PM_TLB_INVAL, 1, 0, 10000))
		TRACE(SFID_DBG_2, 0xE0431348u);
}

/**
 * Store the free-list state a PM context holds before another HWRTData is
 * loaded into it ("Store Freelist ... for PMDM<n>").
 *
 * Without it, two renders that interleave (geometry A, geometry B,
 * fragment A) leave A's free lists stale, and the PM can hand out pages that
 * are still in use (E041). Not called at present:
 * :c:func:`pm_sync_load_freelists` does this as part of each load.
 *
 * Args:
 *     ctx: The PM context.
 *     next_hwrt: Firmware address of the HWRTData about to be loaded.
 */
void pm_slot_evict(uint32_t ctx, fwaddr_t next_hwrt)
{
	struct pm_slot *sl = &pm_slots[ctx];

	if (!sl->valid || sl->hwrt == next_hwrt)
		return;
	if (sl->dirty) {
		for (uint32_t w = 0; w < 2; w++) {
			struct pm_freelist *fl = fwptr(sl->fl[w]);

			if (fl)
				pm_store_freelist(ctx, w, fl);
		}
		TRACE(SFID_DBG_4, 0xE0410E00u | ctx, sl->hwrt, next_hwrt);
	}
	sl->valid = false;
	sl->dirty = false;
}

/**
 * Record that a PM context's free-list state was stored.
 *
 * Happens at the fragment load for context 0 and at the end of the fragment
 * for context 1. The free-list structures are then the newest copy, and a
 * later eviction must not overwrite them with the stale registers. Without
 * this, the second render of an HWRTData pair hung the TA (E041).
 *
 * Args:
 *     ctx: The PM context.
 */
void pm_slot_stored(uint32_t ctx)
{
	pm_slots[ctx].dirty = false;
}

/**
 * Record that a PM context was loaded from an HWRTData.
 *
 * Its free lists are remembered and its live state is marked newer than the
 * structures.
 *
 * Args:
 *     ctx: The PM context.
 *     hwrt: Firmware address of the HWRTData; 0 empties the slot.
 */
void pm_slot_set(uint32_t ctx, fwaddr_t hwrt)
{
	const void *h = fwptr(hwrt);

	pm_slots[ctx].hwrt = hwrt;
	pm_slots[ctx].fl[0] = h ? hw_rd32(h, HWRT_FREELISTS) : 0;
	pm_slots[ctx].fl[1] = h ? hw_rd32(h, HWRT_FREELISTS + 4) : 0;
	pm_slots[ctx].valid = hwrt != 0;
	pm_slots[ctx].dirty = hwrt != 0;
}

/** Forget what both PM contexts hold (power-off, hardware recovery). */
void pm_invalidate(void)
{
	memset(pm_slots, 0, sizeof(pm_slots));
}

/**
 * Which free lists the two PM contexts share (Q38): bit 1 set when the local
 * lists differ, bit 0 when the global lists differ.
 */
#define CR_PM_CONTEXT_PB_BASE    0x02B0   /* bit 1: local FL differs, bit 0: global (Q38) */
/** ``CR_PM_PAGE_MANAGEOP`` bit that pauses PM context 0 (TA allocation). */
#define PM_PAUSE_TA_ALLOC        1u       /* CR_PM_PAGE_MANAGEOP bit for PM context 0 */
/** ``CR_PM_PAGE_MANAGEOP`` bit that pauses PM context 1 (3D deallocation). */
#define PM_PAUSE_3D_DEALLOC      2u       /* ... for PM context 1 */

/**
 * Check whether a PM context's data master is doing PM work right now.
 *
 * Args:
 *     ctx: The PM context.
 *
 * Returns:
 *     ``true`` while the geometry runs (context 0), or while a 3D render
 *     (not a transfer) runs (context 1).
 */
static bool pm_ctx_running(uint32_t ctx)
{
	if (ctx == PM_CTX_GEOM)
		return dms[DM_GEOM].busy;
	return dms[DM_FRAG].busy && dms[DM_FRAG].hwrt;   /* a 3D render, not a TQ_3D */
}

/**
 * Pause or resume a PM context through ``CR_PM_PAGE_MANAGEOP``.
 *
 * A context is only paused when its data master runs, and the change is
 * waited for in ``CR_PM_PAGE_MANAGEOP_ACK``.
 *
 * Args:
 *     ctx: The PM context.
 *     on: Pause (``true``) or resume.
 *     paused: The pause bits this caller set; updated.
 */
static void pm_pause_ctx(uint32_t ctx, bool on, uint32_t *paused)
{
	uint32_t bit = ctx == PM_CTX_GEOM ? PM_PAUSE_TA_ALLOC : PM_PAUSE_3D_DEALLOC;
	uint32_t v = cr_read32(CR_PM_PAGE_MANAGEOP);

	if (on == !!(*paused & bit) || (on && !pm_ctx_running(ctx)))
		return;
	cr_write32(CR_PM_PAGE_MANAGEOP, on ? v | bit : v & ~bit);
	if (!cr_poll32(CR_PM_PAGE_MANAGEOP_ACK, bit, on ? bit : 0, 10000))
		TRACE(SFID_DBG_2, 0xE04802A8u | bit);
	*paused = on ? *paused | bit : *paused & ~bit;
}

/**
 * Load an HWRTData's free lists into a PM context, synchronised with the
 * other PM context.
 *
 * The other context may be running on the other data master for another
 * render context (``scheduler.md`` "Multi-context DM 2 / DM 3 concurrency
 * and PM freelist sync", Q38). For each list:
 *
 * - the list this context held is stored first, unless the other context
 *   owns it now (then the other context is paused instead);
 * - a list the other context holds is paused and stored from there first,
 *   so that this context gets its newest stack pointer.
 *
 * The lists are then loaded, ``CONTEXT_PB_BASE`` tells the PM which lists the
 * two contexts share, and the other context is resumed. The caller loads the
 * catalogues and stack pointers afterwards.
 *
 * Args:
 *     c: The PM context to load.
 *     hwrt_fw: Firmware address of the HWRTData.
 *     hwrt: The HWRTData.
 *
 * Returns:
 *     ``false`` if a free-list load command did not complete.
 */
bool pm_sync_load_freelists(uint32_t c, fwaddr_t hwrt_fw, const void *hwrt)
{
	static uint32_t load_seq;
	uint32_t o = c == PM_CTX_GEOM ? PM_CTX_FRAG : PM_CTX_GEOM;
	struct pm_slot *cs = &pm_slots[c], *os = &pm_slots[o];
	uint32_t paused = 0, pb;
	bool ok = true;
	fwaddr_t nfl[2], oth[2];

	/* fl[] is what each PM context's registers hold, tracked HWRTData or not */
	for (uint32_t i = 0; i < 2; i++) {
		fwaddr_t cur = cs->fl[i];

		nfl[i] = hw_rd32(hwrt, HWRT_FREELISTS + 4 * i);
		oth[i] = os->fl[i];
		if (cur && (cur != nfl[i] || cs->hwrt != hwrt_fw)) {
			/*
			 * O owns the list only while it runs, or when it loaded the
			 * list after C did. An O that finished before C's last load
			 * keeps a stale copy: C's registers are then the newest
			 * (E050: a geometry's allocations were lost when another
			 * context's geometry evicted it, and the 3D freed them twice).
			 */
			if (cur == oth[i] && (pm_ctx_running(o) || os->seq > cs->seq))
				pm_pause_ctx(o, true, &paused);       /* O owns it: no store from C */
			else if (cs->dirty)
				pm_store_freelist(c, i, fwptr(cur));
		}
		if (oth[i] && oth[i] == nfl[i]) {
			pm_pause_ctx(o, true, &paused);
			if (os->dirty)
				pm_store_freelist(o, i, fwptr(oth[i]));
		}
	}
	cs->valid = false;
	cs->dirty = false;
	cs->seq = ++load_seq;
	pm_select(c);
	for (uint32_t i = 0; i < 2; i++) {
		const struct pm_freelist *fl = fwptr(nfl[i]);

		if (fl && !pm_load_freelist(c, i, fl))
			ok = false;
	}
	pb = ((oth[0] != nfl[0]) << 1) | (oth[1] != nfl[1]);
	cr_write32(CR_PM_CONTEXT_PB_BASE, pb);
	TRACE(SFID_RTD_3, pb, (pb >> 1) & 1, pb & 1, 0u);
	pm_pause_ctx(o, false, &paused);
	return ok;
}

/* ------------------------------------------------- free-list page updates */
/**
 * Pause or resume the TA's page allocation (PM context 0).
 *
 * Args:
 *     on: Pause (``true``) or resume.
 */
static void pm_pause(bool on)
{
	cr_write32(CR_PM_PAGE_MANAGEOP, on ? 1 : 0);
	cr_poll32(CR_PM_PAGE_MANAGEOP_ACK, 1, on ? 1 : 0, 10000);
	TRACE(on ? SFID_PM_11 : SFID_PM_12, on ? 1u : 0u);
}

/**
 * Recompute a free list's current base after its page count changed.
 *
 * The host fills the page array from its end (``pvr_free_list.c``), so the
 * base is ``max_pages - current_pages`` entries into the array, aligned down
 * to 16 bytes.
 *
 * Args:
 *     fl: The free list.
 */
static void fl_rebase(struct pm_freelist *fl)
{
	/* the host fills the page array from its end: pvr_free_list.c */
	fl->current_dev_addr = (fl->freelist_dev_addr +
				(uint64_t)(fl->max_pages - fl->current_pages) * 4) &
			       ~(uint64_t)(FREELIST_BASE_ALIGN - 1);
}

/**
 * Apply pages added to a free list.
 *
 * The pages are added below the current base: the base moves down by 4 bytes
 * per page and the stack pointer up by the page count (the specification saw
 * ``0x20C8`` go from 7 to ``0x47`` for 64 pages). When a PM context holds the
 * list with the newest state, the live state is read, updated and reloaded
 * inside a ``PM_PAGE_MANAGEOP`` pause; otherwise only the structure changes.
 *
 * Args:
 *     addr: Firmware address of the free list.
 *     fl: The free list, with ``current_pages`` already raised.
 *     added: Number of pages added.
 *     old_total: ``current_pages`` before the change (only traced).
 */
static void fl_add_pages(fwaddr_t addr, struct pm_freelist *fl, uint32_t added, uint32_t old_total)
{
	bool loaded = false;

	fl_rebase(fl);
	for (uint32_t ctx = 0; ctx < 2; ctx++) {
		for (uint32_t w = 0; w < 2; w++) {
			const struct fl_regs *r = &flr[ctx][w];

			/* only a context whose live state is the newest copy */
			if (!pm_slots[ctx].dirty || pm_slots[ctx].fl[w] != addr)
				continue;
			pm_pause(true);
			fl->current_stack_top = cr_read32(r->live_sp) + added;
			fl->allocated_page_count = cr_read32(r->live_pb);
			fl->allocated_mmu_page_count = cr_read32(r->live_mmu);
			pm_load_freelist(ctx, w, fl);
			pm_pause(false);
			loaded = true;
		}
	}
	if (!loaded)
		fl->current_stack_top += added;
	TRACE(SFID_PM_7, addr, old_total, fl->current_pages,
	      (uint32_t)(fl->current_dev_addr >> 32), (uint32_t)fl->current_dev_addr);
}

/* ------------------------------------------------------------------ SPM */
/** Free lists whose grown pages can wait for the next out-of-memory event. */
#define MAX_PENDING_GROW 4
/**
 * Pages a grow update added while the geometry was not waiting for them.
 *
 * The reference firmware hands them out at the next out-of-memory event,
 * not mid-run (``partial-render-spm.md``, observed sequences); loading them
 * into a running PM stalled the TA in our first test.
 */
static struct { fwaddr_t fl; uint32_t pages; } grown[MAX_PENDING_GROW];

/**
 * Find a free list's pending grown pages.
 *
 * Args:
 *     fl: Firmware address of the free list.
 *     create: Take a free entry when the list has none.
 *
 * Returns:
 *     Pointer to the page count, or ``NULL``.
 */
static uint32_t *grown_slot(fwaddr_t fl, bool create)
{
	for (uint32_t i = 0; i < MAX_PENDING_GROW; i++)
		if (grown[i].fl == fl)
			return &grown[i].pages;
	if (create)
		for (uint32_t i = 0; i < MAX_PENDING_GROW; i++)
			if (!grown[i].fl) {
				grown[i].fl = fl;
				grown[i].pages = 0;
				return &grown[i].pages;
			}
	return NULL;
}

/** ``ROGUE_FWIF_PRBUFFER_ZSBUFFER``: index of the ZS buffer in ``pr_buffer_fw_addr[]``. */
#define PRB_ZS          0
/** ``ROGUE_FWIF_PRBUFFER_MSAABUFFER``. */
#define PRB_MSAA        1
/** ``ROGUE_FWIF_PRBUFFER_MAXSUPPORTED``. */
#define PRB_MAX         2
/* rogue_fwif_prbuffer fields (pvr_rogue_fwif_shared.h, Q41) */
#define PRB_BUFFER_ID   0x00
#define PRB_ON_DEMAND   0x04
#define PRB_STATE       0x08
#define PRB_SUBMITTED   0x10    /* partial renders kicked that use the buffer */
#define PRB_EXECUTED    0x14    /* ... and finished */
/** ``rogue_fwif_prbuffer_state``. */
enum { PRB_UNBACKED, PRB_BACKED, PRB_BACKING_PENDING, PRB_UNBACKING_PENDING };

/**
 * The SPM state of the running geometry: its state, HWRTData, render
 * context, the free list it waits for and its partial-render fence.
 */
static struct {
	enum spm_state st;
	fwaddr_t hwrt, cmd, wait_fl;
	struct ufo pr_fence;
	/** The geometry command's ``pr_buffer_fw_addr[]``: ZS and MSAA buffers. */
	fwaddr_t prb[PRB_MAX];
} spm;

/**
 * Change the SPM state, and trace it.
 *
 * Args:
 *     st: The new state.
 */
static void spm_set(enum spm_state st)
{
	static const uint32_t sfid[] = {
		[SPM_NONE] = SFID_SPM_32, [SPM_WAIT_GROW] = SFID_SPM_34,
		[SPM_WAIT_HW] = SFID_SPM_35, [SPM_PR_RUNNING] = SFID_SPM_36,
		[SPM_WAIT_BACKING] = SFID_SPM_33,       /* "PR blocked" */
	};

	spm.st = st;
	TRACE(sfid[st]);
}

static void prb_count(uint32_t off);

/**
 * Reset the SPM and forget the pending grown pages. A partial render that
 * an HWR cut short counts as executed for its buffers, so that their
 * cleanup does not wait for it forever.
 */
void spm_reset(void)
{
	if (spm.st == SPM_PR_RUNNING)
		prb_count(PRB_EXECUTED);
	memset(&spm, 0, sizeof(spm));
	memset(grown, 0, sizeof(grown));
}

/**
 * Arm the SPM for a geometry kick.
 *
 * A later kick of a render whose out-of-memory handling is still in progress
 * keeps the current state.
 *
 * Args:
 *     hwrt: Firmware address of the HWRTData.
 *     cmd_fw: Firmware address of the render context.
 *     pr_fence: The command's ``partial_render_geom_frag_fence`` UFO.
 *     pr_buffers: The command's ``pr_buffer_fw_addr[]`` (ZS, MSAA; 0 when the
 *         host backed the buffer itself).
 */
void spm_geom_kick(fwaddr_t hwrt, fwaddr_t cmd_fw, const void *pr_fence,
		   const fwaddr_t pr_buffers[PRB_MAX])
{
	if (spm.st != SPM_NONE && spm.hwrt == hwrt)
		return;         /* a later kick of a render still in OOM handling */
	spm.st = SPM_NONE;
	spm.hwrt = hwrt;
	spm.cmd = cmd_fw;
	memcpy(&spm.pr_fence, pr_fence, sizeof(spm.pr_fence));
	for (uint32_t i = 0; i < PRB_MAX; i++)
		spm.prb[i] = pr_buffers[i] & FW_VA_MASK;
}

/* ------------------------------------------- on-demand ZS/MSAA buffers */
/*
 * rogue_fwif_prbuffer (pvr_rogue_fwif_shared.h). The host creates the buffer
 * unbacked when it is on demand; the firmware asks for its pages when the TA
 * runs out of memory, and gives them back at the buffer's cleanup
 * (partial-render-spm.md, "On-demand ZS / MSAA buffer backing details", Q41).
 * The ``PRB_*`` field offsets and states are defined with ``PRB_ZS``.
 */

/**
 * Send a ``ZSBUFFER_BACKING`` or ``ZSBUFFER_UNBACKING`` request to the host.
 *
 * Args:
 *     id: The buffer's ``buffer_id``.
 *     backing: Back (``true``) or unback the buffer.
 */
static void prb_request(uint32_t id, bool backing)
{
	struct fwccb_cmd c;

	memset(&c, 0, sizeof(c));
	c.cmd_type = ROGUE_CMD_MAGIC(backing ? FWCCB_ZSBUFFER_BACKING : FWCCB_ZSBUFFER_UNBACKING);
	c.data[0] = id;         /* rogue_fwif_fwccb_cmd_zsbuffer_backing_data.zs_buffer_id */
	fwccb_send(&c);
}

/**
 * Request the backing of the partial render's buffers, at out of memory
 * (Q41 items 3 and 4).
 *
 * Both non-NULL buffers of ``pr_buffer_fw_addr[]`` are considered, whatever
 * the fragment command's flags. A buffer that is not on demand or already
 * backed needs nothing; one whose backing is pending is left alone; an
 * unbacked one, or one whose unbacking is still pending, is requested at
 * once and becomes ``BACKING_PENDING``.
 */
static void prb_request_all(void)
{
	for (uint32_t t = 0; t < PRB_MAX; t++) {
		uint8_t *b = fwptr(spm.prb[t]);
		uint32_t id, st;

		if (!b) {
			if (t == PRB_ZS)
				TRACE(SFID_SPM_21);
			else
				TRACE(SFID_SPM_46, t);
			continue;
		}
		id = hw_rd32(b, PRB_BUFFER_ID);
		st = hw_rd32(b, PRB_STATE);
		if (!hw_rd32(b, PRB_ON_DEMAND) || st == PRB_BACKED) {
			TRACE(t == PRB_ZS ? SFID_SPM_23 : SFID_SPM_47, id);
		} else if (st == PRB_BACKING_PENDING) {
			TRACE(t == PRB_ZS ? SFID_SPM_28 : SFID_SPM_51, id);
		} else {
			hw_wr32(b, PRB_STATE, PRB_BACKING_PENDING);
			TRACE(t == PRB_ZS ? SFID_SPM_26 : SFID_SPM_49, id);
			prb_request(id, true);
		}
	}
}

/**
 * Whether the partial render may run: it waits only while a backing is
 * pending (Q41 item 2). An unbacked buffer (its backing failed) does not
 * block it; the ``FRAG_PR`` then runs without that buffer.
 *
 * Returns:
 *     ``true`` when no buffer of the partial render is ``BACKING_PENDING``.
 */
static bool prb_ready(void)
{
	bool ready = true;

	for (uint32_t t = 0; t < PRB_MAX; t++) {
		const uint8_t *b = fwptr(spm.prb[t]);

		if (b && hw_rd32(b, PRB_STATE) == PRB_BACKING_PENDING) {
			TRACE(t == PRB_ZS ? SFID_SPM_30 : SFID_SPM_31, hw_rd32(b, PRB_BUFFER_ID));
			ready = false;
		}
	}
	return ready;
}

/**
 * Handle a KCCB ``ZSBUFFER_BACKING_UPDATE`` or ``ZSBUFFER_UNBACKING_UPDATE``.
 *
 * A backing update makes the buffer ``BACKED``, or ``UNBACKED`` when the host
 * failed (``SPM_42``); an unbacking update makes it ``UNBACKED``. A partial
 * render waiting for its buffers is armed once none is pending any more.
 *
 * Args:
 *     zsb: Firmware address of the buffer.
 *     done: The host succeeded.
 *     backing: The update answers a backing request (else an unbacking).
 */
void spm_zsbuffer_update(fwaddr_t zsb, bool done, bool backing)
{
	uint8_t *b = fwptr(zsb & FW_VA_MASK);
	uint32_t id;

	if (!b)
		return;
	id = hw_rd32(b, PRB_BUFFER_ID);
	if (backing) {
		hw_wr32(b, PRB_STATE, done ? PRB_BACKED : PRB_UNBACKED);
		TRACE(done ? SFID_SPM_22 : SFID_SPM_42, id);
	} else {
		hw_wr32(b, PRB_STATE, PRB_UNBACKED);
		TRACE(SFID_SPM_24, id);
	}
	if (spm.st == SPM_WAIT_BACKING && prb_ready())
		spm_set(SPM_WAIT_HW);
}

/**
 * Whether the ``FRAG_PR`` must leave out the forced depth/stencil store and
 * load: its ZS buffer exists but is not backed (Q41 item 2).
 *
 * Returns:
 *     ``true`` when the partial render has a ZS buffer that is not ``BACKED``.
 */
bool spm_pr_zs_unbacked(void)
{
	const uint8_t *b = fwptr(spm.prb[PRB_ZS]);

	return b && hw_rd32(b, PRB_STATE) != PRB_BACKED;
}

/**
 * Count a partial render against its buffers (``submitted`` at the kick,
 * ``executed`` when it finished), so that cleanup waits for it.
 *
 * Args:
 *     off: ``PRB_SUBMITTED`` or ``PRB_EXECUTED``.
 */
static void prb_count(uint32_t off)
{
	for (uint32_t t = 0; t < PRB_MAX; t++) {
		uint8_t *b = fwptr(spm.prb[t]);

		if (b)
			hw_wr32(b, off, hw_rd32(b, off) + 1);
	}
}

/**
 * Clean up a ZS/MSAA buffer (KCCB ``CLEANUP_ZSBUFFER``, Q41 items 1 and 5).
 *
 * - A partial render that uses it has not finished: busy.
 * - It is on demand and not unbacked: an unbacking is requested (unless one
 *   is already pending) and the cleanup is busy until the host's
 *   ``ZSBUFFER_UNBACKING_UPDATE``; the host then retries.
 * - Otherwise it can be freed.
 *
 * Args:
 *     zsb: Firmware address of the buffer.
 *
 * Returns:
 *     ``true`` when the cleanup must be answered busy.
 */
bool spm_zsbuffer_cleanup(fwaddr_t zsb)
{
	uint8_t *b = fwptr(zsb & FW_VA_MASK);
	uint32_t st;

	if (!b)
		return false;
	if ((int32_t)(hw_rd32(b, PRB_EXECUTED) - hw_rd32(b, PRB_SUBMITTED)) < 0)
		return true;
	st = hw_rd32(b, PRB_STATE);
	if (!hw_rd32(b, PRB_ON_DEMAND) || st == PRB_UNBACKED) {
		/* the spec flushes the D-cache here; host memory is uncached in this firmware */
		return false;
	}
	if (st != PRB_UNBACKING_PENDING) {
		hw_wr32(b, PRB_STATE, PRB_UNBACKING_PENDING);
		TRACE(SFID_SPM_27, hw_rd32(b, PRB_BUFFER_ID));
		prb_request(hw_rd32(b, PRB_BUFFER_ID), false);
	}
	return true;
}

/*
 * TA control around an out-of-memory event (partial-render-spm.md, "Resuming
 * geometry after OOM", answer to Q28). None has a public name.
 */

#define CR_PM_RESUME        0x0328   /* resumes the PM / TA after a free-list update */
#define CR_TA_STOP_ON_OOM   0x0320
#define CR_TA_PAUSE         0x0CF0   /* pause TE/TA state, self-clearing */
#define CR_TA_UNPAUSE       0x0CF8
#define CR_PM_PR_RESTART_A  0x0290
#define CR_PM_PR_RESTART_B  0x0198
#define CR_PM_PR_RESTART_C  0x01B8   /* self-clearing */
#define CR_TA_RESTART       0x0CA8

/** Resume the PM and the TA after a free-list update that gave ready pages. */
static void spm_resume_ta(void)
{
	cr_write32(CR_PM_RESUME, 1);
}

/**
 * Stop the TA until a grow update or a partial render.
 *
 * Asks the TA to stop on the out-of-memory event, waits for
 * ``TA_TERMINATE``, then pauses the TE/TA state.
 */
static void spm_pause_ta(void)
{
	cr_write32(CR_TA_STOP_ON_OOM, 1);
	if (!cr_poll32(CR_EVENT_STATUS, EV_TA_TERMINATE, EV_TA_TERMINATE, 10000))
		TRACE(SFID_DBG_2, 0xE0420A40u);
	cr_write32(CR_EVENT_CLEAR, EV_TA_TERMINATE);
	cr_write32(CR_TA_PAUSE, 1);
	if (!cr_poll32(CR_TA_PAUSE, 1, 0, 10000))
		TRACE(SFID_DBG_2, 0xE0420CF0u);
}

/** Restart the geometry paused by :c:func:`spm_pause_ta` after a grow update. */
static void spm_unpause_ta(void)
{
	const void *hwrt = fwptr(spm.hwrt);

	if (hwrt)
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
	cr_write32(CR_PM_RESUME, 1);
	cr_write32(CR_TA_UNPAUSE, 1);
}

/**
 * Ask the host to grow a free list (``FWCCB_FREELIST_GROW``), once.
 *
 * Args:
 *     fl: The free list.
 */
static void request_grow(struct pm_freelist *fl)
{
	struct fwccb_cmd c;

	if (fl->grow_pending)
		return;
	memset(&c, 0, sizeof(c));
	c.cmd_type = ROGUE_CMD_MAGIC(FWCCB_FREELIST_GROW);
	c.data[0] = fl->freelist_id;
	fl->grow_pending = 1;
	fwccb_send(&c);
}

/**
 * Check whether the host can still grow a free list.
 *
 * Args:
 *     fl: The free list.
 *
 * Returns:
 *     ``true`` when it has a grow size and is below its maximum.
 */
static bool can_grow(const struct pm_freelist *fl)
{
	return fl->grow_pages && fl->current_pages + fl->ready_pages < fl->max_pages;
}

/**
 * Arm a partial render.
 *
 * Without a partial-render fence nothing is armed (the watchdog decides).
 * Otherwise the touched layers are listed (:c:func:`geom_rta_oom`, Q39) and
 * the state becomes ``SPM_WAIT_HW``: the scheduler then runs the fragment
 * context's ``FRAG_PR`` as a partial render, looking past its ``FENCE_PR``
 * without consuming it.
 */
static void start_partial_render(void)
{
	volatile uint32_t *p = fwptr(spm.pr_fence.addr & ~UFO_ADDR_IS_SYNC_CHECKPOINT);

	if (!spm.pr_fence.addr || !p) {
		TRACE(SFID_SPM_33);     /* PR blocked: no fence, the watchdog decides */
		return;
	}
	TRACE(SFID_SPM_66, spm.pr_fence.addr, *p, spm.pr_fence.value);
	TRACE(SFID_SPM_33);
	/* render-target arrays: the layers the partial render must cover (Q39) */
	{
		const struct fwcommoncontext *gctx = fwptr(spm.cmd);

		if (gctx)
			geom_rta_oom(fwptr(spm.hwrt), gctx->fw_mem_context_fw_addr);
	}
	/* on-demand ZS/MSAA buffers first: the partial render stores into them (Q41) */
	prb_request_all();
	if (!prb_ready()) {
		spm_set(SPM_WAIT_BACKING);
		return;
	}
	/*
	 * The scheduler now runs the fragment context's FRAG_PR as a partial
	 * render, looking past its FENCE_PR without consuming it (sched.c).
	 */
	spm_set(SPM_WAIT_HW);
}

/**
 * The SPM failing-UFO check (``partial-render-spm.md`` fidelity notes).
 *
 * While a partial render is armed, a ``FENCE_PR`` UFO that only waits for this
 * geometry (its queue timeline: the address of
 * ``partial_render_geom_frag_fence``) does not block the partial render.
 *
 * Args:
 *     u: The unsatisfied UFO.
 *
 * Returns:
 *     ``true`` when the UFO may be passed.
 */
bool spm_pr_fence_ok(const struct ufo *u)
{
	return spm.st == SPM_WAIT_HW && spm.pr_fence.addr && u->addr == spm.pr_fence.addr;
}

/**
 * Handle ``PM_OUT_OF_MEMORY`` while the geometry runs.
 *
 * The firmware does not ask the hardware which list ran out: it always
 * extends the HWRTData's global free list (``freelists[1]``; ``[2]`` for a
 * secondary geometry pipe, flags bit 8).
 *
 * 1. Pages from a grow update that arrived while the geometry ran, or else
 *    the ready pages, are added at once and the TA resumes; more is
 *    requested from the host in the background.
 * 2. With nothing ready the TA is stopped; the host is asked to grow the
 *    list if it can (state ``SPM_WAIT_GROW``), otherwise a partial render is
 *    armed.
 */
void spm_oom(void)
{
	const void *hwrt = fwptr(spm.hwrt);
	uint32_t which;
	fwaddr_t addr;
	struct pm_freelist *fl;
	uint32_t *pend, n, old;

	if (!hwrt || spm.st != SPM_NONE)
		return;
	which = (hw_rd32(hwrt, HWRT_FLAGS) & 0x100) ? 2 : PM_FL_GLOBAL;
	addr = hw_rd32(hwrt, HWRT_FREELISTS + 4 * which);
	fl = fwptr(addr);
	TRACE(SFID_SPM_7, spm.cmd, hw_rd32(hwrt, HWRT_FREELISTS), addr);
	if (!fl) {
		start_partial_render();
		return;
	}
	hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_GEOM_OUTOFMEM);

	/*
	 * 1. pages of a grow update that arrived while the geometry ran, then the
	 * ready pages; no host round trip. More is requested in the background.
	 */
	pend = grown_slot(addr, false);
	n = (pend && *pend) ? *pend : fl->ready_pages;
	if (n) {
		if (pend && *pend)
			*pend = 0;
		else
			fl->ready_pages = 0;
		old = fl->current_pages;
		fl->current_pages += n;
		fl_add_pages(addr, fl, n, old);
		TRACE(SFID_SPM_44, (uint32_t)(fl->current_dev_addr >> 32),
		      (uint32_t)fl->current_dev_addr, fl->current_pages, fl->current_stack_top);
		spm_resume_ta();
		if (can_grow(fl))
			request_grow(fl);
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
		return;
	}
	/* 2. nothing ready: stop the TA, then grow or partial render */
	spm_pause_ta();
	if (can_grow(fl)) {
		request_grow(fl);
		spm.wait_fl = addr;
		TRACE(SFID_SPM_41, addr);
		spm_set(SPM_WAIT_GROW);
		return;
	}
	start_partial_render();
}

/**
 * Handle a KCCB ``FREELIST_GROW_UPDATE``: the host grew a free list.
 *
 * When the geometry is not waiting for this list, the new pages are held
 * until its next out-of-memory event (or applied at once when no entry is
 * free). When it waits, the pages (or the ready pages) are added and the TA
 * restarts; if the host could not add any, a partial render is armed.
 *
 * Args:
 *     addr: Firmware address of the free list.
 *     delta: Pages added by the host (not used: derived from the totals).
 *     new_pages: The list's new page total, ready pages included.
 *     ready: Ready pages held back for the next out-of-memory event.
 */
void spm_grow_update(fwaddr_t addr, uint32_t delta, uint32_t new_pages, uint32_t ready)
{
	struct pm_freelist *fl = fwptr(addr);
	uint32_t old, cur, added;

	if (!fl)
		return;
	TRACE(SFID_SPM_54, 0u, addr, new_pages, ready);
	old = fl->current_pages;
	cur = new_pages > ready ? new_pages - ready : 0;
	added = cur > old ? cur - old : 0;
	fl->ready_pages = ready;
	fl->grow_pending = 0;
	(void)delta;

	if (!(spm.st == SPM_WAIT_GROW && spm.wait_fl == addr)) {
		/* the geometry is not waiting: hold the pages until its next OOM */
		uint32_t *pend = added ? grown_slot(addr, true) : NULL;

		if (pend)
			*pend += added;
		else if (added) {               /* no slot: apply now, not loaded in a PM */
			fl->current_pages = cur;
			fl_add_pages(addr, fl, added, old);
		}
		return;
	}
	fl->current_pages = cur > old ? cur : old;
	if (!added && fl->ready_pages) {
		/* everything the host added is ready pages: hand them over now */
		added = fl->ready_pages;
		fl->ready_pages = 0;
		fl->current_pages = old + added;
	}
	if (added) {
		fl_add_pages(addr, fl, added, old);
		spm_unpause_ta();
		spm_set(SPM_NONE);
	} else {
		spm.st = SPM_NONE;
		start_partial_render();         /* the host could not grow */
	}
}

/**
 * Check whether a partial render is armed for an HWRTData.
 *
 * Args:
 *     hwrt: Firmware address of the HWRTData.
 *
 * Returns:
 *     ``true`` in ``SPM_WAIT_HW`` for this HWRTData.
 */
bool spm_pr_armed(fwaddr_t hwrt)
{
	return spm.st == SPM_WAIT_HW && spm.hwrt == hwrt;
}

/** Record that the armed partial render was kicked. */
void spm_pr_started(void)
{
	prb_count(PRB_SUBMITTED);
	spm_set(SPM_PR_RUNNING);
}

/**
 * Restart the paused geometry after a partial render.
 *
 * Follows ``partial-render-spm.md`` "Restarting geometry after a partial
 * render": turns the partial-render mode off, drops the 3D PM context's
 * state (it is the partial render's), selects PM context 0, pulses the
 * restart registers, invalidates the PM TLB, then resumes the PM and the TA.
 */
void spm_pr_done(void)
{
	const void *hwrt = fwptr(spm.hwrt);

	cr_write32(CR_PM_PARTIAL_RENDER_ENABLE, 0);
	prb_count(PRB_EXECUTED);
	/* the 3D PM context's state is the partial render's: drop the slot */
	pm_slots[PM_CTX_FRAG].valid = false;
	pm_slots[PM_CTX_FRAG].dirty = false;
	if (hwrt)
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
	pm_select(PM_CTX_GEOM);
	cr_write32(CR_PM_PR_RESTART_A, 1);
	cr_write32(CR_PM_PR_RESTART_B, 1);
#ifdef EXP_PR_FULL_INVAL
	pm_mmu_inval();         /* SLC + BIF + PM TLB: hung the board in E043 */
#else
	cr_write32(CR_PM_TLB_INVAL, 1);
#endif
	cr_write32(CR_PM_PR_RESTART_C, 1);
	if (!cr_poll32(CR_PM_PR_RESTART_C, 1, 0, 10000))
		TRACE(SFID_DBG_2, 0xE04201B8u);
	cr_write32(CR_PM_RESUME, 1);
	cr_write32(CR_TA_RESTART, 1);
	TRACE(SFID_SPM_38);
	spm_set(SPM_NONE);
}

/* ------------------------------------------------------------------ HWR */
/** At most this many free lists per reconstruction request (``ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT``). */
#define MAX_FL_RECONSTRUCT 18   /* ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT */

/** Flag set in a free-list ID of the reconstruction update when the host failed. */
#define FL_RECONSTRUCT_FAILED 0x80000000u   /* ROGUE_FWIF_FREELISTS_RECONSTRUCTION_FAILED_FLAG */

/**
 * Free lists whose reconstruction the host has not confirmed yet, and the DMs
 * that the scheduler holds meanwhile (traced once per DM).
 */
static struct {
	uint32_t ids[MAX_FL_RECONSTRUCT];
	uint32_t cnt;
	uint32_t held;
} recon;

/**
 * Add a free list to a reconstruction request, once.
 *
 * Also records it as pending until the host's update, clears its grow
 * request and traces the request.
 *
 * Args:
 *     ids: IDs of the request.
 *     cnt: Number of entries in ``ids``, updated.
 *     fl_fw: Firmware address of the free list.
 */
static void fl_collect(uint32_t *ids, uint32_t *cnt, fwaddr_t fl_fw)
{
	struct pm_freelist *fl = fwptr(fl_fw);
	bool dup = false;

	if (!fl)
		return;
	fl->grow_pending = 0;
	for (uint32_t k = 0; k < *cnt; k++)
		dup |= ids[k] == fl->freelist_id;
	if (dup || *cnt >= MAX_FL_RECONSTRUCT)
		return;
	ids[(*cnt)++] = fl->freelist_id;
	TRACE(SFID_HWR_52, fl_fw, fl->freelist_id);
	for (uint32_t k = 0; k < recon.cnt; k++)
		if (recon.ids[k] == fl->freelist_id)
			return;
	if (recon.cnt < MAX_FL_RECONSTRUCT)
		recon.ids[recon.cnt++] = fl->freelist_id;
}

/**
 * Ask the host to rebuild the free lists after a hardware recovery
 * (``hardware-recovery.md`` step 4).
 *
 * Collects, without duplicates, every free list loaded in a PM context and
 * every free list of the given HWRTData, resets the SPM and the PM context
 * tracking, marks those HWRTData ``HWR`` with the caches to be zeroed, and
 * sends ``FWCCB_FREELISTS_RECONSTRUCTION`` with the list IDs and the HWR
 * count. The geometry and 3D DMs are then held until the host confirms the
 * rebuild (:c:func:`pm_reconstruct_update`, E054): the host rewrites the free
 * lists in memory meanwhile.
 *
 * Args:
 *     hwrts: Firmware addresses of the HWRTData in use.
 *     n: Number of entries in ``hwrts``.
 */
void pm_hwr_reconstruct(const fwaddr_t *hwrts, uint32_t n)
{
	struct fwccb_cmd c;
	uint32_t ids[MAX_FL_RECONSTRUCT], cnt = 0;

	/* every free list loaded in a PM slot (hardware-recovery.md step 4) */
	for (uint32_t ctx = 0; ctx < 2; ctx++)
		for (uint32_t w = 0; pm_slots[ctx].valid && w < 2; w++)
			fl_collect(ids, &cnt, pm_slots[ctx].fl[w]);
	spm_reset();
	pm_invalidate();
	for (uint32_t i = 0; i < n; i++) {
		const void *h = fwptr(hwrts[i]);

		if (!h)
			continue;
		hw_wr32(h, HWRT_STATE, RTDATA_STATE_HWR);
		hw_wr32(h, HWRT_GEOM_CACHES_ZEROING, 1);
		for (uint32_t w = 0; w < 2; w++)
			fl_collect(ids, &cnt, hw_rd32(h, HWRT_FREELISTS + 4 * w));
	}
	if (!cnt)
		return;
	memset(&c, 0, sizeof(c));
	c.cmd_type = ROGUE_CMD_MAGIC(FWCCB_FREELISTS_RECONSTRUCTION);
	c.data[0] = cnt;
	c.data[1] = cr_read32(CR_DBG_HWR_COUNT);
	for (uint32_t k = 0; k < cnt; k++)
		c.data[2 + k] = ids[k];
	fwccb_send(&c);
}

/**
 * Discard a command whose HWRTData an HWR left in the ``HWR`` state (Q42).
 *
 * ``hardware-recovery.md`` "Discarded geometry and subsequent 3D commands":
 * a 3D or a non-first geometry kick on an HWRTData in state ``HWR`` (set by
 * the recovery and by the host's free-list reconstruction) must not touch
 * the GPU. The command is retired (its fences are then updated by the
 * commands that follow it) and the context's resume flags are cleared. A
 * first geometry kick is never discarded: it moves the state on.
 *
 * Args:
 *     dm: The data master of the command.
 *     ctxaddr: Firmware address of the context.
 *     ctx: The context.
 *     hwrt_fw: Firmware address of the HWRTData.
 *     cmd_off: Offset of the command in the client CCB.
 *
 * Returns:
 *     ``true`` when the command must be retired without running.
 */
bool hwrt_discard(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
		  fwaddr_t hwrt_fw, uint32_t cmd_off)
{
	const void *h = fwptr(hwrt_fw);
	uint32_t st;

	if (!h || (st = hw_rd32(h, HWRT_STATE)) != RTDATA_STATE_HWR)
		return false;
	ctx->fw_com_ctx_flags &= ~0xFFu;
	TRACE(SFID_HWR_15, dm, 1u, hwrt_fw, st, ctxaddr, cmd_off);
	return true;
}

/**
 * Handle the host's ``KCCB_FREELISTS_RECONSTRUCTION_UPDATE``.
 *
 * Each listed free list is no longer pending (``HWR_53``, or ``HWR_55``
 * when the host flags a failure; the list is released either way, since the
 * mainline host never fails). Once none is pending, the held DMs may run
 * again (``RTD_22``); the caller runs the scheduler.
 *
 * Args:
 *     ids: The free-list IDs of the update.
 *     n: Number of entries in ``ids``.
 */
void pm_reconstruct_update(const uint32_t *ids, uint32_t n)
{
	if (n > MAX_FL_RECONSTRUCT)
		n = MAX_FL_RECONSTRUCT;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t id = ids[i] & ~FL_RECONSTRUCT_FAILED;

		if (ids[i] & FL_RECONSTRUCT_FAILED)
			TRACE(SFID_HWR_55, id);
		else
			TRACE(SFID_HWR_53, id);
		for (uint32_t k = 0; k < recon.cnt; k++)
			if (recon.ids[k] == id) {
				recon.ids[k] = recon.ids[--recon.cnt];
				break;
			}
	}
	if (!recon.cnt && recon.held) {
		recon.held = 0;
		TRACE(SFID_RTD_22);
	}
}

/**
 * Whether a DM must wait for a free-list reconstruction.
 *
 * The geometry and 3D DMs use the free lists, so they are held while any
 * reconstruction is pending (``HWR_63``, traced once per DM and HWR).
 *
 * Args:
 *     dm: The data master.
 *
 * Returns:
 *     ``true`` when the DM must not start a task now.
 */
bool pm_reconstruct_hold(uint32_t dm)
{
	if (!recon.cnt || (dm != DM_GEOM && dm != DM_FRAG))
		return false;
	if (!(recon.held & (1u << dm))) {
		recon.held |= 1u << dm;
		TRACE(SFID_HWR_63, dm, 0u);
	}
	return true;
}
