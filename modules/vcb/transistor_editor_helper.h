#ifndef TRANSISTOR_EDITOR_HELPER_H
#define TRANSISTOR_EDITOR_HELPER_H

#include "core/image.h"
#include "core/reference.h"

// Editor-side helper: board init, bucket fill/replace, and transpose. Operates on
// Godot Images; needed for the editor, not for headless simulation.
//
// Method names + signatures are taken from the ACTUAL game GDScript (audited in
// docs/gdscript_api_audit.md): editor.gd, tool_bucket.gd, tool_selection.gd.
// (The MethodBind arities in vcb.exe match these; the earlier names in
// vcb-engine-recovery/MASTER_PLAN were inferred and are superseded here.)
class TransistorEditorHelper : public Reference {
	GDCLASS(TransistorEditorHelper, Reference);

	int circuit_span = 0;

protected:
	static void _bind_methods();

public:
	void initialize(int p_span);                                    // editor.gd:62
	void bucket_flood_fill(const Color &p_target, const Color &p_draw, const Vector2 &p_pos,
			const Ref<Image> &p_active_layer, const Ref<Image> &p_logic_layer,
			bool p_is_logic_layer, bool p_pass_through_crosses); // tool_bucket.gd:53
	void bucket_replace(const Color &p_target, const Color &p_draw,
			const Ref<Image> &p_img);                            // tool_bucket.gd:63
	void transpose(const Ref<Image> &p_img);                        // tool_selection.gd:331

	TransistorEditorHelper() {}
};

#endif // TRANSISTOR_EDITOR_HELPER_H
