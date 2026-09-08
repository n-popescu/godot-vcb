#ifndef TRANSISTOR_BUILDER_HELPER_H
#define TRANSISTOR_BUILDER_HELPER_H

#include "core/array.h"
#include "core/reference.h"

// Editor-side builder helper. The method surface is taken from the shipped engine's
// own ClassDB (clear / setup / flood_fill / get_cells); bodies are no-ops -- see the
// .cpp for why, and for the caveat that the arities are unverified.
class TransistorBuilderHelper : public Reference {
	GDCLASS(TransistorBuilderHelper, Reference);

protected:
	static void _bind_methods();

public:
	void clear();
	void setup();
	void flood_fill();
	Array get_cells();

	TransistorBuilderHelper() {}
};

#endif // TRANSISTOR_BUILDER_HELPER_H
