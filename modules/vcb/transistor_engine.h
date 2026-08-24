#ifndef TRANSISTOR_ENGINE_H
#define TRANSISTOR_ENGINE_H

#include "core/image.h"
#include "core/pool_vector.h"
#include "core/reference.h"
#include "scene/resources/texture.h"

#include "transistor_circuit_model.h"

// Godot-free simulator state (owned; forward-declared to keep the header light).
struct VCBSim;

// Runs the simulation tick loop over a compiled TransistorCircuitModel.
// The tick behaviour was decompiled in vcb-engine-recovery
// (src/transistor_engine.c, docs/transistor_engine_methods.md); the actual gate
// evaluation is the recovered, unit-tested core/vcb_sim simulator this class
// drives. This class registers the exact method surface the game calls
// (simulator.gd).
class TransistorEngine : public Reference {
	GDCLASS(TransistorEngine, Reference);

	Ref<TransistorCircuitModel> model;
	Ref<ImageTexture> state_texture;
	int random_seed = 0;
	int clock_interval = 0;
	int timer_interval_us = 0;

	// Virtual-display settings (stored by set_vdisplay_settings; see
	// vcb-engine-recovery/docs/vmem_vdisplay.md). The raster itself needs the VMem
	// memory kernel that runs in the un-decompiled `compute` worker.
	int vdisplay_width = 0;
	int vdisplay_height = 0;
	int vdisplay_index = 0;
	int vdisplay_word_size = 1;
	int vdisplay_color_depth = 1;
	Array vdisplay_palette;

	// Simulation state.
	struct VCBSim *sim = nullptr;   // drives core/vcb_sim over model->native
	int64_t current_tick = 0;
	// wall-clock stamp of the previous solve(), for the real-time TIMER ink
	uint64_t last_solve_usec = 0;
	int64_t current_event = 0;
	int64_t prev_tick = 0;
	int64_t prev_event = 0;

	// Perf: the state texture is rebuilt only when the board actually changed, and the
	// CPU staging buffer + display buffers are reused across frames instead of being
	// reallocated every frame (see transistor_engine.cpp).
	PoolVector<uint8_t> state_buf;   // reused staging for the state texture
	int last_vinput = 0;             // previous virtual-input word (change detection)
	// The VMem editor's visible window, as solve()'s p_vmem_range packs it:
	// low 32 bits = first word address, high 32 bits = word count
	// (vmem_editor.gd::update_range). get_vmem_section() returns exactly that window.
	int64_t vmem_window_start = 0;
	int64_t vmem_window_count = 0;
	PoolVector<int> vd_staging;      // reused vdisplay VMem staging
	PoolVector<int> vd_pal;          // reused vdisplay palette
	PoolVector<uint8_t> vd_px;       // reused vdisplay pixel buffer

	int vdisplay_row_count() const;

protected:
	static void _bind_methods();

public:
	void set_circuit_model(const Ref<TransistorCircuitModel> &p_model);
	void set_clock_timer_intervals(const Array &p_intervals);
	void set_random_seed(int p_seed);
	void set_vdisplay_settings(const Vector2 &p_size, int a, int b, int c, const Array &p_arr);

	// The main tick. Returns the per-tick result array (state deltas).
	// p_time_paused is real seconds spent paused since the last solve (the game
	// passes a float); the decompiled solve (RVA 0x28ea20, section 15) folds it
	// into a real-time paused-tick accumulator for the real-time TIMER ink. That
	// real-time domain is a Phase-B fidelity item, so the value is accepted with
	// the correct ABI type but not yet consumed here.
	Variant solve(int p_ticks, const Array &p_override_keys, int p_vinput, int64_t p_vmem_range,
			real_t p_time_paused);
	void compute(const Variant &p_userdata); // background worker (started via Thread.start)
	void stop();

	Ref<Texture> get_texture();
	Ref<Texture> get_vdisplay_texture();
	PoolVector<int> get_vmem_section();
	PoolVector<uint8_t> get_vmem_persistent(int p_start, int p_end);
	bool get_entity_state(int p_x, int p_y);
	bool is_entity_latch(int p_x, int p_y);

	void snapshot_take();
	void snapshot_clear_all();
	void snapshot_clear_next();
	void snapshot_restore_next();
	void snapshot_restore_prev();
	bool snapshot_is_next_possible();
	bool snapshot_is_prev_possible();

	TransistorEngine() {}
	~TransistorEngine();
};

#endif // TRANSISTOR_ENGINE_H
