/* vcb_pipeline.c -- see vcb_pipeline.h. Reconstructed from vcb.exe; unit-tested
 * in vcb-engine-recovery (make simtest). */
#include "vcb_pipeline.h"
#include "vcb_bus.h"
#include "vcb_vec.h"
#include "vcb_classifier.h"
#include <string.h>

/* ---- connectivity / bookkeeping helpers ----
 * The binary keeps several of these as std::set / RB-tree structures. They are
 * reconstructed here from the documented behaviour (docs/compiler_pipeline.md);
 * the exact predicates are validated by the Phase-C differential harness. */

/* Orthogonal step table (LEFT, RIGHT, UP, DOWN). The binary keeps this as a
 * .rdata table indexed by the `dir` code, which is also the base of the
 * UNMATCHED_TUNNEL_* error codes. */
static const int TC_DIR_X[4] = { -1, 1, 0, 0 };
static const int TC_DIR_Y[4] = { 0, 0, -1, 1 };

/* Append a deduplicated {x, y, code} record to ctx->errors (error emitter
 * 0x3e0650). code is the TransistorCompiler enum surfaced in compiler.gd:
 * 0 UNEXPECTED_TUNNEL_ENTRANCE, 1..4 UNMATCHED_TUNNEL_{LEFT,RIGHT,UP,DOWN}. */
static void TC_emit_error(TCAnalysisCtx *ctx, int32_t x, int32_t y, int32_t code) {
	for (TCPixel *e = (TCPixel *)ctx->errors.begin; e != (TCPixel *)ctx->errors.end; e++)
		if (e->x == x && e->y == y && e->ink == code)
			return; /* already recorded */
	TCPixel rec = { x, y, code };
	TCVecPix_push(&ctx->errors, &rec);
}

/* Record a unique (from -> bus) connection edge. The binary dedups these in a
 * std::set<uint64_t> keyed by the packed endpoint coords (0x3dec60); reconstructed
 * here as a deduplicated list of 8-byte packed keys (coords fit in 16 bits each).
 * Consumed by model construction to link trace nets to the buses they touch. */
static void TC_record_bus_connection(TCAnalysisCtx *ctx, const TCPixel *from, const TCPixel *bus) {
	uint64_t key = ((uint64_t)(uint16_t)from->x) | ((uint64_t)(uint16_t)from->y << 16) |
	               ((uint64_t)(uint16_t)bus->x << 32) | ((uint64_t)(uint16_t)bus->y << 48);
	size_t n = TCVec_count(&ctx->bus_connections, sizeof(void *));
	void **keys = (void **)ctx->bus_connections.begin;
	for (size_t i = 0; i < n; i++)
		if ((uint64_t)(uintptr_t)keys[i] == key)
			return;
	TCVecPtr_push(&ctx->bus_connections, (void *)(uintptr_t)key);
}

/* Tunnel resolver (0x3dff40). From the tunnel entrance at `from + DIR[dir]`, walk
 * on in the same direction to the matching exit tunnel (the paired 0x65 pixel -- a
 * tunnel passes *under* whatever wires lie between the pair) and emit the pixel
 * just past the exit as the neighbour. If the board edge is reached with no exit,
 * emit an UNMATCHED_TUNNEL_<dir> error (code = dir + 1). The finer
 * UNEXPECTED_TUNNEL_ENTRANCE (code 0) diagnostic is a Phase-C refinement. */
static void TC_tunnel_resolve(TCAnalysisCtx *ctx, TCVec *out, const TCPixel *from, int dir) {
	const int32_t side = ctx->side;
	const uint8_t *buf = (const uint8_t *)ctx->classified.begin;
	const int dx = TC_DIR_X[dir], dy = TC_DIR_Y[dir];
	int32_t x = from->x + 2 * dx, y = from->y + 2 * dy; /* first cell past the entrance */
	while (x >= 0 && x < side && y >= 0 && y < side) {
		uint8_t ink = TC_CLASSIFIED_INK(buf, (size_t)y * side + x);
		if (ink == 0x65) { /* exit tunnel: emit the pixel just past it */
			int32_t ex = x + dx, ey = y + dy;
			if (ex >= 0 && ex < side && ey >= 0 && ey < side) {
				uint8_t eink = TC_CLASSIFIED_INK(buf, (size_t)ey * side + ex);
				TCPixel n = { ex, ey, eink };
				TCVecPix_push(out, &n);
			}
			return;
		}
		x += dx;
		y += dy;
	}
	TC_emit_error(ctx, from->x + dx, from->y + dy, dir + 1); /* UNMATCHED_TUNNEL_<dir> */
}

/* Mesh Phase 2 (prepare 0x3dd810+). In the binary this is an RB-tree pass that
 * groups the collected MESH pixels into nets. Here the actual mesh connectivity
 * (every trace touching a MESH pixel joins one board-wide net) is applied later
 * in vcb_model_build via the net_rep union, so this remains a stub kept as the
 * decoded entry point (the mesh pixel list is populated in prepare). */
static void TC_mesh_resolve(TCAnalysisCtx *ctx) { (void)ctx; }

/* Per-category pixel histogram (0x3e0cc0 -> stats map at ctx+0x58), read back by
 * get_stats. `cat` is the classifier's category byte (== GDScript STATSTYPE). */
static void TC_category_record(TCAnalysisCtx *ctx, uint8_t cat) { ctx->stats[cat]++; }

/* Per-category ENTITY histogram (0x3e0cc0 -> map at ctx+0x98), read back by
 * get_stats as the second stats group. `cat` is the STATSTYPE category. */
static void TC_entity_category_record(TCAnalysisCtx *ctx, uint8_t cat) { ctx->entity_stats[cat]++; }

/* -------------------------------------------------------------------------- */
/* get_neighbors: up-to-4 orthogonal neighbours, resolving CROSS/TUNNEL/MESH.  */
/* -------------------------------------------------------------------------- */
static int tc_is_bus(uint8_t ink) { return (uint8_t)(ink - 0xe8) <= 5; }

static void vcb_get_neighbors(TCAnalysisCtx *ctx, TCVec *out, const TCPixel *p) {
	static const int DX[4] = { -1, 1, 0, 0 };
	static const int DY[4] = { 0, 0, -1, 1 };
	const int32_t side = ctx->side;
	const uint8_t *buf = (const uint8_t *)ctx->classified.begin;
	out->begin = out->end = out->cap = 0;

	for (int dir = 0; dir < 4; dir++) {
		int32_t nx = p->x + DX[dir], ny = p->y + DY[dir];
		if (nx < 0 || nx >= side || ny < 0 || ny >= side)
			continue;
		size_t ni = (size_t)ny * side + nx;
		uint8_t ink1 = TC_CLASSIFIED_INK(buf, ni);
		if (ink1 == 0x64) { /* CROSS: skip over */
			int32_t mx = p->x + 2 * DX[dir], my = p->y + 2 * DY[dir];
			if (mx < 0 || mx >= side || my < 0 || my >= side)
				continue;
			size_t mi = (size_t)my * side + mx;
			uint8_t ink2 = TC_CLASSIFIED_INK(buf, mi);
			if (tc_is_bus(ink2) == tc_is_bus((uint8_t)p->ink)) {
				TCPixel n = { mx, my, ink2 };
				TCVecPix_push(out, &n);
			}
		} else if (ink1 == 0x65) { /* TUNNEL */
			TC_tunnel_resolve(ctx, out, p, dir);
		} else if (ink1 == 0x66) { /* MESH: board-wide net, resolved in vcb_model_build */
			/* Mesh pixels are not traversed during the flood; every trace touching a
			 * mesh pixel is unioned into one net later (net_rep). This branch is a
			 * no-op for the physical flood (a mesh neighbour is a dead-end here). */
			(void)0;
		} else {
			TCPixel n = { nx, ny, ink1 };
			TCVecPix_push(out, &n);
		}
	}
}

/* -------------------------------------------------------------------------- */
/* comp_fill / trace_fill: BFS flood-fill.                                     */
/* -------------------------------------------------------------------------- */
static void vcb_flood_component(TCAnalysisCtx *ctx, TCVec *out, const TCPixel *seed) {
	const int32_t side = ctx->side;
	uint8_t *buf = (uint8_t *)ctx->classified.begin;
	out->begin = out->end = out->cap = 0;
	TCVec frontier = { 0, 0, 0 }, buslist = { 0, 0, 0 };
	TCVecPix_push(out, seed);
	TCVecPix_push(&frontier, seed);
	TC_CLASSIFIED_MARK(buf, (size_t)seed->y * side + seed->x) = 1;

	while (frontier.begin != frontier.end) {
		TCPixel cur = *((TCPixel *)frontier.end - 1);
		frontier.end = (uint8_t *)frontier.end - sizeof(TCPixel);
		TCVec nb = { 0, 0, 0 };
		vcb_get_neighbors(ctx, &nb, &cur);
		for (TCPixel *n = (TCPixel *)nb.begin; n != (TCPixel *)nb.end; n++) {
			size_t ni = (size_t)n->y * side + n->x;
			if (TC_CLASSIFIED_MARK(buf, ni) != 0)
				continue;
			uint8_t nink = TC_CLASSIFIED_INK(buf, ni);
			TCPixel np = { n->x, n->y, nink };
			if (nink == (uint8_t)seed->ink)
				TCVecPix_push(out, &np);
			else if (tc_is_bus(nink))
				TCVecPix_push(&buslist, &np);
			else
				continue;
			TCVecPix_push(&frontier, &np);
			TC_CLASSIFIED_MARK(buf, ni) = 1;
		}
		TCVec_free(&nb);
	}
	for (TCPixel *b = (TCPixel *)buslist.begin; b != (TCPixel *)buslist.end; b++) {
		b->ink = 0xed;
		TC_CLASSIFIED_MARK(buf, (size_t)b->y * side + b->x) = 0;
		TCVecPix_push(out, b);
	}
	TCVec_free(&frontier);
	TCVec_free(&buslist);
}

static void vcb_flood_trace(TCAnalysisCtx *ctx, TCVec *out, const TCPixel *seed) {
	const int32_t side = ctx->side;
	uint8_t *buf = (uint8_t *)ctx->classified.begin;
	out->begin = out->end = out->cap = 0;
	TCVec frontier = { 0, 0, 0 };
	TCVecPix_push(out, seed);
	TCVecPix_push(&frontier, seed);
	TC_CLASSIFIED_MARK(buf, (size_t)seed->y * side + seed->x) = 1;

	while (frontier.begin != frontier.end) {
		TCPixel cur = *((TCPixel *)frontier.end - 1);
		frontier.end = (uint8_t *)frontier.end - sizeof(TCPixel);
		TCVec nb = { 0, 0, 0 };
		vcb_get_neighbors(ctx, &nb, &cur);
		for (TCPixel *n = (TCPixel *)nb.begin; n != (TCPixel *)nb.end; n++) {
			size_t ni = (size_t)n->y * side + n->x;
			if (TC_CLASSIFIED_MARK(buf, ni) != 0)
				continue;
			uint8_t nink = TC_CLASSIFIED_INK(buf, ni);
			if (nink >= 0xee) { /* any trace / READ / WRITE merges */
				TCPixel np = { n->x, n->y, nink };
				TCVecPix_push(out, &np);
				TCVecPix_push(&frontier, &np);
				TC_CLASSIFIED_MARK(buf, ni) = 1;
			} else if (tc_is_bus(nink)) {
				TC_record_bus_connection(ctx, &cur, n);
			}
		}
		TCVec_free(&nb);
	}
	TCVec_free(&frontier);
}

/* -------------------------------------------------------------------------- */
/* Stage 1: prepare.                                                           */
/* -------------------------------------------------------------------------- */
void vcb_prepare(TCAnalysisCtx *ctx, const uint8_t *rgba, int32_t len) {
	int32_t nbytes = len / 2; /* 2 bytes/pixel */
	uint8_t *buf = (uint8_t *)TC_alloc_bytes((size_t)nbytes);
	memset(buf, 0, (size_t)nbytes);
	ctx->classified.begin = buf;
	ctx->classified.end = ctx->classified.cap = buf + nbytes;

	int32_t npix = len / 4;
	int32_t side = ctx->side;
	for (int32_t i = 0; i < npix; i++) {
		uint32_t c = ((uint32_t)rgba[i * 4] << 16) | ((uint32_t)rgba[i * 4 + 1] << 8) |
		             (uint32_t)rgba[i * 4 + 2];
		uint8_t cat = 0;
		uint8_t ink = TC_classify_color(c, &cat);
		buf[i * 2] = ink;
		buf[i * 2 + 1] = (ink == 0) ? 1 : 0;
		TC_category_record(ctx, cat);
		if (ink == 0x66) {
			TCPixel m = { i % side, i / side, 0x66 };
			TCVecPix_push(&ctx->mesh_pixels, &m);
		}
	}
	/* Global-net inks (net_table): every pixel of these inks anywhere on the board
	 * belongs to ONE entity. vcb_link merges all same-ink component groups into a
	 * single entity per ink. Verified against the original engine: two CLOCK pixels
	 * at opposite ends of the board compile to the same entity index, and so do two
	 * TIMER pixels and two same-channel WIRELESS pixels -- while two NOT, LED, LATCH,
	 * RANDOM, BREAKPOINT, VINPUT or VMEM pixels do not.
	 *   0x0b CLOCK, 0x10 TIMER   -- the oscillators construct_model records as the
	 *                               single clock/timer index (model +0x150 / +0x154),
	 *                               which is only well-defined because they merge;
	 *   0x13..0x16 WIRELESS_0..3 -- each channel buffers its READ inputs onto its
	 *                               WRITE outputs everywhere on the board. */
	{
		uint8_t *nt = (uint8_t *)TC_alloc_bytes(6);
		nt[0] = 0x0b;
		nt[1] = 0x10;
		nt[2] = 0x13;
		nt[3] = 0x14;
		nt[4] = 0x15;
		nt[5] = 0x16;
		ctx->net_table.begin = nt;
		ctx->net_table.end = nt + 6;
		ctx->net_table.cap = nt + 6;
	}
	TC_mesh_resolve(ctx);
}

/* -------------------------------------------------------------------------- */
/* Stage 2: scan_pixels.                                                       */
/* -------------------------------------------------------------------------- */
void vcb_scan_pixels(TCAnalysisCtx *ctx) {
	const int32_t side = ctx->side;
	uint8_t *buf = (uint8_t *)ctx->classified.begin;
	if (side == 0)
		return;
	for (int32_t y = 0; y < side; y++) {
		for (int32_t x = 0; x < side; x++) {
			size_t i = (size_t)y * side + x;
			uint8_t ink = TC_CLASSIFIED_INK(buf, i);
			if (TC_CLASSIFIED_MARK(buf, i) != 0)
				continue;
			if ((uint8_t)(ink - 0x64) <= 2)
				continue; /* CROSS/TUNNEL/MESH */
			if ((uint8_t)(ink + 0x18) <= 5)
				continue; /* buses 0xe8..0xed */
			TCPixel seed = { x, y, ink };
			TCVec group = { 0, 0, 0 };
			if (ink >= 0xee) {
				vcb_flood_trace(ctx, &group, &seed);
				TCVecGrp_push(&ctx->raw_traces, &group);
			} else {
				vcb_flood_component(ctx, &group, &seed);
				TCVecGrp_push(&ctx->raw_components, &group);
			}
			ctx->processed++;
		}
	}
}

/* Drop the groups a merge emptied, keeping the rest in order. */
static void TC_compact_component_groups(TCAnalysisCtx *ctx) {
	TCVec *groups = (TCVec *)ctx->raw_components.begin;
	size_t ngroups = TCVec_count(&ctx->raw_components, sizeof(TCVec));
	TCVec *w = groups;
	for (size_t gi = 0; gi < ngroups; gi++)
		if (groups[gi].begin != groups[gi].end)
			*w++ = groups[gi];
	ctx->raw_components.end = w;
}

/* Does any body pixel of this group touch a MESH pixel (4-connected)? */
static int TC_group_touches_mesh(const TCAnalysisCtx *ctx, const TCVec *g) {
	static const int DX[4] = { -1, 1, 0, 0 };
	static const int DY[4] = { 0, 0, -1, 1 };
	const int32_t side = ctx->side;
	const uint8_t *buf = (const uint8_t *)ctx->classified.begin;
	if (!buf)
		return 0;
	for (const TCPixel *p = (const TCPixel *)g->begin; p != (const TCPixel *)g->end; p++) {
		/* A component group also carries the bus pixels the flood attached to it
		 * (marked 0xed); only the component's own body counts as touching a mesh. */
		if (p->ink == 0xed)
			continue;
		for (int d = 0; d < 4; d++) {
			int32_t nx = p->x + DX[d], ny = p->y + DY[d];
			if (nx < 0 || nx >= side || ny < 0 || ny >= side)
				continue;
			if (TC_CLASSIFIED_INK(buf, (size_t)ny * side + nx) == 0x66)
				return 1;
		}
	}
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Stage 3: link (global-net merge).                                           */
/* -------------------------------------------------------------------------- */
void vcb_link(TCAnalysisCtx *ctx) {
	/* MESH merges by ink, board-wide -- and that applies to COMPONENTS as well as
	 * traces. Every component group of a given ink that touches any mesh pixel
	 * anywhere becomes one entity, so two NOT gates on opposite corners of the
	 * board, each with a mesh pixel against them, are a single gate that sees both
	 * bodies' inputs and drives both bodies' outputs. Verified against the original:
	 * three mesh-touching LEDs compile to one entity index and light together, two
	 * mesh-touching ANDs share an entity so feeding one lights the other's output,
	 * while a mesh-touching NOT and a mesh-touching AND stay separate (different
	 * inks). The trace side of the same rule is per trace colour and is applied as
	 * a net merge in vcb_model_build. */
	{
		TCVec *groups = (TCVec *)ctx->raw_components.begin;
		size_t ngroups = TCVec_count(&ctx->raw_components, sizeof(TCVec));
		int32_t anchor[256];
		for (int i = 0; i < 256; i++)
			anchor[i] = -1;
		for (size_t gi = 0; gi < ngroups; gi++) {
			TCVec *g = &groups[gi];
			if (g->begin == g->end)
				continue;
			if (!TC_group_touches_mesh(ctx, g))
				continue;
			uint8_t ink = (uint8_t)((TCPixel *)g->begin)->ink;
			if (anchor[ink] < 0) {
				anchor[ink] = (int32_t)gi;
			} else {
				TCVec *tgt = &groups[anchor[ink]];
				for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++)
					TCVecPix_push(tgt, p);
				g->end = g->begin;
			}
		}
	}

	/* A BUS is the same kind of conductor: every component group of a given ink
	 * that touches a given bus net becomes one entity. Verified against the
	 * original -- two LEDs touching one bus net compile to a single entity index,
	 * as do two XORs, while two LEDs on separate bus nets stay distinct. (Touching
	 * a bus does not *wire* a component to the bus's traces: that still needs a
	 * READ or WRITE junction.) Bus nets already carry the mesh merge, so a
	 * mesh-spanned bus merges components board-wide too. */
	if (ctx->side > 0 && ctx->classified.begin) {
		const int32_t side = ctx->side;
		int32_t *blabel = (int32_t *)TC_alloc_bytes((size_t)side * side * sizeof(int32_t));
		if (blabel) {
			int32_t nbus = vcb_bus_label(ctx, blabel);
			if (nbus > 0) {
				static const int DX[4] = { -1, 1, 0, 0 };
				static const int DY[4] = { 0, 0, -1, 1 };
				size_t nslot = (size_t)nbus * 256;
				int32_t *banchor = (int32_t *)TC_alloc_bytes(nslot * sizeof(int32_t));
				if (banchor) {
					for (size_t i = 0; i < nslot; i++)
						banchor[i] = -1;
					TCVec *groups = (TCVec *)ctx->raw_components.begin;
					size_t ngroups = TCVec_count(&ctx->raw_components, sizeof(TCVec));
					for (size_t gi = 0; gi < ngroups; gi++) {
						TCVec *g = &groups[gi];
						if (g->begin == g->end)
							continue;
						uint8_t ink = (uint8_t)((TCPixel *)g->begin)->ink;
						for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++) {
							if (p->ink == 0xed)
								continue; /* attached bus pixel, not the body */
							for (int d = 0; d < 4; d++) {
								int32_t nx = p->x + DX[d], ny = p->y + DY[d];
								if (nx < 0 || nx >= side || ny < 0 || ny >= side)
									continue;
								int32_t bn = blabel[(size_t)ny * side + nx];
								if (bn < 0)
									continue;
								size_t slot = (size_t)bn * 256 + ink;
								if (banchor[slot] < 0) {
									banchor[slot] = (int32_t)gi;
								} else if (banchor[slot] != (int32_t)gi) {
									TCVec *tgt = &groups[banchor[slot]];
									for (TCPixel *q = (TCPixel *)g->begin;
											q != (TCPixel *)g->end; q++)
										TCVecPix_push(tgt, q);
									g->end = g->begin;
									break;
								}
							}
							if (g->begin == g->end)
								break;
						}
					}
					TC_free_bytes(banchor);
				}
			}
			TC_free_bytes(blabel);
		}
	}

	const uint8_t *net = (const uint8_t *)ctx->net_table.begin;
	size_t nslots = (uint8_t *)ctx->net_table.end - (uint8_t *)ctx->net_table.begin;
	if (nslots == 0) {
		TC_compact_component_groups(ctx);
		return;
	}
	int32_t *label = (int32_t *)TC_alloc_bytes(nslots * sizeof(int32_t));
	for (size_t i = 0; i < nslots; i++)
		label[i] = -1;
	TCVec *groups = (TCVec *)ctx->raw_components.begin;
	size_t ngroups = TCVec_count(&ctx->raw_components, sizeof(TCVec));
	for (size_t gi = 0; gi < ngroups; gi++) {
		TCVec *g = &groups[gi];
		if (g->begin == g->end)
			continue;
		uint8_t ink = (uint8_t)((TCPixel *)g->begin)->ink;
		size_t slot = nslots;
		for (size_t s = 0; s < nslots; s++)
			if (net[s] == ink) { slot = s; break; }
		if (slot == nslots)
			continue;
		if (label[slot] < 0) {
			label[slot] = (int32_t)gi;
		} else {
			TCVec *tgt = &groups[label[slot]];
			for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++)
				TCVecPix_push(tgt, p);
			g->end = g->begin;
		}
	}
	TC_free_bytes(label);
	TC_compact_component_groups(ctx);
}

/* -------------------------------------------------------------------------- */
/* Stage 4: finalize (groups -> entities).                                     */
/* -------------------------------------------------------------------------- */
void vcb_finalize(TCAnalysisCtx *ctx) {
	for (TCVec *g = (TCVec *)ctx->raw_components.begin;
	     g != (TCVec *)ctx->raw_components.end; g++) {
		TCEntity *e = (TCEntity *)TC_op_new(sizeof(TCEntity));
		for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++) {
			if (p->ink == 0xed)
				TCVecPix_push(&e->connections, p);
			else
				TCVecPix_push(&e->body, p);
		}
		TCVecPtr_push(&ctx->entities_a, e);
		/* The entity histogram is keyed by STATSTYPE category; the component's
		 * type is its body ink, so map ink -> category (0x02 AND -> 9, etc). */
		uint8_t ink = (e->body.begin != e->body.end)
		                  ? (uint8_t)((TCPixel *)e->body.begin)->ink : 0;
		TC_entity_category_record(ctx, TC_category_from_ink(ink));
	}
	for (TCVec *g = (TCVec *)ctx->raw_traces.begin;
	     g != (TCVec *)ctx->raw_traces.end; g++) {
		TCTraceEntity *e = (TCTraceEntity *)TC_op_new(sizeof(TCTraceEntity));
		for (TCPixel *p = (TCPixel *)g->begin; p != (TCPixel *)g->end; p++) {
			switch (p->ink) {
				case 0xff: TCVecPix_push(&e->write, p); break;
				case 0xfe: TCVecPix_push(&e->read, p); break;
				case 0xfd: TCVecPix_push(&e->ink_fd, p); break;
				case 0xed: TCVecPix_push(&e->conn_ed, p); break;
				default: break;
			}
		}
		TCVecPtr_push(&ctx->entities_b, e);
		TC_entity_category_record(ctx, 7); /* every trace net counts as one TRACE (STATSTYPE 7) */
	}
	ctx->entity_count_a = (int32_t)TCVec_count(&ctx->entities_a, sizeof(void *));
	ctx->entity_count_b = (int32_t)TCVec_count(&ctx->entities_b, sizeof(void *));
	/* entity-LUT side length: smallest pow2 (>=4) with side^2 >= total. */
	size_t total = (size_t)ctx->entity_count_a + ctx->entity_count_b + 1;
	int32_t s = 4;
	while ((size_t)s * s < total)
		s += s;
	ctx->entitylist_sidelength = s;
}

void vcb_ctx_free(TCAnalysisCtx *ctx) {
	for (TCVec *g = (TCVec *)ctx->raw_components.begin;
	     g && g != (TCVec *)ctx->raw_components.end; g++)
		TCVec_free(g);
	for (TCVec *g = (TCVec *)ctx->raw_traces.begin;
	     g && g != (TCVec *)ctx->raw_traces.end; g++)
		TCVec_free(g);
	TCVec_free(&ctx->raw_components);
	TCVec_free(&ctx->raw_traces);
	TCVec_free(&ctx->classified);
	TCVec_free(&ctx->mesh_pixels);
	TCVec_free(&ctx->bus_connections);
	TCVec_free(&ctx->net_table);
	/* entity handles + entity storage: freed by the caller/module. */
}
