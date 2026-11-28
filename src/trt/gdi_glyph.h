// Host font provider for the guest's GDI glyph and font-face queries.
#ifndef TIRAMISU_GDI_GLYPH_H
#define TIRAMISU_GDI_GLYPH_H

#include "pe.h"

namespace trt_glyph {

constexpr u32 error = 0xffffffffu;
constexpr u32 logfontw_bytes = 92;

// Format selected by GetGlyphOutlineW. Native outlines use quadratic splines.
enum format : u32 {
  metrics = 0, bitmap = 1, native = 2, bezier = 3,
  gray2 = 4, gray4 = 5, gray8 = 6, glyph_index = 0x80, unhinted = 0x100,
};

struct glyph_metrics {
  u32 width, height;
  i32 origin_x, origin_y;
  i16 advance_x, advance_y;
};
static_assert(sizeof(glyph_metrics) == 20);

struct fixed {
  u16 fraction;
  i16 integer;
};
struct matrix {
  fixed m11, m12, m21, m22;
};
static_assert(sizeof(matrix) == 16);

// Convert 8-bit coverage to GDI's DWORD-aligned, MSB-first mono or gray rows.
// A null destination queries the number of bytes. Errors leave buffers intact.
u32 pack_bitmap(u32 format, u32 width, u32 height, const u8* alpha,
                u32 alpha_stride, u32 capacity, u8* dest);

u32 outline(const void* logfont, u32 character, u32 format,
            glyph_metrics* result, u32 capacity, void* buffer, const matrix* mat);
u32 indices(const void* logfont, const u16* text, u32 count, u16* result,
            bool mark_missing);

}  // namespace trt_glyph

#endif
