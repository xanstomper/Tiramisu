// tiramisu/src/trt/bridge_gdi.cpp
// GDI32 bridge. See bridge_gdi.h.
//
// Object table: every object carries a type tag and its bytes (LOGFONTW
// for fonts, color for brushes/pens, pixel buffer for bitmaps/DIB sections).
// Handles are 0xGCxxxxxx-tagged values the guest treats as opaque HGDIOBJ.
// DCs are the USER32 DC records (shared with bridge_user32 via
// trt_user32_dc_*); the per-DC "selected object" state lives there.
#include "bridge_gdi.h"
#include "bridge_opengl32.h"
#include "bridge_user32.h"
#include "gdi_raster.h"
#include "input.h"
#include "thunk.h"
#include "trt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <vector>

namespace gdilog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) gdilog::line(s_)

// Defined later in this file (DC text state section and the DC state stack);
// declared at file scope so early callers (SaveDC, the DC forget hook) reach
// the same entities instead of anonymous-namespace duplicates.
extern "C" bool dc_text_state(void* hdc, trt_dc_text_state* st);
extern "C" void dc_saved_forget(void* hdc);

namespace {

constexpr u32 TRT_GDI_OBJ_FONT = 1;
constexpr u32 TRT_GDI_OBJ_BRUSH = 2;
constexpr u32 TRT_GDI_OBJ_PEN = 3;
constexpr u32 TRT_GDI_OBJ_REGION = 4;
constexpr u32 TRT_GDI_OBJ_BITMAP = 5;
constexpr u32 TRT_GDI_OBJ_PALETTE = 6;

constexpr u32 LF_BYTES = 96;

struct gdi_obj {
  u32 type;
  u32 size;          // payload bytes (0 = stateless, e.g. solid brush color)
  u8 data[LF_BYTES]; // LOGFONTW copy for fonts
  u32 color;         // COLORREF for brushes/pens
  i32 bm_w;          // bitmap width
  i32 bm_h;          // bitmap height
  u16 bm_bpp;        // bitmap bits per pixel
  void* bm_bits;     // allocated DIB/bitmap pixel buffer
  u16 dib_color_count;
  u32 dib_colors[256];
  u16 pal_count;
  u32 pal_entries[256];
  i32 rg_l, rg_t, rg_r, rg_b;  // region bounds (CreateRectRgn)
  bool used;
};
constexpr int GDI_MAX_OBJECTS = 128;
gdi_obj g_objs[GDI_MAX_OBJECTS];

constexpr u32 GDI_HANDLE_TAG = 0x67430000u; // 'gC'

u32 handle_index(u32 h) { return h & 0xFFFFu; }

void* make_handle(int slot) {
  return (void*)(uintptr_t)(GDI_HANDLE_TAG | (u32)(slot + 1));
}

gdi_obj* obj_by_handle(u32 h) {
  if ((h & 0xFFFF0000u) != GDI_HANDLE_TAG) return nullptr;
  const u32 idx = handle_index(h);
  if (idx == 0 || idx > GDI_MAX_OBJECTS) return nullptr;
  gdi_obj* o = &g_objs[idx - 1];
  return o->used ? o : nullptr;
}

int obj_alloc(u32 type) {
  for (int i = 0; i < GDI_MAX_OBJECTS; ++i) {
    if (!g_objs[i].used) {
      memset(&g_objs[i], 0, sizeof(g_objs[i]));
      g_objs[i].used = true;
      g_objs[i].type = type;
      return i;
    }
  }
  return -1;
}

// ---- per-DC selection ----------------------------------------------------
// Real GDI keeps one current pen, brush, font, bitmap and palette per DC.
// The user32 DC record only carries a single "selected" slot, so a UI that
// selected a brush to fill a control erased the selected font and every later
// TextOut would measure with brush metrics. Keep the real per-type selection
// here and mirror the most recent one into the DC record for compatibility.
constexpr u32 STOCK_PEN_BLACK = 0x80000017u;
constexpr u32 STOCK_BRUSH_WHITE = 0x80000010u;
constexpr u32 STOCK_FONT_SYSTEM = 0x8000001Eu;

struct dc_selection {
  void* hdc;
  void* pen;
  void* brush;
  void* font;
  void* bitmap;
  void* palette;
  i32 cur_x, cur_y;    // current position (MoveToEx/LineTo)
  i32 vp_x, vp_y;      // viewport origin (SetViewportOrgEx), added to drawing
  u32 brush_color;     // SetDCBrushColor, for the DC_BRUSH stock object
  u32 pen_color;       // SetDCPenColor
  bool used;
};
constexpr int GDI_MAX_DC_STATE = 32;
dc_selection g_sel[GDI_MAX_DC_STATE];
std::mutex g_sel_mu;

dc_selection* dc_sel(void* hdc, bool create) {
  if (!hdc) return nullptr;
  for (int i = 0; i < GDI_MAX_DC_STATE; ++i)
    if (g_sel[i].used && g_sel[i].hdc == hdc) return &g_sel[i];
  if (!create) return nullptr;
  for (int i = 0; i < GDI_MAX_DC_STATE; ++i) {
    if (!g_sel[i].used) {
      memset(&g_sel[i], 0, sizeof(g_sel[i]));
      g_sel[i].used = true;
      g_sel[i].hdc = hdc;
      g_sel[i].pen = (void*)(uintptr_t)STOCK_PEN_BLACK;
      g_sel[i].brush = (void*)(uintptr_t)STOCK_BRUSH_WHITE;
      g_sel[i].font = (void*)(uintptr_t)STOCK_FONT_SYSTEM;
      // Real GDI hands a fresh memory DC a 1x1 monochrome bitmap, so
      // SelectObject(bitmap) has a non-NULL previous object.
      g_sel[i].bitmap = (void*)(uintptr_t)0x80000026u;
      g_sel[i].brush_color = 0x00FFFFFFu;
      g_sel[i].pen_color = 0x00000000u;
      return &g_sel[i];
    }
  }
  // Table full: recycle slot 0 with a named line rather than a wrong answer.
  log_line("gdi32: DC selection table full -> recycled slot 0");
  memset(&g_sel[0], 0, sizeof(g_sel[0]));
  g_sel[0].used = true;
  g_sel[0].hdc = hdc;
  g_sel[0].pen = (void*)(uintptr_t)STOCK_PEN_BLACK;
  g_sel[0].brush = (void*)(uintptr_t)STOCK_BRUSH_WHITE;
  g_sel[0].font = (void*)(uintptr_t)STOCK_FONT_SYSTEM;
  g_sel[0].bitmap = (void*)(uintptr_t)0x80000026u;
  g_sel[0].brush_color = 0x00FFFFFFu;
  return &g_sel[0];
}

void dc_sel_forget(void* hdc) {
  for (int i = 0; i < GDI_MAX_DC_STATE; ++i)
    if (g_sel[i].used && g_sel[i].hdc == hdc) g_sel[i].used = false;
}

// Colour of an object handle (brush/pen), with the stock defaults.
u32 obj_color(void* hobj, u32 fallback) {
  const u32 h = (u32)(uintptr_t)hobj;
  switch (h) {
    case 0x80000010u: return 0x00FFFFFFu;  // WHITE_BRUSH / WHITE_PEN
    case 0x80000011u: return 0x00C0C0C0u;  // LTGRAY_BRUSH
    case 0x80000012u: return 0x00808080u;  // GRAY_BRUSH
    case 0x80000013u: return 0x00404040u;  // DKGRAY_BRUSH
    case 0x80000014u: return 0x00000000u;  // BLACK_BRUSH
    case 0x80000005u: return fallback;     // NULL_BRUSH: caller skips the fill
    case 0x80000006u: return 0x00FFFFFFu;  // WHITE_PEN
    case 0x80000017u: return 0x00000000u;  // BLACK_PEN
    default: break;
  }
  gdi_obj* o = obj_by_handle(h);
  return o ? o->color : fallback;
}


// ---- path recording ------------------------------------------------------
// BeginPath/EndPath record geometry instead of painting it; FillPath,
// StrokePath and SelectClipPath replay it. GDI also records the shape entry
// points (Rectangle/Ellipse/RoundRect/Polygon/Arc) into an open path, which is
// how applications build rounded and elliptical clip regions.
constexpr u8 PATH_START = 2;  // vertex begins a subpath (PT_MOVETO)
constexpr u8 PATH_CLOSE = 1;  // vertex closes its subpath (PT_CLOSEFIGURE)
constexpr int GDI_MAX_PATHS = 32;

struct path_vertex {
  i32 x, y;
  u8 flags;
};

struct path_rec {
  void* hdc;
  bool used;
  bool open;      // between BeginPath and EndPath
  bool closed;    // EndPath succeeded: the path is replayable
  int fill_mode;  // 1 = ALTERNATE (default), 2 = WINDING
  i32 cur_x, cur_y;
  std::vector<path_vertex> v;
};
path_rec g_paths[GDI_MAX_PATHS];
std::mutex g_path_mu;

path_rec* path_find(void* hdc, bool create) {
  if (!hdc) return nullptr;
  std::lock_guard<std::mutex> lk(g_path_mu);
  for (int i = 0; i < GDI_MAX_PATHS; ++i)
    if (g_paths[i].used && g_paths[i].hdc == hdc) return &g_paths[i];
  if (!create) return nullptr;
  int slot = -1;
  for (int i = 0; i < GDI_MAX_PATHS; ++i)
    if (!g_paths[i].used) { slot = i; break; }
  if (slot < 0) {
    log_line("gdi32: path table full -> recycled slot 0");
    slot = 0;
  }
  path_rec* p = &g_paths[slot];
  p->used = true;
  p->hdc = hdc;
  p->open = false;
  p->closed = false;
  p->fill_mode = 1;
  p->cur_x = 0;
  p->cur_y = 0;
  p->v.clear();
  return p;
}

bool path_recording(void* hdc) {
  path_rec* p = path_find(hdc, false);
  return p && p->open;
}

void path_start(path_rec* p, i32 x, i32 y) {
  if (!p || !p->open) return;
  path_vertex v;
  v.x = x;
  v.y = y;
  v.flags = PATH_START;
  p->v.push_back(v);
  p->cur_x = x;
  p->cur_y = y;
}

void path_to(path_rec* p, i32 x, i32 y) {
  if (!p || !p->open) return;
  if (p->v.empty()) {  // GDI starts an implicit subpath here
    path_start(p, x, y);
    return;
  }
  path_vertex v;
  v.x = x;
  v.y = y;
  v.flags = 0;
  p->v.push_back(v);
  p->cur_x = x;
  p->cur_y = y;
}

void path_close(path_rec* p) {
  if (!p || !p->open || p->v.empty()) return;
  p->v.back().flags |= PATH_CLOSE;
}

void path_record_rect(path_rec* p, i32 l, i32 t, i32 r, i32 b) {
  path_start(p, l, t);
  path_to(p, r, t);
  path_to(p, r, b);
  path_to(p, l, b);
  path_close(p);
}

// Ellipses are recorded as flattened polygons: the raster surface has no
// curve machinery, and filling/stroking the flattened outline is what the
// pixels would look like anyway at these sizes.
void path_record_ellipse(path_rec* p, i32 l, i32 t, i32 r, i32 b) {
  const float cx = ((float)l + (float)r) / 2.0f;
  const float cy = ((float)t + (float)b) / 2.0f;
  const float rx = ((float)r - (float)l) / 2.0f;
  const float ry = ((float)b - (float)t) / 2.0f;
  if (rx <= 0.0f || ry <= 0.0f) return;
  const float rad = rx > ry ? rx : ry;
  int steps = (int)(rad * 2.0f) + 16;
  if (steps < 16) steps = 16;
  if (steps > 720) steps = 720;
  for (int i = 0; i <= steps; ++i) {
    const float ang = 6.28318530718f * (float)i / (float)steps;
    const i32 x = (i32)(cx + rx * cosf(ang));
    const i32 y = (i32)(cy + ry * sinf(ang));
    if (i == 0) path_start(p, x, y);
    else path_to(p, x, y);
  }
  path_close(p);
}

// mode 0 = arc, 1 = pie (through the centre), 2 = chord.
void path_record_arc(path_rec* p, i32 l, i32 t, i32 r, i32 b, i32 sx, i32 sy,
                     i32 ex, i32 ey, int mode) {
  const float cx = ((float)l + (float)r) / 2.0f;
  const float cy = ((float)t + (float)b) / 2.0f;
  const float rx = ((float)r - (float)l) / 2.0f;
  const float ry = ((float)b - (float)t) / 2.0f;
  if (rx <= 0.0f || ry <= 0.0f) return;
  constexpr float kPi = 3.14159265358979f;
  const float a0 = atan2f(((float)sy - cy) / ry, ((float)sx - cx) / rx);
  const float a1 = atan2f(((float)ey - cy) / ry, ((float)ex - cx) / rx);
  float sweep = a1 - a0;
  while (sweep <= 0.0f) sweep += 2.0f * kPi;
  const int steps = (int)((rx > ry ? rx : ry) * 4.0f) + 8;
  for (int i = 0; i <= steps; ++i) {
    const float ang = a0 + sweep * (float)i / (float)steps;
    const i32 x = (i32)(cx + rx * cosf(ang));
    const i32 y = (i32)(cy + ry * sinf(ang));
    if (i == 0) path_start(p, x, y);
    else path_to(p, x, y);
  }
  if (mode == 1) {
    path_to(p, (i32)cx, (i32)cy);
    path_close(p);
  } else if (mode == 2) {
    path_close(p);
  }
}

// Flat sides plus four elliptical corner arcs (ew x eh corner ellipse).
void path_record_round_rect(path_rec* p, i32 l, i32 t, i32 r, i32 b, i32 ew,
                            i32 eh) {
  if (r < l) { const i32 tmp = l; l = r; r = tmp; }
  if (b < t) { const i32 tmp = t; t = b; b = tmp; }
  const float rx = (float)(ew > 0 ? ew : 1) / 2.0f;
  const float ry = (float)(eh > 0 ? eh : 1) / 2.0f;
  if (rx * 2.0f > (float)(r - l)) return;  // degenerate: keep the caller's pixels
  if (ry * 2.0f > (float)(b - t)) return;
  constexpr float kPi = 3.14159265358979f;
  auto corner = [&](float ccx, float ccy, float a0, float a1, bool first) {
    const int steps = 12;
    for (int i = 0; i <= steps; ++i) {
      const float ang = a0 + (a1 - a0) * (float)i / (float)steps;
      const i32 x = (i32)(ccx + rx * cosf(ang));
      const i32 y = (i32)(ccy + ry * sinf(ang));
      if (first && i == 0) path_start(p, x, y);
      else path_to(p, x, y);
    }
  };
  const float crx = (float)r - rx, clx = (float)l + rx;
  const float cby = (float)b - ry, cty = (float)t + ry;
  corner(crx, cty, -kPi / 2.0f, 0.0f, true);   // top-right
  corner(crx, cby, 0.0f, kPi / 2.0f, false);   // bottom-right
  corner(clx, cby, kPi / 2.0f, kPi, false);    // bottom-left
  corner(clx, cty, kPi, kPi * 1.5f, false);    // top-left
  path_close(p);
}
} // namespace

extern "C" {

// Called from the DC release path (bridge_user32.cpp) so a released DC drops
// its selection, saved states and raster surface instead of leaking them.
void trt_gdi_forget_dc(void* hdc) {
  {
    std::lock_guard<std::mutex> lk(g_sel_mu);
    dc_sel_forget(hdc);
  }
  {
    std::lock_guard<std::mutex> lk(g_path_mu);
    for (int i = 0; i < GDI_MAX_PATHS; ++i) {
      if (g_paths[i].used && g_paths[i].hdc == hdc) {
        g_paths[i].v.clear();
        g_paths[i].used = false;
        g_paths[i].open = false;
        g_paths[i].closed = false;
      }
    }
  }
  dc_saved_forget(hdc);
  trt_raster::release(hdc);
}

void trt_gdi_surface_hint(void* hdc, i32 w, i32 h) {
  trt_raster::get(hdc, w, h);
}

u32 trt_gdi_brush_color(void* hbrush) {
  // NULL_BRUSH/stock colours resolve through obj_color; an unknown handle
  // paints nothing visible, so it falls back to white rather than black.
  return obj_color(hbrush, 0x00FFFFFFu);
}

void trt_gdi_forget_draw_state(void* hdc) {
  {
    std::lock_guard<std::mutex> lk(g_sel_mu);
    dc_sel_forget(hdc);
  }
  dc_saved_forget(hdc);
  const i32 w = trt_raster::width(hdc);
  const i32 h = trt_raster::height(hdc);
  if (w > 0 && h > 0) trt_raster::set_clip(hdc, 0, 0, w, h);
}

// ---- objects ----

void* TRT_GUEST_ABI trt_CreateFontIndirectW(const void* logfont) {
  if (!logfont) {
    log_line("gdi32: CreateFontIndirectW(NULL) -> NULL");
    return nullptr;
  }
  const u8* lf = (const u8*)logfont;
  const i32 height = (i32)((u32)lf[0] | ((u32)lf[1] << 8) | ((u32)lf[2] << 16) |
                           ((u32)lf[3] << 24));
  const int slot = obj_alloc(TRT_GDI_OBJ_FONT);
  if (slot < 0) {
    log_line("gdi32: object table full -> NULL");
    return nullptr;
  }
  g_objs[slot].size = LF_BYTES;
  memcpy(g_objs[slot].data, lf, LF_BYTES);
  void* h = make_handle(slot);
  char line[128];
  snprintf(line, sizeof(line),
           "gdi32: CreateFontIndirectW h=%p lfHeight=%d -> font object", h,
           height);
  log_line(line);
  return h;
}

void* TRT_GUEST_ABI trt_CreateFontIndirectA(const void* logfont) {
  if (!logfont) return nullptr;
  u8 lfw[LF_BYTES] = {};
  memcpy(lfw, logfont, 28);
  const char* face = (const char*)logfont + 28;
  u16* wface = (u16*)(lfw + 28);
  for (int i = 0; i < 31 && face[i]; ++i) wface[i] = (u16)(u8)face[i];
  return trt_CreateFontIndirectW(lfw);
}

void* TRT_GUEST_ABI trt_CreateFontW(int cHeight, int cWidth, int cEscapement,
                                    int cOrientation, int cWeight, u32 bItalic,
                                    u32 bUnderline, u32 bStrikeOut,
                                    u32 iCharSet, u32 iOutPrecision,
                                    u32 iClipPrecision, u32 iQuality,
                                    u32 iPitchAndFamily, const u16* pszFaceName) {
  (void)bItalic; (void)bUnderline; (void)bStrikeOut; (void)iCharSet;
  (void)iOutPrecision; (void)iClipPrecision; (void)iQuality;
  (void)iPitchAndFamily; (void)pszFaceName;
  u8 lfw[LF_BYTES] = {};
  *(i32*)(lfw + 0) = cHeight;
  *(i32*)(lfw + 4) = cWidth;
  *(i32*)(lfw + 8) = cEscapement;
  *(i32*)(lfw + 12) = cOrientation;
  *(i32*)(lfw + 16) = cWeight;
  return trt_CreateFontIndirectW(lfw);
}

void* TRT_GUEST_ABI trt_CreateFontA(int cHeight, int cWidth, int cEscapement,
                                    int cOrientation, int cWeight, u32 bItalic,
                                    u32 bUnderline, u32 bStrikeOut,
                                    u32 iCharSet, u32 iOutPrecision,
                                    u32 iClipPrecision, u32 iQuality,
                                    u32 iPitchAndFamily, const char* pszFaceName) {
  (void)pszFaceName;
  return trt_CreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight,
                         bItalic, bUnderline, bStrikeOut, iCharSet,
                         iOutPrecision, iClipPrecision, iQuality,
                         iPitchAndFamily, nullptr);
}

void* TRT_GUEST_ABI trt_CreateSolidBrush(u32 color) {
  const int slot = obj_alloc(TRT_GDI_OBJ_BRUSH);
  if (slot < 0) return nullptr;
  g_objs[slot].color = color;
  return make_handle(slot);
}

void* TRT_GUEST_ABI trt_CreateBrushIndirect(const void* plbrush) {
  u32 color = 0;
  if (plbrush) color = *((const u32*)plbrush + 1);
  return trt_CreateSolidBrush(color);
}
void* TRT_GUEST_ABI trt_CreatePatternBrush(void* hbm) {
  (void)hbm;
  return trt_CreateSolidBrush(0);
}
void* TRT_GUEST_ABI trt_CreateDIBPatternBrushPt(const void* lpPackedDIB, u32 iUsage) {
  (void)lpPackedDIB; (void)iUsage;
  return trt_CreateSolidBrush(0);
}

void* TRT_GUEST_ABI trt_CreatePen(int iStyle, int cWidth, u32 color) {
  (void)iStyle; (void)cWidth;
  const int slot = obj_alloc(TRT_GDI_OBJ_PEN);
  if (slot < 0) return nullptr;
  g_objs[slot].color = color;
  return make_handle(slot);
}
void* TRT_GUEST_ABI trt_CreatePenIndirect(const void* plpen) {
  (void)plpen;
  return trt_CreatePen(0, 1, 0);
}
void* TRT_GUEST_ABI trt_ExtCreatePen(u32 iPenStyle, u32 cWidth,
                                     const void* plbrush, u32 cStyle,
                                     const u32* pstyle) {
  (void)iPenStyle; (void)cWidth; (void)plbrush; (void)cStyle; (void)pstyle;
  return trt_CreatePen(0, 1, 0);
}

void* TRT_GUEST_ABI trt_CreateRectRgn(i32 a, i32 b, i32 c, i32 d) {
  const int slot = obj_alloc(TRT_GDI_OBJ_REGION);
  if (slot < 0) return nullptr;
  g_objs[slot].rg_l = a;
  g_objs[slot].rg_t = b;
  g_objs[slot].rg_r = c;
  g_objs[slot].rg_b = d;
  return make_handle(slot);
}
void* TRT_GUEST_ABI trt_CreateRectRgnIndirect(const void* lprect) {
  if (!lprect) return trt_CreateRectRgn(0, 0, 0, 0);
  const i32* r = (const i32*)lprect;
  return trt_CreateRectRgn(r[0], r[1], r[2], r[3]);
}
void* TRT_GUEST_ABI trt_CreatePolygonRgn(const void* pptl, int cPoint, int iMode) {
  (void)pptl; (void)cPoint; (void)iMode;
  return trt_CreateRectRgn(0, 0, 0, 0);
}
void* TRT_GUEST_ABI trt_CreateEllipticRgn(int x1, int y1, int x2, int y2) {
  return trt_CreateRectRgn(x1, y1, x2, y2);
}
int TRT_GUEST_ABI trt_CombineRgn(void* hrgnDst, void* hrgnSrc1, void* hrgnSrc2, int iMode) {
  (void)hrgnDst; (void)hrgnSrc1; (void)hrgnSrc2; (void)iMode;
  return 2;  // SIMPLEREGION
}
int TRT_GUEST_ABI trt_OffsetRgn(void* hrgn, int x, int y) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hrgn);
  if (!o || o->type != TRT_GDI_OBJ_REGION) {
    log_line("gdi32: OffsetRgn(bad handle) -> ERROR");
    return 0;
  }
  o->rg_l += x; o->rg_r += x;
  o->rg_t += y; o->rg_b += y;
  return 2;
}
int TRT_GUEST_ABI trt_PtInRegion(void* hrgn, int x, int y) {
  (void)hrgn; (void)x; (void)y;
  return 1;
}
int TRT_GUEST_ABI trt_RectInRegion(void* hrgn, const void* lprect) {
  (void)hrgn; (void)lprect;
  return 1;
}
int TRT_GUEST_ABI trt_GetRgnBox(void* hrgn, void* lprc) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hrgn);
  if (!o || o->type != TRT_GDI_OBJ_REGION) {
    log_line("gdi32: GetRgnBox(bad handle) -> ERROR");
    return 0;
  }
  if (lprc) {
    i32* r = (i32*)lprc;
    r[0] = o->rg_l; r[1] = o->rg_t; r[2] = o->rg_r; r[3] = o->rg_b;
  }
  if (o->rg_r <= o->rg_l || o->rg_b <= o->rg_t) return 1;  // NULLREGION
  return 2;
}

void* TRT_GUEST_ABI trt_GetStockObject(i32 obj) {
  switch (obj) {
    case 0:  return (void*)(uintptr_t)0x80000010u;  // WHITE_BRUSH
    case 1:  return (void*)(uintptr_t)0x80000011u;  // LTGRAY_BRUSH
    case 2:  return (void*)(uintptr_t)0x80000012u;  // GRAY_BRUSH
    case 3:  return (void*)(uintptr_t)0x80000013u;  // DKGRAY_BRUSH
    case 4:  return (void*)(uintptr_t)0x80000014u;  // BLACK_BRUSH
    case 5:  return (void*)(uintptr_t)0x80000005u;  // NULL_BRUSH
    case 6:  return (void*)(uintptr_t)0x80000006u;  // WHITE_PEN
    case 7:  return (void*)(uintptr_t)0x80000017u;  // BLACK_PEN
    case 8:  return (void*)(uintptr_t)0x80000018u;  // NULL_PEN
    case 10: return (void*)(uintptr_t)0x80000019u;  // OEM_FIXED_FONT
    case 11: return (void*)(uintptr_t)0x8000001Bu;  // ANSI_FIXED_FONT
    case 12: return (void*)(uintptr_t)0x8000001Cu;  // ANSI_VAR_FONT
    case 13: return (void*)(uintptr_t)0x8000001Du;  // SYSTEM_FONT
    case 14: return (void*)(uintptr_t)0x8000001Eu;  // DEVICE_DEFAULT_FONT
    case 15: return (void*)(uintptr_t)0x8000001Fu;  // DEFAULT_PALETTE
    case 16: return (void*)(uintptr_t)0x80000020u;  // SYSTEM_FIXED_FONT
    case 17: return (void*)(uintptr_t)0x80000021u;  // DEFAULT_GUI_FONT
    case 18: return (void*)(uintptr_t)0x80000022u;  // DC_BRUSH
    case 19: return (void*)(uintptr_t)0x80000023u;  // DC_PEN
    default: {
      char line[96];
      snprintf(line, sizeof(line),
               "gdi32: GetStockObject(%d) unknown -> NULL", obj);
      log_line(line);
      return nullptr;
    }
  }
}

// ---- DCs & Bitmaps / DIBs ----

void* TRT_GUEST_ABI trt_CreateCompatibleDC(void* hdc) {
  (void)hdc;
  return trt_user32_dc_alloc();
}
void* TRT_GUEST_ABI trt_CreateDCW(const u16* pwszDriver, const u16* pwszDevice,
                                  const u16* pszPort, const void* pdm) {
  (void)pwszDriver; (void)pwszDevice; (void)pszPort; (void)pdm;
  return trt_user32_dc_alloc();
}
void* TRT_GUEST_ABI trt_CreateDCA(const char* pszDriver, const char* pszDevice,
                                  const char* pszPort, const void* pdm) {
  (void)pszDriver; (void)pszDevice; (void)pszPort; (void)pdm;
  return trt_user32_dc_alloc();
}
int TRT_GUEST_ABI trt_DeleteDC(void* hdc) {
  trt_gdi_forget_dc(hdc);
  return trt_user32_dc_release(hdc);
}
// ---- DC state stack ----------------------------------------------------
// SaveDC/RestoreDC used to log that nothing was saved while still returning
// success, so a dialog that set a font, painted, and restored got the wrong
// font on the next paint. This keeps a real per-DC stack of the selectable
// objects, text attributes, current position, viewport origin and clip.
struct dc_saved {
  void* hdc;
  void* pen;
  void* brush;
  void* font;
  i32 cur_x, cur_y, vp_x, vp_y;
  u32 brush_color, pen_color;
  trt_dc_text_state ts;
  i32 clip[4];
  bool has_clip;
  int level;
};
std::vector<dc_saved> g_saved;
std::mutex g_saved_mu;

void dc_saved_forget(void* hdc) {
  std::lock_guard<std::mutex> lk(g_saved_mu);
  for (size_t i = g_saved.size(); i-- > 0;)
    if (g_saved[i].hdc == hdc) g_saved.erase(g_saved.begin() + (long)i);
}

int TRT_GUEST_ABI trt_SaveDC(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: SaveDC -> 0 (not a user32 DC)");
    return 0;
  }
  dc_saved f{};
  f.hdc = hdc;
  {
    std::lock_guard<std::mutex> lk(g_sel_mu);
    dc_selection* s = dc_sel(hdc, true);
    if (!s) return 0;
    f.pen = s->pen;
    f.brush = s->brush;
    f.font = s->font;
    f.cur_x = s->cur_x;
    f.cur_y = s->cur_y;
    f.vp_x = s->vp_x;
    f.vp_y = s->vp_y;
    f.brush_color = s->brush_color;
    f.pen_color = s->pen_color;
  }
  trt_dc_text_state ts;
  if (!dc_text_state(hdc, &ts)) return 0;
  f.ts = ts;
  f.has_clip = trt_raster::clip_box(hdc, &f.clip[0], &f.clip[1], &f.clip[2],
                                    &f.clip[3]);
  std::lock_guard<std::mutex> lk(g_saved_mu);
  int level = 1;
  for (const dc_saved& e : g_saved)
    if (e.hdc == hdc && e.level >= level) level = e.level + 1;
  f.level = level;
  g_saved.push_back(f);
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SaveDC -> level %d", level);
  log_line(line);
  return level;
}

int TRT_GUEST_ABI trt_RestoreDC(void* hdc, int nSavedDC) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: RestoreDC -> FALSE (not a user32 DC)");
    return 0;
  }
  dc_saved f{};
  bool found = false;
  {
    std::lock_guard<std::mutex> lk(g_saved_mu);
    if (nSavedDC < 0) {  // most recent state for this DC
      for (size_t i = g_saved.size(); i-- > 0;) {
        if (g_saved[i].hdc == hdc) {
          f = g_saved[i];
          g_saved.erase(g_saved.begin() + (long)i);
          found = true;
          break;
        }
      }
    } else {
      for (size_t i = g_saved.size(); i-- > 0;) {
        if (g_saved[i].hdc == hdc && g_saved[i].level == nSavedDC) {
          f = g_saved[i];
          g_saved.erase(g_saved.begin() + (long)i, g_saved.end());
          found = true;
          break;
        }
      }
    }
  }
  if (!found) {
    char line[112];
    snprintf(line, sizeof(line),
             "gdi32: RestoreDC(%d) -> FALSE (no saved state at that level)",
             nSavedDC);
    log_line(line);
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_sel_mu);
    dc_selection* s = dc_sel(hdc, true);
    if (s) {
      s->pen = f.pen;
      s->brush = f.brush;
      s->font = f.font;
      s->cur_x = f.cur_x;
      s->cur_y = f.cur_y;
      s->vp_x = f.vp_x;
      s->vp_y = f.vp_y;
      s->brush_color = f.brush_color;
      s->pen_color = f.pen_color;
    }
  }
  trt_user32_dc_set_text_state(hdc, &f.ts);
  if (f.has_clip)
    trt_raster::set_clip(hdc, f.clip[0], f.clip[1], f.clip[2], f.clip[3]);
  char line[96];
  snprintf(line, sizeof(line), "gdi32: RestoreDC(level %d) restored",
           f.level);
  log_line(line);
  return 1;
}

void* TRT_GUEST_ABI trt_CreateBitmap(int nWidth, int nHeight, u32 nPlanes,
                                     u32 nBitCount, const void* lpBits) {
  (void)nPlanes;
  const int slot = obj_alloc(TRT_GDI_OBJ_BITMAP);
  if (slot < 0) return nullptr;
  i32 w = nWidth > 0 ? nWidth : 1;
  i32 h = nHeight < 0 ? -nHeight : (nHeight > 0 ? nHeight : 1);
  u16 bpp = (u16)(nBitCount ? nBitCount : 32);
  size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
  size_t bytes = stride * (size_t)h;
  void* buf = calloc(1, bytes ? bytes : 4);
  if (buf && lpBits && bytes) memcpy(buf, lpBits, bytes);
  g_objs[slot].bm_w = w;
  g_objs[slot].bm_h = h;
  g_objs[slot].bm_bpp = bpp;
  g_objs[slot].bm_bits = buf;
  return make_handle(slot);
}

void* TRT_GUEST_ABI trt_CreateCompatibleBitmap(void* hdc, int cx, int cy) {
  (void)hdc;
  return trt_CreateBitmap(cx, cy, 1, 32, nullptr);
}

void* TRT_GUEST_ABI trt_CreateDIBSection(void* hdc, const void* pbmi,
                                         u32 usage, void** ppvBits,
                                         void* hSection, u32 offset) {
  (void)hdc; (void)usage; (void)hSection; (void)offset;
  i32 w = 64, h = 64;
  u16 bpp = 32;
  if (pbmi) {
    // BITMAPINFOHEADER: biSize(0), biWidth(4), biHeight(8), biPlanes(12), biBitCount(14)
    const u8* b = (const u8*)pbmi;
    w = *(const i32*)(b + 4);
    h = *(const i32*)(b + 8);
    bpp = *(const u16*)(b + 14);
    if (w <= 0) w = 1;
    if (h < 0) h = -h;
    if (h == 0) h = 1;
    if (bpp == 0) bpp = 32;
  }
  const int slot = obj_alloc(TRT_GDI_OBJ_BITMAP);
  if (slot < 0) {
    if (ppvBits) *ppvBits = nullptr;
    return nullptr;
  }
  size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
  size_t bytes = stride * (size_t)h;
  void* buf = calloc(1, bytes ? bytes : 4);
  g_objs[slot].bm_w = w;
  g_objs[slot].bm_h = h;
  g_objs[slot].bm_bpp = bpp;
  g_objs[slot].bm_bits = buf;
  if (ppvBits) *ppvBits = buf;
  return make_handle(slot);
}

int TRT_GUEST_ABI trt_GetDIBits(void* hdc, void* hbm, u32 start, u32 cLines,
                                void* lpvBits, void* lpbmi, u32 usage) {
  (void)hdc; (void)start; (void)usage;
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hbm);
  if (!o || o->type != TRT_GDI_OBJ_BITMAP) return 0;
  if (lpbmi) {
    u8* b = (u8*)lpbmi;
    *(i32*)(b + 4) = o->bm_w;
    *(i32*)(b + 8) = o->bm_h;
    *(u16*)(b + 12) = 1;
    *(u16*)(b + 14) = o->bm_bpp;
  }
  if (lpvBits && o->bm_bits && cLines > 0) {
    u32 lines = cLines < (u32)o->bm_h ? cLines : (u32)o->bm_h;
    size_t stride = (((size_t)o->bm_w * o->bm_bpp + 31) / 32) * 4;
    memcpy(lpvBits, o->bm_bits, stride * lines);
    return (int)lines;
  }
  return o->bm_h;
}

int TRT_GUEST_ABI trt_SetDIBits(void* hdc, void* hbm, u32 start, u32 cLines,
                                const void* lpBits, const void* lpbmi, u32 fuColorUse) {
  (void)hdc; (void)start; (void)lpbmi; (void)fuColorUse;
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hbm);
  if (!o || o->type != TRT_GDI_OBJ_BITMAP || !lpBits || !o->bm_bits) return 0;
  u32 lines = cLines < (u32)o->bm_h ? cLines : (u32)o->bm_h;
  size_t stride = (((size_t)o->bm_w * o->bm_bpp + 31) / 32) * 4;
  memcpy(o->bm_bits, lpBits, stride * lines);
  return (int)lines;
}

int TRT_GUEST_ABI trt_SetDIBitsToDevice(void* hdc, int xDest, int yDest, u32 w,
                                        u32 h, int xSrc, int ySrc, u32 StartScan,
                                        u32 cLines, const void* lpvBits,
                                        const void* lpbmi, u32 ColorUse) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: SetDIBitsToDevice -> 0 (not a user32 DC)");
    return 0;
  }
  const i32 lines = h ? (i32)h : (i32)cLines;
  // StartScan is the first DIB scan line the call covers; fold it into the
  // source origin, which is what it means for a single-band blit like this.
  const i32 sy = ySrc + (i32)StartScan;
  if (!trt_raster::dib(hdc, xDest, yDest, (i32)w, lines, lpvBits, lpbmi,
                       ColorUse, xSrc, sy, (i32)w, lines, 0x00CC0020u)) {
    char line[176];
    snprintf(line, sizeof(line),
             "gdi32: SetDIBitsToDevice %ux%u @(%d,%d) -> 0 (DIB format "
             "not decoded: fail-visible)", w, h, xDest, yDest);
    log_line(line);
    return 0;
  }
  return (int)(cLines ? cLines : h);
}

int TRT_GUEST_ABI trt_StretchDIBits(void* hdc, int xDest, int yDest,
                                    int DestWidth, int DestHeight, int xSrc,
                                    int ySrc, int SrcWidth, int SrcHeight,
                                    const void* lpBits, const void* lpbmi,
                                    u32 iUsage, u32 rop) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: StretchDIBits -> 0 (not a user32 DC)");
    return 0;
  }
  if (!trt_raster::dib(hdc, xDest, yDest, DestWidth, DestHeight, lpBits, lpbmi,
                       iUsage, xSrc, ySrc, SrcWidth, SrcHeight, rop)) {
    char line[176];
    snprintf(line, sizeof(line),
             "gdi32: StretchDIBits %dx%d -> %dx%d @(%d,%d) -> 0 (DIB format "
             "or ROP 0x%x not decoded: fail-visible)",
             SrcWidth, SrcHeight, DestWidth, DestHeight, xDest, yDest, rop);
    log_line(line);
    return 0;
  }
  return SrcHeight;
}

// Current brush/pen colour for a DC (stock objects included). DC_BRUSH and
// DC_PEN resolve to the per-DC colour SetDCBrushColor/SetDCPenColor set.
static u32 dc_brush_color(void* hdc) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0x00FFFFFFu;
  if ((u32)(uintptr_t)s->brush == 0x80000022u) return s->brush_color;  // DC_BRUSH
  return obj_color(s->brush, s->brush_color);
}
static u32 dc_pen_color(void* hdc) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0x00000000u;
  if ((u32)(uintptr_t)s->pen == 0x80000023u) return s->pen_color;  // DC_PEN
  return obj_color(s->pen, s->pen_color);
}
static bool dc_brush_is_null(void* hdc) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  return s && (u32)(uintptr_t)s->brush == 0x80000005u;  // NULL_BRUSH
}
static bool dc_pen_is_null(void* hdc) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  return s && (u32)(uintptr_t)s->pen == 0x80000018u;  // NULL_PEN
}

int TRT_GUEST_ABI trt_BitBlt(void* hdc, int x, int y, int cx, int cy,
                             void* hdcSrc, int x1, int y1, u32 rop) {
  if (!trt_user32_dc_is_valid(hdc) || !trt_user32_dc_is_valid(hdcSrc)) {
    log_line("gdi32: BitBlt -> FALSE (destination or source is not a DC)");
    return 0;
  }
  if (!trt_raster::blit(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop,
                        dc_brush_color(hdc))) {
    char line[144];
    snprintf(line, sizeof(line),
             "gdi32: BitBlt -> FALSE (ROP 0x%x not implemented by the "
             "raster surface)", rop);
    log_line(line);
    return 0;
  }
  char done[144];
  snprintf(done, sizeof(done),
           "gdi32: BitBlt %dx%d @(%d,%d) rop=0x%x (drawn)", cx, cy, x, y, rop);
  log_line(done);
  return 1;
}
int TRT_GUEST_ABI trt_StretchBlt(void* hdcDest, int xDest, int yDest, int wDest,
                                 int hDest, void* hdcSrc, int xSrc, int ySrc,
                                 int wSrc, int hSrc, u32 rop) {
  if (!trt_user32_dc_is_valid(hdcDest) || !trt_user32_dc_is_valid(hdcSrc)) {
    log_line("gdi32: StretchBlt -> FALSE (destination or source is not a DC)");
    return 0;
  }
  if (!trt_raster::stretch(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc,
                           ySrc, wSrc, hSrc, rop, dc_brush_color(hdcDest))) {
    char line[160];
    snprintf(line, sizeof(line),
             "gdi32: StretchBlt -> FALSE (ROP 0x%x or a zero-size source "
             "not implemented by the raster surface)", rop);
    log_line(line);
    return 0;
  }
  char done[176];
  snprintf(done, sizeof(done),
           "gdi32: StretchBlt %dx%d -> %dx%d @(%d,%d) rop=0x%x (drawn)",
           wSrc, hSrc, wDest, hDest, xDest, yDest, rop);
  log_line(done);
  return 1;
}
// ---- stretch-blit mode (per-DC GDI state) ----
// Real GDI keeps a stretch mode per DC, defaulting to COLORONCOLOR (3).
// Games set HALFTONE before scaling a loading screen and read it back; an
// unresolved SetStretchBltMode is a LOAD-time import failure (measured on
// binkw32.dll), not a cosmetic gap. The mode is recorded — the blit itself
// already names itself as not drawn.
struct stretch_mode_rec {
  void* hdc;
  i32 mode;
};
constexpr int GDI_MAX_STRETCH_DCS = 32;
constexpr i32 GDI_STRETCH_DEFAULT = 3;  // COLORONCOLOR
stretch_mode_rec g_stretch[GDI_MAX_STRETCH_DCS];
std::mutex g_stretch_mu;

int TRT_GUEST_ABI trt_SetStretchBltMode(void* hdc, i32 mode) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: SetStretchBltMode -> 0 (not a user32 DC)");
    return 0;
  }
  if (mode < 1 || mode > 4) {  // BLACKONWHITE..HALFTONE
    char line[112];
    snprintf(line, sizeof(line),
             "gdi32: SetStretchBltMode mode=%d -> 0 (invalid mode, want 1..4)",
             mode);
    log_line(line);
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_stretch_mu);
  for (int i = 0; i < GDI_MAX_STRETCH_DCS; ++i) {
    if (g_stretch[i].hdc == hdc) {
      const i32 prev = g_stretch[i].mode;
      g_stretch[i].mode = mode;
      return (int)prev;
    }
  }
  for (int i = 0; i < GDI_MAX_STRETCH_DCS; ++i) {
    if (!g_stretch[i].hdc) {
      g_stretch[i].hdc = hdc;
      g_stretch[i].mode = mode;
      return (int)GDI_STRETCH_DEFAULT;
    }
  }
  // table full: recycle slot 0 with a named line (never a silent wrong mode)
  g_stretch[0].hdc = hdc;
  g_stretch[0].mode = mode;
  log_line("gdi32: SetStretchBltMode DC table full -> recycled slot 0");
  return (int)GDI_STRETCH_DEFAULT;
}

i32 TRT_GUEST_ABI trt_GetStretchBltMode(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: GetStretchBltMode -> 0 (not a user32 DC)");
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_stretch_mu);
  for (int i = 0; i < GDI_MAX_STRETCH_DCS; ++i)
    if (g_stretch[i].hdc == hdc) return g_stretch[i].mode;
  return GDI_STRETCH_DEFAULT;
}

int TRT_GUEST_ABI trt_PatBlt(void* hdc, int x, int y, int w, int h, u32 rop) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: PatBlt -> FALSE (not a user32 DC)");
    return 0;
  }
  switch (rop) {
    case 0x00F00021u:  // PATCOPY
      trt_raster::fill(hdc, x, y, x + w, y + h, dc_brush_color(hdc));
      return 1;
    case 0x00000042u:  // BLACKNESS
      trt_raster::fill(hdc, x, y, x + w, y + h, 0x00000000u);
      return 1;
    case 0x00FF0062u:  // WHITENESS
      trt_raster::fill(hdc, x, y, x + w, y + h, 0x00FFFFFFu);
      return 1;
    case 0x00550009u:  // DSTINVERT
      return trt_raster::blit(hdc, x, y, w, h, hdc, x, y, rop, 0) ? 1 : 0;
    case 0x005A0049u:  // PATINVERT
      return trt_raster::blit(hdc, x, y, w, h, hdc, x, y, rop,
                              dc_brush_color(hdc)) ? 1 : 0;
    default: {
      char line[144];
      snprintf(line, sizeof(line),
               "gdi32: PatBlt -> FALSE (ROP 0x%x not implemented by the "
               "raster surface)", rop);
      log_line(line);
      return 0;
    }
  }
}
u32 TRT_GUEST_ABI trt_GetPixel(void* hdc, int x, int y) {
  bool ok = false;
  const u32 c = trt_raster::get_pixel(hdc, x, y, &ok);
  if (!ok) {
    char line[112];
    snprintf(line, sizeof(line),
             "gdi32: GetPixel(%d,%d) -> CLR_INVALID (no surface or outside)",
             x, y);
    log_line(line);
    return 0xFFFFFFFFu;
  }
  return c;
}
u32 TRT_GUEST_ABI trt_SetPixel(void* hdc, int x, int y, u32 color) {
  if (!trt_raster::set_pixel(hdc, x, y, color)) {
    char line[112];
    snprintf(line, sizeof(line),
             "gdi32: SetPixel(%d,%d) -> CLR_INVALID (no surface or outside)",
             x, y);
    log_line(line);
    return 0xFFFFFFFFu;
  }
  return color;
}

// ---- DC-coupled ----

void* TRT_GUEST_ABI trt_SelectObject(void* hdc, void* hobj) {
  if (!trt_user32_dc_is_valid(hdc)) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: SelectObject(hdc=%p) not a user32 DC -> NULL", hdc);
    log_line(line);
    return nullptr;
  }
  const u32 h = (u32)(uintptr_t)hobj;
  const bool is_stock = (h & 0xFFFF0000u) == 0x80000000u;
  gdi_obj* o = is_stock ? nullptr : obj_by_handle(h);
  if (!is_stock && !o) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: SelectObject bad object handle 0x%x -> NULL", h);
    log_line(line);
    return nullptr;
  }
  // Object type: from the record, or from the stock ordinal.
  u32 type = o ? o->type : 0;
  if (!o) {
    const u32 ord = h & 0xFFFFu;
    if (ord >= 0x10u && ord <= 0x14u) type = TRT_GDI_OBJ_BRUSH;
    else if (ord == 6u || ord == 0x17u || ord == 0x18u) type = TRT_GDI_OBJ_PEN;
    else if (ord >= 0x19u && ord <= 0x21u) type = TRT_GDI_OBJ_FONT;
  }
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return nullptr;
  void* prev = nullptr;
  switch (type) {
    case TRT_GDI_OBJ_PEN: prev = s->pen; s->pen = hobj; break;
    case TRT_GDI_OBJ_BRUSH: prev = s->brush; s->brush = hobj; break;
    case TRT_GDI_OBJ_BITMAP: prev = s->bitmap; s->bitmap = hobj; break;
    case TRT_GDI_OBJ_PALETTE: prev = s->palette; s->palette = hobj; break;
    default: prev = s->font; s->font = hobj; break;
  }
  // The user32 DC record keeps the most recent selection for compatibility.
  trt_user32_dc_set_selected(hdc, hobj);
  return prev;
}

int TRT_GUEST_ABI trt_DeleteObject(void* hobj) {
  const u32 h = (u32)(uintptr_t)hobj;
  if ((h & 0xFFFF0000u) == 0x80000000u) return 1;  // stock objects
  gdi_obj* o = obj_by_handle(h);
  if (!o) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: DeleteObject bad/stale handle 0x%x -> FALSE", h);
    log_line(line);
    return 0;
  }
  if (o->bm_bits) {
    free(o->bm_bits);
    o->bm_bits = nullptr;
  }
  o->used = false;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: DeleteObject h=0x%x -> TRUE (freed)", h);
  log_line(line);
  return 1;
}

// ---- state setters & text / drawing ----

// ---- per-DC text state (real GDI semantics) ----
// The mode/colour/alignment values live in the user32 DC record, so they
// survive across GetDC/ReleaseDC reuse correctly (a fresh DC is OPAQUE,
// black-on-white, TA_LEFT|TA_TOP) and SelectObject(hdc, font) is what the
// metrics below read. Before this the setters logged and returned constants:
// apps that read back a mode got a lie, and every font measured 8x16.
bool dc_text_state(void* hdc, trt_dc_text_state* st) {
  return trt_user32_dc_get_text_state(hdc, st) != 0;
}

// GDI cell metrics model. No font file is decoded offline, so the only font
// data is the selected LOGFONTW record: lfHeight/lfWidth define the cell and
// everything else scales from it. This replaces the old fixed 8x16 answer.
struct gdi_font_metrics {
  i32 height;
  i32 ascent;
  i32 descent;
  i32 ave_w;
  i32 max_w;
  u32 charset;
  bool italic;
  bool underline;
  bool strikeout;
};

// The font a DC measures/draws with is the FONT selection, never whatever
// object was selected most recently (a brush selection must not change text).
void* dc_font(void* hdc) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  return s ? s->font : nullptr;
}

static void gdi_metrics(void* hdc, gdi_font_metrics* m) {
  i32 lf_height = 0, lf_width = 0;
  u32 charset = 0;
  bool italic = false, underline = false, strikeout = false;
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)dc_font(hdc));
  if (o && o->type == TRT_GDI_OBJ_FONT) {
    memcpy(&lf_height, o->data, 4);
    memcpy(&lf_width, o->data + 4, 4);
    charset = o->data[23];  // lfCharSet
    italic = o->data[20] != 0;
    underline = o->data[21] != 0;
    strikeout = o->data[22] != 0;
  }
  i32 h = lf_height < 0 ? -lf_height : lf_height;
  if (h <= 0) h = 16;         // stock SYSTEM_FONT cell
  if (h > 4096) h = 4096;     // reject absurd requests rather than overflow
  m->height = h;
  m->ascent = (h * 13) / 16;
  m->descent = h - m->ascent;
  m->ave_w = lf_width > 0 ? lf_width : (h + 1) / 2;
  m->max_w = m->ave_w * 2;
  m->charset = charset;
  m->italic = italic;
  m->underline = underline;
  m->strikeout = strikeout;
}

// Fill the raster's text attributes from the DC: colour/bk/alignment come from
// the DC record, the cell from the selected LOGFONTW.
static void fill_text_style(void* hdc, trt_raster::text_style* st) {
  trt_dc_text_state ts;
  if (!dc_text_state(hdc, &ts)) {
    ts.bk_mode = 2;
    ts.text_color = 0;
    ts.bk_color = 0x00FFFFFFu;
    ts.text_align = 0;
  }
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  st->fg = ts.text_color;
  st->bg = ts.bk_color;
  st->bk_mode = ts.bk_mode;
  st->align = ts.text_align;
  st->height = m.height;
  st->advance = m.ave_w;
  st->ascent = m.ascent;
  st->italic = m.italic;
  st->underline = m.underline;
  st->strikeout = m.strikeout;
  st->dx = nullptr;
}

int trt_gdi_draw_text(void* hdc, const u16* text, int count, void* rect,
                      u32 format) {
  constexpr u32 DT_CENTER_ = 0x0001u, DT_RIGHT_ = 0x0002u;
  constexpr u32 DT_VCENTER_ = 0x0004u, DT_BOTTOM_ = 0x0008u;
  constexpr u32 DT_WORDBREAK_ = 0x0010u, DT_SINGLELINE_ = 0x0020u;
  constexpr u32 DT_EXPANDTABS_ = 0x0040u, DT_CALCRECT_ = 0x0400u;
  constexpr u32 DT_NOPREFIX_ = 0x0800u;
  if (!trt_user32_dc_is_valid(hdc) || !text || count < 0 || !rect) {
    log_line("user32: DrawText -> 0 (bad DC, text or rect)");
    return 0;
  }
  i32* rc = (i32*)rect;
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  trt_raster::text_style st;
  fill_text_style(hdc, &st);

  // Build the text the caller asked for: drop '&' accelerators unless
  // DT_NOPREFIX, expand tabs, and keep explicit newlines as breaks.
  std::vector<u16> cleaned;
  cleaned.reserve((size_t)count);
  for (int i = 0; i < count; ++i) {
    const u16 ch = text[i];
    if (ch == '&' && (format & DT_NOPREFIX_) == 0) continue;
    if (ch == '\t' && (format & DT_EXPANDTABS_)) {
      for (int k = 0; k < 4; ++k) cleaned.push_back(' ');
      continue;
    }
    cleaned.push_back(ch);
  }

  const i32 width = rc[2] - rc[0];
  const bool wrap = (format & DT_SINGLELINE_) == 0 &&
                    (format & DT_WORDBREAK_) != 0 && width > 0;
  const i32 max_chars = m.ave_w > 0 ? width / m.ave_w : 0;

  std::vector<std::pair<int, int>> lines;  // (start, length)
  int start = 0;
  for (int i = 0; i <= (int)cleaned.size(); ++i) {
    const bool eol = i == (int)cleaned.size() || cleaned[(size_t)i] == '\n';
    if (!eol) {
      if (wrap && max_chars > 0 && i - start >= max_chars) {
        // break at the last space if there is one, else at the limit
        int brk = i;
        for (int k = i; k > start; --k)
          if (cleaned[(size_t)k - 1] == ' ') { brk = k - 1; break; }
        lines.push_back({start, brk - start});
        start = brk;
        while (start < i && cleaned[(size_t)start] == ' ') ++start;
      }
      continue;
    }
    lines.push_back({start, i - start});
    start = i + 1;
  }
  if (lines.empty()) lines.push_back({0, 0});

  i32 widest = 0;
  for (const auto& l : lines) {
    const i32 w = l.second * m.ave_w;
    if (w > widest) widest = w;
  }
  const i32 total_h = (i32)lines.size() * m.height;

  if (format & DT_CALCRECT_) {
    rc[2] = rc[0] + widest;
    rc[3] = rc[1] + total_h;
    char line[128];
    snprintf(line, sizeof(line),
             "user32: DrawText CALCRECT -> %dx%d (%zu line(s))", widest,
             total_h, lines.size());
    log_line(line);
    return total_h;
  }

  i32 y = rc[1];
  if ((format & DT_BOTTOM_) && (format & DT_SINGLELINE_))
    y = rc[3] - m.height;
  else if ((format & DT_VCENTER_) && (format & DT_SINGLELINE_))
    y = rc[1] + (rc[3] - rc[1] - m.height) / 2;

  int drawn = 0;
  for (const auto& l : lines) {
    i32 x = rc[0];
    const i32 w = l.second * m.ave_w;
    if (format & DT_CENTER_) x = rc[0] + (width - w) / 2;
    else if (format & DT_RIGHT_) x = rc[2] - w;
    if (l.second > 0)
      trt_raster::text(hdc, x, y, cleaned.data() + l.first, l.second, st);
    y += m.height;
    drawn += l.second;
  }
  char line[144];
  snprintf(line, sizeof(line),
           "user32: DrawText fmt=0x%x -> %d char(s), %zu line(s), %d px tall",
           format, drawn, lines.size(), total_h);
  log_line(line);
  return total_h;
}


int TRT_GUEST_ABI trt_SetBkMode(void* hdc, i32 mode) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return 0;
  if (mode != 1 && mode != 2) {
    log_line("gdi32: SetBkMode -> 0 (mode must be TRANSPARENT(1) or OPAQUE(2))");
    return 0;
  }
  const i32 prev = st.bk_mode;
  st.bk_mode = mode;
  trt_user32_dc_set_text_state(hdc, &st);
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkMode mode=%d (prev=%d)", mode, prev);
  log_line(line);
  return prev;
}

int TRT_GUEST_ABI trt_GetBkMode(void* hdc) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return 0;
  return st.bk_mode;
}

u32 TRT_GUEST_ABI trt_SetTextColor(void* hdc, u32 color) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return (u32)-1;  // CLR_INVALID
  const u32 prev = st.text_color;
  st.text_color = color & 0x00FFFFFFu;
  trt_user32_dc_set_text_state(hdc, &st);
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetTextColor color=0x%06x (prev=0x%06x)",
           color & 0xFFFFFFu, prev);
  log_line(line);
  return prev;
}

u32 TRT_GUEST_ABI trt_SetBkColor(void* hdc, u32 color) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return (u32)-1;  // CLR_INVALID
  const u32 prev = st.bk_color;
  st.bk_color = color & 0x00FFFFFFu;
  trt_user32_dc_set_text_state(hdc, &st);
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkColor color=0x%06x (prev=0x%06x)",
           color & 0xFFFFFFu, prev);
  log_line(line);
  return prev;
}

// TA_* flags a DC can hold. Anything outside this set is GDI_ERROR in real
// GDI, and a caller that trusted a stored bad value draws in the wrong place.
constexpr u32 GDI_TA_VALID = 0x000001FFu;

u32 TRT_GUEST_ABI trt_SetTextAlign(void* hdc, u32 flags) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return (u32)-1;  // GDI_ERROR
  if (flags & ~GDI_TA_VALID) {
    char line[112];
    snprintf(line, sizeof(line),
             "gdi32: SetTextAlign flags=0x%x -> GDI_ERROR (unknown TA_ bit)",
             flags);
    log_line(line);
    return (u32)-1;
  }
  const u32 prev = st.text_align;
  st.text_align = flags;
  trt_user32_dc_set_text_state(hdc, &st);
  return prev;
}

u32 TRT_GUEST_ABI trt_GetTextAlign(void* hdc) {
  trt_dc_text_state st;
  if (!dc_text_state(hdc, &st)) return (u32)-1;  // GDI_ERROR
  return st.text_align;
}

// OBJ_* selectors GetCurrentObject accepts, answered from the real per-type
// selection (OBJ_PEN=1, OBJ_BRUSH=2, OBJ_PAL=5, OBJ_FONT=6, OBJ_BITMAP=7).
void* TRT_GUEST_ABI trt_GetCurrentObject(void* hdc, u32 type) {
  if (!trt_user32_dc_is_valid(hdc)) return nullptr;
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return nullptr;
  switch (type) {
    case 1: return s->pen;
    case 2: return s->brush;
    case 5: return s->palette;
    case 6: return s->font;
    case 7: return s->bitmap;
    default:
      log_line("gdi32: GetCurrentObject -> NULL (type outside OBJ_PEN..OBJ_PAL)");
      return nullptr;
  }
}

int TRT_GUEST_ABI trt_GetObjectW(void* handle, int cap, void* out) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)handle);
  if (!o) return 0;
  if (o->type == TRT_GDI_OBJ_BITMAP) {
    // Win64 BITMAP struct is 32 bytes: bmType(4), bmWidth(4), bmHeight(4),
    // bmWidthBytes(4), bmPlanes(2), bmBitsPixel(2), _pad(4), bmBits(8)
    struct win64_bitmap {
      i32 bmType; i32 bmWidth; i32 bmHeight; i32 bmWidthBytes;
      u16 bmPlanes; u16 bmBitsPixel; u32 _pad; void* bmBits;
    };
    static_assert(sizeof(win64_bitmap) == 32, "Win64 BITMAP size");
    if (!out) return 32;
    if (cap < 32) return 0;
    win64_bitmap* b = (win64_bitmap*)out;
    b->bmType = 0;
    b->bmWidth = o->bm_w;
    b->bmHeight = o->bm_h;
    b->bmWidthBytes = ((o->bm_w * o->bm_bpp + 31) / 32) * 4;
    b->bmPlanes = 1;
    b->bmBitsPixel = o->bm_bpp;
    b->_pad = 0;
    b->bmBits = o->bm_bits;
    return 32;
  }
  if (o->type == TRT_GDI_OBJ_FONT) {
    if (!out) return (int)LF_BYTES;
    if (cap < (int)LF_BYTES) return 0;
    memcpy(out, o->data, LF_BYTES);
    return (int)LF_BYTES;
  }
  return 0;
}

// GetObjectA: ANSI callers ask for LOGFONTA (60 bytes: the same 28-byte fixed
// part, lfFaceName[32] as bytes). Putty queries its font this way and a
// fail-visible 0 makes it fall back to bogus metrics.
int TRT_GUEST_ABI trt_GetObjectA(void* handle, int cap, void* out) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)handle);
  if (o && o->type == TRT_GDI_OBJ_BITMAP) return trt_GetObjectW(handle, cap, out);
  if (o && o->type == TRT_GDI_OBJ_FONT) {
    constexpr int k_logfonta_bytes = 60;
    if (!out) return k_logfonta_bytes;
    if (cap < k_logfonta_bytes) return 0;
    u8* d = (u8*)out;
    memset(d, 0, k_logfonta_bytes);
    memcpy(d, o->data, 28);
    const u16* face = (const u16*)(o->data + 28);
    for (int i = 0; i < 31 && face[i]; ++i) d[28 + i] = (u8)(face[i] & 0xFF);
    return k_logfonta_bytes;
  }
  log_line("gdi32: GetObjectA -> 0 (object query not implemented: fail-visible)");
  return 0;
}

// Measured from the selected font (see gdi_metrics): a 16 px cell is 8 px per
// character, a 32 px cell is 16, and a font created with lfWidth uses it.
static void gdi_size_text(void* hdc, int c, void* psizl) {
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  i32* sz = (i32*)psizl;
  sz[0] = c * m.ave_w;
  sz[1] = m.height;
}

int TRT_GUEST_ABI trt_GetTextExtentPoint32W(void* hdc, const u16* lpString,
                                            int c, void* psizl) {
  (void)lpString;
  if (!psizl) return 0;
  gdi_size_text(hdc, c, psizl);
  return 1;
}
int TRT_GUEST_ABI trt_GetTextExtentPoint32A(void* hdc, const char* lpString,
                                            int c, void* psizl) {
  (void)lpString;
  if (!psizl) return 0;
  gdi_size_text(hdc, c, psizl);
  return 1;
}
int TRT_GUEST_ABI trt_GetTextExtentPointW(void* hdc, const u16* lpString, int c,
                                          void* psizl) {
  (void)lpString;
  if (!psizl) return 0;
  gdi_size_text(hdc, c, psizl);
  return 1;
}
int TRT_GUEST_ABI trt_GetTextExtentPointA(void* hdc, const char* lpString, int c,
                                          void* psizl) {
  (void)lpString;
  if (!psizl) return 0;
  gdi_size_text(hdc, c, psizl);
  return 1;
}

// GetTextExtentExPoint*: fills the per-character extent array, the count that
// fits inside maxExtent, and the FULL string extent in psizl (real GDI leaves
// psizl unclipped, which is what apps use to size the scroll area).
static int gdi_extent_ex(void* hdc, int c, i32 max_extent, i32* fit,
                         i32* dx, void* psizl) {
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  int n_fit = 0;
  for (int i = 0; i < c; ++i) {
    const i32 upto = (i + 1) * m.ave_w;
    if (dx) dx[i] = upto;
    if (upto <= max_extent) n_fit = i + 1;
  }
  if (fit) *fit = n_fit;
  if (psizl) {
    i32* sz = (i32*)psizl;
    sz[0] = c * m.ave_w;
    sz[1] = m.height;
  }
  return 1;
}

int TRT_GUEST_ABI trt_GetTextExtentExPointW(void* hdc, const u16* lpString, int c,
                                            i32 max_extent, i32* lpnFit,
                                            i32* lpnDx, void* psizl) {
  (void)lpString;
  return gdi_extent_ex(hdc, c, max_extent, lpnFit, lpnDx, psizl);
}
int TRT_GUEST_ABI trt_GetTextExtentExPointA(void* hdc, const char* lpString, int c,
                                            i32 max_extent, i32* lpnFit,
                                            i32* lpnDx, void* psizl) {
  (void)lpString;
  return gdi_extent_ex(hdc, c, max_extent, lpnFit, lpnDx, psizl);
}

// GetCharWidth*: one advance per character in [first, last].
static int gdi_char_widths(void* hdc, u32 first, u32 last, i32* buf) {
  if (!buf) return 0;
  if (last < first) return 0;
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  for (u32 ch = first; ch <= last; ++ch) buf[ch - first] = m.ave_w;
  return 1;
}
int TRT_GUEST_ABI trt_GetCharWidth32W(void* hdc, u32 first, u32 last,
                                      i32* buf) {
  return gdi_char_widths(hdc, first, last, buf);
}
int TRT_GUEST_ABI trt_GetCharWidth32A(void* hdc, u32 first, u32 last,
                                      i32* buf) {
  return gdi_char_widths(hdc, first, last, buf);
}
int TRT_GUEST_ABI trt_GetCharWidthW(void* hdc, u32 first, u32 last, i32* buf) {
  return gdi_char_widths(hdc, first, last, buf);
}
int TRT_GUEST_ABI trt_GetCharWidthA(void* hdc, u32 first, u32 last, i32* buf) {
  return gdi_char_widths(hdc, first, last, buf);
}

// GetCharABCWidthsFloat*: ABCFLOAT is three floats (a, b, c) per character.
// The model font has no side bearings, so a and c are zero and b is the advance.
static int gdi_abc_float(void* hdc, u32 first, u32 last, float* out) {
  if (!out) return 0;
  if (last < first) return 0;
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  for (u32 ch = first; ch <= last; ++ch) {
    out[(ch - first) * 3 + 0] = 0.0f;
    out[(ch - first) * 3 + 1] = (float)m.ave_w;
    out[(ch - first) * 3 + 2] = 0.0f;
  }
  return 1;
}
int TRT_GUEST_ABI trt_GetCharABCWidthsFloatW(void* hdc, u32 first, u32 last,
                                             void* out) {
  return gdi_abc_float(hdc, first, last, (float*)out);
}
int TRT_GUEST_ABI trt_GetCharABCWidthsFloatA(void* hdc, u32 first, u32 last,
                                             void* out) {
  return gdi_abc_float(hdc, first, last, (float*)out);
}

// GetTextMetrics*: TEXTMETRICW is 60 bytes, TEXTMETRICA 56. The leading
// i32 fields are identical; tmCharSet is the last byte.
static void gdi_fill_text_metrics(void* hdc, void* lptm, size_t bytes,
                                  size_t charset_off) {
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  memset(lptm, 0, bytes);
  i32* tm = (i32*)lptm;
  tm[0] = m.height;       // tmHeight
  tm[1] = m.ascent;       // tmAscent
  tm[2] = m.descent;      // tmDescent
  tm[3] = 0;              // tmInternalLeading
  tm[4] = 0;              // tmExternalLeading
  tm[5] = m.ave_w;        // tmAveCharWidth
  tm[6] = m.max_w;        // tmMaxCharWidth
  ((u8*)lptm)[charset_off] = (u8)m.charset;  // tmCharSet
}

int TRT_GUEST_ABI trt_GetTextMetricsW(void* hdc, void* lptm) {
  if (!lptm) return 0;
  gdi_fill_text_metrics(hdc, lptm, 60, 59);
  return 1;
}
int TRT_GUEST_ABI trt_GetTextMetricsA(void* hdc, void* lptm) {
  if (!lptm) return 0;
  gdi_fill_text_metrics(hdc, lptm, 56, 55);
  return 1;
}

// GetCharacterPlacementW: fills GCP_RESULTSW fields the caller provided.
// Layout (x64, 64 bytes): lStructSize(4+4 pad), lpOutString, lpOrder, lpDx,
// lpCaretPos, lpClass, lpGlyphs, nGlyphs(4), nMaxFit(4).
int TRT_GUEST_ABI trt_GetCharacterPlacementW(void* hdc, const u16* lpString,
                                             int nCount, int nMaxExtent,
                                             void* lpResults, u32 flags) {
  (void)lpString; (void)flags;
  if (!lpResults) return 0;
  gdi_font_metrics m;
  gdi_metrics(hdc, &m);
  u8* r = (u8*)lpResults;
  void* lp_dx = *(void**)(r + 24);
  void* lp_caret = *(void**)(r + 32);
  if (lp_dx) {
    for (int i = 0; i < nCount; ++i) ((i32*)lp_dx)[i] = (i + 1) * m.ave_w;
  }
  if (lp_caret) {
    for (int i = 0; i < nCount; ++i) ((i32*)lp_caret)[i] = i * m.ave_w;
  }
  int fit = 0;
  while (fit < nCount && (fit + 1) * m.ave_w <= nMaxExtent) ++fit;
  *(u32*)(r + 56) = (u32)nCount;  // nGlyphs
  *(i32*)(r + 60) = fit;          // nMaxFit
  return nCount * m.ave_w;
}

// TranslateCharsetInfo: real charset <-> codepage table. Unknown input is a
// named FALSE, never a guessed codepage.
struct charset_cp {
  u32 charset;
  u32 codepage;
};
static const charset_cp k_charset_cp[] = {
    {0, 1252},    // ANSI_CHARSET
    {2, 1252},    // SYMBOL_CHARSET (no distinct ACP; Windows maps to 1252)
    {128, 932},   // SHIFTJIS_CHARSET
    {129, 949},   // HANGUL_CHARSET
    {134, 936},   // GB2312_CHARSET
    {136, 950},   // CHINESEBIG5_CHARSET
    {161, 1253},  // GREEK_CHARSET
    {162, 1254},  // TURKISH_CHARSET
    {163, 1258},  // VIETNAMESE_CHARSET
    {177, 1255},  // HEBREW_CHARSET
    {178, 1256},  // ARABIC_CHARSET
    {186, 1257},  // BALTIC_CHARSET
    {204, 1251},  // RUSSIAN_CHARSET
    {222, 874},   // THAI_CHARSET
    {238, 1250},  // EASTEUROPE_CHARSET
    {255, 437},   // OEM_CHARSET
};

int TRT_GUEST_ABI trt_TranslateCharsetInfo(void* lpSrc, void* lpCs,
                                           u32 dwFlags) {
  if (!lpCs) return 0;
  const u32 src = (u32)(uintptr_t)lpSrc;  // value, not a pointer, for TCI_SRC*
  u32 charset = 0, codepage = 0;
  bool found = false;
  if (dwFlags == 1) {  // TCI_SRCCHARSET
    charset = src & 0xFFu;
    for (const charset_cp& e : k_charset_cp)
      if (e.charset == charset) { codepage = e.codepage; found = true; break; }
  } else if (dwFlags == 2) {  // TCI_SRCCODEPAGE
    for (const charset_cp& e : k_charset_cp)
      if (e.codepage == (src & 0xFFFFu)) {
        charset = e.charset;
        codepage = e.codepage;
        found = true;
        break;
      }
  } else {
    log_line("gdi32: TranslateCharsetInfo -> FALSE (only TCI_SRCCHARSET/"
             "TCI_SRCCODEPAGE are implemented)");
    return 0;
  }
  if (!found) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: TranslateCharsetInfo -> FALSE (no codepage for %s 0x%x)",
             dwFlags == 1 ? "charset" : "codepage", src);
    log_line(line);
    return 0;
  }
  // CHARSETINFO (32 bytes on x64): ciCharset, ciACP, FONTSIGNATURE fs.
  memset(lpCs, 0, 32);
  u32* ci = (u32*)lpCs;
  ci[0] = charset;
  ci[1] = codepage;
  return 1;
}

// Clip rectangles: these now govern the DC's raster surface, so drawing
// outside the clip really is discarded.
int TRT_GUEST_ABI trt_IntersectClipRect(void* hdc, i32 l, i32 t, i32 r,
                                        i32 b) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;  // ERROR
  bool empty = (r <= l || b <= t);
  if (!empty) trt_raster::intersect_clip(hdc, l, t, r, b, &empty);
  char line[128];
  snprintf(line, sizeof(line),
           "gdi32: IntersectClipRect (%d,%d)-(%d,%d) -> %s", l, t, r, b,
           empty ? "NULLREGION" : "SIMPLEREGION");
  log_line(line);
  return empty ? 1 : 2;  // NULLREGION / SIMPLEREGION
}

int TRT_GUEST_ABI trt_ExcludeClipRect(void* hdc, i32 l, i32 t, i32 r, i32 b) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;  // ERROR
  i32 cl = 0, ct = 0, cr = 0, cb = 0;
  bool empty = false;
  if (trt_raster::clip_box(hdc, &cl, &ct, &cr, &cb)) {
    empty = trt_raster::exclude_clip(hdc, l, t, r, b);
  }
  char line[160];
  snprintf(line, sizeof(line),
           "gdi32: ExcludeClipRect (%d,%d)-(%d,%d) -> %s", l, t, r, b,
           empty ? "NULLREGION"
                 : "SIMPLEREGION (clip kept: single-rect model)");
  log_line(line);
  return empty ? 1 : 2;
}

int TRT_GUEST_ABI trt_UnrealizeObject(void* hobj) {
  const u32 h = (u32)(uintptr_t)hobj;
  if ((h & 0xFFFF0000u) == 0x80000000u) return 1;  // stock objects: TRUE
  log_line("gdi32: UnrealizeObject(non-stock) -> FALSE (no realized palettes)");
  return 0;
}

int TRT_GUEST_ABI trt_UpdateColors(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  return 1;  // palette entries already resolved at blend time; nothing pending
}
// ---- text: rasterized into the DC surface -------------------------------
// ANSI strings widen 1:1 to UTF-16 code units (CP1252 range) and share the W
// path, so both surfaces paint identical pixels.
static int textout_w(void* hdc, int x, int y, const u16* lpString, int c) {
  if (!trt_user32_dc_is_valid(hdc) || !lpString) {
    log_line("gdi32: TextOutW -> FALSE (not a user32 DC, or NULL string)");
    return 0;
  }
  if (c < 0) {  // -1 means NUL-terminated, per the GDI contract
    c = 0;
    while (lpString[c]) ++c;
  }
  if (c == 0) return 1;  // empty string: TRUE, nothing to draw
  trt_raster::text_style st;
  fill_text_style(hdc, &st);
  const int n = trt_raster::text(hdc, x, y, lpString, c, st);
  if (n < 0) {
    log_line("gdi32: TextOutW -> FALSE (no raster surface for this DC)");
    return 0;
  }
  char line[176];
  snprintf(line, sizeof(line),
           "gdi32: TextOutW c=%d cell=%dx%d bg=%s at (%d,%d) -> "
           "%d px drawn",
           c, st.advance, st.height, st.bk_mode == 2 ? "opaque" : "transparent",
           x, y, n * st.advance * st.height);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI trt_TextOutW(void* hdc, int x, int y, const u16* lpString, int c) {
  return textout_w(hdc, x, y, lpString, c);
}

int TRT_GUEST_ABI trt_TextOutA(void* hdc, int x, int y, const char* lpString, int c) {
  if (!lpString) {
    log_line("gdi32: TextOutA -> FALSE (NULL string)");
    return 0;
  }
  if (c < 0) {
    c = 0;
    while (lpString[c]) ++c;
  }
  if (c > 4096) c = 4096;
  u16 stack_buf[256];
  u16* wide = c <= 256 ? stack_buf : (u16*)malloc(sizeof(u16) * ((size_t)c + 1));
  if (!wide) return 0;
  for (int i = 0; i < c; ++i) wide[i] = (u16)(u8)lpString[i];
  const int rc = textout_w(hdc, x, y, wide, c);
  if (wide != stack_buf) free(wide);
  return rc;
}

int TRT_GUEST_ABI trt_ExtTextOutW(void* hdc, int x, int y, u32 options,
                                  const void* lprect, const u16* lpString,
                                  u32 c, const int* lpDx) {
  if (!trt_user32_dc_is_valid(hdc) || !lpString) {
    log_line("gdi32: ExtTextOutW -> FALSE (not a user32 DC, or NULL string)");
    return 0;
  }
  if (c == (u32)-1) {
    c = 0;
    while (lpString[c]) ++c;
  }
  if (c > 4096) c = 4096;
  constexpr u32 k_ETO_OPAQUE = 0x0002u;
  constexpr u32 k_ETO_CLIPPED = 0x0004u;
  trt_raster::text_style st;
  fill_text_style(hdc, &st);

  // k_ETO_OPAQUE fills the lprect with the background colour before the text,
  // which is how controls repaint a label cell.
  if ((options & k_ETO_OPAQUE) && lprect) {
    const i32* r = (const i32*)lprect;
    trt_raster::fill(hdc, r[0], r[1], r[2], r[3], st.bg);
  }

  // lpDx carries per-character advances; the raster takes cumulative pen
  // positions, so accumulate a bounded copy.
  std::vector<i32> cum;
  if (lpDx && c) {
    cum.resize(c);
    i32 acc = 0;
    for (u32 i = 0; i < c; ++i) {
      acc += lpDx[i];
      cum[i] = acc;
    }
    st.dx = cum.data();
  }

  // k_ETO_CLIPPED confines drawing to lprect for this call only.
  i32 saved[4] = {0, 0, 0, 0};
  const bool have_clip = trt_raster::clip_box(hdc, &saved[0], &saved[1],
                                             &saved[2], &saved[3]);
  if ((options & k_ETO_CLIPPED) && lprect) {
    const i32* r = (const i32*)lprect;
    bool empty = false;
    if (have_clip) trt_raster::intersect_clip(hdc, r[0], r[1], r[2], r[3], &empty);
  }

  const int n = trt_raster::text(hdc, x, y, lpString, (int)c, st);

  if ((options & k_ETO_CLIPPED) && lprect && have_clip)
    trt_raster::set_clip(hdc, saved[0], saved[1], saved[2], saved[3]);

  if (n < 0) {
    log_line("gdi32: ExtTextOutW -> FALSE (no raster surface for this DC)");
    return 0;
  }
  char line[176];
  snprintf(line, sizeof(line),
           "gdi32: ExtTextOutW c=%u opts=0x%x cell=%dx%d at (%d,%d) -> "
           "%d px drawn",
           c, options, st.advance, st.height, x, y,
           n * st.advance * st.height);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI trt_ExtTextOutA(void* hdc, int x, int y, u32 options,
                                  const void* lprect, const char* lpString,
                                  u32 c, const int* lpDx) {
  if (!lpString) return 0;
  if (c == (u32)-1) {
    c = 0;
    while (lpString[c]) ++c;
  }
  if (c > 4096) c = 4096;
  u16 stack_buf[256];
  u16* wide = c <= 256 ? stack_buf : (u16*)malloc(sizeof(u16) * ((size_t)c + 1));
  if (!wide) return 0;
  for (u32 i = 0; i < c; ++i) wide[i] = (u16)(u8)lpString[i];
  const int rc = trt_ExtTextOutW(hdc, x, y, options, lprect, wide, c, lpDx);
  if (wide != stack_buf) free(wide);
  return rc;
}

// GetOutlineTextMetricsA: OUTLINETEXTMETRICA is byte-compatible with the W
// struct for every field the model font fills, so this is the W path.
int TRT_GUEST_ABI trt_GetOutlineTextMetricsA(void* hdc, u32 cb_size,
                                             void* otm);

int TRT_GUEST_ABI trt_AddFontResourceExW(const u16* name, u32 fl, void* res) {
  (void)name; (void)fl; (void)res;
  return 1;
}
int TRT_GUEST_ABI trt_RemoveFontResourceExW(const u16* name, u32 fl, void* pdv) {
  (void)name; (void)fl; (void)pdv;
  return 1;
}
int TRT_GUEST_ABI trt_EnumFontFamiliesExW(void* hdc, void* lpLogfont,
                                          void* lpProc, i64 lParam, u32 dwFlags) {
  (void)hdc; (void)lpLogfont; (void)lpProc; (void)lParam; (void)dwFlags;
  return 1;
}

int TRT_GUEST_ABI trt_SelectClipRgn(void* hdc, void* hrgn) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;  // ERROR
  if (!hrgn) {  // NULL region = select the whole surface
    const i32 w = trt_raster::width(hdc), h = trt_raster::height(hdc);
    trt_raster::set_clip(hdc, 0, 0, w, h);
    trt_raster::clear_mask(hdc);  // ...and drop any SelectClipPath mask
    return 2;
  }
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hrgn);
  if (!o || o->type != TRT_GDI_OBJ_REGION) {
    log_line("gdi32: SelectClipRgn(bad region handle) -> ERROR");
    return 0;
  }
  trt_raster::set_clip(hdc, o->rg_l, o->rg_t, o->rg_r, o->rg_b);
  char line[128];
  trt_raster::clear_mask(hdc);  // a region replaces any SelectClipPath mask
  snprintf(line, sizeof(line), "gdi32: SelectClipRgn (%d,%d)-(%d,%d)", o->rg_l,
           o->rg_t, o->rg_r, o->rg_b);
  log_line(line);
  return 2;
}
int TRT_GUEST_ABI trt_ExtSelectClipRgn(void* hdc, void* hrgn, int mode) {
  constexpr int k_RGN_AND = 1, k_RGN_DIFF = 4;
  if (!trt_user32_dc_is_valid(hdc)) return 0;  // ERROR
  if (mode == k_RGN_AND) return trt_SelectClipRgn(hdc, hrgn);
  if (mode == k_RGN_DIFF) {
    log_line("gdi32: ExtSelectClipRgn(k_RGN_DIFF) -> clip unchanged "
             "(single-rect model: named)");
    return 2;
  }
  return trt_SelectClipRgn(hdc, hrgn);
}
int TRT_GUEST_ABI trt_GetClipBox(void* hdc, void* lprect) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;  // ERROR
  i32 l = 0, t = 0, r = 1920, b = 1080;
  if (!trt_raster::clip_box(hdc, &l, &t, &r, &b)) {
    const i32 w = trt_raster::width(hdc), h = trt_raster::height(hdc);
    (void)w;
    (void)h;
  }
  if (lprect) {
    i32* rc = (i32*)lprect;
    rc[0] = l; rc[1] = t; rc[2] = r; rc[3] = b;
  }
  if (r <= l || b <= t) return 1;  // NULLREGION
  return 2;
}
int TRT_GUEST_ABI trt_SetMapMode(void* hdc, int iMode) {
  (void)hdc;
  char line[96];
  snprintf(line, sizeof(line),
           "gdi32: SetMapMode mode=%d (mapping not applied: fail-visible)",
           iMode);
  log_line(line);
  return 1;  // MM_TEXT
}
int TRT_GUEST_ABI trt_GetMapMode(void* hdc) {
  (void)hdc;
  return 1;
}
// Viewport origin: real GDI adds it to every logical coordinate, which is how
// a scrolling window reuses one set of drawing coordinates.
static void dc_translate(void* hdc, int* x, int* y) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return;
  *x += s->vp_x;
  *y += s->vp_y;
}

int TRT_GUEST_ABI trt_SetViewportOrgEx(void* hdc, int x, int y, void* lppt) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: SetViewportOrgEx -> FALSE (not a user32 DC)");
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0;
  if (lppt) {  // previous origin, as a POINT
    i32* p = (i32*)lppt;
    p[0] = s->vp_x;
    p[1] = s->vp_y;
  }
  s->vp_x = x;
  s->vp_y = y;
  return 1;
}
int TRT_GUEST_ABI trt_SetWindowOrgEx(void* hdc, int x, int y, void* lppt) {
  (void)hdc; (void)x; (void)y;
  if (lppt) memset(lppt, 0, 8);
  return 1;
}
// Defined further down (Polyline) and just above it (dc_argb); the path
// replay needs both.
int TRT_GUEST_ABI trt_Polyline(void* hdc, const void* apt, int cpt);
static inline u32 dc_argb(u32 colorref);

// Flatten a recorded path into (x,y) pairs plus per-subpath vertex counts, in
// device units (the viewport origin is applied here, once).
static void path_collect(path_rec* p, void* hdc, std::vector<i32>& xy,
                         std::vector<int>& counts) {
  xy.clear();
  counts.clear();
  if (!p) return;
  int cur = -1;
  for (const path_vertex& v : p->v) {
    if (v.flags & PATH_START) {
      counts.push_back(0);
      cur = (int)counts.size() - 1;
    }
    if (cur < 0) continue;
    i32 x = v.x, y = v.y;
    dc_translate(hdc, &x, &y);
    xy.push_back(x);
    xy.push_back(y);
    counts[(size_t)cur]++;
  }
}

static bool path_has_close(const path_rec* p) {
  for (const path_vertex& v : p->v)
    if (v.flags & PATH_CLOSE) return true;
  return false;
}

// ---- BeginPath: record geometry instead of painting it --------------------
int TRT_GUEST_ABI trt_BeginPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: BeginPath -> 0 (not a user32 DC)");
    return 0;
  }
  path_rec* p = path_find(hdc, true);
  if (!p) return 0;
  p->v.clear();
  p->open = true;
  p->closed = false;
  char line[128];
  snprintf(line, sizeof(line), "gdi32: BeginPath on DC %p (recording)", hdc);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI trt_EndPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->open) {
    log_line("gdi32: EndPath -> 0 (no BeginPath on this DC)");
    return 0;
  }
  p->open = false;
  p->closed = !p->v.empty();
  if (!p->closed) {
    log_line("gdi32: EndPath recorded an empty path -> 0");
    return 0;
  }
  size_t subs = 0;
  for (const path_vertex& v : p->v)
    if (v.flags & PATH_START) ++subs;
  char line[160];
  snprintf(line, sizeof(line),
           "gdi32: EndPath recorded %zu vertices in %zu subpath(s)", p->v.size(),
           subs);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI trt_AbortPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (p) {
    p->v.clear();
    p->open = false;
    p->closed = false;
  }
  log_line("gdi32: AbortPath discarded the recorded path");
  return 1;
}

int TRT_GUEST_ABI trt_CloseFigure(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->open || p->v.empty()) {
    log_line("gdi32: CloseFigure -> 0 (no open path)");
    return 0;
  }
  path_close(p);
  return 1;
}

// GetPath returns logical POINTs and the parallel PBYTE type array; the caller
// passes both buffers. GDI returns the vertex count, -1 when no path is
// selected into the DC.
int TRT_GUEST_ABI trt_GetPath(void* hdc, void* ppt, void* ptypes, int n) {
  if (!trt_user32_dc_is_valid(hdc)) return -1;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: GetPath -> -1 (no path selected into the DC)");
    return -1;
  }
  const int count = (int)p->v.size();
  if (!ppt || !ptypes || n < count) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: GetPath needs %d vertices, caller offered %d", count, n);
    log_line(line);
    return -1;
  }
  i32* pts = (i32*)ppt;
  u8* types = (u8*)ptypes;
  for (int i = 0; i < count; ++i) {
    pts[i * 2] = p->v[(size_t)i].x;
    pts[i * 2 + 1] = p->v[(size_t)i].y;
    u8 t = (p->v[(size_t)i].flags & PATH_START) ? 6 : 2;  // PT_MOVETO/PT_LINETO
    if (p->v[(size_t)i].flags & PATH_CLOSE) t |= 1;       // PT_CLOSEFIGURE
    types[i] = t;
  }
  return count;
}

int TRT_GUEST_ABI trt_FlattenPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: FlattenPath -> 0 (no path selected into the DC)");
    return 0;
  }
  // Curves were recorded as polylines already, so there is nothing to flatten.
  return 1;
}

int TRT_GUEST_ABI trt_WidenPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  log_line("gdi32: WidenPath -> 0 (pen-outline conversion not implemented: "
           "fail-visible)");
  return 0;
}

int TRT_GUEST_ABI trt_StrokePath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: StrokePath -> 0 (no path selected into the DC)");
    return 0;
  }
  if (dc_pen_is_null(hdc)) return 1;  // NULL_PEN: path is consumed, nothing paints
  std::vector<i32> xy;
  std::vector<int> counts;
  path_collect(p, hdc, xy, counts);
  if (counts.empty()) return 0;
  trt_raster::stroke_path(hdc, xy.data(), counts.data(), (int)counts.size(),
                          dc_argb(dc_pen_color(hdc)), 1, path_has_close(p));
  return 1;
}

int TRT_GUEST_ABI trt_FillPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: FillPath -> 0 (no path selected into the DC)");
    return 0;
  }
  if (dc_brush_is_null(hdc)) return 1;
  std::vector<i32> xy;
  std::vector<int> counts;
  path_collect(p, hdc, xy, counts);
  if (counts.empty()) return 0;
  trt_raster::fill_path(hdc, xy.data(), counts.data(), (int)counts.size(),
                        dc_argb(dc_brush_color(hdc)), p->fill_mode);
  return 1;
}

int TRT_GUEST_ABI trt_StrokeAndFillPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  // GDI strokes first, then fills inside the stroke so the fill never covers
  // the outline.
  if (!trt_StrokePath(hdc)) return 0;
  return trt_FillPath(hdc);
}

// SelectClipPath: RGN_AND (1) intersects, RGN_COPY (5) replaces. OR/XOR/DIFF
// would need region algebra on a per-pixel mask; they are named, not guessed.
int TRT_GUEST_ABI trt_SelectClipPath(void* hdc, int mode) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: SelectClipPath -> 0 (no path selected into the DC)");
    return 0;
  }
  const int kAnd = 1, kCopy = 5;
  if (mode != kAnd && mode != kCopy) {
    char line[144];
    snprintf(line, sizeof(line),
             "gdi32: SelectClipPath mode=%d -> 0 (only RGN_AND/RGN_COPY "
             "implemented on the raster clip)",
             mode);
    log_line(line);
    return 0;
  }
  std::vector<i32> xy;
  std::vector<int> counts;
  path_collect(p, hdc, xy, counts);
  if (counts.empty()) return 0;
  if (!trt_raster::clip_path(hdc, xy.data(), counts.data(), (int)counts.size(),
                             p->fill_mode, mode == kAnd)) {
    log_line("gdi32: SelectClipPath -> 0 (raster surface rejected the mask)");
    return 0;
  }
  return 1;
}

int TRT_GUEST_ABI trt_SetPolyFillMode(void* hdc, int mode) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, true);
  if (!p) return 0;
  const int prev = p->fill_mode;
  p->fill_mode = (mode == 2) ? 2 : 1;  // WINDING vs ALTERNATE
  return prev;
}

int TRT_GUEST_ABI trt_GetPolyFillMode(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  return p ? p->fill_mode : 1;
}

// PathToRegion answers with the path's bounding box: a real but coarse region,
// named here so a caller that relies on the exact outline is visible.
int TRT_GUEST_ABI trt_PathToRegion(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  path_rec* p = path_find(hdc, false);
  if (!p || !p->closed) {
    log_line("gdi32: PathToRegion -> NULL (no path selected into the DC)");
    return 0;
  }
  std::vector<i32> xy;
  std::vector<int> counts;
  path_collect(p, hdc, xy, counts);
  if (xy.empty()) return 0;
  i32 l = xy[0], t = xy[1], r = xy[0], b = xy[1];
  for (size_t i = 0; i + 1 < xy.size(); i += 2) {
    if (xy[i] < l) l = xy[i];
    if (xy[i] > r) r = xy[i];
    if (xy[i + 1] < t) t = xy[i + 1];
    if (xy[i + 1] > b) b = xy[i + 1];
  }
  char line[160];
  snprintf(line, sizeof(line),
           "gdi32: PathToRegion -> bounding box (%d,%d)-(%d,%d) (outline "
           "region not implemented)",
           l, t, r, b);
  log_line(line);
  return (int)(intptr_t)trt_CreateRectRgn(l, t, r + 1, b + 1);
}

// PolylineTo continues the current position inside an open path.
int TRT_GUEST_ABI trt_PolylineTo(void* hdc, const void* apt, int cpt) {
  if (!trt_user32_dc_is_valid(hdc) || !apt || cpt < 1) {
    log_line("gdi32: PolylineTo -> FALSE (bad DC, NULL points or <1 point)");
    return 0;
  }
  path_rec* p = path_find(hdc, false);
  if (!p || !p->open) return trt_Polyline(hdc, apt, cpt);
  const i32* pt = (const i32*)apt;
  for (int i = 0; i < cpt; ++i) path_to(p, pt[i * 2], pt[i * 2 + 1]);
  return 1;
}

// Cubic Beziers are recorded flattened (16 segments per curve).
static void path_add_bezier(path_rec* p, const i32* pt, int segs) {
  if (!p || !pt) return;
  float x0 = (float)p->cur_x, y0 = (float)p->cur_y;
  for (int s = 0; s < segs; ++s) {
    const float x1 = (float)pt[s * 6 + 0], y1 = (float)pt[s * 6 + 1];
    const float x2 = (float)pt[s * 6 + 2], y2 = (float)pt[s * 6 + 3];
    const float x3 = (float)pt[s * 6 + 4], y3 = (float)pt[s * 6 + 5];
    const int steps = 16;
    for (int i = 1; i <= steps; ++i) {
      const float t = (float)i / (float)steps, u = 1.0f - t;
      const float bx = u * u * u * x0 + 3.0f * u * u * t * x1 +
                     3.0f * u * t * t * x2 + t * t * t * x3;
      const float by = u * u * u * y0 + 3.0f * u * u * t * y1 +
                     3.0f * u * t * t * y2 + t * t * t * y3;
      path_to(p, (i32)(bx + 0.5f), (i32)(by + 0.5f));
    }
    x0 = x3;
    y0 = y3;
  }
}

int TRT_GUEST_ABI trt_PolyBezier(void* hdc, const void* apt, u32 cpt) {
  if (!trt_user32_dc_is_valid(hdc) || !apt || cpt < 4 || (cpt - 1) % 3 != 0) {
    log_line("gdi32: PolyBezier -> FALSE (needs 1 + 3n points)");
    return 0;
  }
  const i32* pt = (const i32*)apt;
  path_rec* p = path_find(hdc, true);
  const bool rec = p && p->open;
  const int segs = (int)((cpt - 1) / 3);
  if (rec) {
    path_start(p, pt[0], pt[1]);
    path_add_bezier(p, pt + 2, segs);
    return 1;
  }
  // Not recording: draw the flattened curve straight onto the surface.
  if (dc_pen_is_null(hdc)) return 1;
  std::vector<i32> flat;
  flat.push_back(pt[0]);
  flat.push_back(pt[1]);
  for (int s = 0; s < segs; ++s) {
    const float x0 = (float)flat[flat.size() - 2], y0 = (float)flat[flat.size() - 1];
    const float x1 = (float)pt[2 + s * 6 + 0], y1 = (float)pt[2 + s * 6 + 1];
    const float x2 = (float)pt[2 + s * 6 + 2], y2 = (float)pt[2 + s * 6 + 3];
    const float x3 = (float)pt[2 + s * 6 + 4], y3 = (float)pt[2 + s * 6 + 5];
    const int steps = 16;
    for (int i = 1; i <= steps; ++i) {
      const float t = (float)i / (float)steps, u = 1.0f - t;
      flat.push_back((i32)(u * u * u * x0 + 3.0f * u * u * t * x1 +
                           3.0f * u * t * t * x2 + t * t * t * x3));
      flat.push_back((i32)(u * u * u * y0 + 3.0f * u * u * t * y1 +
                           3.0f * u * t * t * y2 + t * t * t * y3));
    }
  }
  const u32 col = dc_pen_color(hdc);
  for (size_t i = 0; i + 3 < flat.size(); i += 2) {
    int ax = flat[i], ay = flat[i + 1], bx = flat[i + 2], by = flat[i + 3];
    dc_translate(hdc, &ax, &ay);
    dc_translate(hdc, &bx, &by);
    trt_raster::line(hdc, ax, ay, bx, by, col, 1);
  }
  return 1;
}

int TRT_GUEST_ABI trt_PolyBezierTo(void* hdc, const void* apt, u32 cpt) {
  if (!trt_user32_dc_is_valid(hdc) || !apt || cpt < 3 || cpt % 3 != 0) {
    log_line("gdi32: PolyBezierTo -> FALSE (needs 3n points)");
    return 0;
  }
  path_rec* p = path_find(hdc, false);
  if (!p || !p->open) {
    log_line("gdi32: PolyBezierTo -> FALSE (no open path)");
    return 0;
  }
  path_add_bezier(p, (const i32*)apt, (int)(cpt / 3));
  return 1;
}

int TRT_GUEST_ABI trt_MoveToEx(void* hdc, int x, int y, void* lppt) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: MoveToEx -> FALSE (not a user32 DC)");
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0;
  if (lppt) {
    i32* p = (i32*)lppt;
    p[0] = s->cur_x;
    p[1] = s->cur_y;
  }
  s->cur_x = x;
  s->cur_y = y;
  path_rec* pr = path_find(hdc, false);
  if (pr && pr->open) path_start(pr, x, y);  // BeginPath: subpath start
  return 1;
}
int TRT_GUEST_ABI trt_LineTo(void* hdc, int x, int y) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: LineTo -> FALSE (not a user32 DC)");
    return 0;
  }
  i32 x0 = 0, y0 = 0;
  {
    std::lock_guard<std::mutex> lk(g_sel_mu);
    dc_selection* s = dc_sel(hdc, true);
    if (!s) return 0;
    x0 = s->cur_x;
    y0 = s->cur_y;
    s->cur_x = x;
    s->cur_y = y;
  }
  if (path_recording(hdc)) {  // BeginPath: line goes into the path
    path_to(path_find(hdc, false), x, y);
    return 1;
  }
  if (dc_pen_is_null(hdc)) return 1;  // NULL_PEN: position moves, nothing paints
  int dx0 = x0, dy0 = y0, dx1 = x, dy1 = y;
  dc_translate(hdc, &dx0, &dy0);
  dc_translate(hdc, &dx1, &dy1);
  trt_raster::line(hdc, dx0, dy0, dx1, dy1, dc_pen_color(hdc), 1);
  return 1;
}
int TRT_GUEST_ABI trt_Rectangle(void* hdc, int l, int t, int r, int b) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: Rectangle -> FALSE (not a user32 DC)");
    return 0;
  }
  if (path_recording(hdc)) {  // BeginPath: the rect becomes a closed subpath
    path_record_rect(path_find(hdc, false), l, t, r, b);
    return 1;
  }
  dc_translate(hdc, &l, &t);
  dc_translate(hdc, &r, &b);
  if (!dc_brush_is_null(hdc)) trt_raster::fill(hdc, l, t, r, b, dc_brush_color(hdc));
  if (!dc_pen_is_null(hdc)) trt_raster::frame(hdc, l, t, r, b, dc_pen_color(hdc), 1);
  return 1;
}
int TRT_GUEST_ABI trt_Ellipse(void* hdc, int l, int t, int r, int b) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: Ellipse -> FALSE (not a user32 DC)");
    return 0;
  }
  if (path_recording(hdc)) {
    path_record_ellipse(path_find(hdc, false), l, t, r, b);
    return 1;
  }
  dc_translate(hdc, &l, &t);
  dc_translate(hdc, &r, &b);
  if (dc_brush_is_null(hdc) && dc_pen_is_null(hdc)) return 1;
  const bool filled = !dc_brush_is_null(hdc);
  trt_raster::ellipse(hdc, l, t, r, b,
                      filled ? dc_brush_color(hdc) : dc_pen_color(hdc),
                      filled && !dc_pen_is_null(hdc) ? 2 : 1, filled);
  if (filled && !dc_pen_is_null(hdc))
    trt_raster::ellipse(hdc, l, t, r, b, dc_pen_color(hdc), 1, false);
  return 1;
}
int TRT_GUEST_ABI trt_Polygon(void* hdc, const void* apt, int cpt) {
  if (!trt_user32_dc_is_valid(hdc) || !apt || cpt < 2) {
    log_line("gdi32: Polygon -> FALSE (bad DC, NULL points or <2 points)");
    return 0;
  }
  const i32* pt = (const i32*)apt;  // POINT { LONG x, y }
  if (path_recording(hdc)) {
    path_rec* pr = path_find(hdc, false);
    path_start(pr, pt[0], pt[1]);
    for (int i = 1; i < cpt; ++i) path_to(pr, pt[i * 2], pt[i * 2 + 1]);
    path_close(pr);
    return 1;
  }
  std::vector<i32> xy((size_t)cpt * 2);
  for (int i = 0; i < cpt; ++i) {
    xy[(size_t)i * 2] = pt[i * 2];
    xy[(size_t)i * 2 + 1] = pt[i * 2 + 1];
    dc_translate(hdc, &xy[(size_t)i * 2], &xy[(size_t)i * 2 + 1]);
  }
  if (!dc_brush_is_null(hdc))
    trt_raster::polygon(hdc, xy.data(), cpt, dc_brush_color(hdc), 1, true);
  if (!dc_pen_is_null(hdc))
    trt_raster::polygon(hdc, xy.data(), cpt, dc_pen_color(hdc), 1, false);
  return 1;
}
int TRT_GUEST_ABI trt_Polyline(void* hdc, const void* apt, int cpt) {
  if (!trt_user32_dc_is_valid(hdc) || !apt || cpt < 2) {
    log_line("gdi32: Polyline -> FALSE (bad DC, NULL points or <2 points)");
    return 0;
  }
  if (path_recording(hdc)) {
    path_rec* pr = path_find(hdc, false);
    const i32* rp = (const i32*)apt;
    for (int i = 0; i < cpt; ++i) path_to(pr, rp[i * 2], rp[i * 2 + 1]);
    return 1;
  }
  if (dc_pen_is_null(hdc)) return 1;
  const i32* pt = (const i32*)apt;
  const u32 col = dc_pen_color(hdc);
  for (int i = 0; i + 1 < cpt; ++i) {
    int x0 = pt[i * 2], y0 = pt[i * 2 + 1];
    int x1 = pt[(i + 1) * 2], y1 = pt[(i + 1) * 2 + 1];
    dc_translate(hdc, &x0, &y0);
    dc_translate(hdc, &x1, &y1);
    trt_raster::line(hdc, x0, y0, x1, y1, col, 1);
  }
  return 1;
}
// COLORREF 0x00BBGGRR -> 0xAARRGGBB for the raster's alpha-aware primitives.
static inline u32 dc_argb(u32 colorref) {
  return 0xFF000000u | ((colorref & 0xFFu) << 16) | (colorref & 0x00FF00u) |
         ((colorref >> 16) & 0xFFu);
}

int TRT_GUEST_ABI trt_RoundRect(void* hdc, int l, int t, int r, int b, int w,
                                int h) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: RoundRect -> FALSE (not a user32 DC)");
    return 0;
  }
  if (path_recording(hdc)) {
    path_record_round_rect(path_find(hdc, false), l, t, r, b, w, h);
    return 1;
  }
  dc_translate(hdc, &l, &t);
  dc_translate(hdc, &r, &b);
  if (dc_brush_is_null(hdc) && dc_pen_is_null(hdc)) return 1;
  // Real elliptical corners: the corner ellipse is w x h, centered on the
  // rectangle corner (GDI semantics).
  const i32 ew = w > 0 ? w : 1;
  const i32 eh = h > 0 ? h : 1;
  const bool filled = !dc_brush_is_null(hdc);
  if (filled)
    trt_raster::round_rect(hdc, l, t, r, b, ew, eh,
                           dc_argb(dc_brush_color(hdc)), 1, true);
  if (!dc_pen_is_null(hdc))
    trt_raster::round_rect(hdc, l, t, r, b, ew, eh, dc_argb(dc_pen_color(hdc)),
                           1, false);
  return 1;
}
// ---- arcs -----------------------------------------------------------------
// GDI Arc/Pie/Chord take two radial points on the bounding ellipse. mode:
// 0 = Arc (outline), 1 = Pie (filled wedge), 2 = Chord (filled segment).
static int arc_common(void* hdc, int l, int t, int r, int b, int sx, int sy,
                      int ex, int ey, int mode) {
  if (!trt_user32_dc_is_valid(hdc)) {
    log_line("gdi32: Arc/Pie/Chord -> FALSE (not a user32 DC)");
    return 0;
  }
  if (path_recording(hdc)) {
    path_record_arc(path_find(hdc, false), l, t, r, b, sx, sy, ex, ey, mode);
    return 1;
  }
  dc_translate(hdc, &l, &t);
  dc_translate(hdc, &r, &b);
  dc_translate(hdc, &sx, &sy);
  dc_translate(hdc, &ex, &ey);
  if (dc_brush_is_null(hdc) && dc_pen_is_null(hdc)) return 1;
  // Arc is an outline only; Pie/Chord fill with the brush and then outline the
  // curved edge with the pen.
  if (mode == 0 || dc_brush_is_null(hdc)) {
    const u32 pen = dc_pen_is_null(hdc) ? dc_brush_color(hdc) : dc_pen_color(hdc);
    trt_raster::arc(hdc, l, t, r, b, sx, sy, ex, ey, dc_argb(pen), 1,
                    mode == 0 ? 0 : mode);
  } else {
    trt_raster::arc(hdc, l, t, r, b, sx, sy, ex, ey,
                    dc_argb(dc_brush_color(hdc)), 1, mode);
  }
  if (mode != 0 && !dc_pen_is_null(hdc))
    trt_raster::arc(hdc, l, t, r, b, sx, sy, ex, ey, dc_argb(dc_pen_color(hdc)),
                    1, 0);
  return 1;
}
int TRT_GUEST_ABI trt_Arc(void* hdc, int l, int t, int r, int b, int sx, int sy,
                          int ex, int ey) {
  return arc_common(hdc, l, t, r, b, sx, sy, ex, ey, 0);
}
int TRT_GUEST_ABI trt_Pie(void* hdc, int l, int t, int r, int b, int sx, int sy,
                          int ex, int ey) {
  return arc_common(hdc, l, t, r, b, sx, sy, ex, ey, 1);
}
int TRT_GUEST_ABI trt_Chord(void* hdc, int l, int t, int r, int b, int sx,
                            int sy, int ex, int ey) {
  return arc_common(hdc, l, t, r, b, sx, sy, ex, ey, 2);
}
int TRT_GUEST_ABI trt_GdiFlush(void) { return 1; }

// ---- pixel-format & gamma surface ----
struct px_fmt {
  u16 size; u16 version; u32 flags;
  u8 pixel_type; u8 color_bits;
  u8 red_bits, red_shift, green_bits, green_shift, blue_bits, blue_shift;
  u8 alpha_bits, alpha_shift, accum_bits, accum_r, accum_g, accum_b, accum_a;
  u8 depth_bits, stencil_bits, aux_buffers, layer_type, reserved;
  u32 layer_mask, visible_mask, damage_mask;
};
int TRT_GUEST_ABI trt_ChoosePixelFormat(void* hdc, const void* want) {
  (void)hdc; (void)want;
  log_line("gdi32: ChoosePixelFormat -> 1 (fixed format)");
  return 1;
}
int TRT_GUEST_ABI trt_DescribePixelFormat(void* hdc, int iPixelFormat,
                                          u32 nBytes, void* ppfd) {
  (void)hdc; (void)iPixelFormat;
  if (ppfd && nBytes >= 40) {
    memset(ppfd, 0, 40);
    px_fmt* p = (px_fmt*)ppfd;
    p->size = 40;
    p->version = 1;
    p->flags = 0x00000025u;  // PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
    p->color_bits = 32;
    p->red_bits = 8; p->green_bits = 8; p->blue_bits = 8; p->alpha_bits = 8;
    p->depth_bits = 24;
    p->stencil_bits = 8;
  }
  return 1;
}
u32 TRT_GUEST_ABI trt_SetDCBrushColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return 0xFFFFFFFFu;  // CLR_INVALID
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0xFFFFFFFFu;
  const u32 prev = s->brush_color;
  s->brush_color = color & 0x00FFFFFFu;
  return prev;
}

u32 TRT_GUEST_ABI trt_SetDCPenColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return 0xFFFFFFFFu;  // CLR_INVALID
  std::lock_guard<std::mutex> lk(g_sel_mu);
  dc_selection* s = dc_sel(hdc, true);
  if (!s) return 0xFFFFFFFFu;
  const u32 prev = s->pen_color;
  s->pen_color = color & 0x00FFFFFFu;
  return prev;
}
int TRT_GUEST_ABI trt_GetPixelFormat(void* hdc) {
  (void)hdc;
  // The bridge models one fixed pixel format (index 1) per DC — the same
  // model ChoosePixelFormat/DescribePixelFormat/SetPixelFormat implement.
  // Returning it (not 0) is what keeps SDL3's GL-attribute path consistent.
  return 1;
}
int TRT_GUEST_ABI trt_SetPixelFormat(void* hdc, int fmt, const void* descr) {
  (void)hdc; (void)descr;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetPixelFormat fmt=%d -> 1", fmt);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_SwapBuffers(void* hdc) {
  return trt_wgl_swap_buffers(hdc);
}
int TRT_GUEST_ABI trt_SetDeviceGammaRamp(void* hdc, void* lpRamp) {
  (void)hdc; (void)lpRamp;
  return 1;
}
int TRT_GUEST_ABI trt_GetDeviceGammaRamp(void* hdc, void* lpRamp) {
  (void)hdc;
  if (!lpRamp) return 0;
  u16* r = (u16*)lpRamp;
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
      r[c * 256 + i] = (u16)(i * 257);
  return 1;
}
int TRT_GUEST_ABI trt_GetDeviceCaps(void* hdc, int index) {
  (void)hdc;
  int v = 0;
  switch (index) {
    case 8:   v = 1920; break;  // HORZRES
    case 10:  v = 1080; break;  // VERTRES
    case 12:  v = 32; break;    // BITSPIXEL
    case 14:  v = 1; break;     // PLANES
    case 88:  v = 96; break;    // LOGPIXELSX
    case 90:  v = 96; break;    // LOGPIXELSY
    case 104: v = 256; break;   // SIZEPALETTE
    case 116: v = 60; break;    // VREFRESH
    case 117: v = 1920; break;  // DESKTOPHORZRES
    case 118: v = 1080; break;  // DESKTOPVERTRES
    default:  v = 0; break;
  }
  char line[96];
  snprintf(line, sizeof(line), "gdi32: GetDeviceCaps(%d) -> %d (fixed)", index, v);
  log_line(line);
  return v;
}

// ---- shared with bridge_user32.cpp ----------------------------------------
// Games import GetDC/ReleaseDC from gdi32 as well as user32; the same
// user32 DC-record handlers serve both tables (extern decls, no duplication).
extern "C" void* TRT_STDCALL trt_GetDC(void* hwnd);
extern "C" int TRT_STDCALL trt_ReleaseDC(void* hwnd, void* hdc);

// ---- D3DKMT (engine adapter queries; gdi32 exports) ------------------------
// Honest stub: an adapter DOES exist on Xbox (the APU), so open/close/
// enumerate answer with a synthetic handle; per-type adapter info queries
// fail visibly with STATUS_NOT_IMPLEMENTED so engines take their fallback.
// Struct layouts are the documented WDK d3dkmthk.h ABI (x64).
namespace d3dkmt {
constexpr u32 k_status_not_impl = 0xC0000002u;  // STATUS_NOT_IMPLEMENTED
constexpr u32 k_adapter_handle = 0x44584B31u;   // 'DXK1' synthetic
}  // namespace d3dkmt

int TRT_GUEST_ABI gdi_D3DKMTOpenAdapterFromLuid(void* args) {
  // D3DKMT_OPENADAPTERFROMLUID { LUID AdapterLuid(8); ULONG hAdapter; }
  if (args) {
    u32* h = (u32*)((u8*)args + 8);
    *h = d3dkmt::k_adapter_handle;
    u32 lo, hi;
    __builtin_memcpy(&lo, args, 4);
    __builtin_memcpy(&hi, (u8*)args + 4, 4);
    char b[96];
    snprintf(b, sizeof(b),
             "gdi: D3DKMTOpenAdapterFromLuid(%08x-%08x) -> hAdapter=0x%x "
             "(synthetic Xbox APU adapter)",
             lo, hi, *h);
    log_line(b);
  }
  return 0;  // STATUS_SUCCESS
}
int TRT_GUEST_ABI gdi_D3DKMTCloseAdapter(void* args) {
  // D3DKMT_CLOSEADAPTER { ULONG hAdapter; }
  if (args) {
    char b[64];
    snprintf(b, sizeof(b), "gdi: D3DKMTCloseAdapter(hAdapter=0x%x)", *(u32*)args);
    log_line(b);
  }
  return 0;  // STATUS_SUCCESS
}
int TRT_GUEST_ABI gdi_D3DKMTQueryAdapterInfo(void* args) {
  // D3DKMT_QUERYADAPTERINFO { ULONG hAdapter(0); ULONG Type(4);
  //                           VOID* PrivateDriverData(8); UINT Size(16); }
  u32 type = args ? *(u32*)((u8*)args + 4) : 0;
  char b[96];
  snprintf(b, sizeof(b),
           "gdi: D3DKMTQueryAdapterInfo(type=%u) -> STATUS_NOT_IMPLEMENTED",
           type);
  log_line(b);
  return (int)d3dkmt::k_status_not_impl;
}
int TRT_GUEST_ABI gdi_D3DKMTEnumAdapters(void* args) {
  // D3DKMT_ENUMADAPTERS { UINT NumAdapters(0); D3DKMT_ADAPTERINFO Adapters[16](8); }
  // D3DKMT_ADAPTERINFO { ULONG hAdapter(0); LUID AdapterLuid(8); ULONG Flags(16); }
  if (args) {
    *(u32*)args = 1;
    u8* a = (u8*)args + 8;
    *(u32*)(a + 0) = d3dkmt::k_adapter_handle;
    *(u32*)(a + 8) = 0x1337u;  // LUID low (matches the D3D12 bridge LUID)
    *(i32*)(a + 12) = 0;       // LUID high
    *(u32*)(a + 16) = 0;       // Flags
  }
  log_line("gdi: D3DKMTEnumAdapters -> 1 adapter (Xbox APU, synthetic)");
  return 0;  // STATUS_SUCCESS
}

// GetICMProfileW: the console has no ICM color-management profiles. FALSE
// means "no profile associated with this DC" (real Win32 semantics); SDL3
// then skips color-space setup and uses the default framebuffer path.
int TRT_GUEST_ABI gdi_GetICMProfileW(void* hdc, u32* name_len, u16* name) {
  (void)hdc;
  (void)name_len;
  (void)name;
  log_line("gdi32: GetICMProfileW -> FALSE (no ICM profile on console)");
  return 0;
}

// ExtEscape: printer/driver escapes are not available on the console.
// Sekiro's amd_ags path probes it; return 0 (escape not supported).
int TRT_GUEST_ABI trt_ExtEscape(void* hdc, int nEscape, int cbInput,
                                const void* lpInData, int cbOutput, void* lpOutData) {
  (void)hdc; (void)nEscape; (void)cbInput; (void)lpInData;
  (void)cbOutput; (void)lpOutData;
  return 0;
}

// GetGlyphOutlineW: font outline extraction not supported on console.
u32 TRT_GUEST_ABI trt_GetGlyphOutlineW(void* hdc, u32 uChar, u32 fuFormat,
                                       void* lpgm, u32 cbBuffer, void* lpvBuffer,
                                       const void* lpmat2) {
  (void)hdc; (void)uChar; (void)fuFormat; (void)lpgm;
  (void)cbBuffer; (void)lpvBuffer; (void)lpmat2;
  return (u32)-1;  // GDI_ERROR
}

// GetKerningPairsW: kerning pairs not available.
u32 TRT_GUEST_ABI trt_GetKerningPairsW(void* hdc, u32 nPairs, void* lpkrnpair) {
  (void)hdc; (void)nPairs; (void)lpkrnpair;
  return 0;
}

// ---- real logical palettes & DIB color tables ----------------------------
struct dc_pal_rec {
  void* hdc;
  void* hpal;
};
constexpr int GDI_MAX_DC_PALS = 64;
dc_pal_rec g_dc_pals[GDI_MAX_DC_PALS];
std::mutex g_pal_mu;

void* TRT_GUEST_ABI trt_CreatePalette(const void* logpal) {
  if (!logpal) return nullptr;
  const u8* p = (const u8*)logpal;
  u16 num_entries = 0;
  memcpy(&num_entries, p + 2, 2);
  if (num_entries > 256) num_entries = 256;
  const int slot = obj_alloc(TRT_GDI_OBJ_PALETTE);
  if (slot < 0) return nullptr;
  g_objs[slot].pal_count = num_entries;
  if (num_entries > 0) {
    memcpy(g_objs[slot].pal_entries, p + 4, (size_t)num_entries * 4);
  }
  return make_handle(slot);
}

void* TRT_GUEST_ABI trt_SelectPalette(void* hdc, void* hpal, int force_bkgnd) {
  (void)force_bkgnd;
  if (!trt_user32_dc_is_valid(hdc)) return nullptr;
  const u32 h = (u32)(uintptr_t)hpal;
  const bool is_default = (h == 0x8000000Fu);
  if (!is_default) {
    gdi_obj* o = obj_by_handle(h);
    if (!o || o->type != TRT_GDI_OBJ_PALETTE) return nullptr;
  }
  std::lock_guard<std::mutex> lk(g_pal_mu);
  void* prev = (void*)(uintptr_t)0x8000000Fu;  // DEFAULT_PALETTE
  for (int i = 0; i < GDI_MAX_DC_PALS; ++i) {
    if (g_dc_pals[i].hdc == hdc) {
      prev = g_dc_pals[i].hpal ? g_dc_pals[i].hpal : prev;
      g_dc_pals[i].hpal = hpal;
      return prev;
    }
  }
  for (int i = 0; i < GDI_MAX_DC_PALS; ++i) {
    if (!g_dc_pals[i].hdc) {
      g_dc_pals[i].hdc = hdc;
      g_dc_pals[i].hpal = hpal;
      return prev;
    }
  }
  return prev;
}

u32 TRT_GUEST_ABI trt_RealizePalette(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0xFFFFFFFFu;  // GDI_ERROR
  void* cur_pal = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_pal_mu);
    for (int i = 0; i < GDI_MAX_DC_PALS; ++i) {
      if (g_dc_pals[i].hdc == hdc) {
        cur_pal = g_dc_pals[i].hpal;
        break;
      }
    }
  }
  if (!cur_pal || (uintptr_t)cur_pal == 0x8000000Full) return 0;
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)cur_pal);
  return (o && o->type == TRT_GDI_OBJ_PALETTE) ? (u32)o->pal_count : 0;
}

u32 TRT_GUEST_ABI trt_GetPaletteEntries(void* hpal, u32 start, u32 count,
                                        void* entries) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hpal);
  if (!o || o->type != TRT_GDI_OBJ_PALETTE) return 0;
  if (!entries) return o->pal_count;
  if (start >= o->pal_count || count == 0) return 0;
  u32 n = o->pal_count - start;
  if (count < n) n = count;
  memcpy(entries, o->pal_entries + start, (size_t)n * 4);
  return n;
}

u32 TRT_GUEST_ABI trt_SetPaletteEntries(void* hpal, u32 start, u32 count,
                                        const void* entries) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)hpal);
  if (!o || o->type != TRT_GDI_OBJ_PALETTE || !entries) return 0;
  if (start >= o->pal_count || count == 0) return 0;
  u32 n = o->pal_count - start;
  if (count < n) n = count;
  memcpy(o->pal_entries + start, entries, (size_t)n * 4);
  return n;
}

u32 TRT_GUEST_ABI trt_SetDIBColorTable(void* hdc, u32 start, u32 count,
                                       const void* colors) {
  if (!trt_user32_dc_is_valid(hdc) || !colors || count == 0 || start >= 256)
    return 0;
  void* sel = trt_user32_dc_selected(hdc);
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)sel);
  if (!o || o->type != TRT_GDI_OBJ_BITMAP) return 0;
  u32 avail = 256u - start;
  u32 n = count < avail ? count : avail;
  memcpy(o->dib_colors + start, colors, (size_t)n * 4);
  if (start + n > o->dib_color_count)
    o->dib_color_count = (u16)(start + n);
  return n;
}

u32 TRT_GUEST_ABI trt_GetDIBColorTable(void* hdc, u32 start, u32 count,
                                       void* colors) {
  if (!trt_user32_dc_is_valid(hdc) || !colors || count == 0 || start >= 256)
    return 0;
  void* sel = trt_user32_dc_selected(hdc);
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)sel);
  if (!o || o->type != TRT_GDI_OBJ_BITMAP || start >= o->dib_color_count)
    return 0;
  u32 avail = (u32)o->dib_color_count - start;
  u32 n = count < avail ? count : avail;
  memcpy(colors, o->dib_colors + start, (size_t)n * 4);
  return n;
}

// Glyph indices: the host renders text through its own stack, which indexes by
// UTF-16 code unit. Returning the input characters is a usable documented
// identity mapping instead of an invented glyph table.
u32 TRT_GUEST_ABI trt_GetGlyphIndicesW(void* hdc, const u16* text, int count,
                                       u16* indices, u32 flags) {
  (void)hdc; (void)flags;
  if (!text || !indices || count < 0) return 0xFFFFFFFFu;  // GDI_ERROR
  for (int i = 0; i < count; ++i) indices[i] = text[i];
  return (u32)count;
}

// OUTLINETEXTMETRICW: there is no outline font engine behind the DC, so the
// structure is zeroed and otmSize is filled as the API requires. A caller that
// only probes the size (otm == nullptr or cb_size == 0) gets the required size
// (216 bytes for OUTLINETEXTMETRICW on Win64); one that provides a buffer gets
// otmSize filled.
u32 TRT_GUEST_ABI trt_GetOutlineTextMetricsW(void* hdc, u32 cb_size, void* otm) {
  (void)hdc;
  constexpr u32 k_otm_size = 216;  // sizeof(OUTLINETEXTMETRICW) on x64
  if (!otm || cb_size == 0) return k_otm_size;
  if (cb_size < 4) return 0;
  const u32 to_write = (cb_size < k_otm_size) ? cb_size : k_otm_size;
  memset(otm, 0, to_write);
  *(u32*)otm = to_write;  // otmSize
  return to_write;
}

int TRT_GUEST_ABI trt_GetOutlineTextMetricsA(void* hdc, u32 cb_size,
                                             void* otm) {
  return trt_GetOutlineTextMetricsW(hdc, cb_size, otm);
}

int trt_bridge_register_gdi(void) {
  static const trt_named_fn k_gdi32_fns[] = {
      {"GetICMProfileW", (void*)&gdi_GetICMProfileW},
      {"D3DKMTOpenAdapterFromLuid", (void*)&gdi_D3DKMTOpenAdapterFromLuid},
      {"D3DKMTCloseAdapter", (void*)&gdi_D3DKMTCloseAdapter},
      {"D3DKMTQueryAdapterInfo", (void*)&gdi_D3DKMTQueryAdapterInfo},
      {"D3DKMTEnumAdapters", (void*)&gdi_D3DKMTEnumAdapters},
      {"CreateFontIndirectW", (void*)&trt_CreateFontIndirectW},
      {"CreateFontIndirectA", (void*)&trt_CreateFontIndirectA},
      {"CreateFontW", (void*)&trt_CreateFontW},
      {"CreateFontA", (void*)&trt_CreateFontA},
      {"CreateSolidBrush", (void*)&trt_CreateSolidBrush},
      {"CreateBrushIndirect", (void*)&trt_CreateBrushIndirect},
      {"CreatePatternBrush", (void*)&trt_CreatePatternBrush},
      {"CreateDIBPatternBrushPt", (void*)&trt_CreateDIBPatternBrushPt},
      {"CreatePen", (void*)&trt_CreatePen},
      {"CreatePenIndirect", (void*)&trt_CreatePenIndirect},
      {"ExtCreatePen", (void*)&trt_ExtCreatePen},
      {"CreateRectRgn", (void*)&trt_CreateRectRgn},
      {"CreateRectRgnIndirect", (void*)&trt_CreateRectRgnIndirect},
      {"CreatePolygonRgn", (void*)&trt_CreatePolygonRgn},
      {"CreateEllipticRgn", (void*)&trt_CreateEllipticRgn},
      {"CombineRgn", (void*)&trt_CombineRgn},
      {"OffsetRgn", (void*)&trt_OffsetRgn},
      {"PtInRegion", (void*)&trt_PtInRegion},
      {"RectInRegion", (void*)&trt_RectInRegion},
      {"GetRgnBox", (void*)&trt_GetRgnBox},
      {"GetStockObject", (void*)&trt_GetStockObject},
      {"CreateCompatibleDC", (void*)&trt_CreateCompatibleDC},
      {"CreateDCW", (void*)&trt_CreateDCW},
      {"CreateDCA", (void*)&trt_CreateDCA},
      {"DeleteDC", (void*)&trt_DeleteDC},
      {"GetDC", (void*)&trt_GetDC},
      {"ReleaseDC", (void*)&trt_ReleaseDC},
      {"SaveDC", (void*)&trt_SaveDC},
      {"RestoreDC", (void*)&trt_RestoreDC},
      {"CreateBitmap", (void*)&trt_CreateBitmap},
      {"CreateCompatibleBitmap", (void*)&trt_CreateCompatibleBitmap},
      {"CreateDIBSection", (void*)&trt_CreateDIBSection},
      {"GetDIBits", (void*)&trt_GetDIBits},
      {"SetDIBits", (void*)&trt_SetDIBits},
      {"SetDIBitsToDevice", (void*)&trt_SetDIBitsToDevice},
      {"StretchDIBits", (void*)&trt_StretchDIBits},
      {"BitBlt", (void*)&trt_BitBlt},
      {"StretchBlt", (void*)&trt_StretchBlt},
      {"SetStretchBltMode", (void*)&trt_SetStretchBltMode},
      {"GetStretchBltMode", (void*)&trt_GetStretchBltMode},
      {"PatBlt", (void*)&trt_PatBlt},
      {"GetPixel", (void*)&trt_GetPixel},
      {"SetPixel", (void*)&trt_SetPixel},
      {"SelectObject", (void*)&trt_SelectObject},
      {"DeleteObject", (void*)&trt_DeleteObject},
      {"SetBkMode", (void*)&trt_SetBkMode},
      {"GetBkMode", (void*)&trt_GetBkMode},
      {"SetTextColor", (void*)&trt_SetTextColor},
      {"SetBkColor", (void*)&trt_SetBkColor},
      {"SetTextAlign", (void*)&trt_SetTextAlign},
      {"GetTextAlign", (void*)&trt_GetTextAlign},
      {"GetCurrentObject", (void*)&trt_GetCurrentObject},
      {"GetTextExtentPointA", (void*)&trt_GetTextExtentPointA},
      {"GetTextExtentPointW", (void*)&trt_GetTextExtentPointW},
      {"GetTextExtentExPointA", (void*)&trt_GetTextExtentExPointA},
      {"GetTextExtentExPointW", (void*)&trt_GetTextExtentExPointW},
      {"GetCharWidth32A", (void*)&trt_GetCharWidth32A},
      {"GetCharWidth32W", (void*)&trt_GetCharWidth32W},
      {"GetCharWidthA", (void*)&trt_GetCharWidthA},
      {"GetCharWidthW", (void*)&trt_GetCharWidthW},
      {"GetCharABCWidthsFloatA", (void*)&trt_GetCharABCWidthsFloatA},
      {"GetCharABCWidthsFloatW", (void*)&trt_GetCharABCWidthsFloatW},
      {"GetCharacterPlacementW", (void*)&trt_GetCharacterPlacementW},
      {"TranslateCharsetInfo", (void*)&trt_TranslateCharsetInfo},
      {"IntersectClipRect", (void*)&trt_IntersectClipRect},
      {"ExcludeClipRect", (void*)&trt_ExcludeClipRect},
      {"UnrealizeObject", (void*)&trt_UnrealizeObject},
      {"UpdateColors", (void*)&trt_UpdateColors},
      {"GetOutlineTextMetricsA", (void*)&trt_GetOutlineTextMetricsA},
      {"BeginPath", (void*)&trt_BeginPath},
      {"EndPath", (void*)&trt_EndPath},
      {"AbortPath", (void*)&trt_AbortPath},
      {"CloseFigure", (void*)&trt_CloseFigure},
      {"GetPath", (void*)&trt_GetPath},
      {"FlattenPath", (void*)&trt_FlattenPath},
      {"WidenPath", (void*)&trt_WidenPath},
      {"StrokePath", (void*)&trt_StrokePath},
      {"FillPath", (void*)&trt_FillPath},
      {"StrokeAndFillPath", (void*)&trt_StrokeAndFillPath},
      {"SelectClipPath", (void*)&trt_SelectClipPath},
      {"SetPolyFillMode", (void*)&trt_SetPolyFillMode},
      {"GetPolyFillMode", (void*)&trt_GetPolyFillMode},
      {"PathToRegion", (void*)&trt_PathToRegion},
      {"PolylineTo", (void*)&trt_PolylineTo},
      {"PolyBezier", (void*)&trt_PolyBezier},
      {"PolyBezierTo", (void*)&trt_PolyBezierTo},
      {"GetObjectW", (void*)&trt_GetObjectW},
      {"GetObjectA", (void*)&trt_GetObjectA},
      {"GetTextExtentPoint32W", (void*)&trt_GetTextExtentPoint32W},
      {"GetTextExtentPoint32A", (void*)&trt_GetTextExtentPoint32A},
      {"GetTextMetricsW", (void*)&trt_GetTextMetricsW},
      {"GetTextMetricsA", (void*)&trt_GetTextMetricsA},
      {"TextOutW", (void*)&trt_TextOutW},
      {"TextOutA", (void*)&trt_TextOutA},
      {"ExtTextOutW", (void*)&trt_ExtTextOutW},
      {"ExtTextOutA", (void*)&trt_ExtTextOutA},
      {"ExtEscape", (void*)&trt_ExtEscape},
      {"GetGlyphOutlineW", (void*)&trt_GetGlyphOutlineW},
      {"GetKerningPairsW", (void*)&trt_GetKerningPairsW},
      // Palette and font-metric surface: real palette objects & DIB color table storage.
      {"CreatePalette", (void*)&trt_CreatePalette},
      {"SelectPalette", (void*)&trt_SelectPalette},
      {"RealizePalette", (void*)&trt_RealizePalette},
      {"GetPaletteEntries", (void*)&trt_GetPaletteEntries},
      {"SetPaletteEntries", (void*)&trt_SetPaletteEntries},
      {"SetDIBColorTable", (void*)&trt_SetDIBColorTable},
      {"GetDIBColorTable", (void*)&trt_GetDIBColorTable},
      {"GetGlyphIndicesW", (void*)&trt_GetGlyphIndicesW},
      {"GetOutlineTextMetricsW", (void*)&trt_GetOutlineTextMetricsW},
      {"AddFontResourceExW", (void*)&trt_AddFontResourceExW},
      {"RemoveFontResourceExW", (void*)&trt_RemoveFontResourceExW},
      {"EnumFontFamiliesExW", (void*)&trt_EnumFontFamiliesExW},
      {"SelectClipRgn", (void*)&trt_SelectClipRgn},
      {"ExtSelectClipRgn", (void*)&trt_ExtSelectClipRgn},
      {"GetClipBox", (void*)&trt_GetClipBox},
      {"SetMapMode", (void*)&trt_SetMapMode},
      {"GetMapMode", (void*)&trt_GetMapMode},
      {"SetViewportOrgEx", (void*)&trt_SetViewportOrgEx},
      {"SetWindowOrgEx", (void*)&trt_SetWindowOrgEx},
      {"MoveToEx", (void*)&trt_MoveToEx},
      {"LineTo", (void*)&trt_LineTo},
      {"Rectangle", (void*)&trt_Rectangle},
      {"Ellipse", (void*)&trt_Ellipse},
      {"Polygon", (void*)&trt_Polygon},
      {"Polyline", (void*)&trt_Polyline},
      {"RoundRect", (void*)&trt_RoundRect},
      {"Arc", (void*)&trt_Arc},
      {"Pie", (void*)&trt_Pie},
      {"Chord", (void*)&trt_Chord},
      {"GdiFlush", (void*)&trt_GdiFlush},
      {"ChoosePixelFormat", (void*)&trt_ChoosePixelFormat},
      {"DescribePixelFormat", (void*)&trt_DescribePixelFormat},
      {"SetPixelFormat", (void*)&trt_SetPixelFormat},
      {"GetPixelFormat", (void*)&trt_GetPixelFormat},
      {"SetDCBrushColor", (void*)&trt_SetDCBrushColor},
      {"SetDCPenColor", (void*)&trt_SetDCPenColor},
      {"SwapBuffers", (void*)&trt_SwapBuffers},
      {"SetDeviceGammaRamp", (void*)&trt_SetDeviceGammaRamp},
      {"GetDeviceGammaRamp", (void*)&trt_GetDeviceGammaRamp},
      {"GetDeviceCaps", (void*)&trt_GetDeviceCaps},
  };
  trt_module_register("gdi32.dll", TRT_ROUTE_BUILTIN, k_gdi32_fns,
                      (int)(sizeof(k_gdi32_fns) / sizeof(k_gdi32_fns[0])));
  return 0;
}

} // extern "C"
