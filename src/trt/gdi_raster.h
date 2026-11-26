// tiramisu/src/trt/gdi_raster.h
// A real GDI raster surface, one per DC record.
//
// Until now every drawing entry point in bridge_gdi.cpp was honest about
// drawing nothing ("glyphs not drawn: fail-visible"). A desktop application
// paints through GetDC + TextOut/BitBlt/Rectangle, so "boots and pumps" without
// pixels is not desktop support. This module is the missing output: a per-DC
// 32-bit XRGB framebuffer with a clip rectangle, an embedded 8x16 coverage
// font (gdi_font8x16.h), solid fills, lines, ellipses, polygons, text with
// real BkMode/colour/alignment, ROP-correct BitBlt/StretchBlt, DIB blits and
// a BMP dump so the result can be inspected offline.
//
// It is deliberately NOT a compositor: it rasterizes each DC, which is what
// every GDI painting path actually writes to. Dumps (TIRAMISU_GDI_DUMP=<dir>)
// are how the console/offline harness shows the pixels.
#ifndef TIRAMISU_GDI_RASTER_H
#define TIRAMISU_GDI_RASTER_H

#include "trt.h"

namespace trt_raster {

struct surface {
  void* hdc;
  i32 w, h;
  u32* px;          // 0x00RRGGBB, row-major
  u8* mask;         // optional 1-byte-per-pixel visibility mask (path clip)
  i32 clip_l, clip_t, clip_r, clip_b;
  u64 last_dump_ms;
  bool used;
};

// Text attributes are supplied by bridge_gdi from the DC record.
struct text_style {
  u32 fg;            // COLORREF 0x00BBGGRR
  u32 bg;            // COLORREF
  i32 bk_mode;       // 1 = TRANSPARENT, 2 = OPAQUE
  u32 align;         // TA_* flags
  i32 height;        // cell height (TEXTMETRIC tmHeight)
  i32 advance;       // default per-character advance
  i32 ascent;        // cell ascent (TA_BASELINE placement)
  bool italic;
  bool underline;
  bool strikeout;
  const i32* dx;     // optional cumulative advances, n entries
  u8 alpha = 255;    // GDI+ text: per-call source alpha (255 = opaque GDI text)
};

// Surface lifecycle. get() creates (or resizes) the surface for hdc.
surface* get(void* hdc, i32 w, i32 h);
void release(void* hdc);
bool valid(void* hdc);
i32 width(void* hdc);
i32 height(void* hdc);

// Clip rectangle (device units). set_clip intersects; clip_reset restores the
// full surface. clip_box() reports the current rectangle.
bool clip_box(void* hdc, i32* l, i32* t, i32* r, i32* b);
bool intersect_clip(void* hdc, i32 l, i32 t, i32 r, i32 b, bool* empty);
bool exclude_clip(void* hdc, i32 l, i32 t, i32 r, i32 b);
void set_clip(void* hdc, i32 l, i32 t, i32 r, i32 b);

// Primitives (all clipped to the DC clip rectangle).
void fill(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref);
void frame(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref, i32 pen_width);
void line(void* hdc, i32 x0, i32 y0, i32 x1, i32 y1, u32 colorref, i32 pen_width);
void ellipse(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref, i32 pen_width,
             bool filled);
void polygon(void* hdc, const i32* xy, int n_points, u32 colorref,
             i32 pen_width, bool filled);

// Text. Returns the number of characters drawn, or -1 when hdc has no
// surface (bridge keeps the honest named failure). Unmappable code points
// (outside ASCII) draw a filled box so absence is visible in the pixels.
int text(void* hdc, i32 x, i32 y, const u16* s, int n, const text_style& st);

// Scroll a rectangle's pixels by (-dx, -dy) (ScrollWindowEx semantics): the
// pixels at (x+dx, y+dy) move to (x, y). Vacated pixels keep their old value.
void scroll(void* hdc, i32 dx, i32 dy, i32 l, i32 t, i32 r, i32 b);

// Bit blits. Returns false for an unsupported ROP (bridge logs the name).
bool blit(void* hdc_dst, i32 x, i32 y, i32 cx, i32 cy, void* hdc_src, i32 sx,
          i32 sy, u32 rop, u32 pat_colorref);
bool stretch(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src, i32 sx,
             i32 sy, i32 sw, i32 sh, u32 rop, u32 pat_colorref);

// Alpha-aware variants (GDI+ ARGB, msimg32 AlphaBlend). `argb` is
// 0xAARRGGBB; alpha 255 means a plain overwrite.
void fill_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb);
void frame_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb, i32 pen_width);
void line_argb(void* hdc, i32 x0, i32 y0, i32 x1, i32 y1, u32 argb,
               i32 pen_width);
void ellipse_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb,
                  i32 pen_width, bool filled);
void polygon_argb(void* hdc, const i32* xy, int n_points, u32 argb,
                  i32 pen_width, bool filled);

// GDI paths. A path is a list of subpaths: `xy` holds every vertex (x,y) and
// counts[i] is the vertex count of subpath i. Filling implicitly closes each
// subpath, like FillPath. fill_mode 1 = ALTERNATE (even-odd), 2 = WINDING.
void fill_path(void* hdc, const i32* xy, const int* counts, int n_sub,
               u32 argb, int fill_mode);
// Stroke every subpath as a polyline (close_all joins last->first).
void stroke_path(void* hdc, const i32* xy, const int* counts, int n_sub,
                 u32 argb, i32 pen_width, bool close_all);
// SelectClipPath: turn the path into a per-pixel clip mask. intersect keeps
// the existing clip (k_RGN_AND); otherwise the path replaces it.
bool clip_path(void* hdc, const i32* xy, const int* counts, int n_sub,
               int fill_mode, bool intersect);
// Drop the path clip mask (SelectClipRgn replaces the clip).
void clear_mask(void* hdc);

// RoundRect with real elliptical corners (ew/eh are the corner diameters).
void round_rect(void* hdc, i32 l, i32 t, i32 r, i32 b, i32 ew, i32 eh,
                u32 argb, i32 pen_width, bool filled);

// Arc / Pie / Chord (GDI geometry: the start/end points are radii of the
// bounding ellipse). mode 0 = outline arc, 1 = pie, 2 = chord.
void arc(void* hdc, i32 l, i32 t, i32 r, i32 b, i32 sx, i32 sy, i32 ex, i32 ey,
         u32 argb, i32 pen_width, int mode);

// Axis-aligned linear gradient between two ARGB stops (msimg32 GradientFill).
void gradient(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb0, u32 argb1,
              bool vertical);

// Source-over blend of one surface into another with a constant alpha
// (msimg32 AlphaBlend with AC_SRC_OVER). Nearest-neighbour scaling.
bool alpha_blend(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src,
                 i32 sx, i32 sy, i32 sw, i32 sh, u8 const_alpha);

// Stretch blit that skips source pixels equal to `key` (TransparentBlt).
bool key_blit(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src,
              i32 sx, i32 sy, i32 sw, i32 sh, u32 key_argb);

// Scaled blit of a caller-owned 32-bit BGRA buffer (GDI+ bitmap). Returns
// false when the buffer or rectangle is unusable.
bool image_bgra(void* hdc, i32 x, i32 y, i32 w, i32 h, const u32* bgra,
                i32 src_stride_px, i32 src_w, i32 src_h, u8 alpha);

// SetDIBitsToDevice / StretchDIBits. 1/8/16/24/32 bpp BI_RGB and 16/32 bpp
// BI_BITFIELDS are decoded; anything else returns false (named by the caller).
bool dib(void* hdc, i32 x, i32 y, i32 dw, i32 dh, const void* bits,
         const void* bmi, u32 color_use, i32 sx, i32 sy, i32 sw, i32 sh,
         u32 rop);

u32 get_pixel(void* hdc, i32 x, i32 y, bool* ok);
bool set_pixel(void* hdc, i32 x, i32 y, u32 colorref);

// Evidence helpers: a 24-bit BMP per surface (TIRAMISU_GDI_DUMP=<dir>) and
// a count of pixels differing from the surface background, so a test can
// assert "something was actually drawn".
bool dump_bmp(void* hdc, const char* path);
int dump_all(const char* dir);
i64 non_background_pixels(void* hdc, u32 background_colorref);

}  // namespace trt_raster

#endif
