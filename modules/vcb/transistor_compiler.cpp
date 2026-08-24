#include "transistor_compiler.h"

#include "core/class_db.h"
#include "scene/resources/texture.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "core/vcb_model.h"
#include "core/vcb_pipeline.h"
#include "core/vcb_resolver.h"
#include "core/vcb_vec.h"
#include "core/vcb_vmem.h"
#include "core/vcb_bus.h"
}

// Build a blank (zero-filled) RGBA8 Image of the given size. Used as a documented
// placeholder for the textures that depend on the entity LUT (see get_textures).
static Ref<Image> tc_blank_rgba8(int p_w, int p_h) {
	if (p_w < 1) p_w = 1;
	if (p_h < 1) p_h = 1;
	PoolVector<uint8_t> data;
	data.resize(p_w * p_h * 4);
	{
		PoolVector<uint8_t>::Write w = data.write();
		memset(w.ptr(), 0, (size_t)p_w * p_h * 4);
	}
	Ref<Image> img;
	img.instance();
	img->create(p_w, p_h, false, Image::FORMAT_RGBA8, data);
	return img;
}

// get_textures() returns TEXTURES, not Images: the game binds them to the shader's
// sampler2D uniforms and calls texture.get_data() (Texture->Image) on texture_die
// (circuit_renderer.gd). Wrap each built Image in an ImageTexture (flags 0: no
// filter/mipmap/repeat, so the die/bus/coord data textures sample exactly).
static Ref<Texture> tc_to_texture(const Ref<Image> &p_img) {
	Ref<ImageTexture> t;
	t.instance();
	t->create_from_image(p_img, 0);
	return t;
}

// Build the die (entity-LUT) texture: a board-sized RGBA8 image where each pixel
// encodes the entity index of that board pixel as a coordinate in the
// sidelength x sidelength entity grid -- r+g*256 = idx % sidelength,
// b+a*256 = idx / sidelength -- exactly the decode compiler.gd::get_entity_id
// performs (entity = y*sidelength + x). Pixels with no entity encode index 0.
// Bus pixels (busindex[f] >= 0) instead encode the sentinel (65535, 65535) that
// the shader tests for `is_bus` (engine-recovery/docs/bus_rendering.md).
static Ref<Image> tc_die_texture(const VCBModel *nm, int p_board_side, int p_sidelen,
		const int32_t *busindex) {
	if (p_board_side < 1) p_board_side = 1;
	if (p_sidelen < 1) p_sidelen = 1;
	PoolVector<uint8_t> data;
	data.resize(p_board_side * p_board_side * 4);
	{
		PoolVector<uint8_t>::Write w = data.write();
		uint8_t *p = w.ptr();
		memset(p, 0, (size_t)p_board_side * p_board_side * 4);
		if (nm && nm->lut) {
			for (int f = 0; f < p_board_side * p_board_side; f++) {
				if (busindex && busindex[f] >= 0) {
					p[f * 4 + 0] = p[f * 4 + 1] = p[f * 4 + 2] = p[f * 4 + 3] = 0xff; // (65535,65535)
					continue;
				}
				int32_t idx = nm->lut[f];
				int32_t ix = idx % p_sidelen;
				int32_t iy = idx / p_sidelen;
				p[f * 4 + 0] = ix & 0xff;
				p[f * 4 + 1] = (ix >> 8) & 0xff;
				p[f * 4 + 2] = iy & 0xff;
				p[f * 4 + 3] = (iy >> 8) & 0xff;
			}
		}
	}
	Ref<Image> img;
	img.instance();
	img->create(p_board_side, p_board_side, false, Image::FORMAT_RGBA8, data);
	return img;
}

// Build the inverse entity-LUT texture: a sidelength x sidelength RGBA8 image
// where cell (idx % sidelength, idx / sidelength) encodes a representative board
// position of entity idx (r+g*256 = x, b+a*256 = y). It is the inverse of the die
// texture; the game reads it to turn a breakpoint's entity coordinate back into a
// board position (simulator.gd::push_breakpoints_to_eventlog).
static Ref<Image> tc_inverse_entitylut_texture(const VCBModel *nm, int p_sidelen) {
	if (p_sidelen < 1) p_sidelen = 1;
	PoolVector<uint8_t> data;
	data.resize(p_sidelen * p_sidelen * 4);
	PoolVector<uint8_t>::Write w = data.write();
	uint8_t *p = w.ptr();
	memset(p, 0, (size_t)p_sidelen * p_sidelen * 4);
	if (nm && nm->lut) {
		int32_t *ix = (int32_t *)malloc((size_t)(nm->n_entities + 1) * sizeof(int32_t));
		int32_t *iy = (int32_t *)malloc((size_t)(nm->n_entities + 1) * sizeof(int32_t));
		vcb_model_build_inverse(nm, ix, iy);
		for (int32_t e = 1; e <= nm->n_entities && e < p_sidelen * p_sidelen; e++) {
			if (ix[e] < 0)
				continue;
			int32_t bx = ix[e], by = iy[e];
			p[e * 4 + 0] = bx & 0xff;
			p[e * 4 + 1] = (bx >> 8) & 0xff;
			p[e * 4 + 2] = by & 0xff;
			p[e * 4 + 3] = (by >> 8) & 0xff;
		}
		free(ix);
		free(iy);
	}
	Ref<Image> img;
	img.instance();
	img->create(p_sidelen, p_sidelen, false, Image::FORMAT_RGBA8, data);
	return img;
}

// Smallest square texture side that holds `len` linear entries.
static int tc_busside(int len) {
	int s = 1;
	while (s * s < len)
		s++;
	return s;
}

// texture_busentities: bs x bs RGBA8; linear cell k holds VMem/bus entity
// bus->entities[k] as its entity-LUT coord (r+g*256 = e%sidelen, b+a*256 =
// e/sidelen); a 0 entry (entity 0) is the (0,0) net terminator the shader stops on.
static Ref<Image> tc_busentities_texture(const VCBBusData *bus, int p_sidelen, int p_bs) {
	if (p_sidelen < 1) p_sidelen = 1;
	if (p_bs < 1) p_bs = 1;
	PoolVector<uint8_t> data;
	data.resize(p_bs * p_bs * 4);
	PoolVector<uint8_t>::Write w = data.write();
	uint8_t *p = w.ptr();
	memset(p, 0, (size_t)p_bs * p_bs * 4);
	for (int k = 0; k < bus->entities_len && k < p_bs * p_bs; k++) {
		int e = bus->entities[k];
		int ex = e % p_sidelen, ey = e / p_sidelen;
		p[k * 4 + 0] = ex & 0xff;
		p[k * 4 + 1] = (ex >> 8) & 0xff;
		p[k * 4 + 2] = ey & 0xff;
		p[k * 4 + 3] = (ey >> 8) & 0xff;
	}
	Ref<Image> img;
	img.instance();
	img->create(p_bs, p_bs, false, Image::FORMAT_RGBA8, data);
	return img;
}

// texture_buslut: board side x side RGBA8; each bus pixel encodes its net's start
// index as buscoords (busindex % bs, busindex / bs) so the shader recovers
// busindex = y*bs + x. Non-bus pixels are (0,0).
static Ref<Image> tc_buslut_texture(const VCBBusData *bus, int p_board_side, int p_bs) {
	if (p_board_side < 1) p_board_side = 1;
	if (p_bs < 1) p_bs = 1;
	PoolVector<uint8_t> data;
	data.resize(p_board_side * p_board_side * 4);
	PoolVector<uint8_t>::Write w = data.write();
	uint8_t *p = w.ptr();
	memset(p, 0, (size_t)p_board_side * p_board_side * 4);
	if (bus->busindex) {
		for (int f = 0; f < p_board_side * p_board_side; f++) {
			int bi = bus->busindex[f];
			if (bi < 0)
				continue;
			int bx = bi % p_bs, by = bi / p_bs;
			p[f * 4 + 0] = bx & 0xff;
			p[f * 4 + 1] = (bx >> 8) & 0xff;
			p[f * 4 + 2] = by & 0xff;
			p[f * 4 + 3] = (by >> 8) & 0xff;
		}
	}
	Ref<Image> img;
	img.instance();
	img->create(p_board_side, p_board_side, false, Image::FORMAT_RGBA8, data);
	return img;
}

void TransistorCompiler::setup(const Ref<Image> &p_image) {
	building_image = p_image;
	progress = 0;
}

// Thread worker. Runs the recovered front-end pipeline over the image, then
// builds the model. Godot calls this via Thread.start(compiler, "compute"), which
// always passes a userdata argument (null here) -- so it must be declared with one
// parameter even though the pipeline ignores it. The game polls get_progress()
// until it returns 1000 (or -1 on error).
void TransistorCompiler::compute(const Variant &) {
	errors.clear();
	textures.clear();
	circuit_model = Ref<TransistorCircuitModel>(memnew(TransistorCircuitModel));

	if (building_image.is_null()) {
		progress = -1;
		return;
	}

	Ref<Image> img = building_image;
	const int side = img->get_width();
	PoolVector<uint8_t> data = img->get_data();
	const int len = data.size();
	PoolVector<uint8_t>::Read r = data.read();

	// --- run the recovered Godot-free pipeline ---
	TCAnalysisCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.side = side;
	vcb_prepare(&ctx, r.ptr(), len);
	vcb_scan_pixels(&ctx);

	if (ctx.errors.begin != ctx.errors.end) {
		// Surface the {x, y, code} error records to GDScript.
		for (TCPixel *e = (TCPixel *)ctx.errors.begin; e != (TCPixel *)ctx.errors.end; e++) {
			Array rec;
			rec.push_back(e->ink); // code
			rec.push_back(Vector2(e->x, e->y));
			errors.push_back(rec);
		}
		vcb_ctx_free(&ctx);
		progress = -1;
		return;
	}

	vcb_link(&ctx);
	vcb_finalize(&ctx);
	entitylist_sidelength = ctx.entitylist_sidelength;

	// --- build the circuit graph (build_stage2 + construct_model) ---
	// Recovered Godot-free in core/vcb_model: assign entity indices (components
	// then trace nets), build the pixel->entity LUT, form gate<->net edges (a WRITE
	// junction is an output, an adjacent trace an input), and emit the flat
	// circuit_data {state,ink,n_conn,0} + adjacency arrays as the binary's
	// construct_model (0x3e3550) does. The native model is owned by the
	// TransistorCircuitModel (freed in its dtor) and simulated by TransistorEngine.
	VCBModel *nm = (VCBModel *)malloc(sizeof(VCBModel));
	if (nm && vcb_model_build(&ctx, nm) != 0) {
		free(nm);
		nm = nullptr;
	}
	circuit_model->native = nm;

	// --- render the six textures ---
	textures.clear();
	{
		const int npix = len / 4;
		PoolVector<uint8_t> on_data, off_data;
		on_data.resize(len);
		off_data.resize(len);
		{
			const uint8_t *src = r.ptr();
			PoolVector<uint8_t>::Write ow = on_data.write();
			PoolVector<uint8_t>::Write fw = off_data.write();
			uint8_t *on_p = ow.ptr();
			uint8_t *off_p = fw.ptr();
			for (int i = 0; i < npix; i++) {
				uint32_t rgb = ((uint32_t)src[i * 4] << 16) | ((uint32_t)src[i * 4 + 1] << 8) |
						(uint32_t)src[i * 4 + 2];
				uint32_t on = 0, off = 0;
				TC_resolve_color(rgb, &on, &off);
				on_p[i * 4 + 0] = (on >> 16) & 0xff;
				on_p[i * 4 + 1] = (on >> 8) & 0xff;
				on_p[i * 4 + 2] = on & 0xff;
				on_p[i * 4 + 3] = 0xff;
				off_p[i * 4 + 0] = (off >> 16) & 0xff;
				off_p[i * 4 + 1] = (off >> 8) & 0xff;
				off_p[i * 4 + 2] = off & 0xff;
				off_p[i * 4 + 3] = 0xff;
			}
		}
		Ref<Image> on_img;
		on_img.instance();
		on_img->create(side, side, false, Image::FORMAT_RGBA8, on_data);
		Ref<Image> off_img;
		off_img.instance();
		off_img->create(side, side, false, Image::FORMAT_RGBA8, off_data);

		// [0] on, [1] off, [2] die (entity LUT), [3] inverse_entitylut (entity ->
		// board). [4] buslut / [5] busentities encode the bus subsystem: bus pixels
		// are grouped into nets (vcb_bus), the die marks them with the (65535,65535)
		// sentinel, buslut maps each to its net's entity-list start, and busentities
		// holds the per-net (0)-terminated connected-entity coords. Grouping is a
		// reconstruction (see core/vcb_bus.c); the texture encoding is exact.
		const int lut = entitylist_sidelength > 0 ? entitylist_sidelength : 1;
		VCBBusData bus;
		vcb_bus_build(&ctx, nm, &bus);
		const int32_t *bidx = bus.has_buses ? bus.busindex : nullptr;
		const int bs = bus.has_buses ? tc_busside(bus.entities_len) : 1;
		// compute() runs on a background Thread, so it stores plain Images here;
		// get_textures() (called on the main thread) wraps them in ImageTextures,
		// because GPU texture creation off the main thread yields textures whose
		// get_data() returns null in the real renderer (the game reads texture_die
		// back to an Image). [0] on [1] off [2] die [3] inverse_entitylut
		// [4] buslut [5] busentities.
		textures.push_back(on_img);                              // [0] texture_on
		textures.push_back(off_img);                             // [1] texture_off
		textures.push_back(tc_die_texture(nm, side, lut, bidx)); // [2] texture_die
		textures.push_back(tc_inverse_entitylut_texture(nm, lut)); // [3] inverse_entitylut
		if (bus.has_buses) {
			textures.push_back(tc_buslut_texture(&bus, side, bs));     // [4] buslut
			textures.push_back(tc_busentities_texture(&bus, lut, bs)); // [5] busentities
		} else {
			textures.push_back(tc_blank_rgba8(lut, lut));   // [4] (no buses)
			textures.push_back(tc_blank_rgba8(side, side)); // [5] (no buses)
		}
		vcb_bus_free(&bus);
	}

	// --- populate the Godot model container from the native graph ---
	if (nm) {
		circuit_model->circuit_width = nm->sidelength;
		circuit_model->clock_value = nm->clock_value;
		circuit_model->timer_value = nm->timer_value;
		uint8_t *cd = nullptr;
		int32_t cdl = 0;
		vcb_model_emit_circuit_data(nm, &cd, &cdl);
		circuit_model->circuit_data.resize(cdl);
		{
			PoolVector<uint8_t>::Write w = circuit_model->circuit_data.write();
			memcpy(w.ptr(), cd, (size_t)cdl);
		}
		free(cd);
		int32_t *adj = nullptr;
		int32_t adjl = 0;
		vcb_model_emit_adjacency(nm, &adj, &adjl);
		circuit_model->adjacency.resize(adjl);
		{
			PoolVector<int>::Write w = circuit_model->adjacency.write();
			for (int32_t i = 0; i < adjl; i++)
				w.ptr()[i] = adj[i];
		}
		free(adj);
	} else {
		circuit_model->circuit_width = entitylist_sidelength;
	}

	// --- build get_stats(): [cells_group, entities_group], each a list of
	// [STATSTYPE, count] pairs (only categories present, ascending). card_statistics.gd
	// indexes stats[0] and stats[1], so both groups must exist even when empty.
	stats.clear();
	{
		Array cells_group, entities_group;
		for (int s = 1; s < 256; s++) {
			if (ctx.stats[s] > 0) {
				Array e;
				e.push_back(s);
				e.push_back(ctx.stats[s]);
				cells_group.push_back(e);
			}
		}
		for (int s = 1; s < 256; s++) {
			if (ctx.entity_stats[s] > 0) {
				Array e;
				e.push_back(s);
				e.push_back(ctx.entity_stats[s]);
				entities_group.push_back(e);
			}
		}
		stats.push_back(cells_group);
		stats.push_back(entities_group);
	}

	vcb_ctx_free(&ctx);
	progress = 1000;
}

int TransistorCompiler::get_progress() const { return progress; }
int TransistorCompiler::get_entitylist_sidelength() const { return entitylist_sidelength; }
Ref<TransistorCircuitModel> TransistorCompiler::get_circuit_model() { return circuit_model; }
// Called on the main thread after the compile thread finishes. Wraps each stored
// Image in an ImageTexture here (not in the worker) so the GPU textures are created
// on the main thread and their get_data() works in the real renderer.
Array TransistorCompiler::get_textures() {
	Array out;
	for (int i = 0; i < textures.size(); i++) {
		Ref<Image> img = textures[i];
		out.push_back(tc_to_texture(img));
	}
	return out;
}
Array TransistorCompiler::get_stats() { return stats; }
Array TransistorCompiler::get_errors() { return errors; }

void TransistorCompiler::compute_vmem_data(const PoolVector<uint8_t> &p_live_vmem,
		const PoolVector<int> &p_assembly, const Array &p_queues) {
	// compute_vmem_data (RVA 0x21dc50) -> builder 0x3e3c70 (fully decompiled):
	//  (1) model +0x158 (VMem word image): word[i] = BE32(live_vmem[4i..]) |
	//      assembly[i] (vcb_vmem_build);
	//  (2) model +0x170 <- queues[0]: VMem ADDRESS-latch entity indices;
	//  (3) model +0x188 <- queues[1]: VMem CONTENT-latch entity indices.
	// The engine copies +0x170/+0x188 to its +0x2e0/+0x2f8; the runtime VMem kernel
	// maps the address/data bus from them. Surrounding binary code is Godot Array
	// COW _ref/_unref + PoolVector bounds-check scaffolding.
	if (circuit_model.is_null() || !circuit_model->native)
		return;
	VCBModel *nm = circuit_model->native;
	free(nm->vmem);
	nm->vmem = nullptr;
	nm->vmem_len = 0;
	free(nm->vmem_addr_entities);
	nm->vmem_addr_entities = nullptr;
	nm->vmem_addr_count = 0;
	free(nm->vmem_content_entities);
	nm->vmem_content_entities = nullptr;
	nm->vmem_content_count = 0;

	const int live_len = p_live_vmem.size();
	const int asm_len = p_assembly.size();
	if (live_len >= 4) {
		PoolVector<uint8_t>::Read lr = p_live_vmem.read();
		int32_t *asm_buf = nullptr;
		if (asm_len > 0) {
			asm_buf = (int32_t *)malloc((size_t)asm_len * sizeof(int32_t));
			PoolVector<int>::Read ar = p_assembly.read();
			for (int i = 0; i < asm_len; i++)
				asm_buf[i] = ar.ptr()[i];
		}
		int32_t *words = nullptr;
		int32_t n = vcb_vmem_build(lr.ptr(), live_len, asm_buf, asm_len, &words);
		free(asm_buf);
		nm->vmem = words;
		nm->vmem_len = n;
	}

	// queues = [address_entity_indices, content_entity_indices] (each an Array of
	// int entity-LUT indices); append into the model's two latch lists.
	if (p_queues.size() >= 2) {
		Array qa = p_queues[0];
		Array qc = p_queues[1];
		if (qa.size() > 0) {
			nm->vmem_addr_entities = (int32_t *)malloc((size_t)qa.size() * sizeof(int32_t));
			for (int i = 0; i < qa.size(); i++)
				nm->vmem_addr_entities[i] = (int)qa[i];
			nm->vmem_addr_count = qa.size();
		}
		if (qc.size() > 0) {
			nm->vmem_content_entities = (int32_t *)malloc((size_t)qc.size() * sizeof(int32_t));
			for (int i = 0; i < qc.size(); i++)
				nm->vmem_content_entities[i] = (int)qc[i];
			nm->vmem_content_count = qc.size();
		}
	}
}

void TransistorCompiler::set_vinput_entities_indexes(const Array &p_indexes) {
	// The editor decodes the VINPUT entity indices from texture_die and passes them
	// here (compiler.gd). Record them on the model so the engine can drive those
	// entities from the virtual-input value each solve().
	if (circuit_model.is_null())
		return;
	circuit_model->vinput_indices.resize(p_indexes.size());
	PoolVector<int>::Write w = circuit_model->vinput_indices.write();
	for (int i = 0; i < p_indexes.size(); i++)
		w.ptr()[i] = (int)p_indexes[i];
}

void TransistorCompiler::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "image"), &TransistorCompiler::setup);
	ClassDB::bind_method(D_METHOD("compute", "userdata"), &TransistorCompiler::compute);
	ClassDB::bind_method(D_METHOD("get_progress"), &TransistorCompiler::get_progress);
	ClassDB::bind_method(D_METHOD("get_entitylist_sidelength"),
			&TransistorCompiler::get_entitylist_sidelength);
	ClassDB::bind_method(D_METHOD("get_circuit_model"), &TransistorCompiler::get_circuit_model);
	ClassDB::bind_method(D_METHOD("get_textures"), &TransistorCompiler::get_textures);
	ClassDB::bind_method(D_METHOD("get_stats"), &TransistorCompiler::get_stats);
	ClassDB::bind_method(D_METHOD("get_errors"), &TransistorCompiler::get_errors);
	ClassDB::bind_method(D_METHOD("compute_vmem_data", "live_vmem", "assembly", "queues"),
			&TransistorCompiler::compute_vmem_data);
	ClassDB::bind_method(D_METHOD("set_vinput_entities_indexes", "indexes"),
			&TransistorCompiler::set_vinput_entities_indexes);

	BIND_CONSTANT(UNEXPECTED_TUNNEL_ENTRANCE);
	BIND_CONSTANT(UNMATCHED_TUNNEL_LEFT);
	BIND_CONSTANT(UNMATCHED_TUNNEL_RIGHT);
	BIND_CONSTANT(UNMATCHED_TUNNEL_UP);
	BIND_CONSTANT(UNMATCHED_TUNNEL_DOWN);
}
