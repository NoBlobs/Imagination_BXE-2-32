// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Fragment (3D) kick of a render, and of a partial render (FRAG_PR).
 *
 * Sources: docsClean/kick-compute-and-3d.md ("Fragment kick", "Observed during
 * the render", "Input-attachment renders", "Render-target arrays" for the
 * layer loop, Q27), kick-geometry-ta.md phase B step 3
 * and the OOM/PR register changes (Q16m), partial-render-spm.md, the public
 * rogue_fwif_frag_regs layout; experiments E035-E040.
 */
#include "kick.h"
#include "pm.h"

/* rogue_fwif_cmd_frag (payload offsets, command-structs.md) */
#define FRAG_HWRT                0x04
#define FRAG_REGS                0x10
#define FRAG_FLAGS               0x1D0
#define FRAG_FLAGS_GET_VIS_RESULTS (1u << 5)   /* occlusion query (public) */
#define FRAG_ZLS_STRIDE          0x1D4    /* bytes per layer, render-target arrays */
#define FRAG_SLS_STRIDE          0x1D8
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
#define FR_ISP_OCLQRY_BASE       112
#define FR_ISP_ZLSCTL            120
#define FR_ISP_ZLOAD_STORE_BASE  128
#define FR_ISP_STENCIL_BASE      136
#define FR_PBE_WORD              152    /* [8][3] u64 */
#define FR_TPU_BORDER            344
#define FR_PDS_BGND              352    /* [3] u64 */
#define FR_PDS_PR_BGND           400    /* [3] u64, partial render */
/* rogue_fwif_hwrtdata_common */
#define COMMON_SCREEN_PIXEL_MAX  0x04
#define COMMON_MSCTL             0x08
#define COMMON_MSCTL_FLIPPED     0x10
#define COMMON_MTILE_STRIDE      0x24
#define COMMON_RGN_HEADER_SIZE   0x4C     /* region headers per layer */
#define COMMON_ISP_MTILE_SIZE    0x50

/* PM, fragment side */
#define CR_PM_CONTEXT_PB_BASE    0x02B0
#define CR_PM_ALIST_SP_LIVE0     0x0270
#define CR_PM_MLIST_SP_LIVE0     0x02C0
#define CR_PM_0388               0x0388   /* 1 during a render, a transfer clears it (Q16k) */

/* 3D pipe (kick-compute-and-3d.md "Fragment kick", phases A-F) */
#define CR_FRAG_CTRL_COMMON      0xF310
#define CR_USC_PIXEL_OUTPUT      0x4070
#define CR_USC_CLEAR0            0x4078
#define CR_VIEW_IDX              0x40A0
#define CR_ISP_MTILE_BASE        0x0F20   /* region headers */
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
#define CR_ISP_MSCTL             0x0FA0
#define CR_ISP_MSCTL_FLIPPED     0x0FA8
#define CR_ISP_ZLSCTL            0x0F48
#define CR_ISP_ZLOAD_BASE        0x0F50
#define CR_ISP_ZSTORE_BASE       0x0F58
#define CR_ISP_STENCIL_LOAD_BASE 0x0F60
#define CR_ISP_STENCIL_STORE_BASE 0x0F68
#define CR_TPU_BORDER            0x17A0
#define CR_TPU_BORDER_PIXEL      0x1790   /* same table during the reference's renders (E040) */
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
#define CR_RENDER_TGT_ID         0x03D8   /* USC RENDER_TGT_ID of the end-of-tile program (Q33) */
#define CR_PM_FREE_ON_END        0x03E0   /* 0 on a non-last layer: keep the PM pages (Q33) */
#define CR_ISP_OCLQRY_BASE       0x0FC0   /* per core, 64-bit (Q32) */

/* bytes per region header on this core (simple parameter format v2) */
#define RGN_HEADER_BYTES         6

/*
 * The layers of a render-target-array render are listed by the geometry at
 * TA finished (active_render_targets, valid_render_targets[]; geom.c, Q31).
 */
#ifndef EXP_MEM_FREE_US
#define EXP_MEM_FREE_US          3000     /* the spec saw 0.4-2.5 ms */
#endif

static inline uint64_t rd64(const uint8_t *p, uint32_t off)
{
	return *(const uint64_t *)(p + off);
}

static inline uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

static bool frag_is_pr;         /* the fragment DM runs a partial render */

/* layer loop of a render-target-array render (kick-compute-and-3d.md, Q27) */
static struct {
	bool on;                /* more than one layer: loop at PIXELBE_END_RENDER */
	uint32_t active;
	uint64_t rgn_base, rgn_stride;
	uint64_t zls_base, zls_stride, sls_base, sls_stride;
	uint64_t pbe_w1[8];
	uint8_t pbe_layered;    /* bit n: MRT n is a TWIDDLE_3D target (zslice add) */
} rta;

void frag_hwr_reset(void)
{
	frag_is_pr = false;
	rta.on = false;
}

/* Program the per-layer registers for layer `layer` (steps 1-4). */
static void rta_program(uint32_t layer)
{
	bcast_write64(CR_ISP_MTILE_BASE, rta.rgn_base + rta.rgn_stride * layer);
	bcast_write32(CR_RENDER_TGT_ID, layer);
	for (uint32_t n = 0; n < 8; n++) {
		uint64_t w = rta.pbe_w1[n];
		uint32_t lo = (uint32_t)w;

		if (!(rta.pbe_layered & (1u << n)))
			continue;
		lo = (lo & 0xF0003FFFu) | ((((lo >> 14) & 0x3FFFu) + layer) & 0x3FFFu) << 14;
		bcast_write64(CR_PBE_WORDS + 0x40 + 8 * n, (w & ~0xFFFFFFFFull) | lo);
	}
	if (rta.zls_base) {
		bcast_write64(CR_ISP_ZLOAD_BASE, rta.zls_base + rta.zls_stride * layer);
		bcast_write64(CR_ISP_ZSTORE_BASE, rta.zls_base + rta.zls_stride * layer);
	}
	if (rta.sls_base) {
		bcast_write64(CR_ISP_STENCIL_LOAD_BASE, rta.sls_base + rta.sls_stride * layer);
		bcast_write64(CR_ISP_STENCIL_STORE_BASE, rta.sls_base + rta.sls_stride * layer);
	}
}

/*
 * Set up a render-target-array render (normal render): program layer
 * valid_render_targets[0]. rta.on is true when more layers follow.
 */
static void rta_setup(const uint8_t *cmd, const uint8_t *r, uint8_t *hwrt,
		      const uint8_t *common)
{
	uint32_t max_rts = rd32(hwrt, HWRT_RTA_MAX_RTS);
	uint32_t *valid = fwptr(rd32(hwrt, HWRT_RTA_VALID_RTS));
	uint32_t active = rd32(hwrt, HWRT_RTA_ACTIVE);
	uint32_t layer;

	rta.on = false;
	if (!active || !valid || max_rts < 2) {
		/* one layer: RENDER_TGT_ID = current_render_target, free the PM pages */
		bcast_write32(CR_RENDER_TGT_ID, rd32(hwrt, HWRT_RTA_CURRENT));
		cr_write32(CR_PM_FREE_ON_END, 1);
		return;
	}
	rta.on = active >= 2;
	rta.active = active;
	rta.rgn_base = rd64(hwrt, HWRT_RGN_HEADER);
	rta.rgn_stride = ((uint64_t)rd32(common, COMMON_RGN_HEADER_SIZE) * RGN_HEADER_BYTES +
			  4095u) & ~4095ull;
	rta.zls_base = rd64(r, FR_ISP_ZLOAD_STORE_BASE);
	rta.zls_stride = rd32(cmd, FRAG_ZLS_STRIDE);
	rta.sls_base = rd64(r, FR_ISP_STENCIL_BASE);
	rta.sls_stride = rd32(cmd, FRAG_SLS_STRIDE);
	rta.pbe_layered = 0;
	for (uint32_t n = 0; n < 8; n++) {
		uint64_t w0 = rd64(r, FR_PBE_WORD + 24 * n);

		rta.pbe_w1[n] = rd64(r, FR_PBE_WORD + 24 * n + 8);
		if (((uint32_t)(w0 >> 32) & 3) == 2)
			rta.pbe_layered |= 1u << n;
	}
	layer = hw_rd32(valid, 0);
	hw_wr32(hwrt, HWRT_RTA_CTL, 0);
	hw_wr32(hwrt, HWRT_RTA_CURRENT, layer);
	TRACE(SFID_MAIN_39, layer);
	rta_program(layer);
	cr_write32(CR_PM_FREE_ON_END, rta.on ? 0 : 1);
}

/*
 * PIXELBE_END_RENDER of a render-target-array render (step 4): kick the next
 * layer before the cache flush and the retire. True if a layer was kicked.
 */
bool frag_rta_next(struct dm_state *s)
{
	uint8_t *hwrt = fwptr(s->hwrt);
	uint32_t idx;
	const uint32_t *valid;

	if (!rta.on || frag_is_pr || !hwrt)
		return false;
	idx = rd32(hwrt, HWRT_RTA_CTL);
	if (idx + 1 >= rta.active) {
		/* last layer done; active_render_targets stays until the next first kick */
		rta.on = false;
		hw_wr32(hwrt, HWRT_RTA_CTL, 0);
		return false;
	}
	valid = fwptr(rd32(hwrt, HWRT_RTA_VALID_RTS));
	idx++;
	hw_wr32(hwrt, HWRT_RTA_CTL, idx);
	hw_wr32(hwrt, HWRT_RTA_CURRENT, hw_rd32(valid, 4 * idx));
	TRACE(SFID_MAIN_40, idx + 1, rta.active);
	rta_program(hw_rd32(valid, 4 * idx));
	cr_write32(CR_PM_FREE_ON_END, idx + 1 == rta.active ? 1 : 0);
	cr_write32(CR_EVENT_CLEAR, EV_PIXELBE_END_RENDER);
	s->kick_time = gpu_timer();
	bcast_write32(CR_PIXEL_LAUNCH, 1);
	return true;
}

/*
 * Phase B step 3 of a normal render: store the free lists from PM context 0
 * (the state the geometry left), then load them, the geometry's lists and its
 * catalogues into PM context 1.
 */
static void frag_pm_load(fwaddr_t hwrt_fw, const uint8_t *hwrt)
{
	struct pm_freelist *fl[2] = {
		hwrt_freelist(hwrt, PM_FL_LOCAL), hwrt_freelist(hwrt, PM_FL_GLOBAL),
	};
	bool ok[2] = { true, true };

	/* the geometry's state is still in PM context 0 only if it holds this HWRTData */
	if (pm_slots[PM_CTX_GEOM].dirty && pm_slots[PM_CTX_GEOM].hwrt == hwrt_fw) {
		for (uint32_t w = 0; w < 2; w++)
			if (fl[w])
				pm_store_freelist(PM_CTX_GEOM, w, fl[w]);
		pm_slot_stored(PM_CTX_GEOM);
	}
	pm_slot_evict(PM_CTX_FRAG, hwrt_fw);

	pm_select(PM_CTX_FRAG);
	pm_mmu_inval();
	pm_load_lists(PM_CTX_FRAG, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
		      rd64(hwrt, HWRT_VCE_LAST_CAT_BASE), rd64(hwrt, HWRT_TE_LAST_CAT_BASE),
		      rd64(hwrt, HWRT_ALIST_LAST_CAT_BASE), rd64(hwrt, HWRT_ALIST_SP),
		      rd32(hwrt, HWRT_MLIST_SP));
	for (uint32_t w = 0; w < 2; w++)
		if (fl[w])
			ok[w] = pm_load_freelist(PM_CTX_FRAG, w, fl[w]);
	cr_write32(CR_PM_CONTEXT_PB_BASE, 0);   /* same free lists as the TA */
	pm_slot_set(PM_CTX_FRAG, hwrt_fw);
	TRACE(SFID_RTD_12, 1u, 1u, (uint32_t)rd32(hwrt, HWRT_MLIST_SP),
	      (uint32_t)(rd64(hwrt, HWRT_ALIST_SP) >> 32), (uint32_t)rd64(hwrt, HWRT_ALIST_SP));
	if (!ok[0] || !ok[1])
		TRACE(SFID_DBG_4, 0xE0410B60u, ok[0], ok[1]);
}

/*
 * Partial render (Q16m answer): context 1 takes the geometry's *current*
 * catalogues and list stack pointers; the free lists are shared, not
 * reloaded; then PM_PARTIAL_RENDER_ENABLE = 1.
 */
static void frag_pm_load_pr(const uint8_t *hwrt)
{
	pm_select(PM_CTX_FRAG);
	pm_mmu_inval();
	pm_load_lists(PM_CTX_FRAG, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
		      cr_read64(CR_BIF_PM_CAT_BASE_VCE0), cr_read64(CR_BIF_PM_CAT_BASE_TE0),
		      cr_read64(CR_BIF_PM_CAT_BASE_ALIST0), cr_read32(CR_PM_ALIST_SP_LIVE0),
		      cr_read32(CR_PM_MLIST_SP_LIVE0));
	cr_write32(CR_PM_PARTIAL_RENDER_ENABLE, 1);
}

bool kick_frag(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off,
	       bool partial)
{
	const uint8_t *cmd = payload;
	const uint8_t *r = cmd + FRAG_REGS;
	fwaddr_t hwrt_fw = rd32(cmd, FRAG_HWRT);
	uint8_t *hwrt = fwptr(hwrt_fw);
	const uint8_t *common = hwrt ? fwptr(rd32(hwrt, HWRT_COMMON_FW_ADDR)) : NULL;
	uint32_t slot, isp_ctl, tif;

	if (!hwrt || !common || !hwrt_freelist(hwrt, PM_FL_LOCAL))
		return false;

	/* A. preconditions */
	bcast_write32(CR_FRAG_CTRL_COMMON, 1);          /* single core */
	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_FRAG);
	bif_set_index(DM_FRAG, slot);
	if (partial)
		frag_pm_load_pr(hwrt);
	else
		frag_pm_load(hwrt_fw, hwrt);
	bcast_write32(CR_PM_0388, 1);

	/* B. ISP configuration */
	bcast_write32(CR_USC_PIXEL_OUTPUT, rd32(r, FR_USC_PIXEL_OUTPUT));
	for (uint32_t i = 0; i < 4; i++)
		bcast_write32(CR_USC_CLEAR0 + 8 * i, rd32(r, FR_USC_CLEAR + 4 * i));
	bcast_write32(CR_VIEW_IDX, rd32(r, FR_VIEW_IDX) & 0xFF);
	bcast_write64(CR_ISP_MTILE_BASE, rd64(hwrt, HWRT_RGN_HEADER));
	bcast_write32(CR_FRAG_SCREEN, rd32(common, COMMON_SCREEN_PIXEL_MAX));
	/* isp_mtile_size; mtile_stride in both halves breaks every render (E039) */
	bcast_write32(CR_ISP_MTILE_SIZE, rd32(common, COMMON_ISP_MTILE_SIZE));
	bcast_write32(CR_ISP_RGN, 4 * rd32(common, COMMON_MTILE_STRIDE));   /* Q16l */

	/* C. ZS load/store (none in a partial render), background, border */
	bcast_write64(CR_ISP_SCISSOR_BASE, rd64(r, FR_ISP_SCISSOR_BASE));
	bcast_write64(CR_ISP_DBIAS_BASE, rd64(r, FR_ISP_DBIAS_BASE));
	bcast_write32(CR_ISP_BGOBJDEPTH, rd32(r, FR_ISP_BGOBJDEPTH));
	bcast_write32(CR_ISP_BGOBJVALS, rd32(r, FR_ISP_BGOBJVALS));
	bcast_write32(CR_ISP_AA, rd32(r, FR_ISP_AA));
	bcast_write64(CR_ISP_MSCTL, rd64(common, COMMON_MSCTL));
	bcast_write64(CR_ISP_MSCTL_FLIPPED, rd64(common, COMMON_MSCTL_FLIPPED));
	bcast_write64(CR_ISP_ZLSCTL, partial ? 0 : rd64(r, FR_ISP_ZLSCTL));
	bcast_write64(CR_ISP_ZLOAD_BASE, partial ? 0 : rd64(r, FR_ISP_ZLOAD_STORE_BASE));
	bcast_write64(CR_ISP_ZSTORE_BASE, partial ? 0 : rd64(r, FR_ISP_ZLOAD_STORE_BASE));
	bcast_write64(CR_ISP_STENCIL_LOAD_BASE, partial ? 0 : rd64(r, FR_ISP_STENCIL_BASE));
	bcast_write64(CR_ISP_STENCIL_STORE_BASE, partial ? 0 : rd64(r, FR_ISP_STENCIL_BASE));
	/* occlusion query: the count goes to isp_oclqry_base (Q32, one core) */
	if (rd32(cmd, FRAG_FLAGS) & FRAG_FLAGS_GET_VIS_RESULTS)
		cr_write64(CR_ISP_OCLQRY_BASE, rd64(r, FR_ISP_OCLQRY_BASE));
	bcast_write64(CR_TPU_BORDER, rd64(r, FR_TPU_BORDER));
	bcast_write64(CR_TPU_BORDER_PIXEL, rd64(r, FR_TPU_BORDER));
	/* not in the spec's 3D list: as the transfer kick does */
	isp_ctl = rd32(r, FR_ISP_CTL);
	tif = ((isp_ctl >> 12) & 0xF) + 1;
	bcast_write32(CR_ISP_TILES_IN_FLIGHT, tif > 2 ? 2 : tif);
	bcast_write32(CR_ISP_CTL, isp_ctl);
	{
		uint32_t bg = partial ? FR_PDS_PR_BGND : FR_PDS_BGND;

		bcast_write64(CR_PDS_BGRND0_BASE, rd64(r, bg));
		bcast_write64(CR_PDS_BGRND1_BASE, rd64(r, bg + 8));
		bcast_write64(CR_PDS_BGRND3_SIZEINFO, rd64(r, bg + 16));
	}

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
	/*
	 * render-target arrays: the first layer's registers. A partial render
	 * stays on one layer (the spec's per-layer partial renders are not done).
	 */
	if (partial) {
		rta.on = false;
		bcast_write32(CR_RENDER_TGT_ID, rd32(hwrt, HWRT_RTA_CURRENT));
		cr_write32(CR_PM_FREE_ON_END, 1);
	} else {
		rta_setup(cmd, r, hwrt, common);
	}

	/* E. trace and state, F. trigger */
	TRACE(SFID_MAIN_120, ctxaddr, cmd_off, hwrt_fw, partial ? 1u : 0u, 0u, ctx->pid,
	      ctx->priority, rd32(cmd, 0), hdr->ext_job_ref, hdr->int_job_ref);
	if (!partial)
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_KICK_FRAG);
	else
		spm_pr_started();
	frag_is_pr = partial;
	cr_write32(CR_EVENT_CLEAR, EV_PIXELBE_END_RENDER | EV_PM_3D_MEM_FREE);
	dm_start(DM_FRAG, ctxaddr, cmd_off, EV_PIXELBE_END_RENDER, SFID_MAIN_4, slot);
	dms[DM_FRAG].hwrt = hwrt_fw;
	bcast_write32(CR_PIXEL_LAUNCH, 1);              /* last write */
	cr_write32(CR_MTS_SCHEDULE, 0);
	return true;
}

/*
 * Render or partial-render completion. The PM returns the pages by itself
 * and raises PM_3D_MEM_FREE after PIXELBE_END_RENDER (Q16k/Q16l); in a small
 * render both arrive together, so the IRQ task may already have seen it (E038).
 */
void frag_done(struct dm_state *s)
{
	const uint8_t *hwrt = fwptr(s->hwrt);
	bool freed = cr_read32(CR_DBG_IRQ_LASTEV) & EV_PM_3D_MEM_FREE;

	for (uint32_t us = 0; !freed && us < EXP_MEM_FREE_US; us += 10) {
		if ((freed = cr_read32(CR_EVENT_STATUS) & EV_PM_3D_MEM_FREE))
			break;
		udelay(10);
	}
	cr_write32(CR_EVENT_CLEAR, EV_PM_3D_MEM_FREE);
	if (!freed)
		TRACE(SFID_DBG_2, 0xE0410D01u);
	pm_mmu_inval();         /* every 3D finished (kick-geometry-ta.md B step 3) */

	if (frag_is_pr) {
		frag_is_pr = false;
		spm_pr_done();
		s->keep = true;         /* FRAG_PR may run again in this frame */
		if (hwrt)
			TRACE(SFID_MAIN_2, RTDATA_STATE_PARTIALRENDERFINISHED, 0u);
		s->hwrt = 0;
		return;
	}
	if (!hwrt)
		return;
	for (uint32_t w = 0; w < 2; w++) {
		struct pm_freelist *fl = hwrt_freelist(hwrt, w);

		if (fl)
			pm_store_freelist(PM_CTX_FRAG, w, fl);
	}
	pm_slot_stored(PM_CTX_FRAG);
	hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_FRAG_FINISHED);
	TRACE(SFID_MAIN_2, RTDATA_STATE_FRAG_FINISHED, 0u);
	s->hwrt = 0;
}
