#ifndef TRANSISTOR_BUILDER_HELPER_H
#define TRANSISTOR_BUILDER_HELPER_H

#include "core/array.h"
#include "core/reference.h"

// Build/setup helper (initial state + heatmap) used by the editor. Method surface
// recovered from vcb.exe MethodBind symbols; bodies are remaining Phase-B work.
class TransistorBuilderHelper : public Reference {
	GDCLASS(TransistorBuilderHelper, Reference);

protected:
	static void _bind_methods();

public:
	Array get_build_info();
	void clear();
	void build_circuit(const Array &p_data, int a, int b, const Vector2 &p_pos);
	void build_heatmap(int a, int b, const Array &p_data);

	TransistorBuilderHelper() {}
};

#endif // TRANSISTOR_BUILDER_HELPER_H
