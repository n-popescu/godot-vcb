#include "transistor_builder_helper.h"

#include "core/class_db.h"

// Intentionally minimal. The GDScript API audit (docs/gdscript_api_audit.md) found
// that the shipped game never instantiates TransistorBuilderHelper -- it is
// registered only so the native class surface is complete. Its method
// names/arities were inferred from the vcb.exe MethodBind signatures but never
// confirmed against a call site (docs/transistor_helpers.md), and no behaviour was
// recovered, so there is nothing to reconstruct faithfully. These bodies return
// empty/no-op; if a future game version uses this class, decompile the concrete
// methods first rather than guessing.

Array TransistorBuilderHelper::get_build_info() { return Array(); }
void TransistorBuilderHelper::clear() {}
void TransistorBuilderHelper::build_circuit(const Array &, int, int, const Vector2 &) {}
void TransistorBuilderHelper::build_heatmap(int, int, const Array &) {}

void TransistorBuilderHelper::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_build_info"), &TransistorBuilderHelper::get_build_info);
	ClassDB::bind_method(D_METHOD("clear"), &TransistorBuilderHelper::clear);
	ClassDB::bind_method(D_METHOD("build_circuit", "data", "a", "b", "pos"),
			&TransistorBuilderHelper::build_circuit);
	ClassDB::bind_method(D_METHOD("build_heatmap", "a", "b", "data"),
			&TransistorBuilderHelper::build_heatmap);
}
