// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Parameter Manager: free-list / list loads, PM context bookkeeping and the
 * out-of-memory handling (SPM).
 *
 * Sources:
 *   docsClean/kick-geometry-ta.md phase B (Q16b-Q16m answers): free-list
 *     banks, live stack pointers, OOM / partial-render register changes;
 *   docsClean/partial-render-spm.md: SPM decisions, states, fidelity notes;
 *   E035 / E037: free-list and list stack-pointer load commands;
 *   mainline pvr_free_list.c: the host side of grow and reconstruction.
 */
#include "kick.h"
#include "pm.h"

/* ------------------------------------------------------------ registers */
#define CR_PM_ALIST_SP_LOAD      0x0278   /* load value; live ctx0 0x0270, ctx1 0x0280 */
#define CR_PM_MLIST_SP_LOAD      0x02C8   /* load value; live ctx0 0x02C0, ctx1 0x02D0 */
#define CR_PM_ALIST_SP_CMD       0x0288   /* E037: load commands, write 1, wait bit 0 = 0 */
#define CR_PM_MLIST_SP_CMD       0x0190
#define CR_PM_CTX_SEL_A          0x03D0   /* ctx0: bit 8, ctx1: bit 16 (Q16h) */
#define CR_PM_CTX_SEL_B          0x03D4   /* ctx0: bit 0, ctx1: bit 8 */
#define CR_PM_PAGE_MANAGEOP      0x02A0   /* 1 = pause TA allocation */
#define CR_PM_PAGE_MANAGEOP_ACK  0x02A8
#define FREELIST_BASE_ALIGN      16u      /* ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE */

struct fl_regs {
	uint16_t base, top, cnt, cmd;     /* load registers and load command */
	uint16_t live_sp, live_pb, live_mmu;
	uint8_t top_shift;                /* position of the stack top in the 64-bit top register */
};

/* [ctx][which]: E035 statements, Q16b-Q16k answers */
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

static const uint16_t mlist_base[2] = { 0x02D8, 0x02E0 };
static const uint16_t cat_base[2][3] = {
	{ CR_BIF_PM_CAT_BASE_VCE0, CR_BIF_PM_CAT_BASE_TE0, CR_BIF_PM_CAT_BASE_ALIST0 },
	{ CR_BIF_PM_CAT_BASE_VCE1, CR_BIF_PM_CAT_BASE_TE1, CR_BIF_PM_CAT_BASE_ALIST1 },
};

struct pm_slot pm_slots[2];

static bool pm_cmd(uint32_t reg)
{
	cr_write32(reg, 1);
	return cr_poll32(reg, 1, 0, 10000);
}

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

void pm_load_lists(uint32_t ctx, uint64_t mlist, uint64_t vce, uint64_t te, uint64_t alist,
		   uint64_t alist_sp, uint32_t mlist_sp)
{
	cr_write64(mlist_base[ctx], mlist);
	cr_write64(cat_base[ctx][0], vce);
	cr_write64(cat_base[ctx][1], te);
	cr_write64(cat_base[ctx][2], alist);
	cr_write64(CR_PM_ALIST_SP_LOAD, alist_sp);
	cr_write32(CR_PM_MLIST_SP_LOAD, mlist_sp);
	pm_cmd(CR_PM_ALIST_SP_CMD);
	pm_cmd(CR_PM_MLIST_SP_CMD);
}

/*
 * Before another HWRTData is loaded into a PM context, store the free-list
 * state the context holds ("Store Freelist ... for PMDM<n>"). Without it, two
 * renders that interleave (geometry A, geometry B, fragment A) leave A's free
 * lists stale, and the PM can hand out pages that are still in use (E041).
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

/*
 * The context's free-list state was stored (fragment load for context 0, end
 * of the fragment for context 1): the structs are now the newest copy, and a
 * later eviction must not overwrite them with the stale registers. Without
 * this, the second render of an HWRTData pair hung the TA (E041).
 */
void pm_slot_stored(uint32_t ctx)
{
	pm_slots[ctx].dirty = false;
}

void pm_slot_set(uint32_t ctx, fwaddr_t hwrt)
{
	const void *h = fwptr(hwrt);

	pm_slots[ctx].hwrt = hwrt;
	pm_slots[ctx].fl[0] = h ? hw_rd32(h, HWRT_FREELISTS) : 0;
	pm_slots[ctx].fl[1] = h ? hw_rd32(h, HWRT_FREELISTS + 4) : 0;
	pm_slots[ctx].valid = hwrt != 0;
	pm_slots[ctx].dirty = hwrt != 0;
}

void pm_invalidate(void)
{
	memset(pm_slots, 0, sizeof(pm_slots));
}

/* ------------------------------------------------- free-list page updates */
static void pm_pause(bool on)
{
	cr_write32(CR_PM_PAGE_MANAGEOP, on ? 1 : 0);
	cr_poll32(CR_PM_PAGE_MANAGEOP_ACK, 1, on ? 1 : 0, 10000);
	TRACE(on ? SFID_PM_11 : SFID_PM_12, on ? 1u : 0u);
}

static void fl_rebase(struct pm_freelist *fl)
{
	/* the host fills the page array from its end: pvr_free_list.c */
	fl->current_dev_addr = (fl->freelist_dev_addr +
				(uint64_t)(fl->max_pages - fl->current_pages) * 4) &
			       ~(uint64_t)(FREELIST_BASE_ALIGN - 1);
}

/*
 * N pages were added below the current base: the base moves down by 4 bytes per
 * page and the live stack pointer up by N (spec: 0x20C8 7 -> 0x47 for 64 pages).
 * Applied inside a PM_PAGE_MANAGEOP pause when the free list is loaded.
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
/*
 * Pages a grow update added while the geometry was not waiting for them: in
 * the reference they are handed out at the next OOM, not loaded mid-run
 * (partial-render-spm.md, observed sequences); loading them into a running
 * PM stalled the TA in our first test.
 */
#define MAX_PENDING_GROW 4
static struct { fwaddr_t fl; uint32_t pages; } grown[MAX_PENDING_GROW];

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

static struct {
	enum spm_state st;
	fwaddr_t hwrt, cmd, wait_fl;
	struct ufo pr_fence;
} spm;

static void spm_set(enum spm_state st)
{
	static const uint32_t sfid[] = {
		[SPM_NONE] = SFID_SPM_32, [SPM_WAIT_GROW] = SFID_SPM_34,
		[SPM_WAIT_HW] = SFID_SPM_35, [SPM_PR_RUNNING] = SFID_SPM_36,
	};

	spm.st = st;
	TRACE(sfid[st]);
}

void spm_reset(void)
{
	memset(&spm, 0, sizeof(spm));
	memset(grown, 0, sizeof(grown));
}

void spm_geom_kick(fwaddr_t hwrt, fwaddr_t cmd_fw, const void *pr_fence)
{
	if (spm.st != SPM_NONE && spm.hwrt == hwrt)
		return;         /* a later kick of a render still in OOM handling */
	spm.st = SPM_NONE;
	spm.hwrt = hwrt;
	spm.cmd = cmd_fw;
	memcpy(&spm.pr_fence, pr_fence, sizeof(spm.pr_fence));
}

/*
 * Let the TA continue after pages were added (E041 experiments).
 * EXP_RESUME = 1: clear PM_OUT_OF_MEMORY again after the update.
 */
#ifndef EXP_RESUME
#define EXP_RESUME 1
#endif
static void spm_resume_ta(void)
{
	TRACE(SFID_DBG_6, 0xE0410A01u, cr_read32(CR_EVENT_STATUS), cr_read32(0x20C8),
	      cr_read32(0x0348), cr_read32(0x0D00));
#if EXP_RESUME == 1
	cr_write32(CR_EVENT_CLEAR, EV_PM_OUT_OF_MEMORY);
#elif EXP_RESUME == 2
	cr_write32(CR_EVENT_CLEAR, EV_PM_OUT_OF_MEMORY);
	cr_write32(0x0400, 1);          /* TA run register again */
#endif
	TRACE(SFID_DBG_4, 0xE0410A02u, cr_read32(0x20C8), cr_read32(0x0D00));
}

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

static bool can_grow(const struct pm_freelist *fl)
{
	return fl->grow_pages && fl->current_pages + fl->ready_pages < fl->max_pages;
}

static void start_partial_render(void)
{
	volatile uint32_t *p = fwptr(spm.pr_fence.addr & ~UFO_ADDR_IS_SYNC_CHECKPOINT);

	if (!spm.pr_fence.addr || !p) {
		TRACE(SFID_SPM_33);     /* PR blocked: no fence, the watchdog decides */
		return;
	}
	TRACE(SFID_SPM_66, spm.pr_fence.addr, *p, spm.pr_fence.value);
	TRACE(SFID_SPM_33);
	/* let the fragment context's FENCE_PR pass: FRAG_PR then runs as a PR */
	sched_ufo_signal(&spm.pr_fence);
	spm_set(SPM_WAIT_HW);
}

void spm_oom(void)
{
	const void *hwrt = fwptr(spm.hwrt);
	struct pm_freelist *fls[2];

	TRACE(SFID_DBG_4, 0xE0410A00u, spm.st, cr_read32(CR_EVENT_STATUS));
	if (!hwrt || spm.st != SPM_NONE)
		return;
	fls[0] = hwrt_freelist(hwrt, PM_FL_GLOBAL);
	fls[1] = hwrt_freelist(hwrt, PM_FL_LOCAL);
	TRACE(SFID_SPM_7, spm.cmd, hw_rd32(hwrt, HWRT_FREELISTS + 4), hw_rd32(hwrt, HWRT_FREELISTS));
	hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_GEOM_OUTOFMEM);

	/* 0. pages of a grow update that arrived while the geometry ran */
	for (uint32_t i = 0; i < 2; i++) {
		fwaddr_t addr = hw_rd32(hwrt, HWRT_FREELISTS + 4 * (1 - i));
		uint32_t *pend = grown_slot(addr, false);
		struct pm_freelist *fl = fls[i];
		uint32_t old;

		if (!fl || !pend || !*pend)
			continue;
		old = fl->current_pages;
		fl->current_pages += *pend;
		fl_add_pages(addr, fl, *pend, old);
		*pend = 0;
		TRACE(SFID_SPM_44, (uint32_t)(fl->current_dev_addr >> 32),
		      (uint32_t)fl->current_dev_addr, fl->current_pages, fl->current_stack_top);
		spm_resume_ta();
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
		return;
	}
	/* 1. ready pages: no host round trip; then ask for more in the background */
	for (uint32_t i = 0; i < 2; i++) {
		struct pm_freelist *fl = fls[i];
		fwaddr_t addr = hw_rd32(hwrt, HWRT_FREELISTS + 4 * (1 - i));
		uint32_t n, old;

		if (!fl || !fl->ready_pages)
			continue;
		n = fl->ready_pages;
		old = fl->current_pages;
		fl->current_pages += n;
		fl->ready_pages = 0;
		fl_add_pages(addr, fl, n, old);
		TRACE(SFID_SPM_44, (uint32_t)(fl->current_dev_addr >> 32),
		      (uint32_t)fl->current_dev_addr, fl->current_pages, fl->current_stack_top);
		spm_resume_ta();
		if (can_grow(fl))
			request_grow(fl);
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
		return;
	}
	/* 2. grow: the geometry waits for the host's GROW_UPDATE */
	for (uint32_t i = 0; i < 2; i++) {
		struct pm_freelist *fl = fls[i];

		if (!fl || !can_grow(fl))
			continue;
		request_grow(fl);
		spm.wait_fl = hw_rd32(hwrt, HWRT_FREELISTS + 4 * (1 - i));
		TRACE(SFID_SPM_41, spm.wait_fl);
		spm_set(SPM_WAIT_GROW);
		return;
	}
	/* 3. partial render */
	start_partial_render();
}

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
	if (added)
		fl_add_pages(addr, fl, added, old);
	{
		if (added) {
			const void *hwrt = fwptr(spm.hwrt);

			if (hwrt)
				hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
			spm_resume_ta();
			spm_set(SPM_NONE);
		} else {
			spm.st = SPM_NONE;
			start_partial_render();         /* the host could not grow */
		}
	}
}

bool spm_pr_armed(fwaddr_t hwrt)
{
	return spm.st == SPM_WAIT_HW && spm.hwrt == hwrt;
}

void spm_pr_started(void)
{
	spm_set(SPM_PR_RUNNING);
}

/*
 * End of a partial render: PM_PARTIAL_RENDER_ENABLE back to 0, a context-0
 * load of zero list stack pointers and catalogues (the PM restarts them,
 * INIT_PAGE in VCE0), and the context-1 slot invalidated (fidelity note).
 * The geometry continues without a new kick.
 */
void spm_pr_done(void)
{
	const void *hwrt = fwptr(spm.hwrt);

	cr_write32(CR_PM_PARTIAL_RENDER_ENABLE, 0);
	if (hwrt) {
		pm_select(PM_CTX_GEOM);
		pm_load_lists(PM_CTX_GEOM, hw_rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR), 0, 0, 0, 0, 0);
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_GEOM);
	}
	pm_slots[PM_CTX_FRAG].valid = false;
	pm_slots[PM_CTX_FRAG].dirty = false;
	TRACE(SFID_SPM_38);
	spm_set(SPM_NONE);
}

/* ------------------------------------------------------------------ HWR */
#define MAX_FL_RECONSTRUCT 18   /* ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT */

void pm_hwr_reconstruct(const fwaddr_t *hwrts, uint32_t n)
{
	struct fwccb_cmd c;
	uint32_t ids[MAX_FL_RECONSTRUCT], cnt = 0;

	spm_reset();
	pm_invalidate();
	for (uint32_t i = 0; i < n; i++) {
		const void *h = fwptr(hwrts[i]);

		if (!h)
			continue;
		hw_wr32(h, HWRT_STATE, RTDATA_STATE_HWR);
		hw_wr32(h, HWRT_GEOM_CACHES_ZEROING, 1);
		for (uint32_t w = 0; w < 2; w++) {
			struct pm_freelist *fl = hwrt_freelist(h, w);
			bool dup = false;

			if (!fl)
				continue;
			for (uint32_t k = 0; k < cnt; k++)
				dup |= ids[k] == fl->freelist_id;
			if (!dup && cnt < MAX_FL_RECONSTRUCT)
				ids[cnt++] = fl->freelist_id;
			fl->grow_pending = 0;
		}
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
