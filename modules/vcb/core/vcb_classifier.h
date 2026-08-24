// vcb_classifier.h
//
// The color -> ink-code classifier extracted from TransistorCompiler's `prepare`
// pass (RVA 0x3dd0c0). `prepare` packs each pixel as ecx=(R<<16)|(G<<8)|B and runs
// a compiler-lowered balanced-search switch; this is that switch as a data table.
//
// Recovered/verified by emulating the switch for every palette color in
// vcb-original/src/singletons/constants.gd (scripts/extract_ink_table.py) and
// checked by tools/ink_table_check (make layout). See docs/classifier_ink_table.md.
//
// Extending this table (plus the render resolver at 0x3db0a0) + the ink-code space
// is what the 16->64 trace-color mod requires (docs/color_handling_and_mod.md).

#ifndef VCB_CLASSIFIER_H
#define VCB_CLASSIFIER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TCInkEntry {
    uint32_t editor_rgb;   // packed (R<<16)|(G<<8)|B EDITOR color
    uint8_t  ink;          // ink code written to the classified buffer (buf[i*2])
    uint8_t  category;     // category byte written at ctx+0xf8 (== GDScript STATSTYPE)
    const char* name;      // constants.gd ID
} TCInkEntry;

// Ink-code ranges (from the recovered table):
//   0x01..0x08  logic gates (BUFFER,AND,OR,XOR,NOT,NAND,NOR,XNOR)
//   0x09,0x0a   LATCH_ON / LATCH_OFF
//   0x0b        CLOCK        0x0c LED     0x0d/0x0e VMEM addr/content
//   0x0f        VINPUT       0x10 TIMER   0x11 BREAKPOINT   0x12 RANDOM
//   0x13..0x16  WIRELESS_0..3
//   0x64..0x66  CROSS / TUNNEL / MESH
//   0xe8..0xed  buses BUS_0..BUS_5
//   0xee..0xfd  traces (16: TRACE_GRAY..TRACE_PINK)
//   0xfe        READ         0xff WRITE
// Colors not in the table classify as ink 0 (non-conductive / decoration).

extern const TCInkEntry TC_INK_TABLE[];
extern const size_t     TC_INK_TABLE_LEN;

// Classify a packed EDITOR rgb -> ink code (0 if unrecognized). Optionally
// returns the category via *out_category.
uint8_t TC_classify_color(uint32_t editor_rgb, uint8_t* out_category);

// Map an ink code back to its stats category (== GDScript STATSTYPE); 0 if the
// ink is not in the table. Used by the entity stats histogram, whose entities
// are keyed by their body ink.
uint8_t TC_category_from_ink(uint8_t ink);

#ifdef __cplusplus
}
#endif

#endif // VCB_CLASSIFIER_H
