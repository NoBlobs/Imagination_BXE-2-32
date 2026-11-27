// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Data-master kicks and completion handling: the BIF page-catalogue slots,
 * the compute and transfer kicks, the dispatch of a task to its kick
 * function, and the completion events with their per-job cache flush.
 *
 * See ``docsClean`` ``data-masters.md``, ``kick-compute-and-3d.md`` and
 * ``kick-geometry-ta.md``.
 */
/**/
#include "kick.h"
#include "pm.h"

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


static void dm_cache_flush(uint32_t dm);

/* ------------------------------------------------------------ BIF */
/** Number of BIF page-catalogue slots; slot 0 belongs to the firmware. */
#define BIF_SLOTS 8
/** Page catalogue bound to each slot; all ones when the slot is free. */
static uint64_t bif_pc[BIF_SLOTS];
/** The next slot to try when a new page catalogue must be bound. */
static uint32_t bif_next = 1;

/**
 * Check whether a running task uses a BIF slot.
 *
 * Args:
 *     slot: The slot.
 *
 * Returns:
 *     ``true`` when a busy data master was kicked with this slot.
 */
static bool bif_slot_in_use(uint32_t slot)
{
	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy && dms[d].bif_slot == slot)
			return true;
	return false;
}

/**
 * Bind a memory context to a BIF catalogue slot.
 *
 * A page catalogue already bound keeps its slot. Otherwise the next slot not
 * used by a running task is taken (slot 1 if every slot is busy); the SLC is
 * flushed (page tables may sit stale in it, E015), ``CR_BIF_CAT_BASEn`` is
 * written and the MMU caches are invalidated. The slot is recorded in the
 * memory context's ``page_cat_base_reg_set``.
 *
 * Args:
 *     memctx_addr: Firmware address of the ``fwmemcontext``.
 *     dm: The data master that will use it (only traced).
 *
 * Returns:
 *     The slot, 1 to 7.
 */
uint32_t bif_activate(fwaddr_t memctx_addr, uint32_t dm)
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

/**
 * Select the BIF slot a data master's requests use.
 *
 * Writes the data master's field of ``CR_BIF_CAT_BASE_INDEX`` (geometry
 * bits 0..2, fragment 8..10, compute 16..18, TDM 19..21). Other data masters
 * are ignored.
 *
 * Args:
 *     dm: The data master.
 *     slot: The slot.
 */
void bif_set_index(uint32_t dm, uint32_t slot)
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

/**
 * Get the page catalogue bound to a BIF slot (used by the HWR code).
 *
 * Args:
 *     slot: The slot.
 *
 * Returns:
 *     The page catalogue's bus address, or 0 when the slot is free.
 */
uint64_t kick_bif_pc(uint32_t slot)
{
	return slot < BIF_SLOTS && bif_pc[slot] != ~0ull ? bif_pc[slot] : 0;
}

/**
 * Re-program every bound page catalogue after a GPU soft reset, and
 * invalidate the MMU caches.
 */
void bif_rebuild(void)
{
	for (uint32_t s = 1; s < BIF_SLOTS; s++)
		if (bif_pc[s] != ~0ull)
			cr_write64(CR_BIF_CAT_BASE0 + 8 * s, bif_pc[s]);
	cr_write32(CR_BIF_CTRL_INVAL, 0xF);
}

/**
 * Set by :c:func:`kick_task` when the command must stay queued: neither run
 * nor retired.
 */
bool kick_defer;

/** Forget every BIF slot binding (at boot and at power-off). */
void kick_init(void)
{
	for (uint32_t s = 0; s < BIF_SLOTS; s++)
		bif_pc[s] = ~0ull;
	bif_next = 1;
}

/**
 * Flush and invalidate the whole system-level cache, and wait (at most
 * 10 ms) until the flush is done.
 */
void slc_flush_inval(void)
{
	cr_write32(CR_SLC_CTRL_FLUSH_INVAL, SLC_FLUSH_INVAL_ALL);
	cr_poll32(CR_SLC_STATUS0, SLC_STATUS_FLUSH_PENDING, 0, 10000);
}

/**
 * Record that a data master starts a task.
 *
 * Called by each kick function just before its trigger write. Fills the
 * data master's :c:struct:`dm_state` and sets its bit in the debug register
 * ``CR_SCRATCH15``. The geometry and fragment kicks set ``hwrt`` afterwards.
 *
 * Args:
 *     dm: The data master.
 *     ctxaddr: Firmware address of the context.
 *     cmd_off: Offset of the command in the client CCB.
 *     evmask: ``CR_EVENT_STATUS`` bits that complete the task.
 *     finish_sfid: Trace entry written on completion.
 *     slot: The BIF slot the task uses.
 */
void dm_start(uint32_t dm, fwaddr_t ctxaddr, uint32_t cmd_off, uint32_t evmask,
		     uint32_t finish_sfid, uint32_t slot)
{
	struct dm_state *s = &dms[dm];

	s->busy = true;
	s->keep = false;
	s->ctx = ctxaddr;
	s->cmd_off = cmd_off;
	s->event_mask = evmask;
	s->finish_sfid = finish_sfid;
	s->kick_time = gpu_timer();
	s->bif_slot = slot;
	s->hwrt = 0;            /* set by the geometry / fragment kick after this call */
	cr_write32(CR_SCRATCH15, cr_read32(CR_SCRATCH15) | (1u << dm));
}

/* ------------------------------------------------------------ compute */
/**
 * Kick a compute (``CDM``) command.
 *
 * Programs the multicore compute control, binds the memory context, writes
 * the control stream, border colour table, context state and the context
 * switch PDS programs, then starts the CDM. Completion is
 * ``EV_COMPUTE_FINISHED``.
 *
 * Args:
 *     ctxaddr: Firmware address of the context.
 *     ctx: The compute context.
 *     hdr: The command header.
 *     cmd: The compute command.
 *     cmd_off: Offset of the command in the client CCB.
 *
 * Returns:
 *     ``true``: the CDM is running.
 */
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

/**
 * Kick a transfer command on the fragment data master (``TQ_3D``).
 *
 * Writes the register table of ``kick-compute-and-3d.md`` (answer to Q13):
 * background object, the fast-2D region registers when ``isp_render`` selects
 * fast 2D, USC output and clear values, screen, tile size and origin, tiles
 * in flight (``min(ISP_CTL[15:12] + 1, 2)``), ISP control and anti-aliasing,
 * the end-of-render PDS program and the PBE words of the four render
 * targets. It then launches the pixel pipe (answer to Q14). Completion is
 * ``EV_PIXELBE_END_RENDER``.
 *
 * Args:
 *     ctxaddr: Firmware address of the context.
 *     ctx: The render context.
 *     hdr: The command header.
 *     cmd: The transfer command.
 *     cmd_off: Offset of the command in the client CCB.
 *
 * Returns:
 *     ``true``: the fragment data master is running.
 */
static bool kick_tq3d(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
		      const struct ccb_cmd_header *hdr, const struct cmd_transfer *cmd,
		      uint32_t cmd_off)
{
	const struct transfer_regs *r = &cmd->regs;
	uint32_t slot, isp;

#ifdef EXP_SLC_FLUSH_PER_KICK
	slc_flush_inval();      /* E020: rule out stale SLC lines in the PDS/PBE data */
#endif
#ifdef EXP_DM_FLUSH_PER_KICK
	dm_cache_flush(DM_FRAG);        /* E026: rule out stale TPU/MCU/SLC data between TQs */
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

/**
 * Dump memory to the debug trace, 32 bytes per entry (E027).
 *
 * Each block of 8 words is a ``DBG_2`` tag (``tag | byte offset``) followed
 * by a ``DBG_9`` entry with the words. The debug group must be enabled (log
 * type mask ``0x80007FFE``).
 *
 * Args:
 *     tag: Block tag, ``0xD0000000 | block id``.
 *     base: Start of the memory; nothing is dumped when ``NULL``.
 *     bytes: Size to dump.
 */
static void dump_words(uint32_t tag, const void *base, uint32_t bytes)
{
	const uint32_t *w = base;

	if (!w)
		return;
	for (uint32_t off = 0; off < bytes; off += 32) {
		const uint32_t *p = w + off / 4;

		TRACE(SFID_DBG_2, tag | off);
		TRACE(SFID_DBG_9, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	}
}

/**
 * Dump the host structures behind a geometry or fragment command (E027).
 *
 * Dumps the command, its HWRTData, the HWRTData's common part and free
 * lists, and for a geometry command the render context's static state and
 * context state, so that they can be matched by value against the registers
 * the specification observed. Built only with ``EXP_DUMP_RENDER``.
 *
 * Args:
 *     id: ``CCB_CMD_GEOM`` or ``CCB_CMD_FRAG``.
 *     ctxaddr: Firmware address of the render context.
 *     ctx: The render context.
 *     payload: The command.
 *     size: The command's size.
 */
[[maybe_unused]] static void dump_render_cmd(uint32_t id, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
			    const void *payload, uint32_t size)
{
	/* rogue_fwif_cmd_geom_frag_shared: frame, hwrt_data_fw_addr, pr_buffer[2] */
	const uint32_t *shared = payload;
	const uint8_t *hwrt = fwptr(shared[1]);

	dump_words(0xD0000000u | (id == CCB_CMD_GEOM ? 0x10000 : 0x20000), payload, size);
	if (hwrt) {
		dump_words(0xD0030000u, hwrt, 288);                       /* rogue_fwif_hwrtdata */
		dump_words(0xD0040000u, fwptr(*(const uint32_t *)(hwrt + 164)), 88); /* common */
		for (uint32_t i = 0; i < 3; i++) {
			const void *fl = fwptr(*(const uint32_t *)(hwrt + 176 + 4 * i));

			dump_words(0xD0050000u + (i << 12), fl, 64);              /* freelists */
		}
	}
	/* fwrendercontext: geom ctx @0, frag ctx @152, static state @304 (2 x 184) */
	if (id == CCB_CMD_GEOM) {
		dump_words(0xD0060000u, (const uint8_t *)fwptr(ctxaddr) + 304, 184);
		dump_words(0xD0070000u, fwptr(ctx->context_state_addr), 64);
	}
}

/**
 * Start a task command on its data master.
 *
 * Dispatches by command type to the compute, transfer, geometry or fragment
 * kick. ``NULL``, ``ABORT``, ``PRE_TIMESTAMP`` and ``FBSC_INVALIDATE``
 * complete at once. A ``FRAG_PR`` runs only when the SPM armed a partial
 * render for its HWRTData; otherwise it completes without running, unless
 * that HWRTData's geometry still runs (it may still run out of memory), in
 * which case :c:var:`kick_defer` keeps it queued. Unimplemented commands are
 * traced and complete without running, so the host never hangs.
 *
 * Args:
 *     dm: The data master chosen by the scheduler.
 *     ctxaddr: Firmware address of the context.
 *     ctx: The context.
 *     hdr: The command header.
 *     payload: The command, right after the header.
 *     cmd_off: Offset of the command in the client CCB.
 *
 * Returns:
 *     ``true`` if the data master is now running the task, ``false`` if it
 *     completed at once (or must wait, see :c:var:`kick_defer`).
 */
bool kick_task(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	kick_defer = false;
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
#ifndef EXP_NO_RENDER
	case CCB_CMD_GEOM:
#ifdef EXP_DUMP_RENDER
		dump_render_cmd(id, ctxaddr, ctx, payload, hdr->cmd_size);
#endif
		return kick_geom(ctxaddr, ctx, hdr, payload, cmd_off);
	case CCB_CMD_FRAG:
#ifdef EXP_DUMP_RENDER
		dump_render_cmd(id, ctxaddr, ctx, payload, hdr->cmd_size);
#endif
		return kick_frag(ctxaddr, ctx, hdr, payload, cmd_off, false);
	case CCB_CMD_FRAG_PR:
		/*
		 * Runs only when the SPM armed a partial render for this HWRTData;
		 * otherwise it is consumed without running (Q16m answer).
		 */
		{
			fwaddr_t hwrt = *(const uint32_t *)((const uint8_t *)payload + 4);

			if (spm_pr_armed(hwrt))
				return kick_frag(ctxaddr, ctx, hdr, payload, cmd_off, true);
			/*
			 * While this HWRTData's geometry still runs it may run out of
			 * memory: keep FRAG_PR queued (its fence can already pass).
			 */
			if (dms[DM_GEOM].busy && dms[DM_GEOM].hwrt == hwrt)
				kick_defer = true;
		}
		return false;
#endif
	default:
		/* Not implemented yet: complete without running, never hang the host. */
		TRACE(SFID_MAIN_172, ctxaddr, ctx->ccbctl_fw_addr, ctx->ccb_fw_addr, cmd_off,
		      0, 0, 0, cmd_off, hdr->cmd_type, hdr->cmd_size);
		return false;
	}
}

/**
 * Handle an MMU page fault while a data master runs.
 *
 * Traces the fault status of bank 0, clears the event and starts the
 * hardware recovery (:c:func:`hwr_recover`).
 */
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

/**
 * Per-job cache flush on completion, before the command is retired.
 *
 * Follows ``kick-compute-and-3d.md`` "Completion: per-job cache flush"
 * (answer to Q14; geometry values from Q19b): front end, USC, PDS and TPU
 * flushes, the MCU fence for the data master, then an SLC flush. Data
 * masters other than geometry, fragment and compute only flush the SLC.
 *
 * Args:
 *     dm: The data master that finished.
 */
static void dm_cache_flush(uint32_t dm)
{
	bool frag = dm == DM_FRAG;

	if (dm != DM_FRAG && dm != DM_CDM && dm != DM_GEOM) {
		slc_flush_inval();
		return;
	}
	static const struct {
		uint32_t usc, pds, tpu, mcu_dm, f1720, slc;
	} v[3] = {
		/* geometry (answer to Q19b), fragment/transfer, compute */
		{ 1, 5, 0x10001, 0x080, 0x27, 2 },
		{ 2, 3, 0x38001, 0x180, 0x2B, 6 },
		{ 4, 9, 0x10001, 0x280, 0x33, 8 },
	};
	uint32_t i = dm == DM_GEOM ? 0 : frag ? 1 : 2;

	bcast_write32(CR_FLUSH_FE, 1);
	bcast_write32(CR_FLUSH_USC, v[i].usc);
	cr_poll32(CR_FLUSH_USC, v[i].usc, 0, 10000);
	bcast_write32(CR_FLUSH_PDS, v[i].pds);
	cr_poll32(CR_FLUSH_PDS, 1, 0, 10000);
	bcast_write32(CR_FLUSH_TPU, v[i].tpu);
	cr_poll32(CR_FLUSH_TPU, 1, 0, 10000);
	if (frag)
		bcast_write32(CR_FLUSH_1480, 1);
	bcast_write32(CR_FLUSH_1488, 1);
	bcast_write64(CR_MCU_FENCE, (uint64_t)v[i].mcu_dm << 32);
	bcast_write32(CR_FLUSH_1720, v[i].f1720);
	cr_poll32(CR_FLUSH_1728, 1, 0, 10000);
	if (dm != DM_CDM)
		write_barrier();
	bcast_write32(CR_SLC_CTRL_FLUSH_INVAL, v[i].slc);
	cr_poll32(CR_SLC_STATUS0, SLC_STATUS_FLUSH_PENDING, 0, 10000);
}

/**
 * Handle GPU events.
 *
 * An MMU page fault starts the recovery; a PM out-of-memory event while the
 * geometry runs goes to the SPM (:c:func:`spm_oom`). Each busy data master
 * whose completion event is set then finishes: a render-target array moves
 * to its next layer instead (:c:func:`frag_rta_next`); otherwise the
 * geometry post-processing runs, the caches are flushed, the fragment
 * post-processing runs, and the task is retired (:c:func:`sched_dm_done`).
 *
 * Args:
 *     ev: The ``CR_EVENT_STATUS`` bits that were set.
 */
void kick_events(uint32_t ev)
{
	cr_write32(CR_DBG_EVENTS_SEEN, cr_read32(CR_DBG_EVENTS_SEEN) | ev);
	if (ev & EV_MMU_PAGE_FAULT)
		mmu_fault();
#ifndef EXP_NO_RENDER
	if ((ev & EV_PM_OUT_OF_MEMORY) && dms[DM_GEOM].busy) {
		cr_write32(CR_EVENT_CLEAR, EV_PM_OUT_OF_MEMORY);
		spm_oom();
	}
#endif

	for (uint32_t d = 0; d < DM_MAX; d++) {
		struct dm_state *s = &dms[d];

		if (!s->busy || !(ev & s->event_mask))
			continue;
		cr_write32(CR_EVENT_CLEAR, s->event_mask);
		/* render-target array: next layer, before the flush and the retire */
		if (d == DM_FRAG && frag_rta_next(s))
			continue;
		if (d == DM_GEOM)
			geom_done(s);   /* TPC flush before the cache flush */
#ifdef EXP_SLC_ONLY_FLUSH
		slc_flush_inval();      /* E023 control: the old SLC-only flush */
#else
		dm_cache_flush(d);
#endif
		cr_write32(CR_SCRATCH15, cr_read32(CR_SCRATCH15) & ~(1u << d));
		if (d == DM_FRAG && s->hwrt)
			frag_done(s);
		TRACE(s->finish_sfid);
		sched_dm_done(d);
	}
}

/**
 * Poll ``CR_EVENT_STATUS`` for the events the running tasks wait for.
 *
 * A backstop for a missed MTS interrupt task: called from the timer tick
 * and the background task. Page faults, and out-of-memory while the geometry
 * runs, are polled too.
 */
void sched_poll(void)
{
	uint32_t mask = 0;

	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy)
			mask |= dms[d].event_mask;
	if (!mask)
		return;
	mask |= EV_MMU_PAGE_FAULT;
	if (dms[DM_GEOM].busy)
		mask |= EV_PM_OUT_OF_MEMORY;
	uint32_t ev = cr_read32(CR_EVENT_STATUS) & mask;

	if (ev)
		kick_events(ev);
}

/**
 * Check whether a running task uses a resource the host wants to free.
 *
 * Args:
 *     type: ``CLEANUP_*`` type (not used: every type is compared with the
 *         running HWRTData).
 *     addr: Firmware address of the resource.
 *
 * Returns:
 *     ``true`` when a busy data master uses it.
 */
bool kick_resource_busy(uint32_t type, fwaddr_t addr)
{
	(void)type;
	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy && dms[d].hwrt == addr)
			return true;
	return false;
}
