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

void kick_init(void)
{
	for (uint32_t s = 0; s < BIF_SLOTS; s++)
		bif_pc[s] = ~0ull;
	bif_next = 1;
}

static void slc_flush_inval(void)
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

	bcast_write32(CR_CDM_RUN, 1);   /* trigger: last write */
	return true;
}

bool kick_task(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	uint32_t id = hdr->cmd_type & 0x7FFF;

	switch (id) {
	case CCB_CMD_CDM:
		return kick_cdm(ctxaddr, ctx, hdr, payload, cmd_off);
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

void kick_events(uint32_t ev)
{
	for (uint32_t d = 0; d < DM_MAX; d++) {
		struct dm_state *s = &dms[d];

		if (!s->busy || !(ev & s->event_mask))
			continue;
		cr_write32(CR_EVENT_CLEAR, s->event_mask);
		slc_flush_inval();
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
