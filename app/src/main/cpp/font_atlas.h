/* DeadSpeak Native Anti-Aliased Font Atlas (Auto-generated) */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    DS_FONT_ATLAS_WIDTH = 512,
    DS_FONT_ATLAS_HEIGHT = 256,
    DS_FONT_ATLAS_RAW_SIZE = 131072,
    DS_FONT_ATLAS_RLE_SIZE = 44674,
    DS_FONT_FIRST_CHAR = 32,
    DS_FONT_NUM_CHARS = 95,
};

#define DS_FONT_CAP_HEIGHT 20.0f

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    int8_t   ox;
    int8_t   oy;
    float    adv;
} DSFontGlyph;

extern const DSFontGlyph ds_font_glyphs[2][DS_FONT_NUM_CHARS];

void ds_font_unpack_atlas(uint8_t *dest, size_t dest_size);
