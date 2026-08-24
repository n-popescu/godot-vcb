// Fuzz harness for the tick kernel's delta-accumulator bookkeeping.
//
// The kernel never recomputes an entity's input tally: when an output changes it
// adds +/-1 to each neighbour's `n_high` and schedules it. Every gate decision is
// then made from those running counts, so a single mis-applied delta silently
// corrupts the circuit from that tick on -- the "a trace moves when it shouldn't"
// class of bug. This fuzzer compiles thousands of random boards through the real
// front-end and, after every tick, recomputes each entity's tally from scratch out
// of the actual entity states and compares it with the incrementally maintained
// one. It also runs a second, independent compile of the same board in lockstep to
// catch any order- or allocation-dependent behaviour.
//
// (The behavioural reference for the *semantics* is the original engine itself:
// tools/phasec diffs this core against vcb.exe tick by tick.)
#include "../vcb_pipeline.h"
#include "../vcb_classifier.h"
#include "../vcb_resolver.h"
#include "../vcb_model.h"
#include "../vcb_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 0;
static uint32_t xr(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static void put(uint8_t *img, int i, uint32_t rgb) {
	img[i * 4] = (rgb >> 16) & 0xff; img[i * 4 + 1] = (rgb >> 8) & 0xff;
	img[i * 4 + 2] = rgb & 0xff; img[i * 4 + 3] = 0xff;
}

// A palette weighted toward traces + gates + junctions so random boards actually
// form connected logic. Includes CLOCK so there is a driver.
static const uint32_t PAL[] = {
	0x000000, 0x000000, 0x000000,               // empty (weighted)
	0x2a3541, 0x9fa8ae, 0xa1555e, 0x567ba1,      // traces (gray/white/red/blue)
	0x2e475d, 0x4d383e,                          // READ, WRITE
	0x92ff63, 0xffc663, 0x63f2ff, 0xff628a,      // BUFFER, AND, OR, NOT
	0xffa200, 0x30d9ff, 0xae74ff, 0xa600ff,      // NAND, NOR, XOR, XNOR
	0x63ff9f, 0x384d47,                          // LATCH_ON, LATCH_OFF
	0x66788e, 0x535572, 0x646a57,                // CROSS, TUNNEL, MESH
	0x7a2f24, 0x3e7a24,                          // BUS_0, BUS_1
	0xff0041, 0xff0041,                          // CLOCK (weighted: exercise multi-clock)
	0xff6700,                                    // TIMER
};
enum { NPAL = (int)(sizeof(PAL) / sizeof(PAL[0])) };

int main(int argc, char **argv) {
	int iters = (argc > 1) ? atoi(argv[1]) : 4000;
	int side = 12, ticks = 40, clk = 3;
	int fails = 0, compiled = 0;
	for (int it = 0; it < iters; it++) {
		rng_state = 0x9e3779b9u ^ (uint32_t)(it * 2654435761u);
		if (rng_state == 0) rng_state = 1;
		int npix = side * side;
		uint8_t *img = (uint8_t *)calloc((size_t)npix * 4, 1);
		for (int i = 0; i < npix; i++) {
			uint32_t c = PAL[xr() % NPAL];
			if (c) put(img, i, c);
		}
		// two independent compiles -> two identical models
		VCBModel ms, me;
		int okc = 1;
		for (int which = 0; which < 2; which++) {
			TCAnalysisCtx ctx; memset(&ctx, 0, sizeof(ctx)); ctx.side = side;
			vcb_prepare(&ctx, img, npix * 4);
			vcb_scan_pixels(&ctx);
			vcb_link(&ctx);
			vcb_finalize(&ctx);
			VCBModel *dst = which ? &me : &ms;
			if (vcb_model_build(&ctx, dst) != 0) okc = 0;
			vcb_ctx_free(&ctx);
			if (!okc) break;
		}
		free(img);
		if (!okc) { continue; }
		compiled++;
		int32_t n = ms.n_entities;
		// run sync + event from identical initial state
		VCBSim ss, se;
		vcb_sim_init(&ss, &ms); vcb_sim_set_clock(&ss, clk); vcb_sim_set_timer_us(&ss, 5); vcb_sim_set_seed(&ss, 12345u);
		vcb_sim_init(&se, &me); vcb_sim_set_clock(&se, clk); vcb_sim_set_timer_us(&se, 5); vcb_sim_set_seed(&se, 12345u);
		int diverged = 0;
		uint8_t *tally = (uint8_t *)calloc((size_t)n + 1, 1);
		for (int t = 0; t < ticks && !diverged; t++) {
			vcb_sim_tick(&ss);
			vcb_sim_tick(&se);
			for (int32_t k = 1; k <= n; k++) {
				if (ms.ent[k].state != me.ent[k].state) {
					printf("DIVERGE iter=%d tick=%d entity=%d ink=%02x is_trace=%d a=%d b=%d\n",
					       it, t, k, ms.ent[k].ink, ms.ent[k].is_trace, ms.ent[k].state, me.ent[k].state);
					diverged = 1; fails++; break;
				}
			}
			if (diverged) break;
			if (ss.events != se.events) {
				printf("EVENTDIFF iter=%d tick=%d a=%lld b=%lld\n", it, t,
						(long long)ss.events, (long long)se.events);
				diverged = 1; fails++; break;
			}
			// the invariant: the running tallies must equal a fresh recomputation
			memset(tally, 0, (size_t)n + 1);
			for (int32_t k = 1; k <= n; k++) {
				if (!ms.ent[k].state)
					continue;
				for (int32_t j = 0; j < ss.adj[k].count; j++)
					tally[ss.adj[k].items[j]]++;
			}
			for (int32_t k = 1; k <= n; k++) {
				if (ss.n_high[k] != tally[k]) {
					printf("TALLY iter=%d tick=%d entity=%d ink=%02x running=%d actual=%d\n",
							it, t, k, ms.ent[k].ink, ss.n_high[k], tally[k]);
					diverged = 1; fails++; break;
				}
			}
		}
		free(tally);
		vcb_sim_free(&ss); vcb_sim_free(&se);
		vcb_model_free(&ms); vcb_model_free(&me);
		if (fails >= 8) { printf("... stopping after %d divergences\n", fails); break; }
	}
	printf("\nfuzz: %d iters, %d compiled, %d divergences\n", iters, compiled, fails);
	return fails ? 1 : 0;
}
