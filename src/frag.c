// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Fragment (3D) kick of a render and of a partial render (``FRAG_PR``),
 * the layer loop of render-target arrays, and the render's completion.
 *
 * Sources: ``docsClean`` ``kick-compute-and-3d.md`` ("Fragment kick",
 * "Observed during the render", "Input-attachment renders", "Render-target
 * arrays" for the layer loop, Q27), ``kick-geometry-ta.md`` phase B step 3
 * and the OOM/PR register changes (Q16m), ``partial-render-spm.md``, the
 * public ``rogue_fwif_frag_regs`` layout; experiments E035-E040.
 */
/**/
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
#define CR_PM_PR_CTRL            0x01C0   /* 1 on FRAG_PR, 0 on a render (Q30) */
#define CR_3D_06D0               0x06D0   /* 0x1F, 0x3F on a Z/S partial render, 0x7F (Q30) */
#define ISP_CTL_BIT17            (1u << 17)

/* rogue_fwif_cmd_frag flags (public ROGUE_FRAG_FLAGS_*, and Q30 answer) */
#define FRAG_FLAGS_DEPTH         0x80u
#define FRAG_FLAGS_STENCIL       0x100u
#define FRAG_FLAGS_8000          0x8000u
#define FRAG_FLAGS_80000         0x80000u
#define FRAG_FLAGS_100000        0x100000u

/* bytes per region header on this core (simple parameter format v2) */
#define RGN_HEADER_BYTES         6

/*
 * The layers of a render-target-array render are listed by the geometry at
 * TA finished (active_render_targets, valid_render_targets[]; geom.c, Q31).
 */
#ifndef EXP_MEM_FREE_US
/**
 * How long :c:func:`frag_done` waits for ``PM_3D_MEM_FREE``, in microseconds
 * (the specification saw 0.4 to 2.5 ms). An experiment knob.
 */
#define EXP_MEM_FREE_US          3000     /* the spec saw 0.4-2.5 ms */
#endif

/* rd64() / rd32(): as in geom.c (documented there). */
static inline uint64_t rd64(const uint8_t *p, uint32_t off)
{
	return *(const uint64_t *)(p + off);
}

static inline uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

/** The fragment data master runs a partial render. */
static bool frag_is_pr;         /* the fragment DM runs a partial render */

/**
 * The layer loop of a render-target-array render (``kick-compute-and-3d.md``,
 * Q27): the layer count, and the region-header, depth/stencil and PBE bases
 * with their per-layer strides.
 */
static struct {
	bool on;                /* more than one layer: loop at PIXELBE_END_RENDER */
	uint32_t active;
	uint64_t rgn_base, rgn_stride;
	uint64_t zls_base, zls_stride, sls_base, sls_stride;
	uint64_t pbe_w1[8];
	uint8_t pbe_layered;    /* bit n: MRT n is a TWIDDLE_3D target (zslice add) */
} rta;

/**
 * The command's fields that depend on ``has_pr``, kept for every layer
 * (``partial-render-spm.md`` "Multi-PR kick state", Q30; layered
 * ``FRAG_PR``, Q39). They are written at the kick and again on every layer of
 * a render-target-array render by :c:func:`frag_layer_state`.
 */
static struct {
	bool pr;                /* this render is a FRAG_PR */
	bool prior_pr;          /* hwrtdata.flags & 8 at the kick */
	bool zs_unbacked;       /* FRAG_PR whose on-demand ZS buffer is not backed (Q41) */
	uint32_t *npr;          /* rta_num_partial_renders[], or NULL */
	uint32_t flags, isp_ctl, bgobjvals, bgobjdepth;
	uint64_t zlsctl, bg[3], bg_pr[3];
} ks;

/**
 * The 3D context switch in progress: a store was requested, and the stored
 * context yields the 3D to ``yield_to`` until that context has kicked.
 */
static struct {
	bool req;
	fwaddr_t stored, yield_to;
} csw;

static bool frag_csw_resume(fwaddr_t ctxaddr, const struct fwcommoncontext *ctx);

/** Forget a running partial render, layer loop and 3D context switch (after a hardware recovery). */
void frag_hwr_reset(void)
{
	frag_is_pr = false;
	rta.on = false;
	csw.req = false;
	csw.stored = 0;
	csw.yield_to = 0;
}

/**
 * Write the state that depends on the layer's partial renders.
 *
 * ``has_pr`` is true when an earlier partial render of this frame stored the
 * layer (``HWRT_FLAGS_PRIOR_PR`` and a non-zero per-layer counter):
 *
 * - every ``FRAG_PR`` forces the depth/stencil store, and ``0x06D0`` becomes
 *   ``0x3F`` with depth or stencil;
 * - with ``has_pr`` the depth/stencil load is forced, the partial-render
 *   background program ``pds_pr_bgnd`` is used, ``isp_bgobjvals`` gets bit 9,
 *   and ``ISP_CTL`` bit 17 is cleared unless the flags keep it;
 * - a first ``FRAG_PR`` whose background differs sets ``ISP_CTL`` bit 17;
 * - flag ``0x8000`` makes ``0x06D0`` ``0x7F``.
 *
 * Writes ZLS control, the background object, ``ISP_CTL`` and ``0x06D0``. A
 * ``FRAG_PR`` then counts against the layer (Q39).
 *
 * Args:
 *     layer: The render target (layer) index.
 */
static void frag_layer_state(uint32_t layer)
{
	bool has_pr = ks.prior_pr && (!ks.npr || hw_rd32(ks.npr, 4 * layer) > 0);
	uint32_t zlo = (uint32_t)ks.zlsctl, ctl = ks.isp_ctl, vals = ks.bgobjvals;
	const uint64_t *bg = has_pr ? ks.bg_pr : ks.bg;
	uint32_t v06d0 = 0x1F;

	if (ks.pr && !ks.zs_unbacked) { /* forced Z/S store on every FRAG_PR */
		if (ks.flags & FRAG_FLAGS_DEPTH)
			zlo |= 0x80004u;
		if (ks.flags & FRAG_FLAGS_STENCIL)
			zlo |= 0x40004u;
		if (ks.flags & (FRAG_FLAGS_DEPTH | FRAG_FLAGS_STENCIL))
			v06d0 = 0x3F;
	}
	if (has_pr) {           /* 2nd+ FRAG_PR and the final render: load what was stored */
		if ((ks.flags & FRAG_FLAGS_DEPTH) && !(ks.pr && ks.zs_unbacked))
			zlo |= 0x8002u;
		if ((ks.flags & FRAG_FLAGS_STENCIL) && !(ks.pr && ks.zs_unbacked))
			zlo |= 0x4002u;
		vals |= 0x200u;
		if (!(ks.flags & FRAG_FLAGS_100000) &&
		    !(!ks.pr && (ks.flags & FRAG_FLAGS_80000)))
			ctl &= ~ISP_CTL_BIT17;
	} else if (ks.pr && (ks.bg[0] != ks.bg_pr[0] || ks.bg[1] != ks.bg_pr[1] ||
			     ks.bg[2] != ks.bg_pr[2] || (ks.flags & FRAG_FLAGS_80000))) {
		ctl |= ISP_CTL_BIT17;
	}
	if (ks.flags & FRAG_FLAGS_8000)
		v06d0 = 0x7F;
	bcast_write64(CR_ISP_ZLSCTL, (ks.zlsctl & ~0xFFFFFFFFull) | zlo);
	bcast_write64(CR_PDS_BGRND0_BASE, bg[0]);
	bcast_write64(CR_PDS_BGRND1_BASE, bg[1]);
	bcast_write64(CR_PDS_BGRND3_SIZEINFO, bg[2]);
	bcast_write32(CR_ISP_BGOBJDEPTH, ks.bgobjdepth);
	bcast_write32(CR_ISP_BGOBJVALS, vals);
	bcast_write32(CR_ISP_CTL, ctl);
	bcast_write32(CR_3D_06D0, v06d0);
	/* each FRAG_PR kick counts against its layer (Q39) */
	if (ks.pr && ks.npr)
		hw_wr32(ks.npr, 4 * layer, hw_rd32(ks.npr, 4 * layer) + 1);
}

/**
 * Program the per-layer registers of a render-target-array render (steps
 * 1-4).
 *
 * The region-header base, ``RENDER_TGT_ID`` (read by the end-of-tile
 * program), the z-slice of each 3D-twiddled PBE target, and the depth and
 * stencil load/store bases.
 *
 * Args:
 *     layer: The render target (layer) index.
 */
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

/**
 * Set up a render-target-array render, normal or partial.
 *
 * With one layer (or no array) only ``RENDER_TGT_ID`` is written
 * (``current_render_target``) and the PM pages are freed at the end. With
 * several layers the bases and strides are kept in ``rta``, the first layer
 * of ``valid_render_targets[]`` is programmed, and the PM pages are kept
 * until the last layer. ``rta.on`` is set when more layers follow.
 *
 * Args:
 *     cmd: The fragment command.
 *     r: The command's ``rogue_fwif_frag_regs``.
 *     hwrt: The HWRTData.
 *     common: The HWRTData's common part.
 *
 * Returns:
 *     The layer programmed.
 */
static uint32_t rta_setup(const uint8_t *cmd, const uint8_t *r, uint8_t *hwrt,
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
		return rd32(hwrt, HWRT_RTA_CURRENT);
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
	return layer;
}

/**
 * Move a render-target-array render to its next layer (step 6, Q39).
 *
 * Called at ``PIXELBE_END_RENDER``, before the cache flush and the retire
 * (and, for a partial render, before it is reported executed). Programs the
 * next layer of ``valid_render_targets[]``, writes its partial-render state
 * again (Q36, Q39) and launches the pixel pipe. After the last layer the loop
 * ends; ``active_render_targets`` stays until the next first kick.
 *
 * Args:
 *     s: The fragment data master's state.
 *
 * Returns:
 *     ``true`` if a layer was kicked, ``false`` when the render is complete.
 */
bool frag_rta_next(struct dm_state *s)
{
	uint8_t *hwrt = fwptr(s->hwrt);
	uint32_t idx;
	const uint32_t *valid;

	if (!rta.on || !hwrt)
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
	/* the background object and ZLS control again for this layer (Q36, Q39) */
	frag_layer_state(hw_rd32(valid, 4 * idx));
	bcast_write32(CR_ISP_RENDER_ORIGIN, 0);
	cr_write32(CR_PM_FREE_ON_END, idx + 1 == rta.active ? 1 : 0);
	cr_write32(CR_EVENT_CLEAR, EV_PIXELBE_END_RENDER);
	s->kick_time = gpu_timer();
	bcast_write32(CR_PIXEL_LAUNCH, 1);
	return true;
}

/**
 * Phase B step 3 of a render: give the HWRTData to PM context 1.
 *
 * If PM context 0 still holds this HWRTData, its free lists are stored and
 * it stops being tracked there (the same-HWRTData handoff, Q38). The free
 * lists are then loaded into context 1, synchronised with a geometry of
 * another context, followed by the geometry's saved catalogues and stack
 * pointers.
 *
 * Args:
 *     hwrt_fw: Firmware address of the HWRTData.
 *     hwrt: The HWRTData.
 */
static void frag_pm_load(fwaddr_t hwrt_fw, const uint8_t *hwrt)
{
	struct pm_freelist *fl[2] = {
		hwrt_freelist(hwrt, PM_FL_LOCAL), hwrt_freelist(hwrt, PM_FL_GLOBAL),
	};
	bool ok;

	/*
	 * Same-HWRTData handoff (Q38): the geometry's state is still in PM
	 * context 0 only if it holds this HWRTData; store it and stop tracking it
	 * there (outside SPM).
	 */
	if (pm_slots[PM_CTX_GEOM].valid && pm_slots[PM_CTX_GEOM].hwrt == hwrt_fw) {
		if (pm_slots[PM_CTX_GEOM].dirty)
			for (uint32_t w = 0; w < 2; w++)
				if (fl[w])
					pm_store_freelist(PM_CTX_GEOM, w, fl[w]);
		pm_slot_stored(PM_CTX_GEOM);
		pm_slots[PM_CTX_GEOM].valid = false;
	}
	/* free lists, synchronised with a geometry of another context (Q38) */
	ok = pm_sync_load_freelists(PM_CTX_FRAG, hwrt_fw, hwrt);
	pm_mmu_inval();
	pm_load_lists(PM_CTX_FRAG, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR),
		      rd64(hwrt, HWRT_VCE_LAST_CAT_BASE), rd64(hwrt, HWRT_TE_LAST_CAT_BASE),
		      rd64(hwrt, HWRT_ALIST_LAST_CAT_BASE), rd64(hwrt, HWRT_ALIST_SP),
		      rd32(hwrt, HWRT_MLIST_SP));
	pm_slot_set(PM_CTX_FRAG, hwrt_fw);
	TRACE(SFID_RTD_12, 1u, 1u, (uint32_t)rd32(hwrt, HWRT_MLIST_SP),
	      (uint32_t)(rd64(hwrt, HWRT_ALIST_SP) >> 32), (uint32_t)rd64(hwrt, HWRT_ALIST_SP));
	if (!ok)
		TRACE(SFID_DBG_4, 0xE0410B60u, 0u, 0u);
}

/**
 * Load PM context 1 for a partial render (Q16m answer).
 *
 * Context 1 takes the geometry's *current* catalogues and list stack
 * pointers from the live registers; the free lists are shared, not reloaded.
 * Then ``PM_PARTIAL_RENDER_ENABLE`` is set.
 *
 * Args:
 *     hwrt: The HWRTData.
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

/**
 * Kick a fragment command: a render or a partial render.
 *
 * The phases of ``kick-compute-and-3d.md`` "Fragment kick":
 *
 * - **A.** Single-core control, BIF slot, the PM load (:c:func:`frag_pm_load`
 *   or :c:func:`frag_pm_load_pr`).
 * - **B.** ISP configuration: USC output and clears, view index, region
 *   headers, screen and macrotile layout.
 * - **C.** The command's partial-render state, scissor and depth-bias bases,
 *   anti-aliasing and multisample control, depth/stencil bases (the
 *   command's, in a partial render too: Q30), the occlusion query base
 *   (Q32), border colours, tiles in flight.
 * - **D.** Pipe reset, end-of-render PDS program, PBE words, the
 *   partial-render control ``0x01C0``, the first layer
 *   (:c:func:`rta_setup`) and its partial-render state.
 * - **E.** HWRTData state (or the SPM's partial-render start) and trace.
 * - **F.** Launch the pixel pipe (the last write).
 *
 * Args:
 *     ctxaddr: Firmware address of the render context.
 *     ctx: The render context.
 *     hdr: The command header.
 *     payload: The ``rogue_fwif_cmd_frag`` command.
 *     cmd_off: Offset of the command in the client CCB.
 *     partial: This is a ``FRAG_PR`` armed by the SPM.
 *
 * Returns:
 *     ``true`` when the render runs; ``false`` when the command has no
 *     HWRTData, common data or local free list (it is retired without
 *     running).
 */
bool kick_frag(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off,
	       bool partial)
{
	const uint8_t *cmd = payload;
	const uint8_t *r = cmd + FRAG_REGS;
	fwaddr_t hwrt_fw = rd32(cmd, FRAG_HWRT);
	uint8_t *hwrt = fwptr(hwrt_fw);
	const uint8_t *common = hwrt ? fwptr(rd32(hwrt, HWRT_COMMON_FW_ADDR)) : NULL;
	uint32_t slot, isp_ctl, tif, layer;
	bool resume;

	if (!hwrt || !common || !hwrt_freelist(hwrt, PM_FL_LOCAL))
		return false;
	/* its geometry was discarded by an HWR: retire without touching the GPU (Q42) */
	if (hwrt_discard(DM_FRAG, ctxaddr, ctx, hwrt_fw, cmd_off))
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
	bcast_write32(CR_ISP_RGN, rd32(common, COMMON_RGN_HEADER_SIZE));   /* 19b96b7: rgn_header_size */

	/*
	 * C. ZS bases (the command's, also in a partial render: Q30), background,
	 * border. ZLS control, background and ISP_CTL depend on has_pr and are
	 * written with the first layer (frag_layer_state).
	 */
	ks.pr = partial;
	ks.zs_unbacked = partial && spm_pr_zs_unbacked();
	ks.prior_pr = rd32(hwrt, HWRT_FLAGS) & HWRT_FLAGS_PRIOR_PR;
	ks.npr = fwptr(rd32(hwrt, HWRT_RTA_NUM_PR));
	ks.flags = rd32(cmd, FRAG_FLAGS);
	ks.isp_ctl = rd32(r, FR_ISP_CTL);
	ks.bgobjvals = rd32(r, FR_ISP_BGOBJVALS);
	ks.bgobjdepth = rd32(r, FR_ISP_BGOBJDEPTH);
	ks.zlsctl = rd64(r, FR_ISP_ZLSCTL);
	for (uint32_t i = 0; i < 3; i++) {
		ks.bg[i] = rd64(r, FR_PDS_BGND + 8 * i);
		ks.bg_pr[i] = rd64(r, FR_PDS_PR_BGND + 8 * i);
	}
	bcast_write64(CR_ISP_SCISSOR_BASE, rd64(r, FR_ISP_SCISSOR_BASE));
	bcast_write64(CR_ISP_DBIAS_BASE, rd64(r, FR_ISP_DBIAS_BASE));
	bcast_write32(CR_ISP_AA, rd32(r, FR_ISP_AA));
	bcast_write64(CR_ISP_MSCTL, rd64(common, COMMON_MSCTL));
	bcast_write64(CR_ISP_MSCTL_FLIPPED, rd64(common, COMMON_MSCTL_FLIPPED));
	bcast_write64(CR_ISP_ZLOAD_BASE, rd64(r, FR_ISP_ZLOAD_STORE_BASE));
	bcast_write64(CR_ISP_ZSTORE_BASE, rd64(r, FR_ISP_ZLOAD_STORE_BASE));
	bcast_write64(CR_ISP_STENCIL_LOAD_BASE, rd64(r, FR_ISP_STENCIL_BASE));
	bcast_write64(CR_ISP_STENCIL_STORE_BASE, rd64(r, FR_ISP_STENCIL_BASE));
	/* occlusion query: the count goes to isp_oclqry_base (Q32, one core) */
	if (rd32(cmd, FRAG_FLAGS) & FRAG_FLAGS_GET_VIS_RESULTS)
		cr_write64(CR_ISP_OCLQRY_BASE, rd64(r, FR_ISP_OCLQRY_BASE));
	bcast_write64(CR_TPU_BORDER, rd64(r, FR_TPU_BORDER));
	bcast_write64(CR_TPU_BORDER_PIXEL, rd64(r, FR_TPU_BORDER));
	/* not in the spec's 3D list: as the transfer kick does */
	isp_ctl = rd32(r, FR_ISP_CTL);
	tif = ((isp_ctl >> 12) & 0xF) + 1;
	bcast_write32(CR_ISP_TILES_IN_FLIGHT, tif > 2 ? 2 : tif);

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
	cr_write32(CR_PM_PR_CTRL, partial ? 1 : 0);
	/*
	 * render-target arrays: the first layer's registers, for a render and
	 * for a partial render alike (Q39); then the has_pr-dependent state.
	 */
	layer = rta_setup(cmd, r, hwrt, common);
	frag_layer_state(layer);
	/* 0x0298 / 0x01C8, and the resume of a stored render (Q40) */
	resume = frag_csw_resume(ctxaddr, ctx);

	/* E. trace and state, F. trigger */
	TRACE(SFID_MAIN_120, ctxaddr, cmd_off, hwrt_fw, partial ? 1u : 0u, resume ? 1u : 0u, ctx->pid,
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
	if (resume)
		ctx->fw_com_ctx_flags &= ~0xFFu;        /* resumed (Q40 item 3) */
	cr_write32(CR_MTS_SCHEDULE, 0);
	return true;
}

/* ------------------------------------------------ 3D context store (Q40) */
/* rogue_fwif_frag_ctx_state (public), the render context's 3D state buffer */
#define FCS_PM_DEALLOC_MASK      0x00     /* frag_reg_pm_deallocated_mask_status */
#define FCS_PDS_MTILEFREE        0x04     /* frag_reg_dm_pds_mtilefree_status */
#define FCS_ISP_STORE            0x0C     /* frag_reg_isp_store[core] */
/* registers of the store and resume protocol (scheduler.md, answer to Q40) */
#define CR_ISP_CTXSW_REQ         0x1000   /* BCAST: 1 = store the running 3D */
#define CR_ISP_STORE0            0x1008   /* per core: the stored ISP position */
#define CR_ISP_RESUME0           0x1020   /* per core: the position to resume from */
#define CR_ISP_CTXSW_STATUS      0x1038   /* bit 0: the render finished before storing */
#define CR_PM_3D_STORE_STATUS    0x03F8   /* 0xC2 when the PM 3D store is idle */
#define CR_PM_DEALLOC_MASK       0x0360
#define CR_PDS_MTILEFREE         0x0370
#define ISP_RENDER_RESUME        0x10u
#define EV_ISP_STORED            0x8u     /* CR_EVENT_STATUS bit seen with a store */
#define CTX_FLAGS_NEED_RESUME    0x1u     /* ROGUE_FWIF_CONTEXT_FLAGS_NEED_RESUME (core 0) */
#define OS_CTXSWITCH_FRAG_EN     (1u << 2) /* ROGUE_FWIF_INICFG_OS_CTXSWITCH_FRAG_EN */

/**
 * Whether the 3D may be preempted: the host enables it with
 * ``ROGUE_FWIF_INICFG_OS_CTXSWITCH_FRAG_EN`` in ``osdata.fw_os_config_flags``
 * (the mainline host leaves it at 0). ``EXP_CSW_FORCE`` enables it for tests.
 */
static bool csw_enabled(void)
{
#ifdef EXP_CSW_FORCE
	return true;
#else
	return g.osdata && (g.osdata->fw_os_config_flags & OS_CTXSWITCH_FRAG_EN);
#endif
}

/**
 * Ask the running 3D to store its context, for a higher-priority 3D.
 *
 * Called by the scheduler when a context's task (a ``FRAG``, ``FRAG_PR`` or
 * ``TQ_3D``) waits for the busy 3D (``scheduler.md`` "Preemption"). The store is requested
 * with ``BCAST[0x1000] = 1``, only while ``(ISP_RENDER & 3) != 3`` and not
 * during a partial render (answer to Q40). A render-target-array render with
 * layers left is not stored: the resume of a layer loop is not specified.
 *
 * Args:
 *     new_ctx: Firmware address of the waiting context.
 */
void frag_csw_request(fwaddr_t new_ctx)
{
	const struct fwcommoncontext *run = fwptr(dms[DM_FRAG].ctx);
	const struct fwcommoncontext *nc = fwptr(new_ctx);

	if (!csw_enabled() || csw.req || frag_is_pr || rta.on || !dms[DM_FRAG].hwrt ||
	    !run || !nc || nc->priority <= run->priority)
		return;
	TRACE(SFID_CSW_38, DM_FRAG, 0u, 0u, run->priority, 0u, 0u, nc->priority, 0u);
	if ((cr_read32(CR_ISP_RENDER) & 3) == 3)
		return;         /* too late: the render is finishing */
	bcast_write32(CR_ISP_CTXSW_REQ, 1);
	csw.req = true;
	csw.yield_to = new_ctx;
	TRACE(SFID_CSW_11);
}

/**
 * Finish a 3D whose store was requested, at ``PIXELBE_END_RENDER``.
 *
 * Answer to Q40, items 1, 2 and 4. The render was stored when
 * ``CR_EVENT_STATUS`` bit 3 is set and ``CR[0x1038]`` bit 0 is clear;
 * with ``CR[0x1038]`` bit 0 set it finished normally, the saved
 * deallocation state is zeroed and the normal completion follows.
 *
 * A stored render saves the ISP position and, once the PM 3D store is idle
 * (``CR[0x03F8] & 0xC2``), the PM deallocation state into the context's
 * ``rogue_fwif_frag_ctx_state``; the context needs resume (``CSW_7``). The
 * HWRTData becomes ``3DCONTEXTSTORED``, the PM MMU is invalidated, and the
 * command stays at the head of its queue (``keep``). The caller flushes the
 * 3D caches and releases the data master; ``PM_3D_MEM_FREE``, the layer loop
 * and the retire are skipped.
 *
 * Args:
 *     s: The fragment data master's state.
 *     ev: The ``CR_EVENT_STATUS`` bits that ended the render (the caller
 *         has already cleared them in the register).
 *
 * Returns:
 *     ``true`` when the render was stored.
 */
bool frag_csw_end(struct dm_state *s, uint32_t ev)
{
	struct fwcommoncontext *ctx = fwptr(s->ctx);
	uint8_t *fcs;
	uint8_t *hwrt = fwptr(s->hwrt);
	uint32_t sw;

	if (!csw.req)
		return false;
	csw.req = false;
	if (!ctx)
		return false;
	ctx->fw_com_ctx_flags &= ~0xFFu;
	fcs = fwptr(ctx->context_state_addr);
	ev |= cr_read32(CR_EVENT_STATUS);       /* the caller already cleared bit 3 */
	sw = cr_read32(CR_ISP_CTXSW_STATUS);
	TRACE(SFID_CSW_10, ev, sw, cr_read32(CR_ISP_STORE0));
	if (!(ev & EV_ISP_STORED) || (sw & 1)) {
		/* the request came too late: a normal end */
		if (fcs) {
			hw_wr32(fcs, FCS_PM_DEALLOC_MASK, 0);
			hw_wr32(fcs, FCS_PDS_MTILEFREE, 0);
		}
		csw.yield_to = 0;
		return false;
	}
	if (fcs)
		hw_wr32(fcs, FCS_ISP_STORE, cr_read32(CR_ISP_STORE0));
	if (!cr_poll32(CR_PM_3D_STORE_STATUS, 0xC2u, 0xC2u, 10000)) {
		bcast_write32(CR_EVENT_CLEAR, 0x4108u);
		TRACE(SFID_DBG_2, 0xE0560001u);
	}
	if (fcs) {
		hw_wr32(fcs, FCS_PM_DEALLOC_MASK, cr_read32(CR_PM_DEALLOC_MASK));
		hw_wr32(fcs, FCS_PDS_MTILEFREE, cr_read32(CR_PDS_MTILEFREE));
	}
	ctx->fw_com_ctx_flags |= CTX_FLAGS_NEED_RESUME;
	TRACE(SFID_CSW_9);
	TRACE(SFID_CSW_7, s->ctx);
	if (hwrt) {
		hw_wr32(hwrt, HWRT_STATE, RTDATA_STATE_FRAG_CONTEXT_STORED);
		TRACE(SFID_MAIN_2, RTDATA_STATE_FRAG_CONTEXT_STORED, 0u);
	}
	pm_mmu_inval();
	bcast_write32(CR_EVENT_CLEAR, (ev & 0x4000u) ? 0x4008u : 0x8u);
	csw.stored = s->ctx;
	s->keep = true;         /* the FRAG stays at the head: it is resumed later */
	s->hwrt = 0;
	return true;
}

/**
 * The context a stored 3D context must let run first.
 *
 * Args:
 *     ctxaddr: Firmware address of a context about to kick its 3D.
 *
 * Returns:
 *     The higher-priority context it yields to, or 0.
 */
fwaddr_t frag_csw_yield_to(fwaddr_t ctxaddr)
{
	return csw.stored == ctxaddr ? csw.yield_to : 0;
}

/** Stop yielding: the higher-priority 3D ran, or no longer waits. */
void frag_csw_yield_done(void)
{
	csw.stored = 0;
	csw.yield_to = 0;
}

/**
 * Resume writes of a 3D kick (answer to Q40, item 3), after the layer state
 * and before the pixel launch.
 *
 * A normal kick zeroes ``0x0298`` and ``0x01C8``. A kick of a stored context
 * gives the ISP its stored position (``CR[0x1020]``), sets ``ISP_RENDER``
 * bit 4 and restores the PM deallocation state.
 *
 * Args:
 *     ctxaddr: Firmware address of the context.
 *     ctx: The context.
 *
 * Returns:
 *     ``true`` when the kick resumes a stored render.
 */
static bool frag_csw_resume(fwaddr_t ctxaddr, const struct fwcommoncontext *ctx)
{
	const uint8_t *fcs = fwptr(ctx->context_state_addr);

	if (ctxaddr == csw.yield_to)
		frag_csw_yield_done();  /* the higher-priority 3D runs now */
	if (!(ctx->fw_com_ctx_flags & 0xFFu) || !fcs) {
		bcast_write32(CR_3D_0298, 0);
		bcast_write32(CR_3D_01C8, 0);
		return false;
	}
	TRACE(SFID_CSW_8, ctxaddr);
	cr_write32(CR_ISP_RESUME0, hw_rd32(fcs, FCS_ISP_STORE));
	bcast_write32(CR_ISP_RENDER, cr_read32(CR_ISP_RENDER) | ISP_RENDER_RESUME);
	cr_write32(CR_3D_0298, hw_rd32(fcs, FCS_PM_DEALLOC_MASK));
	cr_write32(CR_3D_01C8, hw_rd32(fcs, FCS_PDS_MTILEFREE));
	return true;
}

/**
 * Finish a render or a partial render.
 *
 * The PM returns the pages by itself and raises ``PM_3D_MEM_FREE`` after
 * ``PIXELBE_END_RENDER`` (Q16k/Q16l); in a small render both arrive together,
 * so the interrupt task may already have seen it (E038). It is awaited for
 * at most :c:macro:`EXP_MEM_FREE_US`, then the PM MMU is invalidated.
 *
 * - A partial render sets ``HWRT_FLAGS_PRIOR_PR`` (the next partial renders
 *   and the final render load what it stored), tells the SPM, and keeps the
 *   ``FRAG_PR`` queued for the next one.
 * - A render stores the free lists from PM context 1 and sets the
 *   ``FRAG_FINISHED`` state.
 *
 * Args:
 *     s: The fragment data master's state.
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
		/* the next partial renders and the final render load what this one stored */
		if (hwrt)
			hw_wr32((uint8_t *)hwrt, HWRT_FLAGS,
				hw_rd32(hwrt, HWRT_FLAGS) | HWRT_FLAGS_PRIOR_PR);
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
