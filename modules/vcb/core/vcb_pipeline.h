/* vcb_pipeline.h -- the recovered TransistorCompiler front-end, Godot-free.
 *
 * These are the reconstructed + unit-tested passes from vcb-engine-recovery
 * (docs/compiler_pipeline.md). `prepare` takes a raw RGBA8 buffer (the Godot
 * module extracts it from the Image via get_data()); everything else operates on
 * the analysis context. The container-heavy helpers the binary keeps in std::set /
 * RB-tree form (tunnel resolver, bus-connection dedup, mesh Phase 2, entity
 * category map) are provided as weak stubs here and completed in the module.
 */
#ifndef VCB_PIPELINE_H
#define VCB_PIPELINE_H

#include "vcb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 1: classify an RGBA8 image (len bytes) into the ctx->classified buffer,
 * collect MESH pixels, pre-mark background. ctx->side must be set first. */
void vcb_prepare(TCAnalysisCtx *ctx, const uint8_t *rgba, int32_t len);

/* Stage 2: flood-fill connected components into raw_components / raw_traces. */
void vcb_scan_pixels(TCAnalysisCtx *ctx);

/* Stage 3: merge component groups sharing a global-net ink (ctx->net_table). */
void vcb_link(TCAnalysisCtx *ctx);

/* Stage 4: turn raw groups into entities (entities_a / entities_b). */
void vcb_finalize(TCAnalysisCtx *ctx);

/* Free everything the passes allocated inside ctx (best-effort). */
void vcb_ctx_free(TCAnalysisCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VCB_PIPELINE_H */
