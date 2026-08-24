/* vcb_sim.h -- the recovered VCB tick kernel over a built VCBModel.
 *
 * This is TransistorEngine::compute (vcb.exe RVA 0x28f2c0) plus its per-ink gate
 * handlers (jump table 0x28fc40), decompiled with Ghidra and validated tick-by-tick
 * against the original engine run headlessly (see tools/phasec/).
 *
 * The engine is an **event-driven delta-accumulator** simulator, not a synchronous
 * one. Each entity carries a running tally `n_high` of how many of its inputs are
 * currently high; when an entity's output changes it adds +/-1 to each neighbour's
 * tally and schedules that neighbour. A tick is exactly one gate->net hop:
 *
 *   PASS 1  evaluate the scheduled components; changes propagate into the net list.
 *   PASS 2  recompute the scheduled nets (`state = n_high != 0`, a wired-OR);
 *           changes propagate into the component list for the *next* tick.
 *
 * Two consequences that a synchronous simulator gets wrong, and that the original
 * really does:
 *
 *  - **`events` counts scheduled propagations, not transitions.** It is
 *    `+= |net list|` after PASS 1 and `+= |component list|` after PASS 2, and a net
 *    driven by two gates is pushed (and counted) twice.
 *  - **Entities are only evaluated when scheduled.** Only inks 0x05..0x09 (NOT,
 *    NAND, NOR, XNOR, LATCH_ON) are seeded, so e.g. an unconnected AND is never
 *    evaluated and stays OFF even though its handler has no 0-input guard.
 *
 * The component work list stores `entity * delta` -- the *sign* records whether the
 * net that scheduled the entity went up or down. LATCH, VMem-latch and RANDOM
 * handlers read that sign, which is why a LATCH_ON comes up HIGH on the first tick
 * (seeded positive) and a LATCH toggles on a rising input only.
 *
 * Connections come from the model: a component is wired to a net only through a
 * READ (input) or WRITE (output) junction pixel, deduplicated per (component, net).
 *
 * KNOWN GAP -- adjacency ORDER, and the one thing it is observable through.
 * Within a pass, evaluation order cannot change any result: PASS 1 reads only
 * component tallies (which only PASS 2 writes) and PASS 2 reads only net tallies
 * (which only PASS 1 writes), and every handler is a pure function of
 * (state, n_high, n_inputs, schedule sign). The single exception is RANDOM, whose
 * handler draws from one MT19937 shared by the whole board: if two RANDOM inks are
 * scheduled in the same tick, whichever is evaluated first takes the earlier draw.
 * The original orders an entity's connection list by its compiler's flood-fill
 * pixel order (build_stage2's trace pass, 0x3e2030, appends to the component's
 * vector at +0x60 as it walks each trace's pixels); this core orders by entity
 * index. Measured over 400 random boards: every board with zero or one RANDOM ink
 * matches the original exactly (77/77), and 289/323 of those with two or more do --
 * the rest differ only in which RANDOM took which draw, and in what follows from
 * that. Nothing else in the engine can observe the order.
 */
#ifndef VCB_SIM_H
#define VCB_SIM_H

#include "vcb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VCBSim {
	VCBModel  *model;
	int64_t    tick;      /* current_tick   (engine +0x150) */
	int64_t    events;    /* current_event  (engine +0x160) */

	/* circuit_data accumulators. n_high is a *byte* in the original's 4-byte cell
	 * {state, ink, n_inputs, n_high}, so it is a byte here and wraps the same way.
	 * The state itself lives in model->ent[k].state. */
	uint8_t   *n_high;    /* [n+1] tally of high inputs (for a net: of high drivers) */
	uint8_t   *n_in;      /* [n+1] circuit_data byte [2]: in-degree (vcb_model_indegree) */

	/* Directed adjacency: for a component, the nets it drives; for a net
	 * representative, the components that read it. Deduplicated. */
	VCBIntVec *adj;       /* [n+1] */
	VCBIntVec *members;   /* [n+1] per representative: the traces sharing it */

	VCBIntVec  cur_g;     /* engine +0x220: components, entries signed by direction */
	VCBIntVec  nxt_n;     /* engine +0x238: nets, entries positive */
	uint8_t    active_ready;

	int32_t    clock_interval; /* CLOCK toggles when tick % interval == 0 (0 = off) */
	/* TIMER is real-time in the original, not tick-based: the kernel fires it when
	 * `timer_interval_us + paused_us < elapsed_us` since the last fire, reading a
	 * wall clock. The core has no clock of its own, so the host feeds elapsed time
	 * through vcb_sim_add_time_us(); a host that never does (the headless trace
	 * tool) simply never fires the TIMER, exactly as the original does when no wall
	 * time passes between solves. */
	int64_t    timer_interval_us;
	int64_t    timer_elapsed_us;
	int64_t    timer_paused_us;

	VCBIntVec  fired_breakpoints; /* BREAKPOINT entities that rose 0->1 this solve */
	uint8_t    breakpoint_hit;    /* a breakpoint fired: stop the run after this tick */

	/* MT19937 (RANDOM), seeded with multiplier 0x6c078965 as the kernel does. */
	uint32_t   mt[624];
	int32_t    mt_index;
	int32_t    mt_seeded;

	/* VMem runtime. The address is the weighted binary of the address-latch states;
	 * a content-latch change writes memory[address], and an address change reads it
	 * back into the content latches and sets vmem_lock for the next tick (during
	 * which latch schedules are recorded as occurrences instead of toggling). */
	int32_t   *vmem;
	int32_t    vmem_len;
	int32_t    vmem_address;
	int32_t   *vmem_addr_ent;
	int32_t    vmem_addr_n;
	int32_t   *vmem_content_ent;
	int32_t    vmem_content_n;
	uint8_t    vmem_present;
	uint8_t    vmem_lock;            /* engine +0x2c4 */
	uint8_t    vmem_addr_changed;
	uint8_t    vmem_content_changed;
	VCBIntVec  vmem_occ_addr;        /* ticks at which an address latch was re-driven */
	VCBIntVec  vmem_occ_content;     /* ... and a content latch */

	/* snapshot history (step back / forward) */
	uint8_t  **snaps;
	int        snap_count;
	int        snap_cap;
	int        snap_cursor;
} VCBSim;

/* Attach a simulator to a model: build the adjacency and seed the work list. */
void vcb_sim_init(VCBSim *s, VCBModel *m);
void vcb_sim_free(VCBSim *s);

/* Advance one tick / n ticks. vcb_sim_run stops early if a breakpoint fires. */
void vcb_sim_tick(VCBSim *s);
void vcb_sim_run(VCBSim *s, int n);

/* Alias for vcb_sim_tick. The kernel's handlers depend on *how* an entity was
 * scheduled, so there is no separate synchronous formulation; kept for callers. */
void vcb_sim_tick_sync(VCBSim *s);

/* Rebuild the tallies and re-seed the work list (init / after a snapshot restore). */
void vcb_sim_seed_all(VCBSim *s);

/* Tell the scheduler an entity's state was written from outside the tick loop
 * (mouse override / virtual input) so its effect propagates. */
void vcb_sim_mark_external(VCBSim *s, int32_t entity);

/* The pure per-ink gate handlers from the 0x28fc40 jump table. The handlers that
 * also depend on the scheduling entry's sign (LATCH 0x09/0x0a, VMem 0x0d/0x0e,
 * RANDOM 0x12) are applied in the tick loop and return 0 here. */
int  vcb_gate_eval(uint8_t ink, int n_high, int n_inputs);

/* Reset the per-solve report lists (breakpoints + VMem occurrences). */
void vcb_sim_clear_fired(VCBSim *s);

void vcb_sim_set_clock(VCBSim *s, int32_t interval);
/* Set the TIMER period in **microseconds** (0 disables it). */
void vcb_sim_set_timer_us(VCBSim *s, int64_t interval_us);
/* Advance the TIMER's real-time accumulator; `paused_us` is time the simulation
 * spent paused, which the kernel adds to the threshold rather than the elapsed. */
void vcb_sim_add_time_us(VCBSim *s, int64_t elapsed_us, int64_t paused_us);
void vcb_sim_set_seed(VCBSim *s, uint32_t seed);

/* Snapshot history (drives TransistorEngine's snapshot_* methods). */
void vcb_sim_snapshot_take(VCBSim *s);
int  vcb_sim_snapshot_prev_possible(VCBSim *s);
int  vcb_sim_snapshot_next_possible(VCBSim *s);
void vcb_sim_snapshot_restore_prev(VCBSim *s);
void vcb_sim_snapshot_restore_next(VCBSim *s);
void vcb_sim_snapshot_clear_all(VCBSim *s);
void vcb_sim_snapshot_clear_next(VCBSim *s);

#ifdef __cplusplus
}
#endif

#endif /* VCB_SIM_H */
