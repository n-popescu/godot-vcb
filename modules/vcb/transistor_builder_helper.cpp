#include "transistor_builder_helper.h"

#include "core/class_db.h"

// Intentionally minimal. The GDScript API audit (docs/gdscript_api_audit.md) found
// that the shipped game never instantiates TransistorBuilderHelper -- it is
// registered only so the native class surface is complete, and no behaviour was
// recovered, so there is nothing to reconstruct faithfully. These bodies are no-ops;
// if a future game version (or a mod) uses this class, decompile the concrete
// methods first rather than guessing.
//
// The METHOD NAMES here are authoritative: they were read out of the shipped
// engine's own ClassDB (ClassDB.class_get_method_list on vcb.x86_64), which lists
// exactly `clear`, `setup`, `flood_fill`, `get_cells`. The previous names
// (`build_circuit`, `build_heatmap`, `get_build_info`) were inferred from RTTI
// signatures and were wrong -- docs/transistor_helpers.md flagged that guess.
//
// ARITIES ARE NOT VERIFIED. The release template strips argument metadata from
// class_get_method_list and suppresses the arg-count error text, so the binding
// below takes no arguments. If a caller turns up, recover the real signature from
// the MethodBind template instantiation before relying on it.

Array TransistorBuilderHelper::get_cells() { return Array(); }
void TransistorBuilderHelper::clear() {}
void TransistorBuilderHelper::setup() {}
void TransistorBuilderHelper::flood_fill() {}

void TransistorBuilderHelper::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &TransistorBuilderHelper::clear);
	ClassDB::bind_method(D_METHOD("setup"), &TransistorBuilderHelper::setup);
	ClassDB::bind_method(D_METHOD("flood_fill"), &TransistorBuilderHelper::flood_fill);
	ClassDB::bind_method(D_METHOD("get_cells"), &TransistorBuilderHelper::get_cells);
}
