// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Client CCB processing and data-master scheduling.
 *
 * Every context the host kicks is remembered in a small table. A context is
 * "pending" while its client CCB has unread commands. The scheduler walks the
 * pending contexts round-robin and, for each one, consumes commands from
 * ``read_offset``:
 *
 * - auxiliary commands (fences, updates, priority, padding...) are handled
 *   in place;
 * - a fence that is not satisfied blocks the context (it is re-evaluated on
 *   the next scheduling pass: every background task, interrupt task and
 *   timer tick);
 * - a task is dispatched to its data master if the DM is idle, and stays at
 *   ``read_offset`` until the DM signals completion; then it is retired and
 *   the following updates are processed.
 */
/**/
#include "fw.h"
#include "kick.h"
#include "pm.h"

/** Size of the context table. */
#define MAX_CTX         64
/** Kicks per context whose cleanup control blocks can wait for retirement. */
#define CLEANUP_Q       8
/** Commands one context may consume in a single pass. */
#define MAX_CMDS_PASS   64

/** One context known to the scheduler. */
struct ctx_ent {
	/** Firmware address of the context's ``fwcommoncontext``. */
	fwaddr_t addr;
	/** The entry is in use. */
	bool used;
	/** The client CCB may have commands to consume. */
	bool pending;
	/** The client CCB is unusable (bad command header or missing buffers). */
	bool dead;
	/** The context's data master, from ``fwcommoncontext.dm``. */
	uint8_t dm;
	/** Cleanup control blocks of the kicks not yet retired (FIFO). */
	fwaddr_t cleanup[CLEANUP_Q][3];
	/** Number of cleanup control blocks in each FIFO slot. */
	uint8_t cleanup_n[CLEANUP_Q];
	/** FIFO head index. */
	uint8_t cq_head;
	/** FIFO length. */
	uint8_t cq_count;
};

/** The context table. */
static struct ctx_ent ctxs[MAX_CTX];
/** Per data master: the task it runs, if any (see :c:struct:`dm_state`). */
struct dm_state dms[DM_MAX];
/** Where the next round-robin pass starts. */
static uint32_t rr_next;
/** A UFO was written or a command retired: interrupt the host after the pass. */
static bool host_notify_needed;

/**
 * Get a context's ``fwcommoncontext``.
 *
 * Args:
 *     e: The table entry.
 *
 * Returns:
 *     Pointer to the host's context structure.
 */
static inline struct fwcommoncontext *ctx_ptr(const struct ctx_ent *e)
{
	return fwptr(e->addr);
}

/**
 * Look a context up in the table.
 *
 * Args:
 *     addr: Firmware address of the context.
 *     create: Add the context when it is not in the table.
 *
 * Returns:
 *     The entry, or ``NULL`` when it is not known (and not created, or the
 *     table is full).
 */
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

/**
 * Check whether the GPU has work.
 *
 * Also traces (``DBG_6``, ``0xE044xxxx``) what keeps the GPU busy whenever
 * that changes (E043).
 *
 * Returns:
 *     ``true`` when a data master runs a task or a live context has commands
 *     pending.
 */
static bool any_busy(void)
{
	static uint32_t last_why = ~0u;
	uint32_t why = 0;
	fwaddr_t who = 0;

	for (uint32_t d = 0; d < DM_MAX; d++)
		if (dms[d].busy) {
			why = 0x100u | d;
			who = dms[d].ctx;
			break;
		}
	for (uint32_t i = 0; !why && i < MAX_CTX; i++)
		if (ctxs[i].used && ctxs[i].pending && !ctxs[i].dead) {
			why = 0x200u | i;
			who = ctxs[i].addr;
		}
	/* debug (E043): what keeps the GPU from reporting idle */
	if (why != last_why) {
		const struct cccb_ctl *ctl = NULL;

		if (who && (why & 0x200u))
			ctl = fwptr(ctx_ptr(&ctxs[why & 0xFF])->ccbctl_fw_addr);
		TRACE(SFID_DBG_6, 0xE0440000u | why, who, ctl ? ctl->read_offset : 0u,
		      ctl ? ctl->write_offset : 0u, ctl ? ctl->dep_offset : 0u);
		last_why = why;
	}
	return why != 0;
}

/**
 * Update the utilisation state and the power state after a pass.
 *
 * The utilisation is ACTIVE while work runs or waits, IDLE otherwise, and
 * BLOCKED while forced idle holds work back. The power state follows (ON or
 * IDLE) unless the firmware is forced idle or powered off.
 */
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

/**
 * Reset the scheduler: forget every context and data-master task, reset the
 * kick state and report the GPU idle.
 */
void sched_init(void)
{
	memset(ctxs, 0, sizeof(ctxs));
	memset(dms, 0, sizeof(dms));
	rr_next = 0;
	kick_init();
	pow_set_state(POW_STATE_IDLE);
}

/* -------------------------------------------------------------- UFOs */
/**
 * Check whether a fence UFO is satisfied.
 *
 * A sync checkpoint is satisfied once it is signalled or errored. A plain
 * UFO must equal its value, or, for partial-render fences, be at least its
 * value (signed comparison, so that the counter may wrap).
 *
 * Args:
 *     u: The UFO.
 *     signed_ge: Compare with ``>=`` instead of ``==``.
 *
 * Returns:
 *     ``true`` when satisfied (or when its address is null).
 */
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

/**
 * Perform an update UFO.
 *
 * A sync checkpoint is marked signalled; a plain UFO is set to its value, or
 * incremented by it.
 *
 * Args:
 *     u: The UFO.
 *     rmw: Add the value instead of writing it.
 */
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

/**
 * Signal a UFO from the firmware.
 *
 * Used by the SPM code to release the partial-render fence. The host is
 * interrupted after the scheduling pass.
 *
 * Args:
 *     u: The UFO to write.
 */
void sched_ufo_signal(const struct ufo *u)
{
	ufo_update(u, false);
	host_notify_needed = true;
}

/**
 * Check whether a ``FENCE_PR`` passed only through the SPM check.
 *
 * Args:
 *     u: The fence's UFOs.
 *     cnt: Number of UFOs.
 *
 * Returns:
 *     ``true`` when one of the UFOs is not satisfied by its own value, so the
 *     fence only passed because a partial render is armed.
 */
static bool pr_fence_blocked(const struct ufo *u, uint32_t cnt)
{
	for (uint32_t i = 0; i < cnt; i++)
		if (!ufo_satisfied(&u[i], true))
			return true;
	return false;
}

/**
 * Kick the ``FRAG_PR`` that follows a partial-render fence.
 *
 * The fence is not consumed: the partial render runs while the queue stays
 * where it is.
 *
 * Args:
 *     e: The context's table entry.
 *     ctx: The context.
 *     ccb: The client CCB.
 *     off: Offset of the command after the fence.
 *
 * Returns:
 *     ``true`` when the partial render was kicked; ``false`` when the command
 *     is not a ``FRAG_PR`` or the fragment DM is busy.
 */
static bool run_partial_render(struct ctx_ent *e, struct fwcommoncontext *ctx, uint8_t *ccb,
			       uint32_t off)
{
	struct ccb_cmd_header *h = (struct ccb_cmd_header *)(ccb + off);

	if ((h->cmd_type & 0x7FFF) != CCB_CMD_FRAG_PR || dms[DM_FRAG].busy)
		return false;
	return kick_task(DM_FRAG, e->addr, ctx, h, h + 1, off);
}

/* --------------------------------------------------------- cCCB walk */
/**
 * Choose the data master for a task command.
 *
 * Args:
 *     id: The command type (``CCB_CMD_*``, without the flag bits).
 *     ctx_dm: The context's data master, used for any other command.
 *
 * Returns:
 *     A ``DM_*`` index.
 */
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

/**
 * Retire the oldest kick's cleanup control blocks.
 *
 * Increments ``executed_commands`` in each of them, which tells the host that
 * the resources the kick used may be freed.
 *
 * Args:
 *     e: The context's table entry.
 */
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

/**
 * Consume the commands of one context.
 *
 * At most :c:macro:`MAX_CMDS_PASS` commands are taken per call. Stops at an
 * unsatisfied fence, at a task whose data master is busy, while one of the
 * context's own tasks runs, or when the CCB is empty. A command without the
 * magic word marks the context dead.
 *
 * Args:
 *     e: The context's table entry.
 *
 * Returns:
 *     ``true`` if the context made progress (anything retired or
 *     dispatched).
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
				if (kick_defer)
					return progress;        /* stays queued */
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

			bool pr_ok = id == CCB_CMD_FENCE_PR;

			for (uint32_t i = 0; i < cnt; i++) {
				if (ufo_satisfied(&u[i], id == CCB_CMD_FENCE_PR))
					continue;
				if (pr_ok && spm_pr_fence_ok(&u[i]))
					continue;       /* waits only for the geometry */
				ctx->last_failed_ufo = u[i];
				return progress;
			}
			if (pr_ok && pr_fence_blocked(u, cnt)) {
				/*
				 * Satisfied only for an armed partial render: run the
				 * FRAG_PR that follows without consuming this fence.
				 */
				if (next != woff && run_partial_render(e, ctx, ccb, next))
					return true;
				return progress;
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

/**
 * Run the scheduler.
 *
 * Walks the pending contexts round-robin, repeating while any of them makes
 * progress (at most 8 passes), then updates the power state and interrupts
 * the host if anything changed for it.
 */
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

/**
 * Retire a data master's task, called by the kick code when it finishes.
 *
 * Moves the context's read offset past the task command and retires its
 * cleanup control blocks. After a partial render (``keep`` set) the
 * ``FRAG_PR`` stays at the head of the queue for the next one.
 *
 * Args:
 *     dm: The data master (``DM_*``).
 */
void sched_dm_done(uint32_t dm)
{
	struct dm_state *s = &dms[dm];
	struct ctx_ent *e;

	if (!s->busy)
		return;
	s->busy = false;
	e = ctx_find(s->ctx, false);
	if (e && s->keep) {
		/* a partial render: FRAG_PR stays at the head for the next one */
		s->keep = false;
		e->pending = true;
	} else if (e) {
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

/**
 * Handle GPU events from the MTS interrupt task.
 *
 * Args:
 *     events: The ``CR_EVENT_STATUS`` bits that were set.
 */
void sched_irq(uint32_t events)
{
	kick_events(events);
	sched_run();
}

/**
 * Handle a KCCB kick: the host wrote new commands to a client CCB.
 *
 * Adds the context to the table if needed, queues the kick's cleanup control
 * blocks (counting them as submitted), takes the new write offset and wrap
 * mask, and runs the scheduler.
 *
 * Args:
 *     kick: The kick command.
 *
 * Returns:
 *     Extra return-slot flags; always 0.
 */
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

/**
 * Handle a KCCB ``FORCE_UPDATE``: unblock a context stuck on a fence.
 *
 * Sync checkpoints of the fence at ``fence_offset`` that are still active
 * are marked errored, then the context is scheduled again.
 *
 * Args:
 *     ctxaddr: Firmware address of the context.
 *     fence_offset: Offset of the fence in the client CCB.
 */
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

/**
 * Handle a KCCB ``CLEANUP`` request.
 *
 * A context may be freed when none of its tasks runs and its CCB is empty
 * (or dead); it is then removed from the table. HWRTData, free lists and
 * ZS buffers are checked by :c:func:`kick_resource_busy`.
 *
 * Args:
 *     type: ``CLEANUP_*`` resource type.
 *     addr: Firmware address of the resource.
 *
 * Returns:
 *     ``KCCB_RTN_CLEANUP_BUSY`` when the resource is still in use, else 0.
 */
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
