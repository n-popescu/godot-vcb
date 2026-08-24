#include "transistor_circuit_model.h"

#include <stdlib.h>

extern "C" {
#include "core/vcb_model.h"
}

// Frees the owned native graph. The Godot-free model allocates its own internal
// arrays (vcb_model_free releases them); the struct itself is malloc'd by the
// compiler when it builds the model.
TransistorCircuitModel::~TransistorCircuitModel() {
	if (native) {
		vcb_model_free(native);
		free(native);
		native = nullptr;
	}
}
