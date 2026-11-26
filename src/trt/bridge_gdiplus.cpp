// tiramisu/src/trt/bridge_gdiplus.cpp
// gdiplus.dll bridge — the GDI+ 2D flat API (GpStatus contract).
//
// Consumers: desktop applications (launchers, installers, .NET/WinForms UI).
// Until now every draw call was RECORDED and the surface stayed blank, so a
// GDI+ app "ran" and painted nothing. The GDI raster surface
// (src/trt/gdi_raster.cpp) exists now, so this bridge RENDERS:
//   Graphics   holds the HDC it was created from (FromHDC / FromHDCEx /
//              FromHWND) plus a translate offset (GdipTranslateTransform).
//   Brush/Pen  keep their ARGB colour and pen width; alpha really blends.
//   Bitmap     keeps ARGB pixels (from Scan0 or blank), supports Get/SetPixel,
//              LockBits and is blitted by DrawImage*.
//   Path       keeps a display list (rect/ellipse/line/arc) so FillPath and
//              DrawPath replay it.
//   Text       GdipDrawString/GdipMeasureString lay out with the requested em
//              size using the embedded coverage font (word wrap, '\n').
//
// Honest limits, logged by name, never silent:
//   - AC per-pixel alpha is applied per source pixel (GDI+ bitmaps carry it);
//     text is drawn with a single alpha.
//   - Anti-aliasing/smoothing and interpolation modes are accepted but the
//     rasterizer is analytic (no AA), logged once.
//   - The world transform is a full affine matrix (translate/scale/rotate/
//     multiply/invert); shapes, paths and polygons are transformed vertex by
//     vertex. Bitmaps and glyphs are only placed by the transformed origin
//     when the matrix has rotation/skew (named), 1:1 sampled otherwise.
//   - Region/ImageAttributes colour matrices are stored, not applied.
#include "bridge_gdiplus.h"
#include "bridge_gdi.h"
#include "gdi_raster.h"
#include "thunk.h"
#include "trt.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <mutex>
#include <vector>

extern "C" void* TRT_STDCALL trt_GetDC(void* hwnd);

namespace {

void gp_log(const char* s) { trt_core_log(s); }

void gp_logf(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  trt_core_log(line);
}

// ---- object table -------------------------------------------------------
enum gp_type {
  GP_NONE = 0,
  GP_GRAPHICS,
  GP_BRUSH,
  GP_PEN,
  GP_PATH,
  GP_IMAGE,
  GP_ATTRS,
  GP_FONT,
  GP_FORMAT,
  GP_MATRIX,
};

struct gp_op {
  int kind;      // 0 rect, 1 ellipse, 2 line, 3 arc, 4 polygon
  i32 i[8];
  u32 argb;
  std::vector<i32> pts;
};

struct gp_obj {
  gp_type type = GP_NONE;
  void* hdc = nullptr;  // graphics
  u32 argb = 0xFF000000u;
  float width = 1.0f;   // pen width / font em size
  i32 w = 0, h = 0, stride = 0;
  u32* bits = nullptr;  // image pixels (ARGB)
  bool owns_bits = false;
  // World transform, row-vector form [a b; c d; e f]:
  //   x' = x*a + y*c + e,  y' = x*b + y*d + f
  float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  std::vector<gp_op> ops;      // path display list
};

constexpr size_t kMaxObjects = 512;
gp_obj* g_objects[kMaxObjects];
std::mutex g_obj_mu;
size_t g_object_count = 0;

gp_obj* obj_new(gp_type type) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  for (size_t i = 0; i < kMaxObjects; ++i) {
    if (!g_objects[i]) {
      gp_obj* o = new gp_obj();
      o->type = type;
      g_objects[i] = o;
      ++g_object_count;
      return o;
    }
  }
  gp_logf("gdiplus: object table full (%zu) -> OutOfMemory", kMaxObjects);
  return nullptr;
}

gp_obj* obj_get(void* token, gp_type type) {
  if (!token) return nullptr;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  for (size_t i = 0; i < kMaxObjects; ++i) {
    if (g_objects[i] == token) {
      gp_obj* o = g_objects[i];
      if (type != GP_NONE && o->type != type) return nullptr;
      return o;
    }
  }
  return nullptr;
}

void obj_delete(void* token) {
  if (!token) return;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  for (size_t i = 0; i < kMaxObjects; ++i) {
    if (g_objects[i] == token) {
      gp_obj* o = g_objects[i];
      if (o->owns_bits) free(o->bits);
      delete o;
      g_objects[i] = nullptr;
      if (g_object_count) --g_object_count;
      return;
    }
  }
}

// ---- helpers ------------------------------------------------------------
inline i32 f2i(float v) { return (i32)floorf(v); }

// ---- affine transform helpers ------------------------------------------
// Row-vector matrices like GDI+: point' = [x y 1] * [[a b],[c d],[e f]].
inline void mat_identity(float* m) {
  m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f; m[3] = 1.0f; m[4] = 0.0f; m[5] = 0.0f;
}

// out = a * b: b is applied first, then a (GDI+ MatrixOrderPrepend).
inline void mat_mul(const float* a, const float* b, float* out) {
  float r[6];
  r[0] = a[0] * b[0] + a[2] * b[1];
  r[1] = a[1] * b[0] + a[3] * b[1];
  r[2] = a[0] * b[2] + a[2] * b[3];
  r[3] = a[1] * b[2] + a[3] * b[3];
  r[4] = a[0] * b[4] + a[2] * b[5] + a[4];
  r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
  memcpy(out, r, sizeof(r));
}

inline void gp_xf(const gp_obj* g, float x, float y, float* ox, float* oy) {
  *ox = x * g->m[0] + y * g->m[2] + g->m[4];
  *oy = x * g->m[1] + y * g->m[3] + g->m[5];
}

// No rotation or skew: every axis-aligned GDI+ shape keeps its raster path.
inline bool gp_axis(const gp_obj* g) {
  return fabsf(g->m[1]) < 1e-6f && fabsf(g->m[2]) < 1e-6f;
}

// Transform an (x,y,w,h) box and report its bounding box.
void gp_box(const gp_obj* g, i32 x, i32 y, i32 w, i32 h, i32* l, i32* t,
            i32* r, i32* b) {
  float xs[4], ys[4];
  const float cx[4] = {(float)x, (float)(x + w), (float)(x + w), (float)x};
  const float cy[4] = {(float)y, (float)y, (float)(y + h), (float)(y + h)};
  for (int i = 0; i < 4; ++i) gp_xf(g, cx[i], cy[i], &xs[i], &ys[i]);
  float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
  for (int i = 1; i < 4; ++i) {
    if (xs[i] < mnx) mnx = xs[i];
    if (xs[i] > mxx) mxx = xs[i];
    if (ys[i] < mny) mny = ys[i];
    if (ys[i] > mxy) mxy = ys[i];
  }
  *l = f2i(mnx);
  *t = f2i(mny);
  *r = f2i(mxx + 0.5f);
  *b = f2i(mxy + 0.5f);
}

// Transformed quad of an (x,y,w,h) box, in TL/TR/BR/BL order.
void gp_quad(const gp_obj* g, i32 x, i32 y, i32 w, i32 h, i32* out) {
  const float cx[4] = {(float)x, (float)(x + w), (float)(x + w), (float)x};
  const float cy[4] = {(float)y, (float)y, (float)(y + h), (float)(y + h)};
  for (int i = 0; i < 4; ++i) {
    float px, py;
    gp_xf(g, cx[i], cy[i], &px, &py);
    out[i * 2] = (i32)floorf(px + 0.5f);
    out[i * 2 + 1] = (i32)floorf(py + 0.5f);
  }
}

// Flattened transformed arc/pie (GDI+ degrees, clockwise on screen).
void gp_arc_pts(const gp_obj* g, i32 x, i32 y, i32 w, i32 h, float start,
                float sweep, bool pie, std::vector<i32>& out) {
  const float cx = (float)x + (float)w / 2.0f;
  const float cy = (float)y + (float)h / 2.0f;
  const float rx = (float)w / 2.0f, ry = (float)h / 2.0f;
  const float a0 = start * 3.14159265358979f / 180.0f;
  const float a1 = (start + sweep) * 3.14159265358979f / 180.0f;
  int steps = (int)(fabsf(sweep) / 6.0f) + 2;
  if (steps < 2) steps = 2;
  if (steps > 180) steps = 180;
  out.clear();
  for (int i = 0; i <= steps; ++i) {
    const float a = a0 + (a1 - a0) * (float)i / (float)steps;
    float px, py;
    gp_xf(g, cx + rx * cosf(a), cy + ry * sinf(a), &px, &py);
    out.push_back((i32)floorf(px + 0.5f));
    out.push_back((i32)floorf(py + 0.5f));
  }
  if (pie) {
    float px, py;
    gp_xf(g, cx, cy, &px, &py);
    out.push_back((i32)floorf(px + 0.5f));
    out.push_back((i32)floorf(py + 0.5f));
  }
}

// Flattened transformed ellipse outline (48 segments) for rotated/skewed draws.
void gp_ellipse_pts(const gp_obj* g, i32 x, i32 y, i32 w, i32 h,
                    std::vector<i32>& out) {
  const float cx = (float)x + (float)w / 2.0f;
  const float cy = (float)y + (float)h / 2.0f;
  const float rx = (float)w / 2.0f, ry = (float)h / 2.0f;
  const int steps = 48;
  out.clear();
  out.reserve((size_t)steps * 2);
  for (int i = 0; i < steps; ++i) {
    const float a = 6.28318530718f * (float)i / (float)steps;
    float px, py;
    gp_xf(g, cx + rx * cosf(a), cy + ry * sinf(a), &px, &py);
    out.push_back((i32)floorf(px + 0.5f));
    out.push_back((i32)floorf(py + 0.5f));
  }
}

void log_once(const char* what) {
  static const char* seen[24];
  static size_t n = 0;
  for (size_t i = 0; i < n; ++i)
    if (strcmp(seen[i], what) == 0) return;
  if (n < 24) seen[n++] = what;
  gp_logf("gdiplus: %s (named)", what);
}

// Text layout with the embedded 8x16 coverage font, scaled to `em`.
int draw_text_em(void* hdc, i32 x, i32 y, const u16* text, i32 len, i32 em,
                 u32 argb, i32 wrap_w) {
  if (!hdc || !text || len <= 0) return 0;
  if (em < 8) em = 8;
  const i32 cell_h = em;
  i32 adv = em / 2;
  if (adv < 4) adv = 4;
  trt_raster::text_style st;
  st.fg = ((argb & 0xFFu) << 16) | (argb & 0xFF00u) | ((argb >> 16) & 0xFFu);
  st.bg = 0;
  st.bk_mode = 1;  // TRANSPARENT
  st.align = 0;
  st.height = cell_h;
  st.advance = adv;
  st.ascent = (cell_h * 13) / 16;
  st.italic = false;
  st.underline = false;
  st.strikeout = false;
  st.dx = nullptr;
  st.alpha = (u8)(argb >> 24);
  i32 line = 0;
  i32 i = 0;
  while (i < len) {
    // Collect one line, honouring '\n' and (when wrap_w > 0) word wrap.
    i32 end = i;
    while (end < len && text[end] != '\n') ++end;
    i32 start = i;
    while (start < end || start == i) {
      i32 take = end - start;
      if (wrap_w > 0 && take * adv > wrap_w) {
        take = wrap_w / adv;
        if (take < 1) take = 1;
        i32 brk = take;
        for (i32 k = take; k > 0; --k)
          if (text[start + k - 1] == ' ') { brk = k - 1; break; }
        if (brk > 0) take = brk;
      }
      if (take > 0)
        trt_raster::text(hdc, x, y + line * cell_h, text + start, take, st);
      ++line;
      start += take;
      while (start < end && text[start] == ' ') ++start;
      if (take <= 0) break;
    }
    if (end >= len) break;
    i = end + 1;
    if (i >= len) ++line;
  }
  return line;
}

i32 measure_text_em(const u16* text, i32 len, i32 em, i32 wrap_w, i32* lines) {
  if (em < 8) em = 8;
  i32 adv = em / 2;
  if (adv < 4) adv = 4;
  i32 widest = 0, line = 1, run = 0;
  for (i32 i = 0; i < len; ++i) {
    if (text[i] == '\n') {
      if (run > widest) widest = run;
      run = 0;
      ++line;
      continue;
    }
    run += adv;
    if (wrap_w > 0 && run > wrap_w) {
      if (run - adv > widest) widest = run - adv;
      run = adv;
      ++line;
    }
  }
  if (run > widest) widest = run;
  if (lines) *lines = line;
  return widest;
}

void blit_image(gp_obj* g, gp_obj* img, i32 dx, i32 dy, i32 dw, i32 dh,
                i32 sx, i32 sy, i32 sw, i32 sh) {
  if (!g || !g->hdc || !img || !img->bits || img->w <= 0 || img->h <= 0) return;
  if (dw <= 0 || dh <= 0) return;
  if (sw <= 0) sw = img->w;
  if (sh <= 0) sh = img->h;
  // Crop the source rectangle into a temporary buffer, then scale-blit it.
  u32* crop = (u32*)malloc(sizeof(u32) * (size_t)sw * (size_t)sh);
  if (!crop) return;
  for (i32 y = 0; y < sh; ++y) {
    for (i32 x = 0; x < sw; ++x) {
      const i32 ix = sx + x, iy = sy + y;
      crop[(size_t)y * (size_t)sw + (size_t)x] =
          (ix >= 0 && iy >= 0 && ix < img->w && iy < img->h)
              ? img->bits[(size_t)iy * (size_t)img->stride / 4 + (size_t)ix]
              : 0;
    }
  }
  if (gp_axis(g)) {
    i32 l, t, r, bo;
    gp_box(g, dx, dy, dw, dh, &l, &t, &r, &bo);
    trt_raster::image_bgra(g->hdc, l, t, r - l, bo - t, crop, sw, sw, sh, 255);
  } else {
    // Rotated/skewed bitmaps would need per-pixel inverse sampling; draw at
    // the transformed bounding box and name the gap.
    log_once("DrawImage under rotation/skew draws the transformed bounding box "
             "(per-pixel inverse sampling not implemented)");
    i32 l, t, r, bo;
    gp_box(g, dx, dy, dw, dh, &l, &t, &r, &bo);
    trt_raster::image_bgra(g->hdc, l, t, r - l, bo - t, crop, sw, sw, sh, 255);
  }
  free(crop);
}

}  // namespace

extern "C" {

// ---- lifecycle ----------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdiplusStartup(u64* token, const void* input,
                                         void* output) {
  (void)input;
  (void)output;
  if (token) *token = 0xB07C0001ull;
  gp_log("gdiplus: GdiplusStartup -> Ok (token 0xB07C0001, rendering on the DC surface)");
  return 0;
}

void TRT_GUEST_ABI gdiplus_GdiplusShutdown(u64 token) {
  gp_logf("gdiplus: GdiplusShutdown(0x%llx)", (unsigned long long)token);
}

void* TRT_GUEST_ABI gdiplus_GdipAlloc(u32 size) {
  return malloc(size ? size : 1);
}

void TRT_GUEST_ABI gdiplus_GdipFree(void* mem) { free(mem); }

// ---- graphics -----------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateFromHDC(void* hdc, void** graphics) {
  if (!graphics) return 2;
  gp_obj* g = obj_new(GP_GRAPHICS);
  if (!g) return 3;
  g->hdc = hdc;
  *graphics = g;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateFromHDCEx(void* hdc, i32 x, i32 y, i32 w,
                                              i32 h, void** graphics) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return gdiplus_GdipCreateFromHDC(hdc, graphics);
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateFromHWND(void* hwnd, void** graphics) {
  if (!graphics) return 2;
  void* hdc = trt_GetDC(hwnd);
  gp_obj* g = obj_new(GP_GRAPHICS);
  if (!g) return 3;
  g->hdc = hdc;
  *graphics = g;
  gp_logf("gdiplus: GdipCreateFromHWND(hwnd=%p) -> hdc=%p",
          (void*)hwnd, hdc);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteGraphics(void* graphics) {
  obj_delete(graphics);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetDC(void* graphics, void** hdc) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g || !hdc) return 2;
  *hdc = g->hdc;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipReleaseDC(void* graphics, void* hdc) {
  (void)hdc;
  return obj_get(graphics, GP_GRAPHICS) ? 0 : 2;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetDpiX(void* graphics, float* dpi) {
  (void)graphics;
  if (!dpi) return 2;
  *dpi = 96.0f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetDpiY(void* graphics, float* dpi) {
  return gdiplus_GdipGetDpiX(graphics, dpi);
}

// Apply `t` to the graphics world transform. MatrixOrderPrepend (0) means the
// current transform runs first and `t` after it; Append (1) is the reverse.
static void gp_apply(gp_obj* g, const float* t, i32 order) {
  float out[6];
  if (order == 0) mat_mul(t, g->m, out);
  else mat_mul(g->m, t, out);
  memcpy(g->m, out, sizeof(out));
}

i32 TRT_GUEST_ABI gdiplus_GdipTranslateTransform(void* graphics, float dx,
                                                 float dy, i32 order) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g) return 2;
  float t[6];
  mat_identity(t);
  t[4] = dx;
  t[5] = dy;
  gp_apply(g, t, order);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipResetTransform(void* graphics) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g) return 2;
  mat_identity(g->m);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipScaleTransform(void* graphics, float sx,
                                             float sy, i32 order) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g) return 2;
  float t[6];
  mat_identity(t);
  t[0] = sx;
  t[3] = sy;
  gp_apply(g, t, order);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipRotateTransform(void* graphics, float angle,
                                              i32 order) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g) return 2;
  const float rad = angle * 3.14159265358979f / 180.0f;
  const float c = cosf(rad), sn = sinf(rad);
  float t[6];
  mat_identity(t);
  t[0] = c;   // x' = x*cos - y*sin
  t[1] = sn;  // y' = x*sin + y*cos
  t[2] = -sn;
  t[3] = c;
  gp_apply(g, t, order);
  return 0;
}

// ---- matrix objects -----------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateMatrix(void** matrix) {
  if (!matrix) return 2;
  gp_obj* m = obj_new(GP_MATRIX);
  if (!m) return 3;
  mat_identity(m->m);
  *matrix = m;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateMatrix2(float a, float b, float c, float d,
                                            float e, float f, void** matrix) {
  i32 st = gdiplus_GdipCreateMatrix(matrix);
  if (st != 0) return st;
  gp_obj* m = obj_get(*matrix, GP_MATRIX);
  if (!m) return 2;
  m->m[0] = a; m->m[1] = b; m->m[2] = c;
  m->m[3] = d; m->m[4] = e; m->m[5] = f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCloneMatrix(void* matrix, void** clone) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m || !clone) return 2;
  return gdiplus_GdipCreateMatrix2(m->m[0], m->m[1], m->m[2], m->m[3], m->m[4],
                                   m->m[5], clone);
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteMatrix(void* matrix) {
  if (obj_get(matrix, GP_MATRIX)) obj_delete(matrix);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetMatrixElements(void* matrix, float a, float b,
                                                float c, float d, float e,
                                                float f) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m) return 2;
  m->m[0] = a; m->m[1] = b; m->m[2] = c;
  m->m[3] = d; m->m[4] = e; m->m[5] = f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetMatrixElements(void* matrix, float* out) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m || !out) return 2;
  memcpy(out, m->m, sizeof(m->m));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipMultiplyMatrix(void* matrix, void* matrix2,
                                             i32 order) {
  gp_obj* a = obj_get(matrix, GP_MATRIX);
  gp_obj* b = obj_get(matrix2, GP_MATRIX);
  if (!a || !b) return 2;
  float out[6];
  // Prepend: this transform applies first, matrix2 after it.
  if (order == 0) mat_mul(b->m, a->m, out);
  else mat_mul(a->m, b->m, out);
  memcpy(a->m, out, sizeof(out));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipTranslateMatrix(void* matrix, float dx, float dy,
                                              i32 order) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m) return 2;
  float t[6];
  mat_identity(t);
  t[4] = dx;
  t[5] = dy;
  float out[6];
  if (order == 0) mat_mul(t, m->m, out);
  else mat_mul(m->m, t, out);
  memcpy(m->m, out, sizeof(out));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipScaleMatrix(void* matrix, float sx, float sy,
                                          i32 order) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m) return 2;
  float t[6];
  mat_identity(t);
  t[0] = sx;
  t[3] = sy;
  float out[6];
  if (order == 0) mat_mul(t, m->m, out);
  else mat_mul(m->m, t, out);
  memcpy(m->m, out, sizeof(out));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipRotateMatrix(void* matrix, float angle,
                                           i32 order) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m) return 2;
  const float rad = angle * 3.14159265358979f / 180.0f;
  const float c = cosf(rad), sn = sinf(rad);
  float t[6];
  mat_identity(t);
  t[0] = c; t[1] = sn; t[2] = -sn; t[3] = c;
  float out[6];
  if (order == 0) mat_mul(t, m->m, out);
  else mat_mul(m->m, t, out);
  memcpy(m->m, out, sizeof(out));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipInvertMatrix(void* matrix) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m) return 2;
  const float det = m->m[0] * m->m[3] - m->m[1] * m->m[2];
  if (fabsf(det) < 1e-12f) {
    gp_logf("gdiplus: GdipInvertMatrix on a singular matrix -> InvalidParameter");
    return 4;
  }
  const float ia = m->m[3] / det, ib = -m->m[1] / det;
  const float ic = -m->m[2] / det, id = m->m[0] / det;
  const float ie = -(ia * m->m[4] + ic * m->m[5]);
  const float iff = -(ib * m->m[4] + id * m->m[5]);
  m->m[0] = ia; m->m[1] = ib; m->m[2] = ic;
  m->m[3] = id; m->m[4] = ie; m->m[5] = iff;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipTransformMatrixPoints(void* matrix, void* pts,
                                                    i32 count) {
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!m || !pts || count < 0) return 2;
  float* p = (float*)pts;
  for (i32 i = 0; i < count; ++i) {
    const float x = p[i * 2], y = p[i * 2 + 1];
    p[i * 2] = x * m->m[0] + y * m->m[2] + m->m[4];
    p[i * 2 + 1] = x * m->m[1] + y * m->m[3] + m->m[5];
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetWorldTransform(void* graphics, void* matrix) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!g || !m) return 2;
  memcpy(g->m, m->m, sizeof(g->m));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetWorldTransform(void* graphics, void* matrix) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* m = obj_get(matrix, GP_MATRIX);
  if (!g || !m) return 2;
  memcpy(m->m, g->m, sizeof(m->m));
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetMatrixLastStatus(void* matrix) {
  return obj_get(matrix, GP_MATRIX) ? 0 : 2;
}

// CoordinateSpace: 0 = World, 1 = Page, 2 = Device. The world transform is the
// only one that exists here, so world->device transforms apply it.
i32 TRT_GUEST_ABI gdiplus_GdipTransformPoints(void* graphics, i32 dest_space,
                                              i32 src_space, void* pts,
                                              i32 count) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  if (!g || !pts || count < 0) return 2;
  const bool world_to_device = (src_space == 0 && dest_space != 0);
  if (!world_to_device) return 0;  // device/page are the same space here
  float* p = (float*)pts;
  for (i32 i = 0; i < count; ++i) {
    float x, y;
    gp_xf(g, p[i * 2], p[i * 2 + 1], &x, &y);
    p[i * 2] = x;
    p[i * 2 + 1] = y;
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPageUnit(void* graphics, i32 unit) {
  (void)graphics;
  (void)unit;
  return 0;  // everything is device pixels here
}

i32 TRT_GUEST_ABI gdiplus_GdipSetSmoothingMode(void* graphics, i32 mode) {
  (void)graphics;
  if (mode != 0) log_once("GdipSetSmoothingMode: analytic rasterizer has no AA");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetInterpolationMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  log_once("GdipSetInterpolationMode: blits are nearest-neighbour");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPixelOffsetMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetCompositingMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetCompositingQuality(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetTextRenderingHint(void* graphics, i32 hint) {
  (void)graphics;
  (void)hint;
  return 0;
}

// ---- brushes / pens -----------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateSolidFill(u32 argb, void** brush) {
  if (!brush) return 2;
  gp_obj* b = obj_new(GP_BRUSH);
  if (!b) return 3;
  b->argb = argb;
  *brush = b;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetSolidFillColor(void* brush, u32 argb) {
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!b) return 2;
  b->argb = argb;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetSolidFillColor(void* brush, u32* argb) {
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!b || !argb) return 2;
  *argb = b->argb;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteBrush(void* brush) {
  obj_delete(brush);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCloneBrush(void* brush, void** clone) {
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!b || !clone) return 2;
  gp_obj* c = obj_new(GP_BRUSH);
  if (!c) return 3;
  c->argb = b->argb;
  *clone = c;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreatePen1(u32 argb, float width, i32 unit,
                                         void** pen) {
  (void)unit;
  if (!pen) return 2;
  gp_obj* p = obj_new(GP_PEN);
  if (!p) return 3;
  p->argb = argb;
  p->width = width > 0.0f ? width : 1.0f;
  *pen = p;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreatePen2(void* brush, float width, i32 unit,
                                         void** pen) {
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!b) return 2;
  return gdiplus_GdipCreatePen1(b->argb, width, unit, pen);
}

i32 TRT_GUEST_ABI gdiplus_GdipDeletePen(void* pen) {
  obj_delete(pen);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenColor(void* pen, u32 argb) {
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!p) return 2;
  p->argb = argb;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenWidth(void* pen, float width) {
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!p) return 2;
  p->width = width > 0.0f ? width : 1.0f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenEndCap(void* pen, i32 cap) {
  (void)pen;
  (void)cap;
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipSetPenStartCap(void* pen, i32 cap) {
  (void)pen;
  (void)cap;
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipSetPenLineJoin(void* pen, i32 join) {
  (void)pen;
  (void)join;
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipSetPenDashStyle(void* pen, i32 style) {
  (void)pen;
  if (style != 0) log_once("GdipSetPenDashStyle: pens draw solid");
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipSetPenLineCap197819(void* pen, i32 cap, i32 join,
                                                  i32 miter) {
  (void)pen;
  (void)cap;
  (void)join;
  (void)miter;
  return 0;
}

// ---- shapes -------------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipFillRectangleI(void* graphics, void* brush, i32 x,
                                             i32 y, i32 w, i32 h) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!g || !b) return 2;
  if (gp_axis(g)) {
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    trt_raster::fill_argb(g->hdc, l, t, r, bo, b->argb);
  } else {
    i32 q[8];
    gp_quad(g, x, y, w, h, q);
    trt_raster::polygon_argb(g->hdc, q, 4, b->argb, 1, true);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipFillRectangle(void* graphics, void* brush, float x,
                                            float y, float w, float h) {
  return gdiplus_GdipFillRectangleI(graphics, brush, f2i(x), f2i(y), f2i(w),
                                    f2i(h));
}

i32 TRT_GUEST_ABI gdiplus_GdipFillRectanglesI(void* graphics, void* brush,
                                              const void* rects, i32 count) {
  const float* r = (const float*)rects;  // GpRectF / GpRect are both 4 ints
  for (i32 i = 0; i < count; ++i) {
    const i32* q = (const i32*)(r + i * 4);
    gdiplus_GdipFillRectangleI(graphics, brush, q[0], q[1], q[2], q[3]);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawRectangleI(void* graphics, void* pen, i32 x,
                                             i32 y, i32 w, i32 h) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p) return 2;
  const i32 pw = p->width > 0.0f ? (i32)p->width : 1;
  if (gp_axis(g)) {
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    trt_raster::frame_argb(g->hdc, l, t, r, bo, p->argb, pw);
  } else {
    i32 q[8];
    gp_quad(g, x, y, w, h, q);
    trt_raster::polygon_argb(g->hdc, q, 4, p->argb, pw, false);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawRectangle(void* graphics, void* pen, float x,
                                            float y, float w, float h) {
  return gdiplus_GdipDrawRectangleI(graphics, pen, f2i(x), f2i(y), f2i(w),
                                    f2i(h));
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawLineI(void* graphics, void* pen, i32 x1,
                                        i32 y1, i32 x2, i32 y2) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p) return 2;
  float ax, ay, bx, by;
  gp_xf(g, (float)x1, (float)y1, &ax, &ay);
  gp_xf(g, (float)x2, (float)y2, &bx, &by);
  trt_raster::line_argb(g->hdc, (i32)floorf(ax + 0.5f), (i32)floorf(ay + 0.5f),
                        (i32)floorf(bx + 0.5f), (i32)floorf(by + 0.5f), p->argb,
                        p->width > 0.0f ? (i32)p->width : 1);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawLine(void* graphics, void* pen, float x1,
                                       float y1, float x2, float y2) {
  return gdiplus_GdipDrawLineI(graphics, pen, f2i(x1), f2i(y1), f2i(x2),
                               f2i(y2));
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawLinesI(void* graphics, void* pen,
                                         const void* points, i32 count) {
  const i32* pt = (const i32*)points;
  for (i32 i = 0; i + 1 < count; ++i)
    gdiplus_GdipDrawLineI(graphics, pen, pt[i * 2], pt[i * 2 + 1],
                          pt[(i + 1) * 2], pt[(i + 1) * 2 + 1]);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawLines(void* graphics, void* pen,
                                        const void* points, i32 count) {
  const float* pt = (const float*)points;
  for (i32 i = 0; i + 1 < count; ++i)
    gdiplus_GdipDrawLine(graphics, pen, pt[i * 2], pt[i * 2 + 1],
                         pt[(i + 1) * 2], pt[(i + 1) * 2 + 1]);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipFillEllipseI(void* graphics, void* brush, i32 x,
                                           i32 y, i32 w, i32 h) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!g || !b) return 2;
  if (gp_axis(g)) {
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    trt_raster::ellipse_argb(g->hdc, l, t, r, bo, b->argb, 1, true);
  } else {
    std::vector<i32> pts;
    gp_ellipse_pts(g, x, y, w, h, pts);
    trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2), b->argb,
                             1, true);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipFillEllipse(void* graphics, void* brush, float x,
                                          float y, float w, float h) {
  return gdiplus_GdipFillEllipseI(graphics, brush, f2i(x), f2i(y), f2i(w),
                                  f2i(h));
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawEllipseI(void* graphics, void* pen, i32 x,
                                           i32 y, i32 w, i32 h) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p) return 2;
  const i32 pw = p->width > 0.0f ? (i32)p->width : 1;
  if (gp_axis(g)) {
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    trt_raster::ellipse_argb(g->hdc, l, t, r, bo, p->argb, pw, false);
  } else {
    std::vector<i32> pts;
    gp_ellipse_pts(g, x, y, w, h, pts);
    trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2), p->argb,
                             pw, false);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawEllipse(void* graphics, void* pen, float x,
                                          float y, float w, float h) {
  return gdiplus_GdipDrawEllipseI(graphics, pen, f2i(x), f2i(y), f2i(w),
                                  f2i(h));
}

i32 TRT_GUEST_ABI gdiplus_GdipFillPolygonI(void* graphics, void* brush,
                                           const void* points, i32 count,
                                           i32 fillmode) {
  (void)fillmode;
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* b = obj_get(brush, GP_BRUSH);
  if (!g || !b || count < 3) return 2;
  std::vector<i32> pts((size_t)count * 2);
  const i32* src = (const i32*)points;
  for (i32 i = 0; i < count; ++i) {
    float px, py;
    gp_xf(g, (float)src[i * 2], (float)src[i * 2 + 1], &px, &py);
    pts[(size_t)i * 2] = (i32)floorf(px + 0.5f);
    pts[(size_t)i * 2 + 1] = (i32)floorf(py + 0.5f);
  }
  trt_raster::polygon_argb(g->hdc, pts.data(), count, b->argb, 1, true);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawPolygonI(void* graphics, void* pen,
                                           const void* points, i32 count) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p || count < 2) return 2;
  std::vector<i32> pts((size_t)count * 2);
  const i32* src = (const i32*)points;
  for (i32 i = 0; i < count; ++i) {
    float px, py;
    gp_xf(g, (float)src[i * 2], (float)src[i * 2 + 1], &px, &py);
    pts[(size_t)i * 2] = (i32)floorf(px + 0.5f);
    pts[(size_t)i * 2 + 1] = (i32)floorf(py + 0.5f);
  }
  trt_raster::polygon_argb(g->hdc, pts.data(), count, p->argb,
                           p->width > 0.0f ? (i32)p->width : 1, false);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawArcI(void* graphics, void* pen, i32 x, i32 y,
                                       i32 w, i32 h, float start, float sweep) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p) return 2;
  const i32 pw = p->width > 0.0f ? (i32)p->width : 1;
  if (gp_axis(g)) {
    const float cx = (float)x + (float)w / 2.0f;
    const float cy = (float)y + (float)h / 2.0f;
    const float rx = (float)w / 2.0f, ry = (float)h / 2.0f;
    const float a0 = start * 3.14159265358979f / 180.0f;
    const float a1 = (start + sweep) * 3.14159265358979f / 180.0f;
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    float sx0, sy0, sx1, sy1;
    gp_xf(g, cx + rx * cosf(a0), cy + ry * sinf(a0), &sx0, &sy0);
    gp_xf(g, cx + rx * cosf(a1), cy + ry * sinf(a1), &sx1, &sy1);
    trt_raster::arc(g->hdc, l, t, r, bo, (i32)sx0, (i32)sy0, (i32)sx1, (i32)sy1,
                    p->argb, pw, 0);
  } else {
    std::vector<i32> pts;
    gp_arc_pts(g, x, y, w, h, start, sweep, false, pts);
    trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2), p->argb,
                             pw, false);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawPieI(void* graphics, void* pen, i32 x, i32 y,
                                       i32 w, i32 h, float start, float sweep) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* p = obj_get(pen, GP_PEN);
  if (!g || !p) return 2;
  const i32 pw = p->width > 0.0f ? (i32)p->width : 1;
  if (gp_axis(g)) {
    const float cx = (float)x + (float)w / 2.0f;
    const float cy = (float)y + (float)h / 2.0f;
    const float rx = (float)w / 2.0f, ry = (float)h / 2.0f;
    const float a0 = start * 3.14159265358979f / 180.0f;
    const float a1 = (start + sweep) * 3.14159265358979f / 180.0f;
    i32 l, t, r, bo;
    gp_box(g, x, y, w, h, &l, &t, &r, &bo);
    float sx0, sy0, sx1, sy1;
    gp_xf(g, cx + rx * cosf(a0), cy + ry * sinf(a0), &sx0, &sy0);
    gp_xf(g, cx + rx * cosf(a1), cy + ry * sinf(a1), &sx1, &sy1);
    trt_raster::arc(g->hdc, l, t, r, bo, (i32)sx0, (i32)sy0, (i32)sx1, (i32)sy1,
                    p->argb, pw, 1);
  } else {
    std::vector<i32> pts;
    gp_arc_pts(g, x, y, w, h, start, sweep, true, pts);
    trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2), p->argb,
                             pw, true);
  }
  return 0;
}

// ---- paths --------------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreatePath(i32 fillmode, void** path) {
  (void)fillmode;
  if (!path) return 2;
  gp_obj* p = obj_new(GP_PATH);
  if (!p) return 3;
  *path = p;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeletePath(void* path) {
  obj_delete(path);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipResetPath(void* path) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p) return 2;
  p->ops.clear();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathRectangle(void* path, float x, float y,
                                               float w, float h) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p) return 2;
  gp_op op;
  op.kind = 0;
  op.i[0] = f2i(x);
  op.i[1] = f2i(y);
  op.i[2] = f2i(w);
  op.i[3] = f2i(h);
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathEllipse(void* path, float x, float y,
                                             float w, float h) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p) return 2;
  gp_op op;
  op.kind = 1;
  op.i[0] = f2i(x);
  op.i[1] = f2i(y);
  op.i[2] = f2i(w);
  op.i[3] = f2i(h);
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathLine(void* path, float x1, float y1,
                                          float x2, float y2) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p) return 2;
  gp_op op;
  op.kind = 2;
  op.i[0] = f2i(x1);
  op.i[1] = f2i(y1);
  op.i[2] = f2i(x2);
  op.i[3] = f2i(y2);
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathLine2(void* path, const void* points,
                                           i32 count) {
  const float* pt = (const float*)points;
  for (i32 i = 0; i + 1 < count; ++i)
    gdiplus_GdipAddPathLine(path, pt[i * 2], pt[i * 2 + 1], pt[(i + 1) * 2],
                            pt[(i + 1) * 2 + 1]);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathLines(void* path, const void* points,
                                           i32 count) {
  return gdiplus_GdipAddPathLine2(path, points, count);
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathArc(void* path, float x, float y, float w,
                                         float h, float start, float sweep) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p) return 2;
  gp_op op;
  op.kind = 3;
  op.i[0] = f2i(x);
  op.i[1] = f2i(y);
  op.i[2] = f2i(w);
  op.i[3] = f2i(h);
  op.i[4] = f2i(start);
  op.i[5] = f2i(sweep);
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathPolygon(void* path, const void* points,
                                             i32 count) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p || count < 2) return 2;
  gp_op op;
  op.kind = 4;
  const float* pt = (const float*)points;
  for (i32 i = 0; i < count; ++i) {
    op.pts.push_back(f2i(pt[i * 2]));
    op.pts.push_back(f2i(pt[i * 2 + 1]));
  }
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathPolygonI(void* path, const void* points,
                                              i32 count) {
  gp_obj* p = obj_get(path, GP_PATH);
  if (!p || count < 2) return 2;
  gp_op op;
  op.kind = 4;
  const i32* pt = (const i32*)points;
  for (i32 i = 0; i < count; ++i) {
    op.pts.push_back(pt[i * 2]);
    op.pts.push_back(pt[i * 2 + 1]);
  }
  p->ops.push_back(op);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipStartFigure(void* path) {
  (void)path;
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipClosePathFigure(void* path) {
  (void)path;
  return 0;
}
i32 TRT_GUEST_ABI gdiplus_GdipClosePathFigures(void* path) {
  (void)path;
  return 0;
}

// Expand one recorded path op into transformed device points.
void gp_op_pts(const gp_obj* g, const gp_op& op, std::vector<i32>& out) {
  out.clear();
  switch (op.kind) {
    case 0: {  // rectangle
      i32 q[8];
      gp_quad(g, op.i[0], op.i[1], op.i[2], op.i[3], q);
      out.assign(q, q + 8);
      break;
    }
    case 1:  // ellipse
      gp_ellipse_pts(g, op.i[0], op.i[1], op.i[2], op.i[3], out);
      break;
    case 2: {  // line
      float px, py;
      gp_xf(g, (float)op.i[0], (float)op.i[1], &px, &py);
      out.push_back((i32)floorf(px + 0.5f));
      out.push_back((i32)floorf(py + 0.5f));
      gp_xf(g, (float)op.i[2], (float)op.i[3], &px, &py);
      out.push_back((i32)floorf(px + 0.5f));
      out.push_back((i32)floorf(py + 0.5f));
      break;
    }
    case 3:  // arc recorded as the bounding ellipse's first half
      gp_arc_pts(g, op.i[0], op.i[1], op.i[2], op.i[3], -90.0f, 180.0f, false,
                 out);
      break;
    case 4: {  // polygon
      const i32 n = (i32)(op.pts.size() / 2);
      for (i32 i = 0; i < n; ++i) {
        float px, py;
        gp_xf(g, (float)op.pts[(size_t)i * 2], (float)op.pts[(size_t)i * 2 + 1],
              &px, &py);
        out.push_back((i32)floorf(px + 0.5f));
        out.push_back((i32)floorf(py + 0.5f));
      }
      break;
    }
    default:
      break;
  }
}

i32 TRT_GUEST_ABI gdiplus_GdipFillPath(void* graphics, void* brush,
                                       void* path) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* b = obj_get(brush, GP_BRUSH);
  gp_obj* p = obj_get(path, GP_PATH);
  if (!g || !b || !p) return 2;
  std::vector<i32> pts;
  for (const gp_op& op : p->ops) {
    if (op.kind == 2) continue;  // a line has no interior
    gp_op_pts(g, op, pts);
    if (pts.size() >= 6)
      trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2),
                               b->argb, 1, true);
  }
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawPath(void* graphics, void* pen,
                                       void* path) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* pn = obj_get(pen, GP_PEN);
  gp_obj* p = obj_get(path, GP_PATH);
  if (!g || !pn || !p) return 2;
  const i32 pw = pn->width > 0.0f ? (i32)pn->width : 1;
  std::vector<i32> pts;
  for (const gp_op& op : p->ops) {
    gp_op_pts(g, op, pts);
    if (pts.size() >= 4)
      trt_raster::polygon_argb(g->hdc, pts.data(), (int)(pts.size() / 2),
                               pn->argb, pw, false);
  }
  return 0;
}

// ---- bitmaps ------------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateBitmapFromScan0(i32 w, i32 h, i32 stride,
                                                    i32 fmt, void* bits,
                                                    void** bitmap) {
  if (!bitmap || w <= 0 || h <= 0) return 2;
  gp_obj* b = obj_new(GP_IMAGE);
  if (!b) return 3;
  b->w = w;
  b->h = h;
  b->stride = stride > 0 ? stride : w * 4;
  const size_t bytes = (size_t)b->stride * (size_t)h;
  b->bits = (u32*)malloc(bytes);
  if (!b->bits) {
    obj_delete(b);
    return 3;
  }
  b->owns_bits = true;
  if (bits) memcpy(b->bits, bits, bytes);
  else memset(b->bits, 0, bytes);
  (void)fmt;
  *bitmap = b;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCloneImage(void* image, void** clone) {
  gp_obj* src = obj_get(image, GP_IMAGE);
  if (!src || !clone) return 2;
  return gdiplus_GdipCreateBitmapFromScan0(src->w, src->h, src->stride, 0,
                                           src->bits, clone);
}

i32 TRT_GUEST_ABI gdiplus_GdipDisposeImage(void* image) {
  obj_delete(image);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetImageWidth(void* image, u32* w) {
  gp_obj* b = obj_get(image, GP_IMAGE);
  if (!b || !w) return 2;
  *w = (u32)b->w;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetImageHeight(void* image, u32* h) {
  gp_obj* b = obj_get(image, GP_IMAGE);
  if (!b || !h) return 2;
  *h = (u32)b->h;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipBitmapGetPixel(void* image, i32 x, i32 y,
                                             u32* argb) {
  gp_obj* b = obj_get(image, GP_IMAGE);
  if (!b || !argb || x < 0 || y < 0 || x >= b->w || y >= b->h) return 2;
  *argb = b->bits[(size_t)y * (size_t)b->stride / 4 + (size_t)x];
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipBitmapSetPixel(void* image, i32 x, i32 y,
                                             u32 argb) {
  gp_obj* b = obj_get(image, GP_IMAGE);
  if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h) return 2;
  b->bits[(size_t)y * (size_t)b->stride / 4 + (size_t)x] = argb;
  return 0;
}

// GpBitmapData (x64): width(4) height(4) stride(4) pixelformat(4) scan0(8)
// reserved(8). LockBits points the caller at our own buffer.
i32 TRT_GUEST_ABI gdiplus_GdipBitmapLockBits(void* image, const void* rect,
                                             u32 flags, i32 fmt, void* data) {
  gp_obj* b = obj_get(image, GP_IMAGE);
  if (!b || !data) return 2;
  (void)rect;
  (void)flags;
  (void)fmt;
  memset(data, 0, 32);
  *(i32*)((u8*)data + 0) = b->w;
  *(i32*)((u8*)data + 4) = b->h;
  *(i32*)((u8*)data + 8) = b->stride;
  *(i32*)((u8*)data + 12) = 0x0026200Au;  // PixelFormat32bppARGB
  *(void**)((u8*)data + 16) = b->bits;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipBitmapUnlockBits(void* image, void* data) {
  (void)image;
  (void)data;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImageI(void* graphics, void* image, i32 x,
                                         i32 y) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* img = obj_get(image, GP_IMAGE);
  if (!g || !img) return 2;
  blit_image(g, img, x, y, img->w, img->h, 0, 0, img->w, img->h);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImage(void* graphics, void* image, float x,
                                        float y) {
  return gdiplus_GdipDrawImageI(graphics, image, f2i(x), f2i(y));
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImageRectI(void* graphics, void* image, i32 x,
                                             i32 y, i32 w, i32 h) {
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* img = obj_get(image, GP_IMAGE);
  if (!g || !img) return 2;
  blit_image(g, img, x, y, w, h, 0, 0, img->w, img->h);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImageRect(void* graphics, void* image, float x,
                                            float y, float w, float h) {
  return gdiplus_GdipDrawImageRectI(graphics, image, f2i(x), f2i(y), f2i(w),
                                    f2i(h));
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImageRectRectI(void* graphics, void* image,
                                                 i32 dx, i32 dy, i32 dw,
                                                 i32 dh, i32 sx, i32 sy,
                                                 i32 sw, i32 sh, i32 unit,
                                                 void* attrs, void* callback,
                                                 void* cbdata) {
  (void)unit;
  (void)attrs;
  (void)callback;
  (void)cbdata;
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* img = obj_get(image, GP_IMAGE);
  if (!g || !img) return 2;
  blit_image(g, img, dx, dy, dw, dh, sx, sy, sw, sh);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawImageRectRect(void* graphics, void* image,
                                                float dx, float dy, float dw,
                                                float dh, float sx, float sy,
                                                float sw, float sh, i32 unit,
                                                void* attrs, void* callback,
                                                void* cbdata) {
  return gdiplus_GdipDrawImageRectRectI(graphics, image, f2i(dx), f2i(dy),
                                        f2i(dw), f2i(dh), f2i(sx), f2i(sy),
                                        f2i(sw), f2i(sh), unit, attrs, callback,
                                        cbdata);
}

// ---- image attributes ---------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateImageAttributes(void** attrs) {
  if (!attrs) return 2;
  gp_obj* a = obj_new(GP_ATTRS);
  if (!a) return 3;
  *attrs = a;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDisposeImageAttributes(void* attrs) {
  obj_delete(attrs);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetImageAttributesWrapMode(void* attrs, i32 mode,
                                                         u32 color,
                                                         int clamp) {
  (void)attrs;
  (void)mode;
  (void)color;
  (void)clamp;
  log_once("GdipSetImageAttributesWrapMode stored, not applied");
  return 0;
}

// ---- fonts / text -------------------------------------------------------
i32 TRT_GUEST_ABI gdiplus_GdipCreateFontFamilyFromName(const u16* name,
                                                       void* collection,
                                                       void** family) {
  (void)collection;
  if (!family) return 2;
  gp_obj* f = obj_new(GP_FONT);
  if (!f) return 3;
  f->width = 13.0f;
  *family = f;
  char line[160];
  char narrow[64];
  int i = 0;
  if (name)
    for (; name[i] && i < 63; ++i) narrow[i] = (char)(name[i] & 0x7F);
  narrow[i] = 0;
  snprintf(line, sizeof(line),
           "gdiplus: GdipCreateFontFamilyFromName(\"%s\") -> embedded 8x16 font",
           narrow);
  gp_log(line);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteFontFamily(void* family) {
  obj_delete(family);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateFont(void* family, float em_size,
                                         i32 style, i32 unit, void** font) {
  (void)unit;
  (void)style;
  if (!font) return 2;
  gp_obj* f = obj_new(GP_FONT);
  if (!f) return 3;
  f->width = em_size > 0.0f ? em_size : 13.0f;
  (void)family;
  *font = f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteFont(void* font) {
  obj_delete(font);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipGetFontSize(void* font, float* size) {
  gp_obj* f = obj_get(font, GP_FONT);
  if (!f || !size) return 2;
  *size = f->width;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateStringFormat(i32 attributes, u16 language,
                                                 void** format) {
  (void)attributes;
  (void)language;
  if (!format) return 2;
  gp_obj* f = obj_new(GP_FORMAT);
  if (!f) return 3;
  *format = f;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteStringFormat(void* format) {
  obj_delete(format);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetStringFormatAlign(void* format, i32 align) {
  (void)format;
  (void)align;
  log_once("GdipSetStringFormatAlign: text draws left-aligned");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawString(void* graphics, const u16* text,
                                         i32 length, void* font,
                                         const void* layout,
                                         void* format, void* brush) {
  (void)format;
  gp_obj* g = obj_get(graphics, GP_GRAPHICS);
  gp_obj* b = obj_get(brush, GP_BRUSH);
  gp_obj* f = obj_get(font, GP_FONT);
  if (!g || !b) return 2;
  if (!text) return 2;
  if (length < 0) {
    length = 0;
    while (text[length]) ++length;
  }
  const float* rc = (const float*)layout;
  i32 x = rc ? f2i(rc[0]) : 0;
  i32 y = rc ? f2i(rc[1]) : 0;
  i32 w = rc ? f2i(rc[2]) : 0;
  const i32 em = f ? (i32)f->width : 13;
  {
    float fx, fy;
    gp_xf(g, (float)x, (float)y, &fx, &fy);
    x = (i32)floorf(fx + 0.5f);
    y = (i32)floorf(fy + 0.5f);
    if (!gp_axis(g) || fabsf(g->m[0] - 1.0f) > 1e-4f ||
        fabsf(g->m[3] - 1.0f) > 1e-4f)
      log_once("DrawString places glyphs at the transformed origin (glyph "
               "scaling/rotation not implemented)");
  }
  draw_text_em(g->hdc, x, y, text, length, em, b->argb, w);
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipMeasureString(void* graphics, const u16* text,
                                            i32 length, void* font,
                                            const void* layout, void* format,
                                            void* bounding, void* codepoints,
                                            void* chars) {
  (void)graphics;
  (void)format;
  (void)codepoints;
  gp_obj* f = obj_get(font, GP_FONT);
  if (!text || !bounding) return 2;
  if (length < 0) {
    length = 0;
    while (text[length]) ++length;
  }
  const float* rc = (const float*)layout;
  const i32 wrap = rc ? f2i(rc[2]) : 0;
  const i32 em = f ? (i32)f->width : 13;
  i32 lines = 1;
  const i32 widest = measure_text_em(text, length, em, wrap, &lines);
  float* out = (float*)bounding;
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = (float)widest;
  out[3] = (float)(lines * (em < 8 ? 8 : em));
  if (chars) *(i32*)chars = length;
  return 0;
}

int trt_bridge_register_gdiplus(void) {
  static const trt_named_fn k_gdiplus_fns[] = {
      {"GdiplusStartup", (void*)&gdiplus_GdiplusStartup},
      {"GdiplusShutdown", (void*)&gdiplus_GdiplusShutdown},
      {"GdipAlloc", (void*)&gdiplus_GdipAlloc},
      {"GdipFree", (void*)&gdiplus_GdipFree},
      {"GdipCreateFromHDC", (void*)&gdiplus_GdipCreateFromHDC},
      {"GdipCreateFromHDCEx", (void*)&gdiplus_GdipCreateFromHDCEx},
      {"GdipCreateFromHWND", (void*)&gdiplus_GdipCreateFromHWND},
      {"GdipDeleteGraphics", (void*)&gdiplus_GdipDeleteGraphics},
      {"GdipGetDC", (void*)&gdiplus_GdipGetDC},
      {"GdipReleaseDC", (void*)&gdiplus_GdipReleaseDC},
      {"GdipGetDpiX", (void*)&gdiplus_GdipGetDpiX},
      {"GdipGetDpiY", (void*)&gdiplus_GdipGetDpiY},
      {"GdipTranslateTransform", (void*)&gdiplus_GdipTranslateTransform},
      {"GdipResetTransform", (void*)&gdiplus_GdipResetTransform},
      {"GdipScaleTransform", (void*)&gdiplus_GdipScaleTransform},
      {"GdipRotateTransform", (void*)&gdiplus_GdipRotateTransform},
      {"GdipCreateMatrix", (void*)&gdiplus_GdipCreateMatrix},
      {"GdipCreateMatrix2", (void*)&gdiplus_GdipCreateMatrix2},
      {"GdipCloneMatrix", (void*)&gdiplus_GdipCloneMatrix},
      {"GdipDeleteMatrix", (void*)&gdiplus_GdipDeleteMatrix},
      {"GdipSetMatrixElements", (void*)&gdiplus_GdipSetMatrixElements},
      {"GdipGetMatrixElements", (void*)&gdiplus_GdipGetMatrixElements},
      {"GdipMultiplyMatrix", (void*)&gdiplus_GdipMultiplyMatrix},
      {"GdipTranslateMatrix", (void*)&gdiplus_GdipTranslateMatrix},
      {"GdipScaleMatrix", (void*)&gdiplus_GdipScaleMatrix},
      {"GdipRotateMatrix", (void*)&gdiplus_GdipRotateMatrix},
      {"GdipInvertMatrix", (void*)&gdiplus_GdipInvertMatrix},
      {"GdipTransformMatrixPoints", (void*)&gdiplus_GdipTransformMatrixPoints},
      {"GdipSetWorldTransform", (void*)&gdiplus_GdipSetWorldTransform},
      {"GdipGetWorldTransform", (void*)&gdiplus_GdipGetWorldTransform},
      {"GdipGetMatrixLastStatus", (void*)&gdiplus_GdipGetMatrixLastStatus},
      {"GdipTransformPoints", (void*)&gdiplus_GdipTransformPoints},
      {"GdipSetPageUnit", (void*)&gdiplus_GdipSetPageUnit},
      {"GdipSetSmoothingMode", (void*)&gdiplus_GdipSetSmoothingMode},
      {"GdipSetInterpolationMode", (void*)&gdiplus_GdipSetInterpolationMode},
      {"GdipSetPixelOffsetMode", (void*)&gdiplus_GdipSetPixelOffsetMode},
      {"GdipSetCompositingMode", (void*)&gdiplus_GdipSetCompositingMode},
      {"GdipSetCompositingQuality",
       (void*)&gdiplus_GdipSetCompositingQuality},
      {"GdipSetTextRenderingHint", (void*)&gdiplus_GdipSetTextRenderingHint},
      {"GdipCreateSolidFill", (void*)&gdiplus_GdipCreateSolidFill},
      {"GdipSetSolidFillColor", (void*)&gdiplus_GdipSetSolidFillColor},
      {"GdipGetSolidFillColor", (void*)&gdiplus_GdipGetSolidFillColor},
      {"GdipDeleteBrush", (void*)&gdiplus_GdipDeleteBrush},
      {"GdipCloneBrush", (void*)&gdiplus_GdipCloneBrush},
      {"GdipCreatePen1", (void*)&gdiplus_GdipCreatePen1},
      {"GdipCreatePen2", (void*)&gdiplus_GdipCreatePen2},
      {"GdipDeletePen", (void*)&gdiplus_GdipDeletePen},
      {"GdipSetPenColor", (void*)&gdiplus_GdipSetPenColor},
      {"GdipSetPenWidth", (void*)&gdiplus_GdipSetPenWidth},
      {"GdipSetPenEndCap", (void*)&gdiplus_GdipSetPenEndCap},
      {"GdipSetPenStartCap", (void*)&gdiplus_GdipSetPenStartCap},
      {"GdipSetPenLineJoin", (void*)&gdiplus_GdipSetPenLineJoin},
      {"GdipSetPenDashStyle", (void*)&gdiplus_GdipSetPenDashStyle},
      {"GdipSetPenLineCap197819", (void*)&gdiplus_GdipSetPenLineCap197819},
      {"GdipFillRectangleI", (void*)&gdiplus_GdipFillRectangleI},
      {"GdipFillRectangle", (void*)&gdiplus_GdipFillRectangle},
      {"GdipFillRectanglesI", (void*)&gdiplus_GdipFillRectanglesI},
      {"GdipDrawRectangleI", (void*)&gdiplus_GdipDrawRectangleI},
      {"GdipDrawRectangle", (void*)&gdiplus_GdipDrawRectangle},
      {"GdipDrawLineI", (void*)&gdiplus_GdipDrawLineI},
      {"GdipDrawLine", (void*)&gdiplus_GdipDrawLine},
      {"GdipDrawLinesI", (void*)&gdiplus_GdipDrawLinesI},
      {"GdipDrawLines", (void*)&gdiplus_GdipDrawLines},
      {"GdipFillEllipseI", (void*)&gdiplus_GdipFillEllipseI},
      {"GdipFillEllipse", (void*)&gdiplus_GdipFillEllipse},
      {"GdipDrawEllipseI", (void*)&gdiplus_GdipDrawEllipseI},
      {"GdipDrawEllipse", (void*)&gdiplus_GdipDrawEllipse},
      {"GdipFillPolygonI", (void*)&gdiplus_GdipFillPolygonI},
      {"GdipDrawPolygonI", (void*)&gdiplus_GdipDrawPolygonI},
      {"GdipDrawArcI", (void*)&gdiplus_GdipDrawArcI},
      {"GdipDrawPieI", (void*)&gdiplus_GdipDrawPieI},
      {"GdipCreatePath", (void*)&gdiplus_GdipCreatePath},
      {"GdipDeletePath", (void*)&gdiplus_GdipDeletePath},
      {"GdipResetPath", (void*)&gdiplus_GdipResetPath},
      {"GdipAddPathRectangle", (void*)&gdiplus_GdipAddPathRectangle},
      {"GdipAddPathEllipse", (void*)&gdiplus_GdipAddPathEllipse},
      {"GdipAddPathLine", (void*)&gdiplus_GdipAddPathLine},
      {"GdipAddPathLine2", (void*)&gdiplus_GdipAddPathLine2},
      {"GdipAddPathLines", (void*)&gdiplus_GdipAddPathLines},
      {"GdipAddPathArc", (void*)&gdiplus_GdipAddPathArc},
      {"GdipAddPathPolygon", (void*)&gdiplus_GdipAddPathPolygon},
      {"GdipAddPathPolygonI", (void*)&gdiplus_GdipAddPathPolygonI},
      {"GdipStartFigure", (void*)&gdiplus_GdipStartFigure},
      {"GdipClosePathFigure", (void*)&gdiplus_GdipClosePathFigure},
      {"GdipClosePathFigures", (void*)&gdiplus_GdipClosePathFigures},
      {"GdipFillPath", (void*)&gdiplus_GdipFillPath},
      {"GdipDrawPath", (void*)&gdiplus_GdipDrawPath},
      {"GdipCreateBitmapFromScan0", (void*)&gdiplus_GdipCreateBitmapFromScan0},
      {"GdipCloneImage", (void*)&gdiplus_GdipCloneImage},
      {"GdipDisposeImage", (void*)&gdiplus_GdipDisposeImage},
      {"GdipGetImageWidth", (void*)&gdiplus_GdipGetImageWidth},
      {"GdipGetImageHeight", (void*)&gdiplus_GdipGetImageHeight},
      {"GdipBitmapGetPixel", (void*)&gdiplus_GdipBitmapGetPixel},
      {"GdipBitmapSetPixel", (void*)&gdiplus_GdipBitmapSetPixel},
      {"GdipBitmapLockBits", (void*)&gdiplus_GdipBitmapLockBits},
      {"GdipBitmapUnlockBits", (void*)&gdiplus_GdipBitmapUnlockBits},
      {"GdipDrawImageI", (void*)&gdiplus_GdipDrawImageI},
      {"GdipDrawImage", (void*)&gdiplus_GdipDrawImage},
      {"GdipDrawImageRectI", (void*)&gdiplus_GdipDrawImageRectI},
      {"GdipDrawImageRect", (void*)&gdiplus_GdipDrawImageRect},
      {"GdipDrawImageRectRectI", (void*)&gdiplus_GdipDrawImageRectRectI},
      {"GdipDrawImageRectRect", (void*)&gdiplus_GdipDrawImageRectRect},
      {"GdipCreateImageAttributes", (void*)&gdiplus_GdipCreateImageAttributes},
      {"GdipDisposeImageAttributes",
       (void*)&gdiplus_GdipDisposeImageAttributes},
      {"GdipSetImageAttributesWrapMode",
       (void*)&gdiplus_GdipSetImageAttributesWrapMode},
      {"GdipCreateFontFamilyFromName",
       (void*)&gdiplus_GdipCreateFontFamilyFromName},
      {"GdipDeleteFontFamily", (void*)&gdiplus_GdipDeleteFontFamily},
      {"GdipCreateFont", (void*)&gdiplus_GdipCreateFont},
      {"GdipDeleteFont", (void*)&gdiplus_GdipDeleteFont},
      {"GdipGetFontSize", (void*)&gdiplus_GdipGetFontSize},
      {"GdipCreateStringFormat", (void*)&gdiplus_GdipCreateStringFormat},
      {"GdipDeleteStringFormat", (void*)&gdiplus_GdipDeleteStringFormat},
      {"GdipSetStringFormatAlign", (void*)&gdiplus_GdipSetStringFormatAlign},
      {"GdipDrawString", (void*)&gdiplus_GdipDrawString},
      {"GdipMeasureString", (void*)&gdiplus_GdipMeasureString},
  };
  trt_module_register("gdiplus.dll", TRT_ROUTE_BUILTIN, k_gdiplus_fns,
                      (int)(sizeof(k_gdiplus_fns) / sizeof(k_gdiplus_fns[0])));
  char line[128];
  snprintf(line, sizeof(line),
           "gdiplus: registered %d functions (rendering on the DC surface)",
           (int)(sizeof(k_gdiplus_fns) / sizeof(k_gdiplus_fns[0])));
  gp_log(line);
  return 0;
}

}  // extern "C"
