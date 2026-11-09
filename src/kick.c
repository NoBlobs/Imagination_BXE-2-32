// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Data-master kicks and completion handling (see docsClean/data-masters.md,
 * kick-compute-and-3d.md, kick-geometry-ta.md).
 */
#include "kick.h"

/* Registers without a public name, from the spec. */
#define CR_CDM_RUN                  0x0478
#define CR_CDM_CTRL_STREAM_BASE     0x0480
#define CR_CDM_CONTEXT_STATE_BASE   0x0498
#define CR_CDM_CONTEXT_PDS0         0x04A8
#define CR_CDM_CONTEXT_PDS1         0x04B0
#define CR_CDM_TERMINATE_PDS        0x04B8
#define CR_CDM_TERMINATE_PDS1       0x04C0
#define CR_TPU_BORDER_COLOUR_CDM    0x17A8
#define CR_MULTICORE_COMPUTE_CTRL   0xF330

#define SLC_FLUSH_INVAL_ALL         0x1u
#define SLC_STATUS_FLUSH_PENDING    0x4u

/* ------------------------------------------------------------ BIF */
#define BIF_SLOTS 8
static uint64_t bif_pc[BIF_SLOTS];
static uint32_t bif_next = 1;

static bool bif_slot_in_use(uint32_t slot)
{
	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy && dms[d].bif_slot == slot)
			return true;
	return false;
}

/* Bind a memory context to a BIF catalogue slot (1..7); returns the slot. */
static uint32_t bif_activate(fwaddr_t memctx_addr, uint32_t dm)
{
	struct fwmemcontext *mc = fwptr(memctx_addr);
	uint64_t pc = mc ? (mc->pc_dev_paddr & 0xFFFFFFF000ull) : 0;
	uint32_t slot = 0;
	bool newpc = false;

	for (uint32_t s = 1; s < BIF_SLOTS; s++) {
		if (bif_pc[s] == pc) {
			slot = s;
			break;
		}
	}
	if (!slot) {
		for (uint32_t tries = 0; tries < BIF_SLOTS; tries++) {
			uint32_t s = bif_next;

			bif_next = bif_next % (BIF_SLOTS - 1) + 1;
			if (!bif_slot_in_use(s)) {
				slot = s;
				break;
			}
		}
		if (!slot)
			slot = 1;
		bif_pc[slot] = pc;
		slc_flush_inval();      /* page tables may sit stale in the SLC (E015) */
		cr_write64(CR_BIF_CAT_BASE0 + 8 * slot, pc);
		cr_write32(CR_BIF_CTRL_INVAL, 0xF);
		newpc = true;
	}
	if (mc)
		mc->page_cat_base_reg_set = slot;

	TRACE(SFID_BIF_14, memctx_addr, dm, 0);
	TRACE(SFID_BIF_19, slot, dm, (uint32_t)(pc >> 32), (uint32_t)pc, 0, newpc);
	return slot;
}

static void bif_set_index(uint32_t dm, uint32_t slot)
{
	uint32_t lo = cr_read32(CR_BIF_CAT_BASE_INDEX);
	uint32_t shift;

	switch (dm) {
	case DM_GEOM: shift = 0; break;
	case DM_FRAG: shift = 8; break;
	case DM_CDM:  shift = 16; break;
	case DM_TDM:  shift = 19; break;
	default: return;
	}
	lo = (lo & ~(7u << shift)) | (slot << shift);
	cr_write32(CR_BIF_CAT_BASE_INDEX, lo);
}

uint64_t kick_bif_pc(uint32_t slot)
{
	return slot < BIF_SLOTS && bif_pc[slot] != ~0ull ? bif_pc[slot] : 0;
}

/* After a GPU soft reset: re-program every bound page catalogue. */
void bif_rebuild(void)
{
	for (uint32_t s = 1; s < BIF_SLOTS; s++)
		if (bif_pc[s] != ~0ull)
			cr_write64(CR_BIF_CAT_BASE0 + 8 * s, bif_pc[s]);
	cr_write32(CR_BIF_CTRL_INVAL, 0xF);
}

void kick_init(void)
{
	for (uint32_t s = 0; s < BIF_SLOTS; s++)
		bif_pc[s] = ~0ull;
	bif_next = 1;
}

void slc_flush_inval(void)
{
	cr_write32(CR_SLC_CTRL_FLUSH_INVAL, SLC_FLUSH_INVAL_ALL);
	cr_poll32(CR_SLC_STATUS0, SLC_STATUS_FLUSH_PENDING, 0, 10000);
}

static void dm_start(uint32_t dm, fwaddr_t ctxaddr, uint32_t cmd_off, uint32_t evmask,
		     uint32_t finish_sfid, uint32_t slot)
{
	struct dm_state *s = &dms[dm];

	s->busy = true;
	s->ctx = ctxaddr;
	s->cmd_off = cmd_off;
	s->event_mask = evmask;
	s->finish_sfid = finish_sfid;
	s->kick_time = gpu_timer();
	s->bif_slot = slot;
	cr_write32(CR_SCRATCH15, cr_read32(CR_SCRATCH15) | (1u << dm));
}

/* ------------------------------------------------------------ compute */
static bool kick_cdm(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
		     const struct ccb_cmd_header *hdr, const struct cmd_compute *cmd,
		     uint32_t cmd_off)
{
	struct fwcomputecontext *cctx = (struct fwcomputecontext *)ctx;
	uint32_t slot;

	(void)cr_read32(CR_MULTICORE_SYSTEM);
	bcast_write32(CR_MULTICORE_COMPUTE_CTRL, (cmd->execute_count << 8) | 1);

	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_CDM);
	bif_set_index(DM_CDM, slot);

	bcast_write64(CR_CDM_CTRL_STREAM_BASE, cmd->regs.cdm_ctrl_stream_base);
	bcast_write64(CR_TPU_BORDER_COLOUR_CDM, cmd->regs.tpu_border_colour_table);
	bcast_write64(CR_CDM_CONTEXT_STATE_BASE, cmd->regs.cdm_context_state_base_addr);

	TRACE(SFID_MAIN_122, ctxaddr, cmd_off, ctx->pid, ctx->priority,
	      hdr->ext_job_ref, hdr->int_job_ref);

	bcast_write64(CR_CDM_CONTEXT_PDS0, cctx->ctxswitch_regs.cdm_context_pds0);
	bcast_write32(CR_CDM_CONTEXT_PDS1, (uint32_t)cctx->ctxswitch_regs.cdm_context_pds1);
	bcast_write64(CR_CDM_TERMINATE_PDS, cctx->ctxswitch_regs.cdm_terminate_pds);
	bcast_write32(CR_CDM_TERMINATE_PDS1, (uint32_t)cctx->ctxswitch_regs.cdm_terminate_pds1);

	cr_write32(CR_EVENT_CLEAR, EV_COMPUTE_FINISHED);
	dm_start(DM_CDM, ctxaddr, cmd_off, EV_COMPUTE_FINISHED, SFID_MAIN_6, slot);
	(void)cr_read32(CR_CORE_REVISION);

	g.cdm_kicks++;
	bcast_write32(CR_CDM_RUN, 1);   /* trigger: last write */
	return true;
}

/* ---------------------------------------------------- transfer (TQ_3D) */
/*
 * docsClean/kick-compute-and-3d.md, "Transfer kick".  cmd+N in the spec is
 * relative to the cCCB command header (0x28 bytes), so cmd+0x30 is regs+0.
 */
#define CR_FRAG_CTRL_COMMON     0xF310
#define CR_ISP_BGOBJVALS        0x0F88  /* cmd+0x30 */
#define CR_PDS_BGRND0_BASE      0x06A0  /* layouts match Mesa PDS_BGRND0..3 (E014) */
#define CR_PDS_BGRND1_BASE      0x06A8
#define CR_PDS_BGRND2_BASE      0x06B0  /* not written by the transfer kick (spec) */
#define CR_PDS_BGRND3_SIZEINFO  0x06B8
#define CR_ISP_MTILE_BASE       0x0F20  /* cmd+0x90 */
#define CR_ISP_RGN              0x0F28  /* cmd+0x6C */
#define CR_ISP_ZLS_A            0x0FC8
#define CR_ISP_ZLS_B            0x0FCC
#define CR_USC_PIXEL_OUTPUT     0x4070
#define CR_USC_CLEAR0           0x4078  /* 0x4078/80/88/90 */
#define CR_FRAG_SCREEN          0x3E40  /* cmd+0x70 */
#define CR_ISP_MTILE_SIZE       0x0F18  /* cmd+0x48 */
#define CR_ISP_RENDER_ORIGIN    0x0F10  /* cmd+0x4C */
#define CR_ISP_CTL              0x0F38  /* public */
#define CR_ISP_AA               0x0F30  /* cmd+0x58 */
#define CR_ISP_TILES_IN_FLIGHT  0x0FD8
#define CR_TQ_0388              0x0388  /* no public name, role unknown; 0 per spec */
#define CR_ZS_PAIR              0x0FA8
#define CR_ZS_EXTRA             0x0F48
#define CR_EVENT_PIXEL_PDS_INFO 0x0628  /* cmd+0x5C */
#define CR_EVENT_PIXEL_PDS_CODE 0x0618  /* cmd+0x60 */
#define CR_EVENT_PIXEL_PDS_DATA 0x0620  /* cmd+0x64 */
#define CR_PBE_WORDS            0x1510  /* cmd+0x98 ... */
#define CR_ISP_RENDER           0x0F08  /* public */
#define CR_PIXEL_LAUNCH         0x0F00  /* no public name: pixel-pipe launch */

static bool kick_tq3d(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
		      const struct ccb_cmd_header *hdr, const struct cmd_transfer *cmd,
		      uint32_t cmd_off)
{
	const struct transfer_regs *r = &cmd->regs;
	uint32_t slot, isp;

#ifdef EXP_SLC_FLUSH_PER_KICK
	slc_flush_inval();      /* E020: rule out stale SLC lines in the PDS/PBE data */
#endif
#ifndef EXP_FRAG_CTRL
#define EXP_FRAG_CTRL 1
#endif
	bcast_write32(CR_FRAG_CTRL_COMMON, EXP_FRAG_CTRL);  /* 1: single-core GPU (spec) */

	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_FRAG);
	bif_set_index(DM_FRAG, slot);

	/*
	 * B-D. register table of kick-compute-and-3d.md (answer to Q13).
	 * Conditional registers are written only when their condition holds.
	 */
	bool fast2d = (r->isp_render & 3) == 2;

	bcast_write32(CR_ISP_BGOBJVALS, r->isp_bgobjvals);
	bcast_write64(CR_PDS_BGRND0_BASE, r->pds_bgnd0_base);
	bcast_write64(CR_PDS_BGRND1_BASE, r->pds_bgnd1_base);
	bcast_write64(CR_PDS_BGRND3_SIZEINFO, r->pds_bgnd3_sizeinfo);
#ifdef EXP_TQ_FORCE_FAULT
	fast2d = true;          /* E025: reproduce the E017 address-0 fault on purpose */
#endif
	if (fast2d) {
		bcast_write64(CR_ISP_MTILE_BASE, r->isp_mtile_base);
		bcast_write32(CR_ISP_RGN, r->isp_rgn);
		bcast_write32(CR_ISP_ZLS_A, 0);
		bcast_write32(CR_ISP_ZLS_B, 0xE4);
	}
	bcast_write32(CR_USC_PIXEL_OUTPUT, r->usc_pixel_output_ctrl);
	for (uint32_t i = 0; i < 4; i++)
		bcast_write32(CR_USC_CLEAR0 + 8 * i, r->usc_clear_register[i]);
	bcast_write32(CR_FRAG_SCREEN, r->frag_screen);
	bcast_write32(CR_ISP_MTILE_SIZE, r->isp_mtile_size);
	bcast_write32(CR_ISP_RENDER_ORIGIN, r->isp_render_origin);
	{
		/* tiles in flight: min(ISP_CTL[15:12] + 1, 2) */
		uint32_t tif = ((r->isp_ctl >> 12) & 0xF) + 1;

#ifdef EXP_TIF
		tif = EXP_TIF;          /* E022 experiment */
#endif
		bcast_write32(CR_ISP_TILES_IN_FLIGHT, tif > 2 ? 2 : tif);
	}
	bcast_write32(CR_ISP_CTL, r->isp_ctl);
	bcast_write32(CR_ISP_AA, r->isp_aa);
	if (r->isp_aa & 3) {
		bcast_write32(CR_ZS_PAIR, 0x88888888);
		bcast_write32(CR_ZS_PAIR + 4, 0x88888888);
	}
	bcast_write32(CR_ZS_EXTRA, 0);
	bcast_write32(CR_ZS_EXTRA + 4, 0);
	bcast_write32(CR_EVENT_PIXEL_PDS_INFO, r->event_pixel_pds_info);
	bcast_write32(CR_EVENT_PIXEL_PDS_CODE, r->event_pixel_pds_code);
	bcast_write32(CR_EVENT_PIXEL_PDS_DATA, r->event_pixel_pds_data);
	bcast_write32(CR_TQ_0388, 0);
	/*
	 * PBE state = destination surface (spec answer to Q12): two banks,
	 * WORD0 of render target n -> 0x1510 + 8n, WORD1 -> 0x1550 + 8n, taken
	 * from consecutive pairs pbe_wordx_mrty[2n], [2n+1].
	 */
	for (uint32_t n = 0; n < 4; n++) {
		bcast_write64(CR_PBE_WORDS + 8 * n, r->pbe_wordx_mrty[2 * n]);
		bcast_write64(CR_PBE_WORDS + 0x40 + 8 * n, r->pbe_wordx_mrty[2 * n + 1]);
	}
	TRACE(SFID_DBG_7, (uint32_t)(r->pbe_wordx_mrty[0] >> 32), (uint32_t)r->pbe_wordx_mrty[0],
	      (uint32_t)(r->pbe_wordx_mrty[1] >> 32), (uint32_t)r->pbe_wordx_mrty[1],
	      (uint32_t)(r->pbe_wordx_mrty[2] >> 32), (uint32_t)r->pbe_wordx_mrty[2]);
	TRACE(SFID_DBG_6, r->frag_screen, r->isp_mtile_size, r->isp_ctl, r->isp_render_origin, r->isp_aa);
	TRACE(SFID_DBG_4, r->event_pixel_pds_info, r->event_pixel_pds_code, r->event_pixel_pds_data);
	TRACE(SFID_DBG_9, (uint32_t)(r->isp_mtile_base >> 32), (uint32_t)r->isp_mtile_base,
	      (uint32_t)(r->pds_bgnd0_base >> 32), (uint32_t)r->pds_bgnd0_base,
	      (uint32_t)(r->pds_bgnd3_sizeinfo >> 32), (uint32_t)r->pds_bgnd3_sizeinfo,
	      r->isp_render, r->isp_rgn);

	/* D. trace, state, trigger */
	TRACE(SFID_MAIN_121, ctxaddr, cmd_off, 0, ctx->pid, ctx->priority, cmd->frame_num,
	      hdr->ext_job_ref, hdr->int_job_ref);
	cr_write32(CR_EVENT_CLEAR, EV_PIXELBE_END_RENDER);
	dm_start(DM_FRAG, ctxaddr, cmd_off, EV_PIXELBE_END_RENDER, SFID_MAIN_4, slot);
	/*
	 * Launch (answer to Q14): ISP_RENDER = isp_render, then 0x0F00 = 1.
	 * ISP_RENDER |= 0x10 belongs to the context-resume path only, which is not
	 * implemented (contexts are never stored).
	 */
	isp = r->isp_render & ~0x10u;
	bcast_write32(CR_ISP_RENDER, isp);
	bcast_write32(CR_PIXEL_LAUNCH, 1);      /* last write */
	cr_write32(CR_MTS_SCHEDULE, 0);         /* request a BG pass, as the original does */
	return true;
}

bool kick_task(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	uint32_t id = hdr->cmd_type & 0x7FFF;

	switch (id) {
	case CCB_CMD_CDM:
		return kick_cdm(ctxaddr, ctx, hdr, payload, cmd_off);
	case CCB_CMD_TQ_3D:
		return kick_tq3d(ctxaddr, ctx, hdr, payload, cmd_off);
	case CCB_CMD_NULL:
	case CCB_CMD_ABORT:
	case CCB_CMD_PRE_TIMESTAMP:
	case CCB_CMD_FBSC_INVALIDATE:
		return false;
	default:
		/* Not implemented yet: complete without running, never hang the host. */
		TRACE(SFID_MAIN_172, ctxaddr, ctx->ccbctl_fw_addr, ctx->ccb_fw_addr, cmd_off,
		      0, 0, 0, cmd_off, hdr->cmd_type, hdr->cmd_size);
		return false;
	}
}

/* MMU page fault while a DM runs: report it and recover (hwr.c). */
static void mmu_fault(void)
{
	uint64_t mmu = cr_read64(CR_BIF_FAULT_BANK0_MMU_STATUS);
	uint64_t req = cr_read64(CR_BIF_FAULT_BANK0_MMU_STATUS + 8);

	TRACE(SFID_HWR_80, 0, (uint32_t)(mmu >> 32), (uint32_t)mmu,
	      (uint32_t)(req >> 32), (uint32_t)req);
	cr_write32(CR_EVENT_CLEAR, EV_MMU_PAGE_FAULT);
	hwr_recover(HWR_REASON_MMU_FAULT, mmu, req);
}

/*
 * Per-job cache flush on completion, before the command is retired
 * (kick-compute-and-3d.md, "Completion: per-job cache flush", answer to Q14).
 * None of these registers has a public name except MCU_FENCE and the SLC.
 */
#define CR_FLUSH_FE     0x4000
#define CR_FLUSH_USC    0x0688
#define CR_FLUSH_PDS    0x0668
#define CR_FLUSH_TPU    0x1608
#define CR_FLUSH_1480   0x1480
#define CR_FLUSH_1488   0x1488
#define CR_MCU_FENCE    0x1740
#define CR_FLUSH_1720   0x1720
#define CR_FLUSH_1728   0x1728

[[maybe_unused]] static void dm_cache_flush(uint32_t dm)
{
	bool frag = dm == DM_FRAG;

	if (dm != DM_FRAG && dm != DM_CDM) {
		slc_flush_inval();
		return;
	}
	bcast_write32(CR_FLUSH_FE, 1);
	bcast_write32(CR_FLUSH_USC, frag ? 2 : 4);
	cr_poll32(CR_FLUSH_USC, frag ? 2 : 4, 0, 10000);
	bcast_write32(CR_FLUSH_PDS, frag ? 3 : 9);
	cr_poll32(CR_FLUSH_PDS, 1, 0, 10000);
	bcast_write32(CR_FLUSH_TPU, frag ? 0x38001 : 0x10001);
	cr_poll32(CR_FLUSH_TPU, 1, 0, 10000);
	if (frag)
		bcast_write32(CR_FLUSH_1480, 1);
	bcast_write32(CR_FLUSH_1488, 1);
	bcast_write64(CR_MCU_FENCE, frag ? 0x0000018000000000ull : 0x0000028000000000ull);
	bcast_write32(CR_FLUSH_1720, frag ? 0x2B : 0x33);
	cr_poll32(CR_FLUSH_1728, 1, 0, 10000);
	if (frag)
		write_barrier();
	bcast_write32(CR_SLC_CTRL_FLUSH_INVAL, frag ? 6 : 8);
	cr_poll32(CR_SLC_STATUS0, SLC_STATUS_FLUSH_PENDING, 0, 10000);
}

void kick_events(uint32_t ev)
{
	cr_write32(CR_DBG_EVENTS_SEEN, cr_read32(CR_DBG_EVENTS_SEEN) | ev);
	if (ev & EV_MMU_PAGE_FAULT)
		mmu_fault();

	for (uint32_t d = 0; d < DM_MAX; d++) {
		struct dm_state *s = &dms[d];

		if (!s->busy || !(ev & s->event_mask))
			continue;
		cr_write32(CR_EVENT_CLEAR, s->event_mask);
#ifdef EXP_SLC_ONLY_FLUSH
		slc_flush_inval();      /* E023 control: the old SLC-only flush */
#else
		dm_cache_flush(d);
#endif
		cr_write32(CR_SCRATCH15, cr_read32(CR_SCRATCH15) & ~(1u << d));
		TRACE(s->finish_sfid);
		sched_dm_done(d);
	}
}

void sched_poll(void)
{
	uint32_t mask = 0;

	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy)
			mask |= dms[d].event_mask;
	if (!mask)
		return;
	mask |= EV_MMU_PAGE_FAULT;
	uint32_t ev = cr_read32(CR_EVENT_STATUS) & mask;

	if (ev)
		kick_events(ev);
}

bool kick_resource_busy(uint32_t type, fwaddr_t addr)
{
	(void)type;
	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy && dms[d].hwrt == addr)
			return true;
	return false;
}
