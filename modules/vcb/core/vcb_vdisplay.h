/* vcb_vdisplay.h -- virtual-display raster repack (Phase B).
 *
 * Faithful reconstruction of TransistorEngine::get_vdisplay_texture's pixel
 * repack (RVA 0x28fd80, disassembled from working_exes/vcb.exe; see
 * vcb-engine-recovery/docs/vmem_vdisplay.md). Pure, Godot-free, unit-tested; the
 * module/GDNative get_vdisplay_texture wrappers call this to turn the per-tick
 * display memory slice ("staging") into an RGBA8 image.
 */
#ifndef VCB_VDISPLAY_H
#define VCB_VDISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Repack `staging` (staging_count int32 words -- the display's per-tick VMem
 * slice) into `out` (w*h*4 bytes, RGBA8). `out` must be pre-zeroed by the caller;
 * pixels beyond what the staging provides are left at 0.
 *
 * Exactly mirrors get_vdisplay_texture (0x28fd80):
 *   - color_depth == 24: each staging word is a direct 0xRRGGBB pixel
 *     (R=(w>>16), G=(w>>8), B=w, A=0), one word per pixel.
 *   - otherwise: each staging word packs `word_size / color_depth` pixels,
 *     MSB-first (sub-pixel index counts down from ppw-1), each `color_depth`
 *     bits wide, used as an index into `palette` (palette_len 0xRRGGBB colors).
 * Output stops once w*h pixels have been written. */
void vcb_vdisplay_repack(const int32_t *staging, int32_t staging_count,
		int w, int h, int word_size, int color_depth,
		const int32_t *palette, int palette_len,
		uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VCB_VDISPLAY_H */
