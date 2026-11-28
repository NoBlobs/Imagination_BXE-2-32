/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * Data-master state shared by the scheduler and the kick code, and the
 * functions of ``kick.c``, ``geom.c``, ``frag.c`` and ``hwr.c``.
 */
/**/
#pragma once

#include "fw.h"

/** The task a data master runs. */
struct dm_state {
	/** A task is running. */
	bool busy;
	/** Firmware address of the running context. */
	fwaddr_t ctx;
	/** Offset of the running command in its client CCB. */
	uint32_t cmd_off;
	/** ``CR_EVENT_STATUS`` bits that complete the task. */
	uint32_t event_mask;
	/** Trace entry written on completion. */
	uint32_t finish_sfid;
	/** ``CR_TIMER`` at the kick (for the lockup watchdog). */
	uint64_t kick_time;
	/** HWRTData in use (geometry and fragment). */
	fwaddr_t hwrt;
	/** BIF page-catalogue slot of the task's memory context. */
	uint32_t bif_slot;
	/** Geometry command flags (first and last kick). */
	uint32_t flags;
	/** Done, but the command stays queued (a ``FRAG_PR`` partial render). */
	bool keep;
};

extern struct dm_state dms[DM_MAX];

void kick_init(void);
/* Set by kick_task when the command must stay queued (not run, not retired). */
extern bool kick_defer;
/* Start a task; true if the DM is now running it, false if it completed at once. */
bool kick_task(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off);
void kick_events(uint32_t events);
bool kick_resource_busy(uint32_t type, fwaddr_t addr);
void sched_dm_done(uint32_t dm);
/* Poll EVENT_STATUS for completions (backstop when no IRQ task arrives). */
void sched_poll(void);

void bif_rebuild(void);

/* hwr.c */
/** Why :c:func:`hwr_recover` was called. */
enum {
	/** An MMU page fault. */
	HWR_REASON_MMU_FAULT = 1,
	/** A data master did not finish in time. */
	HWR_REASON_LOCKUP = 2,
};
void hwr_recover(uint32_t reason, uint64_t mmu_status, uint64_t req_status);
uint64_t kick_bif_pc(uint32_t slot);
void hwr_watchdog(void);

/* shared kick helpers (kick.c) */
uint32_t bif_activate(fwaddr_t memctx_addr, uint32_t dm);
void bif_set_index(uint32_t dm, uint32_t slot);
void dm_start(uint32_t dm, fwaddr_t ctxaddr, uint32_t cmd_off, uint32_t evmask,
	      uint32_t finish_sfid, uint32_t slot);

/* geometry (geom.c) */
bool kick_geom(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off);
void geom_done(struct dm_state *s);
void geom_rta_oom(uint8_t *hwrt, fwaddr_t memctx);
/* fragment (frag.c) */
bool kick_frag(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off,
	       bool partial);
void frag_done(struct dm_state *s);
bool frag_rta_next(struct dm_state *s);
void frag_hwr_reset(void);
/* 3D context store and resume (Q40) */
void frag_csw_request(fwaddr_t new_ctx);
bool frag_csw_end(struct dm_state *s, uint32_t ev);
fwaddr_t frag_csw_yield_to(fwaddr_t ctxaddr);
void frag_csw_yield_done(void);
/* compute context store and resume (scheduler.md, 19b96b7) */
void cdm_csw_request(fwaddr_t new_ctx);
fwaddr_t cdm_csw_yield_to(fwaddr_t ctxaddr);
void cdm_csw_yield_done(void);
void cdm_hwr_reset(void);
