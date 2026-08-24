/* vcb_bus.h -- bus-net grouping + render-texture data (Phase B reconstruction).
 *
 * Produces the data the game's circuit shader needs for VCB "bus" rendering:
 * per-board-pixel bus indices (buslut) and a flat, 0-terminated per-net list of
 * connected entity indices (busentities). The exact texture ENCODING is decoded
 * byte-for-byte from the shader (engine-recovery/docs/bus_rendering.md); the bus
 * GROUPING here (4-connected components of same-ink bus pixels; a net's entities =
 * the traces recorded touching it) is a reasoned reconstruction of build_stage1's
 * bus pass, not yet differentially validated against vcb.exe.
 */
#ifndef VCB_BUS_H
#define VCB_BUS_H

#include "vcb_types.h"
#include "vcb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VCBBusData {
	int32_t *busindex;      /* [board_side^2]: bus pixel -> its net's start index in
	                         * `entities`; -1 for non-bus pixels. */
	int32_t  board_side;
	int32_t *entities;      /* flat: each net's entity indices then a 0 terminator */
	int32_t  entities_len;
	int32_t  has_buses;     /* 1 if any bus pixel exists */
} VCBBusData;

/* Label every bus pixel with its bus-net id, using the verified grouping rule:
 *   - bus pixels are 4-connected, and ANY two adjacent bus pixels join whatever
 *     their bus colours (a BUS_1 next to a BUS_2 carries a signal straight through);
 *   - a bus runs straight through a CROSS to the pixel two cells along;
 *   - every bus net of a given bus COLOUR that touches any MESH pixel is merged
 *     into one board-wide net, the same way a mesh merges same-ink traces and
 *     same-ink components.
 * `out_label` is [side*side], -1 for non-bus pixels; returns the net count (net
 * ids are dense, 0..count-1). What a bus keeps separate is the TRACE colour and
 * the component INK of the groups touching it, which callers key on. */
int32_t vcb_bus_label(const TCAnalysisCtx *ctx, int32_t *out_label);

/* Group bus pixels (inks 0xe8..0xed) into 4-connected same-ink nets over the
 * finalized ctx (uses ctx->classified + ctx->bus_connections + ctx->side) and the
 * built model's LUT, and fill `out`. Caller frees with vcb_bus_free. */
void vcb_bus_build(const TCAnalysisCtx *ctx, const VCBModel *model, VCBBusData *out);
void vcb_bus_free(VCBBusData *b);

#ifdef __cplusplus
}
#endif

#endif /* VCB_BUS_H */
