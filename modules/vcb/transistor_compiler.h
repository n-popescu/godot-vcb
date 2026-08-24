#ifndef TRANSISTOR_COMPILER_H
#define TRANSISTOR_COMPILER_H

#include "core/image.h"
#include "core/pool_vector.h"
#include "core/reference.h"

#include "transistor_circuit_model.h"

// Converts a circuit pixel-image into a compiled TransistorCircuitModel.
// The circuit-graph algorithm is the recovered Godot-free core in core/; this
// class is the Godot ClassDB wrapper that feeds it the Image and turns its output
// into the model + textures. Method surface recovered from the vcb.exe MethodBind
// symbols (see vcb-engine-recovery/MASTER_PLAN.md).
class TransistorCompiler : public Reference {
	GDCLASS(TransistorCompiler, Reference);

	Ref<Image> building_image;
	Ref<TransistorCircuitModel> circuit_model;
	int progress = 0;   // 0..1000, or -1 on compile error
	int entitylist_sidelength = 0;
	Array errors;       // [ [code, Vector2(pos)], ... ]
	Array textures;     // 6 Images (on, off, die, inverse_entitylut, buslut, busentities)
	Array stats;

protected:
	static void _bind_methods();

public:
	// Tunnel/compile error codes (values recovered from the binary).
	enum {
		UNEXPECTED_TUNNEL_ENTRANCE = 0,
		UNMATCHED_TUNNEL_LEFT = 1,
		UNMATCHED_TUNNEL_RIGHT = 2,
		UNMATCHED_TUNNEL_UP = 3,
		UNMATCHED_TUNNEL_DOWN = 4,
	};

	void setup(const Ref<Image> &p_image);
	void compute(const Variant &p_userdata); // thread worker: runs the pipeline
	int get_progress() const;
	int get_entitylist_sidelength() const;
	Ref<TransistorCircuitModel> get_circuit_model();
	Array get_textures();
	Array get_stats();
	Array get_errors();
	void compute_vmem_data(const PoolVector<uint8_t> &p_live_vmem,
			const PoolVector<int> &p_assembly, const Array &p_queues);
	void set_vinput_entities_indexes(const Array &p_indexes);

	TransistorCompiler() {}
};

#endif // TRANSISTOR_COMPILER_H
