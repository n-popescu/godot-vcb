#include "transistor_engine.h"

#include "core/class_db.h"
#include "core/os/os.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "core/vcb_sim.h"
#include "core/vcb_vdisplay.h"
#include "core/vcb_vmem.h"
}

// The tick loop is the recovered, unit-tested Godot-free simulator (core/vcb_sim)
// running over the model's native graph (built by TransistorCompiler). This class
// drives it, tracks tick/event telemetry, applies mouse overrides + virtual input
// (encodings taken from the game's own simulator.gd), reports fired breakpoints,
// does snapshot step back/forward, and renders the per-entity state texture the
// game samples. The VMem memory subsystem and the virtual display are the only
// remaining features; their exact layouts aren't decompiled yet and are marked
// TODO(phase-b).

TransistorEngine::~TransistorEngine() {
	if (sim) {
		vcb_sim_free(sim);
		free(sim);
		sim = nullptr;
	}
}

void TransistorEngine::set_circuit_model(const Ref<TransistorCircuitModel> &p_model) {
	model = p_model;
	if (sim) {
		vcb_sim_free(sim);
		free(sim);
		sim = nullptr;
	}
	current_tick = current_event = prev_tick = prev_event = 0;
	if (p_model.is_valid() && p_model->native) {
		sim = (VCBSim *)malloc(sizeof(VCBSim));
		vcb_sim_init(sim, p_model->native);
		vcb_sim_set_clock(sim, clock_interval);
		vcb_sim_set_timer_us(sim, timer_interval_us);
		vcb_sim_set_seed(sim, (uint32_t)random_seed);
	}
}

void TransistorEngine::set_clock_timer_intervals(const Array &p_intervals) {
	if (p_intervals.size() >= 2) {
		clock_interval = (int)p_intervals[0];
		timer_interval_us = (int)p_intervals[1] * 1000; // ms -> us (recovered)
	}
	if (sim) {
		vcb_sim_set_clock(sim, clock_interval);
		vcb_sim_set_timer_us(sim, timer_interval_us);
	}
}

void TransistorEngine::set_random_seed(int p_seed) {
	random_seed = p_seed;
	if (sim)
		vcb_sim_set_seed(sim, (uint32_t)p_seed);
}

void TransistorEngine::set_vdisplay_settings(const Vector2 &p_size, int p_index, int p_word_size,
		int p_color_depth, const Array &p_palette) {
	// Faithful port of the decompiled set_vdisplay_settings (RVA 0x0028e8d0,
	// vcb-engine-recovery/docs/vmem_vdisplay.md): record the display geometry +
	// palette. The per-tick raster + row buffers are produced by the VMem/display
	// kernel that runs in the (un-decompiled) `compute` worker, so only the config
	// is captured here.
	vdisplay_width = (int)p_size.x;
	vdisplay_height = (int)p_size.y;
	vdisplay_index = p_index;      // pointer/address
	vdisplay_word_size = p_word_size;
	vdisplay_color_depth = p_color_depth;
	vdisplay_palette = p_palette;
}

// Rebuild the per-entity state texture: a sidelength x sidelength RGBA8 image
// whose cell k holds entity k's state. Paired with the compiler's die texture
// (pixel -> entity coord) and on/off textures, this is what the renderer samples.
//
// Perf (v-perf): this runs once per rendered frame, so it must scale with the board,
// not with wall-clock:
//   * The staging buffer `buf` is reused across frames (no side*side*4 alloc/free each
//     frame — that is up to megabytes per frame on large boards).
//   * `n_high` (the A channel, the LED palette index) is only meaningful for LED cells
//     (ink 0x0c), and the shader only reads A for LEDs, so we compute it *only* for
//     LEDs. Previously it summed every entity's input states every frame — an
//     O(total connections) rescan of the whole graph each frame, which is exactly the
//     kind of per-frame cost that made big (VMem/VDisplay-heavy) projects lag.
static Ref<ImageTexture> te_build_state_texture(VCBSim *sim, Ref<ImageTexture> reuse,
		PoolVector<uint8_t> &buf) {
	VCBModel *m = sim->model;
	int side = m->sidelength > 0 ? m->sidelength : 1;
	const int need = side * side * 4;
	if (buf.size() != need)
		buf.resize(need);
	{
		PoolVector<uint8_t>::Write w = buf.write();
		uint8_t *p = w.ptr();
		memset(p, 0, (size_t)need);
		for (int32_t k = 1; k <= m->n_entities && k < side * side; k++) {
			VCBEntity *e = &m->ent[k];
			// The state texture is a copy of circuit_data {state, ink, _, n_high}. The
			// shader reads R = on/off, G = raw ink (==12 => LED), and A = the LED palette
			// index = min(n_high, 15). A is only sampled for LEDs, so only LEDs pay the
			// input scan.
			p[k * 4 + 0] = e->state ? 255 : 0; // R = on/off (shader: is_on = ceil(r))
			p[k * 4 + 1] = e->ink;             // G = raw type (shader: is_led = g*255 == 12)
			if (e->ink == 0x0c) {              // LED: A = number of high inputs
				int n_high = 0;
				for (int32_t j = 0; j < e->inputs.count; j++)
					n_high += m->ent[e->inputs.items[j]].state ? 1 : 0;
				p[k * 4 + 3] = (uint8_t)(n_high < 15 ? n_high : 15);
			}
		}
	}
	Ref<Image> img;
	img.instance();
	img->create(side, side, false, Image::FORMAT_RGBA8, buf);
	Ref<ImageTexture> tex = reuse;
	// Re-upload into the existing GPU texture (update) instead of recreating it every
	// frame; only (re)create on the first build or when the board size changed.
	if (tex.is_valid() && tex->get_width() == side && tex->get_height() == side) {
		tex->set_data(img);
	} else {
		if (tex.is_null())
			tex.instance();
		tex->create_from_image(img, 0);
	}
	return tex;
}

Variant TransistorEngine::solve(int p_ticks, const Array &p_override_keys, int p_vinput, int,
		real_t p_time_paused) {
	prev_tick = current_tick;
	prev_event = current_event;

	// The TIMER ink is real-time in the original engine, not tick-based: the kernel
	// compares wall-clock microseconds since the last fire against the interval,
	// adding the time the simulation spent paused to the threshold. Feed the core
	// the same two quantities so a TIMER keeps real-world period at any tick rate.
	{
		const uint64_t now_us = OS::get_singleton()->get_ticks_usec();
		const int64_t elapsed_us = last_solve_usec ? (int64_t)(now_us - last_solve_usec) : 0;
		last_solve_usec = now_us;
		if (sim)
			vcb_sim_add_time_us(sim, elapsed_us, (int64_t)(p_time_paused * 1000000.0));
	}

	Array breakpoints; // result[6]: entity-LUT positions of breakpoints that fired

	if (sim) {
		VCBModel *m = sim->model;
		const int side = m->sidelength > 0 ? m->sidelength : 1;
		vcb_sim_clear_fired(sim); // this frame's breakpoint fires

		// Perf (v-perf): only rebuild + re-upload the state texture when the board can
		// actually have changed this frame. Rebuilding it every frame (megabytes of
		// alloc + a full graph scan + a GPU upload) is what made large projects lag even
		// while idle. `board_changed` starts true only for the first build.
		bool board_changed = state_texture.is_null();

		// Mouse overrides: each key is simlist_x<<32 | simlist_y<<8 | state, where
		// (simlist_x, simlist_y) are the entity-LUT coordinates the game decodes
		// from texture_die (simulator.gd::set_mouse_override); entity index is
		// y*sidelength + x. Only interactive inks (latches / VMEM cells) are
		// overridable; the state is held by the simulator.
		if (p_override_keys.size() > 0)
			board_changed = true; // a user interaction touched the board
		for (int i = 0; i < p_override_keys.size(); i++) {
			int64_t key = (int64_t)p_override_keys[i];
			int32_t ex = (int32_t)(key >> 32);
			int32_t ey = (int32_t)((key >> 8) & 0xffffff);
			uint8_t st = (uint8_t)(key & 1);
			int64_t idx = (int64_t)ey * side + ex;
			if (idx > 0 && idx <= m->n_entities) {
				uint8_t ink = m->ent[idx].ink;
				if (ink == 0x09 || ink == 0x0a || ink == 0x0d || ink == 0x0e) {
					m->ent[idx].state = st;
					vcb_sim_mark_external(sim, (int32_t)idx); // propagate under the active scheduler
				}
			}
		}

		// Virtual input: drive the VINPUT entities (indices recorded by the
		// compiler in vinput_indices) from the bits of p_vinput.
		if (model.is_valid() && model->vinput_indices.size() > 0) {
			if (p_vinput != last_vinput)
				board_changed = true; // the input word changed
			PoolVector<int>::Read vi = model->vinput_indices.read();
			int n = model->vinput_indices.size();
			uint32_t v = (uint32_t)p_vinput;
			for (int i = 0; i < n && i < 32; i++) {
				int idx = vi[i];
				if (idx > 0 && idx <= m->n_entities) {
					m->ent[idx].state = (uint8_t)((v >> i) & 1u);
					vcb_sim_mark_external(sim, idx);
				}
			}
		}
		last_vinput = p_vinput;

		if (p_ticks > 0) {
			int64_t e0 = sim->events;
			vcb_sim_run(sim, p_ticks);
			current_tick += p_ticks;
			current_event += sim->events - e0;
			board_changed = true; // any tick can change state (incl. CLOCK/TIMER/VMem)
		}
		if (board_changed) {
			state_texture = te_build_state_texture(sim, state_texture, state_buf);
		}

		// Report breakpoints that fired this frame as their entity-LUT positions
		// (the same simlist coordinates the game uses elsewhere: x = idx %
		// sidelength, y = idx / sidelength).
		for (int32_t i = 0; i < sim->fired_breakpoints.count; i++) {
			int32_t idx = sim->fired_breakpoints.items[i];
			breakpoints.push_back(Vector2(idx % side, idx / side));
		}
	}

	// TE_RESULT (simulator.gd): [tick, tpf, event, epf, vmem_addr, vmem_ready,
	// breakpoints, vmem_occ_addr, vmem_occ_content].
	Array res;
	res.resize(9);
	res[0] = current_tick;
	res[1] = current_tick - prev_tick;
	res[2] = current_event;
	res[3] = current_event - prev_event;
	res[4] = sim ? sim->vmem_address : 0; // VMEM_ADDRESS (kernel VMem sweep)
	res[5] = true;  // vmem ready
	res[6] = breakpoints; // breakpoint positions that fired this frame
	res[7] = Array(); // vmem occ address (TODO(phase-b): VMem subsystem)
	res[8] = Array(); // vmem occ content (TODO(phase-b): VMem subsystem)
	return res;
}

void TransistorEngine::stop() {}

// Background worker the game starts with Thread.start(TE, "compute", null)
// (simulator.gd, right after set_circuit_model). In the original engine this is
// the continuous simulation kernel run off the main thread; in this
// reconstruction the tick advance is driven synchronously by solve() each physics
// frame, so this is a no-op that simply lets Thread.start()/wait_to_finish()
// succeed. It must accept the userdata argument Thread.start always passes. If a
// threaded kernel is added later, run its loop here.
void TransistorEngine::compute(const Variant &) {}

Ref<Texture> TransistorEngine::get_texture() { return state_texture; }

// Number of VMem words the display reads per frame:
// row_count = (w*h) / (word_size/color_depth) + 1 (set_vdisplay_settings, 0x28e8d0).
int TransistorEngine::vdisplay_row_count() const {
	int ppw = vdisplay_color_depth > 0 ? vdisplay_word_size / vdisplay_color_depth : 1;
	if (ppw < 1)
		ppw = 1;
	return (vdisplay_width * vdisplay_height) / ppw + 1;
}

// The virtual display raster. get_vdisplay_texture (RVA 0x28fd80) repacks the
// per-tick display slice ("staging") into RGBA8 -- faithfully reconstructed in
// vcb_vdisplay_repack. The staging is `row_count` VMem words read from the display
// base address (vdisplay_index) of the model's VMem image (circuit_state_arr, i.e.
// model +0x158), matching solve()'s memcpy. The VMem image is initialised by the
// compiler's compute_vmem_data; runtime VMem writes (the memory kernel) are still
// Phase-B, so the display currently shows the loaded VMem image.
Ref<Texture> TransistorEngine::get_vdisplay_texture() {
	if (vdisplay_width <= 0 || vdisplay_height <= 0)
		return Ref<Texture>();
	const int w = vdisplay_width, h = vdisplay_height;
	const int row_count = vdisplay_row_count();

	// Staging: row_count words from the model's VMem starting at vdisplay_index.
	// Perf (v-perf): reuse the staging/palette/pixel buffers across frames.
	PoolVector<int> &staging = vd_staging;
	if (staging.size() != row_count)
		staging.resize(row_count);
	{
		PoolVector<int>::Write sw = staging.write();
		int32_t *sp = sw.ptr();
		// The display reads the LIVE VMem the simulator maintains (runtime writes),
		// falling back to the model's initial image before the sim exists.
		const int32_t *vmem = sim && sim->vmem ? sim->vmem
				: ((model.is_valid() && model->native) ? model->native->vmem : nullptr);
		const int vlen = sim && sim->vmem ? sim->vmem_len
				: ((model.is_valid() && model->native) ? model->native->vmem_len : 0);
		for (int i = 0; i < row_count; i++) {
			int idx = vdisplay_index + i;
			sp[i] = (vmem && idx >= 0 && idx < vlen) ? vmem[idx] : 0;
		}
	}

	// Palette (stored int colours) -> contiguous int32 buffer for the repack.
	PoolVector<int> &pal = vd_pal;
	const int pal_len = vdisplay_palette.size();
	if (pal.size() != (pal_len > 0 ? pal_len : 1))
		pal.resize(pal_len > 0 ? pal_len : 1);
	{
		PoolVector<int>::Write pw = pal.write();
		for (int i = 0; i < pal_len; i++)
			pw.ptr()[i] = (int)vdisplay_palette[i];
	}

	PoolVector<uint8_t> &px = vd_px;
	if (px.size() != w * h * 4)
		px.resize(w * h * 4);
	{
		PoolVector<uint8_t>::Write pxw = px.write();
		memset(pxw.ptr(), 0, (size_t)w * h * 4);
		PoolVector<int>::Read sr = staging.read();
		PoolVector<int>::Read pr = pal.read();
		vcb_vdisplay_repack(sr.ptr(), row_count, w, h, vdisplay_word_size, vdisplay_color_depth,
				pr.ptr(), pal_len, pxw.ptr());
	}
	Ref<Image> img;
	img.instance();
	img->create(w, h, false, Image::FORMAT_RGBA8, px);
	Ref<ImageTexture> tex;
	tex.instance();
	tex->create_from_image(img, 0);
	return tex;
}

// get_vmem_section (0x28ffd0): the VMem section shown in the VMem editor -- the
// model's VMem words as a PoolIntArray. get_vmem_persistent (0x2900a0): the
// big-endian byte repack of a VMem word range (vcb_vmem_persistent). Both read the
// model's VMem image (built by compute_vmem_data); runtime updates are Phase-B.
PoolVector<int> TransistorEngine::get_vmem_section() {
	PoolVector<int> out;
	// Prefer the simulator's live VMem (runtime writes); fall back to the model's
	// initial image.
	const int32_t *vmem = sim && sim->vmem ? sim->vmem
			: ((model.is_valid() && model->native) ? model->native->vmem : nullptr);
	const int n = sim && sim->vmem ? sim->vmem_len
			: ((model.is_valid() && model->native) ? model->native->vmem_len : 0);
	if (!vmem)
		return out;
	out.resize(n);
	PoolVector<int>::Write w = out.write();
	for (int i = 0; i < n; i++)
		w.ptr()[i] = vmem[i];
	return out;
}
PoolVector<uint8_t> TransistorEngine::get_vmem_persistent(int p_start, int p_end) {
	PoolVector<uint8_t> out;
	if (p_end <= p_start)
		return out;
	const int32_t *vmem = sim && sim->vmem ? sim->vmem
			: ((model.is_valid() && model->native) ? model->native->vmem : nullptr);
	const int n = sim && sim->vmem ? sim->vmem_len
			: ((model.is_valid() && model->native) ? model->native->vmem_len : 0);
	out.resize((p_end - p_start) * 4);
	PoolVector<uint8_t>::Write w = out.write();
	vcb_vmem_persistent(vmem, n, p_start, p_end, w.ptr());
	return out;
}

// get_entity_state / is_entity_latch take ENTITY-LUT coordinates (decoded from
// texture_die by the caller, simulator.gd), not raw board pixels: the entity
// index is y * sidelength + x.
bool TransistorEngine::get_entity_state(int p_x, int p_y) {
	if (!sim)
		return false;
	VCBModel *m = sim->model;
	int64_t idx = (int64_t)p_y * (m->sidelength > 0 ? m->sidelength : 1) + p_x;
	if (idx <= 0 || idx > m->n_entities)
		return false;
	return m->ent[idx].state != 0;
}

bool TransistorEngine::is_entity_latch(int p_x, int p_y) {
	if (!sim)
		return false;
	VCBModel *m = sim->model;
	int64_t idx = (int64_t)p_y * (m->sidelength > 0 ? m->sidelength : 1) + p_x;
	if (idx <= 0 || idx > m->n_entities)
		return false;
	uint8_t ink = m->ent[idx].ink;
	// The binary's is_entity_latch (RVA 0x290270) returns true for ink in
	// {0x09, 0x0a, 0x0d, 0x0e} -- LATCH_ON/OFF and the VMEM address/content cells
	// (all the click-interactive inks). Verified by disassembly.
	return ink == 0x09 || ink == 0x0a || ink == 0x0d || ink == 0x0e;
}

// Snapshot history (step back/forward) delegates to the simulator's state stack.
// Restores rebuild the state texture so the display reflects the stepped-to state.
void TransistorEngine::snapshot_take() {
	if (sim)
		vcb_sim_snapshot_take(sim);
}
void TransistorEngine::snapshot_clear_all() {
	if (sim)
		vcb_sim_snapshot_clear_all(sim);
}
void TransistorEngine::snapshot_clear_next() {
	if (sim)
		vcb_sim_snapshot_clear_next(sim);
}
void TransistorEngine::snapshot_restore_next() {
	if (sim) {
		vcb_sim_snapshot_restore_next(sim);
		state_texture = te_build_state_texture(sim, state_texture, state_buf);
	}
}
void TransistorEngine::snapshot_restore_prev() {
	if (sim) {
		vcb_sim_snapshot_restore_prev(sim);
		state_texture = te_build_state_texture(sim, state_texture, state_buf);
	}
}
bool TransistorEngine::snapshot_is_next_possible() { return sim && vcb_sim_snapshot_next_possible(sim); }
bool TransistorEngine::snapshot_is_prev_possible() { return sim && vcb_sim_snapshot_prev_possible(sim); }

void TransistorEngine::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_circuit_model", "model"), &TransistorEngine::set_circuit_model);
	ClassDB::bind_method(D_METHOD("set_clock_timer_intervals", "intervals"),
			&TransistorEngine::set_clock_timer_intervals);
	ClassDB::bind_method(D_METHOD("set_random_seed", "seed"), &TransistorEngine::set_random_seed);
	ClassDB::bind_method(D_METHOD("set_vdisplay_settings", "size", "a", "b", "c", "arr"),
			&TransistorEngine::set_vdisplay_settings);
	ClassDB::bind_method(
			D_METHOD("solve", "ticks", "override_keys", "vinput", "vmem_range", "time_paused"),
			&TransistorEngine::solve);
	// Same as TransistorCompiler::compute -- callable bare or via Thread.start().
	ClassDB::bind_method(D_METHOD("compute", "userdata"), &TransistorEngine::compute,
			DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("stop"), &TransistorEngine::stop);
	ClassDB::bind_method(D_METHOD("get_texture"), &TransistorEngine::get_texture);
	ClassDB::bind_method(D_METHOD("get_vdisplay_texture"), &TransistorEngine::get_vdisplay_texture);
	ClassDB::bind_method(D_METHOD("get_vmem_section"), &TransistorEngine::get_vmem_section);
	ClassDB::bind_method(D_METHOD("get_vmem_persistent", "start", "end"),
			&TransistorEngine::get_vmem_persistent);
	ClassDB::bind_method(D_METHOD("get_entity_state", "x", "y"), &TransistorEngine::get_entity_state);
	ClassDB::bind_method(D_METHOD("is_entity_latch", "x", "y"), &TransistorEngine::is_entity_latch);
	ClassDB::bind_method(D_METHOD("snapshot_take"), &TransistorEngine::snapshot_take);
	ClassDB::bind_method(D_METHOD("snapshot_clear_all"), &TransistorEngine::snapshot_clear_all);
	ClassDB::bind_method(D_METHOD("snapshot_clear_next"), &TransistorEngine::snapshot_clear_next);
	ClassDB::bind_method(D_METHOD("snapshot_restore_next"), &TransistorEngine::snapshot_restore_next);
	ClassDB::bind_method(D_METHOD("snapshot_restore_prev"), &TransistorEngine::snapshot_restore_prev);
	ClassDB::bind_method(D_METHOD("snapshot_is_next_possible"),
			&TransistorEngine::snapshot_is_next_possible);
	ClassDB::bind_method(D_METHOD("snapshot_is_prev_possible"),
			&TransistorEngine::snapshot_is_prev_possible);
}
