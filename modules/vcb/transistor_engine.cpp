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

static Ref<ImageTexture> te_build_state_texture(VCBSim *sim, Ref<ImageTexture> reuse,
		PoolVector<uint8_t> &buf);

void TransistorEngine::set_circuit_model(const Ref<TransistorCircuitModel> &p_model) {
	MutexLock sim_lock(sim_mutex);
	model = p_model;
	if (sim) {
		vcb_sim_free(sim);
		free(sim);
		sim = nullptr;
	}
	current_tick = current_event = prev_tick = prev_event = 0;
	state_texture = Ref<ImageTexture>();
	if (p_model.is_valid() && p_model->native) {
		sim = (VCBSim *)malloc(sizeof(VCBSim));
		vcb_sim_init(sim, p_model->native);
		vcb_sim_set_clock(sim, clock_interval);
		vcb_sim_set_timer_us(sim, timer_interval_us);
		vcb_sim_set_seed(sim, (uint32_t)random_seed);
		// In the original engine get_texture() is already valid here -- the worker
		// publishes the compiled circuit_data before the first solve() -- so build the
		// tick-0 texture now instead of leaving it null until something solves.
		state_texture = te_build_state_texture(sim, state_texture, state_buf);
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
// whose cell k holds entity k's circuit_data. Paired with the compiler's die
// texture (pixel -> entity coord) and the on/off textures, this is what the
// renderer samples.
//
// The image is a copy of circuit_data, cell k = {state, ink, n_conn, n_high} --
// verified byte for byte against the original engine's get_texture (0x28fca0) on
// the probe boards. All four channels are RAW: R is 0 or **1**, not 0/255 (the
// shader does `is_on = ceil(entity_data.r)`, so 255 renders identically -- which is
// why writing 255 looked right and still made every lit pixel differ from the
// original's texture byte for byte), G is the ink (0xff for a net), B is the
// in-degree, and A is the raw n_high the shader clamps itself
// (`round(min(entity_data.a * 255.0, 15.0))` for the LED palette index).
//
// Perf (v-perf): this runs once per rendered frame, so it must scale with the board,
// not with wall-clock. The staging buffer `buf` is reused across frames (no
// side*side*4 alloc/free each frame -- that is up to megabytes per frame on large
// boards), and the fill is one linear pass over the entities: every value it needs
// is already maintained by the kernel, so there is no per-frame rescan of the graph.
// CPU half of the state texture: fills `buf` with the {state, ink, n_conn, n_high}
// cell for every entity. Runs on the compute worker, so it must not touch any GPU
// resource -- te_upload_state_texture() does that on the main thread.
static void te_fill_state_bytes(VCBSim *sim, PoolVector<uint8_t> &buf) {
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
			// A bus/mesh-merged net's tallies live on the group's representative (the
			// original engine merges those nets into a single entity at compile time),
			// so every trace in the group reports the group's counts.
			const int32_t r = (e->is_trace && m->net_rep) ? m->net_rep[k] : k;
			p[k * 4 + 0] = e->state ? 1 : 0; // state, raw 0/1
			p[k * 4 + 1] = e->ink;           // ink (0xff for a net)
			p[k * 4 + 2] = sim->n_in[r];     // n_conn (in-degree)
			p[k * 4 + 3] = sim->n_high[r];   // n_high accumulator, unclamped
		}
	}
}

// GPU half: upload `buf` into the reusable ImageTexture. Main thread only.
static Ref<ImageTexture> te_upload_state_texture(int side, const PoolVector<uint8_t> &buf,
		Ref<ImageTexture> reuse) {
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

// ---------------------------------------------------------------------------
// The asynchronous solve()/compute() pair.
//
// The original engine does NOT tick inside solve(). solve() hands the compute
// worker a tick budget and returns the counters as they stood BEFORE the call;
// get_texture() publishes the worker's previous completed frame. Two behaviours
// the game depends on follow from that, and both are reproduced here:
//
//   * solve() OVERWRITES the pending budget instead of adding to it, so a frame
//     the worker could not finish is simply dropped. That is what makes a heavy
//     board settle at a lower tick rate instead of stalling the main thread --
//     measured on the shipped engine, which delivers ~16k of a requested 200k
//     ticks per call and varies run to run.
//   * solve(0) therefore CANCELS whatever was pending (tools/phasec's harness
//     documents this: polling with solve(0) deadlocks the original).
//
// The simulator is touched only by the worker. solve() publishes a request and
// returns; worker_apply() applies the overrides / virtual input and runs the
// ticks. If no worker was ever started (Thread.start(TE, "compute", null)), the
// request is applied inline so a standalone caller still works exactly as before.
// ---------------------------------------------------------------------------

// Apply a new request: the VMem window, the wall-clock the real-time TIMER needs,
// the mouse overrides and the virtual input. Runs on the worker, under sim_mutex.
void TransistorEngine::worker_prepare(const Request &r) {

	MutexLock sim_lock(sim_mutex);

	// The VMem editor's visible window, packed as address | (count << 32)
	// (vmem_editor.gd::update_range); get_vmem_section() reports that slice.
	vmem_window_start = r.vmem_range & 0xffffffff;
	vmem_window_count = (r.vmem_range >> 32) & 0xffffffff;

	// The TIMER ink is real-time in the original engine, not tick-based: the kernel
	// compares wall-clock microseconds since the last fire against the interval,
	// adding the time the simulation spent paused to the threshold. Feed the core
	// the same two quantities so a TIMER keeps real-world period at any tick rate.
	{
		const uint64_t now_us = OS::get_singleton()->get_ticks_usec();
		const int64_t elapsed_us = last_solve_usec ? (int64_t)(now_us - last_solve_usec) : 0;
		last_solve_usec = now_us;
		if (sim)
			vcb_sim_add_time_us(sim, elapsed_us, (int64_t)(r.time_paused * 1000000.0));
	}

	Array breakpoints; // result[6]: entity-LUT positions of breakpoints that fired
	bool state_dirty = false;

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
		if (r.overrides.size() > 0)
			board_changed = true; // a user interaction touched the board
		for (int i = 0; i < r.overrides.size(); i++) {
			int64_t key = (int64_t)r.overrides[i];
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
		// compiler in vinput_indices) from the bits of r.vinput.
		if (model.is_valid() && model->vinput_indices.size() > 0) {
			if (r.vinput != last_vinput)
				board_changed = true; // the input word changed
			PoolVector<int>::Read vi = model->vinput_indices.read();
			int n = model->vinput_indices.size();
			uint32_t v = (uint32_t)r.vinput;
			for (int i = 0; i < n && i < 32; i++) {
				int idx = vi[i];
				if (idx > 0 && idx <= m->n_entities) {
					m->ent[idx].state = (uint8_t)((v >> i) & 1u);
					vcb_sim_mark_external(sim, idx);
				}
			}
		}
		last_vinput = r.vinput;

	}
}

// Advance the simulation by at most `n` ticks. Split out so the worker can run a
// long budget in chunks and publish between them: the original's worker reports
// progress continuously (it delivers roughly 16k ticks per 5 ms on a mid-size
// board and a later solve() overwrites whatever is LEFT), so running a whole
// 200k-tick budget atomically before publishing would report no progress at all.
void TransistorEngine::worker_run(int64_t n) {
	MutexLock sim_lock(sim_mutex);
	if (!sim || n <= 0)
		return;
	int64_t e0 = sim->events;
	vcb_sim_run(sim, (int)n);
	current_tick += n;
	current_event += sim->events - e0;
	te_fill_state_bytes(sim, state_buf);
}

// Publish the frame the main thread reads: counters, VMem telemetry, breakpoints
// and the state-texture bytes.
void TransistorEngine::worker_publish() {
	Array breakpoints;
	{
		MutexLock sim_lock(sim_mutex);
		if (sim && sim->model) {
			const int side = sim->model->sidelength > 0 ? sim->model->sidelength : 1;
			for (int32_t i = 0; i < sim->fired_breakpoints.count; i++) {
				int32_t idx = sim->fired_breakpoints.items[i];
				breakpoints.push_back(Vector2(idx % side, idx / side));
			}
		}
	}
	bool state_dirty = true;
	// Publish this frame for the main thread: counters, the VMem telemetry, and the
	// state-texture bytes. get_texture() uploads `pub_state`, which is why it shows
	// the worker's PREVIOUS frame whenever the worker has not finished this one.
	{
		MutexLock lock(req_mutex);
		pub_prev_tick = pub_tick;
		pub_prev_event = pub_event;
		pub_tick = current_tick;
		pub_event = current_event;
		pub_vmem_address = sim ? sim->vmem_address : 0;
		pub_vmem_ready = sim ? (sim->vmem_lock == 0) : true;
		pub_breakpoints = breakpoints;
		pub_occ_addr.clear();
		pub_occ_content.clear();
		if (sim) {
			for (int32_t i = 0; i < sim->vmem_occ_addr.count; i++)
				pub_occ_addr.push_back(sim->vmem_occ_addr.items[i]);
			for (int32_t i = 0; i < sim->vmem_occ_content.count; i++)
				pub_occ_content.push_back(sim->vmem_occ_content.items[i]);
		}
		if (state_dirty) {
			// the frame we published last time becomes the one get_texture() serves
			if (pub_state_dirty) {
				pub_state_prev = pub_state;
				pub_prev_valid = true;
			}
			pub_state = state_buf;
			pub_side = sim && sim->model && sim->model->sidelength > 0
					? sim->model->sidelength : 1;
			pub_state_dirty = true;
		}
	}
}

// Main-thread convenience: fill + upload in one go. Used by the paths that touch
// the simulator directly (set_circuit_model, snapshot restore), which hold
// sim_mutex so they cannot run while the worker is mid-batch.
static Ref<ImageTexture> te_build_state_texture(VCBSim *sim, Ref<ImageTexture> reuse,
		PoolVector<uint8_t> &buf) {
	te_fill_state_bytes(sim, buf);
	const int side = (sim && sim->model && sim->model->sidelength > 0)
			? sim->model->sidelength : 1;
	return te_upload_state_texture(side, buf, reuse);
}

Variant TransistorEngine::solve(int p_ticks, const Array &p_override_keys, int p_vinput,
		int64_t p_vmem_range, real_t p_time_paused) {
	Request r;
	r.ticks = p_ticks;
	r.vinput = p_vinput;
	r.vmem_range = p_vmem_range;
	r.time_paused = (double)p_time_paused;
	r.overrides.resize(p_override_keys.size());
	for (int i = 0; i < p_override_keys.size(); i++)
		r.overrides.write[i] = (int64_t)p_override_keys[i];

	// The result must be the counters as they stood BEFORE this call. Snapshot them
	// in the SAME critical section that hands over the request -- reading them after
	// posting the semaphore is a race the worker can win, which reports this frame's
	// numbers a frame early.
	int64_t s_tick, s_event, s_ptick, s_pevent;
	int32_t s_addr;
	bool s_ready, threaded;
	Array s_bp, s_occa, s_occc;
	{
		MutexLock lock(req_mutex);
		threaded = worker_running;
		if (threaded) {
			req = r;          // OVERWRITE: an unfinished budget is dropped, not queued
			req.pending = true;
			s_tick = pub_tick;
			s_event = pub_event;
			s_ptick = pub_prev_tick;
			s_pevent = pub_prev_event;
			s_addr = pub_vmem_address;
			s_ready = pub_vmem_ready;
			s_bp = pub_breakpoints;
			s_occa = pub_occ_addr;
			s_occc = pub_occ_content;
		}
	}
	if (threaded) {
		req_sem.post();
	} else {
		// No worker is running yet -- Thread.start() has returned but the OS has not
		// scheduled compute() (or nobody started one at all). Run inline, but still
		// report the counters from BEFORE the call, so solve()'s contract does not
		// depend on thread scheduling. Reporting post-call numbers here is what made
		// SOLVE0 differ between runs.
		{
			MutexLock lock(req_mutex);
			s_tick = pub_tick;
			s_event = pub_event;
			s_ptick = pub_prev_tick;
			s_pevent = pub_prev_event;
			s_addr = pub_vmem_address;
			s_ready = pub_vmem_ready;
			s_bp = pub_breakpoints;
			s_occa = pub_occ_addr;
			s_occc = pub_occ_content;
		}
		prev_tick = current_tick;
		prev_event = current_event;
		worker_prepare(r);
		worker_run(r.ticks);
		worker_publish();
	}

	// TE_RESULT (simulator.gd): [tick, tpf, event, epf, vmem_addr, vmem_ready,
	// breakpoints, vmem_occ_addr, vmem_occ_content].
	Array res;
	res.resize(9);
	res[0] = s_tick;
	res[1] = s_tick - s_ptick;
	res[2] = s_event;
	res[3] = s_event - s_pevent;
	res[4] = s_addr;
	res[5] = s_ready;
	res[6] = s_bp;
	res[7] = s_occa;
	res[8] = s_occc;
	return res;
}

void TransistorEngine::stop() {
	// simulator.gd calls stop() then thread.wait_to_finish(). Wake the worker with
	// the exit flag set so its loop returns instead of blocking on the semaphore.
	{
		MutexLock lock(req_mutex);
		if (!worker_running)
			return;
		worker_exit = true;
	}
	req_sem.post();
}

// The background worker the game starts with Thread.start(TE, "compute", null)
// (simulator.gd, right after set_circuit_model). This is the original's continuous
// simulation kernel: it blocks until solve() hands it a tick budget, runs it, and
// publishes the frame. It must accept the userdata argument Thread.start always
// passes, and it returns when stop() sets the exit flag.
void TransistorEngine::compute(const Variant &) {
	{
		MutexLock lock(req_mutex);
		if (worker_running)
			return;           // only one worker
		worker_running = true;
		worker_exit = false;
	}
	// How many ticks to run between publishes. The original's worker reports
	// progress continuously rather than only at the end of a budget, so a long
	// budget must be visible as it is consumed; this is the granularity of that.
	const int64_t CHUNK = 1024;
	int64_t remaining = 0;
	for (;;) {
		if (remaining <= 0)
			req_sem.wait();   // nothing to do: block until a request lands
		Request r;
		bool have_new = false;
		{
			MutexLock lock(req_mutex);
			if (worker_exit)
				break;
			if (req.ticks != 0 || req.pending) {
				r = req;
				req.ticks = 0;
				req.pending = false;
				have_new = true;
			}
		}
		if (have_new) {
			// A new request OVERWRITES whatever is left of the previous budget --
			// the frame the worker could not finish is dropped, not queued.
			worker_prepare(r);
			remaining = r.ticks;
			if (remaining <= 0)
				worker_publish(); // a solve(0) still publishes a frame
		}
		if (remaining > 0) {
			const int64_t n = remaining < CHUNK ? remaining : CHUNK;
			worker_run(n);
			remaining -= n;
			worker_publish();
		}
	}
	MutexLock lock(req_mutex);
	worker_running = false;
	worker_exit = false;
}

Ref<Texture> TransistorEngine::get_texture() {
	// Publishes the worker's last COMPLETED frame -- if the worker is still chewing
	// through this frame's budget, that is the previous one, which is exactly the
	// one-frame lag the original has (tools/phasec comparators call it `shift`).
	PoolVector<uint8_t> buf;
	int side = 0;
	{
		MutexLock lock(req_mutex);
		if (pub_prev_valid) {
			buf = pub_state_prev;
			side = pub_side;
			pub_prev_valid = false;
		}
	}
	if (side > 0)
		state_texture = te_upload_state_texture(side, buf, state_texture);
	return state_texture;
}

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

// get_vmem_section (0x28ffd0): the slice of VMem the VMem editor is showing -- the
// window solve() was last given in p_vmem_range (address | count << 32), NOT the
// whole image: vmem_editor.gd indexes the returned array from 0 for the line at
// `address_top`, so returning everything would show the wrong words on any scroll
// position but the top. get_vmem_persistent (0x2900a0) is the big-endian byte repack
// of an explicit word range (vcb_vmem_persistent).
PoolVector<int> TransistorEngine::get_vmem_section() {
	PoolVector<int> out;
	// The simulator's live VMem (runtime writes); the model's initial image before
	// the sim exists.
	const int32_t *vmem = sim && sim->vmem ? sim->vmem
			: ((model.is_valid() && model->native) ? model->native->vmem : nullptr);
	const int64_t n = sim && sim->vmem ? sim->vmem_len
			: ((model.is_valid() && model->native) ? model->native->vmem_len : 0);
	if (!vmem || vmem_window_count <= 0)
		return out;
	int64_t start = vmem_window_start < 0 ? 0 : vmem_window_start;
	int64_t count = vmem_window_count;
	if (start > n)
		start = n;
	if (start + count > n)
		count = n - start;
	if (count <= 0)
		return out;
	out.resize((int)count);
	PoolVector<int>::Write w = out.write();
	for (int64_t i = 0; i < count; i++)
		w.ptr()[i] = vmem[start + i];
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
	MutexLock sim_lock(sim_mutex);
	if (sim)
		vcb_sim_snapshot_take(sim);
}
void TransistorEngine::snapshot_clear_all() {
	MutexLock sim_lock(sim_mutex);
	if (sim)
		vcb_sim_snapshot_clear_all(sim);
}
void TransistorEngine::snapshot_clear_next() {
	MutexLock sim_lock(sim_mutex);
	if (sim)
		vcb_sim_snapshot_clear_next(sim);
}
void TransistorEngine::snapshot_restore_next() {
	MutexLock sim_lock(sim_mutex);
	if (sim) {
		vcb_sim_snapshot_restore_next(sim);
		state_texture = te_build_state_texture(sim, state_texture, state_buf);
	}
}
void TransistorEngine::snapshot_restore_prev() {
	MutexLock sim_lock(sim_mutex);
	if (sim) {
		vcb_sim_snapshot_restore_prev(sim);
		state_texture = te_build_state_texture(sim, state_texture, state_buf);
	}
}

void TransistorEngine::snapshot_restore_most_recent() {
	MutexLock sim_lock(sim_mutex);
	// Walk forward to the newest snapshot. restore_next is the single-step form;
	// the original exposes this as the "jump to the end of the history" shortcut.
	if (!sim)
		return;
	while (vcb_sim_snapshot_next_possible(sim))
		vcb_sim_snapshot_restore_next(sim);
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
	ClassDB::bind_method(D_METHOD("snapshot_restore_most_recent"),
			&TransistorEngine::snapshot_restore_most_recent);

	// The original registers these five as class constants (verified by dumping the
	// shipped engine's ClassDB). They describe the state texture's RGBA channels:
	// R = state, G = ink/type, B = total in-degree, A = active (high) in-degree,
	// 4 bytes per entity -- which is exactly the cell te_build_state_texture writes.
	BIND_CONSTANT(BYTES_WIDTH);
	BIND_CONSTANT(OFFSET_STATE);
	BIND_CONSTANT(OFFSET_TYPE);
	BIND_CONSTANT(OFFSET_IN_TOTAL);
	BIND_CONSTANT(OFFSET_IN_ACTIVE);
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
