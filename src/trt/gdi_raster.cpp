// tiramisu/src/trt/gdi_raster.cpp
// Per-DC raster surface + embedded glyph rasterizer. See gdi_raster.h.
//
// Everything here is device units and clipped to the DC's clip rectangle.
// The surface exists so a desktop app's painting produces pixels at all; it
// is not a window compositor (the console host composites D3D), so the proof
// that painting happened is a BMP dump plus pixel counts in the gate.
#include "gdi_raster.h"

#include "gdi_font8x16.h"
#include "thunk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#include <algorithm>

#include <mutex>
#include <vector>

namespace trt_raster {

// Defined at the bottom of this file; declared here so the auto-dump path can
// use it while it already holds the surface mutex.
bool dump_bmp_impl(surface* s, const char* path);

namespace {

constexpr int kMaxSurfaces = 16;
constexpr i32 kMaxDim = 4096;
constexpr i32 kDefaultW = 1920;
constexpr i32 kDefaultH = 1080;

surface g_surfaces[kMaxSurfaces];
std::mutex g_mu;

u64 now_ms() {
  using namespace std::chrono;
  return static_cast<u64>(duration_cast<milliseconds>(
      steady_clock::now().time_since_epoch()).count());
}

// COLORREF 0x00BBGGRR <-> surface pixel 0x00RRGGBB.
inline u32 px_of(u32 colorref) {
  return ((colorref & 0xFFu) << 16) | (colorref & 0x00FF00u) |
         ((colorref >> 16) & 0xFFu);
}
inline u32 colorref_of(u32 px) { return px_of(px); }

inline u32 blend(u32 dst, u32 src, u32 a) {
  const u32 dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
  const u32 sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
  const u32 ia = 255u - a;
  const u32 r = (sr * a + dr * ia) / 255u;
  const u32 g = (sg * a + dg * ia) / 255u;
  const u32 b = (sb * a + db * ia) / 255u;
  return (r << 16) | (g << 8) | b;
}

void put(surface* s, i32 x, i32 y, u32 px) {
  if (!s || !s->px) return;
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
  if (x < s->clip_l || y < s->clip_t || x >= s->clip_r || y >= s->clip_b) return;
  const size_t idx = (size_t)y * (size_t)s->w + (size_t)x;
  if (s->mask && !s->mask[idx]) return;
  s->px[idx] = px;
}

u32 get_clipped(surface* s, i32 x, i32 y) {
  if (!s || !s->px) return 0;
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
  return s->px[(size_t)y * (size_t)s->w + (size_t)x];
}

// ROP3 subset that desktop painting actually uses. Returns false for a ROP
// this surface does not implement so the caller can name it.
bool apply_rop(u32 rop, u32 d, u32 s, u32 p, u32* out) {
  switch (rop) {
    case 0x00CC0020u: *out = s; return true;                          // SRCCOPY
    case 0x00EE0086u: *out = s | d; return true;                      // SRCPAINT
    case 0x008800C6u: *out = s & d; return true;                      // SRCAND
    case 0x00660046u: *out = s ^ d; return true;                      // SRCINVERT
    case 0x00330008u: *out = ~s; return true;                         // NOTSRCCOPY
    case 0x00550009u: *out = ~d; return true;                         // DSTINVERT
    case 0x00000042u: *out = 0; return true;                          // BLACKNESS
    case 0x00FF0062u: *out = 0x00FFFFFFu; return true;                // WHITENESS
    case 0x00F00021u: *out = p; return true;                          // PATCOPY
    case 0x005A0049u: *out = p ^ d; return true;                      // PATINVERT
    case 0x00BB0226u: *out = (~s) | d; return true;                   // MERGEPAINT
    default: return false;
  }
}

void maybe_dump(surface* s) {
  const char* dir = getenv("TIRAMISU_GDI_DUMP");
  if (!dir || !*dir) return;
  const u64 now = now_ms();
  if (s->last_dump_ms && now - s->last_dump_ms < 500) return;
  s->last_dump_ms = now;
  char path[512];
  const int slot = (int)(s - g_surfaces) + 1;
  snprintf(path, sizeof(path), "%s/gdi-dc%d.bmp", dir, slot);
  dump_bmp_impl(s, path);  // caller already holds g_mu
}

surface* ensure_locked(void* hdc, i32 w, i32 h, bool* created) {
  if (created) *created = false;
  if (!hdc) return nullptr;
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) return &g_surfaces[i];
  }
  if (w <= 0) w = kDefaultW;
  if (h <= 0) h = kDefaultH;
  if (w > kMaxDim) w = kMaxDim;
  if (h > kMaxDim) h = kMaxDim;
  int slot = -1;
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (!g_surfaces[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    // Recycle the least recently dumped surface rather than failing to draw.
    u64 oldest = (u64)-1;
    for (int i = 0; i < kMaxSurfaces; ++i) {
      if (g_surfaces[i].last_dump_ms <= oldest) {
        oldest = g_surfaces[i].last_dump_ms;
        slot = i;
      }
    }
    free(g_surfaces[slot].px);
    free(g_surfaces[slot].mask);
    memset(&g_surfaces[slot], 0, sizeof(g_surfaces[slot]));
  }
  surface* s = &g_surfaces[slot];
  s->px = (u32*)calloc((size_t)w * (size_t)h, sizeof(u32));
  if (!s->px) return nullptr;
  s->hdc = hdc;
  s->w = w;
  s->h = h;
  s->clip_l = 0;
  s->clip_t = 0;
  s->clip_r = w;
  s->clip_b = h;
  s->used = true;
  if (created) *created = true;
  {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: raster surface created for DC %p (%dx%d, %zu KiB)", hdc,
             w, h, ((size_t)w * (size_t)h * 4) / 1024);
    trt_core_log(line);
  }
  return s;
}

// ---- DIB decoding -------------------------------------------------------
struct dib_info {
  i32 w, h;
  u16 bpp;
  u32 compression;
  u32 rmask, gmask, bmask;
  const u8* base;
  const u32* palette;  // COLORREF entries
  int palette_n;
  size_t stride;
  bool topdown;
};

bool parse_dib(const void* bits, const void* bmi, u32 color_use, dib_info* d) {
  if (!bits || !bmi || !d) return false;
  const u8* b = (const u8*)bmi;
  const u32 hdr_size = *(const u32*)b;
  if (hdr_size < 40) return false;
  d->w = *(const i32*)(b + 4);
  d->h = *(const i32*)(b + 8);
  d->bpp = *(const u16*)(b + 14);
  d->compression = *(const u32*)(b + 16);
  d->topdown = d->h < 0;
  if (d->h < 0) d->h = -d->h;
  if (d->w <= 0 || d->h <= 0) return false;
  if (d->w > kMaxDim || d->h > kMaxDim) return false;

  const u8* after = b + hdr_size;
  d->rmask = d->gmask = d->bmask = 0;
  if (d->compression == 3) {  // BI_BITFIELDS
    if (d->bpp != 16 && d->bpp != 32) return false;
    d->rmask = *(const u32*)(after + 0);
    d->gmask = *(const u32*)(after + 4);
    d->bmask = *(const u32*)(after + 8);
    after += 12;
  } else if (d->compression != 0) {  // BI_RLE*/JPEG/PNG: not decoded
    return false;
  }
  if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 &&
      d->bpp != 24 && d->bpp != 32)
    return false;

  d->palette_n = 0;
  d->palette = nullptr;
  if (d->bpp <= 8) {
    u32 used = *(const u32*)(b + 32);  // biClrUsed
    int n = used ? (int)used : (1 << d->bpp);
    if (n > 256) n = 256;
    d->palette_n = n;
    d->palette = (const u32*)after;  // BGRA0 entries == COLORREF
  }
  d->base = (const u8*)bits;
  d->stride = ((((size_t)d->w * d->bpp) + 31) / 32) * 4;
  (void)color_use;
  return true;
}

inline int mask_shift(u32 mask) {
  if (!mask) return -1;
  int s = 0;
  while (!(mask & 1u) && s < 32) { mask >>= 1; ++s; }
  return s;
}
inline u32 mask_width(u32 mask) {
  int n = 0;
  while (mask & 1u) { mask >>= 1; ++n; }
  return (u32)n;
}
inline u32 expand_bits(u32 v, u32 width) {
  if (width == 0) return 0;
  if (width >= 8) return (v >> (width - 8)) & 0xFFu;
  return (v * 255u) / ((1u << width) - 1u);
}

u32 dib_pixel(const dib_info* d, i32 x, i32 y) {
  const i32 row = d->topdown ? y : (d->h - 1 - y);
  const u8* p = d->base + (size_t)row * d->stride;
  switch (d->bpp) {
    case 4: {
      const u8 v = p[x / 2];
      const u32 idx = (x & 1) ? (v & 0x0Fu) : (v >> 4);
      if (d->palette && (int)idx < d->palette_n)
        return px_of(d->palette[idx]) & 0xFFFFFFu;
      return 0;
    }
    case 1: {
      const u8 v = p[x / 8];
      const u32 idx = (v >> (7 - (x & 7))) & 1u;
      if (d->palette && (int)idx < d->palette_n)
        return px_of(d->palette[idx]) & 0xFFFFFFu;
      return idx ? 0xFFFFFFu : 0;
    }
    case 8: {
      const u32 idx = p[x];
      if (d->palette && (int)idx < d->palette_n)
        return px_of(d->palette[idx]) & 0xFFFFFFu;
      return (idx << 16) | (idx << 8) | idx;
    }
    case 16: {
      u32 v;
      memcpy(&v, p + (size_t)x * 2, 2);
      const u32 rmask = d->rmask ? d->rmask : 0x7C00u;
      const u32 gmask = d->gmask ? d->gmask : 0x03E0u;
      const u32 bmask = d->bmask ? d->bmask : 0x001Fu;
      const u32 r = expand_bits((v & rmask) >> mask_shift(rmask),
                                mask_width(rmask));
      const u32 g = expand_bits((v & gmask) >> mask_shift(gmask),
                                mask_width(gmask));
      const u32 b = expand_bits((v & bmask) >> mask_shift(bmask),
                                mask_width(bmask));
      return (r << 16) | (g << 8) | b;
    }
    case 24: {
      const u8* q = p + (size_t)x * 3;
      return ((u32)q[2] << 16) | ((u32)q[1] << 8) | (u32)q[0];
    }
    case 32: {
      u32 v;
      memcpy(&v, p + (size_t)x * 4, 4);
      if (d->compression == 3 && d->rmask) {
        const u32 r = expand_bits((v & d->rmask) >> mask_shift(d->rmask),
                                  mask_width(d->rmask));
        const u32 g = expand_bits((v & d->gmask) >> mask_shift(d->gmask),
                                  mask_width(d->gmask));
        const u32 b = expand_bits((v & d->bmask) >> mask_shift(d->bmask),
                                  mask_width(d->bmask));
        return (r << 16) | (g << 8) | b;
      }
      return v & 0x00FFFFFFu;  // BI_RGB 32bpp: 0x00RRGGBB
    }
    default:
      return 0;
  }
}

}  // namespace

// ---- lifecycle ----------------------------------------------------------

surface* get(void* hdc, i32 w, i32 h) {
  std::lock_guard<std::mutex> lk(g_mu);
  return ensure_locked(hdc, w, h, nullptr);
}

void release(void* hdc) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      // Final dump before the pixels go away: this is the image the app
      // painted, and the only offline evidence a DC ever drew anything.
      const char* dir = getenv("TIRAMISU_GDI_DUMP");
      if (dir && *dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/gdi-dc%d.bmp", dir, i + 1);
        dump_bmp_impl(&g_surfaces[i], path);
      }
      free(g_surfaces[i].px);
      free(g_surfaces[i].mask);
      memset(&g_surfaces[i], 0, sizeof(g_surfaces[i]));
      return;
    }
  }
}

bool valid(void* hdc) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i)
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) return true;
  return false;
}

i32 width(void* hdc) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i)
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) return g_surfaces[i].w;
  return 0;
}

i32 height(void* hdc) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i)
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) return g_surfaces[i].h;
  return 0;
}

bool clip_box(void* hdc, i32* l, i32* t, i32* r, i32* b) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      const surface& s = g_surfaces[i];
      if (l) *l = s.clip_l;
      if (t) *t = s.clip_t;
      if (r) *r = s.clip_r;
      if (b) *b = s.clip_b;
      return true;
    }
  }
  return false;
}

bool intersect_clip(void* hdc, i32 l, i32 t, i32 r, i32 b2, bool* empty) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      surface& s = g_surfaces[i];
      if (l > s.clip_l) s.clip_l = l;
      if (t > s.clip_t) s.clip_t = t;
      if (r < s.clip_r) s.clip_r = r;
      if (b2 < s.clip_b) s.clip_b = b2;
      const bool e = s.clip_r <= s.clip_l || s.clip_b <= s.clip_t;
      if (e) { s.clip_r = s.clip_l; s.clip_b = s.clip_t; }
      if (empty) *empty = e;
      return true;
    }
  }
  if (empty) *empty = true;
  return false;
}

void set_clip(void* hdc, i32 l, i32 t, i32 r, i32 b) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  s->clip_l = l;
  s->clip_t = t;
  s->clip_r = r;
  s->clip_b = b;
}

bool exclude_clip(void* hdc, i32 l, i32 t, i32 r, i32 b2) {
  // Single-rectangle clip model: excluding a rectangle that covers the whole
  // clip empties it, otherwise the clip is left as-is (named by the caller).
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      surface& s = g_surfaces[i];
      if (l <= s.clip_l && t <= s.clip_t && r >= s.clip_r && b2 >= s.clip_b) {
        s.clip_r = s.clip_l;
        s.clip_b = s.clip_t;
      }
      return true;
    }
  }
  return false;
}


// ---- primitives ---------------------------------------------------------

namespace {

// Every primitive below paints through put_rgb so one code path serves both
// opaque GDI colours and ARGB (GDI+) colours. a == 255 keeps the fast path.
inline void put_rgb(surface* s, i32 x, i32 y, u32 rgb, u32 a) {
  if (a == 0) return;
  if (a >= 255) { put(s, x, y, rgb); return; }
  if (!s || !s->px) return;
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
  if (x < s->clip_l || y < s->clip_t || x >= s->clip_r || y >= s->clip_b) return;
  u32* p = s->px + (size_t)y * (size_t)s->w + (size_t)x;
  if (s->mask && !s->mask[(size_t)y * (size_t)s->w + (size_t)x]) return;
  *p = blend(*p, rgb, a);
}

void fill_i(surface* s, i32 l, i32 t, i32 r, i32 b, u32 rgb, u32 a) {
  if (!s) return;
  if (l < s->clip_l) l = s->clip_l;
  if (t < s->clip_t) t = s->clip_t;
  if (r > s->clip_r) r = s->clip_r;
  if (b > s->clip_b) b = s->clip_b;
  if (r <= l || b <= t) return;
  if (s->mask) {  // a path clip decides per pixel: no row fast path
    for (i32 y = t; y < b; ++y)
      for (i32 x = l; x < r; ++x) put_rgb(s, x, y, rgb, a);
    return;
  }
  for (i32 y = t; y < b; ++y) {
    u32* row = s->px + (size_t)y * (size_t)s->w;
    if (a >= 255) {
      for (i32 x = l; x < r; ++x) row[x] = rgb;
    } else {
      for (i32 x = l; x < r; ++x) row[x] = blend(row[x], rgb, a);
    }
  }
}

void frame_i(surface* s, i32 l, i32 t, i32 r, i32 b, u32 rgb, u32 a,
             i32 pen_width) {
  if (!s) return;
  if (pen_width < 1) pen_width = 1;
  // GDI's Rectangle() outlines the right/bottom edges inside the rect.
  for (i32 y = t; y < t + pen_width; ++y)
    for (i32 x = l; x < r; ++x) put_rgb(s, x, y, rgb, a);
  for (i32 y = b - pen_width; y < b; ++y)
    for (i32 x = l; x < r; ++x) put_rgb(s, x, y, rgb, a);
  for (i32 x = l; x < l + pen_width; ++x)
    for (i32 y = t; y < b; ++y) put_rgb(s, x, y, rgb, a);
  for (i32 x = r - pen_width; x < r; ++x)
    for (i32 y = t; y < b; ++y) put_rgb(s, x, y, rgb, a);
}

void line_i(surface* s, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb, u32 a,
            i32 pen_width) {
  if (!s) return;
  if (pen_width < 1) pen_width = 1;
  const i32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
  const i32 dy = y1 > y0 ? y1 - y0 : y0 - y1;
  const i32 sx = x0 < x1 ? 1 : -1;
  const i32 sy = y0 < y1 ? 1 : -1;
  i32 err = dx - dy;
  i32 x = x0, y = y0;
  for (;;) {
    for (i32 oy = 0; oy < pen_width; ++oy)
      for (i32 ox = 0; ox < pen_width; ++ox)
        put_rgb(s, x + ox, y + oy, rgb, a);
    if (x == x1 && y == y1) break;
    const i32 e2 = err * 2;
    if (e2 > -dy) { err -= dy; x += sx; }
    if (e2 < dx) { err += dx; y += sy; }
  }
}

void ellipse_i(surface* s, i32 l, i32 t, i32 r, i32 b, u32 rgb, u32 a,
               i32 pen_width, bool filled) {
  if (!s) return;
  if (pen_width < 1) pen_width = 1;
  const float cx = ((float)l + (float)r) / 2.0f;
  const float cy = ((float)t + (float)b) / 2.0f;
  const float rx = ((float)r - (float)l) / 2.0f;
  const float ry = ((float)b - (float)t) / 2.0f;
  if (rx <= 0.0f || ry <= 0.0f) return;
  const float outer = 1.0f;
  const float inner = 1.0f - (float)pen_width * 2.0f / (rx < ry ? rx : ry);
  for (i32 y = (i32)cy - (i32)ry; y <= (i32)cy + (i32)ry; ++y) {
    for (i32 x = (i32)cx - (i32)rx; x <= (i32)cx + (i32)rx; ++x) {
      const float nx = ((float)x + 0.5f - cx) / rx;
      const float ny = ((float)y + 0.5f - cy) / ry;
      const float d = nx * nx + ny * ny;
      if (d > outer) continue;
      if (!filled && d < inner) continue;
      put_rgb(s, x, y, rgb, a);
    }
  }
}

void polygon_i(surface* s, const i32* xy, int n, u32 rgb, u32 a,
               i32 pen_width, bool filled) {
  if (!s || !xy || n < 2) return;
  if (filled && n >= 3) {
    i32 min_y = xy[1], max_y = xy[1];
    for (int i = 1; i < n; ++i) {
      if (xy[i * 2 + 1] < min_y) min_y = xy[i * 2 + 1];
      if (xy[i * 2 + 1] > max_y) max_y = xy[i * 2 + 1];
    }
    if (min_y < s->clip_t) min_y = s->clip_t;
    if (max_y > s->clip_b - 1) max_y = s->clip_b - 1;
    i32* xs = (i32*)malloc(sizeof(i32) * (size_t)n);
    if (!xs) return;
    for (i32 y = min_y; y <= max_y; ++y) {
      int nx = 0;
      for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const i32 y0 = xy[i * 2 + 1], y1 = xy[j * 2 + 1];
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
          const i32 x0 = xy[i * 2], x1 = xy[j * 2];
          const float tt = (float)(y - y0) / (float)(y1 - y0);
          xs[nx++] = x0 + (i32)((float)(x1 - x0) * tt);
        }
      }
      for (int p = 0; p < nx; ++p)
        for (int c = p + 1; c < nx; ++c)
          if (xs[c] < xs[p]) { const i32 tmp = xs[p]; xs[p] = xs[c]; xs[c] = tmp; }
      for (int p = 0; p + 1 < nx; p += 2)
        for (i32 x = xs[p]; x <= xs[p + 1]; ++x) put_rgb(s, x, y, rgb, a);
    }
    free(xs);
  }
  // Outline (always, like Polygon with a valid pen).
  const i32 pw = pen_width < 1 ? 1 : pen_width;
  for (int i = 0; i < n; ++i) {
    const int j = (i + 1) % n;
    i32 x0 = xy[i * 2], y0 = xy[i * 2 + 1];
    const i32 x1 = xy[j * 2], y1 = xy[j * 2 + 1];
    const i32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const i32 dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const i32 sx = x0 < x1 ? 1 : -1;
    const i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx - dy;
    for (;;) {
      for (i32 oy = 0; oy < pw; ++oy)
        for (i32 ox = 0; ox < pw; ++ox)
          put_rgb(s, x0 + ox, y0 + oy, rgb, a);
      if (x0 == x1 && y0 == y1) break;
      const i32 e2 = err * 2;
      if (e2 > -dy) { err -= dy; x0 += sx; }
      if (e2 < dx) { err += dx; y0 += sy; }
    }
  }
}

// ---- path rasterization -------------------------------------------------
// GDI paths arrive as subpaths of vertices. Every subpath is implicitly closed
// for filling (FillPath semantics), and the fill rule is ALTERNATE (even-odd)
// or WINDING, exactly like SetPolyFillMode.

struct path_edge {
  float x0, y0, x1, y1;
};

void collect_path_edges(const i32* xy, const int* counts, int n_sub,
                        std::vector<path_edge>& out) {
  int base = 0;
  for (int s = 0; s < n_sub; ++s) {
    const int n = counts[s];
    if (n >= 2) {
      for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        path_edge e;
        e.x0 = (float)xy[(base + i) * 2];
        e.y0 = (float)xy[(base + i) * 2 + 1];
        e.x1 = (float)xy[(base + j) * 2];
        e.y1 = (float)xy[(base + j) * 2 + 1];
        if (e.y0 != e.y1) out.push_back(e);
      }
    }
    base += n;
  }
}

// One byte per pixel: 1 = inside the filled path.
void rasterize_path_mask(const i32* xy, const int* counts, int n_sub, i32 w,
                         i32 h, int fill_mode, u8* mask) {
  if (!mask) return;
  std::vector<path_edge> edges;
  collect_path_edges(xy, counts, n_sub, edges);
  if (edges.empty()) return;
  struct crossing {
    float x;
    int dir;
  };
  std::vector<crossing> cs;
  cs.reserve(edges.size());
  for (i32 y = 0; y < h; ++y) {
    const float sy = (float)y + 0.5f;
    cs.clear();
    for (const path_edge& e : edges) {
      const bool down = e.y1 > e.y0;
      const float lo = down ? e.y0 : e.y1;
      const float hi = down ? e.y1 : e.y0;
      if (sy < lo || sy >= hi) continue;
      crossing c;
      c.x = e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0);
      c.dir = down ? 1 : -1;
      cs.push_back(c);
    }
    if (cs.size() < 2) continue;
    std::sort(cs.begin(), cs.end(),
              [](const crossing& a, const crossing& b) { return a.x < b.x; });
    u8* row = mask + (size_t)y * (size_t)w;
    auto span = [&](float xa, float xb) {
      if (xb <= xa) return;
      i32 x0 = (i32)ceilf(xa - 0.5f);
      i32 x1 = (i32)ceilf(xb - 0.5f);
      if (x0 < 0) x0 = 0;
      if (x1 > w) x1 = w;
      for (i32 x = x0; x < x1; ++x) row[x] = 1;
    };
    if (fill_mode == 2) {
      int wind = 0;
      for (size_t i = 0; i + 1 < cs.size(); ++i) {
        wind += cs[i].dir;
        if (wind != 0) span(cs[i].x, cs[i + 1].x);
      }
    } else {
      for (size_t i = 0; i + 1 < cs.size(); i += 2) span(cs[i].x, cs[i + 1].x);
    }
  }
}

inline u32 rgb_of(u32 argb) { return argb & 0xFFFFFFu; }
inline u32 alpha_of(u32 argb) { return (argb >> 24) & 0xFFu; }

}  // namespace

void fill(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  fill_i(s, l, t, r, b, px_of(colorref) & 0xFFFFFFu, 255);
  maybe_dump(s);
}

void frame(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref, i32 pen_width) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  frame_i(s, l, t, r, b, px_of(colorref) & 0xFFFFFFu, 255, pen_width);
  maybe_dump(s);
}

void line(void* hdc, i32 x0, i32 y0, i32 x1, i32 y1, u32 colorref,
          i32 pen_width) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  line_i(s, x0, y0, x1, y1, px_of(colorref) & 0xFFFFFFu, 255, pen_width);
  maybe_dump(s);
}

void ellipse(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 colorref,
             i32 pen_width, bool filled) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  ellipse_i(s, l, t, r, b, px_of(colorref) & 0xFFFFFFu, 255, pen_width, filled);
  maybe_dump(s);
}

void polygon(void* hdc, const i32* xy, int n, u32 colorref, i32 pen_width,
             bool filled) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  polygon_i(s, xy, n, px_of(colorref) & 0xFFFFFFu, 255, pen_width, filled);
  maybe_dump(s);
}

// ---- ARGB primitives (GDI+) --------------------------------------------

void fill_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  fill_i(s, l, t, r, b, rgb_of(argb), alpha_of(argb));
  maybe_dump(s);
}

void frame_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb,
                i32 pen_width) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  frame_i(s, l, t, r, b, rgb_of(argb), alpha_of(argb), pen_width);
  maybe_dump(s);
}

void line_argb(void* hdc, i32 x0, i32 y0, i32 x1, i32 y1, u32 argb,
               i32 pen_width) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  line_i(s, x0, y0, x1, y1, rgb_of(argb), alpha_of(argb), pen_width);
  maybe_dump(s);
}

void ellipse_argb(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb,
                  i32 pen_width, bool filled) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  ellipse_i(s, l, t, r, b, rgb_of(argb), alpha_of(argb), pen_width, filled);
  maybe_dump(s);
}

void polygon_argb(void* hdc, const i32* xy, int n, u32 argb, i32 pen_width,
                  bool filled) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  polygon_i(s, xy, n, rgb_of(argb), alpha_of(argb), pen_width, filled);
  maybe_dump(s);
}

// ---- GDI paths ----------------------------------------------------------

void fill_path(void* hdc, const i32* xy, const int* counts, int n_sub,
               u32 argb, int fill_mode) {
  if (!xy || !counts || n_sub <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  std::vector<u8> m((size_t)s->w * (size_t)s->h, 0);
  rasterize_path_mask(xy, counts, n_sub, s->w, s->h, fill_mode, m.data());
  const u32 rgb = rgb_of(argb), a = alpha_of(argb);
  for (i32 y = s->clip_t; y < s->clip_b; ++y) {
    const u8* row = m.data() + (size_t)y * (size_t)s->w;
    for (i32 x = s->clip_l; x < s->clip_r; ++x)
      if (row[x]) put_rgb(s, x, y, rgb, a);
  }
  maybe_dump(s);
}

void stroke_path(void* hdc, const i32* xy, const int* counts, int n_sub,
                 u32 argb, i32 pen_width, bool close_all) {
  if (!xy || !counts || n_sub <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  const u32 rgb = rgb_of(argb), a = alpha_of(argb);
  int base = 0;
  for (int sub = 0; sub < n_sub; ++sub) {
    const int n = counts[sub];
    for (int i = 0; i + 1 < n; ++i)
      line_i(s, xy[(base + i) * 2], xy[(base + i) * 2 + 1],
             xy[(base + i + 1) * 2], xy[(base + i + 1) * 2 + 1], rgb, a,
             pen_width);
    if (close_all && n > 2)
      line_i(s, xy[(base + n - 1) * 2], xy[(base + n - 1) * 2 + 1],
             xy[base * 2], xy[base * 2 + 1], rgb, a, pen_width);
    base += n;
  }
  maybe_dump(s);
}

bool clip_path(void* hdc, const i32* xy, const int* counts, int n_sub,
               int fill_mode, bool intersect) {
  if (!xy || !counts || n_sub <= 0) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return false;
  const size_t n_px = (size_t)s->w * (size_t)s->h;
  std::vector<u8> m(n_px, 0);
  rasterize_path_mask(xy, counts, n_sub, s->w, s->h, fill_mode, m.data());
  if (intersect && s->mask)
    for (size_t i = 0; i < n_px; ++i) m[i] = (u8)(m[i] && s->mask[i]);
  u8* nm = (u8*)malloc(n_px);
  if (!nm) return false;
  memcpy(nm, m.data(), n_px);
  free(s->mask);
  s->mask = nm;
  maybe_dump(s);
  return true;
}

void clear_mask(void* hdc) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      free(g_surfaces[i].mask);
      g_surfaces[i].mask = nullptr;
      return;
    }
  }
}

// ---- rounded rectangles / arcs -----------------------------------------

void round_rect(void* hdc, i32 l, i32 t, i32 r, i32 b, i32 ew, i32 eh,
                u32 argb, i32 pen_width, bool filled) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  if (r < l) { const i32 tmp = l; l = r; r = tmp; }
  if (b < t) { const i32 tmp = t; t = b; b = tmp; }
  if (pen_width < 1) pen_width = 1;
  const u32 rgb = rgb_of(argb), a = alpha_of(argb);
  const float rx = (float)(ew > 0 ? ew : 1) / 2.0f;
  const float ry = (float)(eh > 0 ? eh : 1) / 2.0f;
  const float cx0 = (float)l + rx, cx1 = (float)r - rx;
  const float cy0 = (float)t + ry, cy1 = (float)b - ry;
  auto inside = [&](float x, float y) -> bool {
    if (x < (float)l || x >= (float)r || y < (float)t || y >= (float)b)
      return false;
    if (x >= cx0 && x < cx1) return true;
    if (y >= cy0 && y < cy1) return true;
    const float ccx = x < cx0 ? cx0 : cx1;
    const float ccy = y < cy0 ? cy0 : cy1;
    const float nx = (x - ccx) / rx, ny = (y - ccy) / ry;
    return nx * nx + ny * ny <= 1.0f;
  };
  const i32 edge = pen_width;
  for (i32 y = t; y < b; ++y) {
    for (i32 x = l; x < r; ++x) {
      const bool in = inside((float)x + 0.5f, (float)y + 0.5f);
      if (!in) continue;
      if (!filled) {
        const bool boundary =
            !inside((float)x + 0.5f + (float)edge, (float)y + 0.5f) ||
            !inside((float)x + 0.5f - (float)edge, (float)y + 0.5f) ||
            !inside((float)x + 0.5f, (float)y + 0.5f + (float)edge) ||
            !inside((float)x + 0.5f, (float)y + 0.5f - (float)edge);
        if (!boundary) continue;
      }
      put_rgb(s, x, y, rgb, a);
    }
  }
  maybe_dump(s);
}

void arc(void* hdc, i32 l, i32 t, i32 r, i32 b, i32 sx, i32 sy, i32 ex,
         i32 ey, u32 argb, i32 pen_width, int mode) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  if (pen_width < 1) pen_width = 1;
  const u32 rgb = rgb_of(argb), a = alpha_of(argb);
  const float cx = ((float)l + (float)r) / 2.0f;
  const float cy = ((float)t + (float)b) / 2.0f;
  const float rx = ((float)r - (float)l) / 2.0f;
  const float ry = ((float)b - (float)t) / 2.0f;
  if (rx <= 0.0f || ry <= 0.0f) return;
  constexpr float kPi = 3.14159265358979f;
  const float a0 = atan2f(((float)sy - cy) / ry, ((float)sx - cx) / rx);
  const float a1 = atan2f(((float)ey - cy) / ry, ((float)ex - cx) / rx);
  // GDI arcs run counter-clockwise from start to end. Screen coordinates have
  // y pointing down, so counter-clockwise is an increasing angle here.
  float sweep = a1 - a0;
  while (sweep <= 0.0f) sweep += 2.0f * kPi;
  const int steps = (int)((rx > ry ? rx : ry) * 4.0f) + 8;
  std::vector<i32> pts;
  pts.reserve((size_t)steps * 2 + 2);
  for (int i = 0; i <= steps; ++i) {
    const float ang = a0 + sweep * (float)i / (float)steps;
    pts.push_back((i32)(cx + rx * cosf(ang)));
    pts.push_back((i32)(cy + ry * sinf(ang)));
  }
  if (mode == 0) {
    for (size_t i = 0; i + 3 < pts.size(); i += 2)
      line_i(s, pts[i], pts[i + 1], pts[i + 2], pts[i + 3], rgb, a, pen_width);
  } else {
    std::vector<i32> shape = pts;
    if (mode == 1) {  // pie: close through the centre
      shape.push_back((i32)cx);
      shape.push_back((i32)cy);
    }
    polygon_i(s, shape.data(), (int)(shape.size() / 2), rgb, a, pen_width,
              true);
  }
  maybe_dump(s);
}

void scroll(void* hdc, i32 dx, i32 dy, i32 l, i32 t, i32 r, i32 b) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  if (l < s->clip_l) l = s->clip_l;
  if (t < s->clip_t) t = s->clip_t;
  if (r > s->clip_r) r = s->clip_r;
  if (b > s->clip_b) b = s->clip_b;
  if (r <= l || b <= t) return;
  if (dx == 0 && dy == 0) return;
  // Clip the source range so the copy stays inside the rectangle, then move
  // rows/columns in the direction that never overwrites unread pixels.
  const i32 sy0 = t + (dy > 0 ? dy : 0);
  const i32 sy1 = b + (dy < 0 ? dy : 0);
  const i32 sx0 = l + (dx > 0 ? dx : 0);
  const i32 sx1 = r + (dx < 0 ? dx : 0);
  if (sx1 <= sx0 || sy1 <= sy0) return;
  // Move towards the source: when the destination is above/left of the source
  // (dy/dx > 0) the safe order is ascending, otherwise descending. The other
  // way round reads rows the copy already overwrote.
  const i32 step = (dy > 0 || (dy == 0 && dx > 0)) ? 1 : -1;
  const i32 y_start = step > 0 ? sy0 : sy1 - 1;
  const i32 y_end = step > 0 ? sy1 : sy0 - 1;
  for (i32 y = y_start; y != y_end; y += step) {
    u32* dst_row = s->px + (size_t)(y - dy) * (size_t)s->w;
    const u32* src_row = s->px + (size_t)y * (size_t)s->w;
    if (dx > 0) {
      for (i32 x = sx1 - 1; x >= sx0; --x) dst_row[x - dx] = src_row[x];
    } else {
      for (i32 x = sx0; x < sx1; ++x) dst_row[x - dx] = src_row[x];
    }
  }
  // A path clip mask is per-pixel geometry; it has to travel with the pixels
  // or every later draw would be clipped by a stale shape.
  if (s->mask) {
    for (i32 y = y_start; y != y_end; y += step) {
      u8* dst_row = s->mask + (size_t)(y - dy) * (size_t)s->w;
      const u8* src_row = s->mask + (size_t)y * (size_t)s->w;
      if (dx > 0) {
        for (i32 x = sx1 - 1; x >= sx0; --x) dst_row[x - dx] = src_row[x];
      } else {
        for (i32 x = sx0; x < sx1; ++x) dst_row[x - dx] = src_row[x];
      }
    }
  }
  maybe_dump(s);
}

// ---- gradients and blended blits ---------------------------------------

void gradient(void* hdc, i32 l, i32 t, i32 r, i32 b, u32 argb0, u32 argb1,
              bool vertical) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return;
  if (l < s->clip_l) l = s->clip_l;
  if (t < s->clip_t) t = s->clip_t;
  if (r > s->clip_r) r = s->clip_r;
  if (b > s->clip_b) b = s->clip_b;
  if (r <= l || b <= t) return;
  const i32 span = vertical ? (b - t - 1) : (r - l - 1);
  const float denom = span > 0 ? (float)span : 1.0f;
  for (i32 y = t; y < b; ++y) {
    for (i32 x = l; x < r; ++x) {
      const float tt = (vertical ? (float)(y - t) : (float)(x - l)) / denom;
      auto mix = [&](u32 shift) -> u32 {
        const float c0 = (float)((argb0 >> shift) & 0xFFu);
        const float c1 = (float)((argb1 >> shift) & 0xFFu);
        return (u32)(c0 + (c1 - c0) * tt);
      };
      put_rgb(s, x, y, (mix(16) << 16) | (mix(8) << 8) | mix(0), mix(24));
    }
  }
  maybe_dump(s);
}

bool alpha_blend(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src,
                 i32 sx, i32 sy, i32 sw, i32 sh, u8 const_alpha) {
  if (const_alpha == 0 || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return true;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc_dst, 0, 0, nullptr);
  surface* src = ensure_locked(hdc_src, 0, 0, nullptr);
  if (!d || !src) return false;
  for (i32 dy = 0; dy < h; ++dy) {
    const i32 syy = sy + (i32)((i64)dy * sh / h);
    if (syy < 0 || syy >= src->h) continue;
    const u32* srow = src->px + (size_t)syy * (size_t)src->w;
    for (i32 dx = 0; dx < w; ++dx) {
      const i32 sxx = sx + (i32)((i64)dx * sw / w);
      if (sxx < 0 || sxx >= src->w) continue;
      const u32 spx = srow[sxx];
      if (const_alpha >= 255) {
        put(d, x + dx, y + dy, spx);
      } else {
        put_rgb(d, x + dx, y + dy, spx, const_alpha);
      }
    }
  }
  maybe_dump(d);
  return true;
}

bool key_blit(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src,
              i32 sx, i32 sy, i32 sw, i32 sh, u32 key_argb) {
  if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return true;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc_dst, 0, 0, nullptr);
  surface* src = ensure_locked(hdc_src, 0, 0, nullptr);
  if (!d || !src) return false;
  const u32 key = rgb_of(key_argb);
  for (i32 dy = 0; dy < h; ++dy) {
    const i32 syy = sy + (i32)((i64)dy * sh / h);
    if (syy < 0 || syy >= src->h) continue;
    const u32* srow = src->px + (size_t)syy * (size_t)src->w;
    for (i32 dx = 0; dx < w; ++dx) {
      const i32 sxx = sx + (i32)((i64)dx * sw / w);
      if (sxx < 0 || sxx >= src->w) continue;
      const u32 spx = srow[sxx];
      if ((spx & 0xFFFFFFu) == key) continue;
      put(d, x + dx, y + dy, spx);
    }
  }
  maybe_dump(d);
  return true;
}

bool image_bgra(void* hdc, i32 x, i32 y, i32 w, i32 h, const u32* bgra,
                i32 src_stride_px, i32 src_w, i32 src_h, u8 alpha) {
  if (!bgra || w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0) return false;
  if (src_stride_px < src_w) src_stride_px = src_w;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc, 0, 0, nullptr);
  if (!d) return false;
  for (i32 dy = 0; dy < h; ++dy) {
    const i32 syy = (i32)((i64)dy * src_h / h);
    if (syy < 0 || syy >= src_h) continue;
    const u32* srow = bgra + (size_t)syy * (size_t)src_stride_px;
    for (i32 dx = 0; dx < w; ++dx) {
      const i32 sxx = (i32)((i64)dx * src_w / w);
      if (sxx < 0 || sxx >= src_w) continue;
      // GDI+ bitmaps hold PixelFormat32bppARGB: the little-endian u32 is
      // already 0xAARRGGBB, so the low 24 bits are the surface pixel order.
      const u32 argb = srow[sxx];
      const u32 a = ((argb >> 24) * alpha) / 255u;
      put_rgb(d, x + dx, y + dy, argb & 0xFFFFFFu, a);
    }
  }
  maybe_dump(d);
  return true;
}

// ---- text ---------------------------------------------------------------

int text(void* hdc, i32 x, i32 y, const u16* str, int n,
         const text_style& st) {
  if (!str || n <= 0) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return -1;

  const i32 cell_h = st.height > 0 ? st.height : trt_font8x16::kGlyphH;
  const i32 adv = st.advance > 0 ? st.advance : trt_font8x16::kGlyphAdvance;
  const i32 ascent = st.ascent > 0 ? st.ascent : (cell_h * 13) / 16;
  i32 total = 0;
  if (st.dx) {
    total = st.dx[n - 1];
  } else {
    total = n * adv;
  }

  // TA_* placement. TA_LEFT/TA_TOP are zero, so the default is unchanged.
  const u32 ta = st.align;
  i32 px0 = x;
  if ((ta & 0x6u) == 0x2u) px0 -= total;                 // TA_RIGHT
  else if ((ta & 0x6u) == 0x6u) px0 -= total / 2;        // TA_CENTER
  i32 py0 = y;
  if ((ta & 0x18u) == 0x8u) py0 -= cell_h;               // TA_BOTTOM
  else if ((ta & 0x18u) == 0x18u) py0 -= ascent;         // TA_BASELINE

  const u32 fg = px_of(st.fg) & 0xFFFFFFu;
  const u32 bg = px_of(st.bg) & 0xFFFFFFu;
  const bool opaque = st.bk_mode == 2;
  const u32 text_alpha = st.alpha;

  for (int i = 0; i < n; ++i) {
    const i32 pen = px0 + (st.dx ? (i ? st.dx[i - 1] : 0) : i * adv);
    i32 cell_w = st.dx ? st.dx[i] - (i ? st.dx[i - 1] : 0) : adv;
    if (cell_w < 1) cell_w = 1;
    if (opaque)
      for (i32 dy = 0; dy < cell_h; ++dy)
        for (i32 dx = 0; dx < cell_w; ++dx) put(s, pen + dx, py0 + dy, bg);
    const u16 ch = str[i];
    // ASCII + Latin-1 are covered (kGlyphIndex); anything else draws a box.
    i32 glyph = -1;
    if (ch < 256) {
      const int slot = trt_font8x16::kGlyphIndex[ch];
      if (slot != trt_font8x16::kGlyphMissing) glyph = slot;
    }
    if (glyph < 0) {
      // Unmappable code point: draw a hollow box so the absence is visible.
      for (i32 dx = 0; dx < cell_w; ++dx) {
        put(s, pen + dx, py0, fg);
        put(s, pen + dx, py0 + cell_h - 1, fg);
      }
      for (i32 dy = 0; dy < cell_h; ++dy) {
        put(s, pen, py0 + dy, fg);
        put(s, pen + cell_w - 1, py0 + dy, fg);
      }
      continue;
    }
    for (i32 dy = 0; dy < cell_h; ++dy) {
      i32 sy = (i32)((i64)dy * trt_font8x16::kGlyphH / cell_h);
      if (sy < 0) sy = 0;
      if (sy >= trt_font8x16::kGlyphH) sy = trt_font8x16::kGlyphH - 1;
      const i32 shear = st.italic ? ((cell_h - dy) / 6) : 0;
      for (i32 dx = 0; dx < cell_w; ++dx) {
        i32 sx = (i32)((i64)dx * trt_font8x16::kGlyphW / cell_w);
        if (sx < 0) sx = 0;
        if (sx >= trt_font8x16::kGlyphW) sx = trt_font8x16::kGlyphW - 1;
        const u32 cov =
            trt_font8x16::kGlyphCoverage[glyph][sy][sx];
        if (!cov) continue;
        const i32 tx = pen + dx + shear;
        const u32 cov_a =
            text_alpha >= 255 ? cov : (cov * text_alpha) / 255u;
        if (cov_a == 255) {
          put(s, tx, py0 + dy, fg);
        } else {
          u32 d = get_clipped(s, tx, py0 + dy);
          put(s, tx, py0 + dy, blend(d, fg, cov_a));
        }
      }
    }
    if (st.underline) {
      const i32 uy = py0 + ascent + 1 < py0 + cell_h - 1 ? py0 + ascent + 1
                                                        : py0 + cell_h - 1;
      for (i32 dx = 0; dx < cell_w; ++dx) put(s, pen + dx, uy, fg);
    }
    if (st.strikeout) {
      const i32 sy2 = py0 + cell_h / 2;
      for (i32 dx = 0; dx < cell_w; ++dx) put(s, pen + dx, sy2, fg);
    }
  }
  maybe_dump(s);
  return n;
}

// ---- blits --------------------------------------------------------------

bool blit(void* hdc_dst, i32 x, i32 y, i32 cx, i32 cy, void* hdc_src, i32 sx,
          i32 sy, u32 rop, u32 pat_colorref) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc_dst, 0, 0, nullptr);
  if (!d) return false;
  surface* src = ensure_locked(hdc_src, 0, 0, nullptr);
  if (!src) return false;
  if (cx <= 0 || cy <= 0) return true;
  const u32 pat = px_of(pat_colorref) & 0xFFFFFFu;
  // Snapshot the source: BitBlt(hdc, ..., hdc, ...) is legal and overlapping.
  u32* tmp = (u32*)malloc(sizeof(u32) * (size_t)cx * (size_t)cy);
  if (!tmp) return false;
  for (i32 j = 0; j < cy; ++j)
    for (i32 i = 0; i < cx; ++i)
      tmp[(size_t)j * (size_t)cx + (size_t)i] = get_clipped(src, sx + i, sy + j);
  bool ok = true;
  for (i32 j = 0; j < cy && ok; ++j) {
    for (i32 i = 0; i < cx; ++i) {
      u32 out;
      if (!apply_rop(rop, get_clipped(d, x + i, y + j),
                     tmp[(size_t)j * (size_t)cx + (size_t)i], pat, &out)) {
        ok = false;
        break;
      }
      put(d, x + i, y + j, out & 0xFFFFFFu);
    }
  }
  free(tmp);
  if (ok) maybe_dump(d);
  return ok;
}

bool stretch(void* hdc_dst, i32 x, i32 y, i32 w, i32 h, void* hdc_src, i32 sx,
             i32 sy, i32 sw, i32 sh, u32 rop, u32 pat_colorref) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc_dst, 0, 0, nullptr);
  if (!d) return false;
  surface* src = ensure_locked(hdc_src, 0, 0, nullptr);
  if (!src) return false;
  if (sw == 0 || sh == 0 || w == 0 || h == 0) return true;
  const u32 pat = px_of(pat_colorref) & 0xFFFFFFu;
  const i32 adw = w < 0 ? -w : w, adh = h < 0 ? -h : h;
  const i32 asw = sw < 0 ? -sw : sw, ash = sh < 0 ? -sh : sh;
  for (i32 j = 0; j < adh; ++j) {
    const i32 srcy = sy + (i32)((i64)j * ash / adh);
    for (i32 i = 0; i < adw; ++i) {
      const i32 srcx = sx + (i32)((i64)i * asw / adw);
      u32 out;
      if (!apply_rop(rop, get_clipped(d, x + i, y + j),
                     get_clipped(src, srcx, srcy), pat, &out))
        return false;
      put(d, x + i, y + j, out & 0xFFFFFFu);
    }
  }
  maybe_dump(d);
  return true;
}

bool dib(void* hdc, i32 x, i32 y, i32 dw, i32 dh, const void* bits,
         const void* bmi, u32 color_use, i32 sx, i32 sy, i32 sw, i32 sh,
         u32 rop) {
  dib_info info;
  if (!parse_dib(bits, bmi, color_use, &info)) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  surface* d = ensure_locked(hdc, 0, 0, nullptr);
  if (!d) return false;
  if (dw == 0 || dh == 0) return true;
  const i32 adw = dw < 0 ? -dw : dw, adh = dh < 0 ? -dh : dh;
  const i32 asw = sw <= 0 ? info.w : sw;
  const i32 ash = sh <= 0 ? info.h : sh;
  for (i32 j = 0; j < adh; ++j) {
    const i32 src_row = sy + (i32)((i64)j * ash / adh);
    if (src_row < 0 || src_row >= info.h) continue;
    for (i32 i = 0; i < adw; ++i) {
      const i32 src_col = sx + (i32)((i64)i * asw / adw);
      if (src_col < 0 || src_col >= info.w) continue;
      u32 out;
      if (!apply_rop(rop, get_clipped(d, x + i, y + j),
                     dib_pixel(&info, src_col, src_row), 0xFFFFFFu, &out))
        return false;
      put(d, x + i, y + j, out & 0xFFFFFFu);
    }
  }
  maybe_dump(d);
  return true;
}

// ---- pixel access -------------------------------------------------------

u32 get_pixel(void* hdc, i32 x, i32 y, bool* ok) {
  if (ok) *ok = false;
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc) {
      surface* s = &g_surfaces[i];
      if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0xFFFFFFFFu;
      if (ok) *ok = true;
      return colorref_of(s->px[(size_t)y * (size_t)s->w + (size_t)x]);
    }
  }
  return 0;  // CLR_INVALID reported by the caller (0xFFFFFFFF) when !ok
}

bool set_pixel(void* hdc, i32 x, i32 y, u32 colorref) {
  std::lock_guard<std::mutex> lk(g_mu);
  surface* s = ensure_locked(hdc, 0, 0, nullptr);
  if (!s) return false;
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) return false;
  if (x < s->clip_l || y < s->clip_t || x >= s->clip_r || y >= s->clip_b)
    return false;
  const size_t idx = (size_t)y * (size_t)s->w + (size_t)x;
  if (s->mask && !s->mask[idx]) return false;  // path clip rejects the pixel
  s->px[idx] = px_of(colorref) & 0xFFFFFFu;
  maybe_dump(s);
  return true;
}

// ---- evidence -----------------------------------------------------------

bool dump_bmp_impl(surface* s, const char* path) {
  if (!s || !s->px || !path) return false;
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const u32 row_bytes = (u32)s->w * 3u;
  const u32 pad = (4u - (row_bytes & 3u)) & 3u;
  const u32 image_bytes = (row_bytes + pad) * (u32)s->h;
  u8 hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  const u32 file_bytes = 54u + image_bytes;
  memcpy(hdr + 2, &file_bytes, 4);
  const u32 off = 54;
  memcpy(hdr + 10, &off, 4);
  const u32 info = 40;
  memcpy(hdr + 14, &info, 4);
  memcpy(hdr + 18, &s->w, 4);
  memcpy(hdr + 22, &s->h, 4);
  const u16 planes = 1, bpp = 24;
  memcpy(hdr + 26, &planes, 2);
  memcpy(hdr + 28, &bpp, 2);
  memcpy(hdr + 34, &image_bytes, 4);
  fwrite(hdr, 1, sizeof(hdr), f);
  const u8 zero[4] = {0, 0, 0, 0};
  for (i32 y = s->h - 1; y >= 0; --y) {
    const u32* row = s->px + (size_t)y * (size_t)s->w;
    for (i32 x = 0; x < s->w; ++x) {
      const u8 bgr[3] = {(u8)(row[x] & 0xFF), (u8)((row[x] >> 8) & 0xFF),
                         (u8)((row[x] >> 16) & 0xFF)};
      fwrite(bgr, 1, 3, f);
    }
    if (pad) fwrite(zero, 1, pad, f);
  }
  fclose(f);
  return true;
}

bool dump_bmp(void* hdc, const char* path) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i)
    if (g_surfaces[i].used && g_surfaces[i].hdc == hdc)
      return dump_bmp_impl(&g_surfaces[i], path);
  return false;
}

int dump_all(const char* dir) {
  if (!dir) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  int n = 0;
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (!g_surfaces[i].used) continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/gdi-dc%d.bmp", dir, i + 1);
    if (dump_bmp_impl(&g_surfaces[i], path)) ++n;
  }
  return n;
}

i64 non_background_pixels(void* hdc, u32 background_colorref) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxSurfaces; ++i) {
    if (!(g_surfaces[i].used && g_surfaces[i].hdc == hdc)) continue;
    surface* s = &g_surfaces[i];
    const u32 bg = px_of(background_colorref) & 0xFFFFFFu;
    i64 n = 0;
    for (size_t k = 0; k < (size_t)s->w * (size_t)s->h; ++k)
      if (s->px[k] != bg) ++n;
    return n;
  }
  return -1;
}

}  // namespace trt_raster
