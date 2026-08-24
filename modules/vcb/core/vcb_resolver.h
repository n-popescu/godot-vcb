// vcb_resolver.h
//
// The EDITOR -> (ON, OFF) render resolver extracted from vcb.exe (RVA 0x3db0a0,
// switch body 0x3db300..0x3db81e). The resolver builds the ON texture (this+0x90)
// and OFF texture (this+0xa8): for each pixel it packs ecx=(R<<16)|(G<<8)|B and a
// compiler-lowered switch yields r8d = ON colour, r9d = OFF colour.
//
// This is that switch as a data table, recovered by emulating it for every
// palette colour in constants.gd (scripts/extract_color_resolver.py); the emulated
// output matches constants.gd exactly for all 51 circuit inks (checked by
// tools/color_resolver_check.c). Unrecognized colours resolve to black (0,0).
//
// Together with the classifier (vcb_classifier), this is the colour
// data the 16->64 trace mod must extend. See docs/color_handling_and_mod.md.

#ifndef VCB_RESOLVER_H
#define VCB_RESOLVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TCColorEntry {
    uint32_t editor;   // packed (R<<16)|(G<<8)|B EDITOR colour
    uint32_t on;       // lit colour written to the ON texture
    uint32_t off;      // unlit colour written to the OFF texture
    const char* name;  // constants.gd ID
} TCColorEntry;

extern const TCColorEntry TC_COLOR_TABLE[];
extern const size_t       TC_COLOR_TABLE_LEN;

// Resolve EDITOR -> ON/OFF. Returns 1 if recognized (and fills *on/*off),
// 0 otherwise (ON=OFF=0, i.e. black, matching the binary's default case).
int TC_resolve_color(uint32_t editor, uint32_t* on, uint32_t* off);

#ifdef __cplusplus
}
#endif

#endif // VCB_RESOLVER_H
