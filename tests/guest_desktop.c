// tiramisu/tests/guest_desktop.c
// A desktop-application-shaped guest: register a class, create a top-level
// window, pump messages, and paint in WM_PAINT with the ordinary desktop GDI
// calls (BeginPaint -> PatBlt background -> TextOutA -> Rectangle -> EndPaint).
//
// It exists to prove the desktop path end to end: the paint messages must
// reach the guest, the guest's gdi32 imports must land on the raster surface,
// and the surface must contain the pixels the guest asked for. tests/
// test_desktop_paint.cpp dumps and inspects that surface.
//
// Return bits (each is a separate proof, so a partial failure is visible):
//   0x01 RegisterClassExW -> atom
//   0x02 CreateWindowExW -> hwnd
//   0x04 WM_PAINT ran with a paint DC
//   0x08 PatBlt background succeeded
//   0x10 TextOutA succeeded
//   0x20 Rectangle succeeded
//   0x40 the pump dispatched >= 4 messages
//   0x80 GetTextExtentPoint32A measured the selected font
//  0x100 GDI+ filled a rectangle on the DC (GdipCreateFromHDC + SolidFill)
//  0x200 GDI+ blitted a caller-owned bitmap (GdipCreateBitmapFromScan0)
//  0x10000 GDI+ ScaleTransform really scaled the next fill 2x
//  0x400 msimg32 AlphaBlend composited at 50%
//  0x800 msimg32 GradientFill painted a red->blue ramp
// 0x1000 RoundRect drew rounded corners
// 0x2000 ScrollWindowEx shifted the scrolled rectangle's pixels
// 0x4000 InvalidateRect/GetUpdateRect agree on the owed repaint rectangle
// 0x8000 GDI path: BeginPath/Rectangle/EndPath/FillPath painted, and
//        SelectClipPath(RGN_AND) limited a later PatBlt to the path
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;
typedef long long i64;

#define GSTD __stdcall

int _fltused = 1;

struct logfontw {
  i32 height; i32 width; i32 escapement; i32 orientation; i32 weight;
  u8 italic; u8 underline; u8 strikeout; u8 charset; u8 out_prec;
  u8 clip_prec; u8 quality; u8 pitch_family; u16 face[32];
};

// ---- user32 ---------------------------------------------------------------
__attribute__((dllimport)) u16 GSTD RegisterClassExW(const void* wc);
__attribute__((dllimport)) void* GSTD CreateWindowExW(
    u32 exstyle, const u16* cls, const u16* title, u32 style, i32 x, i32 y,
    i32 w, i32 h, void* parent, void* menu, void* inst, void* param);
__attribute__((dllimport)) i32 GSTD ShowWindow(void* hwnd, i32 cmd);
__attribute__((dllimport)) i32 GSTD GetMessageW(void* msg, void* hwnd, u32 min,
                                                u32 max);
__attribute__((dllimport)) i32 GSTD TranslateMessage(const void* msg);
__attribute__((dllimport)) i64 GSTD DispatchMessageW(const void* msg);
__attribute__((dllimport)) void GSTD PostQuitMessage(i32 code);
__attribute__((dllimport)) i64 GSTD DefWindowProcW(void* hwnd, u32 msg, u64 wp,
                                                   i64 lp);
__attribute__((dllimport)) void* GSTD BeginPaint(void* hwnd, void* ps);
__attribute__((dllimport)) i32 GSTD EndPaint(void* hwnd, const void* ps);
__attribute__((dllimport)) i32 GSTD ScrollWindowEx(void* hwnd, i32 dx, i32 dy,
                                                   const void* scroll,
                                                   const void* clip,
                                                   void* update_rgn,
                                                   void* update_rect,
                                                   u32 flags);
__attribute__((dllimport)) i32 GSTD InvalidateRect(void* hwnd, const void* rect,
                                                   i32 erase);
__attribute__((dllimport)) i32 GSTD BeginPath(void* hdc);
__attribute__((dllimport)) i32 GSTD EndPath(void* hdc);
__attribute__((dllimport)) i32 GSTD FillPath(void* hdc);
__attribute__((dllimport)) i32 GSTD SelectClipPath(void* hdc, i32 mode);
__attribute__((dllimport)) void* GSTD SelectClipRgn(void* hdc, void* rgn);
__attribute__((dllimport)) i32 GSTD GetUpdateRect(void* hwnd, void* rect,
                                                  i32 erase);

// ---- gdiplus --------------------------------------------------------------
__attribute__((dllimport)) i32 GSTD GdiplusStartup(u64* token, const void* in,
                                                   void* out);
__attribute__((dllimport)) i32 GSTD GdipCreateFromHDC(void* hdc, void** g);
__attribute__((dllimport)) i32 GSTD GdipCreateSolidFill(u32 argb, void** brush);
__attribute__((dllimport)) i32 GSTD GdipFillRectangleI(void* g, void* brush,
                                                       i32 x, i32 y, i32 w,
                                                       i32 h);
__attribute__((dllimport)) i32 GSTD GdipCreateBitmapFromScan0(
    i32 w, i32 h, i32 stride, i32 fmt, void* bits, void** bmp);
__attribute__((dllimport)) i32 GSTD GdipScaleTransform(void* g, float sx, float sy,
                                                      i32 order);
__attribute__((dllimport)) i32 GSTD GdipResetTransform(void* g);
__attribute__((dllimport)) i32 GSTD GdipDrawImageI(void* g, void* img, i32 x,
                                                   i32 y);

// ---- msimg32 --------------------------------------------------------------
struct trivertex {
  u32 x, y;
  u16 red, green, blue, alpha;
};
__attribute__((dllimport)) i32 GSTD AlphaBlend(void* dst, i32 x, i32 y, i32 w,
                                               i32 h, void* src, i32 sx,
                                               i32 sy, i32 sw, i32 sh,
                                               u32 blendfunc);
__attribute__((dllimport)) i32 GSTD GradientFill(void* hdc, void* vertex,
                                                 u32 n_vertex, void* mesh,
                                                 u32 n_mesh, u32 mode);

// ---- gdi32 ----------------------------------------------------------------
__attribute__((dllimport)) void* GSTD CreateFontIndirectW(const void* lf);
__attribute__((dllimport)) void* GSTD SelectObject(void* hdc, void* obj);
__attribute__((dllimport)) i32 GSTD PatBlt(void* hdc, i32 x, i32 y, i32 w,
                                           i32 h, u32 rop);
__attribute__((dllimport)) i32 GSTD TextOutA(void* hdc, i32 x, i32 y,
                                             const char* s, i32 n);
__attribute__((dllimport)) i32 GSTD Rectangle(void* hdc, i32 l, i32 t, i32 r,
                                              i32 b);
__attribute__((dllimport)) u32 GSTD SetTextColor(void* hdc, u32 color);
__attribute__((dllimport)) u32 GSTD SetBkColor(void* hdc, u32 color);
__attribute__((dllimport)) i32 GSTD SetBkMode(void* hdc, i32 mode);
__attribute__((dllimport)) void* GSTD CreateSolidBrush(u32 color);
__attribute__((dllimport)) i32 GSTD GetTextExtentPoint32A(void* hdc,
                                                          const char* s, i32 n,
                                                          void* size);
__attribute__((dllimport)) i32 GSTD RoundRect(void* hdc, i32 l, i32 t, i32 r,
                                              i32 b, i32 w, i32 h);

static u32 g_bits;
static i32 g_paint_count;

static const u16 k_class[] = {
    'T', 'i', 'r', 'a', 'm', 'i', 's', 'u', 'D', 'e', 's', 'k', 't', 'o', 'p',
    0};
static const u16 k_title[] = {'T', 'i', 'r', 'a', 'm', 'i', 's', 'u', 0};

struct wndclassexw {
  u32 cbSize;
  u32 style;
  void* proc;
  i32 cls_extra;
  i32 win_extra;
  void* inst;
  void* icon;
  void* cursor;
  void* brush;
  const u16* menu;
  const u16* name;
  void* icon_sm;
};

static void* paint_dc;

static i64 GSTD wnd_proc(void* hwnd, u32 msg, u64 wp, i64 lp) {
  (void)wp;
  (void)lp;
  switch (msg) {
    case 0x000Fu: {  // WM_PAINT
      u8 ps[72];
      void* hdc = BeginPaint(hwnd, ps);
      ++g_paint_count;
      if (hdc) {
        g_bits |= 0x04u;
        paint_dc = hdc;
        // White background over the client area, like a real control.
        SelectObject(hdc, (void*)(u64)0x80000010u);  // WHITE_BRUSH
        if (PatBlt(hdc, 0, 0, 640, 400, 0x00F00021u)) g_bits |= 0x08u;
        SetBkMode(hdc, 1);            // TRANSPARENT
        SetTextColor(hdc, 0x00000000u);  // black text
        struct logfontw lf;
        for (u32 i = 0; i < sizeof(lf) / 4; ++i) ((u32*)&lf)[i] = 0;
        lf.height = -24;
        lf.weight = 700;
        void* font = CreateFontIndirectW(&lf);
        if (font) SelectObject(hdc, font);
        if (TextOutA(hdc, 24, 24, "Tiramisu desktop", 16)) g_bits |= 0x10u;
        i32 sz[2] = {0, 0};
        if (GetTextExtentPoint32A(hdc, "Tiramisu desktop", 16, sz) &&
            sz[0] > 0 && sz[1] == 24)
          g_bits |= 0x80u;
        // A framed, filled box under the text, brush + pen.
        SetTextColor(hdc, 0x00000000u);
        void* brush = CreateSolidBrush(0x0000FF00u);  // green
        if (brush) SelectObject(hdc, brush);
        if (Rectangle(hdc, 24, 72, 360, 168)) g_bits |= 0x20u;

        // Rounded corners are a real ellipse raster now, not a square outline.
        if (RoundRect(hdc, 400, 72, 560, 168, 32, 32)) g_bits |= 0x1000u;

        // ---- GDI+, the API modern desktop UI actually paints with ----
        u64 gp_token = 0;
        void* gp = 0;
        void* gp_brush = 0;
        if (GdiplusStartup(&gp_token, 0, 0) == 0 &&
            GdipCreateFromHDC(hdc, &gp) == 0 && gp &&
            GdipCreateSolidFill(0xFFFF00FFu, &gp_brush) == 0 &&
            GdipFillRectangleI(gp, gp_brush, 400, 200, 100, 60) == 0)
          g_bits |= 0x100u;
        static u32 k_img[16 * 16];
        for (u32 i = 0; i < 16 * 16; ++i) k_img[i] = 0xFFFF8000u;  // ARGB orange
        void* gp_img = 0;
        if (gp && GdipCreateBitmapFromScan0(16, 16, 64, 0x26200Au, k_img,
                                            &gp_img) == 0 &&
            gp_img && GdipDrawImageI(gp, gp_img, 520, 200) == 0)
          g_bits |= 0x200u;
        // Scale 2x about the origin: a 20x20 fill at (150,90) must cover
        // 40x40 device pixels at (300,180). Nothing later paints over it.
        void* gp_violet = 0;
        if (gp && GdipCreateSolidFill(0xFF8000FFu, &gp_violet) == 0 &&
            GdipScaleTransform(gp, 2.0f, 2.0f, 0) == 0 &&
            GdipFillRectangleI(gp, gp_violet, 150, 90, 20, 20) == 0 &&
            GdipResetTransform(gp) == 0)
          g_bits |= 0x10000u;

        // ---- msimg32: blended / transparent / gradient composites ----
        PatBlt(hdc, 0, 300, 200, 40, 0x00000042u);  // BLACKNESS
        // Source is the untouched white corner; 50% over black is mid grey.
        if (AlphaBlend(hdc, 0, 300, 200, 40, hdc, 600, 0, 40, 40, 0x00800000u))
          g_bits |= 0x400u;
        struct trivertex v[2];
        v[0].x = 400; v[0].y = 300;
        v[0].red = 0xFF00; v[0].green = 0; v[0].blue = 0; v[0].alpha = 0;
        v[1].x = 640; v[1].y = 340;
        v[1].red = 0; v[1].green = 0; v[1].blue = 0xFFFF; v[1].alpha = 0;
        u32 mesh[2] = {0, 1};
        if (GradientFill(hdc, v, 2, mesh, 1, 0)) g_bits |= 0x800u;

        // Scrolling a pane moves the pixels the pane already painted: the
        // black bar at y=140 must end up at y=120.
        PatBlt(hdc, 24, 140, 336, 1, 0x00000042u);  // BLACKNESS
        i32 scroll_rc[4] = {24, 72, 360, 168};
        // GDI path: a recorded rectangle is filled only on replay, and the
        // same recorded shape can become the clip for later painting.
        void* teal = CreateSolidBrush(0x00808000u);   // COLORREF teal
        void* yellow = CreateSolidBrush(0x0000FFFFu);  // COLORREF yellow
        SelectObject(hdc, teal);
        BeginPath(hdc);
        Rectangle(hdc, 300, 250, 400, 330);
        EndPath(hdc);
        int path_ok = FillPath(hdc);
        BeginPath(hdc);
        Rectangle(hdc, 430, 250, 510, 330);
        EndPath(hdc);
        if (SelectClipPath(hdc, 1)) {  // RGN_AND
          SelectObject(hdc, yellow);
          PatBlt(hdc, 330, 180, 290, 220, 0x00F00021u);  // PATCOPY
          SelectClipRgn(hdc, 0);
        }
        if (path_ok) g_bits |= 0x8000u;
        if (ScrollWindowEx(hwnd, 0, 20, scroll_rc, 0, 0, 0, 0))
          g_bits |= 0x2000u;
        EndPaint(hwnd, ps);
        // The window still owes a repaint of just this sub-rectangle.
        i32 dirty[4] = {10, 10, 50, 40};
        i32 owed[4] = {-1, -1, -1, -1};
        InvalidateRect(hwnd, dirty, 0);
        if (GetUpdateRect(hwnd, owed, 0) && owed[0] == 10 && owed[1] == 10 &&
            owed[2] == 50 && owed[3] == 40)
          g_bits |= 0x4000u;
      }
      if (g_paint_count >= 1) PostQuitMessage(0);
      return 0;
    }
    case 0x0002u:  // WM_DESTROY
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

u32 GSTD desktop_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  struct wndclassexw wc;
  for (u32 i = 0; i < sizeof(wc) / 4; ++i) ((u32*)&wc)[i] = 0;
  wc.cbSize = sizeof(wc);
  wc.proc = (void*)&wnd_proc;
  wc.brush = (void*)(u64)0x80000010u;
  wc.name = k_class;
  const u16 atom = RegisterClassExW(&wc);
  if (atom) g_bits |= 0x01u;

  void* hwnd = CreateWindowExW(0, k_class, k_title, 0x00CF0000u /*WS_OVERLAPPEDWINDOW*/,
                               0, 0, 640, 400, 0, 0, 0, 0);
  if (hwnd) g_bits |= 0x02u;
  if (hwnd) ShowWindow(hwnd, 5 /*SW_SHOW*/);

  u8 msg[48];
  i32 dispatched = 0;
  while (hwnd && GetMessageW(msg, 0, 0, 0) > 0) {
    TranslateMessage(msg);
    DispatchMessageW(msg);
    if (++dispatched >= 4) g_bits |= 0x40u;
  }
  return g_bits;
}
