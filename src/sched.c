// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Client CCB processing and data-master scheduling.
 *
 * Every context the host kicks is remembered in a small table.  A context is
 * "pending" while its cCCB has unread commands.  The scheduler walks pending
 * contexts round-robin and, for each one, consumes commands from read_offset:
 *   - auxiliary commands (fences, updates, priority, padding...) are handled
 *     in place;
 *   - a fence that is not satisfied blocks the context (re-evaluated on the
 *     next scheduling pass: every BG task, IRQ task and timer tick);
 *   - a task is dispatched to its data master if the DM is idle, and stays
 *     at read_offset until the DM signals completion; then it is retired and
 *     the following updates are processed.
 */
#include "fw.h"
#include "kick.h"

#define MAX_CTX         64
#define CLEANUP_Q       8
#define MAX_CMDS_PASS   64

struct ctx_ent {
	fwaddr_t addr;
	bool used;
	bool pending;
	bool dead;
	uint8_t dm;
	/* cleanup control blocks of the kicks not yet retired (FIFO) */
	fwaddr_t cleanup[CLEANUP_Q][3];
	uint8_t cleanup_n[CLEANUP_Q];
	uint8_t cq_head, cq_count;
};

static struct ctx_ent ctxs[MAX_CTX];
struct dm_state dms[DM_MAX];
static uint32_t rr_next;
static bool host_notify_needed;

static inline struct fwcommoncontext *ctx_ptr(const struct ctx_ent *e)
{
	return fwptr(e->addr);
}

static struct ctx_ent *ctx_find(fwaddr_t addr, bool create)
{
	struct ctx_ent *free = NULL;

	addr &= FW_VA_MASK;
	for (uint32_t i = 0; i < MAX_CTX; i++) {
		if (ctxs[i].used && ctxs[i].addr == addr)
			return &ctxs[i];
		if (!ctxs[i].used && !free)
			free = &ctxs[i];
	}
	if (!create || !free)
		return NULL;
	memset(free, 0, sizeof(*free));
	free->used = true;
	free->addr = addr;
	free->dm = (uint8_t)ctx_ptr(free)->dm;
	return free;
}

static bool any_busy(void)
{
	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy)
			return true;
	for (uint32_t i = 0; i < MAX_CTX; i++)
		if (ctxs[i].used && ctxs[i].pending && !ctxs[i].dead)
			return true;
	return false;
}

static void update_pow_state(void)
{
	bool busy = any_busy();

	if (g.powered_off)
		return;
	/* utilisation: ACTIVE while work runs or waits, BLOCKED if held by forced idle */
	if (g.forced_idle)
		stats_set_state(busy ? GPU_UTIL_STATE_BLOCKED : GPU_UTIL_STATE_IDLE);
	else
		stats_set_state(busy ? GPU_UTIL_STATE_ACTIVE : GPU_UTIL_STATE_IDLE);
	if (g.forced_idle)
		return;
	pow_set_state(busy ? POW_STATE_ON : POW_STATE_IDLE);
}

void sched_init(void)
{
	memset(ctxs, 0, sizeof(ctxs));
	memset(dms, 0, sizeof(dms));
	rr_next = 0;
	kick_init();
	pow_set_state(POW_STATE_IDLE);
}

/* -------------------------------------------------------------- UFOs */
static bool ufo_satisfied(const struct ufo *u, bool signed_ge)
{
	volatile uint32_t *p = fwptr(u->addr & ~UFO_ADDR_IS_SYNC_CHECKPOINT);

	if (!p)
		return true;
	if (u->addr & UFO_ADDR_IS_SYNC_CHECKPOINT) {
		uint32_t s = *p;

		return s == SYNC_CHECKPOINT_SIGNALED || s == SYNC_CHECKPOINT_ERRORED;
	}
	if (signed_ge)
		return (int32_t)(*p - u->value) >= 0;
	return *p == u->value;
}

static void ufo_update(const struct ufo *u, bool rmw)
{
	volatile uint32_t *p = fwptr(u->addr & ~UFO_ADDR_IS_SYNC_CHECKPOINT);

	if (!p)
		return;
	if (u->addr & UFO_ADDR_IS_SYNC_CHECKPOINT)
		*p = SYNC_CHECKPOINT_SIGNALED;
	else if (rmw)
		*p = *p + u->value;
	else
		*p = u->value;
}

/* --------------------------------------------------------- cCCB walk */
static uint32_t task_dm(uint32_t id, uint32_t ctx_dm)
{
	switch (id) {
	case CCB_CMD_CDM:
		return DM_CDM;
	case CCB_CMD_GEOM:
		return DM_GEOM;
	case CCB_CMD_FRAG:
	case CCB_CMD_FRAG_PR:
	case CCB_CMD_TQ_3D:
		return DM_FRAG;
	case CCB_CMD_TQ_TDM:
	case CCB_CMD_TQ_2D:
		return DM_TDM;
	default:
		return ctx_dm < DM_MAX ? ctx_dm : DM_GP;
	}
}

static void cleanup_ctl_done(struct ctx_ent *e)
{
	if (!e->cq_count)
		return;
	uint32_t h = e->cq_head;

	for (uint32_t i = 0; i < e->cleanup_n[h]; i++) {
		struct cleanup_ctl *c = fwptr(e->cleanup[h][i]);

		if (c)
			c->executed_commands++;
	}
	e->cq_head = (h + 1) % CLEANUP_Q;
	e->cq_count--;
}

/*
 * Consume commands of one context. Returns true if the context made
 * progress (anything retired or dispatched).
 */
static bool ctx_process(struct ctx_ent *e)
{
	struct fwcommoncontext *ctx = ctx_ptr(e);
	struct cccb_ctl *ctl = fwptr(ctx->ccbctl_fw_addr);
	uint8_t *ccb = fwptr(ctx->ccb_fw_addr);
	bool progress = false;

	if (!ctl || !ccb) {
		e->dead = true;
		return false;
	}

	for (uint32_t n = 0; n < MAX_CMDS_PASS; n++) {
		uint32_t roff = ctl->read_offset;
		uint32_t woff = ctl->write_offset;

		if (roff == woff) {
			e->pending = false;
			ctl->dep_offset = roff;
			break;
		}

		/* Is this context's task already running on its DM? */
		for (uint32_t d = 0; d < DM_MAX; d++)
			if (dms[d].busy && dms[d].ctx == e->addr)
				return progress;

		struct ccb_cmd_header *hdr = (struct ccb_cmd_header *)(ccb + roff);
		uint32_t type = hdr->cmd_type;
		uint32_t id = type & 0x7FFF;
		void *payload = hdr + 1;
		uint32_t next = (roff + sizeof(*hdr) + hdr->cmd_size) & ctl->wrap_mask;

		if ((type >> 16) != ROGUE_CMD_MAGIC_DWORD) {
			TRACE(SFID_MAIN_171, e->addr, ctx->ccbctl_fw_addr, ctx->ccb_fw_addr, roff,
			      ctl->dep_offset, woff, ctl->wrap_mask, roff, type, hdr->cmd_size);
			e->dead = true;
			return progress;
		}

		if (type & ROGUE_CCB_TYPE_TASK) {
			uint32_t dm = task_dm(id, e->dm);

			if (dms[dm].busy)
				return progress;
			if (!g.forced_idle) {
				TRACE(SFID_MTS_9, dm, ctx->ccb_fw_addr, type);
				ctl->dep_offset = roff;
				if (kick_task(dm, e->addr, ctx, hdr, payload, roff))
					return true;   /* running: stays at read_offset */
				/* completed at once (or not supported): retire */
				cleanup_ctl_done(e);
			} else {
				return progress;
			}
			ctl->read_offset = next;
			progress = true;
			host_notify_needed = true;
			continue;
		}

		switch (id) {
		case CCB_CMD_FENCE:
		case CCB_CMD_FENCE_PR: {
			const struct ufo *u = payload;
			uint32_t cnt = hdr->cmd_size / sizeof(struct ufo);

			for (uint32_t i = 0; i < cnt; i++) {
				if (!ufo_satisfied(&u[i], id == CCB_CMD_FENCE_PR)) {
					ctx->last_failed_ufo = u[i];
					return progress;
				}
			}
			break;
		}
		case CCB_CMD_UPDATE:
		case CCB_CMD_UNFENCED_UPDATE:
		case CCB_CMD_RMW_UPDATE:
		case CCB_CMD_UNFENCED_RMW_UPDATE: {
			const struct ufo *u = payload;
			uint32_t cnt = hdr->cmd_size / sizeof(struct ufo);
			bool rmw = id == CCB_CMD_RMW_UPDATE || id == CCB_CMD_UNFENCED_RMW_UPDATE;

			TRACE(SFID_MAIN_22, e->addr, roff);
			for (uint32_t i = 0; i < cnt; i++)
				ufo_update(&u[i], rmw);
			host_notify_needed = true;
			break;
		}
		case CCB_CMD_PRIORITY:
			ctx->priority = *(const uint32_t *)payload;
			break;
		case CCB_CMD_PADDING:
		case CCB_CMD_POST_TIMESTAMP:
			break;
		default:
			TRACE(SFID_MAIN_171, e->addr, ctx->ccbctl_fw_addr, ctx->ccb_fw_addr, roff,
			      ctl->dep_offset, woff, ctl->wrap_mask, roff, type, hdr->cmd_size);
			break;
		}
		ctl->read_offset = next;
		ctl->dep_offset = next;
		progress = true;
	}
	return progress;
}

void sched_run(void)
{
	bool progress;
	uint32_t guard = 0;

	do {
		progress = false;
		for (uint32_t k = 0; k < MAX_CTX; k++) {
			struct ctx_ent *e = &ctxs[(rr_next + k) % MAX_CTX];

			if (!e->used || !e->pending || e->dead)
				continue;
			if (ctx_process(e))
				progress = true;
		}
		rr_next = (rr_next + 1) % MAX_CTX;
	} while (progress && ++guard < 8);

	update_pow_state();
	if (host_notify_needed) {
		host_notify_needed = false;
		notify_host();
	}
}

/* Called by the kick code when a DM finished its task. */
void sched_dm_done(uint32_t dm)
{
	struct dm_state *s = &dms[dm];
	struct ctx_ent *e;

	if (!s->busy)
		return;
	s->busy = false;
	e = ctx_find(s->ctx, false);
	if (e) {
		struct fwcommoncontext *ctx = ctx_ptr(e);
		struct cccb_ctl *ctl = fwptr(ctx->ccbctl_fw_addr);
		struct ccb_cmd_header *hdr = (struct ccb_cmd_header *)
			((uint8_t *)fwptr(ctx->ccb_fw_addr) + s->cmd_off);

		ctl->read_offset = (s->cmd_off + sizeof(*hdr) + hdr->cmd_size) & ctl->wrap_mask;
		ctl->dep_offset = ctl->read_offset;
		cleanup_ctl_done(e);
		e->pending = true;      /* process the following updates */
	}
	host_notify_needed = true;
}

void sched_irq(uint32_t events)
{
	kick_events(events);
	sched_run();
}

uint32_t sched_kick(const struct kick_data *kick)
{
	struct ctx_ent *e = ctx_find(kick->context_fw_addr, true);
	struct fwcommoncontext *ctx;
	struct cccb_ctl *ctl;

	if (!e)
		return 0;
	ctx = ctx_ptr(e);
	ctl = fwptr(ctx->ccbctl_fw_addr);

	uint32_t n = kick->num_cleanup_ctl > 3 ? 3 : kick->num_cleanup_ctl;

	if (n && e->cq_count < CLEANUP_Q) {
		uint32_t t = (e->cq_head + e->cq_count) % CLEANUP_Q;

		for (uint32_t i = 0; i < n; i++)
			e->cleanup[t][i] = kick->cleanup_ctl_fw_addr[i];
		e->cleanup_n[t] = (uint8_t)n;
		e->cq_count++;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct cleanup_ctl *c = fwptr(kick->cleanup_ctl_fw_addr[i]);

		if (c)
			c->submitted_commands++;
	}
	if (ctl) {
		ctl->wrap_mask = kick->client_wrap_mask_update;
		ctl->write_offset = kick->client_woff_update;
	}
	TRACE(SFID_MAIN_14, kick->client_woff_update, e->dm, e->addr);
	e->pending = true;
	e->dead = false;
	sched_run();
	return 0;
}

void sched_force_update(fwaddr_t ctxaddr, uint32_t fence_offset)
{
	struct ctx_ent *e = ctx_find(ctxaddr, false);

	if (!e)
		return;
	struct fwcommoncontext *ctx = ctx_ptr(e);
	struct cccb_ctl *ctl = fwptr(ctx->ccbctl_fw_addr);
	uint8_t *ccb = fwptr(ctx->ccb_fw_addr);
	struct ccb_cmd_header *hdr = (struct ccb_cmd_header *)(ccb + (fence_offset & ctl->wrap_mask));
	uint32_t id = hdr->cmd_type & 0x7FFF;

	if (id == CCB_CMD_FENCE || id == CCB_CMD_FENCE_PR) {
		struct ufo *u = (struct ufo *)(hdr + 1);

		for (uint32_t i = 0; i < hdr->cmd_size / sizeof(*u); i++) {
			volatile uint32_t *p = fwptr(u[i].addr & ~UFO_ADDR_IS_SYNC_CHECKPOINT);

			if ((u[i].addr & UFO_ADDR_IS_SYNC_CHECKPOINT) && p &&
			    *p == SYNC_CHECKPOINT_ACTIVE)
				*p = SYNC_CHECKPOINT_ERRORED;
		}
	}
	e->pending = true;
	sched_run();
}

uint32_t sched_cleanup(uint32_t type, fwaddr_t addr)
{
	addr &= FW_VA_MASK;

	switch (type) {
	case CLEANUP_FWCOMMONCONTEXT: {
		struct ctx_ent *e = ctx_find(addr, false);

		if (!e)
			return 0;
		for (uint32_t d = 0; d < DM_MAX; d++)
			if (dms[d].busy && dms[d].ctx == e->addr)
				return KCCB_RTN_CLEANUP_BUSY;
		struct cccb_ctl *ctl = fwptr(ctx_ptr(e)->ccbctl_fw_addr);

		if (ctl && ctl->read_offset != ctl->write_offset && !e->dead)
			return KCCB_RTN_CLEANUP_BUSY;
		e->used = false;
		return 0;
	}
	case CLEANUP_HWRTDATA:
	case CLEANUP_FREELIST:
	case CLEANUP_ZSBUFFER:
		return kick_resource_busy(type, addr) ? KCCB_RTN_CLEANUP_BUSY : 0;
	default:
		return 0;
	}
}
