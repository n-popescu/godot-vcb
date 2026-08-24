/* vcb_vdisplay.c -- see vcb_vdisplay.h.
 *
 * Line-by-line port of get_vdisplay_texture's repack loop (RVA 0x28fd80):
 *   r14 = w*h (total pixels); r15 = (1<<color_depth)-1 (mask);
 *   ebx = word_size / color_depth (pixels per word, "ppw");
 *   if color_depth == 0x18 -> direct-RGB path, else palette path (MSB-first).
 * The engine stops emitting once r14 pixels are written; anything past the
 * staging data stays zero (caller-zeroed buffer).
 */
#include "vcb_vdisplay.h"

void vcb_vdisplay_repack(const int32_t *staging, int32_t staging_count,
		int w, int h, int word_size, int color_depth,
		const int32_t *palette, int palette_len, uint8_t *out) {
	if (!out || w <= 0 || h <= 0)
		return;
	const int total = w * h;
	int pix = 0;

	if (color_depth == 24) {
		/* Direct 0xRRGGBB, one staging word per pixel. */
		for (int32_t i = 0; i < staging_count && pix < total; i++, pix++) {
			int32_t v = staging ? staging[i] : 0;
			out[pix * 4 + 0] = (uint8_t)((v >> 16) & 0xff);
			out[pix * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
			out[pix * 4 + 2] = (uint8_t)(v & 0xff);
			out[pix * 4 + 3] = 0;
		}
		return;
	}

	/* Palette-indexed: ppw pixels per word, most-significant sub-pixel first. */
	if (color_depth <= 0)
		return;
	const uint32_t mask = (color_depth >= 32) ? 0xffffffffu : ((1u << color_depth) - 1u);
	const int ppw = word_size / color_depth;
	if (ppw <= 0)
		return;
	for (int32_t i = 0; i < staging_count && pix < total; i++) {
		int32_t word = staging ? staging[i] : 0;
		for (int sp = ppw - 1; sp >= 0 && pix < total; sp--, pix++) {
			int idx = (int)(((uint32_t)word >> (sp * color_depth)) & mask);
			int32_t c = (palette && idx >= 0 && idx < palette_len) ? palette[idx] : 0;
			out[pix * 4 + 0] = (uint8_t)((c >> 16) & 0xff);
			out[pix * 4 + 1] = (uint8_t)((c >> 8) & 0xff);
			out[pix * 4 + 2] = (uint8_t)(c & 0xff);
			out[pix * 4 + 3] = 0;
		}
	}
}
