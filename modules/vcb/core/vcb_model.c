/* vcb_model.c -- builds the compiled circuit graph from the compiler pipeline's
 * entities (the build_stage2 + construct_model back-half, vcb.exe 0x3e1e40 /
 * 0x3e3550). It assigns entity indices (components 1..A, then trace nets), builds
 * the pixel->entity LUT, forms gate<->net edges (a WRITE junction is a gate output,
 * a READ an input), emits the flat circuit_data {state, ink, n_conn, 0} + adjacency,
 * and merges bus/mesh/wireless nets per colour/channel via net_rep. Godot-free;
 * unit-tested in core/test. See vcb_model.h and docs/compiler_pipeline.md. */
#include "vcb_model.h"
#include "vcb_bus.h"
#include "vcb_vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- small int-vector helpers ---- */
static void ivec_push_unique(VCBIntVec *v, int32_t x) {
	for (int32_t i = 0; i < v->count; i++)
		if (v->items[i] == x)
			return;
	if (v->count == v->cap) {
		int32_t nc = v->cap ? v->cap * 2 : 4;
		v->items = (int32_t *)realloc(v->items, (size_t)nc * sizeof(int32_t));
		v->cap = nc;
	}
	v->items[v->count++] = x;
}

static void ivec_free(VCBIntVec *v) {
	free(v->items);
	v->items = NULL;
	v->count = v->cap = 0;
}

/* ---- union-find over entity indices (bus net merge). Representative = the
 * lowest entity index in the group. ---- */
static int32_t uf_find(int32_t *rep, int32_t x) {
	while (rep[x] != x) {
		rep[x] = rep[rep[x]];
		x = rep[x];
	}
	return x;
}
static void uf_union(int32_t *rep, int32_t a, int32_t b) {
	int32_t ra = uf_find(rep, a), rb = uf_find(rep, b);
	if (ra == rb)
		return;
	if (ra < rb)
		rep[rb] = ra;
	else
		rep[ra] = rb;
}

/* Add a directed gate->net edge plus the symmetric undirected connection. */
static void model_add_edge(VCBModel *m, int32_t gate, int32_t net, int is_output) {
	if (is_output)
		ivec_push_unique(&m->ent[gate].outputs, net);
	else
		ivec_push_unique(&m->ent[gate].inputs, net);
	ivec_push_unique(&m->ent[gate].conns, net);
	ivec_push_unique(&m->ent[net].conns, gate);
}

int vcb_model_build(TCAnalysisCtx *ctx, VCBModel *out) {
	memset(out, 0, sizeof(*out));
	const int32_t side = ctx->side;
	out->board_side = side;
	out->sidelength = ctx->entitylist_sidelength;

	void **comps = (void **)ctx->entities_a.begin;
	const int32_t A = (int32_t)TCVec_count(&ctx->entities_a, sizeof(void *));
	TCVec *tgroups = (TCVec *)ctx->raw_traces.begin;
	const int32_t B = (int32_t)TCVec_count(&ctx->raw_traces, sizeof(TCVec));

	out->n_components = A;
	out->n_entities = A + B;
	out->ent = (VCBEntity *)calloc((size_t)out->n_entities + 1, sizeof(VCBEntity));
	if (!out->ent)
		return 1;
	if (side > 0) {
		out->lut = (int32_t *)calloc((size_t)side * side, sizeof(int32_t));
		if (!out->lut) {
			free(out->ent);
			out->ent = NULL;
			return 1;
		}
	}

	const uint8_t *cls = (const uint8_t *)ctx->classified.begin;

	/* --- components: indices 1..A. ink = first body pixel's ink. --- */
	for (int32_t i = 0; i < A; i++) {
		int32_t idx = 1 + i;
		TCEntity *e = (TCEntity *)comps[i];
		VCBEntity *me = &out->ent[idx];
		me->is_trace = 0;
		me->ink = 0;
		TCPixel *bp = (TCPixel *)e->body.begin;
		TCPixel *be = (TCPixel *)e->body.end;
		if (bp != be)
			me->ink = (uint8_t)bp->ink;
		for (TCPixel *p = bp; p != be; p++) {
			if (out->lut && p->x >= 0 && p->x < side && p->y >= 0 && p->y < side)
				out->lut[(size_t)p->y * side + p->x] = idx;
		}
		/* Every entity starts at 0. LATCH_ON does not get a special initial state:
		 * it comes up HIGH because construct_model seeds inks 0x05..0x09 into the
		 * kernel's work list as positive entries, and the latch handler is
		 * `(entry > 0) XOR state` -- so the seed itself flips it on the first tick.
		 * Presetting it here would make that seed a no-op and lose the event. */
		if (me->ink == 0x0b)
			out->clock_value = idx; /* CLOCK entity -> stat_150 */
		if (me->ink == 0x10)
			out->timer_value = idx; /* TIMER entity -> stat_154 */
	}

	/* --- trace nets: indices A+1..A+B. ink forced 0xff (as construct_model). --- */
	for (int32_t i = 0; i < B; i++) {
		int32_t idx = 1 + A + i;
		VCBEntity *me = &out->ent[idx];
		me->is_trace = 1;
		me->ink = 0xff;
		TCVec *g = &tgroups[i];
		for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++) {
			if (out->lut && p->x >= 0 && p->x < side && p->y >= 0 && p->y < side)
				out->lut[(size_t)p->y * side + p->x] = idx;
		}
	}

	/* --- edges: scan each component's body pixels' 4-neighbours. A component is
	 * wired to a net ONLY through a junction pixel: READ (0xfe) makes that net an
	 * input, WRITE (0xff) makes it an output. Plain trace colours touching a
	 * component body do NOT connect it -- verified against the original engine
	 * (tools/phasec/probes: a LED adjacent to a bare trace stays dark, the same
	 * LED behind a READ lights). Edges are deduplicated per (component, net), so
	 * several READ pixels onto one net still count as a single input; that is what
	 * makes a 2-pixel-wide READ into an XOR behave as one input. --- */
	static const int DX[4] = { -1, 1, 0, 0 };
	static const int DY[4] = { 0, 0, -1, 1 };
	for (int32_t i = 0; i < A; i++) {
		int32_t gate = 1 + i;
		TCEntity *e = (TCEntity *)comps[i];
		for (TCPixel *p = (TCPixel *)e->body.begin; p != (TCPixel *)e->body.end; p++) {
			for (int d = 0; d < 4; d++) {
				int32_t nx = p->x + DX[d], ny = p->y + DY[d];
				if (nx < 0 || nx >= side || ny < 0 || ny >= side)
					continue;
				size_t nf = (size_t)ny * side + nx;
				int32_t ni = out->lut ? out->lut[nf] : 0;
				if (ni == 0 || ni == gate || !out->ent[ni].is_trace)
					continue;
				uint8_t nink = cls ? TC_CLASSIFIED_INK(cls, nf) : 0;
				if (nink != 0xfe && nink != 0xff)
					continue; /* not a junction: no connection */
				/* CLOCK (0x0b), VINPUT (0x0f) and TIMER (0x10) share one kernel handler
				 * (0x28f5a0: `state ^ 1`) and take no input connections: the CLOCK and
				 * TIMER are driven only by the kernel's interval scheduler, the VINPUT
				 * only by the virtual-input sweep. A READ junction drawn against any of
				 * them is inert -- verified against the original, where a CLOCK behind a
				 * READ whose net goes high keeps its interval phase, and a VINPUT behind
				 * a live READ stays low. They still drive nets through WRITE junctions. */
				if (nink == 0xfe && (out->ent[gate].ink == 0x0b || out->ent[gate].ink == 0x0f ||
						out->ent[gate].ink == 0x10))
					continue;
				model_add_edge(out, gate, ni, nink == 0xff /* WRITE -> output */);
			}
		}
	}

	/* --- bus merge: a bus is a multi-channel conductor. Traces touch it with a
	 * given ink (colour), and only SAME-COLOUR traces on the same bus net connect
	 * -- each trace colour is an independent channel, and READ (0xfe) / WRITE
	 * (0xff) are their own channels too. Bus nets are 4-connected same-ink
	 * (0xe8..0xed) pixel components; ctx->bus_connections records which trace pixel
	 * touches which bus pixel, and cls gives that trace pixel's colour. --- */
	enum { TC_TRACE_INK0 = 0xee, TC_TRACE_NINK = 18 }; /* 0xee..0xff */
	out->net_rep = (int32_t *)malloc((size_t)(out->n_entities + 1) * sizeof(int32_t));
	if (out->net_rep) {
		for (int32_t k = 0; k <= out->n_entities; k++)
			out->net_rep[k] = k;
		if (side > 0 && cls && out->lut) {
			const int32_t n = side * side;
			/* One implementation of bus-net grouping, shared with the render data:
			 * any two adjacent bus pixels join whatever their colours, a bus runs
			 * straight through a CROSS and on through a TUNNEL pair, and every bus
			 * net of a given bus colour touching a MESH is one board-wide net. */
			int32_t *bnet = (int32_t *)malloc((size_t)n * sizeof(int32_t));
			if (bnet) {
				int32_t nnets = vcb_bus_label(ctx, bnet);
				/* per-(bus net, trace colour) union anchor; 0 = none yet */
				size_t nanchor = (size_t)(nnets > 0 ? nnets : 1) * TC_TRACE_NINK;
				int32_t *anchor = (int32_t *)calloc(nanchor, sizeof(int32_t));
				if (anchor) {
					const size_t nconn = TCVec_count(&ctx->bus_connections, sizeof(void *));
					void *const *keys = (void *const *)ctx->bus_connections.begin;
					for (size_t kk = 0; kk < nconn; kk++) {
						uint64_t key = (uint64_t)(uintptr_t)keys[kk];
						int fx = (int)(uint16_t)key, fy = (int)(uint16_t)(key >> 16);
						int bx = (int)(uint16_t)(key >> 32), by = (int)(uint16_t)(key >> 48);
						if (fx < 0 || fx >= side || fy < 0 || fy >= side ||
						    bx < 0 || bx >= side || by < 0 || by >= side)
							continue;
						int bn = bnet[by * side + bx];
						if (bn < 0)
							continue;
						size_t fflat = (size_t)fy * side + fx;
						int32_t te = out->lut[fflat];
						if (te <= 0 || te > out->n_entities || !out->ent[te].is_trace)
							continue;
						uint8_t fink = TC_CLASSIFIED_INK(cls, fflat);
						if (fink < TC_TRACE_INK0) /* only trace-ink pixels form channels */
							continue;
						size_t slot = (size_t)bn * TC_TRACE_NINK + (fink - TC_TRACE_INK0);
						if (anchor[slot] == 0)
							anchor[slot] = te;
						else
							uf_union(out->net_rep, te, anchor[slot]);
					}
					free(anchor);
				}
			}
			free(bnet);
			/* --- mesh merge: a MESH (ink 0x66) is a board-wide "wireless bus" --
			 * same-colour traces touching ANY mesh pixel join one global net per
			 * colour, regardless of position (mesh pixels need not be connected). --- */
			{
				int32_t manchor[TC_TRACE_NINK];
				for (int i = 0; i < TC_TRACE_NINK; i++)
					manchor[i] = 0;
				for (int32_t p = 0; p < n; p++) {
					if (TC_CLASSIFIED_INK(cls, p) != 0x66)
						continue;
					int px = p % side, py = p / side;
					for (int d = 0; d < 4; d++) {
						int nx = px + DX[d], ny = py + DY[d];
						if (nx < 0 || nx >= side || ny < 0 || ny >= side)
							continue;
						size_t nf = (size_t)ny * side + nx;
						int32_t te = out->lut[nf];
						if (te <= 0 || te > out->n_entities || !out->ent[te].is_trace)
							continue;
						uint8_t nink = TC_CLASSIFIED_INK(cls, nf);
						if (nink < TC_TRACE_INK0)
							continue;
						int slot = nink - TC_TRACE_INK0;
						if (manchor[slot] == 0)
							manchor[slot] = te;
						else
							uf_union(out->net_rep, te, manchor[slot]);
					}
				}
			}
			for (int32_t k = 1; k <= out->n_entities; k++)
				out->net_rep[k] = uf_find(out->net_rep, k);
		}
	}
	return 0;
}

void vcb_model_free(VCBModel *m) {
	if (m->ent) {
		for (int32_t i = 0; i <= m->n_entities; i++) {
			ivec_free(&m->ent[i].inputs);
			ivec_free(&m->ent[i].outputs);
			ivec_free(&m->ent[i].conns);
		}
		free(m->ent);
	}
	free(m->lut);
	free(m->net_rep);
	free(m->vmem);
	free(m->vmem_addr_entities);
	free(m->vmem_content_entities);
	memset(m, 0, sizeof(*m));
}

void vcb_model_emit_circuit_data(const VCBModel *m, uint8_t **out_data, int32_t *out_len) {
	int32_t side = m->sidelength > 0 ? m->sidelength : 1;
	int32_t n = side * side * 4;
	uint8_t *d = (uint8_t *)calloc((size_t)n, 1);
	for (int32_t k = 1; k <= m->n_entities && k < side * side; k++) {
		d[k * 4 + 0] = m->ent[k].state;
		d[k * 4 + 1] = m->ent[k].ink;
		d[k * 4 + 2] = (uint8_t)m->ent[k].conns.count;
		d[k * 4 + 3] = 0;
	}
	*out_data = d;
	*out_len = n;
}

void vcb_model_emit_adjacency(const VCBModel *m, int32_t **out_adj, int32_t *out_len) {
	/* worst-case length: leading 0 + each entity's conns + a 0 separator each. */
	int32_t total = 1;
	for (int32_t k = 1; k <= m->n_entities; k++)
		total += m->ent[k].conns.count + 1;
	int32_t *a = (int32_t *)malloc((size_t)total * sizeof(int32_t));
	int32_t w = 0;
	a[w++] = 0; /* leading sentinel (matches construct_model) */
	for (int32_t k = 1; k <= m->n_entities; k++) {
		for (int32_t j = 0; j < m->ent[k].conns.count; j++)
			a[w++] = m->ent[k].conns.items[j];
		a[w++] = 0; /* per-entity separator */
	}
	*out_adj = a;
	*out_len = w;
}

void vcb_model_build_inverse(const VCBModel *m, int32_t *out_x, int32_t *out_y) {
	for (int32_t e = 0; e <= m->n_entities; e++) {
		out_x[e] = -1;
		out_y[e] = -1;
	}
	if (!m->lut)
		return;
	const int bs = m->board_side;
	for (int f = 0; f < bs * bs; f++) {
		int32_t e = m->lut[f];
		if (e <= 0 || e > m->n_entities)
			continue;
		if (out_x[e] < 0) { /* first board pixel that maps to this entity */
			out_x[e] = f % bs;
			out_y[e] = f / bs;
		}
	}
}
