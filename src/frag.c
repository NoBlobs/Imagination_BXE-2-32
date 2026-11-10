// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Fragment (3D) kick of a render: docsClean/kick-compute-and-3d.md "Fragment
 * kick" plus the PM context 1 load of docsClean/kick-geometry-ta.md phase B
 * step 3, with the free-list load commands found in E035.  Experimental:
 * reached only with -DEXP_TA (E036).
 */
#include "kick.h"

/* rogue_fwif_cmd_frag (public layout) */
#define FRAG_HWRT                4      /* cmd_shared.hwrt_data_fw_addr */
#define FRAG_REGS                16
#define FRAG_FLAGS               464
/* rogue_fwif_frag_regs, relative to FRAG_REGS */
#define FR_USC_PIXEL_OUTPUT      0
#define FR_USC_CLEAR             4
#define FR_ISP_BGOBJDEPTH        36
#define FR_ISP_BGOBJVALS         40
#define FR_ISP_AA                44
#define FR_ISP_CTL               52
#define FR_EVENT_PDS_INFO        60
#define FR_VIEW_IDX              68
#define FR_EVENT_PDS_DATA        72
#define FR_ISP_SCISSOR_BASE      96
#define FR_ISP_DBIAS_BASE        104
#define FR_PBE_WORD              152    /* [8][3] u64 */
#define FR_TPU_BORDER            344
#define FR_PDS_BGND              352    /* [3] u64 */

/* rogue_fwif_hwrtdata / _common / freelist (public layouts) */
#define HWRT_PM_MLIST_DEV_ADDR   0
#define HWRT_VCE_LAST_CAT_BASE   40
#define HWRT_TE_LAST_CAT_BASE    104
#define HWRT_ALIST_LAST_CAT_BASE 144
#define HWRT_ALIST_SP            152
#define HWRT_MLIST_SP            160
#define HWRT_COMMON_FW_ADDR      164
#define HWRT_STATE               172
#define HWRT_FREELISTS           176
#define HWRT_RGN_HEADER          256
#define COMMON_SCREEN_PIXEL_MAX  4
#define COMMON_RGN_HEADER_SIZE   76
#define COMMON_ISP_MTILE_SIZE    80
#define FL_CURRENT_DEV_ADDR      8
#define FL_STACK_TOP             16
#define FL_ALLOC_PAGES           32
#define FL_ALLOC_MMU_PAGES       36

#define RTDATA_STATE_KICK_FRAG       4
#define RTDATA_STATE_FRAG_FINISHED   5

/* PM context 1 (fragment) */
#define CR_PM_FL_LOAD_LOCAL1     0x0200
#define CR_PM_FL_BASE_LOCAL1     0x0218
#define CR_PM_FL_TOP_LOCAL1      0x0230  /* 64-bit, stack top from bit 22 (E035) */
#define CR_PM_FL_CNT_LOCAL1      0x0398
#define CR_PM_FL_LOAD_GLOBAL1    0x20A8
#define CR_PM_FL_BANK_GLOBAL1    0x2078
#define CR_PM_LIVE_SP_LOCAL0     0x0348
#define CR_PM_LIVE_PB_LOCAL0     0x03A0  /* live local counts (Q16k): PB ctx0 / ctx1 0x03A8 */
#define CR_PM_LIVE_PB_LOCAL1     0x03A8
#define CR_PM_LIVE_MMU_LOCAL0    0x2000  /* MMU ctx0 / ctx1 0x2008 */
#define CR_PM_LIVE_MMU_LOCAL1    0x2008
#define EV_PM_3D_MEM_FREE        (1u << 8)
#ifndef EXP_MEM_FREE_US
#define EXP_MEM_FREE_US          3000
#endif
#define CR_PM_LIVE_SP_LOCAL1     0x0350
#define CR_PM_LIVE_SP_GLOBAL0    0x20C8
#define CR_PM_LIVE_SP_GLOBAL1    0x2088
#define CR_PM_LIVE_ALLOC_GLOBAL0 0x20D8  /* +0x20 / +0x28 of the bank */
#define CR_PM_LIVE_ALLOC_GLOBAL1 0x2098
#define CR_PM_CONTEXT_PB_BASE    0x02B0
#define CR_PM_MLIST1_BASE        0x02E0
#define CR_PM_ALIST_SP_LOAD      0x0278
#define CR_PM_MLIST_SP_LOAD      0x02C8
#define CR_BIF_PM_CAT_BASE_VCE1  0x1268
#define CR_BIF_PM_CAT_BASE_TE1   0x1270
#define CR_BIF_PM_CAT_BASE_ALIST1 0x1280

/* 3D pipe (kick-compute-and-3d.md "Fragment kick", phases A-F) */
#define CR_FRAG_CTRL_COMMON      0xF310
#define CR_USC_PIXEL_OUTPUT      0x4070
#define CR_USC_CLEAR0            0x4078
#define CR_VIEW_IDX              0x40A0
#define CR_ISP_MTILE_BASE        0x0F20  /* region headers */
#define CR_FRAG_SCREEN           0x3E40
#define CR_ISP_MTILE_SIZE        0x0F18
#define CR_ISP_RGN               0x0F28
#define CR_ISP_SCISSOR_BASE      0x0FB0
#define CR_ISP_DBIAS_BASE        0x0FB8
#define CR_ISP_BGOBJDEPTH        0x0F80
#define CR_ISP_BGOBJVALS         0x0F88
#define CR_ISP_AA                0x0F30
#define CR_ISP_CTL               0x0F38
#define CR_ISP_TILES_IN_FLIGHT   0x0FD8
#define CR_ZS_PAIR               0x0FA8
#define CR_TPU_BORDER            0x17A0
#define CR_PDS_BGRND0_BASE       0x06A0
#define CR_PDS_BGRND1_BASE       0x06A8
#define CR_PDS_BGRND3_SIZEINFO   0x06B8
#define CR_ISP_RENDER            0x0F08
#define CR_EVENT_PIXEL_PDS_CODE  0x0618
#define CR_EVENT_PIXEL_PDS_DATA  0x0620
#define CR_EVENT_PIXEL_PDS_INFO  0x0628
#define CR_ISP_RENDER_ORIGIN     0x0F10
#define CR_ISP_ZLS_A             0x0FC8
#define CR_ISP_ZLS_B             0x0FCC
#define CR_PBE_WORDS             0x1510
#define CR_3D_0298               0x0298
#define CR_3D_01C8               0x01C8
#define CR_PIXEL_LAUNCH          0x0F00

static inline uint64_t rd64(const uint8_t *p, uint32_t off)
{
	return *(const uint64_t *)(p + off);
}

static inline uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

static inline void wr32(const uint8_t *p, uint32_t off, uint32_t v)
{
	*(uint32_t *)(uintptr_t)(p + off) = v;
}

/* E035: value registers first, then the load command, wait for bit 0 = 0 */
static bool pm_load_cmd(uint32_t reg)
{
	cr_write32(reg, 1);
	return cr_poll32(reg, 1, 0, 10000);
}

/*
 * Phase B step 3: store the free lists from PM context 0 (geometry), then load
 * them into PM context 1 (fragment), with the geometry's lists and catalogues.
 */
static void frag_pm_load(const uint8_t *hwrt, const uint8_t *fl_local, const uint8_t *fl_global)
{
	bool ok_l, ok_g = true;

	/* store from PM context 0: the stack tops the geometry left */
	wr32(fl_local, FL_STACK_TOP, cr_read32(CR_PM_LIVE_SP_LOCAL0));
	wr32(fl_local, FL_ALLOC_PAGES, cr_read32(CR_PM_LIVE_PB_LOCAL0));
	wr32(fl_local, FL_ALLOC_MMU_PAGES, cr_read32(CR_PM_LIVE_MMU_LOCAL0));
	if (fl_global) {
		wr32(fl_global, FL_STACK_TOP, cr_read32(CR_PM_LIVE_SP_GLOBAL0));
		wr32(fl_global, FL_ALLOC_PAGES, cr_read32(CR_PM_LIVE_ALLOC_GLOBAL0));
		wr32(fl_global, FL_ALLOC_MMU_PAGES, cr_read32(CR_PM_LIVE_ALLOC_GLOBAL0 + 8));
	}

#ifndef EXP_FLAGS_AFTER_SP
	/* fragment-load flags (Q16h): bit 16 / bit 8 */
	cr_write32(0x03D0, cr_read32(0x03D0) | 0x10000u);
	cr_write32(0x03D4, cr_read32(0x03D4) | 0x100u);
#endif

	/*
	 * E037: the geometry's lists, page catalogues and ALIST/MLIST SPs go in
	 * before the load commands; in the original the loaded 0x0278/0x02C8 and
	 * the live 0x0280/0x02D0 change in the same sample (Q16k answer).
	 */
	cr_write64(CR_PM_MLIST1_BASE, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR));
	cr_write64(CR_BIF_PM_CAT_BASE_VCE1, rd64(hwrt, HWRT_VCE_LAST_CAT_BASE));
	cr_write64(CR_BIF_PM_CAT_BASE_TE1, rd64(hwrt, HWRT_TE_LAST_CAT_BASE));
	cr_write64(CR_BIF_PM_CAT_BASE_ALIST1, rd64(hwrt, HWRT_ALIST_LAST_CAT_BASE));
	cr_write64(CR_PM_ALIST_SP_LOAD, rd64(hwrt, HWRT_ALIST_SP));
	cr_write32(CR_PM_MLIST_SP_LOAD, rd32(hwrt, HWRT_MLIST_SP));
#ifndef EXP_SP_LOAD_CMD
#define EXP_SP_LOAD_CMD 0x0288    /* ALIST SP load command (E037) */
#endif
#ifndef EXP_SP_LOAD_CMD2
#define EXP_SP_LOAD_CMD2 0x0190   /* MLIST SP load command (E037) */
#endif
#if EXP_SP_LOAD_CMD
	/* E037: candidate ALIST/MLIST SP load command of PM context 1 (Q16k list) */
	{
		bool ok = pm_load_cmd(EXP_SP_LOAD_CMD);

		TRACE(SFID_DBG_5, 0xE0370B02u, ok, cr_read32(0x0280), cr_read32(0x02D0));
	}
#endif
#if EXP_SP_LOAD_CMD2
	{
		bool ok = pm_load_cmd(EXP_SP_LOAD_CMD2);

		TRACE(SFID_DBG_5, 0xE0370B03u, ok, cr_read32(0x0280), cr_read32(0x02D0));
	}
#endif
#ifdef EXP_FLAGS_AFTER_SP
	/* E037: the flag bits as the trigger of the ALIST/MLIST SP copy */
	cr_write32(0x03D0, cr_read32(0x03D0) | 0x10000u);
	cr_write32(0x03D4, cr_read32(0x03D4) | 0x100u);
	TRACE(SFID_DBG_4, 0xE0370B01u, cr_read32(0x0280), cr_read32(0x02D0));
#endif

	/* local free list, PM context 1 */
	cr_write64(CR_PM_FL_BASE_LOCAL1, rd64(fl_local, FL_CURRENT_DEV_ADDR));
	cr_write64(CR_PM_FL_TOP_LOCAL1, (uint64_t)rd32(fl_local, FL_STACK_TOP) << 22);
	cr_write32(CR_PM_FL_CNT_LOCAL1, rd32(fl_local, FL_ALLOC_PAGES));
	cr_write32(CR_PM_FL_CNT_LOCAL1 + 4, rd32(fl_local, FL_ALLOC_MMU_PAGES));
	ok_l = pm_load_cmd(CR_PM_FL_LOAD_LOCAL1);

	/* global free list, PM context 1 (bank 0x2078) */
	if (fl_global) {
		cr_write64(CR_PM_FL_BANK_GLOBAL1, rd64(fl_global, FL_CURRENT_DEV_ADDR));
		cr_write64(CR_PM_FL_BANK_GLOBAL1 + 0x08, rd32(fl_global, FL_STACK_TOP));
		cr_write32(CR_PM_FL_BANK_GLOBAL1 + 0x18, rd32(fl_global, FL_ALLOC_PAGES));
		cr_write32(CR_PM_FL_BANK_GLOBAL1 + 0x1C, rd32(fl_global, FL_ALLOC_MMU_PAGES));
		ok_g = pm_load_cmd(CR_PM_FL_LOAD_GLOBAL1);
	}
	/* CONTEXT_PB_BASE = 0 at the fragment load (same free lists as the TA) */
	cr_write32(CR_PM_CONTEXT_PB_BASE, 0);


	TRACE(SFID_DBG_8, 0xE0360B00u, ok_l, ok_g, cr_read32(CR_PM_LIVE_SP_LOCAL1),
	      cr_read32(CR_PM_LIVE_SP_GLOBAL1), cr_read32(0x0280), cr_read32(0x02D0));
}

bool kick_frag(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	const uint8_t *cmd = payload;
	const uint8_t *r = cmd + FRAG_REGS;
	const uint8_t *hwrt = fwptr(rd32(cmd, FRAG_HWRT));
	const uint8_t *common = hwrt ? fwptr(rd32(hwrt, HWRT_COMMON_FW_ADDR)) : NULL;
	const uint8_t *fl_local = hwrt ? fwptr(rd32(hwrt, HWRT_FREELISTS)) : NULL;
	const uint8_t *fl_global = hwrt ? fwptr(rd32(hwrt, HWRT_FREELISTS + 4)) : NULL;
	uint32_t slot, isp_ctl, tif;

	if (!hwrt || !common || !fl_local)
		return false;

	/* A. preconditions */
	bcast_write32(CR_FRAG_CTRL_COMMON, 1);          /* single core */
	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_FRAG);
	bif_set_index(DM_FRAG, slot);
	frag_pm_load(hwrt, fl_local, fl_global);
	bcast_write32(0x0388, 1);       /* a transfer clears it; the 3D kick sets it back (Q16k) */

	/* B. ISP configuration */
	bcast_write32(CR_USC_PIXEL_OUTPUT, rd32(r, FR_USC_PIXEL_OUTPUT));
	for (uint32_t i = 0; i < 4; i++)
		bcast_write32(CR_USC_CLEAR0 + 8 * i, rd32(r, FR_USC_CLEAR + 4 * i));
	bcast_write32(CR_VIEW_IDX, rd32(r, FR_VIEW_IDX) & 0xFF);
	bcast_write64(CR_ISP_MTILE_BASE, rd64(hwrt, HWRT_RGN_HEADER));
	bcast_write32(CR_FRAG_SCREEN, rd32(common, COMMON_SCREEN_PIXEL_MAX));
	bcast_write32(CR_ISP_MTILE_SIZE, rd32(common, COMMON_ISP_MTILE_SIZE));
#ifdef EXP_FRAG_RGN
	bcast_write32(CR_ISP_RGN, rd32(common, COMMON_RGN_HEADER_SIZE));
#endif

	/* C. ZS (no load/store yet), background, border */
	bcast_write64(CR_ISP_SCISSOR_BASE, rd64(r, FR_ISP_SCISSOR_BASE));
	bcast_write64(CR_ISP_DBIAS_BASE, rd64(r, FR_ISP_DBIAS_BASE));
	bcast_write32(CR_ISP_BGOBJDEPTH, rd32(r, FR_ISP_BGOBJDEPTH));
	bcast_write32(CR_ISP_BGOBJVALS, rd32(r, FR_ISP_BGOBJVALS));
	bcast_write32(CR_ISP_AA, rd32(r, FR_ISP_AA));
	if (rd32(r, FR_ISP_AA) & 3) {
		bcast_write32(CR_ZS_PAIR, 0x88888888);
		bcast_write32(CR_ZS_PAIR + 4, 0x88888888);
	}
	bcast_write64(CR_TPU_BORDER, rd64(r, FR_TPU_BORDER));
	/* not in the spec's 3D list (PENDING): as the transfer kick does */
	isp_ctl = rd32(r, FR_ISP_CTL);
	tif = ((isp_ctl >> 12) & 0xF) + 1;
	bcast_write32(CR_ISP_TILES_IN_FLIGHT, tif > 2 ? 2 : tif);
	bcast_write32(CR_ISP_CTL, isp_ctl);
	bcast_write64(CR_PDS_BGRND0_BASE, rd64(r, FR_PDS_BGND));
	bcast_write64(CR_PDS_BGRND1_BASE, rd64(r, FR_PDS_BGND + 8));
	bcast_write64(CR_PDS_BGRND3_SIZEINFO, rd64(r, FR_PDS_BGND + 16));

	/* D. pipe reset and output */
	bcast_write32(CR_ISP_RENDER, 0);
	bcast_write32(CR_EVENT_PIXEL_PDS_CODE, 0x80);
	bcast_write32(CR_EVENT_PIXEL_PDS_DATA, rd32(r, FR_EVENT_PDS_DATA));
	bcast_write32(CR_EVENT_PIXEL_PDS_INFO, rd32(r, FR_EVENT_PDS_INFO));
	bcast_write32(CR_ISP_RENDER_ORIGIN, 0);
	bcast_write32(CR_ISP_ZLS_A, 0);
	bcast_write32(CR_ISP_ZLS_B, 0);
	for (uint32_t n = 0; n < 8; n++) {
		bcast_write64(CR_PBE_WORDS + 8 * n, rd64(r, FR_PBE_WORD + 24 * n));
		bcast_write64(CR_PBE_WORDS + 0x40 + 8 * n, rd64(r, FR_PBE_WORD + 24 * n + 8));
	}
	bcast_write32(CR_3D_0298, 0);
	bcast_write32(CR_3D_01C8, 0);

	TRACE(SFID_DBG_8, 0xE0360C00u, rd32(r, FR_EVENT_PDS_INFO), rd32(r, FR_EVENT_PDS_DATA),
	      isp_ctl, rd32(r, FR_ISP_AA), rd32(common, COMMON_ISP_MTILE_SIZE),
	      rd32(cmd, FRAG_FLAGS));

	/* E. trace and state, F. trigger */
	TRACE(SFID_MAIN_120, ctxaddr, cmd_off, rd32(cmd, FRAG_HWRT), 0, 0, ctx->pid, ctx->priority,
	      rd32(cmd, 0), hdr->ext_job_ref, hdr->int_job_ref);
	wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_FRAG);
	cr_write32(CR_EVENT_CLEAR, EV_PIXELBE_END_RENDER);
	dm_start(DM_FRAG, ctxaddr, cmd_off, EV_PIXELBE_END_RENDER, SFID_MAIN_4, slot);
	dms[DM_FRAG].hwrt = rd32(cmd, FRAG_HWRT);
#ifdef EXP_FRAG_ISP_RENDER
	bcast_write32(CR_ISP_RENDER, EXP_FRAG_ISP_RENDER);
#endif
	bcast_write32(CR_PIXEL_LAUNCH, 1);              /* last write */
	cr_write32(CR_MTS_SCHEDULE, 0);
	return true;
}

/* Fragment completion: store the free lists back from PM context 1. */
void frag_done(struct dm_state *s)
{
	/* the PM returns the pages by itself, then raises PM_3D_MEM_FREE (Q16k) */
	bool freed = false;

	/* up to ~3 ms (the spec saw 0.4-2.5 ms); bit 8 is not seen yet (E037) */
	for (uint32_t us = 0; us < EXP_MEM_FREE_US; us += 10) {
		if ((freed = cr_read32(CR_EVENT_STATUS) & EV_PM_3D_MEM_FREE))
			break;
		udelay(10);
	}

	cr_write32(CR_EVENT_CLEAR, EV_PM_3D_MEM_FREE);
	TRACE(SFID_DBG_3, 0xE0370D01u, freed);
	const uint8_t *hwrt = fwptr(s->hwrt);
	const uint8_t *fl_local = hwrt ? fwptr(rd32(hwrt, HWRT_FREELISTS)) : NULL;
	const uint8_t *fl_global = hwrt ? fwptr(rd32(hwrt, HWRT_FREELISTS + 4)) : NULL;

	TRACE(SFID_DBG_6, 0xE0360D00u, cr_read32(CR_PM_LIVE_SP_LOCAL1),
	      cr_read32(CR_PM_LIVE_SP_GLOBAL1), cr_read32(CR_PM_LIVE_ALLOC_GLOBAL1),
	      cr_read32(0x0D00));
	if (!hwrt)
		return;
	if (fl_local) {
		wr32(fl_local, FL_STACK_TOP, cr_read32(CR_PM_LIVE_SP_LOCAL1));
		wr32(fl_local, FL_ALLOC_PAGES, cr_read32(CR_PM_LIVE_PB_LOCAL1));
		wr32(fl_local, FL_ALLOC_MMU_PAGES, cr_read32(CR_PM_LIVE_MMU_LOCAL1));
	}
	if (fl_global) {
		wr32(fl_global, FL_STACK_TOP, cr_read32(CR_PM_LIVE_SP_GLOBAL1));
		wr32(fl_global, FL_ALLOC_PAGES, cr_read32(CR_PM_LIVE_ALLOC_GLOBAL1));
		wr32(fl_global, FL_ALLOC_MMU_PAGES, cr_read32(CR_PM_LIVE_ALLOC_GLOBAL1 + 8));
	}
	wr32(hwrt, HWRT_STATE, RTDATA_STATE_FRAG_FINISHED);
	s->hwrt = 0;
}
