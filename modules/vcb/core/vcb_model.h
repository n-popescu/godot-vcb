/* vcb_model.h -- Godot-free circuit-model builder (Phase B).
 *
 * Turns the finalized analysis context (after vcb_finalize) into the compiled
 * circuit graph: an entity list (gates + trace nets), a pixel->entity LUT, and
 * the flat circuit_data / adjacency arrays that TransistorCircuitModel exposes.
 *
 * Reconstructed from vcb.exe build_stage2 (0x3e1ac0 / component proc 0x3e1e40)
 * and construct_model (0x3e3550), verified by disassembly:
 *   - entities are 1-indexed (index 0 reserved); components first, then traces;
 *   - each component entity's ink is its first body pixel's classifier ink;
 *   - trace-net entities use ink 0xff (as construct_model forces);
 *   - circuit_data is sidelength^2 * 4 bytes, cell k = {state, ink, n_conn, 0};
 *   - adjacency is [0] then, per entity, its connected indices and a 0 separator.
 * The directed input/output edges (used by the simulator) follow VCB's connection
 * rule: a trace adjacent to a gate is an input by default; a WRITE (0xff) pixel
 * makes it an output. Validated for logic in the unit tests; bit-exactness vs the
 * original binary is a Phase-C concern.
 */
#ifndef VCB_MODEL_H
#define VCB_MODEL_H

#include "vcb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A growable int32 list (entity-index adjacency). */
typedef struct VCBIntVec {
	int32_t *items;
	int32_t  count;
	int32_t  cap;
} VCBIntVec;

typedef struct VCBEntity {
	uint8_t   ink;      /* classifier ink (gate type); trace nets carry 0xff */
	uint8_t   is_trace; /* 1 = trace net, 0 = gate/component */
	uint8_t   state;    /* simulation state (0/1); model starts at 0 */
	VCBIntVec inputs;   /* gate: the trace nets it reads */
	VCBIntVec outputs;  /* gate: the trace nets it drives */
	VCBIntVec conns;    /* undirected connection list (for circuit_data/adjacency) */
	/* Nets a READ junction wires to this gate that the kernel must NOT schedule it
	 * from: CLOCK / VINPUT / TIMER are driven only by the interval scheduler and the
	 * virtual-input sweep. The original still counts them in the gate's circuit_data
	 * n_conn byte (measured on probe p10), so they are kept here -- counted, never
	 * simulated. */
	VCBIntVec inert_inputs;
} VCBEntity;

typedef struct VCBModel {
	int32_t    sidelength;   /* entity-LUT side (== circuit_width) */
	int32_t    board_side;   /* board width/height in pixels */
	int32_t    n_entities;   /* entities live at indices 1..n_entities (0 reserved) */
	int32_t    n_components; /* components are 1..n_components, trace nets after */
	VCBEntity *ent;          /* [n_entities + 1]; ent[0] is unused */
	int32_t   *lut;          /* [board_side^2]: pixel flat index -> entity index (0 = none) */
	/* Bus merge: [n_entities+1] union-find representative per entity. Trace nets
	 * joined by a common bus net share a representative so the simulator treats them
	 * as one electrical net (buses transport data between the traces they touch).
	 * Gates and unmerged traces map to themselves (net_rep[k] == k). NULL until built. */
	int32_t   *net_rep;
	int32_t    clock_value;  /* CLOCK entity value (ink 0x0b) -> model stat_150 */
	int32_t    timer_value;  /* TIMER entity value (ink 0x10) -> model stat_154 */
	/* Initial VMem word image (model +0x158 / engine circuit_state_arr), built by
	 * the compiler's compute_vmem_data (vcb_vmem_build). NULL until built. */
	int32_t   *vmem;
	int32_t    vmem_len;
	/* VMem latch entity-index lists, appended by the builder 0x3e3c70 from the
	 * compute_vmem_data `queues` arg: address latches (ink 0x0d) -> model +0x170,
	 * content latches (ink 0x0e) -> model +0x188. The runtime VMem kernel maps the
	 * address bus / data bus from these; stored here ready for that port. */
	int32_t   *vmem_addr_entities;
	int32_t    vmem_addr_count;
	int32_t   *vmem_content_entities;
	int32_t    vmem_content_count;
} VCBModel;

/* Build the model from a finalized ctx (entities_a = components, raw_traces =
 * full trace nets in entities_b order). Returns 0 on success, non-zero on OOM. */
int  vcb_model_build(TCAnalysisCtx *ctx, VCBModel *out);
void vcb_model_free(VCBModel *m);

/* Fill out (n_entities + 1 bytes) with each entity's `n_conn` -- byte [2] of its
 * circuit_data cell. Measured against the original engine's live state texture
 * (tools/phasec, probe p1), that byte is the entity's **in-degree**, not its total
 * connection count:
 *   - a component: the number of distinct nets it READs (its input count, which is
 *     what the gate handlers compare n_high against);
 *   - a trace net: the number of distinct components that WRITE to it.
 * Nets are counted per bus/mesh group (the original merges those into one entity at
 * compile time), and every member of a group reports the group's count. It is a
 * byte, so it wraps exactly as the original's cell does. */
void vcb_model_indegree(const VCBModel *m, uint8_t *out);

/* Emit circuit_data (sidelength^2 * 4 bytes; cell k = {state, ink, n_conn, 0},
 * 1-indexed) exactly as construct_model does. Caller frees *out_data. */
void vcb_model_emit_circuit_data(const VCBModel *m, uint8_t **out_data, int32_t *out_len);

/* Emit adjacency ([0] then per-entity {conn indices, 0}). Caller frees *out_adj. */
void vcb_model_emit_adjacency(const VCBModel *m, int32_t **out_adj, int32_t *out_len);

/* Fill out_x / out_y (each [n_entities + 1]) with a representative board pixel per
 * entity -- the first board cell that maps to it -- or (-1, -1) for an entity with
 * no board pixels. This is the inverse of the die/entity LUT (entity -> board),
 * used to build texture_inverse_entitylut. */
void vcb_model_build_inverse(const VCBModel *m, int32_t *out_x, int32_t *out_y);

#ifdef __cplusplus
}
#endif

#endif /* VCB_MODEL_H */
