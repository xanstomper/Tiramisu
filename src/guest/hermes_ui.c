// tiramisu/src/guest/hermes_ui.c
// Terminal front end for the native Hermes agent.
//
// The host gives a guest a real D3D11 device and swapchain, but GDI text is
// record-only ("glyphs not drawn") and there is no console window.  So this
// file owns the screen: an 80-column text grid rasterised on the CPU into the
// backbuffer (UpdateSubresource + Present, no shaders), plus keyboard input
// from the guest window's WM_CHAR stream (the host's input bridge posts it for
// a USB keyboard).
//
// It is a terminal, not a status card: scrollback, line editing, history,
// /commands and a prompt.  The agent loop lives in hermes.c.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <xinput.h>
#include "font8x8.h"

#ifndef AGENT_BRAND
#define AGENT_BRAND "Hermes"
#endif

#define TCOLS 80
#define TMAX 600
#define INMAX 220

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_sc;
static HWND g_hwnd;
static unsigned char* g_pix;
static UINT g_w, g_h;
static int g_ready;
static int g_rgba;

// --- terminal state ---------------------------------------------------------
static char g_lines[TMAX][TCOLS + 1];
static unsigned char g_lcol[TMAX];
static int g_nlines;
static int g_scroll;  // 0 = tail, >0 = lines scrolled up
static char g_status[96];
static char g_input[INMAX + 1];
static int g_inlen;
static int g_submitted;
static char g_submit[INMAX + 1];
static char g_hist[16][INMAX + 1];
static int g_histn;
static int g_histpos;

static LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CHAR) {
    const unsigned ch = (unsigned)w;
    if (ch == 8) {
      if (g_inlen > 0) g_input[--g_inlen] = 0;
      return 0;
    }
    if (ch == 13) {
      if (g_inlen > 0) {
        for (int i = 0; i <= g_inlen; ++i) g_submit[i] = g_input[i];
        g_submitted = 1;
        g_inlen = 0;
        g_input[0] = 0;
      }
      return 0;
    }
    if (ch == 9) {
      for (int k = 0; k < 2 && g_inlen < INMAX - 1; ++k) g_input[g_inlen++] = ' ';
      g_input[g_inlen] = 0;
      return 0;
    }
    if (ch >= 32 && ch < 127 && g_inlen < INMAX - 1) {
      g_input[g_inlen++] = (char)ch;
      g_input[g_inlen] = 0;
    }
    return 0;
  }
  if (m == WM_KEYDOWN) {
    const int vk = (int)w;
    if (vk == 0x26 || vk == 0x28) {  // Up / Down = history
      int n = g_histn < 16 ? g_histn : 16;
      if (n > 0) {
        if (vk == 0x26) {
          if (g_histpos < n) g_histpos++;
          const int idx = (g_histn - g_histpos) % 16;
          for (int i = 0; i < INMAX; ++i) {
            g_input[i] = g_hist[idx][i];
            if (!g_input[i]) break;
          }
        } else {
          if (g_histpos > 1) g_histpos--;
          else g_histpos = 0;
          if (g_histpos == 0) g_input[0] = 0;
          else {
            const int idx = (g_histn - g_histpos) % 16;
            for (int i = 0; i < INMAX; ++i) {
              g_input[i] = g_hist[idx][i];
              if (!g_input[i]) break;
            }
          }
        }
        g_inlen = (int)lstrlenA(g_input);
      }
      return 0;
    }
    if (vk == 0x21) {  // PageUp
      g_scroll += 12;
      return 0;
    }
    if (vk == 0x22) {  // PageDown
      g_scroll -= 12;
      if (g_scroll < 0) g_scroll = 0;
      return 0;
    }
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

static int ui_alloc(UINT w, UINT h) {
  if (g_pix && w == g_w && h == g_h) return 1;
  if (g_pix) HeapFree(GetProcessHeap(), 0, g_pix);
  g_pix = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
  g_w = w;
  g_h = h;
  return g_pix != 0;
}

static void px(UINT x, UINT y, unsigned char r, unsigned char g, unsigned char b) {
  if (!g_pix || x >= g_w || y >= g_h) return;
  unsigned char* p = g_pix + ((SIZE_T)y * g_w + x) * 4;
  if (g_rgba) { p[0] = r; p[1] = g; p[2] = b; }
  else        { p[0] = b; p[1] = g; p[2] = r; }
  p[3] = 255;
}

static void fill(int x, int y, int w, int h, unsigned char r,
                 unsigned char g, unsigned char b) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) px((UINT)xx, (UINT)yy, r, g, b);
}

// 8x8 glyphs, bit 0 = leftmost pixel.  Advance is exactly 8*scale so a row is
// 80 cells across a 1280px surface.
static void draw_str(int x, int y, int scale, const char* s,
                     unsigned char r, unsigned char g, unsigned char b) {
  if (!s) return;
  int cx = x;
  for (; *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c < 32 || c > 126) c = '?';
    const unsigned char* gl = k_font8[c - 32];
    for (int row = 0; row < 8; ++row) {
      const unsigned char bits = gl[row];
      for (int col = 0; col < 8; ++col) {
        if (bits & (1u << col)) {
          if (scale == 1) px((UINT)(cx + col), (UINT)(y + row), r, g, b);
          else fill(cx + col * scale, y + row * scale, scale, scale, r, g, b);
        }
      }
    }
    cx += 8 * scale;
    if (cx > (int)g_w - 8 * scale) break;
  }
}

static void term_redraw(void);

int term_init(void) {
  if (g_ready) return 1;
  if (!g_hwnd) {
    static const wchar_t cls[] = L"TiramisuTerminal";
    WNDCLASSEXW wc;
    for (unsigned i = 0; i < sizeof(wc); ++i) ((unsigned char*)&wc)[i] = 0;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ui_wndproc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, cls, L"hermes", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             0, 0, 1280, 720, 0, 0, wc.hInstance, 0);
  }
  DXGI_SWAP_CHAIN_DESC sd;
  for (unsigned i = 0; i < sizeof(sd); ++i) ((unsigned char*)&sd)[i] = 0;
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 1280;
  sd.BufferDesc.Height = 720;
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = g_hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0,
                                 D3D_FEATURE_LEVEL_10_1};
  D3D_FEATURE_LEVEL got = 0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, levels, 2, D3D11_SDK_VERSION, &sd,
      &g_sc, &g_dev, &got, &g_ctx);
  if (FAILED(hr) || !g_sc || !g_ctx) return 0;

  ID3D11Texture2D* bb = 0;
  if (FAILED(g_sc->lpVtbl->GetBuffer(g_sc, 0, &IID_ID3D11Texture2D,
                                     (void**)&bb)) ||
      !bb)
    return 0;
  D3D11_TEXTURE2D_DESC td;
  bb->lpVtbl->GetDesc(bb, &td);
  bb->lpVtbl->Release(bb);
  g_rgba = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  if (!ui_alloc(td.Width, td.Height)) return 0;
  g_ready = 1;
  term_redraw();
  return 1;
}

static void term_redraw(void) {
  if (!g_ready) return;
  ID3D11Texture2D* bb = 0;
  if (FAILED(g_sc->lpVtbl->GetBuffer(g_sc, 0, &IID_ID3D11Texture2D,
                                     (void**)&bb)) ||
      !bb)
    return;
  D3D11_TEXTURE2D_DESC td;
  bb->lpVtbl->GetDesc(bb, &td);
  if (!ui_alloc(td.Width, td.Height)) {
    bb->lpVtbl->Release(bb);
    return;
  }

  const int W = (int)g_w, H = (int)g_h;
  fill(0, 0, W, H, 8, 10, 15);
  fill(0, 0, W, 3, 64, 190, 160);
  draw_str(16, 10, 2, AGENT_BRAND, 235, 242, 250);
  if (g_status[0]) {
    int n = 0;
    while (g_status[n]) ++n;
    const int w = n * 16;
    draw_str(W - 16 - w, 10, 2, g_status, 120, 215, 190);
  }
  fill(16, 32, W - 32, 1, 38, 46, 58);

  const int cell = 16;
  const int top = 40;
  const int bottom = H - 92;
  int rows = (bottom - top) / cell;
  if (rows < 8) rows = 8;
  if (rows > TMAX) rows = TMAX;
  int max_scroll = g_nlines - rows;
  if (max_scroll < 0) max_scroll = 0;
  if (g_scroll > max_scroll) g_scroll = max_scroll;
  int start = g_nlines - rows + g_scroll;
  if (start < 0) start = 0;

  for (int i = 0; i < rows; ++i) {
    const int li = start + i;
    if (li >= g_nlines) break;
    if (!g_lines[li][0]) continue;
    const unsigned char k = g_lcol[li];
    unsigned char r = 198, g = 206, b = 218;
    if (k == 1) { r = 120; g = 215; b = 190; }   // user
    else if (k == 2) { r = 130; g = 140; b = 158; }  // tool
    else if (k == 3) { r = 235; g = 110; b = 110; }  // error
    else if (k == 4) { r = 235; g = 196; b = 120; }  // system/accent
    draw_str(16, top + i * cell, 2, g_lines[li], r, g, b);
  }

  // prompt
  const int py = H - 84;
  fill(0, py - 10, W, 1, 38, 46, 58);
  draw_str(16, py, 2, ">", 120, 215, 190);
  draw_str(16 + 24, py, 2, g_input, 235, 242, 250);
  const int cx = 16 + 24 + g_inlen * 16;
  fill(cx, py, 10, 16, 120, 215, 190);

  draw_str(16, H - 34, 1,
           "USB KEYBOARD  ENTER SEND  PGUP/PGDN SCROLL  /HELP  /CLEAR  /QUIT",
           78, 86, 102);
  draw_str(16, H - 50, 1,
           "TOOLS ARE REAL: LIST_DIR READ_FILE WRITE_FILE APPEND_FILE MAKE_DIR STAT DELETE_FILE SHELL SYSINFO",
           58, 68, 82);

  g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource*)bb, 0, 0, g_pix,
                                   (UINT)(g_w * 4), 0);
  bb->lpVtbl->Release(bb);
  g_sc->lpVtbl->Present(g_sc, 0, 0);
}

static void term_newline(int color) {
  if (g_nlines > 0) g_lcol[g_nlines - 1] = (unsigned char)color;
  if (g_nlines >= TMAX) {
    for (int i = 1; i < TMAX; ++i) {
      for (int k = 0; k <= TCOLS; ++k) g_lines[i - 1][k] = g_lines[i][k];
      g_lcol[i - 1] = g_lcol[i];
    }
    --g_nlines;
  }
  g_lines[g_nlines][0] = 0;
  g_lcol[g_nlines] = (unsigned char)color;
  ++g_nlines;
  g_scroll = 0;
}

void term_clear(void) {
  g_nlines = 0;
  g_scroll = 0;
  term_newline(0);
  term_redraw();
}

void term_status(const char* s) {
  if (!s) s = "";
  int i = 0;
  for (; s[i] && i < 95; ++i) g_status[i] = s[i];
  g_status[i] = 0;
  term_redraw();
}

void term_write_c(const char* s, int color) {
  if (!s) return;
  if (g_nlines == 0) term_newline(color);
  for (; *s; ++s) {
    char c = *s;
    if (c == '\r') continue;
    if (c == '\n') { term_newline(color); continue; }
    if (c == '\t') {
      for (int k = 0; k < 2; ++k) {
        int n = (int)lstrlenA(g_lines[g_nlines - 1]);
        if (n < TCOLS) { g_lines[g_nlines - 1][n] = ' '; g_lines[g_nlines - 1][n + 1] = 0; }
      }
      continue;
    }
    if (c < 32) continue;
    if (c > 126) c = '?';
    int n = (int)lstrlenA(g_lines[g_nlines - 1]);
    if (n >= TCOLS) term_newline(color);
    n = (int)lstrlenA(g_lines[g_nlines - 1]);
    g_lines[g_nlines - 1][n] = c;
    g_lines[g_nlines - 1][n + 1] = 0;
    g_lcol[g_nlines - 1] = (unsigned char)color;
  }
  g_scroll = 0;
  term_redraw();
}

void term_write(const char* s) { term_write_c(s, 0); }

int term_poll(void) {
  MSG m;
  while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (g_submitted) {
    g_submitted = 0;
    return 1;
  }
  return 0;
}

const char* term_line(void) { return g_submit; }

void term_push_history(const char* s) {
  if (!s || !s[0]) return;
  // Ignore an immediate duplicate of the previous entry.
  const int last = (g_histn - 1) % 16;
  int same = 1;
  if (g_histn == 0) same = 0;
  else for (int i = 0; i < INMAX; ++i) { if (g_hist[last][i] != s[i]) { same = 0; break; } if (!s[i]) break; }
  if (!same) {
    for (int i = 0; i < INMAX; ++i) { g_hist[g_histn % 16][i] = s[i]; if (!s[i]) break; }
    ++g_histn;
  }
  g_histpos = 0;
}

int term_ready(void) { return g_ready; }
