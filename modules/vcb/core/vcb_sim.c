/* vcb_sim.c -- the recovered VCB tick kernel. See vcb_sim.h.
 *
 * This is a direct implementation of TransistorEngine::compute (vcb.exe RVA
 * 0x28f2c0) and its per-ink gate handlers (jump table at 0x28fc40), decompiled
 * with Ghidra and cross-checked tick-by-tick against the original engine driven
 * headlessly (tools/phasec/). Godot-free; unit-tested in core/test.
 *
 * The kernel is an event-driven delta-accumulator simulator over two work lists:
 *
 *   gate list  (engine +0x220)  signed entries: `entity * delta` of the net that
 *                               scheduled it, so the sign carries the *direction*
 *                               of that net's change (latches/VMem/RANDOM read it).
 *   net list   (engine +0x238)  plain positive net indices.
 *
 * One tick is exactly one gate->net hop:
 *
 *   PASS 1  drain the gate list; each entity's new output is gate_fn(ink, n_high,
 *           n_inputs, state, sign). On a change: commit, form delta = 2*new-1, and
 *           for every entity in its adjacency add delta to that entity's n_high and
 *           push it (positive) onto the net list.
 *   events += |net list|;  clear the gate list.
 *   PASS 2  drain the net list; a net's state is simply `n_high != 0` (wired-OR).
 *           On a change: commit, and propagate the same delta into each consumer's
 *           n_high, pushing `consumer * delta` (signed) onto the gate list for the
 *           *next* tick.
 *   events += |gate list|;  clear the net list.
 *
 * so `current_event` counts scheduled propagations, not raw state transitions.
 *
 * Adjacency is directed: a component's list is the nets it drives (WRITE
 * junctions), a net's list is the components that read it (READ junctions).
 * n_high is a *byte* accumulator in the original's circuit_data cell, and is a
 * byte here too so the wrap behaviour matches.
 */
#include "vcb_sim.h"
#include <stdlib.h>
#include <string.h>

/* The per-ink gate handlers, exactly as the 0x28fc40 jump table dispatches them.
 * `n_high` is the tally of high inputs, `n_inputs` the number of input nets.
 * Handlers that depend on the scheduling entry's sign (LATCH, VMem, RANDOM) are
 * evaluated in the tick loop instead; this covers the pure ones and is what the
 * unit tests exercise. */
int vcb_gate_eval(uint8_t ink, int n_high, int n_inputs) {
	switch (ink) {
		case 0x01:                              /* BUFFER  0x28f549 */
		case 0x03:                              /* OR      0x28f549 */
		case 0x0c:                              /* LED     0x28f549 */
		case 0x11:                              /* BREAKPOINT 0x28f656 (same test) */
		case 0x13: case 0x14: case 0x15:        /* WIRELESS_0..2  0x28f549 */
		case 0x16:                              /* WIRELESS_3     0x28f708 */
			return n_high != 0;
		/* AND is `n_high == n_inputs` with **no** n_inputs>0 guard (0x28f553). A
		 * 0-input AND would therefore read as HIGH -- but it never gets evaluated:
		 * construct_model only seeds inks 0x05..0x09 into the initial work list, and
		 * an entity is only re-evaluated when one of its inputs changes. So an
		 * unconnected AND stays OFF, which is what the original engine shows. */
		case 0x02: return n_high == n_inputs;   /* AND     0x28f553 */
		case 0x04: return (n_high & 1) != 0;    /* XOR     0x28f560 */
		case 0x05:                              /* NOT     0x28f56b */
		case 0x07: return n_high == 0;          /* NOR     0x28f56b */
		case 0x06: return n_high != n_inputs;   /* NAND    0x28f578 */
		case 0x08: return (~(unsigned)n_high & 1u) != 0; /* XNOR 0x28f582 */
		default: return 0;                      /* 0x00 and the stateful inks */
	}
}

/* MT19937 -- the kernel's RANDOM generator (0x292130), seeded at the top of
 * `compute` with multiplier 0x6c078965 over a 624-word state. */
static void mt_seed(VCBSim *s, uint32_t seed) {
	s->mt[0] = seed;
	for (int i = 1; i < 624; i++)
		s->mt[i] = (uint32_t)(1812433253u * (s->mt[i - 1] ^ (s->mt[i - 1] >> 30)) + (uint32_t)i);
	s->mt_index = 624;
	s->mt_seeded = 1;
}

static uint32_t mt_next(VCBSim *s) {
	if (!s->mt_seeded)
		mt_seed(s, 5489u);
	if (s->mt_index >= 624) {
		for (int i = 0; i < 624; i++) {
			uint32_t y = (s->mt[i] & 0x80000000u) | (s->mt[(i + 1) % 624] & 0x7fffffffu);
			s->mt[i] = s->mt[(i + 397) % 624] ^ (y >> 1);
			if (y & 1)
				s->mt[i] ^= 2567483615u;
		}
		s->mt_index = 0;
	}
	uint32_t y = s->mt[s->mt_index++];
	y ^= y >> 11;
	y ^= (y << 7) & 2636928640u;
	y ^= (y << 15) & 4022730752u;
	y ^= y >> 18;
	return y;
}

/* ---- small int-vector helpers ---- */
static void ivec_push(VCBIntVec *v, int32_t x) {
	if (v->count == v->cap) {
		int32_t nc = v->cap ? v->cap * 2 : 8;
		v->items = (int32_t *)realloc(v->items, (size_t)nc * sizeof(int32_t));
		v->cap = nc;
	}
	v->items[v->count++] = x;
}

static void ivec_push_unique(VCBIntVec *v, int32_t x) {
	for (int32_t i = 0; i < v->count; i++)
		if (v->items[i] == x)
			return;
	ivec_push(v, x);
}

/* Resolve a trace entity to the representative of its bus/mesh-merged group. */
static int32_t rep_of(const VCBSim *s, int32_t k) {
	const VCBModel *m = s->model;
	if (m->net_rep && k >= 0 && k <= m->n_entities)
		return m->net_rep[k];
	return k;
}

/* ---- VMem helpers (kernel 0x28f8fa..0x28fb4b) ------------------------------ */

/* Pack a latch list's states as a weighted binary word (weight rotates left, so
 * bit 31 wraps back to bit 0 exactly as the original's `rol` does). */
static uint32_t vmem_pack(const VCBSim *s, const int32_t *list, int32_t n) {
	const VCBModel *m = s->model;
	uint32_t val = 0, w = 1;
	for (int32_t i = 0; i < n; i++) {
		int32_t e = list[i];
		if (e > 0 && e <= m->n_entities && m->ent[e].state)
			val += w;
		w = (w << 1) | (w >> 31);
	}
	return val;
}

/* ---------------------------------------------------------------------------
 * init / teardown
 * ------------------------------------------------------------------------- */

void vcb_sim_init(VCBSim *s, VCBModel *m) {
	memset(s, 0, sizeof(*s));
	s->model = m;
	s->snap_cursor = -1;
	const int32_t n = m->n_entities;

	s->n_high = (uint8_t *)calloc((size_t)n + 1, 1);
	s->n_in = (uint8_t *)calloc((size_t)n + 1, 1);
	s->adj = (VCBIntVec *)calloc((size_t)n + 1, sizeof(VCBIntVec));
	s->members = (VCBIntVec *)calloc((size_t)n + 1, sizeof(VCBIntVec));

	/* members[rep] = every trace entity sharing that representative, so a bus or
	 * mesh group presents one state to every trace pixel in it. */
	for (int32_t k = 1; k <= n; k++) {
		if (!m->ent[k].is_trace)
			continue;
		ivec_push(&s->members[rep_of(s, k)], k);
	}

	/* Directed adjacency, deduplicated per (component, net) exactly as the
	 * original: several READ pixels from one net onto one gate are a single input
	 * (verified: a 2-pixel READ into an XOR still reads as one input). */
	for (int32_t g = 1; g <= n; g++) {
		if (m->ent[g].is_trace)
			continue;
		for (int32_t j = 0; j < m->ent[g].outputs.count; j++)
			ivec_push_unique(&s->adj[g], rep_of(s, m->ent[g].outputs.items[j]));
		for (int32_t j = 0; j < m->ent[g].inputs.count; j++) {
			int32_t r = rep_of(s, m->ent[g].inputs.items[j]);
			ivec_push_unique(&s->adj[r], g);
		}
	}
	/* circuit_data byte [2] per entity: a component's deduplicated input-net count
	 * (what the gate handlers compare n_high against) and a net's driver count.
	 * Both are the entity's in-degree; vcb_model_indegree is the one definition,
	 * shared with the emitted circuit_data and the state texture. */
	vcb_model_indegree(m, s->n_in);

	/* VMem runtime: a mutable copy of the compiled image plus the latch lists. */
	if (m->vmem && m->vmem_len > 0) {
		s->vmem_len = m->vmem_len;
		s->vmem = (int32_t *)malloc((size_t)m->vmem_len * sizeof(int32_t));
		if (s->vmem)
			memcpy(s->vmem, m->vmem, (size_t)m->vmem_len * sizeof(int32_t));
	}
	if (m->vmem_addr_entities && m->vmem_addr_count > 0) {
		s->vmem_addr_n = m->vmem_addr_count;
		s->vmem_addr_ent = (int32_t *)malloc((size_t)m->vmem_addr_count * sizeof(int32_t));
		memcpy(s->vmem_addr_ent, m->vmem_addr_entities, (size_t)m->vmem_addr_count * sizeof(int32_t));
	}
	if (m->vmem_content_entities && m->vmem_content_count > 0) {
		s->vmem_content_n = m->vmem_content_count;
		s->vmem_content_ent = (int32_t *)malloc((size_t)m->vmem_content_count * sizeof(int32_t));
		memcpy(s->vmem_content_ent, m->vmem_content_entities,
				(size_t)m->vmem_content_count * sizeof(int32_t));
	}
	s->vmem_present = (s->vmem_addr_n > 0 || s->vmem_content_n > 0) ? 1 : 0;

	vcb_sim_seed_all(s);
}

void vcb_sim_free(VCBSim *s) {
	if (s->model) {
		const int32_t n = s->model->n_entities;
		if (s->adj)
			for (int32_t i = 0; i <= n; i++)
				free(s->adj[i].items);
		if (s->members)
			for (int32_t i = 0; i <= n; i++)
				free(s->members[i].items);
	}
	free(s->adj);
	free(s->members);
	free(s->n_high);
	free(s->n_in);
	free(s->cur_g.items);
	free(s->nxt_n.items);
	free(s->fired_breakpoints.items);
	free(s->vmem_occ_addr.items);
	free(s->vmem_occ_content.items);
	for (int i = 0; i < s->snap_count; i++)
		free(s->snaps[i]);
	free(s->snaps);
	free(s->vmem);
	free(s->vmem_addr_ent);
	free(s->vmem_content_ent);
	memset(s, 0, sizeof(*s));
	s->snap_cursor = -1;
}

void vcb_sim_clear_fired(VCBSim *s) {
	s->fired_breakpoints.count = 0;
	s->vmem_occ_addr.count = 0;
	s->vmem_occ_content.count = 0;
}

void vcb_sim_set_clock(VCBSim *s, int32_t interval) { s->clock_interval = interval; }
void vcb_sim_set_timer_us(VCBSim *s, int64_t interval_us) { s->timer_interval_us = interval_us; }

void vcb_sim_add_time_us(VCBSim *s, int64_t elapsed_us, int64_t paused_us) {
	s->timer_elapsed_us += elapsed_us;
	s->timer_paused_us += paused_us;
}
void vcb_sim_set_seed(VCBSim *s, uint32_t seed) { mt_seed(s, seed); }

/* Recompute every accumulator from the current states. Used at init and after a
 * snapshot restore, where states changed out from under the work lists. */
static void resync_accumulators(VCBSim *s) {
	VCBModel *m = s->model;
	const int32_t n = m->n_entities;
	memset(s->n_high, 0, (size_t)n + 1);
	for (int32_t k = 1; k <= n; k++) {
		if (m->ent[k].is_trace && rep_of(s, k) != k)
			continue; /* only representatives carry a tally */
		if (!m->ent[k].state)
			continue;
		for (int32_t j = 0; j < s->adj[k].count; j++)
			s->n_high[s->adj[k].items[j]]++;
	}
}

/* construct_model (0x3e3550) appends exactly the entities whose ink is in
 * 0x05..0x09 -- NOT, NAND, NOR, XNOR, LATCH_ON -- to the model's seed list, as
 * **positive** indices. That sign is what makes a LATCH_ON come up HIGH on the
 * first tick (its handler is `(entry > 0) XOR state`), while LATCH_OFF (0x0a),
 * being outside the range, is never seeded and stays LOW. */
void vcb_sim_seed_all(VCBSim *s) {
	VCBModel *m = s->model;
	const int32_t n = m->n_entities;
	s->cur_g.count = 0;
	s->nxt_n.count = 0;
	resync_accumulators(s);
	for (int32_t k = 1; k <= n; k++) {
		if (m->ent[k].is_trace)
			continue;
		if (m->ent[k].ink >= 0x05 && m->ent[k].ink <= 0x09)
			ivec_push(&s->cur_g, k); /* positive */
	}
	s->active_ready = 1;
}

/* Commit a component's new output and propagate it (PASS 1's tail, 0x28f710). */
static void commit_and_propagate(VCBSim *s, int32_t k, uint8_t out) {
	VCBModel *m = s->model;
	m->ent[k].state = out;
	int delta = (int)out * 2 - 1;
	for (int32_t j = 0; j < s->adj[k].count; j++) {
		int32_t c = s->adj[k].items[j];
		s->n_high[c] = (uint8_t)(s->n_high[c] + delta);
		ivec_push(&s->nxt_n, c); /* nets are pushed unsigned */
	}
}

/* ---------------------------------------------------------------------------
 * the tick
 * ------------------------------------------------------------------------- */

void vcb_sim_tick(VCBSim *s) {
	VCBModel *m = s->model;
	const int32_t n = m->n_entities;
	if (!s->active_ready)
		vcb_sim_seed_all(s);

	/* CLOCK: `current_tick % clock_interval == 0` schedules the one CLOCK entity
	 * construct_model recorded, positively (0x28f4a0). Its handler toggles. */
	if (s->clock_interval > 0 && m->clock_value > 0 && m->clock_value <= n &&
	    (s->tick % s->clock_interval) == 0)
		ivec_push(&s->cur_g, m->clock_value);
	/* TIMER: real time, not ticks (0x28f4c0). The kernel fires it when
	 * `interval_us + paused_us < elapsed_us` since the last fire, then resets the
	 * paused accumulator. With no wall time fed in, it never fires -- which is what
	 * the original does too when solves are back to back. */
	if (s->timer_interval_us > 0 && m->timer_value > 0 && m->timer_value <= n &&
	    s->timer_interval_us + s->timer_paused_us < s->timer_elapsed_us) {
		ivec_push(&s->cur_g, m->timer_value);
		s->timer_paused_us = 0;
		s->timer_elapsed_us = 0;
	}

	/* ---- PASS 1: components ---- */
	for (int32_t i = 0; i < s->cur_g.count; i++) {
		int32_t v = s->cur_g.items[i];
		int32_t k = v < 0 ? -v : v;
		if (k <= 0 || k > n)
			continue;
		VCBEntity *e = &m->ent[k];
		const uint8_t ink = e->ink;
		const uint8_t state = e->state;
		const int rising = v > 0; /* the scheduling net went up */
		uint8_t out;

		switch (ink) {
			case 0x09: /* LATCH_ON  0x28f58f */
			case 0x0a: /* LATCH_OFF 0x28f58f */
				out = (uint8_t)(rising ^ state);
				break;
			case 0x0b: /* CLOCK  0x28f5a0 */
			case 0x0f: /* VINPUT 0x28f5a0 */
			case 0x10: /* TIMER  0x28f5a0 */
				out = (uint8_t)(state ^ 1);
				break;
			case 0x0d: /* VMEM address latch 0x28f5ab */
			case 0x0e: /* VMEM content latch 0x28f617 */
				if (s->vmem_lock) {
					/* already latched this tick: a rising schedule is recorded as an
					 * occurrence (the event-log timestamps) instead of toggling. */
					if (rising)
						ivec_push(ink == 0x0d ? &s->vmem_occ_addr : &s->vmem_occ_content,
								(int32_t)s->tick);
					continue;
				}
				out = (uint8_t)(rising ^ state);
				if (out == state)
					continue;
				if (ink == 0x0d)
					s->vmem_addr_changed = 1;
				else
					s->vmem_content_changed = 1;
				commit_and_propagate(s, k, out);
				continue;
			case 0x11: /* BREAKPOINT 0x28f656 */
				out = (uint8_t)(s->n_high[k] != 0);
				if (s->n_high[k] != 0 && state == 0) {
					int dup = 0;
					for (int32_t j = 0; j < s->fired_breakpoints.count; j++)
						if (s->fired_breakpoints.items[j] == k)
							dup = 1;
					if (!dup)
						ivec_push(&s->fired_breakpoints, k);
					s->breakpoint_hit = 1; /* the kernel sets the tick budget to 1 */
				}
				break;
			case 0x12: /* RANDOM 0x28f6cf */
				out = (uint8_t)((((mt_next(s) & 1u) | state) & (uint32_t)(rising ? 1 : 0)) & 1u);
				break;
			default:
				out = (uint8_t)vcb_gate_eval(ink, s->n_high[k], s->n_in[k]);
				break;
		}
		if (out == state)
			continue;
		commit_and_propagate(s, k, out);
	}

	s->events += s->nxt_n.count; /* 0x28f7e9: events += |net list| */
	s->cur_g.count = 0;          /* 0x28f7fb: +0x228 = +0x220 */

	/* ---- PASS 2: nets (wired-OR of their drivers) ---- */
	for (int32_t i = 0; i < s->nxt_n.count; i++) {
		int32_t k = s->nxt_n.items[i];
		if (k <= 0 || k > n)
			continue;
		uint8_t out = (uint8_t)(s->n_high[k] != 0);
		if (out == m->ent[k].state)
			continue;
		m->ent[k].state = out;
		/* every trace sharing this representative shows the group's state */
		VCBIntVec *mem = &s->members[k];
		for (int32_t j = 0; j < mem->count; j++)
			m->ent[mem->items[j]].state = out;
		int delta = (int)out * 2 - 1;
		for (int32_t j = 0; j < s->adj[k].count; j++) {
			int32_t c = s->adj[k].items[j];
			s->n_high[c] = (uint8_t)(s->n_high[c] + delta);
			ivec_push(&s->cur_g, c * delta); /* signed: carries the direction */
		}
	}

	s->events += s->cur_g.count; /* 0x28f8ec: events += |gate list| */
	s->nxt_n.count = 0;
	s->vmem_lock = 0;            /* 0x28f906 */

	/* ---- VMem write / address+read-back (0x28f8fa..0x28fb4b) ---- */
	if (s->vmem_content_changed && s->vmem && s->vmem_len > 0) {
		uint32_t val = vmem_pack(s, s->vmem_content_ent, s->vmem_content_n);
		if (s->vmem_address >= 0 && s->vmem_address < s->vmem_len)
			s->vmem[s->vmem_address] = (int32_t)val;
	}
	if (s->vmem_addr_changed && s->vmem && s->vmem_len > 0) {
		s->vmem_address = (int32_t)vmem_pack(s, s->vmem_addr_ent, s->vmem_addr_n);
		if (s->vmem_address >= 0 && s->vmem_address < s->vmem_len) {
			uint32_t word = (uint32_t)s->vmem[s->vmem_address];
			uint32_t w = 1;
			for (int32_t i = 0; i < s->vmem_content_n; i++) {
				int32_t e = s->vmem_content_ent[i];
				uint8_t bit = (uint8_t)((word & w) != 0);
				if (e > 0 && e <= n && bit != m->ent[e].state) {
					m->ent[e].state = bit;
					int delta = (int)bit * 2 - 1;
					for (int32_t j = 0; j < s->adj[e].count; j++) {
						int32_t c = s->adj[e].items[j];
						s->n_high[c] = (uint8_t)(s->n_high[c] + delta);
						ivec_push(&s->nxt_n, c); /* processed by the next PASS 2 */
					}
				}
				w = (w << 1) | (w >> 31);
			}
		}
		s->vmem_lock = 1; /* 0x28fb4b */
	}
	s->vmem_addr_changed = 0;
	s->vmem_content_changed = 0;

	s->tick++;
}

/* The kernel's per-ink handlers are inherently event-driven (a LATCH toggles on
 * the *sign* of the entry that scheduled it, RANDOM draws only when scheduled),
 * so there is no separate synchronous formulation to cross-check against; this
 * is kept as the documented alias the tests and the module already call. */
void vcb_sim_tick_sync(VCBSim *s) { vcb_sim_tick(s); }

void vcb_sim_run(VCBSim *s, int n) {
	for (int i = 0; i < n; i++) {
		vcb_sim_tick(s);
		if (s->breakpoint_hit) {
			/* the BREAKPOINT handler sets the tick budget to 1, so the run stops
			 * after the tick that fired (0x28f6a7). */
			s->breakpoint_hit = 0;
			break;
		}
	}
}

/* An entity written from outside the tick loop (mouse override / virtual input):
 * propagate the value it now holds so its readers see it, exactly as PASS 1's
 * commit would have. */
void vcb_sim_mark_external(VCBSim *s, int32_t entity) {
	VCBModel *m = s->model;
	const int32_t n = m->n_entities;
	if (entity <= 0 || entity > n)
		return;
	int32_t k = entity;
	if (m->ent[k].is_trace) {
		k = rep_of(s, k);
		uint8_t v = m->ent[entity].state;
		m->ent[k].state = v;
		VCBIntVec *mem = &s->members[k];
		for (int32_t j = 0; j < mem->count; j++)
			m->ent[mem->items[j]].state = v;
		int delta = (int)v * 2 - 1;
		for (int32_t j = 0; j < s->adj[k].count; j++) {
			int32_t c = s->adj[k].items[j];
			s->n_high[c] = (uint8_t)(s->n_high[c] + delta);
			ivec_push(&s->cur_g, c * delta);
		}
		return;
	}
	commit_and_propagate(s, k, m->ent[k].state);
}

/* ---- snapshot history ---------------------------------------------------- */
static void snap_drop_future(VCBSim *s) {
	for (int i = s->snap_cursor + 1; i < s->snap_count; i++)
		free(s->snaps[i]);
	if (s->snap_cursor + 1 < s->snap_count)
		s->snap_count = s->snap_cursor + 1;
}

void vcb_sim_snapshot_take(VCBSim *s) {
	snap_drop_future(s);
	int32_t n = s->model->n_entities + 1;
	uint8_t *snap = (uint8_t *)malloc((size_t)n);
	if (!snap)
		return;
	for (int32_t k = 0; k < n; k++)
		snap[k] = s->model->ent[k].state;
	if (s->snap_count == s->snap_cap) {
		int nc = s->snap_cap ? s->snap_cap * 2 : 8;
		s->snaps = (uint8_t **)realloc(s->snaps, (size_t)nc * sizeof(uint8_t *));
		s->snap_cap = nc;
	}
	s->snaps[s->snap_count++] = snap;
	s->snap_cursor = s->snap_count - 1;
}

int vcb_sim_snapshot_prev_possible(VCBSim *s) { return s->snap_cursor > 0; }
int vcb_sim_snapshot_next_possible(VCBSim *s) {
	return s->snap_cursor >= 0 && s->snap_cursor < s->snap_count - 1;
}

static void snap_apply(VCBSim *s, int idx) {
	VCBModel *m = s->model;
	int32_t n = m->n_entities + 1;
	const uint8_t *snap = s->snaps[idx];
	for (int32_t k = 0; k < n; k++)
		m->ent[k].state = snap[k];
	/* the work lists describe a state we just discarded: rebuild the tallies and
	 * start from a quiet scheduler. */
	s->cur_g.count = 0;
	s->nxt_n.count = 0;
	resync_accumulators(s);
}

void vcb_sim_snapshot_restore_prev(VCBSim *s) {
	if (s->snap_cursor > 0)
		snap_apply(s, --s->snap_cursor);
}

void vcb_sim_snapshot_restore_next(VCBSim *s) {
	if (s->snap_cursor >= 0 && s->snap_cursor < s->snap_count - 1)
		snap_apply(s, ++s->snap_cursor);
}

void vcb_sim_snapshot_clear_all(VCBSim *s) {
	for (int i = 0; i < s->snap_count; i++)
		free(s->snaps[i]);
	free(s->snaps);
	s->snaps = NULL;
	s->snap_count = s->snap_cap = 0;
	s->snap_cursor = -1;
}

void vcb_sim_snapshot_clear_next(VCBSim *s) { snap_drop_future(s); }
