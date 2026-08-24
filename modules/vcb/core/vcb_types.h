/* vcb_types.h -- Godot-free data types for the recovered VCB compiler pipeline.
 *
 * These mirror the structures recovered from vcb.exe (see the vcb-engine-recovery
 * repo, docs/compiler_pipeline.md). They are pure C with no Godot dependency so
 * the algorithm can be unit-tested standalone; the Godot module wraps them.
 */
#ifndef VCB_TYPES_H
#define VCB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A classified pixel flowing through the pipeline (12 bytes). */
typedef struct TCPixel {
	int32_t x;
	int32_t y;
	int32_t ink;
} TCPixel;

/* Layout-exact std::vector<T> header {begin,end,cap} (0x18 bytes). */
typedef struct TCVec {
	void *begin;
	void *end;
	void *cap;
} TCVec;

/* A component/gate entity: body pixels + bus-connection pixels (0x30). */
typedef struct TCEntity {
	TCVec body;
	TCVec connections;
} TCEntity;

/* A trace entity: I/O + connection pixels routed by ink (0x60). */
typedef struct TCTraceEntity {
	TCVec write;   /* ink 0xff */
	TCVec read;    /* ink 0xfe */
	TCVec ink_fd;  /* ink 0xfd */
	TCVec conn_ed; /* ink 0xed */
} TCTraceEntity;

/* The analysis context the pipeline passes operate on (compiler+0xe8 in vcb.exe).
 * Only the fields the recovered passes use are named. */
typedef struct TCAnalysisCtx {
	int32_t entity_count_a;
	int32_t entity_count_b;
	int32_t entitylist_sidelength;
	int32_t side;                 /* board width/height */
	TCVec   entities_a;           /* component entities (finalize) */
	TCVec   entities_b;           /* trace entities (finalize) */
	TCVec   errors;               /* {x,y,code} error records */
	TCVec   net_table;            /* bytes: global-net inks (wireless/mesh) */
	TCVec   classified;           /* 2 bytes/pixel: [ink, visited-marker] */
	TCVec   mesh_pixels;          /* all MESH pixels {x,y,0x66} */
	TCVec   raw_components;       /* std::vector<std::vector<TCPixel>> */
	TCVec   raw_traces;           /* std::vector<std::vector<TCPixel>> */
	TCVec   bus_connections;      /* deduped (from->bus) edges, 8-byte packed keys */
	int32_t stats[256];           /* per-category pixel histogram (cells, ctx+0x58) */
	int32_t entity_stats[256];    /* per-category entity histogram (entities, ctx+0x98) */
	int32_t processed;
} TCAnalysisCtx;

/* Classified buffer accessors: 2 bytes/pixel at flat index i = y*side + x. */
#define TC_CLASSIFIED_INK(buf, i)  ((buf)[(size_t)(i) * 2 + 0])
#define TC_CLASSIFIED_MARK(buf, i) ((buf)[(size_t)(i) * 2 + 1])

#ifdef __cplusplus
}
#endif

#endif /* VCB_TYPES_H */
