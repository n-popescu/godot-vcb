/* vcb_bus.c -- see vcb_bus.h. Reasoned reconstruction of build_stage1's bus pass;
 * the texture ENCODING it feeds is exact (from the shader), and the GROUPING is now
 * measured against the original's own texture_buslut (tools/phasec/cmp_bus.py): on
 * 01_32_bit_computer_compact the two partitions agree over all 10542 bus pixels
 * except one 16-pixel net, where the original's buslut points two geometrically
 * separate runs at the same entity-list index. */
#include "vcb_bus.h"

#include <stdlib.h>
#include <string.h>

static int bus_is_bus(uint8_t ink) { return (uint8_t)(ink - 0xe8) <= 5; }

int32_t vcb_bus_label(const TCAnalysisCtx *ctx, int32_t *out_label) {
	if (!ctx || ctx->side <= 0 || !ctx->classified.begin || !out_label)
		return 0;
	const int side = ctx->side;
	const int n = side * side;
	const uint8_t *cls = (const uint8_t *)ctx->classified.begin;
	static const int DX[4] = { -1, 1, 0, 0 }, DY[4] = { 0, 0, -1, 1 };
	int32_t *stack = (int32_t *)malloc((size_t)n * sizeof(int32_t));
	if (!stack)
		return 0;
	for (int i = 0; i < n; i++)
		out_label[i] = -1;

	int32_t nnets = 0;
	for (int p = 0; p < n; p++) {
		if (!bus_is_bus(cls[(size_t)p * 2]) || out_label[p] != -1)
			continue;
		int sp = 0;
		stack[sp++] = p;
		out_label[p] = nnets;
		while (sp) {
			int cur = stack[--sp];
			int cx = cur % side, cy = cur / side;
			for (int d = 0; d < 4; d++) {
				int nx = cx + DX[d], ny = cy + DY[d];
				if (nx < 0 || nx >= side || ny < 0 || ny >= side)
					continue;
				int nn = ny * side + nx;
				uint8_t nink = cls[(size_t)nn * 2];
				if (nink == 0x64) { /* straight through a CROSS */
					int mx = cx + 2 * DX[d], my = cy + 2 * DY[d];
					if (mx < 0 || mx >= side || my < 0 || my >= side)
						continue;
					nn = my * side + mx;
				} else if (nink == 0x65) {
					/* and through a TUNNEL, exactly like a trace (TC_tunnel_resolve in
					 * vcb_pipeline.c): scan on from the cell past the entrance and take
					 * the first tunnel whose far cell carries the SAME INK as the bus
					 * pixel we came from. Tunnel pairs serving other wires lie between
					 * the two ends of a long run and must be walked over -- taking the
					 * first tunnel found instead split 64 trace nets on
					 * 01_32_bit_computer_compact. Measured against the original: BUS_0
					 * tunnels to BUS_0 but not to BUS_1 (probe p16). */
					uint8_t want = cls[(size_t)cur * 2];
					int tx = cx + 2 * DX[d], ty = cy + 2 * DY[d];
					int found = 0;
					while (tx >= 0 && tx < side && ty >= 0 && ty < side) {
						if (cls[((size_t)ty * side + tx) * 2] == 0x65) {
							int ex = tx + DX[d], ey = ty + DY[d];
							if (ex >= 0 && ex < side && ey >= 0 && ey < side &&
									cls[((size_t)ey * side + ex) * 2] == want) {
								nn = ey * side + ex;
								found = 1;
								break;
							}
						}
						tx += DX[d];
						ty += DY[d];
					}
					if (!found)
						continue;
				}
				if (out_label[nn] != -1 || !bus_is_bus(cls[(size_t)nn * 2]))
					continue;
				out_label[nn] = nnets;
				stack[sp++] = nn;
			}
		}
		nnets++;
	}
	free(stack);
	if (nnets == 0)
		return 0;

	/* MESH merges same-ink groups board-wide, and a bus net is such a group. */
	int32_t *rep = (int32_t *)malloc((size_t)nnets * sizeof(int32_t));
	if (!rep)
		return nnets;
	for (int32_t i = 0; i < nnets; i++)
		rep[i] = i;
	int32_t mesh_anchor[6];
	for (int i = 0; i < 6; i++)
		mesh_anchor[i] = -1;
	for (int p = 0; p < n; p++) {
		if (out_label[p] < 0)
			continue;
		uint8_t bink = cls[(size_t)p * 2];
		if (!bus_is_bus(bink))
			continue;
		int px = p % side, py = p / side, touches = 0;
		for (int d = 0; d < 4 && !touches; d++) {
			int nx = px + DX[d], ny = py + DY[d];
			if (nx < 0 || nx >= side || ny < 0 || ny >= side)
				continue;
			if (cls[((size_t)ny * side + nx) * 2] == 0x66)
				touches = 1;
		}
		if (!touches)
			continue;
		int slot = bink - 0xe8;
		if (mesh_anchor[slot] < 0) {
			mesh_anchor[slot] = out_label[p];
			continue;
		}
		int32_t a = out_label[p], b = mesh_anchor[slot];
		while (rep[a] != a) a = rep[a];
		while (rep[b] != b) b = rep[b];
		if (a != b)
			rep[a > b ? a : b] = (a < b ? a : b);
	}
	for (int32_t i = 0; i < nnets; i++) {
		int32_t r = i;
		while (rep[r] != r)
			r = rep[r];
		rep[i] = r;
	}
	for (int p = 0; p < n; p++)
		if (out_label[p] >= 0)
			out_label[p] = rep[out_label[p]];
	free(rep);
	return nnets;
}

void vcb_bus_build(const TCAnalysisCtx *ctx, const VCBModel *model, VCBBusData *out) {
	memset(out, 0, sizeof(*out));
	if (!ctx || ctx->side <= 0 || !ctx->classified.begin)
		return;
	const int side = ctx->side;
	const int n = side * side;
	out->board_side = side;

	/* 1. Flood-fill bus pixels into nets -- the SAME grouping the electrical merge
	 *    in vcb_model.c uses (vcb_bus_label), so the rendered bus net and the net
	 *    whose states the shader walks cannot disagree. This used to be a second,
	 *    subtly different flood here (same-ink only, no tunnel handling), which is
	 *    why our texture_buslut had a different net count from the original's. */
	int32_t *net = (int32_t *)malloc((size_t)n * sizeof(int32_t));
	if (!net)
		return;
	int nnets = (int)vcb_bus_label(ctx, net);
	if (nnets == 0) {
		free(net);
		return;
	}
	out->has_buses = 1;

	/* 2. Per net, gather the distinct entity indices of traces touching it
	 *    (ctx->bus_connections edges: packed fx|fy<<16|bx<<32|by<<48). */
	int32_t **ents = (int32_t **)calloc((size_t)nnets, sizeof(int32_t *));
	int32_t *ecnt = (int32_t *)calloc((size_t)nnets, sizeof(int32_t));
	int32_t *ecap = (int32_t *)calloc((size_t)nnets, sizeof(int32_t));
	const size_t nconn = ((const uint8_t *)ctx->bus_connections.end -
			(const uint8_t *)ctx->bus_connections.begin) / sizeof(void *);
	void *const *keys = (void *const *)ctx->bus_connections.begin;
	for (size_t k = 0; k < nconn; k++) {
		uint64_t key = (uint64_t)(uintptr_t)keys[k];
		int fx = (int)(uint16_t)key, fy = (int)(uint16_t)(key >> 16);
		int bx = (int)(uint16_t)(key >> 32), by = (int)(uint16_t)(key >> 48);
		if (bx < 0 || bx >= side || by < 0 || by >= side)
			continue;
		int bnet = net[by * side + bx];
		if (bnet < 0)
			continue;
		int fflat = fy * side + fx;
		int ent = (model && model->lut && fflat >= 0 && fflat < n) ? model->lut[fflat] : 0;
		if (ent <= 0)
			continue;
		int dup = 0;
		for (int j = 0; j < ecnt[bnet]; j++)
			if (ents[bnet][j] == ent) { dup = 1; break; }
		if (dup)
			continue;
		if (ecnt[bnet] == ecap[bnet]) {
			ecap[bnet] = ecap[bnet] ? ecap[bnet] * 2 : 4;
			ents[bnet] = (int32_t *)realloc(ents[bnet], (size_t)ecap[bnet] * sizeof(int32_t));
		}
		ents[bnet][ecnt[bnet]++] = ent;
	}

	/* 3. Flatten: each net's entities then a 0 terminator; record start index. */
	int32_t *netstart = (int32_t *)malloc((size_t)nnets * sizeof(int32_t));
	int total = 0;
	for (int i = 0; i < nnets; i++)
		total += ecnt[i] + 1;
	int32_t *flat = (int32_t *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int32_t));
	int pos = 0;
	for (int i = 0; i < nnets; i++) {
		netstart[i] = pos;
		for (int j = 0; j < ecnt[i]; j++)
			flat[pos++] = ents[i][j];
		flat[pos++] = 0; /* (0,0) terminator */
	}

	/* 4. Per board pixel: bus pixel -> its net's start index; else -1. */
	int32_t *bidx = (int32_t *)malloc((size_t)n * sizeof(int32_t));
	for (int i = 0; i < n; i++)
		bidx[i] = (net[i] >= 0) ? netstart[net[i]] : -1;

	out->busindex = bidx;
	out->entities = flat;
	out->entities_len = pos;

	for (int i = 0; i < nnets; i++)
		free(ents[i]);
	free(ents);
	free(ecnt);
	free(ecap);
	free(netstart);
	free(net);
}

void vcb_bus_free(VCBBusData *b) {
	if (!b)
		return;
	free(b->busindex);
	free(b->entities);
	memset(b, 0, sizeof(*b));
}
