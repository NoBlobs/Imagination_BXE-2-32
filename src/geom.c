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
#define COMMON_TE_SCREEN         0x20
#define COMMON_MTILE_STRIDE      0x24
#define COMMON_TE_MTILE1         0x2C
#define COMMON_TE_MTILE2         0x30

/* rogue_fwif_cmd_geom (payload offsets, command-structs.md) */
#define GEOM_HWRT                0x04
#define GEOM_REGS                0x10
#define GEOM_FLAGS               0x50
#define GEOM_PR_FENCE            0x54   /* partial_render_geom_frag_fence (rogue_fwif_ufo) */
#define GEOM_FLAGS_FIRSTKICK     (1u << 0)
#define GEOM_FLAGS_LASTKICK      (1u << 1)

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
#define CR_PPP_MSCTL             0x0C78
#define CR_PPP_MSCTL_FLIPPED     0x0C80
#define CR_PPP_CTRL              0x0C88
#define CR_PPP_SCREEN            0x0C98
#define CR_TA_CTX_STATE_BASE     0x0CB0
#define CR_GEOM_CE0              0x0CE0
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
 * B. Load the HWRTData into PM context 0, at a first kick only (a kick with
 * First kick:0 continues the PM state, Q16m).  Order observed at a first
 * load: 0x20F8, 0x0240, context selection, local free list, global free
 * list, CONTEXT_PB_BASE (Q16h-Q16j).
 */
static void geom_pm_load(fwaddr_t hwrt_fw, const uint8_t *hwrt)
{
	struct pm_freelist *fl_local = hwrt_freelist(hwrt, PM_FL_LOCAL);
	struct pm_freelist *fl_global = hwrt_freelist(hwrt, PM_FL_GLOBAL);
	uint32_t pb = 3;
	bool ok_l = true, ok_g = true;

#ifdef EXP_SHARED_PB
	/*
	 * CONTEXT_PB_BASE bit 0 / bit 1 = the local / global free list differs
	 * from the one PM context 1 holds; equal ones are shared and not loaded
	 * (Q16m).  Off by default: it hung the board in E040.
	 */
	if (pm_slots[PM_CTX_FRAG].valid)
		pb = (rd32(hwrt, HWRT_FREELISTS) != pm_slots[PM_CTX_FRAG].fl[0]) |
		     ((rd32(hwrt, HWRT_FREELISTS + 4) != pm_slots[PM_CTX_FRAG].fl[1]) << 1);
#endif
	pm_slot_evict(PM_CTX_GEOM, hwrt_fw);
	cr_write32(CR_PM_20F8, 1);
	cr_write64(CR_PM_HEAP_BASE, 0x8000000000ull);
	pm_select(PM_CTX_GEOM);
	cr_write64(CR_PM_VHEAP_TABLE, rd64(hwrt, HWRT_VHEAP_TABLE));
	/* catalogues and list stack pointers start from 0 at a first kick */
	pm_load_lists(PM_CTX_GEOM, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
		      rd64(hwrt, HWRT_VCE_CAT_BASE), rd64(hwrt, HWRT_TE_CAT_BASE),
		      rd64(hwrt, HWRT_ALIST_CAT_BASE), 0, 0);
	if ((pb & 1) && fl_local)
		ok_l = pm_load_freelist(PM_CTX_GEOM, PM_FL_LOCAL, fl_local);
	if ((pb & 2) && fl_global)
		ok_g = pm_load_freelist(PM_CTX_GEOM, PM_FL_GLOBAL, fl_global);
	cr_write32(CR_PM_CONTEXT_PB_BASE, pb);
	pm_slot_set(PM_CTX_GEOM, hwrt_fw);
	TRACE(SFID_RTD_3, pb, pb & 1, (pb >> 1) & 1, 0u);
	TRACE(SFID_RTD_15, hwrt_fw, 0u);
	if (!ok_l || !ok_g)
		TRACE(SFID_DBG_4, 0xE0410B50u, ok_l, ok_g);
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
	if (rd32(hwrt, HWRT_GEOM_CACHES_ZEROING)) {
		/* after HWR (kick-geometry-ta.md phase D): the cache clear registers
		 * are not in the spec; flush the SLC so no stale state is reused */
		slc_flush_inval();
		hw_wr32(hwrt, HWRT_GEOM_CACHES_ZEROING, 0);
	}

	/* B. PM */
	if ((flags & GEOM_FLAGS_FIRSTKICK) || !pm_slots[PM_CTX_GEOM].valid ||
	    pm_slots[PM_CTX_GEOM].hwrt != hwrt_fw)
		geom_pm_load(hwrt_fw, hwrt);
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
	cr_write64(CR_PPP_MSCTL, rd64(common, COMMON_MSCTL));
	cr_write64(CR_PPP_MSCTL_FLIPPED, rd64(common, COMMON_MSCTL_FLIPPED));
	if (gstate)
		cr_write64(CR_VDM_CALL_STACK_PTR, rd64(gstate, 8)); /* call_stack_pointer_init */
	cr_write64(CR_VDM_CTRL_STREAM_BASE, rd64(regs, 0x00));

	/*
	 * D. context state bases (static render-context state, core 0, E028)
	 * and the TE core selection.
	 */
	{
		const uint8_t *st = (const uint8_t *)fwptr(ctxaddr) + 304;

		cr_write64(CR_VDM_CTX_STATE_BASE, rd64(st, 0));
		cr_write64(CR_TA_CTX_STATE_BASE, rd64(st, 16));
	}
	cr_write32(CR_TE_CORE_SEL, 0);          /* primary core 0 */
	cr_write32(CR_GEOM_CE0, 1);             /* new (non-resumed) kick */

	/*
	 * E. enable the context: 0x0C68 is 1 for <= 20 us at every kick and back
	 * to 0 before the trigger (Q16l); wait for the return to 0 (E038).
	 */
	cr_write32(CR_GEOM_CTX_ENABLE, 1);
	if (!cr_poll32(CR_GEOM_CTX_ENABLE, 1, 0, 10000))
		TRACE(SFID_DBG_2, 0xE028E000u);

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

	/* G. trigger: last write */
	cr_write32(CR_VDM_KICK, 1);
	return true;
}

/*
 * TA finished: record the PM state the geometry reached in the HWRTData.
 * The free lists themselves are stored at the fragment kick (Q19 answer).
 */
void geom_done(struct dm_state *s)
{
	uint8_t *hwrt = fwptr(s->hwrt);

	if (!hwrt)
		return;
	hw_wr64(hwrt, HWRT_ALIST_SP, cr_read32(CR_PM_ALIST_SP));
	hw_wr32(hwrt, HWRT_MLIST_SP, cr_read32(CR_PM_MLIST_SP));
	hw_wr64(hwrt, HWRT_VCE_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_VCE0));
	hw_wr64(hwrt, HWRT_TE_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_TE0));
	hw_wr64(hwrt, HWRT_ALIST_LAST_CAT_BASE, cr_read64(CR_BIF_PM_CAT_BASE_ALIST0));
	hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_GEOM_FINISHED);
}
