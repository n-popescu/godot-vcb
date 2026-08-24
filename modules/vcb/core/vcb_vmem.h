/* vcb_vmem.h -- virtual-memory (VMem) initial-image builder + persistent repack.
 *
 * Reconstructed from TransistorCompiler::compute_vmem_data (RVA 0x21dc50) and its
 * worker 0x3e3c70, and TransistorEngine::get_vmem_persistent (RVA 0x2900a0),
 * disassembled from working_exes/vcb.exe (see
 * vcb-engine-recovery/docs/vmem_vdisplay.md). Pure, Godot-free, unit-tested.
 *
 * The builder's core data transform is fully recovered: the model's VMem word
 * image is `BE32(live_vmem[4i..4i+4]) | assembly[i]` per word. The surrounding
 * `queues` pass in the binary is Godot error/warning-macro scaffolding around
 * bounds checks (per-queue diagnostics), not additional data writes, so it is not
 * reproduced here. See the doc for the confidence split.
 */
#ifndef VCB_VMEM_H
#define VCB_VMEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the initial VMem word image exactly as builder 0x3e3c70 does:
 *   n = live_len / 4;  words[i] = (b0<<24)|(b1<<16)|(b2<<8)|b3 | assembly[i]
 * where b0..b3 = live_vmem[4i..4i+4] (big-endian) and assembly[i] is the program
 * word (0 if `assembly` is shorter than n -- the binary asserts len; we tolerate).
 * Allocates *out_words (caller frees); returns n (>= 0). */
int32_t vcb_vmem_build(const uint8_t *live_vmem, int32_t live_len,
		const int32_t *assembly, int32_t asm_len, int32_t **out_words);

/* get_vmem_persistent (0x2900a0): for each word index in [start, end), emit its 4
 * bytes BIG-ENDIAN (byte-reversed from little-endian storage). Writes
 * (end-start)*4 bytes into `out` (caller sizes it). Indices out of range emit 0. */
void vcb_vmem_persistent(const int32_t *words, int32_t n_words,
		int32_t start, int32_t end, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VCB_VMEM_H */
