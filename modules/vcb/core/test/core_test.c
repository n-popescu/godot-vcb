/* core_test.c -- standalone verification of the recovered VCB compiler front-end
 * (no Godot). Build: make -C modules/vcb/core/test  (or see the commands below).
 *   gcc -std=c11 -Wall -I.. core_test.c ../vcb_pipeline.c ../vcb_vec.c \
 *       ../vcb_classifier.c ../vcb_resolver.c -o core_test && ./core_test
 */
#include "../vcb_pipeline.h"
#include "../vcb_classifier.h"
#include "../vcb_resolver.h"
#include "../vcb_model.h"
#include "../vcb_sim.h"
#include "../vcb_vdisplay.h"
#include "../vcb_vmem.h"
#include "../vcb_bus.h"
#include "../vcb_vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  OK  %s\n", m); else { printf("  FAIL %s\n", m); bad++; } } while (0)

static void put(uint8_t *img, int i, uint32_t rgb) {
	img[i * 4] = (rgb >> 16) & 0xff;
	img[i * 4 + 1] = (rgb >> 8) & 0xff;
	img[i * 4 + 2] = rgb & 0xff;
	img[i * 4 + 3] = 0xff;
}

// Look up an EDITOR colour for a given ink code from the classifier table, so the
// tests can place pixels of any ink without hardcoding RGB values.
static uint32_t rgb_for_ink(uint8_t ink) {
	for (size_t i = 0; i < TC_INK_TABLE_LEN; i++)
		if (TC_INK_TABLE[i].ink == ink)
			return TC_INK_TABLE[i].editor_rgb;
	return 0;
}

// Append to a VCBIntVec (for hand-built models in the differential test).
static void elink(VCBIntVec *v, int32_t x) {
	v->items = (int32_t *)realloc(v->items, (size_t)(v->count + 1) * sizeof(int32_t));
	v->items[v->count++] = x;
	v->cap = v->count;
}

// Drive a net from outside the tick loop and tell the scheduler, which is what a
// real driver (or a mouse override) does. The kernel only evaluates an entity when
// something schedules it, so a test that just pokes a state and ticks would leave
// every reader unevaluated -- exactly as the original engine does.
static void drive_net(VCBSim *s, VCBModel *m, int32_t net, int v) {
	if (m->ent[net].state == (uint8_t)v)
		return;
	m->ent[net].state = (uint8_t)v;
	vcb_sim_mark_external(s, net);
}

static void free_manual_model(VCBModel *m) {
	if (m->ent) {
		for (int32_t k = 0; k <= m->n_entities; k++) {
			free(m->ent[k].inputs.items);
			free(m->ent[k].outputs.items);
			free(m->ent[k].conns.items);
		}
		free(m->ent);
	}
	free(m->net_rep);
	m->ent = NULL;
	m->net_rep = NULL;
}

// Run a model through the synchronous reference and the event-driven tick and
// return 1 iff every entity's state matches on every tick.
static int diff_sim(VCBModel *m, int clk, int tmr, uint32_t seed, int nticks) {
	int32_t N = m->n_entities;
	uint8_t *init = (uint8_t *)malloc((size_t)N + 1);
	for (int32_t k = 0; k <= N; k++)
		init[k] = m->ent[k].state;
	uint8_t *hist = (uint8_t *)malloc((size_t)nticks * (N + 1));
	VCBSim s;
	vcb_sim_init(&s, m);
	vcb_sim_set_clock(&s, clk);
	vcb_sim_set_timer_us(&s, tmr);
	vcb_sim_set_seed(&s, seed);
	for (int t = 0; t < nticks; t++) {
		vcb_sim_tick_sync(&s);
		for (int32_t k = 0; k <= N; k++)
			hist[(size_t)t * (N + 1) + k] = m->ent[k].state;
	}
	vcb_sim_free(&s);
	for (int32_t k = 0; k <= N; k++)
		m->ent[k].state = init[k];
	VCBSim s2;
	vcb_sim_init(&s2, m);
	vcb_sim_set_clock(&s2, clk);
	vcb_sim_set_timer_us(&s2, tmr);
	vcb_sim_set_seed(&s2, seed);
	int ok = 1;
	for (int t = 0; t < nticks && ok; t++) {
		vcb_sim_tick(&s2);
		for (int32_t k = 0; k <= N; k++)
			if (m->ent[k].state != hist[(size_t)t * (N + 1) + k]) {
				printf("  ... diff at tick %d entity %d: sync=%d active=%d\n",
						t, k, hist[(size_t)t * (N + 1) + k], m->ent[k].state);
				ok = 0;
				break;
			}
	}
	vcb_sim_free(&s2);
	free(init);
	free(hist);
	return ok;
}

// Build a tiny 4-entity model: gate 1 (given ink) reads nets 2 and 3 and drives
// net 4. Used to exercise the simulator's gate logic directly.
static void mk_two_input_gate(VCBModel *m, uint8_t ink) {
	memset(m, 0, sizeof(*m));
	m->sidelength = 4;
	m->board_side = 0;
	m->n_components = 1;
	m->n_entities = 4;
	m->ent = (VCBEntity *)calloc(5, sizeof(VCBEntity));
	m->ent[1].ink = ink;
	m->ent[1].is_trace = 0;
	m->ent[1].inputs.items = (int32_t *)malloc(2 * sizeof(int32_t));
	m->ent[1].inputs.items[0] = 2;
	m->ent[1].inputs.items[1] = 3;
	m->ent[1].inputs.count = m->ent[1].inputs.cap = 2;
	m->ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
	m->ent[1].outputs.items[0] = 4;
	m->ent[1].outputs.count = m->ent[1].outputs.cap = 1;
	for (int i = 2; i <= 4; i++) {
		m->ent[i].is_trace = 1;
		m->ent[i].ink = 0xff;
	}
}

// Drive nets 2,3 with (a,b), advance one tick, return the output net (4) state.
/* Build a lone gate with NO input nets driving one output trace net (entity 2),
 * run the simulator a few ticks, and return the output net's settled state. Used
 * to check the 0-input behaviour of gates as constant sources. */
static int lone_gate_net(uint8_t ink) {
	VCBModel m;
	memset(&m, 0, sizeof(m));
	m.sidelength = 4;
	m.n_components = 1;
	m.n_entities = 2;
	m.ent = (VCBEntity *)calloc(3, sizeof(VCBEntity));
	m.ent[1].ink = ink;
	m.ent[1].is_trace = 0; /* no inputs */
	m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
	m.ent[1].outputs.items[0] = 2;
	m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
	m.ent[2].is_trace = 1;
	m.ent[2].ink = 0xff;
	VCBSim s;
	vcb_sim_init(&s, &m);
	vcb_sim_run(&s, 4);
	int out = m.ent[2].state;
	vcb_sim_free(&s);
	vcb_model_free(&m);
	return out;
}

static int gate_out(uint8_t ink, int a, int b) {
	VCBModel m;
	mk_two_input_gate(&m, ink);
	VCBSim s;
	vcb_sim_init(&s, &m);
	drive_net(&s, &m, 2, a);
	drive_net(&s, &m, 3, b);
	vcb_sim_tick(&s);
	int out = m.ent[4].state;
	vcb_sim_free(&s);
	vcb_model_free(&m);
	return out;
}

int main(void) {
	// classifier + resolver data tables
	CHECK(TC_classify_color(0x92ff63, NULL) == 0x01, "classify BUFFER -> 0x01");
	CHECK(TC_classify_color(0x2a3541, NULL) == 0xee, "classify TRACE_GRAY -> 0xee");
	uint32_t on = 0, off = 0;
	CHECK(TC_resolve_color(0x7a2f24, &on, &off) && on == 0xff2700, "resolve BUS_0 ON -> ff2700");

	// full front-end on a 4x4 image: BUFFER + two different-colour traces
	const int side = 4;
	uint8_t img[4 * 4 * 4];
	memset(img, 0, sizeof(img));
	put(img, 1 * 4 + 1, 0x92ff63);   // BUFFER
	put(img, 1 * 4 + 2, 0x2a3541);   // TRACE_GRAY
	put(img, 1 * 4 + 3, 0xa1555e);   // TRACE_RED (adjacent -> merges)

	TCAnalysisCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.side = side;
	vcb_prepare(&ctx, img, sizeof(img));
	vcb_scan_pixels(&ctx);
	vcb_link(&ctx);
	vcb_finalize(&ctx);

	CHECK(ctx.entity_count_a == 1, "one component entity (BUFFER)");
	CHECK(ctx.entity_count_b == 1, "one trace-net entity (grey+red merged)");
	CHECK(ctx.entitylist_sidelength >= 4, "entity-LUT side computed");

	vcb_ctx_free(&ctx);

	// --- tunnel resolver: a trace entering a tunnel emerges at the paired exit,
	// so two trace segments on either side of a tunnel pair form ONE net. ---
	{
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		uint32_t tr = rgb_for_ink(0xee); // a trace
		uint32_t tu = rgb_for_ink(0x65); // TUNNEL
		put(im, 1 * s + 1, tr);          // trace segment A (1,1)
		put(im, 1 * s + 2, tu);          // entrance tunnel (2,1)
		// (3,1) left empty: the tunnel passes under it
		put(im, 1 * s + 4, tu);          // exit tunnel (4,1)
		put(im, 1 * s + 5, tr);          // trace segment B (5,1)
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		CHECK(c.errors.begin == c.errors.end, "tunnel: no error on a matched pair");
		CHECK(c.entity_count_b == 1, "tunnel: two trace segments merge through the tunnel");
		vcb_ctx_free(&c);
	}

	// --- tunnel resolver: a lone tunnel with no exit before the edge is an
	// UNMATCHED_TUNNEL_RIGHT error (code = dir + 1, RIGHT = dir 1 -> code 2). ---
	{
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, rgb_for_ink(0xee)); // trace (1,1)
		put(im, 1 * s + 2, rgb_for_ink(0x65)); // tunnel (2,1), no exit to the right
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		int has_err = (c.errors.begin != c.errors.end);
		int code_ok = has_err && ((TCPixel *)c.errors.begin)->ink == 2;
		CHECK(has_err && code_ok, "tunnel: unmatched -> UNMATCHED_TUNNEL_RIGHT");
		vcb_ctx_free(&c);
	}

	// --- the tunnel EXIT-MATCHING rule (probes p15/p16). The resolver used to take
	// the first tunnel it met in the direction; the original takes the first one whose
	// far cell carries the SAME INK as the source pixel. Taking the first tunnel split
	// 64 trace nets on a real project, where a bus run tunnels past ten tunnel pairs
	// belonging to other wires. ---
	{
		const int s = 24;
		static uint8_t im[24 * 24 * 4];
		uint32_t tr = rgb_for_ink(0xee), tu = rgb_for_ink(0x65), xs = rgb_for_ink(0x64);

		// (a) tunnel pairs serving OTHER wires (T, CROSS, T) lie between the two ends;
		//     the walk must pass them and land on the same-ink cell at the far end.
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, tr);                      // source (1,1)
		put(im, 1 * s + 2, tu);                      // entrance
		put(im, 1 * s + 4, tu); put(im, 1 * s + 5, xs); put(im, 1 * s + 6, tu);
		put(im, 1 * s + 8, tu); put(im, 1 * s + 9, xs); put(im, 1 * s + 10, tu);
		put(im, 1 * s + 12, tu);                     // the matching exit
		put(im, 1 * s + 13, tr);                     // target (13,1)
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		CHECK(c.errors.begin == c.errors.end && c.entity_count_b == 1,
				"tunnel: the walk passes other wires' tunnel pairs to the matching exit");
		vcb_ctx_free(&c);

		// (b) exact ink: a trace does NOT tunnel to a trace of a different colour --
		//     both ends report UNMATCHED instead.
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, tr);
		put(im, 1 * s + 2, tu);
		put(im, 1 * s + 4, tu);
		put(im, 1 * s + 5, rgb_for_ink(0xf0));       // TRACE_RED, a different ink
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		int n_err = 0, codes_ok = 1;
		for (TCPixel *e = (TCPixel *)c.errors.begin; e != (TCPixel *)c.errors.end; e++) {
			n_err++;
			if (e->ink != 1 && e->ink != 2)
				codes_ok = 0;
		}
		CHECK(n_err == 2 && codes_ok,
				"tunnel: different inks do not pair -- UNMATCHED from both ends");
		vcb_ctx_free(&c);

		// (c) UNEXPECTED_TUNNEL_ENTRANCE (code 0): walking left from (9,1), the tunnel
		//     at (5,1) carries the source ink on its NEAR side, so the walk gives up --
		//     code 0 at that tunnel plus UNMATCHED_TUNNEL_LEFT at the source.
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, tr); put(im, 1 * s + 2, tu);
		put(im, 1 * s + 5, tu); put(im, 1 * s + 6, tr);
		put(im, 1 * s + 8, tu); put(im, 1 * s + 9, tr);
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		int have0 = 0, have1 = 0;
		for (TCPixel *e = (TCPixel *)c.errors.begin; e != (TCPixel *)c.errors.end; e++) {
			if (e->ink == 0 && e->x == 5 && e->y == 1)
				have0 = 1;
			if (e->ink == 1 && e->x == 9 && e->y == 1)
				have1 = 1;
		}
		CHECK(have0 && have1,
				"tunnel: a same-ink entrance en route -> UNEXPECTED_TUNNEL_ENTRANCE at it");
		vcb_ctx_free(&c);

		// (d) a lone TUNNEL pixel between two same-ink traces is NOT a pair: the walk
		//     starts two cells out, so neither side ever sees the other.
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, tr); put(im, 1 * s + 2, tu); put(im, 1 * s + 3, tr);
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		CHECK(c.errors.begin != c.errors.end,
				"tunnel: a lone TUNNEL pixel is unmatched, not a pair");
		vcb_ctx_free(&c);
	}

	// --- bus connection: a trace adjacent to a bus records exactly one edge. ---
	{
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, rgb_for_ink(0xee)); // trace (1,1)
		put(im, 1 * s + 2, rgb_for_ink(0xe8)); // BUS_0 (2,1)
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		size_t nb = TCVec_count(&c.bus_connections, sizeof(void *));
		CHECK(nb == 1, "bus: trace->bus connection recorded once");
		vcb_ctx_free(&c);
	}

	// --- model builder: input trace -> READ -> AND gate -> WRITE -> output trace.
	// A component is wired to a net ONLY through a junction pixel: READ makes that
	// net an input, WRITE an output. A bare trace touching the gate body connects
	// nothing (checked separately below). ---
	{
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		put(im, 2 * s + 0, rgb_for_ink(0xee)); // input trace   (0,2)
		put(im, 2 * s + 1, rgb_for_ink(0xfe)); // READ junction (1,2)
		put(im, 2 * s + 2, rgb_for_ink(0x02)); // AND gate      (2,2)
		put(im, 2 * s + 3, rgb_for_ink(0xff)); // WRITE junction (3,2)
		put(im, 2 * s + 4, rgb_for_ink(0xee)); // output trace  (4,2)
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m;
		int rc = vcb_model_build(&c, &m);
		CHECK(rc == 0, "model: build ok");
		CHECK(m.n_components == 1, "model: one gate component");
		CHECK(m.n_entities == 3, "model: one gate + two trace nets");
		CHECK(m.ent[1].ink == 0x02, "model: gate ink is AND (0x02)");
		CHECK(m.ent[1].inputs.count == 1 && m.ent[m.ent[1].inputs.items[0]].is_trace,
				"model: gate reads one trace net");
		CHECK(m.ent[1].outputs.count == 1 && m.ent[m.ent[1].outputs.items[0]].is_trace,
				"model: gate drives one trace net (WRITE junction)");
		// emit + sanity-check the flat circuit_data / adjacency
		uint8_t *cd = NULL;
		int32_t cdl = 0;
		vcb_model_emit_circuit_data(&m, &cd, &cdl);
		CHECK(cd[1 * 4 + 1] == 0x02, "model: circuit_data cell 1 ink = AND");
		free(cd);
		// inverse entity-LUT: the gate (entity 1) maps back to its board pixel (2,2)
		int32_t *ix = (int32_t *)malloc((size_t)(m.n_entities + 1) * sizeof(int32_t));
		int32_t *iy = (int32_t *)malloc((size_t)(m.n_entities + 1) * sizeof(int32_t));
		vcb_model_build_inverse(&m, ix, iy);
		CHECK(ix[1] == 2 && iy[1] == 2, "model: inverse LUT maps the gate to its board pixel");
		free(ix);
		free(iy);
		vcb_model_free(&m);
		vcb_ctx_free(&c);
	}

	// --- the same board with the READ removed: a bare trace against the gate body
	// wires nothing at all, so the gate has no inputs. Verified against the original
	// engine, where a LED sitting next to a live trace stays dark until a READ is
	// drawn between them. ---
	{
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		put(im, 2 * s + 0, rgb_for_ink(0xee)); // input trace  (0,2)
		put(im, 2 * s + 1, rgb_for_ink(0xee)); // still a trace, no READ
		put(im, 2 * s + 2, rgb_for_ink(0x02)); // AND gate     (2,2)
		put(im, 2 * s + 3, rgb_for_ink(0xff)); // WRITE junction (3,2)
		put(im, 2 * s + 4, rgb_for_ink(0xee)); // output trace (4,2)
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m;
		vcb_model_build(&c, &m);
		CHECK(m.ent[1].inputs.count == 0 && m.ent[1].outputs.count == 1,
				"model: a bare trace against a gate body connects nothing (READ required)");
		vcb_model_free(&m);
		vcb_ctx_free(&c);
	}

	// --- simulator: gate truth tables through a net (input nets 2,3 -> gate ->
	// output net 4), plus a wired-OR check. Validates the unit-delay tick. ---
	CHECK(gate_out(0x02, 0, 0) == 0 && gate_out(0x02, 1, 0) == 0 && gate_out(0x02, 1, 1) == 1,
			"sim: AND truth table");
	CHECK(gate_out(0x03, 0, 0) == 0 && gate_out(0x03, 1, 0) == 1 && gate_out(0x03, 1, 1) == 1,
			"sim: OR truth table");
	CHECK(gate_out(0x04, 0, 0) == 0 && gate_out(0x04, 1, 0) == 1 && gate_out(0x04, 1, 1) == 0,
			"sim: XOR truth table");
	CHECK(gate_out(0x01, 0, 0) == 0 && gate_out(0x01, 1, 0) == 1, "sim: BUFFER passes OR");
	CHECK(gate_out(0x05, 0, 0) == 1 && gate_out(0x05, 1, 0) == 0, "sim: NOT inverts OR");
	CHECK(gate_out(0x06, 1, 1) == 0 && gate_out(0x06, 1, 0) == 1, "sim: NAND truth table");
	CHECK(gate_out(0x07, 0, 0) == 1 && gate_out(0x07, 1, 0) == 0, "sim: NOR truth table");
	CHECK(gate_out(0x08, 0, 0) == 1 && gate_out(0x08, 1, 0) == 0 && gate_out(0x08, 1, 1) == 1,
			"sim: XNOR truth table");
	// The AND handler (0x28f553) is a bare `n_high == n_inputs` with no 0-input
	// guard, so on its own it reads HIGH for an unconnected gate. The gate never
	// gets there: construct_model seeds only inks 0x05..0x09, and nothing else is
	// evaluated until an input changes, so an unconnected AND stays OFF. Both halves
	// are checked -- the raw handler here, the end-to-end behaviour just below.
	CHECK(vcb_gate_eval(0x02, 0, 0) == 1 && vcb_gate_eval(0x02, 2, 2) == 1 &&
					vcb_gate_eval(0x02, 1, 2) == 0,
			"sim: AND handler is n_high == n_inputs (no 0-input guard)");
	CHECK(vcb_gate_eval(0x05, 0, 0) == 1 && vcb_gate_eval(0x07, 0, 0) == 1 &&
			vcb_gate_eval(0x08, 0, 0) == 1 && vcb_gate_eval(0x06, 0, 0) == 0,
			"sim: 0-input NOT/NOR/XNOR = HIGH, NAND = LOW (kernel)");
	// End-to-end: an unconnected AND drives its output net OFF forever, while an
	// unconnected NOT is a constant-HIGH source (drives its net HIGH). This is the
	// behaviour a differential run against vcb.exe would confirm.
	{
		int and_off = lone_gate_net(0x02); // AND with 0 inputs -> net stays OFF
		int not_on = lone_gate_net(0x05);  // NOT with 0 inputs -> net driven HIGH
		CHECK(and_off == 0 && not_on == 1,
				"sim: unconnected AND net OFF, unconnected NOT net HIGH");
	}
	// AND driven high stays high across many ticks (stable), then drops when an
	// input goes low (one unit-delay tick later).
	{
		VCBModel m;
		mk_two_input_gate(&m, 0x02);
		VCBSim s;
		vcb_sim_init(&s, &m);
		drive_net(&s, &m, 2, 1);
		drive_net(&s, &m, 3, 1);
		vcb_sim_run(&s, 5);
		int stable_high = m.ent[4].state;
		drive_net(&s, &m, 3, 0);
		vcb_sim_tick(&s);
		int dropped = (m.ent[4].state == 0);
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(stable_high == 1 && dropped, "sim: AND stable then drops when input clears");
	}

	// --- snapshots: step back/forward through simulation state history. ---
	{
		VCBModel m;
		mk_two_input_gate(&m, 0x02); // AND
		VCBSim s;
		vcb_sim_init(&s, &m);
		drive_net(&s, &m, 2, 1);
		drive_net(&s, &m, 3, 1);
		vcb_sim_tick(&s);                 // out net 4 -> 1
		vcb_sim_snapshot_take(&s);        // snap A (net4 = 1)
		int a_state = m.ent[4].state;
		drive_net(&s, &m, 3, 0);
		vcb_sim_tick(&s);                 // out net 4 -> 0
		vcb_sim_snapshot_take(&s);        // snap B (net4 = 0)
		int prev_ok = vcb_sim_snapshot_prev_possible(&s);
		vcb_sim_snapshot_restore_prev(&s); // back to snap A
		int restored_a = m.ent[4].state;
		int next_ok = vcb_sim_snapshot_next_possible(&s);
		vcb_sim_snapshot_restore_next(&s); // forward to snap B
		int restored_b = m.ent[4].state;
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(a_state == 1 && prev_ok && restored_a == 1 && next_ok && restored_b == 0,
				"sim: snapshot step back/forward restores state");
	}

	// --- breakpoints: a BREAKPOINT entity (ink 0x11) fires on a 0->1 edge of its
	// monitored input, once per rising edge, and is cleared each frame. ---
	{
		VCBModel m;
		mk_two_input_gate(&m, 0x11); // breakpoint monitoring nets 2,3
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_clear_fired(&s);
		drive_net(&s, &m, 2, 1);         // raise the monitored input
		vcb_sim_tick(&s);                // breakpoint 0->1: fires
		int fired_once = (s.fired_breakpoints.count == 1 && s.fired_breakpoints.items[0] == 1);
		vcb_sim_clear_fired(&s);
		vcb_sim_tick(&s);                // still high, no new edge: no fire
		int no_refire = (s.fired_breakpoints.count == 0);
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(fired_once && no_refire, "sim: breakpoint fires on rising edge, once per edge");
	}

	// --- clock: a CLOCK entity toggles its net every clock_interval ticks. ---
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 1;
		m.n_entities = 2;
		m.ent = (VCBEntity *)calloc(3, sizeof(VCBEntity));
		m.ent[1].ink = 0x0b; // CLOCK
		m.ent[1].is_trace = 0;
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 2;
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		m.ent[2].is_trace = 1;
		m.ent[2].ink = 0xff;
		m.clock_value = 1;
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_set_clock(&s, 2);
		vcb_sim_tick(&s);
		int t0 = m.ent[2].state; // tick 0: toggle -> net 1
		vcb_sim_tick(&s);
		int t1 = m.ent[2].state; // tick 1: hold -> net 1
		vcb_sim_tick(&s);
		int t2 = m.ent[2].state; // tick 2: toggle -> net 0
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(t0 == 1 && t1 == 1 && t2 == 0, "sim: CLOCK toggles its net every interval");
	}

	// --- TIMER: real-time, not tick-based. The kernel reads a wall clock and fires
	// when `interval_us + paused_us < elapsed_us`, so the core takes elapsed time
	// from its host; with no time fed in a TIMER never fires however long the sim
	// runs (which is what the original does across back-to-back solves). ---
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 1;
		m.n_entities = 2;
		m.ent = (VCBEntity *)calloc(3, sizeof(VCBEntity));
		m.ent[1].ink = 0x10; // TIMER
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 2;
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		m.ent[2].is_trace = 1;
		m.ent[2].ink = 0xff;
		m.timer_value = 1;
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_set_timer_us(&s, 1000);
		vcb_sim_run(&s, 50);
		int idle = m.ent[2].state; // no wall time fed: never fires
		vcb_sim_add_time_us(&s, 1500, 0);
		vcb_sim_run(&s, 1);
		int a = m.ent[2].state; // 1500us > 1000us: toggle -> 1
		vcb_sim_run(&s, 5);
		int b = m.ent[2].state; // no further time: hold -> 1
		vcb_sim_add_time_us(&s, 1500, 0);
		vcb_sim_run(&s, 1);
		int c = m.ent[2].state; // another period: toggle -> 0
		// paused time is added to the threshold, not the elapsed, so it defers a fire
		vcb_sim_add_time_us(&s, 1500, 5000);
		vcb_sim_run(&s, 1);
		int d = m.ent[2].state; // 1500us < 1000+5000: no toggle
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(idle == 0 && a == 1 && b == 1 && c == 0 && d == 0,
				"sim: TIMER fires on elapsed microseconds, not ticks");
	}

	// --- RANDOM (MT19937, handler 0x28f6cf): `((mt_next() & 1) | state) & (entry > 0)`.
	// It draws only when a *rising* net schedules it -- a falling one clears it to 0 --
	// so it is not a free-running noise source; it samples on each rising edge. ---
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 1;
		m.n_entities = 2;
		m.ent = (VCBEntity *)calloc(3, sizeof(VCBEntity));
		m.n_entities = 3;
		m.ent = (VCBEntity *)calloc(4, sizeof(VCBEntity));
		m.ent[1].ink = 0x12; // RANDOM, clocked by net 3
		m.ent[1].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].inputs.items[0] = 3;
		m.ent[1].inputs.count = m.ent[1].inputs.cap = 1;
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 2;
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		m.ent[2].is_trace = 1;
		m.ent[2].ink = 0xff;
		m.ent[3].is_trace = 1;
		m.ent[3].ink = 0xff;
		int seq[2][16];
		for (int pass = 0; pass < 2; pass++) {
			for (int k = 1; k <= 3; k++)
				m.ent[k].state = 0;
			VCBSim s;
			vcb_sim_init(&s, &m);
			vcb_sim_set_seed(&s, 777u);
			for (int i = 0; i < 16; i++) {
				drive_net(&s, &m, 3, 1);  // rising edge: draw a bit
				vcb_sim_tick(&s);
				seq[pass][i] = m.ent[1].state;
				drive_net(&s, &m, 3, 0);  // falling edge: clears to 0
				vcb_sim_tick(&s);
			}
			vcb_sim_free(&s);
		}
		int deterministic = 1, ones = 0, zeros = 0;
		for (int i = 0; i < 16; i++) {
			if (seq[0][i] != seq[1][i])
				deterministic = 0;
			if (seq[0][i])
				ones++;
			else
				zeros++;
		}
		free_manual_model(&m);
		CHECK(deterministic && ones > 0 && zeros > 0,
				"sim: RANDOM samples on a rising edge, deterministic per seed (MT19937)");
	}

	// --- virtual-display repack (get_vdisplay_texture 0x28fd80) ---
	{
		// 24-bit direct: each staging word is one 0xRRGGBB pixel.
		int32_t st[2] = { 0x112233, 0x445566 };
		uint8_t out[2 * 1 * 4];
		memset(out, 0, sizeof(out));
		vcb_vdisplay_repack(st, 2, 2, 1, 24, 24, NULL, 0, out);
		CHECK(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0 &&
						out[4] == 0x44 && out[5] == 0x55 && out[6] == 0x66,
				"vdisplay: 24-bit direct RGB repack");
	}
	{
		// 4-bit palette, 32-bit words -> 8 px/word, MSB-first. word 0x01234567
		// yields sub-pixel indices 0,1,2,3,4,5,6,7; palette[i] = i*0x010101.
		int32_t pal[16];
		for (int i = 0; i < 16; i++)
			pal[i] = i * 0x010101;
		int32_t st[1] = { 0x01234567 };
		uint8_t out[8 * 1 * 4];
		memset(out, 0, sizeof(out));
		vcb_vdisplay_repack(st, 1, 8, 1, 32, 4, pal, 16, out);
		int ok = 1;
		for (int k = 0; k < 8; k++)
			if (out[k * 4] != k || out[k * 4 + 1] != k || out[k * 4 + 2] != k)
				ok = 0;
		CHECK(ok, "vdisplay: 4-bit palette repack (MSB-first, 8 px/word)");
	}
	{
		// 1-bit palette: word 0xA0 = 1010_0000b -> px 1,0,1,0,0,0,0,0.
		int32_t pal[2] = { 0x000000, 0xffffff };
		int32_t st[1] = { 0xA0 };
		uint8_t out[8 * 1 * 4];
		memset(out, 0, sizeof(out));
		vcb_vdisplay_repack(st, 1, 8, 1, 8, 1, pal, 2, out);
		CHECK(out[0] == 0xff && out[4] == 0x00 && out[8] == 0xff && out[12] == 0x00 &&
						out[16] == 0x00,
				"vdisplay: 1-bit palette repack");
	}
	{
		// Output is capped at w*h pixels (excess staging ignored, tail stays 0).
		int32_t pal[16];
		for (int i = 0; i < 16; i++)
			pal[i] = 0x010101 * (i + 1);
		int32_t st[2] = { 0x01234567, 0x89abcdef };
		uint8_t out[3 * 1 * 4 + 4];
		memset(out, 0xEE, sizeof(out));
		// zero only the valid region as the wrapper does; sentinel byte after it
		memset(out, 0, 3 * 4);
		vcb_vdisplay_repack(st, 2, 3, 1, 32, 4, pal, 16, out);
		CHECK(out[0] == 1 && out[4] == 2 && out[8] == 3 && out[12] == 0xEE,
				"vdisplay: repack stops at w*h pixels");
	}

	// --- VMem initial-image builder (compute_vmem_data / 0x3e3c70) ---
	{
		// live_vmem big-endian words OR'd with the assembly program words.
		uint8_t live[8] = { 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
		int32_t asmv[2] = { 0x00000000, 0x01000000 };
		int32_t *w = NULL;
		int32_t n = vcb_vmem_build(live, 8, asmv, 2, &w);
		// Word 0 is the reserved slot -- the original's image is always 0 there
		// (measured; see vcb_vmem.c) -- and word 1 up is BE32(live) | assembly.
		CHECK(n == 2 && w && w[0] == 0 && (uint32_t)w[1] == 0xABBBCCDDu,
				"vmem: build = BE32(live) | assembly, word 0 reserved");
		free(w);
	}
	{
		// Missing assembly words are treated as 0 (we tolerate; the binary asserts).
		uint8_t live[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
		int32_t *w = NULL;
		int32_t n = vcb_vmem_build(live, 8, NULL, 0, &w);
		CHECK(n == 2 && w && w[0] == 0 && (uint32_t)w[1] == 0x05060708u,
				"vmem: build tolerates short assembly");
		free(w);
	}
	{
		// get_vmem_persistent: big-endian byte repack of a word range.
		int32_t words[2] = { (int32_t)0x11223344, (int32_t)0xAABBCCDD };
		uint8_t out[8];
		memset(out, 0, sizeof(out));
		vcb_vmem_persistent(words, 2, 0, 2, out);
		CHECK(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0x44 &&
						out[4] == 0xAA && out[5] == 0xBB && out[6] == 0xCC && out[7] == 0xDD,
				"vmem: persistent big-endian repack");
	}

	// --- bus grouping (build_stage1 bus pass reconstruction, vcb_bus) ---
	{
		// 4x4 board: two adjacent BUS_0 (0x0e8... wait 0xe8) pixels at (1,1),(2,1);
		// a trace at (0,1) recorded touching (1,1), mapping to entity 7.
		const int side = 4;
		uint8_t cls[4 * 4 * 2];
		memset(cls, 0, sizeof(cls));
		cls[(1 * 4 + 1) * 2] = 0xe8; // (1,1)
		cls[(1 * 4 + 2) * 2] = 0xe8; // (2,1)
		uint64_t key = (uint64_t)(uint16_t)0 | ((uint64_t)(uint16_t)1 << 16) |
				((uint64_t)(uint16_t)1 << 32) | ((uint64_t)(uint16_t)1 << 48); // f=(0,1) b=(1,1)
		void *conn[1] = { (void *)(uintptr_t)key };
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = side;
		int32_t lut[16];
		memset(lut, 0, sizeof(lut));
		lut[1 * 4 + 0] = 7; // trace pixel (0,1) -> entity 7
		m.lut = lut;
		m.n_entities = 7;
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = side;
		c.classified.begin = cls;
		c.classified.end = cls + sizeof(cls);
		c.bus_connections.begin = conn;
		c.bus_connections.end = conn + 1;
		VCBBusData bus;
		vcb_bus_build(&c, &m, &bus);
		int ok = bus.has_buses && bus.entities_len == 2 && bus.entities[0] == 7 &&
				bus.entities[1] == 0 && bus.busindex[1 * 4 + 1] == 0 &&
				bus.busindex[1 * 4 + 2] == 0 && bus.busindex[0] == -1;
		vcb_bus_free(&bus);
		m.lut = NULL; // not owned here
		CHECK(ok, "bus: same-ink net groups pixels + maps trace->entity list");
	}

	// --- VMem runtime, decoded from the kernel's post-PASS-2 block (0x28f8fa..0x28fb4b).
	// The VMem latches are LATCHes: each toggles when a *rising* net schedules it.
	// After the passes, in this order:
	//   - if a content latch changed, memory[address] = weighted binary of the
	//     content latches (a write, using the address as it was);
	//   - if an address latch changed, the address is recomputed from the address
	//     latches and memory[address] is read back into the content latches (which
	//     propagate normally), and the VMem lock is set so that during the next tick
	//     a latch schedule is recorded as an occurrence instead of toggling. ---
	{
		// entity1 = address latch (0x0d) clocked by net 3; entity2 = content latch
		// (0x0e) clocked by net 4.
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 2;
		m.n_entities = 4;
		m.ent = (VCBEntity *)calloc(5, sizeof(VCBEntity));
		m.ent[1].ink = 0x0d;
		m.ent[1].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].inputs.items[0] = 3;
		m.ent[1].inputs.count = m.ent[1].inputs.cap = 1;
		m.ent[2].ink = 0x0e;
		m.ent[2].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[2].inputs.items[0] = 4;
		m.ent[2].inputs.count = m.ent[2].inputs.cap = 1;
		m.ent[3].is_trace = 1;
		m.ent[3].ink = 0xff;
		m.ent[4].is_trace = 1;
		m.ent[4].ink = 0xff;
		m.vmem = (int32_t *)calloc(2, sizeof(int32_t));
		m.vmem_len = 2;
		m.vmem_addr_entities = (int32_t *)malloc(sizeof(int32_t));
		m.vmem_addr_entities[0] = 1;
		m.vmem_addr_count = 1;
		m.vmem_content_entities = (int32_t *)malloc(sizeof(int32_t));
		m.vmem_content_entities[0] = 2;
		m.vmem_content_count = 1;
		VCBSim s;
		vcb_sim_init(&s, &m);
		// clock the content latch: it toggles 0->1 and writes 1 to memory[0]
		drive_net(&s, &m, 4, 1);
		vcb_sim_tick(&s);
		int w0 = s.vmem[0], data_now = m.ent[2].state;
		// clock the address latch: it toggles 0->1, the address becomes 1, and
		// memory[1] (still 0) is read back into the content latch
		drive_net(&s, &m, 3, 1);
		vcb_sim_tick(&s);
		int addr_now = s.vmem_address, keep0 = s.vmem[0];
		int read_back = m.ent[2].state, locked = s.vmem_lock;
		vcb_sim_free(&s);
		free(m.ent[1].inputs.items);
		free(m.ent[2].inputs.items);
		free(m.ent);
		free(m.vmem);
		free(m.vmem_addr_entities);
		free(m.vmem_content_entities);
		CHECK(w0 == 1 && data_now == 1 && addr_now == 1 && keep0 == 1 &&
						read_back == 0 && locked == 1,
				"vmem: content latch writes memory, address latch selects + reads back");
	}

	// --- LATCH: a T flip-flop that toggles on the rising edge of its input net,
	// holds otherwise, and drives its output net with its stored state. ---
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 1;
		m.n_entities = 3;
		m.ent = (VCBEntity *)calloc(4, sizeof(VCBEntity));
		// LATCH_ON needs no preset state: construct_model seeds inks 0x05..0x09 into
		// the work list as positive entries, and the latch handler is
		// `(entry > 0) XOR state`, so the seed itself brings it up HIGH on tick 1.
		m.ent[1].ink = 0x09; // LATCH_ON
		m.ent[1].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].inputs.items[0] = 2; // input net
		m.ent[1].inputs.count = m.ent[1].inputs.cap = 1;
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 3; // output net
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		m.ent[2].is_trace = 1;
		m.ent[2].ink = 0xff; // input trace (held external input)
		m.ent[3].is_trace = 1;
		m.ent[3].ink = 0xff; // output trace
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_tick(&s);            // seeded: comes up HIGH and drives its net
		int h1 = m.ent[1].state;     // 1
		int out_on = m.ent[3].state; // latch drives output net -> 1
		drive_net(&s, &m, 2, 1);     // raise input
		vcb_sim_tick(&s);            // rising edge: toggle -> 0
		int tog1 = m.ent[1].state;   // 0
		vcb_sim_tick(&s);            // input still high: not scheduled, holds
		int hold = m.ent[1].state;   // 0
		drive_net(&s, &m, 2, 0);
		vcb_sim_tick(&s);            // falling edge: scheduled, but holds
		int held_on_fall = m.ent[1].state; // 0
		drive_net(&s, &m, 2, 1);
		vcb_sim_tick(&s);            // rising edge again: toggle -> 1
		int tog2 = m.ent[1].state;   // 1
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(h1 == 1 && out_on == 1 && tog1 == 0 && hold == 0 && held_on_fall == 0 &&
						tog2 == 1,
				"sim: LATCH_ON seeds HIGH, toggles on a rising edge, holds on a falling one");
	}

	// --- MOUSE OVERRIDE: forcing a latch's OWN state (what a click during simulation
	// does) must drive the net it feeds and any gate reading it. This is exactly the
	// path transistor_engine.cpp::solve takes for p_override_keys:
	//   m->ent[latch].state = st;  vcb_sim_mark_external(sim, latch);
	// Regression for "clicking a latch during sim does nothing".
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 4;
		m.n_components = 1;
		m.n_entities = 4;
		m.ent = (VCBEntity *)calloc(5, sizeof(VCBEntity));
		// entity 1: LATCH_ON with NO input net -> its own logic never moves it, so only
		// an external override changes it; it drives output net 2.
		m.ent[1].ink = 0x09;
		m.ent[1].is_trace = 0;
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 2;
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		// entity 2: the latch's output trace net.
		m.ent[2].is_trace = 1;
		m.ent[2].ink = 0xff;
		// entity 3: an OR gate reading net 2, driving net 4 -> proves the forced state
		// is visible to a downstream reader.
		m.ent[3].ink = 0x03; // OR
		m.ent[3].is_trace = 0;
		m.ent[3].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[3].inputs.items[0] = 2;
		m.ent[3].inputs.count = m.ent[3].inputs.cap = 1;
		m.ent[3].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[3].outputs.items[0] = 4;
		m.ent[3].outputs.count = m.ent[3].outputs.cap = 1;
		// entity 4: the OR's output trace net.
		m.ent[4].is_trace = 1;
		m.ent[4].ink = 0xff;

		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_run(&s, 3); // settle: seeded LATCH_ON comes up -> net2 on -> OR on
		int on0 = (m.ent[2].state == 1 && m.ent[4].state == 1);

		m.ent[1].state = 0; // "click": force the latch OFF
		vcb_sim_mark_external(&s, 1);
		vcb_sim_run(&s, 3);
		int off = (m.ent[2].state == 0 && m.ent[4].state == 0);

		m.ent[1].state = 1; // "click": force it back ON
		vcb_sim_mark_external(&s, 1);
		vcb_sim_run(&s, 3);
		int on1 = (m.ent[2].state == 1 && m.ent[4].state == 1);

		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(on0 && off && on1,
				"sim: forcing a latch's state (mouse override) drives its net + readers");
	}

	// --- BUS merge: two trace nets joined by a bus (net_rep) share state, so a
	// gate driving one net transports its signal to the other. ---
	{
		VCBModel m;
		memset(&m, 0, sizeof(m));
		m.sidelength = 8;
		m.n_components = 1;
		m.n_entities = 4;
		m.ent = (VCBEntity *)calloc(5, sizeof(VCBEntity));
		m.ent[1].ink = 0x01; // BUFFER: out = OR(in)
		m.ent[1].inputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].inputs.items[0] = 3; // reads trace src (net 3)
		m.ent[1].inputs.count = m.ent[1].inputs.cap = 1;
		m.ent[1].outputs.items = (int32_t *)malloc(sizeof(int32_t));
		m.ent[1].outputs.items[0] = 2; // drives trace A (net 2)
		m.ent[1].outputs.count = m.ent[1].outputs.cap = 1;
		m.ent[2].is_trace = 1; m.ent[2].ink = 0xff; // trace A
		m.ent[3].is_trace = 1; m.ent[3].ink = 0xff; // trace src (external input)
		m.ent[4].is_trace = 1; m.ent[4].ink = 0xff; // trace B (bus-merged with A)
		m.net_rep = (int32_t *)malloc(5 * sizeof(int32_t));
		m.net_rep[0] = 0; m.net_rep[1] = 1; m.net_rep[2] = 2; m.net_rep[3] = 3;
		m.net_rep[4] = 2; // B shares A's representative (bus)
		VCBSim s;
		vcb_sim_init(&s, &m);
		m.ent[3].state = 1;        // drive the source high
		vcb_sim_mark_external(&s, 3);
		vcb_sim_tick(&s);          // BUFFER -> A high; bus carries it to B
		int a = m.ent[2].state;    // 1
		int b = m.ent[4].state;    // 1 (transported via the bus)
		m.ent[3].state = 0;
		vcb_sim_mark_external(&s, 3);
		vcb_sim_tick(&s);          // source low -> A low; B follows
		int a0 = m.ent[2].state;   // 0
		int b0 = m.ent[4].state;   // 0
		vcb_sim_free(&s);
		vcb_model_free(&m);
		CHECK(a == 1 && b == 1 && a0 == 0 && b0 == 0,
				"sim: BUS-merged trace nets share state (data transport)");
	}

	// --- Event-driven vs synchronous: identical state sequences (differential). ---
	// Runs a model through vcb_sim_tick_sync (reference) and vcb_sim_tick (active set)
	// and checks every entity's state matches on every tick.
	{
		// helper via macros/inline below
		int all_ok = 1;

		// Model 1: NOT self-feedback oscillator + buffer + AND + CLOCK (heavy churn).
		{
			VCBModel m; memset(&m, 0, sizeof(m));
			m.n_components = 4; m.n_entities = 8;
			m.ent = (VCBEntity *)calloc(9, sizeof(VCBEntity));
			m.ent[1].ink = 0x05; elink(&m.ent[1].inputs, 5); elink(&m.ent[1].outputs, 5); // NOT net5 feedback
			m.ent[2].ink = 0x01; elink(&m.ent[2].inputs, 5); elink(&m.ent[2].outputs, 6); // BUFFER
			m.ent[3].ink = 0x02; elink(&m.ent[3].inputs, 6); elink(&m.ent[3].inputs, 7); elink(&m.ent[3].outputs, 8); // AND
			m.ent[4].ink = 0x0b; elink(&m.ent[4].outputs, 7); m.clock_value = 4; // CLOCK
			for (int k = 5; k <= 8; k++) { m.ent[k].is_trace = 1; m.ent[k].ink = 0xff; }
			all_ok &= diff_sim(&m, 3, 0, 1u, 300);
			free_manual_model(&m);
		}
		// Model 2: CLOCK-toggled LATCH whose output feeds a BUFFER through a BUS merge.
		{
			VCBModel m; memset(&m, 0, sizeof(m));
			m.n_components = 3; m.n_entities = 7;
			m.ent = (VCBEntity *)calloc(8, sizeof(VCBEntity));
			m.ent[1].ink = 0x0b; elink(&m.ent[1].outputs, 4); m.clock_value = 1; // CLOCK -> net4
			m.ent[2].ink = 0x09; m.ent[2].state = 1; elink(&m.ent[2].inputs, 4); elink(&m.ent[2].outputs, 5); // LATCH_ON
			m.ent[3].ink = 0x01; elink(&m.ent[3].inputs, 6); elink(&m.ent[3].outputs, 7); // BUFFER reads net6
			for (int k = 4; k <= 7; k++) { m.ent[k].is_trace = 1; m.ent[k].ink = 0xff; }
			m.net_rep = (int32_t *)malloc(8 * sizeof(int32_t));
			for (int k = 0; k <= 7; k++) m.net_rep[k] = k;
			m.net_rep[6] = 5; // bus: net6 merged into net5 (latch output)
			all_ok &= diff_sim(&m, 2, 0, 7u, 300);
			free_manual_model(&m);
		}
		// Model 3: RANDOM feeding an XOR (checks the MT draw order matches).
		{
			VCBModel m; memset(&m, 0, sizeof(m));
			m.n_components = 2; m.n_entities = 5;
			m.ent = (VCBEntity *)calloc(6, sizeof(VCBEntity));
			m.ent[1].ink = 0x12; elink(&m.ent[1].outputs, 3); // RANDOM -> net3
			m.ent[2].ink = 0x04; elink(&m.ent[2].inputs, 3); elink(&m.ent[2].inputs, 4); elink(&m.ent[2].outputs, 5); // XOR
			for (int k = 3; k <= 5; k++) { m.ent[k].is_trace = 1; m.ent[k].ink = 0xff; }
			all_ok &= diff_sim(&m, 0, 0, 0xC0FFEEu, 200);
			free_manual_model(&m);
		}
		CHECK(all_ok, "sim: event-driven tick matches synchronous reference (differential)");
	}

	// --- CROSS: perpendicular traces cross without connecting; each stays whole. ---
	// A horizontal trace (driven high by a constant NOT via a WRITE bridge) and a
	// vertical trace share one CROSS pixel. They must remain separate nets, the
	// horizontal signal must pass through the cross, and it must not leak to the
	// vertical net.
	{
		const int side = 12;
		uint8_t *img = (uint8_t *)calloc((size_t)side * side * 4, 1);
		put(img, 1 * side + 1, rgb_for_ink(0x05));            // NOT (0 inputs -> constant high) at (1,1)
		put(img, 1 * side + 2, rgb_for_ink(0xff));            // WRITE bridge at (2,1)
		for (int x = 3; x <= 9; x++)
			put(img, 5 * side + x, rgb_for_ink(0xee));        // H trace row 5, x=3..9
		for (int y = 1; y <= 9; y++)
			put(img, y * side + 6, rgb_for_ink(0xee));        // V trace col 6, y=1..9
		// Re-place the NOT/WRITE onto row 5 so the NOT actually drives the H trace.
		put(img, 5 * side + 1, rgb_for_ink(0x05));            // NOT at (1,5)
		put(img, 5 * side + 2, rgb_for_ink(0xff));            // WRITE at (2,5)
		put(img, 5 * side + 6, rgb_for_ink(0x64));            // CROSS placed LAST at (6,5)
		TCAnalysisCtx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.side = side;
		vcb_prepare(&ctx, img, side * side * 4);
		vcb_scan_pixels(&ctx);
		vcb_link(&ctx);
		vcb_finalize(&ctx);
		VCBModel m;
		vcb_model_build(&ctx, &m);
		int eH = m.lut[5 * side + 9]; // far end of the horizontal trace
		int eV = m.lut[1 * side + 6]; // top end of the vertical trace
		int separate = (eH > 0 && eV > 0 && eH != eV);
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_run(&s, 20);
		int h_on = eH > 0 ? m.ent[eH].state : -1;  // driven through the cross -> 1
		int v_off = eV > 0 ? m.ent[eV].state : -1; // no leak -> 0
		vcb_sim_free(&s);
		vcb_model_free(&m);
		vcb_ctx_free(&ctx);
		free(img);
		CHECK(separate && h_on == 1 && v_off == 0,
				"sim: CROSS keeps perpendicular traces separate + passes signal through");
	}

	// --- MESH: a board-wide wireless net. Two physically disconnected traces, each
	// touching a separate MESH pixel, must merge into one net (so a signal on one
	// reaches the other), while a control build without mesh keeps them separate.
	{
		const int side = 14;
		int merged_ok = 0, control_ok = 0;
		for (int with_mesh = 0; with_mesh <= 1; with_mesh++) {
			uint8_t *img = (uint8_t *)calloc((size_t)side * side * 4, 1);
			put(img, 1 * side + 1, rgb_for_ink(0x05));        // NOT (constant high) at (1,1)
			put(img, 1 * side + 2, rgb_for_ink(0xff));        // WRITE bridge at (2,1)
			put(img, 1 * side + 3, rgb_for_ink(0xee));        // trace A
			put(img, 1 * side + 4, rgb_for_ink(0xee));
			for (int x = 9; x <= 11; x++)
				put(img, 10 * side + x, rgb_for_ink(0xee));   // trace B, far away
			if (with_mesh) {
				put(img, 1 * side + 5, rgb_for_ink(0x66));    // mesh touching A
				put(img, 10 * side + 8, rgb_for_ink(0x66));   // mesh touching B (disconnected)
			}
			TCAnalysisCtx ctx;
			memset(&ctx, 0, sizeof(ctx));
			ctx.side = side;
			vcb_prepare(&ctx, img, side * side * 4);
			vcb_scan_pixels(&ctx);
			vcb_link(&ctx);
			vcb_finalize(&ctx);
			VCBModel m;
			vcb_model_build(&ctx, &m);
			int eA = m.lut[1 * side + 4], eB = m.lut[10 * side + 11];
			int repA = m.net_rep ? m.net_rep[eA] : eA;
			int repB = m.net_rep ? m.net_rep[eB] : eB;
			VCBSim s;
			vcb_sim_init(&s, &m);
			vcb_sim_run(&s, 20);
			int a = m.ent[eA].state, b = m.ent[eB].state;
			if (with_mesh)
				merged_ok = (repA == repB && a == 1 && b == 1);
			else
				control_ok = (repA != repB && a == 1 && b == 0);
			vcb_sim_free(&s);
			vcb_model_free(&m);
			vcb_ctx_free(&ctx);
			free(img);
		}
		CHECK(control_ok && merged_ok,
				"sim: MESH is a board-wide net (disconnected traces merge + transfer)");
	}

	// --- BUS off-propagation: a CLOCK-driven bus must follow its driver ON *and*
	// OFF (no stuck-high). Both traces touch the bus with the SAME colour. ---
	{
		const int side = 12;
		uint8_t *img = (uint8_t *)calloc((size_t)side * side * 4, 1);
		put(img, 1 * side + 1, rgb_for_ink(0x0b));   // CLOCK at (1,1)
		put(img, 1 * side + 2, rgb_for_ink(0xff));   // WRITE bridge -> drives trace A
		put(img, 1 * side + 3, rgb_for_ink(0xee));   // trace A (gray)
		put(img, 1 * side + 4, rgb_for_ink(0xee));   // gray pixel touches the bus
		put(img, 1 * side + 5, rgb_for_ink(0xe8));   // BUS_0
		put(img, 2 * side + 5, rgb_for_ink(0xe8));
		put(img, 2 * side + 6, rgb_for_ink(0xee));   // trace B (gray) touches the bus
		put(img, 2 * side + 7, rgb_for_ink(0xee));
		TCAnalysisCtx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.side = side;
		vcb_prepare(&ctx, img, side * side * 4);
		vcb_scan_pixels(&ctx);
		vcb_link(&ctx);
		vcb_finalize(&ctx);
		VCBModel m;
		vcb_model_build(&ctx, &m);
		int eB = m.lut[2 * side + 7], eA = m.lut[1 * side + 3];
		int merged = (eA > 0 && eB > 0 && m.net_rep[eA] == m.net_rep[eB]);
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_set_clock(&s, 2);
		int saw_on = 0, saw_off = 0;
		for (int t = 0; t < 12; t++) {
			vcb_sim_tick(&s);
			if (m.ent[eB].state)
				saw_on = 1;
			else
				saw_off = 1;
		}
		vcb_sim_free(&s);
		vcb_model_free(&m);
		vcb_ctx_free(&ctx);
		free(img);
		CHECK(merged && saw_on && saw_off,
				"sim: BUS follows its driver ON and OFF (no stuck-high)");
	}

	// --- BUS is multi-channel by colour: same-colour traces on a bus connect, but
	// a different-colour trace on the SAME bus stays independent. ---
	{
		const int side = 12;
		uint8_t *img = (uint8_t *)calloc((size_t)side * side * 4, 1);
		// driver: NOT (constant high) -> WRITE -> gray trace A touching the bus
		put(img, 1 * side + 1, rgb_for_ink(0x05));   // NOT
		put(img, 1 * side + 2, rgb_for_ink(0xff));   // WRITE
		put(img, 1 * side + 3, rgb_for_ink(0xee));   // gray A
		put(img, 1 * side + 4, rgb_for_ink(0xee));   // touches bus at (4,1)-(5,1)
		for (int y = 1; y <= 5; y++)
			put(img, y * side + 5, rgb_for_ink(0xe8)); // BUS_0 column x=5, rows 1..5
		put(img, 3 * side + 6, rgb_for_ink(0xee));   // gray B on the bus (same colour), row 3
		put(img, 3 * side + 7, rgb_for_ink(0xee));
		put(img, 5 * side + 6, rgb_for_ink(0xf0));   // RED C on the same bus, row 5 (isolated from B)
		put(img, 5 * side + 7, rgb_for_ink(0xf0));
		TCAnalysisCtx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.side = side;
		vcb_prepare(&ctx, img, side * side * 4);
		vcb_scan_pixels(&ctx);
		vcb_link(&ctx);
		vcb_finalize(&ctx);
		VCBModel m;
		vcb_model_build(&ctx, &m);
		int eA = m.lut[1 * side + 3];  // gray driver
		int eB = m.lut[3 * side + 7];  // gray receiver (same colour -> connected)
		int eC = m.lut[5 * side + 7];  // red receiver (different colour -> independent)
		int gray_connected = (eA > 0 && eB > 0 && m.net_rep[eA] == m.net_rep[eB]);
		int red_independent = (eC > 0 && m.net_rep[eC] != m.net_rep[eA]);
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_run(&s, 20);
		int gray_on = m.ent[eB].state; // 1: gray channel carries the signal
		int red_off = m.ent[eC].state; // 0: red channel is not driven
		vcb_sim_free(&s);
		vcb_model_free(&m);
		vcb_ctx_free(&ctx);
		free(img);
		CHECK(gray_connected && red_independent && gray_on == 1 && red_off == 0,
				"sim: BUS connects only same-colour traces (per-colour channels)");
	}

	// --- WIRELESS: the 4 channels (0x13..0x16) connect board-wide. A trace driven
	// into WIRELESS_0 via a READ turns on a physically disconnected WIRELESS_0
	// receiver's WRITE output; a WIRELESS_1 receiver on no channel stays off. ---
	{
		const int side = 14;
		uint8_t *img = (uint8_t *)calloc((size_t)side * side * 4, 1);
		// sender: NOT(constant high) -> WRITE -> gray -> READ -> WIRELESS_0
		put(img, 1 * side + 1, rgb_for_ink(0x05));
		put(img, 1 * side + 2, rgb_for_ink(0xff));
		put(img, 1 * side + 3, rgb_for_ink(0xee));
		put(img, 1 * side + 4, rgb_for_ink(0xee));
		put(img, 1 * side + 5, rgb_for_ink(0xfe)); // READ: trace drives the wireless
		put(img, 1 * side + 6, rgb_for_ink(0x13)); // WIRELESS_0 (input side)
		// receiver on the same channel, physically disconnected:
		put(img, 6 * side + 6, rgb_for_ink(0x13)); // WIRELESS_0 (output side)
		put(img, 6 * side + 7, rgb_for_ink(0xff)); // WRITE: wireless drives the trace
		put(img, 6 * side + 8, rgb_for_ink(0xee));
		put(img, 6 * side + 9, rgb_for_ink(0xee));
		// control on a different channel: WIRELESS_1 must not receive
		put(img, 10 * side + 6, rgb_for_ink(0x14));
		put(img, 10 * side + 7, rgb_for_ink(0xff));
		put(img, 10 * side + 8, rgb_for_ink(0xee));
		put(img, 10 * side + 9, rgb_for_ink(0xee));
		TCAnalysisCtx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.side = side;
		vcb_prepare(&ctx, img, side * side * 4);
		vcb_scan_pixels(&ctx);
		vcb_link(&ctx);
		vcb_finalize(&ctx);
		VCBModel m;
		vcb_model_build(&ctx, &m);
		int out_w0 = m.lut[6 * side + 9];   // WIRELESS_0 receiver output trace
		int out_w1 = m.lut[10 * side + 9];  // WIRELESS_1 control output trace
		VCBSim s;
		vcb_sim_init(&s, &m);
		vcb_sim_run(&s, 20);
		int recv = (out_w0 > 0 && m.ent[out_w0].state == 1);
		int ctrl_off = (out_w1 > 0 && m.ent[out_w1].state == 0);
		vcb_sim_free(&s);
		vcb_model_free(&m);
		vcb_ctx_free(&ctx);
		free(img);
		CHECK(recv && ctrl_off,
				"sim: WIRELESS carries a channel board-wide (READ in -> WRITE out), per channel");
	}

	// --- CLOCK seeding regression (session: "traces move when they should not"):
	// a board with TWO CLOCK components. The kernel (construct_model 0x3e3550)
	// records a SINGLE clock entity (+0x150, last wins) and never seeds clocks
	// into the initial evaluation list; only that one clock is toggled by the
	// interval scheduler. The event-driven tick must therefore match the
	// synchronous reference on every tick -- in particular the non-tracked
	// (secondary) clocks and the traces they drive must stay LOW, not pulse once
	// at startup. Guards vcb_sim_seed_all skipping ink 0x0b / 0x10. ---
	{
		uint32_t trace_rgb = 0;
		for (size_t i = 0; i < TC_INK_TABLE_LEN; i++)
			if (TC_INK_TABLE[i].category == 7) { trace_rgb = TC_INK_TABLE[i].editor_rgb; break; }
		const int s = 8;
		uint8_t im[8 * 8 * 4];
		memset(im, 0, sizeof(im));
		put(im, 1 * s + 1, rgb_for_ink(0x0b)); // clock A
		put(im, 1 * s + 2, rgb_for_ink(0xff)); // WRITE
		put(im, 1 * s + 3, trace_rgb);         // trace A (driven by clock A)
		put(im, 5 * s + 1, rgb_for_ink(0x0b)); // clock B (disconnected)
		put(im, 5 * s + 2, rgb_for_ink(0xff)); // WRITE
		put(im, 5 * s + 3, trace_rgb);         // trace B (driven by clock B)
		VCBModel ma, mb;
		int okc = 1;
		for (int which = 0; which < 2; which++) {
			TCAnalysisCtx c; memset(&c, 0, sizeof(c)); c.side = s;
			vcb_prepare(&c, im, sizeof(im));
			vcb_scan_pixels(&c);
			vcb_link(&c);
			vcb_finalize(&c);
			if (vcb_model_build(&c, which ? &mb : &ma) != 0) okc = 0;
			vcb_ctx_free(&c);
		}
		int match = okc;
		if (okc) {
			int32_t nn = ma.n_entities;
			VCBSim sa, sb;
			vcb_sim_init(&sa, &ma); vcb_sim_set_clock(&sa, 2);
			vcb_sim_init(&sb, &mb); vcb_sim_set_clock(&sb, 2);
			for (int t = 0; t < 12 && match; t++) {
				vcb_sim_tick_sync(&sa);
				vcb_sim_tick(&sb);
				for (int32_t k = 1; k <= nn; k++)
					if (ma.ent[k].state != mb.ent[k].state) { match = 0; break; }
			}
			vcb_sim_free(&sa); vcb_sim_free(&sb);
			vcb_model_free(&ma); vcb_model_free(&mb);
		}
		CHECK(match, "sim: multi-CLOCK board -- event tick matches sync (no spurious startup toggle)");
	}

	// --- bus through CROSS: a bus routed straight through a cross must stay ONE
	// net, so a signal driven onto one side reaches a trace on the far side, while
	// the perpendicular wire crossing there stays isolated. Guards the cross-skip in
	// the bus flood (vcb_model.c + vcb_bus.c); without it a bus splits at every cross. ---
	{
		uint32_t busrgb = 0, trrgb = 0;
		for (size_t i = 0; i < TC_INK_TABLE_LEN; i++) {
			if (!busrgb && TC_INK_TABLE[i].category == 4) busrgb = TC_INK_TABLE[i].editor_rgb;
			if (!trrgb && TC_INK_TABLE[i].category == 7) trrgb = TC_INK_TABLE[i].editor_rgb;
		}
		const int s = 12;
		uint8_t im[12 * 12 * 4];
		memset(im, 0, sizeof(im));
		put(im, 3 * s + 1, rgb_for_ink(0x05)); // NOT (constant HIGH)
		put(im, 3 * s + 2, rgb_for_ink(0xff)); // WRITE
		put(im, 3 * s + 3, trrgb);             // driven trace
		put(im, 3 * s + 4, busrgb);            // bus
		put(im, 3 * s + 5, busrgb);
		put(im, 3 * s + 6, rgb_for_ink(0x64)); // CROSS (bus passes through)
		put(im, 3 * s + 7, busrgb);            // bus (far side)
		put(im, 3 * s + 8, trrgb);             // receiver trace on the far side
		put(im, 2 * s + 6, trrgb);             // perpendicular trace through the cross
		put(im, 4 * s + 6, trrgb);
		TCAnalysisCtx c; memset(&c, 0, sizeof(c)); c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m; vcb_model_build(&c, &m);
		int dst = m.lut[3 * s + 8], vert = m.lut[2 * s + 6];
		VCBSim sim; vcb_sim_init(&sim, &m); vcb_sim_run(&sim, 25);
		int carried = (dst > 0 && m.ent[dst].state == 1);
		int isolated = (vert <= 0 || m.ent[vert].state == 0);
		vcb_sim_free(&sim); vcb_model_free(&m); vcb_ctx_free(&c);
		CHECK(carried && isolated,
				"bus: conducts straight through a CROSS (perpendicular wire stays isolated)");
	}

	// --- a BUS conducts through a TUNNEL pair, the same way it runs through a
	// CROSS. Found on a real project: without it, bus-linked nets on opposite
	// sides of a tunnel compile to separate entities. ---
	{
		const int s = 16;
		uint8_t im[16 * 16 * 4];
		memset(im, 0, sizeof(im));
		int y = 4;
		put(im, y * s + 0, rgb_for_ink(0x05));  // NOT source
		put(im, y * s + 1, rgb_for_ink(0xff));  // WRITE
		put(im, y * s + 2, rgb_for_ink(0xee));  // gray trace
		put(im, y * s + 3, rgb_for_ink(0xe8));  // BUS_0
		put(im, y * s + 4, rgb_for_ink(0x65));  // TUNNEL
		put(im, y * s + 8, rgb_for_ink(0x65));  // TUNNEL
		put(im, y * s + 9, rgb_for_ink(0xe8));  // BUS_0
		put(im, y * s + 10, rgb_for_ink(0xee)); // gray trace
		put(im, y * s + 11, rgb_for_ink(0xfe)); // READ
		put(im, y * s + 12, rgb_for_ink(0x0c)); // LED
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m;
		vcb_model_build(&c, &m);
		int32_t near_net = m.lut[y * s + 2], far_net = m.lut[y * s + 10];
		int joined = (near_net > 0 && far_net > 0 && m.net_rep &&
				m.net_rep[near_net] == m.net_rep[far_net]);
		VCBSim sim;
		vcb_sim_init(&sim, &m);
		vcb_sim_run(&sim, 4);
		int32_t led = m.lut[y * s + 12];
		CHECK(joined && led > 0 && m.ent[led].state == 1,
				"bus: conducts through a TUNNEL pair (bus-linked nets are one net)");
		vcb_sim_free(&sim);
		vcb_model_free(&m);
		vcb_ctx_free(&c);
	}

	// --- MESH merges same-ink groups board-wide, components included: two NOT
	// gates that each touch a mesh pixel compile to ONE entity. ---
	{
		const int s = 16;
		uint8_t im[16 * 16 * 4];
		memset(im, 0, sizeof(im));
		put(im, 2 * s + 2, rgb_for_ink(0x66)); // MESH
		put(im, 2 * s + 3, rgb_for_ink(0x05)); // NOT #1
		put(im, 9 * s + 9, rgb_for_ink(0x66)); // MESH, far away
		put(im, 9 * s + 10, rgb_for_ink(0x05)); // NOT #2
		put(im, 12 * s + 3, rgb_for_ink(0x05)); // NOT #3, no mesh -> stays separate
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m;
		vcb_model_build(&c, &m);
		int32_t a = m.lut[2 * s + 3], b = m.lut[9 * s + 10], d = m.lut[12 * s + 3];
		CHECK(a > 0 && a == b && d > 0 && d != a,
				"mesh: merges same-ink COMPONENTS board-wide (and only mesh-touching ones)");
		vcb_model_free(&m);
		vcb_ctx_free(&c);
	}

	// --- circuit_data byte [2] (n_conn) is the entity's IN-degree, and counting a
	// READ junction drawn against a CLOCK / VINPUT / TIMER in it cannot change the
	// simulation. Those inks are driven only by the interval scheduler / the virtual
	// input sweep, so the kernel must not schedule them from a net -- but the original
	// still counts the connection in n_conn (measured; see vcb_model.c). The count is
	// therefore deliberately inflated for exactly those three inks, which is safe only
	// because n_in is read by nothing else: vcb_gate_eval consults it for AND (0x02)
	// and NAND (0x06) alone, and neither can ever carry an inert input. This pins that
	// invariant down so the two facts can't drift apart. ---
	{
		const int s = 16;
		uint8_t im[16 * 16 * 4];
		memset(im, 0, sizeof(im));
		// row 1: NOT -> WRITE -> trace -> READ -> CLOCK   (the inert connection)
		put(im, 1 * s + 0, rgb_for_ink(0x05));  // NOT
		put(im, 1 * s + 1, rgb_for_ink(0xff));  // WRITE
		put(im, 1 * s + 2, rgb_for_ink(0xee));  // trace
		put(im, 1 * s + 3, rgb_for_ink(0xfe));  // READ
		put(im, 1 * s + 4, rgb_for_ink(0x0b));  // CLOCK
		// row 5: NOT -> WRITE -> trace -> READ -> AND     (a real input, for contrast)
		put(im, 5 * s + 0, rgb_for_ink(0x05));
		put(im, 5 * s + 1, rgb_for_ink(0xff));
		put(im, 5 * s + 2, rgb_for_ink(0xee));
		put(im, 5 * s + 3, rgb_for_ink(0xfe));
		put(im, 5 * s + 4, rgb_for_ink(0x02));  // AND
		TCAnalysisCtx c;
		memset(&c, 0, sizeof(c));
		c.side = s;
		vcb_prepare(&c, im, sizeof(im));
		vcb_scan_pixels(&c);
		vcb_link(&c);
		vcb_finalize(&c);
		VCBModel m;
		vcb_model_build(&c, &m);
		int32_t clk = m.lut[1 * s + 4], and_g = m.lut[5 * s + 4];
		int32_t net1 = m.lut[1 * s + 2], net2 = m.lut[5 * s + 2];
		uint8_t *nconn = (uint8_t *)calloc((size_t)m.n_entities + 1, 1);
		vcb_model_indegree(&m, nconn);
		// The CLOCK counts its READ, but it is NOT an input edge (nothing schedules it
		// from that net); the AND counts its READ and IS wired to it.
		int clock_counted = clk > 0 && nconn[clk] == 1 && m.ent[clk].inputs.count == 0 &&
				m.ent[clk].inert_inputs.count == 1;
		int and_wired = and_g > 0 && nconn[and_g] == 1 && m.ent[and_g].inputs.count == 1 &&
				m.ent[and_g].inert_inputs.count == 0;
		// A net's n_conn is its driver count: one NOT writes each of these two nets.
		int nets_counted = net1 > 0 && net2 > 0 && nconn[net1] == 1 && nconn[net2] == 1;
		// And no entity outside {CLOCK, VINPUT, TIMER} can carry an inert input at all,
		// which is what makes the inflated count unobservable to gate_eval.
		int only_three_inks = 1;
		for (int32_t k = 1; k <= m.n_entities; k++)
			if (m.ent[k].inert_inputs.count > 0 && m.ent[k].ink != 0x0b &&
					m.ent[k].ink != 0x0f && m.ent[k].ink != 0x10)
				only_three_inks = 0;
		free(nconn);
		CHECK(clock_counted && and_wired && nets_counted && only_three_inks,
				"model: n_conn is the in-degree; an inert READ counts but never wires up");
		// Behaviour: the CLOCK keeps its own phase while that net is high -- it is not
		// scheduled by it, and its inflated n_conn does not reach any gate handler.
		VCBSim sim;
		vcb_sim_init(&sim, &m);
		vcb_sim_set_clock(&sim, 4);
		vcb_sim_set_seed(&sim, 0);
		int toggles = 0;
		uint8_t prev = m.ent[clk].state;
		for (int t = 0; t < 16; t++) {
			vcb_sim_tick(&sim);
			if (m.ent[clk].state != prev) {
				toggles++;
				prev = m.ent[clk].state;
			}
		}
		vcb_sim_free(&sim);
		// 16 ticks at interval 4 -> the CLOCK is scheduled at ticks 0,4,8,12: 4 toggles.
		CHECK(toggles == 4,
				"sim: a CLOCK behind a READ keeps its interval phase (inert connection)");
		vcb_model_free(&m);
		vcb_ctx_free(&c);
	}

	printf("\n%d failure(s)\n", bad);
	return bad;
}
