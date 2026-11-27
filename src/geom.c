// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Geometry (TA) kick.
 *
 * Sources: docsClean/kick-geometry-ta.md (phases A-G, the Q16b-Q16m answers),
 * docsClean/command-structs.md and data-structures.md (public layouts),
 * the experiments E027-E040 (docs/logRefs/).  The PM side lives in pm.c.
 */
#include "kick.h"
#include "pm.h"

#define COMMON_SCREEN_PIXEL_MAX  0x04
#define COMMON_MSCTL             0x08
#define COMMON_MSCTL_FLIPPED     0x10
#define COMMON_TPC_STRIDE        0x18
#define COMMON_TPC_SIZE          0x1C
#define COMMON_TE_SCREEN         0x20
#define COMMON_MTILE_STRIDE      0x24
#define COMMON_TE_AA             0x28
#define COMMON_TE_MTILE1         0x2C
#define COMMON_TE_MTILE2         0x30

/* rogue_fwif_cmd_geom (payload offsets, command-structs.md) */
#define GEOM_HWRT                0x04
#define GEOM_REGS                0x10
#define GEOM_FLAGS               0x50
#define GEOM_PR_FENCE            0x54   /* partial_render_geom_frag_fence (rogue_fwif_ufo) */
#define GEOM_FLAGS_FIRSTKICK     (1u << 0)
#define GEOM_FLAGS_LASTKICK      (1u << 1)
#define GEOM_FLAGS_FLIP          (1u << 2)   /* flipped multisample control */

/* registers (geometry core bank) */
#define CR_PM_HEAP_BASE          0x0240   /* 0x80_0000_0000, general heap (Q16g) */
#define CR_PM_VHEAP_TABLE        0x0248
#define CR_PM_CONTEXT_PB_BASE    0x02B0
#define CR_PM_20F8               0x20F8   /* 1 from the first load (Q16h) */
#define CR_VDM_KICK              0x0400
#define CR_VDM_CTRL_STREAM_BASE  0x0408
#define CR_VDM_CALL_STACK_PTR    0x0418
#define CR_VDM_CTX_STATE_BASE    0x0428
#define CR_TE_MTILE1             0x0C08
#define CR_TE_MTILE2             0x0C10
#define CR_TE_SCREEN             0x0C18
#define CR_TE_MTILE_STRIDE       0x0C20
#define CR_TE_PSG                0x0C28
#define CR_TE_PSGREGION_ADDR     0x0C38
#define CR_TE_TPC_ADDR           0x0C40
#define CR_GEOM_CTX_ENABLE       0x0C68
#define CR_TE_AA                 0x0C00
#define CR_PPP_MSCTL_SEL         0x0C80   /* msctl or flipped msctl */
#define CR_PDS_CTRL_GEOM         0x4098   /* cmd_geom.pds_ctrl (8-bit) */
#define CR_PM_CTX_SEL_A          0x03D0
#define CR_PM_CTX_SEL_B          0x03D4
#define CR_PM_RESET_A            0x0290   /* first-kick PM reset pulses */
#define CR_PM_RESET_B            0x0198
#define CR_PPP_CTRL              0x0C88
#define CR_PPP_SCREEN            0x0C98
#define CR_RTC_BASE              0x0CB0   /* rtc_dev_addr, 0 without an RTC */
#define CR_RTC_CTRL              0x0CB8   /* 1 at kick, 2 = flush (TA finished) */
#define CR_TE_TPC_STRIDE         0x0C48
#define CR_TE_TPC_FLUSH          0x0C50   /* bit 30 */
#define CR_TA_CACHE_INIT_FIRST   0x0258
#define CR_TA_CACHE_INIT         0x0250
#define CR_VHEAP_STORE           0x0260
#define CR_RTA_ACTIVE            0x0D20
#define CR_GEOM_CE0              0x0CE0   /* 1 first kick, 3 later kicks, 0 with an RTC */
#define CR_TPU_BORDER_VDM        0x17A0
#define CR_TE_CORE_SEL           0x40A8   /* (primary core) & 1, phase D */
#define CR_PM_ALIST_SP           0x0270   /* live, PM context 0 */
#define CR_PM_MLIST_SP           0x02C0

static inline uint64_t rd64(const uint8_t *p, uint32_t off)
{
	return *(const uint64_t *)(p + off);
}

static inline uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

/*
 * B. Load the HWRTData into PM context 0 (kick-geometry-ta.md B, answers to
 * Q16h-Q16j and Q29b): context select, free lists and CONTEXT_PB_BASE, the
 * PM-MMU invalidation, MLIST base and catalogues, VHeap, then on a first kick
 * the TA cache command 0x0258 and the PM reset pulses 0x0290 / 0x0198 (render
 * 2 on the second HWRTData stalls without them). A non-first kick of an
 * HWRTData that another one evicted reloads its saved lists and SPs instead.
 */
static void geom_pm_load(fwaddr_t hwrt_fw, uint8_t *hwrt, bool first)
{
	bool ok;

	cr_write32(CR_PM_20F8, 1);
	cr_write64(CR_PM_HEAP_BASE, 0x8000000000ull);
	/* free lists, synchronised with a 3D of another context (Q38) */
	ok = pm_sync_load_freelists(PM_CTX_GEOM, hwrt_fw, hwrt);
	pm_mmu_inval();
	if (first)
		pm_load_bases(PM_CTX_GEOM, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
			      rd64(hwrt, HWRT_VCE_CAT_BASE), rd64(hwrt, HWRT_TE_CAT_BASE),
			      rd64(hwrt, HWRT_ALIST_CAT_BASE));
	else
		pm_load_lists(PM_CTX_GEOM, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
			      rd64(hwrt, HWRT_VCE_LAST_CAT_BASE), rd64(hwrt, HWRT_TE_LAST_CAT_BASE),
			      rd64(hwrt, HWRT_ALIST_LAST_CAT_BASE), rd64(hwrt, HWRT_ALIST_SP),
			      rd32(hwrt, HWRT_MLIST_SP));
	cr_write64(CR_PM_VHEAP_TABLE, rd64(hwrt, HWRT_VHEAP_TABLE));
	if (first) {
		/*
		 * The reference zeroes the TPC and the RTC with the CPU when the
		 * flag is set (after HWR); this firmware cannot map client memory
		 * yet and flushes the SLC instead. The flag stays 1 until TA
		 * finished of a last kick.
		 */
		if (rd32(hwrt, HWRT_GEOM_CACHES_ZEROING))
			slc_flush_inval();
		hw_wr32(hwrt, HWRT_GEOM_CACHES_ZEROING, 1);
		cr_write32(CR_TA_CACHE_INIT_FIRST, 1);
		if (!cr_poll32(CR_TA_CACHE_INIT_FIRST, 1, 0, 10000))
			TRACE(SFID_DBG_2, 0xE0430258u);
		cr_write32(CR_PM_CTX_SEL_A, cr_read32(CR_PM_CTX_SEL_A) & ~0x10000u);
		cr_write32(CR_PM_CTX_SEL_B, cr_read32(CR_PM_CTX_SEL_B) & ~0x100u);
		cr_write32(CR_PM_RESET_A, 1);
		cr_write32(CR_PM_RESET_B, 1);
	}
	pm_slot_set(PM_CTX_GEOM, hwrt_fw);
	TRACE(SFID_RTD_15, hwrt_fw, 0u);
	if (!ok)
		TRACE(SFID_DBG_4, 0xE0410B50u, 0u, 0u);
}

/*
 * Render-target arrays (kick-compute-and-3d.md "Render-target arrays",
 * answer to Q31). At a first kick after a partial render, the per-layer
 * partial-render counters start again.
 */
static void geom_rta_pr_reset(uint8_t *hwrt)
{
	uint32_t f = rd32(hwrt, HWRT_FLAGS);
	uint32_t *npr = fwptr(rd32(hwrt, HWRT_RTA_NUM_PR));

	if (!(f & HWRT_FLAGS_PRIOR_PR))
		return;
	for (uint32_t i = 0; npr && i < rd32(hwrt, HWRT_RTA_MAX_RTS); i++)
		hw_wr32(npr, 4 * i, 0);
	/* a new frame: the renders of the last one's partial renders are done */
	hw_wr32(hwrt, HWRT_FLAGS, f & ~HWRT_FLAGS_PRIOR_PR);
}

/*
 * The layers of the render, at the last kick before the TA starts (answer to
 * Q35): the first `active_render_targets` set bits of the RTC bitmap at
 * rtc_dev_addr + 0x200 (bit k = layer k touched by an earlier kick).
 */
#define RTC_BITMAP_OFF  0x200u
#define RTC_BITMAP_LEN  0x100u

static void geom_rta_fill(uint8_t *hwrt, fwaddr_t memctx, uint64_t rtc)
{
	uint32_t active = rd32(hwrt, HWRT_RTA_ACTIVE);
	uint32_t max_rts = rd32(hwrt, HWRT_RTA_MAX_RTS);
	uint32_t *valid = fwptr(rd32(hwrt, HWRT_RTA_VALID_RTS));
	uint8_t bm[RTC_BITMAP_LEN];
	uint32_t n = 0;

	hw_wr32(hwrt, HWRT_RTA_CTL, 0);
	hw_wr32(hwrt, HWRT_RTA_CURRENT, 0);
	if (!valid || !active)
		return;
	slc_flush_inval();      /* the RTC flush at TA finished may sit in the SLC */
	if (!gpu_read(memctx, rtc + RTC_BITMAP_OFF, bm, sizeof(bm))) {
		/* cannot read it: every layer, in order */
		TRACE(SFID_DBG_4, 0xE0450002u, active, max_rts);
		active = max_rts;
		hw_wr32(hwrt, HWRT_RTA_ACTIVE, active);
		for (n = 0; n < active; n++)
			hw_wr32(valid, 4 * n, n);
	} else {
		uint32_t bits = 0;

		for (uint32_t k = 0; k < 8 * RTC_BITMAP_LEN; k++) {
			if (!(bm[k / 8] & (1u << (k % 8))))
				continue;
			if (n < active)
				hw_wr32(valid, 4 * n++, k);
			bits++;
		}
		if (bits > active)
			TRACE(SFID_DBG_4, 0xE0450005u, bits, active);   /* more layers marked than counted */
		if (n < active) {
			TRACE(SFID_DBG_4, 0xE0450004u, n, active);   /* fewer bits than counted */
			active = n;
			hw_wr32(hwrt, HWRT_RTA_ACTIVE, active);
		}
	}
	if (active)
		hw_wr32(hwrt, HWRT_RTA_CURRENT, hw_rd32(valid, 0));
	TRACE(SFID_MAIN_191, active);
}

/*
 * OOM before a FRAG_PR (Q31 answer, step 3; layered partial renders, Q39):
 * the layers touched so far, so that the partial render runs the layer loop.
 * The spec reads CR[0x0D20], waits for the TA (paused at the OOM) and zeroes
 * rtc_dev_addr + 0x000..0x1FF before the bitmap scan. The RTC is flushed
 * first so that the bitmap is in memory (our addition).
 */
void geom_rta_oom(uint8_t *hwrt, fwaddr_t memctx)
{
	uint64_t rtc = cr_read64(CR_RTC_BASE);

	if (!hwrt || !rtc || rd32(hwrt, HWRT_RTA_MAX_RTS) < 2)
		return;
	hw_wr32(hwrt, HWRT_RTA_ACTIVE, cr_read32(CR_RTA_ACTIVE));
	cr_write32(CR_RTC_CTRL, 2);
	if (!cr_poll32(CR_RTC_CTRL, 2, 0, 10000))
		TRACE(SFID_DBG_2, 0xE0420BB9u);
	if (!gpu_zero(memctx, rtc, RTC_BITMAP_OFF))
		TRACE(SFID_DBG_2, 0xE0450008u);
	geom_rta_fill(hwrt, memctx, rtc);
}

bool kick_geom(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	const uint8_t *cmd = payload;
	const uint8_t *regs = cmd + GEOM_REGS;
	uint32_t flags = rd32(cmd, GEOM_FLAGS);
	fwaddr_t hwrt_fw = rd32(cmd, GEOM_HWRT);
	uint8_t *hwrt = fwptr(hwrt_fw);
	const uint8_t *common = hwrt ? fwptr(rd32(hwrt, HWRT_COMMON_FW_ADDR)) : NULL;
	const uint8_t *gstate = fwptr(ctx->context_state_addr);
	uint32_t slot;

	if (!hwrt || !common || !hwrt_freelist(hwrt, PM_FL_LOCAL))
		return false;   /* cannot run without a render target: retire */

	/* A. preconditions */
	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_GEOM);
	bif_set_index(DM_GEOM, slot);
	/* B. PM */
	{
		bool first = flags & GEOM_FLAGS_FIRSTKICK;

		if (first || !pm_slots[PM_CTX_GEOM].valid || pm_slots[PM_CTX_GEOM].hwrt != hwrt_fw)
			geom_pm_load(hwrt_fw, hwrt, first);
		if (!first) {
			/* mid-TA: the TA cache command of a later kick */
			cr_write32(CR_TA_CACHE_INIT, 1);
			if (!cr_poll32(CR_TA_CACHE_INIT, 1, 0, 10000))
				TRACE(SFID_DBG_2, 0xE0430250u);
		}
	}
	spm_geom_kick(hwrt_fw, ctxaddr, cmd + GEOM_PR_FENCE);

	/* C. geometry registers (observed by the spec, matched by value in E027) */
	cr_write32(CR_PPP_CTRL, rd32(regs, 0x1C));
	cr_write32(CR_TE_PSG, rd32(regs, 0x20));
	cr_write64(CR_TE_PSGREGION_ADDR, rd64(hwrt, HWRT_RGN_HEADER));
	cr_write64(CR_TE_TPC_ADDR, rd64(hwrt, HWRT_TAIL_PTRS));
	cr_write64(CR_TPU_BORDER_VDM, rd64(regs, 0x08));
	cr_write32(CR_PPP_SCREEN, rd32(common, COMMON_SCREEN_PIXEL_MAX));
	cr_write32(CR_TE_SCREEN, rd32(common, COMMON_TE_SCREEN));
	cr_write32(CR_TE_MTILE1, rd32(common, COMMON_TE_MTILE1));
	cr_write32(CR_TE_MTILE2, rd32(common, COMMON_TE_MTILE2));
	cr_write32(CR_TE_MTILE_STRIDE, rd32(common, COMMON_MTILE_STRIDE));
	cr_write32(CR_TE_AA, rd32(common, COMMON_TE_AA));
	/* 0x0C78 keeps its boot default; 0x0C80 = msctl, or the flipped one */
	cr_write64(CR_PPP_MSCTL_SEL, (rd32(common, COMMON_TE_AA) && (flags & GEOM_FLAGS_FLIP)) ?
		   rd64(common, COMMON_MSCTL_FLIPPED) : rd64(common, COMMON_MSCTL));
	/* the spec reads pds_ctrl at cmd + 0x6C, padding in the public layout: observed 0 */
	cr_write32(CR_PDS_CTRL_GEOM, 0);
	if (gstate)
		cr_write64(CR_VDM_CALL_STACK_PTR, rd64(gstate, 8)); /* call_stack_pointer_init */
	cr_write64(CR_VDM_CTRL_STREAM_BASE, rd64(regs, 0x00));

	/*
	 * D. VDM context-state base (static render-context state, E028), the
	 * RTC (kick-geometry-ta.md E, answer to Q16n) and the TE core selection.
	 */
	{
		const uint8_t *st = (const uint8_t *)fwptr(ctxaddr) + 304;
		uint64_t rtc = rd64(hwrt, HWRT_RTC);
		bool first = flags & GEOM_FLAGS_FIRSTKICK;

		cr_write64(CR_VDM_CTX_STATE_BASE, rd64(st, 0));
		cr_write64(CR_RTC_BASE, rtc);
		if (rtc) {
			/* render-target arrays: the TA marks the layers it touches (Q31) */
			if (first) {
				hw_wr32(hwrt, HWRT_RTA_ACTIVE, 0);
				geom_rta_pr_reset(hwrt);
			}
			if (flags & GEOM_FLAGS_LASTKICK)
				geom_rta_fill(hwrt, ctx->fw_mem_context_fw_addr, rtc);
			cr_write32(CR_TE_TPC_STRIDE, rd32(common, COMMON_TPC_STRIDE));
			cr_write32(CR_GEOM_CE0, 0);
		} else {
			if (first)
				geom_rta_pr_reset(hwrt);
			hw_wr32(hwrt, HWRT_RTA_ACTIVE, first ? 0 : 1);
			hw_wr32(hwrt, HWRT_RTA_CTL, 0);
			hw_wr32(hwrt, HWRT_RTA_CURRENT, 0);
			cr_write32(CR_GEOM_CE0, first ? 1 : 3);
		}
		cr_write32(CR_RTC_CTRL, 1);
		cr_write32(CR_RTA_ACTIVE, rd32(hwrt, HWRT_RTA_ACTIVE));
	}
	cr_write32(CR_TE_CORE_SEL, 0);          /* primary core 0 */

	/*
	 * E. first kick: enable the context; 0x0C68 is self-clearing once the
	 * region headers are initialised (kick-geometry-ta.md E).
	 */
	if (flags & GEOM_FLAGS_FIRSTKICK) {
		cr_write32(CR_GEOM_CTX_ENABLE, 1);
		if (!cr_poll32(CR_GEOM_CTX_ENABLE, 1, 0, 10000))
			TRACE(SFID_DBG_2, 0xE028E000u);
	}

	/* F. trace, state, watchdog */
	hw_wr32(hwrt, HWRT_STATE, (flags & GEOM_FLAGS_FIRSTKICK) ?
		RTDATA_STATE_KICK_GEOM_FIRST : RTDATA_STATE_KICK_GEOM);
	hw_wr32(hwrt, HWRT_OWNER_GEOM, ctxaddr);
	TRACE(SFID_MAIN_119, ctxaddr, cmd_off, hwrt_fw, !!(flags & GEOM_FLAGS_FIRSTKICK),
	      !!(flags & GEOM_FLAGS_LASTKICK), 0, ctx->pid, ctx->priority, rd32(cmd, 0),
	      hdr->ext_job_ref, hdr->int_job_ref);
	cr_write32(CR_EVENT_CLEAR, EV_TA_FINISHED);
	dm_start(DM_GEOM, ctxaddr, cmd_off, EV_TA_FINISHED, SFID_MAIN_8, slot);
	dms[DM_GEOM].hwrt = hwrt_fw;
	dms[DM_GEOM].flags = flags;

	/* G. trigger: last write */
	cr_write32(CR_VDM_KICK, 1);
	return true;
}

/*
 * TA finished (kick-geometry-ta.md "TPC flush, VHEAP store, and completion
 * sequence", answers to Q19b and Q29). Runs before the geometry cache flush.
 * Without the TPC flush the tail pointers stay in the TE and every later
 * render on the device stalls (E041). The free lists themselves are stored
 * at the fragment kick (Q19 answer).
 */
void geom_done(struct dm_state *s)
{
	uint8_t *hwrt = fwptr(s->hwrt);
	bool last = s->flags & GEOM_FLAGS_LASTKICK;
	bool rtc = cr_read64(CR_RTC_BASE) != 0;

	if (!last) {
		cr_write32(CR_VHEAP_STORE, 1);
		if (!cr_poll32(CR_VHEAP_STORE, 1, 0, 10000))
			TRACE(SFID_DBG_2, 0xE0420B60u);
	}
	cr_write32(CR_TE_TPC_FLUSH, 0x40000000u);
	if (rtc) {
		cr_write32(CR_RTC_CTRL, 2);
		if (!cr_poll32(CR_RTC_CTRL, 2, 0, 10000))
			TRACE(SFID_DBG_2, 0xE0420BB8u);
	}
	if (!cr_poll32(CR_TE_TPC_FLUSH, 0x40000000u, 0, 10000))
		TRACE(SFID_DBG_2, 0xE0420C50u);

	if (!hwrt)
		return;
	/* render-target arrays: the hardware count of touched layers (Q31) */
	/*
	 * CR[0x0D20] counts the layers newly marked by this kick (Q35): it gives
	 * active_render_targets at a non-last kick only; the last kick keeps the
	 * count, its layers were listed at its kick.
	 */
	if (rtc) {
		if (!last)
			hw_wr32(hwrt, HWRT_RTA_ACTIVE, cr_read32(CR_RTA_ACTIVE));
	} else if (last && !(rd32(hwrt, HWRT_FLAGS) & HWRT_FLAGS_PRIOR_PR)) {
		hw_wr32(hwrt, HWRT_RTA_ACTIVE, 1);
	}
	if (last)
		hw_wr32(hwrt, HWRT_GEOM_CACHES_ZEROING, 0);
	hw_wr64(hwrt, HWRT_ALIST_SP, cr_read32(CR_PM_ALIST_SP));
	hw_wr32(hwrt, HWRT_MLIST_SP, cr_read32(CR_PM_MLIST_SP));
	hw_wr64(hwrt, HWRT_VCE_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_VCE0));
	hw_wr64(hwrt, HWRT_TE_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_TE0));
	hw_wr64(hwrt, HWRT_ALIST_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_ALIST0));
	hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_GEOM_FINISHED);
}
