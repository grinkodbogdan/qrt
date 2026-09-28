#pragma once
#include "../kernel/rt.h"

typedef struct {
    u16 cp;
    i16 w, h;       /* bitmap size */
    i16 bx, by;     /* bitmap offset from pen x / line top */
    i16 adv64;      /* advance, 1/64 px */
    u32 off;        /* offset into coverage bits */
} glyph_t;

typedef struct {
    u16 size;
    i16 ascent, line;
    u16 count;
    const glyph_t *g;
    const u8 *bits;
} font_t;

/* size-ladders terminated by size == 0 */
extern const font_t font_regular[];
extern const font_t font_semibold[];
extern const font_t font_light[];
