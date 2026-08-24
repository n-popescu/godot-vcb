/* register_types.cpp -- module entry point.
 *
 * Registers the five native VCB classes with Godot's ClassDB so the game's
 * GDScript can instantiate them by name (TransistorCompiler.new(), etc.) with no
 * GDNative library. These are the only five non-Godot RTTI classes in the original
 * vcb.exe (confirmed by the binary RTTI audit, docs/rtti_class_audit.md); the
 * remaining ~6800 classes are stock Godot 3.5.1, which the game links normally.
 *
 * register_vcb_types() is invoked by the engine's module init (SCsub/config.py wire
 * this file into the Godot build). */
#include "register_types.h"

#include "core/class_db.h"

#include "transistor_builder_helper.h"
#include "transistor_circuit_model.h"
#include "transistor_compiler.h"
#include "transistor_editor_helper.h"
#include "transistor_engine.h"

void register_vcb_types() {
	/* TransistorCircuitModel is a pure Ref data container (no methods); the other
	 * four expose the compile/simulate/edit API the game calls. */
	ClassDB::register_class<TransistorCircuitModel>();
	ClassDB::register_class<TransistorCompiler>();
	ClassDB::register_class<TransistorEngine>();
	ClassDB::register_class<TransistorEditorHelper>();
	ClassDB::register_class<TransistorBuilderHelper>();
}

void unregister_vcb_types() {
	/* No teardown needed: ClassDB owns the registrations for the process lifetime. */
}
