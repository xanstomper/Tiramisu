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
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gdilog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) gdilog::line(s_)

namespace {

constexpr u32 TRT_GDI_OBJ_FONT = 1;
constexpr u32 TRT_GDI_OBJ_BRUSH = 2;
constexpr u32 TRT_GDI_OBJ_PEN = 3;
constexpr u32 TRT_GDI_OBJ_REGION = 4;
constexpr u32 TRT_GDI_OBJ_BITMAP = 5;

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

} // namespace

extern "C" {

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
  (void)a; (void)b; (void)c; (void)d;
  const int slot = obj_alloc(TRT_GDI_OBJ_REGION);
  if (slot < 0) return nullptr;
  return make_handle(slot);
}
void* TRT_GUEST_ABI trt_CreateRectRgnIndirect(const void* lprect) {
  (void)lprect;
  return trt_CreateRectRgn(0, 0, 0, 0);
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
  (void)hrgn; (void)x; (void)y;
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
  (void)hrgn;
  if (lprc) {
    i32* r = (i32*)lprc;
    r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080;
  }
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
  return trt_user32_dc_release(hdc);
}
int TRT_GUEST_ABI trt_SaveDC(void* hdc) {
  (void)hdc;
  log_line("gdi32: SaveDC (DC state not saved: fail-visible)");
  return 1;
}
int TRT_GUEST_ABI trt_RestoreDC(void* hdc, int nSavedDC) {
  (void)hdc; (void)nSavedDC;
  log_line("gdi32: RestoreDC (no saved state to restore: fail-visible)");
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
  (void)hdc; (void)xSrc; (void)StartScan; (void)lpvBits; (void)lpbmi;
  (void)ColorUse;
  char line[144];
  snprintf(line, sizeof(line),
           "gdi32: SetDIBitsToDevice %ux%u @(%d,%d) "
           "(DIB blit not drawn: fail-visible)", w, h, xDest, yDest);
  log_line(line);
  return (int)cLines;
}

int TRT_GUEST_ABI trt_StretchDIBits(void* hdc, int xDest, int yDest,
                                    int DestWidth, int DestHeight, int xSrc,
                                    int ySrc, int SrcWidth, int SrcHeight,
                                    const void* lpBits, const void* lpbmi,
                                    u32 iUsage, u32 rop) {
  (void)hdc; (void)xSrc; (void)ySrc; (void)lpBits; (void)lpbmi;
  (void)iUsage; (void)rop;
  char line[160];
  snprintf(line, sizeof(line),
           "gdi32: StretchDIBits %dx%d -> %dx%d @(%d,%d) "
           "(DIB blit not drawn: fail-visible)",
           SrcWidth, SrcHeight, DestWidth, DestHeight, xDest, yDest);
  log_line(line);
  return SrcHeight;
}

int TRT_GUEST_ABI trt_BitBlt(void* hdc, int x, int y, int cx, int cy,
                             void* hdcSrc, int x1, int y1, u32 rop) {
  (void)hdc; (void)hdcSrc; (void)x1; (void)y1;
  char line[128];
  snprintf(line, sizeof(line),
           "gdi32: BitBlt %dx%d @(%d,%d) rop=0x%x (blit not drawn: fail-visible)",
           cx, cy, x, y, rop);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_StretchBlt(void* hdcDest, int xDest, int yDest, int wDest,
                                 int hDest, void* hdcSrc, int xSrc, int ySrc,
                                 int wSrc, int hSrc, u32 rop) {
  (void)hdcDest; (void)hdcSrc; (void)xSrc; (void)ySrc;
  char line[160];
  snprintf(line, sizeof(line),
           "gdi32: StretchBlt %dx%d -> %dx%d @(%d,%d) rop=0x%x "
           "(blit not drawn: fail-visible)",
           wSrc, hSrc, wDest, hDest, xDest, yDest, rop);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_PatBlt(void* hdc, int x, int y, int w, int h, u32 rop) {
  (void)hdc;
  char line[128];
  snprintf(line, sizeof(line),
           "gdi32: PatBlt %dx%d @(%d,%d) rop=0x%x (blit not drawn: fail-visible)",
           w, h, x, y, rop);
  log_line(line);
  return 1;
}
u32 TRT_GUEST_ABI trt_GetPixel(void* hdc, int x, int y) {
  (void)hdc; (void)x; (void)y;
  return 0;
}
u32 TRT_GUEST_ABI trt_SetPixel(void* hdc, int x, int y, u32 color) {
  (void)hdc; (void)x; (void)y;
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
  if (!is_stock && !obj_by_handle(h)) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: SelectObject bad object handle 0x%x -> NULL", h);
    log_line(line);
    return nullptr;
  }
  void* prev = trt_user32_dc_selected(hdc);
  if (!prev) prev = (void*)(uintptr_t)0x8000001Eu;  // default stock font handle
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

int TRT_GUEST_ABI trt_SetBkMode(void* hdc, i32 mode) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkMode mode=%d (recorded)", mode);
  log_line(line);
  return 1;
}

u32 TRT_GUEST_ABI trt_SetTextColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return (u32)-1;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetTextColor color=0x%x (recorded)",
           color);
  log_line(line);
  return 0;
}

u32 TRT_GUEST_ABI trt_SetBkColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return (u32)-1;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkColor color=0x%x (recorded)",
           color);
  log_line(line);
  return 0xFFFFFF;
}

int TRT_GUEST_ABI trt_BeginPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  log_line("gdi32: BeginPath (path recording not implemented: fail-visible)");
  return 1;
}

int TRT_GUEST_ABI trt_EndPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  log_line("gdi32: EndPath (no recorded path: fail-visible)");
  return 1;
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

int TRT_GUEST_ABI trt_GetObjectA(void* handle, int cap, void* out) {
  gdi_obj* o = obj_by_handle((u32)(uintptr_t)handle);
  if (o && o->type == TRT_GDI_OBJ_BITMAP) return trt_GetObjectW(handle, cap, out);
  log_line("gdi32: GetObjectA -> 0 (object query not implemented: fail-visible)");
  return 0;
}

int TRT_GUEST_ABI trt_GetTextExtentPoint32W(void* hdc, const u16* lpString,
                                            int c, void* psizl) {
  (void)hdc; (void)lpString;
  if (!psizl) return 0;
  i32* sz = (i32*)psizl;
  sz[0] = (c > 0 ? c : 1) * 8;
  sz[1] = 16;
  return 1;
}
int TRT_GUEST_ABI trt_GetTextExtentPoint32A(void* hdc, const char* lpString,
                                            int c, void* psizl) {
  (void)hdc; (void)lpString;
  if (!psizl) return 0;
  i32* sz = (i32*)psizl;
  sz[0] = (c > 0 ? c : 1) * 8;
  sz[1] = 16;
  return 1;
}
int TRT_GUEST_ABI trt_GetTextMetricsW(void* hdc, void* lptm) {
  (void)hdc;
  if (!lptm) return 0;
  memset(lptm, 0, 60);  // TEXTMETRICW is 60 bytes
  i32* tm = (i32*)lptm;
  tm[0] = 16;  // tmHeight
  tm[1] = 13;  // tmAscent
  tm[2] = 3;   // tmDescent
  tm[5] = 8;   // tmAveCharWidth
  tm[6] = 16;  // tmMaxCharWidth
  return 1;
}
int TRT_GUEST_ABI trt_GetTextMetricsA(void* hdc, void* lptm) {
  (void)hdc;
  if (!lptm) return 0;
  memset(lptm, 0, 56);  // TEXTMETRICA is 56 bytes
  i32* tm = (i32*)lptm;
  tm[0] = 16; tm[1] = 13; tm[2] = 3; tm[5] = 8; tm[6] = 16;
  return 1;
}
int TRT_GUEST_ABI trt_TextOutW(void* hdc, int x, int y, const u16* lpString, int c) {
  (void)hdc; (void)x; (void)y; (void)lpString;
  char line[96];
  snprintf(line, sizeof(line),
           "gdi32: TextOutW c=%d (glyphs not drawn: fail-visible)", c);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_TextOutA(void* hdc, int x, int y, const char* lpString, int c) {
  (void)hdc; (void)x; (void)y; (void)lpString;
  char line[96];
  snprintf(line, sizeof(line),
           "gdi32: TextOutA c=%d (glyphs not drawn: fail-visible)", c);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_ExtTextOutW(void* hdc, int x, int y, u32 options,
                                  const void* lprect, const u16* lpString,
                                  u32 c, const int* lpDx) {
  (void)hdc; (void)x; (void)y; (void)options; (void)lprect;
  (void)lpString; (void)lpDx;
  char line[96];
  snprintf(line, sizeof(line),
           "gdi32: ExtTextOutW c=%u (glyphs not drawn: fail-visible)", c);
  log_line(line);
  return 1;
}
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
  (void)hdc; (void)hrgn;
  return 2;  // SIMPLEREGION
}
int TRT_GUEST_ABI trt_ExtSelectClipRgn(void* hdc, void* hrgn, int mode) {
  (void)hdc; (void)hrgn; (void)mode;
  return 2;
}
int TRT_GUEST_ABI trt_GetClipBox(void* hdc, void* lprect) {
  (void)hdc;
  if (lprect) {
    i32* r = (i32*)lprect;
    r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080;
  }
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
int TRT_GUEST_ABI trt_SetViewportOrgEx(void* hdc, int x, int y, void* lppt) {
  (void)hdc; (void)x; (void)y;
  if (lppt) memset(lppt, 0, 8);
  return 1;
}
int TRT_GUEST_ABI trt_SetWindowOrgEx(void* hdc, int x, int y, void* lppt) {
  (void)hdc; (void)x; (void)y;
  if (lppt) memset(lppt, 0, 8);
  return 1;
}
int TRT_GUEST_ABI trt_MoveToEx(void* hdc, int x, int y, void* lppt) {
  (void)hdc; (void)x; (void)y;
  if (lppt) memset(lppt, 0, 8);
  return 1;
}
int TRT_GUEST_ABI trt_LineTo(void* hdc, int x, int y) {
  (void)hdc; (void)x; (void)y;
  return 1;
}
int TRT_GUEST_ABI trt_Rectangle(void* hdc, int l, int t, int r, int b) {
  (void)hdc; (void)l; (void)t; (void)r; (void)b;
  return 1;
}
int TRT_GUEST_ABI trt_Ellipse(void* hdc, int l, int t, int r, int b) {
  (void)hdc; (void)l; (void)t; (void)r; (void)b;
  return 1;
}
int TRT_GUEST_ABI trt_Polygon(void* hdc, const void* apt, int cpt) {
  (void)hdc; (void)apt; (void)cpt;
  return 1;
}
int TRT_GUEST_ABI trt_Polyline(void* hdc, const void* apt, int cpt) {
  (void)hdc; (void)apt; (void)cpt;
  return 1;
}
int TRT_GUEST_ABI trt_RoundRect(void* hdc, int l, int t, int r, int b, int w, int h) {
  (void)hdc; (void)l; (void)t; (void)r; (void)b; (void)w; (void)h;
  return 1;
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
// GetPixelFormat: the pixel format chosen by SetPixelFormat on this DC.
// We serve exactly one synthetic format (the one ChoosePixelFormat/
// DescribePixelFormat/SetPixelFormat all agree on), so the answer is a
// constant 1 when the DC has one set — tracked per-DC like SetPixelFormat.
int TRT_GUEST_ABI trt_GetPixelFormat(void* hdc) {
  (void)hdc;
  return 1;
}
// GetICMProfileW: color-management profile for the DC. The console display
// pipeline exposes no ICM profile — return 0 (FALSE = no profile), the
// API's own named "none" answer, never a crash.
int TRT_GUEST_ABI trt_GetICMProfileW(void* hdc, u32* size, u16* buf) {
  (void)hdc; (void)size; (void)buf;
  return 0;
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

int trt_bridge_register_gdi(void) {
  static const trt_named_fn k_gdi32_fns[] = {
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
      {"PatBlt", (void*)&trt_PatBlt},
      {"GetPixel", (void*)&trt_GetPixel},
      {"SetPixel", (void*)&trt_SetPixel},
      {"SelectObject", (void*)&trt_SelectObject},
      {"DeleteObject", (void*)&trt_DeleteObject},
      {"SetBkMode", (void*)&trt_SetBkMode},
      {"SetTextColor", (void*)&trt_SetTextColor},
      {"SetBkColor", (void*)&trt_SetBkColor},
      {"BeginPath", (void*)&trt_BeginPath},
      {"EndPath", (void*)&trt_EndPath},
      {"GetObjectW", (void*)&trt_GetObjectW},
      {"GetObjectA", (void*)&trt_GetObjectA},
      {"GetTextExtentPoint32W", (void*)&trt_GetTextExtentPoint32W},
      {"GetTextExtentPoint32A", (void*)&trt_GetTextExtentPoint32A},
      {"GetTextMetricsW", (void*)&trt_GetTextMetricsW},
      {"GetTextMetricsA", (void*)&trt_GetTextMetricsA},
      {"TextOutW", (void*)&trt_TextOutW},
      {"TextOutA", (void*)&trt_TextOutA},
      {"ExtTextOutW", (void*)&trt_ExtTextOutW},
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
      {"GdiFlush", (void*)&trt_GdiFlush},
      {"ChoosePixelFormat", (void*)&trt_ChoosePixelFormat},
      {"DescribePixelFormat", (void*)&trt_DescribePixelFormat},
      {"SetPixelFormat", (void*)&trt_SetPixelFormat},
      {"GetPixelFormat", (void*)&trt_GetPixelFormat},
      {"GetICMProfileW", (void*)&trt_GetICMProfileW},
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
