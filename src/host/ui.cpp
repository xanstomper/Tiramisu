// tiramisu/src/host/ui.cpp
// DirectWrite/D2D overlay. Pixel-consistent: one text scale, fixed margins,
// everything positioned from the 1920x1080 design grid scaled to the
// swapchain size.
#include "ui.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <stdint.h>

#include "settings.h"
#include "vendor/qrcodegen.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace longan {

namespace {
float g_w = 1920, g_h = 1080;
ID2D1DeviceContext* g_ctx = nullptr;
ID2D1Device* g_d2d_device = nullptr;
IDXGISwapChain1* g_swap = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
ID2D1SolidColorBrush* g_teal = nullptr;
ID2D1SolidColorBrush* g_dim = nullptr;
IDWriteFactory* g_dw = nullptr;
IDWriteTextFormat* g_title = nullptr;
IDWriteTextFormat* g_text = nullptr;
IDWriteTextFormat* g_small = nullptr;

std::vector<std::wstring> g_lines;
std::vector<std::wstring> g_setup_steps;
int g_setup_done_count = 0;
bool g_setup_finished = false;
std::vector<ui_game> g_games;
int g_selected = 0;
std::string g_qr_text;
uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
int g_qr_size = 0;

// dashboard screens
ui_screen g_screen = UI_SHELF;
bool g_quick_menu = false;
float g_fps = 0.0f;
std::wstring g_sysinfo;
std::vector<ui_setting> g_set_rows;
int g_set_selected = 0;
std::vector<std::wstring> g_log_tail;
}

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v) {
  g_w = (float)width;
  g_h = (float)height;
  ID2D1Factory1* f = (ID2D1Factory1*)d2d_factory_v;
  g_d2d_device = (ID2D1Device*)d2d_device_v;
  g_dw = (IDWriteFactory*)dwrite_factory_v;
  g_swap = (IDXGISwapChain1*)swapchain_v;
  f->AddRef();
  g_dw->AddRef();
  g_d2d_device->AddRef();
  if (g_swap) g_swap->AddRef();

  g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx);

  D2D1_COLOR_F white{0.94f, 0.96f, 0.97f, 1.0f};
  D2D1_COLOR_F teal{0.125f, 0.502f, 0.553f, 1.0f};
  D2D1_COLOR_F dim{0.45f, 0.52f, 0.55f, 1.0f};
  g_ctx->CreateSolidColorBrush(white, &g_brush);
  g_ctx->CreateSolidColorBrush(teal, &g_teal);
  g_ctx->CreateSolidColorBrush(dim, &g_dim);

  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         72.0f, L"en-US", &g_title);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         34.0f, L"en-US", &g_text);
  g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         26.0f, L"en-US", &g_small);
}

void ui_shutdown() {
  if (g_ctx) g_ctx->Release();
  if (g_brush) g_brush->Release();
  if (g_teal) g_teal->Release();
  if (g_dim) g_dim->Release();
  if (g_title) g_title->Release();
  if (g_text) g_text->Release();
  if (g_small) g_small->Release();
  if (g_dw) g_dw->Release();
  if (g_d2d_device) g_d2d_device->Release();
  if (g_swap) g_swap->Release();
  g_ctx = nullptr;
  g_brush = g_teal = g_dim = nullptr;
  g_title = g_text = g_small = nullptr;
  g_dw = nullptr;
  g_d2d_device = nullptr;
  g_swap = nullptr;
}

void ui_set_games(const std::vector<ui_game>& games, int selected) {
  g_games = games;
  g_selected = selected;
}

void ui_set_lines(const std::vector<std::wstring>& lines) { g_lines = lines; }

void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished) {
  g_setup_steps = steps;
  g_setup_done_count = done_count;
  g_setup_finished = finished;
}

void ui_set_screen(ui_screen s) { g_screen = s; }

void ui_set_quick_menu(bool visible) {
  g_quick_menu = visible;
  if (visible) g_screen = UI_QUICKMENU;
  else g_screen = UI_SHELF;
}

void ui_set_fps(float fps) { g_fps = fps; }
void ui_set_sysinfo(const std::wstring& info) { g_sysinfo = info; }

void ui_set_settings(const ui_setting* rows, int count, int selected) {
  g_set_rows.assign(rows, rows + (count > 0 ? count : 0));
  g_set_selected = selected;
}

void ui_set_log_tail(const std::vector<std::wstring>& lines) {
  g_log_tail = lines;
}

void ui_set_qr(const std::string& qr_text) {
  g_qr_text = qr_text;
  g_qr_size = 0;
  if (!qr_text.empty()) {
    qrcodegen_encodeText(qr_text.c_str(), g_qr, g_qr, qrcodegen_Ecc_MEDIUM,
                         qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                         qrcodegen_Mask_AUTO, true);
    g_qr_size = qrcodegen_getSize(g_qr);
  }
}

void text_out(IDWriteTextFormat* fmt, float x, float y, float w, float h,
              const wchar_t* s, ID2D1SolidColorBrush* br) {
  D2D1_RECT_F r{x, y, x + w, y + h};
  g_ctx->DrawText(s, (UINT32)wcslen(s), fmt, r, br);
}

void draw_qr_overlay(float m) {
  if (g_qr_size <= 0) return;
  const float q = g_h * 0.30f;
  const float qx = g_w - m - q, qy = g_h - m - q - g_h * 0.04f;
  D2D1_RECT_F bg{qx - 14, qy - 14, qx + q + 14, qy + q + 14};
  D2D1_COLOR_F white{0.94f, 0.96f, 0.97f, 1.0f};
  ID2D1SolidColorBrush* wbr = nullptr;
  g_ctx->CreateSolidColorBrush(white, &wbr);
  g_ctx->FillRectangle(&bg, wbr);
  const float cell = q / (float)g_qr_size;
  D2D1_COLOR_F black{0.05f, 0.07f, 0.086f, 1.0f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(black, &bbr);
  for (int yy = 0; yy < g_qr_size; ++yy)
    for (int xx = 0; xx < g_qr_size; ++xx)
      if (qrcodegen_getModule(g_qr, xx, yy)) {
        D2D1_RECT_F c{qx + xx * cell, qy + yy * cell, qx + (xx + 1) * cell,
                      qy + (yy + 1) * cell};
        g_ctx->FillRectangle(&c, bbr);
      }
  bbr->Release();
  wbr->Release();
  text_out(g_small, qx - 14, qy + q + 18, q + 28, g_h * 0.04f,
           L"SCAN OR OPEN LAN URL", g_teal);
}

void draw_quick_menu(float m) {
  // RetroArch-style semi-transparent overlay on top of the running game
  const float panel_w = g_w * 0.32f;
  const float panel_h = g_h * 0.65f;
  const float px = m;
  const float py = m + g_h * 0.05f;

  // backdrop
  D2D1_COLOR_F bg{0.05f, 0.07f, 0.09f, 0.88f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_RECT_F panel{px, py, px + panel_w, py + panel_h};
  g_ctx->FillRectangle(&panel, bbr);
  bbr->Release();

  // border
  D2D1_COLOR_F border{0.13f, 0.83f, 0.93f, 0.6f};
  ID2D1SolidColorBrush* bor = nullptr;
  g_ctx->CreateSolidColorBrush(border, &bor);
  g_ctx->DrawRectangle(&panel, bor, 2.0f);
  bor->Release();

  // title
  text_out(g_title, px + 16, py + 8, panel_w - 32, g_h * 0.06f,
           L"Quick Menu", g_teal);

  float y = py + g_h * 0.08f;
  const float row_h = g_h * 0.045f;

  // FPS
  if (g_fps > 0) {
    wchar_t fpsbuf[64];
    swprintf(fpsbuf, 64, L"FPS: %.1f", g_fps);
    text_out(g_text, px + 16, y, panel_w - 32, row_h, fpsbuf, g_brush);
    y += row_h;
  }

  // System info
  if (!g_sysinfo.empty()) {
    text_out(g_small, px + 16, y, panel_w - 32, row_h, g_sysinfo.c_str(), g_dim);
    y += row_h * 1.5f;
  }

  // Menu items
  const wchar_t* items[] = {
    L"  Resume Game",
    L"  Settings",
    L"  Save State",
    L"  Load State",
    L"  Reset Game",
    L"  Exit to Shelf",
  };
  for (int i = 0; i < 6; ++i) {
    text_out(g_text, px + 16, y, panel_w - 32, row_h, items[i], g_brush);
    y += row_h;
  }

  // hint
  text_out(g_small, px + 16, py + panel_h - g_h * 0.04f, panel_w - 32,
           g_h * 0.03f, L"Back+Start: close menu", g_dim);
}

void draw_fps_overlay() {
  if (g_fps <= 0) return;
  wchar_t buf[32];
  swprintf(buf, 32, L"%.0f FPS", g_fps);
  // top-right corner
  D2D1_COLOR_F bg{0.0f, 0.0f, 0.0f, 0.6f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_RECT_F r{g_w - 120, 8, g_w - 8, 36};
  g_ctx->FillRectangle(&r, bbr);
  bbr->Release();
  text_out(g_small, g_w - 112, 10, 100, 24, buf, g_teal);
}

void draw_sysinfo_overlay() {
  if (g_sysinfo.empty()) return;
  D2D1_COLOR_F bg{0.0f, 0.0f, 0.0f, 0.6f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_RECT_F r{8, 8, g_w * 0.35f, 28};
  g_ctx->FillRectangle(&r, bbr);
  bbr->Release();
  text_out(g_small, 12, 10, g_w * 0.35f - 8, 20, g_sysinfo.c_str(), g_dim);
}

void draw_tabs(float m) {
  // three-screen tab strip: SHELF / SETTINGS / LOG — teal on the active tab
  const wchar_t* names[3] = {L"SHELF", L"SETTINGS", L"LIVE LOG"};
  const float tab_w = g_w * 0.115f, tab_h = g_h * 0.052f;
  for (int i = 0; i < 3; ++i) {
    const float tx = m + i * (tab_w + g_w * 0.012f);
    D2D1_RECT_F bg{tx, m * 0.52f, tx + tab_w, m * 0.52f + tab_h};
    if ((int)g_screen == i) g_ctx->FillRectangle(&bg, g_teal);
    else {
      D2D1_COLOR_F slate{0.10f, 0.13f, 0.17f, 0.85f};
      ID2D1SolidColorBrush* sbr = nullptr;
      g_ctx->CreateSolidColorBrush(slate, &sbr);
      g_ctx->FillRectangle(&bg, sbr);
      sbr->Release();
    }
    D2D1_COLOR_F fg{0.94f, 0.96f, 0.97f, 1.0f};
    ID2D1SolidColorBrush* fbr = nullptr;
    g_ctx->CreateSolidColorBrush(fg, &fbr);
    D2D1_RECT_F tr{tx + 10, m * 0.52f, tx + tab_w, m * 0.52f + tab_h};
    // small centered label
    g_ctx->DrawText(names[i], (UINT32)wcslen(names[i]), g_small, tr, fbr);
    fbr->Release();
  }
}

void draw_settings_screen(float m, float y0) {
  if (g_set_rows.empty()) {
    text_out(g_text, m, y0, g_w - 2 * m, g_h * 0.06f,
             L"No settings rows (host not ready)", g_dim);
    return;
  }
  const float row_h = g_h * 0.058f;
  const float lab_w = g_w * 0.38f;
  const float val_w = g_w * 0.34f;
  wchar_t hint[128];
  const ui_setting& sel = g_set_rows[g_set_selected];
  if (sel.is_bool) wcscpy(hint, L"A: toggle");
  else if (sel.is_choice) wcscpy(hint, L"LEFT/RIGHT: change");
  else if (sel.is_int) wcscpy(hint, L"UP/DOWN: change");
  else wcscpy(hint, L"keyboard: type a new value");
  const int max_rows = 12;
  int start = 0;
  if ((int)g_set_rows.size() > max_rows) {
    start = g_set_selected - 6;
    if (start < 0) start = 0;
    if (start + max_rows > (int)g_set_rows.size())
      start = (int)g_set_rows.size() - max_rows;
  }
  int shown = 0;
  for (int i = start;
       i < (int)g_set_rows.size() && shown < max_rows; ++i, ++shown) {
    const float ry = y0 + shown * row_h;
    if (i == g_set_selected) {
      D2D1_RECT_F sel{m - 12, ry - 3, m + g_w - 2 * m + 12, ry + row_h};
      g_ctx->FillRectangle(&sel, g_teal);
    }
    D2D1_COLOR_F onsel{0.05f, 0.07f, 0.086f, 1.0f};
    ID2D1SolidColorBrush* fg = g_dim;
    if (i == g_set_selected) g_ctx->CreateSolidColorBrush(onsel, &fg);
    text_out(g_text, m, ry, lab_w, row_h, g_set_rows[i].label,
             i == g_set_selected ? fg : g_brush);
    std::wstring val = g_set_rows[i].value;
    if (g_set_rows[i].is_bool) {
      val = (wcscmp(g_set_rows[i].value, L"1") == 0) ? L"ON" : L"OFF";
    }
    text_out(g_text, m + lab_w, ry, val_w, row_h, val.c_str(),
             i == g_set_selected ? fg : g_teal);
    if (i == g_set_selected) fg->Release();
  }
  text_out(g_small, m, g_h - m * 1.15f, g_w - 2 * m, g_h * 0.04f, hint,
           g_dim);
}

void draw_log_screen(float m, float y0) {
  const float row_h = g_h * 0.040f;
  const int max_rows = (int)((g_h - y0 - m * 1.2f) / row_h);
  int start = (int)g_log_tail.size() - max_rows;
  if (start < 0) start = 0;
  float y = y0;
  for (size_t i = (size_t)start; i < g_log_tail.size(); ++i) {
    text_out(g_small, m, y, g_w - 2 * m, row_h, g_log_tail[i].c_str(), g_dim);
    y += row_h;
  }
  if (g_log_tail.empty())
    text_out(g_text, m, y0, g_w - 2 * m, row_h * 2,
             L"Log is empty — boot activity lands here live", g_dim);
}

void draw_lan_link(float m) {
  // the transfer URL, big enough to type from a laptop across the room
  if (g_qr_text.empty()) return;
  std::wstring url(g_qr_text.begin(), g_qr_text.end());
  text_out(g_small, m, g_h - m * 0.62f, g_w - 2 * m, g_h * 0.042f,
           (L"LAN LINK  " + url).c_str(), g_teal);
}

void ui_render() {
  if (!g_ctx || !g_swap) return;
  // bind this frame's backbuffer as the D2D target
  ID2D1Image* old_target = nullptr;
  g_ctx->GetTarget(&old_target);
  ID2D1Bitmap1* bmp = nullptr;
  IDXGISurface* surf = nullptr;
  if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) {
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bp.dpiX = 96.0f;
    bp.dpiY = 96.0f;
    if (FAILED(g_ctx->CreateBitmapFromDxgiSurface(surf, &bp, &bmp)) || !bmp) {
      surf->Release();
      if (old_target) old_target->Release();
      return;
    }
    g_ctx->SetTarget(bmp);
    g_ctx->BeginDraw();

    const float m = g_h * 0.06f;

    // ---- first-run setup screen ----
    if (!g_setup_steps.empty()) {
      text_out(g_title, m, m * 0.55f, g_w - 2 * m, g_h * 0.1f,
               L"Tiramisu Setup", g_teal);
      float sy = m + g_h * 0.12f;
      const float step_h = g_h * 0.062f;
      const float setup_w = (g_qr_size > 0) ? (g_w - m - (g_h * 0.30f + 56.0f)) : (g_w - 2 * m);
      for (size_t i = 0; i < g_setup_steps.size() && sy < g_h - m; ++i) {
        // done = teal check, current = white, pending = dim
        const bool done = (int)i < g_setup_done_count;
        const bool cur = (int)i == g_setup_done_count && !g_setup_finished;
        wchar_t mark[4] = L"+  ";
        if (done) { mark[0] = (wchar_t)0x2713; mark[1] = L' '; mark[2] = L' '; }
        const std::wstring row = std::wstring(mark) + g_setup_steps[i];
        if (done) text_out(g_text, m, sy, setup_w, step_h, row.c_str(), g_teal);
        else if (cur) text_out(g_text, m, sy, setup_w, step_h, row.c_str(), g_brush ? g_brush : g_dim);
        else text_out(g_text, m, sy, setup_w, step_h, row.c_str(), g_dim);
        sy += step_h;
      }
      if (g_setup_finished)
        text_out(g_text, m, g_h - m - g_h * 0.06f, setup_w, g_h * 0.06f,
                 L"Setup complete — scan QR or press A for shelf", g_teal);
      draw_qr_overlay(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }

    // title + tab strip
    text_out(g_title, m, m * 0.55f, g_w * 0.5f, g_h * 0.1f, L"Tiramisu", g_teal);
    draw_tabs(m);

    // transfer URL + status lines
    float y = m + g_h * 0.1f;
    for (const auto& l : g_lines) {
      text_out(g_small, m, y, g_w - 2 * m, g_h * 0.045f, l.c_str(), g_dim);
      y += g_h * 0.042f;
    }

    if (g_screen == UI_SETTINGS) {
      draw_settings_screen(m, y + g_h * 0.01f);
      draw_qr_overlay(m);
      draw_lan_link(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }
    if (g_screen == UI_QUICKMENU) {
      draw_quick_menu(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }
    if (g_screen == UI_LOG) {
      draw_log_screen(m, y + g_h * 0.01f);
      draw_lan_link(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }

    // game shelf (scrollable 6-row window clear of bottom-right QR)
    y += g_h * 0.02f;
    const float row_h = g_h * 0.072f;
    const float qr_q = (g_qr_size > 0) ? (g_h * 0.30f) : 0.0f;
    const float shelf_w =
        (g_qr_size > 0) ? (g_w - m - (qr_q + 56.0f + m * 0.4f)) : (g_w - 2 * m);
    const int total = (int)g_games.size();
    const int max_rows = 6;
    int start = 0;
    if (total > max_rows) {
      start = g_selected - 2;
      if (start < 0) start = 0;
      if (start + max_rows > total) start = total - max_rows;
    }
    if (start > 0) {
      text_out(g_small, m, y - g_h * 0.025f, shelf_w, g_h * 0.03f,
               L"▲ more games above", g_dim);
    }
    int shown = 0;
    for (int i = start; i < total && shown < max_rows; ++i) {
      const float a = i == g_selected ? 1.0f : 0.0f;
      if (a > 0.0f) {
        D2D1_RECT_F sel{m - 12, y - 4, m + shelf_w + 12, y + row_h};
        g_ctx->FillRectangle(&sel, g_teal);
      }
      // selection row inverts: dark text on teal
      ID2D1SolidColorBrush* br = g_dim;
      D2D1_COLOR_F onsel{0.05f, 0.07f, 0.086f, 1.0f};
      ID2D1SolidColorBrush* fg = g_dim;
      if (i == g_selected) {
        g_ctx->CreateSolidColorBrush(onsel, &fg);
      }
      std::wstring row = g_games[i].name + L"   —   " + g_games[i].exe;
      if (!g_games[i].meta.empty()) row += L"  [" + g_games[i].meta + L"]";
      text_out(g_text, m, y, shelf_w, row_h, row.c_str(),
               i == g_selected ? fg : br);
      if (i == g_selected) fg->Release();
      y += row_h;
      ++shown;
    }
    if (start + shown < total) {
      const std::wstring more =
          L"▼ " + std::to_wstring(total - (start + shown)) + L" more below";
      text_out(g_small, m, y, shelf_w, g_h * 0.03f, more.c_str(), g_dim);
    }
    if (g_games.empty()) {
      text_out(g_text, m, y, shelf_w, row_h * 1.6f,
               L"No games yet — scan the QR or open the URL above from any device\n"
               L"to drop a game folder or .zip right onto the console", g_dim);
      y += row_h * 1.6f;
    }

    draw_qr_overlay(m);
    draw_lan_link(m);

    // FPS and system info overlays (when enabled in settings)
    if (longan::trt_settings_show_fps()) draw_fps_overlay();
    if (longan::trt_settings_show_sysinfo()) draw_sysinfo_overlay();

    g_ctx->EndDraw();
    if (bmp) bmp->Release();
    surf->Release();
    g_ctx->SetTarget(old_target);
    if (old_target) old_target->Release();
  }
}

} // namespace longan
