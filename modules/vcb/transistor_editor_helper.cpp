#include "transistor_editor_helper.h"

#include "core/class_db.h"
// bucket_flood_fill binds 7 parameters; ClassDB::bind_method only supports up to
// 5 unless this extended generated binder is included (see core/class_db.h).
#include "core/method_bind_ext.gen.inc"
#include "core/vector.h"

extern "C" {
#include "core/vcb_classifier.h"
}

// Editor-side helpers, operating on Godot Images. These are standard editor
// tools (not the recovered simulation IP): the original bodies in vcb.exe were
// Godot-Image pixel ops, so they are (re)implemented here against the Godot 3.5.1
// Image API to match the behaviour the game GDScript relies on (editor.gd,
// tool_bucket.gd, tool_selection.gd -- see docs/gdscript_api_audit.md).

// Pack a Color's 8-bit RGB the same way the compiler/classifier does
// ((R<<16)|(G<<8)|B) so we can reuse the recovered ink classifier to recognise
// the CROSS ink for the bucket's cross pass-through.
static inline uint32_t teh_pack_rgb(const Color &c) {
	uint32_t r = (uint32_t)(c.r * 255.0f + 0.5f) & 0xff;
	uint32_t g = (uint32_t)(c.g * 255.0f + 0.5f) & 0xff;
	uint32_t b = (uint32_t)(c.b * 255.0f + 0.5f) & 0xff;
	return (r << 16) | (g << 8) | b;
}

static inline bool teh_is_cross(const Color &c) {
	return TC_classify_color(teh_pack_rgb(c), NULL) == 0x64; // CROSS ink
}

void TransistorEditorHelper::initialize(int p_span) { circuit_span = p_span; }

// Flood-fill the region of p_active_layer that is the same colour as p_target,
// starting at p_pos, recolouring it to p_draw. 4-connected. When
// p_pass_through_crosses is set (only true when filling the LOGIC layer, per
// tool_bucket.gd), a CROSS pixel does not stop the fill: it is skipped over to
// the pixel two cells further in the same direction (the cross itself keeps its
// colour), mirroring the compiler's get_neighbors cross rule so a trace fill
// continues across a wire crossing.
void TransistorEditorHelper::bucket_flood_fill(const Color &p_target, const Color &p_draw,
		const Vector2 &p_pos, const Ref<Image> &p_active_layer, const Ref<Image> &p_logic_layer,
		bool p_is_logic_layer, bool p_pass_through_crosses) {
	(void)p_is_logic_layer;
	if (p_active_layer.is_null())
		return;
	if (p_target == p_draw)
		return; // no-op; also avoids re-visiting freshly drawn pixels

	Ref<Image> img = p_active_layer;
	const int w = img->get_width();
	const int h = img->get_height();
	const int sx = (int)p_pos.x;
	const int sy = (int)p_pos.y;
	if (w <= 0 || h <= 0 || sx < 0 || sx >= w || sy < 0 || sy >= h)
		return;

	// Cross detection reads the logic layer (which equals the active layer when
	// filling the logic layer, the only case pass_through_crosses is set).
	Ref<Image> logic = p_logic_layer;
	const bool logic_is_other = logic.is_valid() && logic.ptr() != img.ptr();

	img->lock();
	if (logic_is_other)
		logic->lock();

	Vector<uint8_t> visited;
	visited.resize(w * h);
	uint8_t *vis = visited.ptrw();
	for (int i = 0; i < w * h; i++)
		vis[i] = 0;

	if (!(img->get_pixel(sx, sy) == p_target)) {
		img->unlock();
		if (logic_is_other)
			logic->unlock();
		return;
	}

	static const int DX[4] = { -1, 1, 0, 0 };
	static const int DY[4] = { 0, 0, -1, 1 };

	Vector<int> stack;
	stack.push_back(sy * w + sx);

	while (stack.size() > 0) {
		const int idx = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		if (vis[idx])
			continue;
		vis[idx] = 1;
		const int x = idx % w;
		const int y = idx / w;
		if (!(img->get_pixel(x, y) == p_target))
			continue;
		img->set_pixel(x, y, p_draw);

		for (int d = 0; d < 4; d++) {
			const int nx = x + DX[d];
			const int ny = y + DY[d];
			if (nx < 0 || nx >= w || ny < 0 || ny >= h)
				continue;
			const int ni = ny * w + nx;
			if (!vis[ni] && img->get_pixel(nx, ny) == p_target) {
				stack.push_back(ni);
				continue;
			}
			if (p_pass_through_crosses) {
				const Color cross_probe =
						logic_is_other ? logic->get_pixel(nx, ny) : img->get_pixel(nx, ny);
				if (teh_is_cross(cross_probe)) {
					const int mx = x + 2 * DX[d];
					const int my = y + 2 * DY[d];
					if (mx < 0 || mx >= w || my < 0 || my >= h)
						continue;
					const int mi = my * w + mx;
					if (!vis[mi] && img->get_pixel(mx, my) == p_target)
						stack.push_back(mi); // skip the cross; do not recolour it
				}
			}
		}
	}

	img->unlock();
	if (logic_is_other)
		logic->unlock();
}

// Replace every pixel equal to p_target with p_draw across the whole image
// (the bucket's non-adjacent mode, tool_bucket.gd:63).
void TransistorEditorHelper::bucket_replace(const Color &p_target, const Color &p_draw,
		const Ref<Image> &p_img) {
	if (p_img.is_null())
		return;
	if (p_target == p_draw)
		return;
	Ref<Image> img = p_img;
	const int w = img->get_width();
	const int h = img->get_height();
	img->lock();
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (img->get_pixel(x, y) == p_target)
				img->set_pixel(x, y, p_draw);
		}
	}
	img->unlock();
}

// Transpose the image in place: pixel (x, y) <- (y, x); dimensions swap
// w x h -> h x w. Used by tool_selection.gd's get_rotated_image (transpose +
// flip = rotate).
void TransistorEditorHelper::transpose(const Ref<Image> &p_img) {
	if (p_img.is_null())
		return;
	Ref<Image> img = p_img;
	const int w = img->get_width();
	const int h = img->get_height();
	if (w <= 0 || h <= 0)
		return;
	const Image::Format fmt = img->get_format();

	Vector<Color> src;
	src.resize(w * h);
	Color *sp = src.ptrw();
	img->lock();
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			sp[y * w + x] = img->get_pixel(x, y);
	img->unlock();

	img->create(h, w, false, fmt); // new dims: width = h, height = w
	img->lock();
	for (int nx = 0; nx < h; nx++)
		for (int ny = 0; ny < w; ny++)
			img->set_pixel(nx, ny, sp[nx * w + ny]); // new(nx,ny) = old(ny,nx)
	img->unlock();
}

void TransistorEditorHelper::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "span"), &TransistorEditorHelper::initialize);
	ClassDB::bind_method(
			D_METHOD("bucket_flood_fill", "target", "draw", "position", "active_layer",
					"logic_layer", "is_logic_layer", "pass_through_crosses"),
			&TransistorEditorHelper::bucket_flood_fill);
	ClassDB::bind_method(D_METHOD("bucket_replace", "target", "draw", "img"),
			&TransistorEditorHelper::bucket_replace);
	ClassDB::bind_method(D_METHOD("transpose", "img"), &TransistorEditorHelper::transpose);
}
