// vcb_classifier.c
//
// Data table + lookup for the color->ink classifier (see the header and
// docs/classifier_ink_table.md). Generated from the `prepare` switch by
// scripts/extract_ink_table.py and verified against the binary.

#include "vcb_classifier.h"

const TCInkEntry TC_INK_TABLE[] = {
    { 0x92ff63, 0x01,  8, "BUFFER" },
    { 0xffc663, 0x02,  9, "AND" },
    { 0x63f2ff, 0x03, 10, "OR" },
    { 0xae74ff, 0x04, 11, "XOR" },
    { 0xff628a, 0x05, 12, "NOT" },
    { 0xffa200, 0x06, 13, "NAND" },
    { 0x30d9ff, 0x07, 14, "NOR" },
    { 0xa600ff, 0x08, 15, "XNOR" },
    { 0x63ff9f, 0x09, 16, "LATCH_ON" },
    { 0x384d47, 0x0a, 17, "LATCH_OFF" },
    { 0xff0041, 0x0b, 18, "CLOCK" },
    { 0xffffff, 0x0c, 19, "LED" },
    { 0xa3ff61, 0x0d, 27, "VMEM_LATCH_ADDRESS" },
    { 0x61ff61, 0x0e, 28, "VMEM_LATCH_CONTENT" },
    { 0xc0ff61, 0x0f, 29, "VINPUT_COMPONENT" },
    { 0xff6700, 0x10, 20, "TIMER" },
    { 0xe00000, 0x11, 21, "BREAKPOINT" },
    { 0xe5ff00, 0x12, 22, "RANDOM" },
    { 0xff00bf, 0x13, 23, "WIRELESS_0" },
    { 0xff00af, 0x14, 24, "WIRELESS_1" },
    { 0xff009f, 0x15, 25, "WIRELESS_2" },
    { 0xff008f, 0x16, 26, "WIRELESS_3" },
    { 0x66788e, 0x64,  1, "CROSS" },
    { 0x535572, 0x65,  2, "TUNNEL" },
    { 0x646a57, 0x66,  3, "MESH" },
    { 0x7a2f24, 0xe8,  4, "BUS_0" },
    { 0x3e7a24, 0xe9,  4, "BUS_1" },
    { 0x24417a, 0xea,  4, "BUS_2" },
    { 0x25627a, 0xeb,  4, "BUS_3" },
    { 0x7a2d66, 0xec,  4, "BUS_4" },
    { 0x7a7024, 0xed,  4, "BUS_5" },
    { 0x2a3541, 0xee,  7, "TRACE_GRAY" },
    { 0x9fa8ae, 0xef,  7, "TRACE_WHITE" },
    { 0xa1555e, 0xf0,  7, "TRACE_RED" },
    { 0xa16c56, 0xf1,  7, "TRACE_ORANGE" },
    { 0xa18556, 0xf2,  7, "TRACE_YELLOW_WARM" },
    { 0xa19856, 0xf3,  7, "TRACE_YELLOW_COLD" },
    { 0x99a156, 0xf4,  7, "TRACE_LEMON" },
    { 0x88a156, 0xf5,  7, "TRACE_GREEN_WARM" },
    { 0x6ca156, 0xf6,  7, "TRACE_GREEN_COLD" },
    { 0x56a18d, 0xf7,  7, "TRACE_TURQUOISE" },
    { 0x5693a1, 0xf8,  7, "TRACE_BLUE_LIGHT" },
    { 0x567ba1, 0xf9,  7, "TRACE_BLUE" },
    { 0x5662a1, 0xfa,  7, "TRACE_BLUE_DARK" },
    { 0x6656a1, 0xfb,  7, "TRACE_PURPLE" },
    { 0x8756a1, 0xfc,  7, "TRACE_VIOLET" },
    { 0xa15597, 0xfd,  7, "TRACE_PINK" },
    { 0x2e475d, 0xfe,  6, "READ" },
    { 0x4d383e, 0xff,  5, "WRITE" },
};

const size_t TC_INK_TABLE_LEN = sizeof(TC_INK_TABLE) / sizeof(TC_INK_TABLE[0]);

uint8_t TC_classify_color(uint32_t editor_rgb, uint8_t* out_category) {
    for (size_t i = 0; i < TC_INK_TABLE_LEN; i++) {
        if (TC_INK_TABLE[i].editor_rgb == editor_rgb) {
            if (out_category) *out_category = TC_INK_TABLE[i].category;
            return TC_INK_TABLE[i].ink;
        }
    }
    if (out_category) *out_category = 0;
    return 0;  // unrecognized -> non-conductive
}

uint8_t TC_category_from_ink(uint8_t ink) {
    if (ink == 0) return 0;
    for (size_t i = 0; i < TC_INK_TABLE_LEN; i++) {
        if (TC_INK_TABLE[i].ink == ink)
            return TC_INK_TABLE[i].category;
    }
    return 0;
}
