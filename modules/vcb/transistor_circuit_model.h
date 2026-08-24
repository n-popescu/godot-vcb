#ifndef TRANSISTOR_CIRCUIT_MODEL_H
#define TRANSISTOR_CIRCUIT_MODEL_H

#include "core/pool_vector.h"
#include "core/reference.h"

// Godot-free native model (owned; the simulator's input). Forward-declared here
// so the header stays light; freed in the destructor (see .cpp).
struct VCBModel;

// The compiled circuit graph produced by TransistorCompiler and consumed by
// TransistorEngine. A pure data container (no GDScript-callable methods) passed
// by Ref between the two. Field layout recovered in vcb-engine-recovery
// (docs/transistor_circuit_model.md).
class TransistorCircuitModel : public Reference {
	GDCLASS(TransistorCircuitModel, Reference);

protected:
	static void _bind_methods() {}

public:
	int circuit_width = 0;                 // == entity-LUT side length

	PoolVector<uint8_t> circuit_data;      // side^2 * 4 bytes: per entity {state, ink, n_conn, 0}
	PoolVector<int> entity_state_index;    // +0x108: per-entity state/output index list
	PoolVector<int> adjacency;             // +0x120: connection indices (per-entity segments)
	PoolVector<int> special_io;            // +0x138: WRITE/READ/... entity indices
	int clock_value = 0;                   // +0x150 (CLOCK entity)
	int timer_value = 0;                   // +0x154 (TIMER entity)
	PoolVector<int> tick_schedule;         // +0x158
	PoolVector<int> aux_170;               // +0x170
	PoolVector<int> aux_188;               // +0x188
	PoolVector<int> vinput_indices;        // +0x1a0

	// Owned native graph (built by the compiler, simulated by the engine).
	struct VCBModel *native = nullptr;

	TransistorCircuitModel() {}
	~TransistorCircuitModel();
};

#endif // TRANSISTOR_CIRCUIT_MODEL_H
