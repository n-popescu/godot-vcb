/* vcb_vmem.c -- the compile-time VMem image builder (from vcb.exe 0x3e3c70, called
 * via compute_vmem_data 0x21dc50). It merges the player's live VMem bytes with the
 * assembled program into the initial memory image the simulator runs against:
 *   word[i] = BE32(live_vmem[4i .. 4i+4]) | assembly[i]
 * (big-endian pack of the live bytes, OR the assembled word). The runtime read/
 * write sweep lives in vcb_sim.c; get_vmem_persistent's repack is in the engine
 * wrapper. See vcb_vmem.h and docs/vmem_vdisplay.md. */
#include "vcb_vmem.h"

#include <stdlib.h>

int32_t vcb_vmem_build(const uint8_t *live_vmem, int32_t live_len,
		const int32_t *assembly, int32_t asm_len, int32_t **out_words) {
	if (out_words)
		*out_words = NULL;
	if (live_len < 0)
		live_len = 0;
	int32_t n = live_len / 4;
	if (n <= 0 || !out_words)
		return 0;
	int32_t *w = (int32_t *)malloc((size_t)n * sizeof(int32_t));
	if (!w)
		return 0;
	/* Word 0 is the reserved slot: the original's image is always 0 there, dropping
	 * both the live bytes and the assembly word at index 0 -- measured against the
	 * original engine (tools/phasec, probe p13 + the vpat/vasm images: live word 0 =
	 * 0xab with assembly[0] = 0x55 still reads back as 0, while every word from 1 up
	 * carries live | assembly exactly). Same 1-indexing as the rest of the engine
	 * (entities, circuit_data, adjacency). Runtime writes to address 0 do land -- it
	 * is only the compiled image that skips it. */
	if (n > 0)
		w[0] = 0;
	for (int32_t i = 1; i < n; i++) {
		uint32_t b0 = live_vmem[i * 4 + 0];
		uint32_t b1 = live_vmem[i * 4 + 1];
		uint32_t b2 = live_vmem[i * 4 + 2];
		uint32_t b3 = live_vmem[i * 4 + 3];
		uint32_t packed = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
		uint32_t prog = (assembly && i < asm_len) ? (uint32_t)assembly[i] : 0u;
		w[i] = (int32_t)(packed | prog);
	}
	*out_words = w;
	return n;
}

void vcb_vmem_persistent(const int32_t *words, int32_t n_words,
		int32_t start, int32_t end, uint8_t *out) {
	if (!out)
		return;
	for (int32_t idx = start, o = 0; idx < end; idx++, o += 4) {
		uint32_t v = (words && idx >= 0 && idx < n_words) ? (uint32_t)words[idx] : 0u;
		out[o + 0] = (uint8_t)((v >> 24) & 0xff);
		out[o + 1] = (uint8_t)((v >> 16) & 0xff);
		out[o + 2] = (uint8_t)((v >> 8) & 0xff);
		out[o + 3] = (uint8_t)(v & 0xff);
	}
}
