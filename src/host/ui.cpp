// tiramisu/src/host/ui.cpp
// Native Direct2D / DirectWrite implementation of the Tiramisu Xbox Series X|S
// Obsidian & Champagne Glassmorphic Dashboard UI.
#include "ui.h"
#include "cursor_art.h"
#include "boot_anim.h"
#include "cover_art.h"
#include "settings.h"
#include "tiramisu_mascot.h"
#include "ui_sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>

#include "vendor/qrcodegen.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace longan {

namespace {

float g_w = 1920.0f, g_h = 1080.0f;
ID2D1Factory1* g_factory = nullptr;
ID2D1PathGeometry* g_cursor_shape = nullptr;
ID2D1DeviceContext* g_ctx = nullptr;
ID2D1Device* g_d2d_device = nullptr;
IDXGISwapChain1* g_swap = nullptr;
IDWriteFactory* g_dw = nullptr;

// Brushes
ID2D1SolidColorBrush* g_br_white = nullptr;     // #FFFFFF crisp pure white
ID2D1SolidColorBrush* g_br_accent = nullptr;    // #DFCFC9 luminous champagne-pearl
ID2D1SolidColorBrush* g_br_accent2 = nullptr;   // #C9B5AE warm champagne
ID2D1SolidColorBrush* g_br_ink = nullptr;       // #141318 dark pill ink
ID2D1SolidColorBrush* g_br_sub = nullptr;       // #E4E1E6 bright secondary text
ID2D1SolidColorBrush* g_br_dim = nullptr;       // #ACA8B2 cool muted text
ID2D1SolidColorBrush* g_br_glass = nullptr;     // rgba(28,30,38,0.84)
ID2D1SolidColorBrush* g_br_glass2 = nullptr;    // rgba(42,45,56,0.90)
ID2D1SolidColorBrush* g_br_border = nullptr;    // rgba(255,255,255,0.16)
ID2D1SolidColorBrush* g_br_border_hi = nullptr; // rgba(255,255,255,0.38)
ID2D1SolidColorBrush* g_br_pill_bg = nullptr;   // rgba(255,255,255,0.10)
ID2D1SolidColorBrush* g_br_shadow = nullptr;    // rgba(0,0,0,0.62) text shadow
ID2D1SolidColorBrush* g_br_xbox_a = nullptr;    // #35C759
ID2D1SolidColorBrush* g_br_xbox_b = nullptr;    // #E5484D
ID2D1SolidColorBrush* g_br_xbox_x = nullptr;    // #2B8AFF
ID2D1SolidColorBrush* g_br_xbox_y = nullptr;    // #F5B700

// Typography
IDWriteTextFormat* g_f_hero = nullptr;    // 50pt Bold
IDWriteTextFormat* g_f_hero_md = nullptr; // 36pt Bold (auto-fit long hero titles)
IDWriteTextFormat* g_f_stat = nullptr;    // 44pt Bold
IDWriteTextFormat* g_f_h2 = nullptr;      // 24pt SemiBold
IDWriteTextFormat* g_f_nav = nullptr;     // 18pt SemiBold
IDWriteTextFormat* g_f_body = nullptr;    // 16.5pt Normal
IDWriteTextFormat* g_f_small = nullptr;   // 14pt Normal
IDWriteTextFormat* g_f_mono = nullptr;    // 13.5pt Consolas
IDWriteTextFormat* g_f_btn = nullptr;     // 12.5pt Bold centered

// Mascot avatar & dynamic cover / HD hero GPU bitmaps
ID2D1Bitmap* g_mascot_bmp = nullptr;
struct gpu_cover {
  ID2D1Bitmap* bmp = nullptr;
  uint64_t version = 0;
  unsigned w = 0;
  unsigned h = 0;
};
std::unordered_map<std::string, gpu_cover> g_cover_bmps;
std::unordered_map<std::string, gpu_cover> g_hero_bmps;
ID2D1Bitmap* g_boot_bmp = nullptr;
uint64_t g_boot_frame_ver = 0;

// Dashboard state
std::vector<std::wstring> g_lines;
std::vector<std::wstring> g_setup_steps;
int g_setup_done_count = 0;
bool g_setup_finished = false;
bool g_first_run = false;  // land straight on Home once booted!
std::wstring g_transfer_url;
std::wstring g_transfer_status;
std::wstring g_control_hint;
std::vector<ui_game> g_games;
int g_selected = 0;

// QR state
std::string g_qr_text;
uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
int g_qr_size = 0;

// Screen & animations
ui_screen g_screen = UI_SHELF;
bool g_quick_menu = false;
float g_fps = 0.0f;
std::wstring g_sysinfo;
std::vector<ui_setting> g_set_rows;
int g_set_selected = 0;
struct settings_section {
  const wchar_t* title;
  const wchar_t* description;
  std::vector<int> rows;
};
const settings_section k_settings_sections[] = {
    {L"Input", L"Default controls, cursor speed and controller response.", {0, 1, 2, 3, 15}},
    {L"Display", L"Frame pacing, render targets and on-screen indicators.", {12, 13, 18, 19, 9, 10}},
    {L"Audio", L"Audio latency and decoding options for compatible games.", {16, 20}},
    {L"Console", L"Steam, the LAN dashboard and the in-game overlay.", {7, 8, 11}},
    {L"Compatibility", L"Guest OS, memory budget and engine compatibility.", {4, 5, 6, 14, 17}},
};

int selected_settings_section() {
  for (int i = 0; i < static_cast<int>(std::size(k_settings_sections)); ++i) {
    const auto& rows = k_settings_sections[i].rows;
    if (std::find(rows.begin(), rows.end(), g_set_selected) != rows.end()) return i;
  }
  return 0;
}

constexpr const wchar_t* k_setting_help[] = {
    L"Auto exposes connected devices. Controller enables a keyboard gamepad when none is connected.",
    L"Controls movement speed when a controller drives the pointer.",
    L"Controls scroll speed for the controller wheel shortcut.",
    L"Passes game vibration to a connected controller.",
    L"Reports this Windows version to guest apps.",
    L"Reports a Windows 11 build to compatible guest apps.",
    L"Enables write tracking for guest runtimes that request it.",
    L"Enables Steam account linking on the console.",
    L"Shows the transfer address and QR code on the TV.",
    L"Adds an FPS indicator to the on-screen overlay.",
    L"Shows runtime and console information on the TV.",
    L"Enables the Shift+Tab shortcut for the in-game menu.",
    L"Synchronizes presentation with the display refresh where supported.",
    L"Limits the runtime's target frame rate. Off removes the cap.",
    L"Limits the guest heap inside the console's available memory.",
    L"Ignores small stick movements around the controller's neutral position.",
    L"Sets the requested audio buffer duration.",
    L"Reuses compiled shaders in runtime paths that support caching.",
    L"Sets the render scale used by compatible runtime graphics paths.",
    L"Sets the texture preference used by compatible graphics paths.",
    L"Enables the runtime's FSB5 decoding path.",
};

std::vector<std::wstring> g_log_tail;
int g_log_scroll_offset = 0;
float g_anim_time = 0.0f;
float g_nav_pill_pos = 0.0f;  // smoothed tab index for fluid sliding pill
float g_intro_t = 0.0f;       // master dashboard opening choreography clock (sec)
float g_tab_t = 0.0f;         // per-screen reveal clock (resets on tab change)
ULONGLONG g_last_frame_ms = 0;
bool g_boot_was_active = false;
// Smooth Hero banner crossfade when switching selected games
std::string g_hero_cur_key;
std::string g_hero_prev_key;
float g_hero_xfade_t = 1.0f;  // 0.0 -> 1.0 over 0.34s
// Two-step Library Delete confirmation highlight (-1 = idle)
int g_delete_confirm_idx = -1;

// Adaptive input mode (Controller vs KBM)
bool g_controller_active = true;
bool g_pad_connected = true;
int g_ptr_x = -1000, g_ptr_y = -1000;
bool g_cursor_visible = false;

// Toast notification banner
std::wstring g_toast_msg;
float g_toast_ttl = 0.0f;
float g_toast_max = 3.2f;

// Real storage telemetry
double g_storage_used_gb = 0.0;
double g_storage_total_gb = 0.0;

// Live upload progress
uint64_t g_upload_done = 0;
uint64_t g_upload_total = 0;
std::wstring g_upload_label;

// Interactive tools list on Tools screen
std::vector<ui_tool_item> g_tools;
int g_tool_selected = 0;
std::wstring g_tool_result;

// Steam state
int g_steam_selected = 0;
std::vector<ui_steam_item> g_steam_items;
std::vector<ui_steam_item> g_steam_library;
int g_steam_filter = 0;
constexpr const wchar_t* k_steam_filters[] = {L"Library", L"Installed", L"Downloads"};
size_t g_steam_installed = 0;
size_t g_steam_downloading = 0;

void filter_steam_items(unsigned selected_app) {
  g_steam_items.clear();
  for (const auto& item : g_steam_library) {
    if ((g_steam_filter == 1 && !item.installed) ||
        (g_steam_filter == 2 && !item.downloading && !item.queued)) continue;
    g_steam_items.push_back(item);
  }
  const auto selected = std::find_if(g_steam_items.begin(), g_steam_items.end(),
      [selected_app](const auto& item) { return item.app_id == selected_app; });
  if (selected != g_steam_items.end())
    g_steam_selected = static_cast<int>(selected - g_steam_items.begin());
  else
    g_steam_selected = std::clamp(g_steam_selected, 0,
                                std::max(0, static_cast<int>(g_steam_items.size()) - 1));
}

bool g_steam_signed_in = false;
uint64_t g_steam_id64 = 0;
std::wstring g_steam_persona;
std::wstring g_steam_status_line = L"Sign in with Steam QR to browse & download purchases";
std::vector<ui_steam_friend> g_steam_friends;
std::vector<ui_steam_ach> g_steam_achs;
int g_qm_selected = 0;

// Scale helpers anchored to 1440x810 design coordinates (16:9)
inline float sx(float v) { return v * (g_w / 1440.0f); }
inline float sy(float v) { return v * (g_h / 810.0f); }
inline float sc(float v) { return v * std::min(g_w / 1440.0f, g_h / 810.0f); }

inline bool ptr_in(float l, float t, float w, float h) {
  return !g_controller_active && g_ptr_x >= l && g_ptr_y >= t &&
         g_ptr_x < l + w && g_ptr_y < t + h;
}

inline float ease_out_quart(float p) {
  const float inv = 1.0f - std::clamp(p, 0.0f, 1.0f);
  return 1.0f - inv * inv * inv * inv;
}

// Hardware-accelerated Direct2D entrance choreography scope (slide + scale + fade).
// Zero-cost fast path once settled (clock >= delay + dur).
struct anim_scope {
  bool pushed = false;
  bool visible = true;
  D2D1::Matrix3x2F prev_xf = D2D1::Matrix3x2F::Identity();

  anim_scope(float clock, float delay, float dur, float dx, float dy,
             float scale_from = 1.0f, float cx = 0.0f, float cy = 0.0f) {
    if (clock >= delay + dur) return;
    if (clock <= delay) {
      visible = false;
      return;
    }
    const float p =
        std::clamp((clock - delay) / std::max(0.01f, dur), 0.0f, 1.0f);
    const float e = ease_out_quart(p);
    const float alpha = std::clamp(p * 1.45f, 0.0f, 1.0f);
    const float tx = dx * (1.0f - e);
    const float ty = dy * (1.0f - e);
    const float s = scale_from + (1.0f - scale_from) * e;

    g_ctx->GetTransform(&prev_xf);
    D2D1::Matrix3x2F xf = D2D1::Matrix3x2F::Translation(tx, ty);
    if (fabsf(s - 1.0f) > 0.0005f) {
      xf = D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, cy)) * xf;
    }
    g_ctx->SetTransform(xf * prev_xf);

    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
        D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    g_ctx->PushLayer(lp, nullptr);
    pushed = true;
  }

  ~anim_scope() {
    if (pushed) {
      g_ctx->PopLayer();
      g_ctx->SetTransform(prev_xf);
    }
  }
};

void text_out(IDWriteTextFormat* fmt, float x, float y, float w, float h,
              const wchar_t* s, ID2D1Brush* br,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
  if (!fmt || !s || !br || w <= 1.0f || h <= 1.0f) return;
  fmt->SetTextAlignment(align);
  fmt->SetParagraphAlignment(valign);
  D2D1_RECT_F r{x, y, x + w, y + h};
  g_ctx->DrawText(s, (UINT32)wcslen(s), fmt, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Strict single-line text with DirectWrite ellipsis trimming ("...") so text
// NEVER wraps out of its container or clips ugly mid-glyph.
void text_out_line(IDWriteTextFormat* fmt, float x, float y, float w, float h,
                   const wchar_t* s, ID2D1Brush* br,
                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                   bool shadow = false) {
  if (!fmt || !s || !br || !g_dw || w <= 2.0f || h <= 2.0f) return;
  const UINT32 len = (UINT32)wcslen(s);
  if (len == 0) return;
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g_dw->CreateTextLayout(s, len, fmt, w, h, &layout)) || !layout) {
    text_out(fmt, x, y, w, h, s, br, align, valign);
    return;
  }
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  layout->SetTextAlignment(align);
  layout->SetParagraphAlignment(valign);
  IDWriteInlineObject* ell = nullptr;
  if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(fmt, &ell)) && ell) {
    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&tr, ell);
    ell->Release();
  }
  if (shadow && g_br_shadow) {
    g_ctx->DrawTextLayout({x, y + sc(2.0f)}, layout, g_br_shadow,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  g_ctx->DrawTextLayout({x, y}, layout, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  layout->Release();
}

float measure_text_width(IDWriteTextFormat* fmt, const wchar_t* s) {
  if (!fmt || !s || !g_dw || !*s) return 0.0f;
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g_dw->CreateTextLayout(s, (UINT32)wcslen(s), fmt, 4096.0f, 200.0f,
                                    &layout)) ||
      !layout)
    return 0.0f;
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  layout->Release();
  return m.widthIncludingTrailingWhitespace;
}

void fill_rrect(float x, float y, float w, float h, float rad, ID2D1Brush* br) {
  D2D1_ROUNDED_RECT rr{{x, y, x + w, y + h}, rad, rad};
  g_ctx->FillRoundedRectangle(&rr, br);
}

void stroke_rrect(float x, float y, float w, float h, float rad, ID2D1Brush* br,
                  float stroke_w = 1.2f) {
  D2D1_ROUNDED_RECT rr{{x, y, x + w, y + h}, rad, rad};
  g_ctx->DrawRoundedRectangle(&rr, br, stroke_w);
}

void draw_top_specular_rim(float x, float y, float w, float rad, float alpha = 0.24f) {
  const float inset = std::min(rad * 0.85f, w * 0.22f);
  D2D1_GRADIENT_STOP st[3] = {
      {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
      {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha)},
      {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)}};
  ID2D1GradientStopCollection* gsc = nullptr;
  if (SUCCEEDED(g_ctx->CreateGradientStopCollection(st, 3, &gsc))) {
    ID2D1LinearGradientBrush* lgb = nullptr;
    if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({x + inset, y + 1.0f},
                                                {x + w - inset, y + 1.0f}),
            gsc, &lgb))) {
      g_ctx->DrawLine({x + inset, y + sc(1.2f)}, {x + w - inset, y + sc(1.2f)},
                      lgb, sc(1.4f));
      lgb->Release();
    }
    gsc->Release();
  }
}

void draw_scrollbar(float x, float y, float h, int start, int vis, int total) {
  if (total <= vis || vis <= 0 || h <= 20.0f) return;
  const float trk_w = sx(5.0f);
  fill_rrect(x, y, trk_w, h, trk_w * 0.5f, g_br_pill_bg);
  const float thumb_h = std::max(sy(28.0f), h * ((float)vis / (float)total));
  const float max_start = (float)std::max(1, total - vis);
  const float thumb_y = y + (h - thumb_h) * ((float)start / max_start);
  fill_rrect(x, thumb_y, trk_w, thumb_h, trk_w * 0.5f, g_br_accent);
}

void draw_glass_card(float x, float y, float w, float h, float rad = 24.0f,
                     bool focused = false) {
  const float r = sc(rad);
  // Crisp luminous obsidian frosted-glass gradient
  D2D1_GRADIENT_STOP stops[2] = {
      {0.0f, D2D1::ColorF(0.145f, 0.155f, 0.192f, 0.84f)},
      {1.0f, D2D1::ColorF(0.082f, 0.088f, 0.112f, 0.90f)}};
  ID2D1GradientStopCollection* gsc = nullptr;
  if (SUCCEEDED(g_ctx->CreateGradientStopCollection(stops, 2, &gsc))) {
    ID2D1LinearGradientBrush* lgb = nullptr;
    if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({x, y}, {x, y + h}), gsc,
            &lgb))) {
      fill_rrect(x, y, w, h, r, lgb);
      lgb->Release();
    }
    gsc->Release();
  } else {
    fill_rrect(x, y, w, h, r, g_br_glass);
  }
  draw_top_specular_rim(x, y, w, r, focused ? 0.42f : 0.22f);
  stroke_rrect(x, y, w, h, r, focused ? g_br_white : g_br_border,
               focused ? sc(2.4f) : sc(1.2f));
}

void draw_champagne_pill(float x, float y, float w, float h) {
  const float r = h * 0.5f;
  D2D1_GRADIENT_STOP stops[2] = {
      {0.0f, D2D1::ColorF(0.905f, 0.855f, 0.835f, 1.0f)},  // luminous pearl-champagne
      {1.0f, D2D1::ColorF(0.796f, 0.725f, 0.702f, 1.0f)}}; // warm champagne
  ID2D1GradientStopCollection* gsc = nullptr;
  if (SUCCEEDED(g_ctx->CreateGradientStopCollection(stops, 2, &gsc))) {
    ID2D1LinearGradientBrush* lgb = nullptr;
    if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({x, y}, {x, y + h}), gsc,
            &lgb))) {
      fill_rrect(x, y, w, h, r, lgb);
      lgb->Release();
    }
    gsc->Release();
  } else {
    fill_rrect(x, y, w, h, r, g_br_accent);
  }
  draw_top_specular_rim(x, y, w, r, 0.45f);
}

// Crisp Xbox Controller face button badge (ONLY drawn when controller active!)
void draw_xbox_btn(float cx, float cy, float radius, wchar_t letter,
                   ID2D1SolidColorBrush* fill_br, bool dark_text = true) {
  if (!g_controller_active) return;
  D2D1_ELLIPSE el{{cx, cy}, radius, radius};
  g_ctx->FillEllipse(&el, fill_br);
  wchar_t s[2] = {letter, 0};
  text_out(g_f_btn, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, s,
           dark_text ? g_br_ink : g_br_white, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// Bumper pill badge [LB] / [RB] (ONLY drawn when controller active!)
void draw_bumper_pill(float x, float y, float w, float h, const wchar_t* label) {
  if (!g_controller_active) return;
  fill_rrect(x, y, w, h, h * 0.45f, g_br_pill_bg);
  stroke_rrect(x, y, w, h, h * 0.45f, g_br_border_hi, sc(1.0f));
  text_out(g_f_btn, x, y, w, h, label, g_br_sub, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// Fetch or lazily upload a game's cover art bitmap to Direct2D
ID2D1Bitmap* get_cover_bitmap(const std::string& key_utf8) {
  if (key_utf8.empty() || !g_ctx) return nullptr;
  std::vector<uint8_t> bgra;
  unsigned w = 0, h = 0;
  uint64_t ver = 0;
  auto it = g_cover_bmps.find(key_utf8);
  if (!cover_art_get_bgra(key_utf8, nullptr, &w, &h, &ver)) {
    return (it != g_cover_bmps.end()) ? it->second.bmp : nullptr;
  }
  if (it != g_cover_bmps.end() && it->second.bmp && it->second.version == ver) {
    return it->second.bmp;
  }
  if (!cover_art_get_bgra(key_utf8, &bgra, &w, &h, &ver) || w == 0 || h == 0) {
    return nullptr;
  }
  D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  ID2D1Bitmap* bmp = nullptr;
  if (SUCCEEDED(g_ctx->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), w * 4,
                                    props, &bmp))) {
    auto& entry = g_cover_bmps[key_utf8];
    if (entry.bmp) entry.bmp->Release();
    entry.bmp = bmp;
    entry.version = ver;
    entry.w = w;
    entry.h = h;
    return bmp;
  }
  return nullptr;
}

// Fetch or lazily upload a game's HD Hero banner bitmap (1920px library_hero / 1080p art)
ID2D1Bitmap* get_hero_bitmap(const std::string& key_utf8) {
  if (key_utf8.empty() || !g_ctx) return nullptr;
  std::vector<uint8_t> bgra;
  unsigned w = 0, h = 0;
  uint64_t ver = 0;
  auto it = g_hero_bmps.find(key_utf8);
  if (!cover_art_get_hero_bgra(key_utf8, nullptr, &w, &h, &ver)) {
    return (it != g_hero_bmps.end()) ? it->second.bmp : nullptr;
  }
  if (it != g_hero_bmps.end() && it->second.bmp && it->second.version == ver &&
      it->second.w == w && it->second.h == h) {
    return it->second.bmp;
  }
  if (!cover_art_get_hero_bgra(key_utf8, &bgra, &w, &h, &ver) || w == 0 || h == 0) {
    return nullptr;
  }
  D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  ID2D1Bitmap* bmp = nullptr;
  if (SUCCEEDED(g_ctx->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), w * 4,
                                    props, &bmp))) {
    auto& entry = g_hero_bmps[key_utf8];
    if (entry.bmp) entry.bmp->Release();
    entry.bmp = bmp;
    entry.version = ver;
    entry.w = w;
    entry.h = h;
    return bmp;
  }
  return nullptr;
}

// Draw bitmap aspect-filled inside a clipped rounded rectangle using
// High-Quality Cubic interpolation for razor-sharp art at any TV resolution.
void draw_clipped_cover(ID2D1Bitmap* bmp, float x, float y, float w, float h,
                        float rad, float opacity = 1.0f,
                        bool bottom_scrim = false) {
  if (!bmp || !g_factory) return;
  ID2D1RoundedRectangleGeometry* geom = nullptr;
  D2D1_ROUNDED_RECT rr{{x, y, x + w, y + h}, rad, rad};
  if (FAILED(g_factory->CreateRoundedRectangleGeometry(&rr, &geom))) return;

  D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
      D2D1::RectF(x, y, x + w, y + h), geom, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
      D2D1::IdentityMatrix(), opacity, nullptr, D2D1_LAYER_OPTIONS1_NONE);
  g_ctx->PushLayer(lp, nullptr);

  const D2D1_SIZE_F bs = bmp->GetSize();
  const float dst_ar = w / std::max(1.0f, h);
  const float src_ar = bs.width / std::max(1.0f, bs.height);
  D2D1_RECT_F src{0, 0, bs.width, bs.height};
  if (src_ar > dst_ar) {
    float nw = bs.height * dst_ar;
    src.left = (bs.width - nw) * 0.5f;
    src.right = src.left + nw;
  } else {
    float nh = bs.width / dst_ar;
    src.top = (bs.height - nh) * 0.5f;
    src.bottom = src.top + nh;
  }
  D2D1_RECT_F dst{x, y, x + w, y + h};
  g_ctx->DrawBitmap(bmp, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                    &src);

  if (bottom_scrim) {
    // Seamless bottom title scrim clipped strictly inside the card's rounded corners
    D2D1_GRADIENT_STOP bfade[3] = {
        {0.00f, D2D1::ColorF(0.03f, 0.04f, 0.06f, 0.00f)},
        {0.55f, D2D1::ColorF(0.03f, 0.04f, 0.06f, 0.34f)},
        {1.00f, D2D1::ColorF(0.02f, 0.03f, 0.05f, 0.86f)}};
    ID2D1GradientStopCollection* gsc = nullptr;
    if (SUCCEEDED(g_ctx->CreateGradientStopCollection(bfade, 3, &gsc))) {
      ID2D1LinearGradientBrush* lgb = nullptr;
      if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties({x, y + h * 0.42f},
                                                  {x, y + h}),
              gsc, &lgb))) {
        g_ctx->FillRectangle(D2D1::RectF(x, y + h * 0.42f, x + w, y + h), lgb);
        lgb->Release();
      }
      gsc->Release();
    }
  }

  g_ctx->PopLayer();
  geom->Release();
}

// Background is rendered by the 60 FPS D3D11 HLSL PSP XMB silk-wave shader in
// swapchain.cpp — keep the top luminous and clean so the nav bar pops brightly.
void draw_ambient_background() {
  D2D1_GRADIENT_STOP st[2] = {
      {0.0f, D2D1::ColorF(0.03f, 0.035f, 0.05f, 0.16f)},
      {1.0f, D2D1::ColorF(0.03f, 0.035f, 0.05f, 0.0f)}};
  ID2D1GradientStopCollection* gsc = nullptr;
  if (SUCCEEDED(g_ctx->CreateGradientStopCollection(st, 2, &gsc))) {
    ID2D1LinearGradientBrush* lgb = nullptr;
    if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({0, 0}, {0, sy(90.0f)}), gsc,
            &lgb))) {
      g_ctx->FillRectangle(D2D1::RectF(0, 0, g_w, sy(90.0f)), lgb);
      lgb->Release();
    }
    gsc->Release();
  }
}

// Draw crisp vector Wi-Fi arcs
void draw_wifi_icon(float cx, float cy, float size, bool online) {
  ID2D1SolidColorBrush* br = online ? g_br_white : g_br_dim;
  D2D1_ELLIPSE dot{{cx, cy + size * 0.34f}, size * 0.10f, size * 0.10f};
  g_ctx->FillEllipse(&dot, br);
  if (!g_factory) return;
  const float radii[3] = {size * 0.28f, size * 0.50f, size * 0.72f};
  for (int i = 0; i < 3; ++i) {
    float r = radii[i];
    ID2D1PathGeometry* arc = nullptr;
    if (SUCCEEDED(g_factory->CreatePathGeometry(&arc))) {
      ID2D1GeometrySink* sink = nullptr;
      if (SUCCEEDED(arc->Open(&sink))) {
        float dx = r * 0.707f, dy = r * 0.707f;
        sink->BeginFigure({cx - dx, cy + size * 0.34f - dy},
                          D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(
            {cx + dx, cy + size * 0.34f - dy}, {r, r}, 0.0f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
      }
      g_ctx->DrawGeometry(arc, br, sc(2.4f));
      arc->Release();
    }
  }
}

// Draw crisp QR code centered inside (box_x, box_y, box_size, box_size)
void draw_qr_in_box(float box_x, float box_y, float box_size) {
  if (g_qr_size <= 0) return;
  const float cell = std::floor(box_size / (float)(g_qr_size + 6));
  if (cell < 1.0f) return;
  const float q = cell * (float)g_qr_size;
  const float pad = cell * 3.0f;
  const float total = q + 2.0f * pad;
  const float qx = std::floor(box_x + (box_size - total) * 0.5f) + pad;
  const float qy = std::floor(box_y + (box_size - total) * 0.5f) + pad;

  ID2D1SolidColorBrush* wbr = nullptr;
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.97f, 0.96f, 0.96f, 1.0f), &wbr);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.07f, 1.0f), &bbr);
  if (wbr && bbr) {
    D2D1_ROUNDED_RECT bg{{qx - pad, qy - pad, qx + q + pad, qy + q + pad},
                         sc(14.0f), sc(14.0f)};
    g_ctx->FillRoundedRectangle(&bg, wbr);
    for (int yy = 0; yy < g_qr_size; ++yy) {
      for (int xx = 0; xx < g_qr_size; ++xx) {
        if (qrcodegen_getModule(g_qr, xx, yy)) {
          D2D1_RECT_F c{qx + xx * cell, qy + yy * cell,
                        qx + (xx + 1) * cell, qy + (yy + 1) * cell};
          g_ctx->FillRectangle(&c, bbr);
        }
      }
    }
  }
  if (wbr) wbr->Release();
  if (bbr) bbr->Release();
}

// Layout geometry constants (1440x810 design grid matching TopNavigation.tsx & App.tsx)
struct nav_layout {
  float nav_x, nav_y, nav_w, nav_h;
  float tab_w;
  float wifi_cx, wifi_cy;
};

nav_layout get_nav_layout() {
  nav_layout n{};
  const float pad_x = sx(44.0f);
  const float bumper_reserve = g_controller_active ? sx(48.0f) : 0.0f;
  n.nav_x = pad_x + bumper_reserve;
  n.nav_y = sy(26.0f);
  n.nav_w = sx(940.0f) - bumper_reserve * 2.0f;
  n.nav_h = sy(58.0f);
  n.tab_w = n.nav_w / (float)UI_TAB_COUNT;
  n.wifi_cx = g_w - pad_x - sx(92.0f);
  n.wifi_cy = n.nav_y + n.nav_h * 0.5f;
  return n;
}

void draw_top_navigation() {
  const nav_layout nl = get_nav_layout();
  const int target_tab = (g_screen < UI_TAB_COUNT) ? (int)g_screen : 0;
  g_nav_pill_pos += ((float)target_tab - g_nav_pill_pos) * 0.25f;
  if (fabsf((float)target_tab - g_nav_pill_pos) < 0.005f) {
    g_nav_pill_pos = (float)target_tab;
  }

  // Group 1: Top Navigation Capsule drops smoothly from top
  {
    anim_scope nav_a(g_intro_t, 0.00f, 0.48f, 0.0f, -sy(38.0f), 0.985f,
                     nl.nav_x + nl.nav_w * 0.5f, nl.nav_y + nl.nav_h * 0.5f);
    if (nav_a.visible) {
      // Optional [LB] / [RB] shoulder bumper badges ONLY when controller active
      if (g_controller_active) {
        draw_bumper_pill(sx(44.0f), nl.nav_y + sy(12.0f), sx(40.0f), sy(34.0f), L"LB");
        draw_bumper_pill(nl.nav_x + nl.nav_w + sx(8.0f), nl.nav_y + sy(12.0f),
                         sx(40.0f), sy(34.0f), L"RB");
      }

      // Outer capsule shell — bright frosted obsidian-silver glass + top rim
      {
        D2D1_GRADIENT_STOP nst[2] = {
            {0.0f, D2D1::ColorF(0.185f, 0.198f, 0.242f, 0.86f)},
            {1.0f, D2D1::ColorF(0.105f, 0.112f, 0.142f, 0.92f)}};
        ID2D1GradientStopCollection* gsc = nullptr;
        if (SUCCEEDED(g_ctx->CreateGradientStopCollection(nst, 2, &gsc))) {
          ID2D1LinearGradientBrush* lgb = nullptr;
          if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
                  D2D1::LinearGradientBrushProperties(
                      {nl.nav_x, nl.nav_y}, {nl.nav_x, nl.nav_y + nl.nav_h}),
                  gsc, &lgb))) {
            fill_rrect(nl.nav_x, nl.nav_y, nl.nav_w, nl.nav_h, nl.nav_h * 0.5f, lgb);
            lgb->Release();
          }
          gsc->Release();
        } else {
          fill_rrect(nl.nav_x, nl.nav_y, nl.nav_w, nl.nav_h, nl.nav_h * 0.5f, g_br_glass);
        }
      }
      draw_top_specular_rim(nl.nav_x, nl.nav_y, nl.nav_w, nl.nav_h * 0.5f, 0.34f);
      stroke_rrect(nl.nav_x, nl.nav_y, nl.nav_w, nl.nav_h, nl.nav_h * 0.5f,
                   g_br_border_hi, sc(1.2f));

      // Sliding champagne-taupe pill
      const float inset = sc(7.0f);
      const float pill_x =
          nl.nav_x + inset + g_nav_pill_pos * (nl.nav_w - 2.0f * inset) / (float)UI_TAB_COUNT;
      const float pill_w = (nl.nav_w - 2.0f * inset) / (float)UI_TAB_COUNT;
      draw_champagne_pill(pill_x, nl.nav_y + inset, pill_w, nl.nav_h - 2.0f * inset);

      // 5 Primary Tabs matching TopNavigation.tsx
      const wchar_t* names[UI_TAB_COUNT] = {
          L"Home", L"Library", L"Steam", L"Tools", L"Settings"};
      for (int i = 0; i < UI_TAB_COUNT; ++i) {
        const float tx = nl.nav_x + inset + i * pill_w;
        const bool active = (target_tab == i);
        const bool hov = !active && ptr_in(tx, nl.nav_y, pill_w, nl.nav_h);
        if (hov) {
          fill_rrect(tx + sc(2.0f), nl.nav_y + inset, pill_w - sc(4.0f),
                     nl.nav_h - 2.0f * inset, (nl.nav_h - 2.0f * inset) * 0.5f,
                     g_br_pill_bg);
        }
        text_out(g_f_nav, tx, nl.nav_y, pill_w, nl.nav_h, names[i],
                 active ? g_br_ink : (hov ? g_br_white : g_br_sub),
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Thin vertical divider between inactive tabs
        if (i < UI_TAB_COUNT - 1 && !active && target_tab != i + 1) {
          const float dx = tx + pill_w;
          g_ctx->DrawLine({dx, nl.nav_y + sy(18.0f)}, {dx, nl.nav_y + nl.nav_h - sy(18.0f)},
                          g_br_border, sc(1.0f));
        }
      }

      // Glowing champagne under-bar cleanly centered beneath the active tab pill
      const float ub_w = sx(66.0f);
      const float ub_h = sy(4.0f);
      const float ub_x = pill_x + (pill_w - ub_w) * 0.5f;
      fill_rrect(ub_x, nl.nav_y + nl.nav_h + sy(6.0f), ub_w, ub_h, ub_h * 0.5f,
                 g_br_accent);
    }
  }

  // Group 2: Right status cluster drops smoothly with slight parallax stagger
  {
    anim_scope stat_a(g_intro_t, 0.08f, 0.46f, sx(16.0f), -sy(30.0f), 0.96f,
                      nl.wifi_cx, nl.wifi_cy);
    if (stat_a.visible) {
      const float right_sep_x = sx(1065.0f);
      g_ctx->DrawLine({right_sep_x, nl.nav_y + sy(10.0f)},
                      {right_sep_x, nl.nav_y + nl.nav_h - sy(10.0f)},
                      g_br_border_hi, sc(1.2f));

      SYSTEMTIME st{};
      GetLocalTime(&st);
      int hr12 = st.wHour % 12;
      if (hr12 == 0) hr12 = 12;
      const wchar_t* ampm = (st.wHour >= 12) ? L"PM" : L"AM";
      static const wchar_t* kDays[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
      static const wchar_t* kMons[12] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                         L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
      wchar_t tbuf[32], dbuf[32];
      swprintf(tbuf, 32, L"%d:%02d %s", hr12, (int)st.wMinute, ampm);
      swprintf(dbuf, 32, L"%s, %s %d", kDays[st.wDayOfWeek % 7],
               kMons[(st.wMonth >= 1 && st.wMonth <= 12) ? st.wMonth - 1 : 0],
               (int)st.wDay);

      const float clock_w = sx(160.0f);
      const float clock_x = nl.wifi_cx - sx(34.0f) - clock_w;
      text_out(g_f_h2, clock_x, nl.nav_y + sy(2.0f), clock_w, sy(30.0f), tbuf,
               g_br_white, DWRITE_TEXT_ALIGNMENT_TRAILING);
      text_out(g_f_small, clock_x, nl.nav_y + sy(33.0f), clock_w, sy(22.0f), dbuf,
               g_br_dim, DWRITE_TEXT_ALIGNMENT_TRAILING);

      draw_wifi_icon(nl.wifi_cx, nl.wifi_cy, sc(26.0f), !g_transfer_url.empty());

      // Exact Tiramisu Mascot Avatar (avatar_bear.png) in top-right circular ring
      const float av_sz = sc(48.0f);
      const float av_x = g_w - sx(44.0f) - av_sz;
      const float av_y = nl.nav_y + (nl.nav_h - av_sz) * 0.5f;
      fill_rrect(av_x, av_y, av_sz, av_sz, av_sz * 0.5f, g_br_glass2);
      if (g_mascot_bmp) {
        draw_clipped_cover(g_mascot_bmp, av_x + sc(2.0f), av_y + sc(2.0f),
                           av_sz - sc(4.0f), av_sz - sc(4.0f), av_sz * 0.5f);
      }
      stroke_rrect(av_x, av_y, av_sz, av_sz, av_sz * 0.5f, g_br_border_hi, sc(1.3f));
    }
  }
}

// Bottom adaptive controller hint strip (ONLY drawn when controller active!)
void draw_controller_footer(const std::vector<std::pair<wchar_t, const wchar_t*>>& hints) {
  if (!g_controller_active || hints.empty()) return;
  const float bar_h = sy(34.0f);
  const float y = g_h - bar_h - sy(10.0f);
  float x = sx(44.0f);
  for (const auto& h : hints) {
    ID2D1SolidColorBrush* br = g_br_accent;
    bool dark = true;
    if (h.first == L'A') { br = g_br_xbox_a; dark = true; }
    else if (h.first == L'B') { br = g_br_xbox_b; dark = false; }
    else if (h.first == L'X') { br = g_br_xbox_x; dark = false; }
    else if (h.first == L'Y') { br = g_br_xbox_y; dark = true; }
    draw_xbox_btn(x + sc(12.0f), y + bar_h * 0.5f, sc(11.0f), h.first, br, dark);
    text_out(g_f_small, x + sc(28.0f), y, sx(190.0f), bar_h, h.second, g_br_sub,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    x += sx(210.0f);
  }
}

// ============================================================================
// SCREEN 1: HOME (Exact App.tsx + IMG_4524 layout with REAL installed games)
// ============================================================================
void draw_home_screen() {
  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float gap = sx(18.0f);
  const float full_w = g_w - 2.0f * pad_x;

  // Upper 9/12 Hero Spotlight + 3/12 Right Stat Cards
  const float hero_w = full_w * 0.742f;
  const float hero_h = sy(368.0f);
  const float right_x = pad_x + hero_w + gap;
  const float right_w = full_w - hero_w - gap;

  const bool has_game =
      !g_games.empty() && g_selected >= 0 && g_selected < (int)g_games.size();
  const ui_game* cur = has_game ? &g_games[g_selected] : nullptr;

  const float anim_clk = std::min(g_intro_t, g_tab_t);

  // --- HERO SPOTLIGHT CARD (1920px HD Key Art, Bright Vivid Exposure) ---
  const float hero_rad = sc(28.0f);
  {
    anim_scope hero_a(anim_clk, 0.06f, 0.52f, 0.0f, sy(28.0f), 0.965f,
                      pad_x + hero_w * 0.5f, top_y + hero_h * 0.5f);
    if (hero_a.visible) {
      fill_rrect(pad_x, top_y, hero_w, hero_h, hero_rad, g_br_glass);

      if (cur) {
        const float xf = ease_out_quart(std::clamp(g_hero_xfade_t, 0.0f, 1.0f));
        if (xf < 0.999f && !g_hero_prev_key.empty() &&
            g_hero_prev_key != cur->key_utf8) {
          ID2D1Bitmap* prev_bmp = get_hero_bitmap(g_hero_prev_key);
          if (!prev_bmp) prev_bmp = get_cover_bitmap(g_hero_prev_key);
          if (prev_bmp) {
            draw_clipped_cover(prev_bmp, pad_x, top_y, hero_w, hero_h, hero_rad,
                               1.0f);
          }
        }
        ID2D1Bitmap* hero_bmp = get_hero_bitmap(cur->key_utf8);
        if (!hero_bmp) hero_bmp = get_cover_bitmap(cur->key_utf8);
        if (hero_bmp) {
          // Full-brightness 100% HD widescreen artwork with smooth crossfade
          draw_clipped_cover(hero_bmp, pad_x, top_y, hero_w, hero_h, hero_rad,
                             xf);
        }
        // Focused editorial scrim on the left side ONLY so right 60% of artwork
        // shines bright, vibrant, and unmuted while title text stays razor-crisp.
        ID2D1RoundedRectangleGeometry* hgeom = nullptr;
        D2D1_ROUNDED_RECT hrr{{pad_x, top_y, pad_x + hero_w, top_y + hero_h},
                              hero_rad, hero_rad};
        if (g_factory &&
            SUCCEEDED(g_factory->CreateRoundedRectangleGeometry(&hrr, &hgeom))) {
          D2D1_LAYER_PARAMETERS1 hlp = D2D1::LayerParameters1(
              D2D1::RectF(pad_x, top_y, pad_x + hero_w, top_y + hero_h), hgeom,
              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          g_ctx->PushLayer(hlp, nullptr);

          D2D1_GRADIENT_STOP scrim[4] = {
              {0.00f, D2D1::ColorF(0.055f, 0.057f, 0.062f, 0.76f)},
              {0.26f, D2D1::ColorF(0.055f, 0.057f, 0.062f, 0.44f)},
              {0.50f, D2D1::ColorF(0.055f, 0.057f, 0.062f, 0.08f)},
              {0.72f, D2D1::ColorF(0.055f, 0.057f, 0.062f, 0.00f)}};
          ID2D1GradientStopCollection* gsc = nullptr;
          if (SUCCEEDED(g_ctx->CreateGradientStopCollection(scrim, 4, &gsc))) {
            ID2D1LinearGradientBrush* lgb = nullptr;
            if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        {pad_x, top_y}, {pad_x + hero_w, top_y}),
                    gsc, &lgb))) {
              g_ctx->FillRectangle(
                  D2D1::RectF(pad_x, top_y, pad_x + hero_w, top_y + hero_h), lgb);
              lgb->Release();
            }
            gsc->Release();
          }
          // Subtle neutral matte-black bottom anchor strip behind the action buttons
          D2D1_GRADIENT_STOP bscrim[2] = {
              {0.0f, D2D1::ColorF(0.052f, 0.054f, 0.058f, 0.00f)},
              {1.0f, D2D1::ColorF(0.052f, 0.054f, 0.058f, 0.42f)}};
          ID2D1GradientStopCollection* bgsc = nullptr;
          if (SUCCEEDED(g_ctx->CreateGradientStopCollection(bscrim, 2, &bgsc))) {
            ID2D1LinearGradientBrush* blgb = nullptr;
            if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        {pad_x, top_y + hero_h * 0.70f}, {pad_x, top_y + hero_h}),
                    bgsc, &blgb))) {
              g_ctx->FillRectangle(
                  D2D1::RectF(pad_x, top_y + hero_h * 0.70f, pad_x + hero_w,
                              top_y + hero_h),
                  blgb);
              blgb->Release();
            }
            bgsc->Release();
          }
          g_ctx->PopLayer();
          hgeom->Release();
        }
      }
      draw_top_specular_rim(pad_x, top_y, hero_w, hero_rad, 0.36f);
      stroke_rrect(pad_x, top_y, hero_w, hero_h, hero_rad, g_br_border_hi, sc(1.3f));

      // Layered parallax foreground elements cascade cleanly inside the Hero card
      const float hp_x = pad_x + sx(32.0f);
      const float hp_y = top_y + sy(26.0f);
      {
        anim_scope fg_a(anim_clk, 0.16f, 0.44f, -sx(14.0f), sy(14.0f), 1.0f);
        if (fg_a.visible) {
          std::wstring pill_txt = L"▶  Ready";
          if (cur && !cur->engine_type.empty()) pill_txt = L"▶  " + cur->engine_type;
          const float pill_tw = measure_text_width(g_f_small, pill_txt.c_str());
          const float hp_w = std::clamp(pill_tw + sx(36.0f), sx(124.0f), hero_w * 0.44f);
          fill_rrect(hp_x, hp_y, hp_w, sy(36.0f), sy(18.0f), g_br_glass2);
          stroke_rrect(hp_x, hp_y, hp_w, sy(36.0f), sy(18.0f), g_br_border_hi, sc(1.1f));
          text_out_line(g_f_small, hp_x + sx(16.0f), hp_y, hp_w - sx(32.0f), sy(36.0f),
                        pill_txt.c_str(), g_br_white, DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

          const float title_max_w = hero_w * 0.58f;
          if (cur) {
            const float hero_tw = measure_text_width(g_f_hero, cur->name.c_str());
            IDWriteTextFormat* title_fmt =
                (hero_tw > title_max_w && g_f_hero_md) ? g_f_hero_md : g_f_hero;
            text_out_line(title_fmt, hp_x, top_y + sy(108.0f), title_max_w, sy(74.0f),
                          cur->name.c_str(), g_br_white, DWRITE_TEXT_ALIGNMENT_LEADING,
                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
            std::wstring sub = cur->exe;
            if (!cur->meta.empty()) sub += L"  ·  Input: " + cur->meta;
            text_out_line(g_f_body, hp_x, top_y + sy(192.0f), title_max_w, sy(30.0f),
                          sub.c_str(), g_br_sub, DWRITE_TEXT_ALIGNMENT_LEADING,
                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
          } else {
            text_out_line(g_f_hero, hp_x, top_y + sy(108.0f), hero_w * 0.72f, sy(74.0f),
                          L"Tiramisu", g_br_white, DWRITE_TEXT_ALIGNMENT_LEADING,
                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
            text_out(g_f_body, hp_x, top_y + sy(194.0f), hero_w * 0.72f, sy(50.0f),
                     L"No games installed yet — Open Tools to transfer via QR / LAN, or open Steam.",
                     g_br_sub);
          }
        }
      }

      // Hero Bottom-Left Action Row: Play Pill + Circular Exe + Circular Tools
      {
        const float act_y = top_y + hero_h - sy(78.0f);
        const float play_w = sx(184.0f);
        const float play_h = sy(50.0f);
        anim_scope act_a(anim_clk, 0.22f, 0.42f, 0.0f, sy(18.0f), 0.96f,
                         hp_x + play_w * 0.5f, act_y + play_h * 0.5f);
        if (act_a.visible) {
          draw_champagne_pill(hp_x, act_y, play_w, play_h);
          if (g_controller_active) {
            draw_xbox_btn(hp_x + sx(28.0f), act_y + play_h * 0.5f, sc(12.0f), L'A',
                          g_br_xbox_a, true);
            text_out(g_f_nav, hp_x + sx(46.0f), act_y, play_w - sx(54.0f), play_h,
                     has_game ? L"▶  Play" : L"Transfer", g_br_ink,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
          } else {
            text_out(g_f_nav, hp_x, act_y, play_w, play_h,
                     has_game ? L"▶  Play" : L"Open Transfer", g_br_ink,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
          }

          const float cbtn_sz = play_h;
          const float c1_x = hp_x + play_w + sx(16.0f);
          fill_rrect(c1_x, act_y, cbtn_sz, cbtn_sz, cbtn_sz * 0.5f, g_br_glass2);
          stroke_rrect(c1_x, act_y, cbtn_sz, cbtn_sz, cbtn_sz * 0.5f, g_br_border_hi, sc(1.1f));
          text_out(g_f_h2, c1_x, act_y, cbtn_sz, cbtn_sz, L"≡", g_br_white,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
          if (g_controller_active) {
            draw_xbox_btn(c1_x + cbtn_sz - sc(4.0f), act_y + sc(4.0f), sc(10.0f), L'X',
                          g_br_xbox_x, false);
          }

          const float c2_x = c1_x + cbtn_sz + sx(14.0f);
          fill_rrect(c2_x, act_y, cbtn_sz, cbtn_sz, cbtn_sz * 0.5f, g_br_glass2);
          stroke_rrect(c2_x, act_y, cbtn_sz, cbtn_sz, cbtn_sz * 0.5f, g_br_border_hi, sc(1.1f));
          text_out(g_f_h2, c2_x, act_y, cbtn_sz, cbtn_sz, L"⚙", g_br_white,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
          if (g_controller_active) {
            draw_xbox_btn(c2_x + cbtn_sz - sc(4.0f), act_y + sc(4.0f), sc(10.0f), L'Y',
                          g_br_xbox_y, true);
          }
        }
      }

      // Hero Bottom-Right Carousel Dots (real game count, max 6 dots)
      const int dot_n = std::max(1, std::min(6, (int)g_games.size()));
      float dot_x = pad_x + hero_w - sx(30.0f);
      const float dot_y = top_y + hero_h - sy(36.0f);
      for (int i = dot_n - 1; i >= 0; --i) {
        const bool active_dot = (i == (g_selected % dot_n));
        const float dw = active_dot ? sx(28.0f) : sx(9.0f);
        const float dh = sy(8.0f);
        dot_x -= dw;
        fill_rrect(dot_x, dot_y, dw, dh, dh * 0.5f,
                   active_dot ? g_br_white : g_br_border_hi);
        dot_x -= sx(8.0f);
      }
    }
  }

  // --- RIGHT STACKED GLASS STAT CARDS (Glide smoothly in from right) ---
  const float card_h = (hero_h - gap) * 0.5f;

  // Top Card: Library (REAL installed game count + animated growing pill bars)
  {
    anim_scope lib_a(anim_clk, 0.14f, 0.48f, sx(44.0f), sy(10.0f), 0.96f,
                     right_x + right_w * 0.5f, top_y + card_h * 0.5f);
    if (lib_a.visible) {
      draw_glass_card(right_x, top_y, right_w, card_h, 24.0f, false);
      text_out(g_f_nav, right_x + sx(22.0f), top_y + sy(18.0f), right_w - sx(60.0f),
               sy(28.0f), L"◫  Library", g_br_white);
      text_out(g_f_nav, right_x + right_w - sx(34.0f), top_y + sy(18.0f), sx(20.0f),
               sy(28.0f), L"›", g_br_dim, DWRITE_TEXT_ALIGNMENT_TRAILING);

      wchar_t cnt_buf[32];
      swprintf(cnt_buf, 32, L"%zu", g_games.size());
      text_out(g_f_stat, right_x + sx(22.0f), top_y + sy(66.0f), sx(120.0f),
               sy(56.0f), cnt_buf, g_br_white);
      text_out(g_f_small, right_x + sx(22.0f), top_y + sy(124.0f), sx(120.0f),
               sy(24.0f), g_games.size() == 1 ? L"Game" : L"Games", g_br_dim);

      static const float kBars[7] = {0.24f, 0.34f, 0.48f, 0.56f, 0.86f, 0.62f, 0.68f};
      const float bars_max_h = sy(74.0f);
      const float bars_base_y = top_y + card_h - sy(22.0f);
      const float bar_w = sx(8.0f);
      const float bar_gap = sx(9.0f);
      const float bars_start_x =
          right_x + right_w - sx(24.0f) - (7 * bar_w + 6 * bar_gap);
      for (int i = 0; i < 7; ++i) {
        const float bp = ease_out_quart(
            std::clamp((anim_clk - (0.24f + i * 0.035f)) / 0.36f, 0.0f, 1.0f));
        const float bh = std::max(bar_w, bars_max_h * kBars[i] * bp);
        const float bx = bars_start_x + i * (bar_w + bar_gap);
        const bool hi = (i >= 2 && i <= 4);
        fill_rrect(bx, bars_base_y - bh, bar_w, bh, bar_w * 0.5f,
                   hi ? g_br_white : g_br_border_hi);
      }
    }
  }

  // Bottom Card: Storage (REAL Xbox LocalState disk usage + animated sweeping bar)
  const float st_y = top_y + card_h + gap;
  {
    anim_scope sto_a(anim_clk, 0.22f, 0.48f, sx(50.0f), sy(16.0f), 0.96f,
                     right_x + right_w * 0.5f, st_y + card_h * 0.5f);
    if (sto_a.visible) {
      draw_glass_card(right_x, st_y, right_w, card_h, 24.0f, false);
      text_out(g_f_nav, right_x + sx(22.0f), st_y + sy(18.0f), right_w - sx(60.0f),
               sy(28.0f), L"▤  Storage", g_br_white);
      text_out(g_f_nav, right_x + right_w - sx(34.0f), st_y + sy(18.0f), sx(20.0f),
               sy(28.0f), L"›", g_br_dim, DWRITE_TEXT_ALIGNMENT_TRAILING);

      wchar_t st_buf[64];
      float pct = 0.0f;
      if (g_storage_total_gb > 0.1) {
        pct = (float)std::clamp(g_storage_used_gb / g_storage_total_gb, 0.0, 1.0);
        if (g_storage_total_gb >= 900.0) {
          swprintf(st_buf, 64, L"%.1f GB / %.1f TB", g_storage_used_gb,
                   g_storage_total_gb / 1000.0);
        } else {
          swprintf(st_buf, 64, L"%.1f GB / %.0f GB", g_storage_used_gb,
                   g_storage_total_gb);
        }
      } else {
        swprintf(st_buf, 64, L"Storage usage unavailable");
      }
      text_out(g_f_nav, right_x + sx(22.0f), st_y + sy(74.0f), right_w - sx(44.0f),
               sy(28.0f), st_buf, g_br_white);

      const float trk_x = right_x + sx(22.0f);
      const float trk_y = st_y + card_h - sy(48.0f);
      const float trk_w = right_w - sx(44.0f);
      const float trk_h = sy(22.0f);
      const float fill_p =
          ease_out_quart(std::clamp((anim_clk - 0.34f) / 0.50f, 0.0f, 1.0f));
      fill_rrect(trk_x, trk_y, trk_w, trk_h, trk_h * 0.5f, g_br_pill_bg);
      if (pct > 0.0f && fill_p > 0.0f) {
        const float width = (trk_w - sc(4.0f)) * pct * fill_p;
        fill_rrect(trk_x + sc(2.0f), trk_y + sc(2.0f), width,
                   trk_h - sc(4.0f), std::min(width, trk_h - sc(4.0f)) * 0.5f, g_br_white);
      }
    }
  }

  // --- BOTTOM ROW: CONTINUE PLAYING (Header slides in, cards cascade up!) ---
  const float row_hdr_y = top_y + hero_h + sy(22.0f);
  {
    anim_scope hdr_a(anim_clk, 0.25f, 0.40f, -sx(20.0f), sy(8.0f));
    if (hdr_a.visible) {
      text_out(g_f_h2, pad_x + sx(4.0f), row_hdr_y, sx(320.0f), sy(32.0f),
               L"🔖  Continue Playing", g_br_white);

      if (!g_transfer_url.empty()) {
        std::wstring lan_hint = L"LAN Transfer: " + g_transfer_url;
        if (g_upload_total > 0 && g_upload_done < g_upload_total) {
          int upct = (int)((double)g_upload_done * 100.0 / (double)g_upload_total);
          wchar_t ub[128];
          swprintf(ub, 128, L"Uploading %d%%  ·  %s", upct, g_upload_label.c_str());
          lan_hint = ub;
        }
        text_out(g_f_small, pad_x + full_w - sx(460.0f), row_hdr_y + sy(5.0f),
                 sx(456.0f), sy(26.0f), lan_hint.c_str(), g_br_accent,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
      }
    }
  }

  const float cards_y = row_hdr_y + sy(42.0f);
  // 4 wide-cinema cards (~1.93:1) so Steam cover art & logos fit edge-to-edge
  // without slicing off the left/right letters of game titles!
  const int max_cards = 4;
  const float card_gap = sx(18.0f);
  const float card_w = (full_w - (float)(max_cards - 1) * card_gap) / (float)max_cards;
  const float card_h2 = sy(168.0f);

  if (g_games.empty()) {
    anim_scope ec_a(anim_clk, 0.28f, 0.45f, 0.0f, sy(36.0f), 0.96f,
                    pad_x + full_w * 0.5f, cards_y + card_h2 * 0.5f);
    if (ec_a.visible) {
      draw_glass_card(pad_x, cards_y, full_w, card_h2, 22.0f, false);
      text_out_line(g_f_h2, pad_x + sx(24.0f), cards_y + sy(44.0f), full_w - sx(48.0f),
                    sy(36.0f), L"Your installed Xbox games will appear here", g_br_white,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
      text_out_line(g_f_body, pad_x + sx(24.0f), cards_y + sy(88.0f), full_w - sx(48.0f),
                    sy(32.0f),
                    L"Open Tools to scan the QR code and drop a game from your phone/PC, or open Steam to download.",
                    g_br_dim, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
  } else {
    const int total = (int)g_games.size();
    int start = 0;
    if (total > max_cards) {
      start = std::clamp(g_selected - 1, 0, total - max_cards);
    }
    for (int slot = 0; slot < max_cards && (start + slot) < total; ++slot) {
      const int idx = start + slot;
      const auto& gm = g_games[idx];
      const bool sel = (idx == g_selected);
      const bool hov = !sel && ptr_in(pad_x + slot * (card_w + card_gap),
                                      cards_y, card_w, card_h2);
      const float cx = pad_x + slot * (card_w + card_gap);
      const float cy = cards_y - (sel ? sy(5.0f) : (hov ? sy(2.0f) : 0.0f));
      const float rad = sc(20.0f);

      // Sequential left-to-right rising cascade for each game icon/card!
      anim_scope card_a(anim_clk, 0.28f + slot * 0.09f, 0.46f, 0.0f, sy(46.0f),
                        0.91f, cx + card_w * 0.5f, cy + card_h2 * 0.5f);
      if (!card_a.visible) continue;

      if (sel) {
        ID2D1SolidColorBrush* glow = nullptr;
        g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.87f, 0.86f, 0.22f), &glow);
        if (glow) {
          stroke_rrect(cx - sc(3.0f), cy - sc(3.0f), card_w + sc(6.0f),
                       card_h2 + sc(6.0f), rad + sc(3.0f), glow, sc(4.5f));
          glow->Release();
        }
      }

      draw_glass_card(cx, cy, card_w, card_h2, 20.0f, sel);
      ID2D1Bitmap* cover = get_cover_bitmap(gm.key_utf8);
      if (cover) {
        draw_clipped_cover(cover, cx, cy, card_w, card_h2, rad, 1.0f, true);
      } else {
        std::wstring mono_ch = gm.name.empty() ? L"T" : gm.name.substr(0, 1);
        text_out_line(g_f_stat, cx, cy + sy(26.0f), card_w, sy(66.0f),
                      mono_ch.c_str(), g_br_accent, DWRITE_TEXT_ALIGNMENT_CENTER);
      }

      text_out_line(g_f_nav, cx + sx(16.0f), cy + card_h2 - sy(40.0f),
                    card_w - sx(32.0f), sy(28.0f), gm.name.c_str(), g_br_white,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
      draw_top_specular_rim(cx, cy, card_w, rad, sel ? 0.45f : 0.22f);
      stroke_rrect(cx, cy, card_w, card_h2, rad,
                   sel ? g_br_white : (hov ? g_br_border_hi : g_br_border),
                   sel ? sc(2.6f) : sc(1.2f));
    }
  }

  draw_controller_footer({{L'A', L"Launch Game"},
                          {L'X', L"Switch Exe"},
                          {L'Y', L"Transfer / QR"}});
}

// ============================================================================
// SCREEN 2: LIBRARY (2-Pane Inspector + 4-Column Cover Art Grid)
// ============================================================================
void draw_library_screen() {
  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float full_w = g_w - 2.0f * pad_x;
  const float full_h = g_h - top_y - sy(54.0f);
  const float gap = sx(18.0f);

  const float left_w = full_w * 0.25f;
  const float right_x = pad_x + left_w + gap;
  const float right_w = full_w - left_w - gap;

  // Left Rail: Real Library Summary & Drives
  draw_glass_card(pad_x, top_y, left_w, full_h, 24.0f);
  text_out(g_f_h2, pad_x + sx(22.0f), top_y + sy(22.0f), left_w - sx(44.0f),
           sy(34.0f), L"Game Library", g_br_white);
  wchar_t sub[64];
  swprintf(sub, 64, L"%zu Installed on Xbox", g_games.size());
  text_out(g_f_small, pad_x + sx(22.0f), top_y + sy(58.0f), left_w - sx(44.0f),
           sy(24.0f), sub, g_br_dim);

  // Active pill "All Titles"
  const float pill_y = top_y + sy(102.0f);
  draw_champagne_pill(pad_x + sx(18.0f), pill_y, left_w - sx(36.0f), sy(40.0f));
  text_out(g_f_body, pad_x + sx(34.0f), pill_y, left_w - sx(68.0f), sy(40.0f),
           L"All Installed Titles", g_br_ink, DWRITE_TEXT_ALIGNMENT_LEADING,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  text_out(g_f_small, pad_x + sx(24.0f), pill_y + sy(60.0f), left_w - sx(48.0f),
           sy(26.0f), L"Add Games", g_br_accent);
  constexpr const wchar_t* add_actions[] = {L"Transfer over LAN", L"Open Steam library"};
  for (int i = 0; i < 2; ++i) {
    const float x = pad_x + sx(18.0f);
    const float y = top_y + sy(190.0f + i * 48.0f);
    const float w = left_w - sx(36.0f);
    const bool hover = ptr_in(x, y, w, sy(40.0f));
    fill_rrect(x, y, w, sy(40.0f), sy(20.0f), hover ? g_br_glass2 : g_br_pill_bg);
    stroke_rrect(x, y, w, sy(40.0f), sy(20.0f),
                 hover ? g_br_border_hi : g_br_border, sc(1.1f));
    text_out_line(g_f_small, x + sx(14.0f), y, w - sx(28.0f), sy(40.0f),
                  add_actions[i], g_br_white, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  text_out(g_f_small, pad_x + sx(24.0f), top_y + sy(292.0f), left_w - sx(48.0f),
           sy(64.0f), L"USB games appear here when Xbox can read the drive's game folder.", g_br_dim);

  // Right Top: Selected Game Inspector Banner
  const float insp_h = sy(196.0f);
  draw_glass_card(right_x, top_y, right_w, insp_h, 24.0f);
  const bool has_game =
      !g_games.empty() && g_selected >= 0 && g_selected < (int)g_games.size();
  if (has_game) {
    const auto& gm = g_games[g_selected];
    const float xf = ease_out_quart(std::clamp(g_hero_xfade_t, 0.0f, 1.0f));
    if (xf < 0.999f && !g_hero_prev_key.empty() &&
        g_hero_prev_key != gm.key_utf8) {
      ID2D1Bitmap* prev_bmp = get_hero_bitmap(g_hero_prev_key);
      if (!prev_bmp) prev_bmp = get_cover_bitmap(g_hero_prev_key);
      if (prev_bmp) {
        draw_clipped_cover(prev_bmp, right_x, top_y, right_w, insp_h,
                           sc(24.0f), 1.0f);
      }
    }
    ID2D1Bitmap* hero_bmp = get_hero_bitmap(gm.key_utf8);
    if (!hero_bmp) hero_bmp = get_cover_bitmap(gm.key_utf8);
    if (hero_bmp) {
      // Bright HD hero banner art across the Library inspector with smooth crossfade
      draw_clipped_cover(hero_bmp, right_x, top_y, right_w, insp_h, sc(24.0f),
                         xf);
    }
    ID2D1RoundedRectangleGeometry* igeom = nullptr;
    D2D1_ROUNDED_RECT irr{{right_x, top_y, right_x + right_w, top_y + insp_h},
                          sc(24.0f), sc(24.0f)};
    if (g_factory &&
        SUCCEEDED(g_factory->CreateRoundedRectangleGeometry(&irr, &igeom))) {
      D2D1_LAYER_PARAMETERS1 ilp = D2D1::LayerParameters1(
          D2D1::RectF(right_x, top_y, right_x + right_w, top_y + insp_h), igeom,
          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      g_ctx->PushLayer(ilp, nullptr);
      D2D1_GRADIENT_STOP scrm[3] = {
          {0.00f, D2D1::ColorF(0.035f, 0.042f, 0.060f, 0.80f)},
          {0.42f, D2D1::ColorF(0.035f, 0.042f, 0.060f, 0.42f)},
          {0.78f, D2D1::ColorF(0.035f, 0.042f, 0.060f, 0.00f)}};
      ID2D1GradientStopCollection* gsc = nullptr;
      if (SUCCEEDED(g_ctx->CreateGradientStopCollection(scrm, 3, &gsc))) {
        ID2D1LinearGradientBrush* lgb = nullptr;
        if (SUCCEEDED(g_ctx->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({right_x, top_y},
                                                    {right_x + right_w, top_y}),
                gsc, &lgb))) {
          g_ctx->FillRectangle(
              D2D1::RectF(right_x, top_y, right_x + right_w, top_y + insp_h), lgb);
          lgb->Release();
        }
        gsc->Release();
      }
      g_ctx->PopLayer();
      igeom->Release();
    }
    draw_top_specular_rim(right_x, top_y, right_w, sc(24.0f), 0.34f);
    stroke_rrect(right_x, top_y, right_w, insp_h, sc(24.0f), g_br_border_hi, sc(1.2f));

    text_out_line(g_f_small, right_x + sx(26.0f), top_y + sy(20.0f), right_w * 0.56f,
                  sy(22.0f),
                  gm.engine_type.empty() ? L"XBOX DEV MODE READY"
                                         : gm.engine_type.c_str(),
                  g_br_accent, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    text_out_line(g_f_h2, right_x + sx(26.0f), top_y + sy(46.0f), right_w * 0.58f,
                  sy(36.0f), gm.name.c_str(), g_br_white,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    text_out_line(g_f_mono, right_x + sx(26.0f), top_y + sy(88.0f), right_w * 0.58f,
                  sy(24.0f), gm.exe.c_str(), g_br_sub,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

    const float by = top_y + insp_h - sy(58.0f);
    draw_champagne_pill(right_x + sx(26.0f), by, sx(152.0f), sy(40.0f));
    if (g_controller_active) {
      draw_xbox_btn(right_x + sx(50.0f), by + sy(20.0f), sc(10.0f), L'A',
                    g_br_xbox_a, true);
    }
    text_out(g_f_body, right_x + sx(g_controller_active ? 64.0f : 26.0f), by,
             sx(g_controller_active ? 104.0f : 152.0f), sy(40.0f), L"▶  Play",
             g_br_ink, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    fill_rrect(right_x + sx(190.0f), by, sx(168.0f), sy(40.0f), sy(20.0f),
               g_br_glass2);
    stroke_rrect(right_x + sx(190.0f), by, sx(168.0f), sy(40.0f), sy(20.0f),
                 ptr_in(right_x + sx(190.0f), by, sx(168.0f), sy(40.0f))
                     ? g_br_white : g_br_border_hi, sc(1.0f));
    if (g_controller_active) {
      draw_xbox_btn(right_x + sx(212.0f), by + sy(20.0f), sc(10.0f), L'X',
                    g_br_xbox_x, false);
    }
    text_out(g_f_small, right_x + sx(g_controller_active ? 226.0f : 190.0f), by,
             sx(g_controller_active ? 124.0f : 168.0f), sy(40.0f),
             L"Switch Exe", g_br_white, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Per-game Input Mode cycle button (auto / controller / kbm / pad2kbm)
    const float inp_x = right_x + sx(370.0f);
    const float inp_w = sx(184.0f);
    fill_rrect(inp_x, by, inp_w, sy(40.0f), sy(20.0f), g_br_glass2);
    stroke_rrect(inp_x, by, inp_w, sy(40.0f), sy(20.0f),
                 ptr_in(inp_x, by, inp_w, sy(40.0f)) ? g_br_white : g_br_border_hi,
                 sc(1.0f));
    std::wstring inp_lbl = L"Input: " + (gm.meta.empty() ? std::wstring(L"auto") : gm.meta);
    text_out_line(g_f_small, inp_x + sx(10.0f), by, inp_w - sx(20.0f), sy(40.0f),
                  inp_lbl.c_str(), g_br_accent, DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Two-step safe Delete / Uninstall Game pill button
    const float del_x = right_x + sx(566.0f);
    const float del_w = sx(168.0f);
    const bool armed = (g_delete_confirm_idx == g_selected);
    const bool dhov = ptr_in(del_x, by, del_w, sy(40.0f));
    fill_rrect(del_x, by, del_w, sy(40.0f), sy(20.0f),
               armed ? g_br_xbox_b : g_br_glass2);
    stroke_rrect(del_x, by, del_w, sy(40.0f), sy(20.0f),
                 (armed || dhov) ? g_br_white : g_br_xbox_b, sc(1.2f));
    text_out_line(g_f_small, del_x + sx(10.0f), by, del_w - sx(20.0f), sy(40.0f),
                  armed ? L"Confirm Delete?" : L"Uninstall",
                  armed ? g_br_white : g_br_xbox_b,
                  DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  // Right Bottom: 4-Column Cover Art Grid
  const float grid_y = top_y + insp_h + gap;
  const float grid_h = full_h - insp_h - gap;
  draw_glass_card(right_x, grid_y, right_w, grid_h, 24.0f);

  const int cols = 4;
  const float gpad = sx(18.0f);
  const float cgap = sx(16.0f);
  const float cw = (right_w - 2.0f * gpad - (cols - 1) * cgap) / (float)cols;
  const float ch = cw / 1.92f;
  const int vis_rows = std::max(1, (int)((grid_h - 2.0f * gpad + cgap) / (ch + cgap)));
  const int sel_row = g_selected / cols;
  const int start_row = std::max(0, sel_row - vis_rows + 1);

  for (int i = start_row * cols; i < (int)g_games.size(); ++i) {
    const int r = (i / cols) - start_row;
    const int c = i % cols;
    if (r >= vis_rows) break;
    const float cx = right_x + gpad + c * (cw + cgap);
    const float cy = grid_y + gpad + r * (ch + cgap);
    const bool sel = (i == g_selected);
    const bool hov = !sel && ptr_in(cx, cy, cw, ch);
    const float rad = sc(16.0f);

    const int vis_idx = r * cols + c;
    anim_scope lc_a(g_tab_t, 0.06f + vis_idx * 0.055f, 0.38f, 0.0f, sy(28.0f),
                    0.93f, cx + cw * 0.5f, cy + ch * 0.5f);
    if (!lc_a.visible) continue;

    draw_glass_card(cx, cy, cw, ch, 16.0f, sel);
    ID2D1Bitmap* cover = get_cover_bitmap(g_games[i].key_utf8);
    if (cover) draw_clipped_cover(cover, cx, cy, cw, ch, rad, 1.0f, true);
    text_out_line(g_f_small, cx + sx(12.0f), cy + ch - sy(30.0f), cw - sx(24.0f),
                  sy(24.0f), g_games[i].name.c_str(), g_br_white,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    draw_top_specular_rim(cx, cy, cw, rad, sel ? 0.40f : 0.20f);
    stroke_rrect(cx, cy, cw, ch, rad,
                 sel ? g_br_white : (hov ? g_br_border_hi : g_br_border),
                 sel ? sc(2.4f) : sc(1.1f));
  }

  draw_controller_footer({{L'A', L"Play Selected"},
                          {L'X', L"Switch Exe"},
                          {L'Y', L"Rescan Library"},
                          {L'B', L"Back to Home"}});
}

// ============================================================================
// SCREEN 3: STEAM (Obsidian Glass Steam Account & Native Depot Downloader)
// ============================================================================
void draw_steam_screen() {
  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float full_w = g_w - 2.0f * pad_x;
  const float full_h = g_h - top_y - sy(54.0f);
  const float gap = sx(18.0f);

  const float left_w = full_w * 0.54f;
  const float right_x = pad_x + left_w + gap;
  const float right_w = full_w - left_w - gap;

  // Left Card: Owned & Local Steam Titles
  draw_glass_card(pad_x, top_y, left_w, full_h, 24.0f);
  text_out(g_f_h2, pad_x + sx(24.0f), top_y + sy(20.0f), left_w - sx(48.0f),
           sy(32.0f),
           g_steam_signed_in ? L"Your Steam Library" : L"Steam Library",
           g_br_white);

  const float filter_x = pad_x + sx(16.0f);
  const float filter_y = top_y + sy(62.0f);
  const float filter_w = (left_w - sx(48.0f)) / 3;
  const size_t counts[] = {g_steam_library.size(), g_steam_installed, g_steam_downloading};
  for (int i = 0; i < 3; ++i) {
    const float x = filter_x + i * (filter_w + sx(8.0f));
    const auto title = std::wstring(k_steam_filters[i]) + L"  " + std::to_wstring(counts[i]);
    if (i == g_steam_filter) {
      draw_champagne_pill(x, filter_y, filter_w, sy(36.0f));
    } else {
      fill_rrect(x, filter_y, filter_w, sy(36.0f), sy(18.0f), g_br_pill_bg);
      if (ptr_in(x, filter_y, filter_w, sy(36.0f)))
        stroke_rrect(x, filter_y, filter_w, sy(36.0f), sy(18.0f), g_br_border_hi, sc(1.4f));
    }
    text_out_line(g_f_small, x + sx(6.0f), filter_y, filter_w - sx(12.0f),
                  sy(36.0f), title.c_str(), i == g_steam_filter ? g_br_ink : g_br_sub,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  const float list_y0 = top_y + sy(112.0f);
  const float row_h = sy(68.0f);
  const int max_vis = std::max(1, (int)((full_h - sy(128.0f)) / row_h));
  if (g_steam_items.empty()) {
    text_out(g_f_body, pad_x + sx(26.0f), list_y0 + sy(24.0f),
             left_w - sx(52.0f), sy(120.0f),
             g_steam_filter == 2 ? L"No downloads or queued installs."
             : g_steam_filter == 1 ? L"No installed Steam games."
             : g_steam_signed_in ? L"No titles found. Refresh to reload your Steam library."
             : L"Sign in with Steam QR to view your library and install games on Xbox.",
             g_br_dim);
  } else {
    int start = 0;
    if ((int)g_steam_items.size() > max_vis) {
      start = std::clamp(g_steam_selected - max_vis / 2, 0,
                         (int)g_steam_items.size() - max_vis);
    }
    for (int i = start; i < (int)g_steam_items.size() && (i - start) < max_vis; ++i) {
      const auto& it = g_steam_items[i];
      const bool sel = (i == g_steam_selected);
      const float ry = list_y0 + (i - start) * row_h;
      const float rx = pad_x + sx(16.0f);
      const float rw = left_w - sx(32.0f);
      if (sel) {
        fill_rrect(rx, ry, rw, row_h - sy(8.0f), sc(16.0f), g_br_glass2);
        stroke_rrect(rx, ry, rw, row_h - sy(8.0f), sc(16.0f), g_br_white, sc(2.0f));
      } else {
        fill_rrect(rx, ry, rw, row_h - sy(8.0f), sc(16.0f), g_br_pill_bg);
      }

      text_out_line(g_f_nav, rx + sx(18.0f), ry + sy(8.0f), rw * 0.65f, sy(26.0f),
                    it.title.c_str(), sel ? g_br_white : g_br_sub);

      std::wstring st;
      if (it.queued) {
        st = L"Queued";
      } else if (it.downloading) {
        wchar_t b[128];
        swprintf(b, 128, L"%d%% · %s", (int)(it.download_pct * 100.0f),
                 it.status_str.empty() ? L"Downloading..." : it.status_str.c_str());
        st = b;
      } else if (it.installed) {
        st = L"Ready to Play  ·  " + it.size_str;
      } else {
        st = (it.status_str.empty() ? L"In Steam Account" : it.status_str) +
             L"  ·  " + it.size_str;
      }
      text_out_line(g_f_small, rx + sx(18.0f), ry + sy(34.0f), rw - sx(36.0f),
                    sy(22.0f), st.c_str(), it.downloading ? g_br_accent : g_br_dim);

      if (it.downloading) {
        const float pw = rw * 0.28f;
        const float px = rx + rw - pw - sx(18.0f);
        const float py = ry + sy(16.0f);
        fill_rrect(px, py, pw, sy(8.0f), sy(4.0f), g_br_pill_bg);
        fill_rrect(px, py, pw * std::clamp(it.download_pct, 0.02f, 1.0f),
                   sy(8.0f), sy(4.0f), g_br_accent);
      }
    }
  }

  const int total = static_cast<int>(g_steam_items.size());
  const int first_visible = total > max_vis
      ? std::clamp(g_steam_selected - max_vis / 2, 0, total - max_vis) : 0;
  draw_scrollbar(pad_x + left_w - sx(9.0f), list_y0,
                 full_h - sy(128.0f), first_visible, max_vis, total);

  // Right Card: Account Status + Action Button + Centered Steam QR Code
  draw_glass_card(right_x, top_y, right_w, full_h, 24.0f);
  wchar_t hdr[160];
  if (g_steam_signed_in) {
    swprintf(hdr, 160, L"%s  ·  SteamID %llu",
             g_steam_persona.empty() ? L"Steam User" : g_steam_persona.c_str(),
             (unsigned long long)g_steam_id64);
  } else {
    swprintf(hdr, 160, L"Steam Account (Not Signed In)");
  }
  text_out(g_f_small, right_x + sx(24.0f), top_y + sy(22.0f), right_w - sx(48.0f),
           sy(24.0f), hdr, g_br_accent);

  if (g_steam_selected >= 0 && g_steam_selected < (int)g_steam_items.size()) {
    const auto& sel = g_steam_items[g_steam_selected];
    text_out(g_f_h2, right_x + sx(24.0f), top_y + sy(52.0f), right_w - sx(48.0f),
             sy(38.0f), sel.title.c_str(), g_br_white);

    const float by = top_y + sy(104.0f);
    const bool action_enabled = sel.installed || sel.downloading || sel.queued || g_steam_signed_in;
    if (action_enabled)
      draw_champagne_pill(right_x + sx(24.0f), by, right_w - sx(48.0f), sy(48.0f));
    else
      fill_rrect(right_x + sx(24.0f), by, right_w - sx(48.0f), sy(48.0f), sy(24.0f), g_br_pill_bg);
    if (g_controller_active) {
      draw_xbox_btn(right_x + sx(52.0f), by + sy(24.0f), sc(11.0f), L'A',
                    g_br_xbox_a, true);
    }
    std::wstring bt = sel.downloading || sel.queued
                          ? L"Cancel download"
                          : (sel.installed ? L"▶  Launch on Xbox"
                                           : g_steam_signed_in ? L"Install on Xbox"
                                                                  : L"Sign in to install");
    text_out(g_f_nav, right_x + sx(24.0f), by, right_w - sx(48.0f), sy(48.0f),
             bt.c_str(), action_enabled ? g_br_ink : g_br_dim, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  // QR Box & Status
  const float qy = top_y + sy(174.0f);
  text_out(g_f_small, right_x + sx(24.0f), qy, right_w - sx(48.0f), sy(48.0f),
           g_steam_status_line.c_str(), g_br_sub);

  if (g_qr_size > 0 && g_qr_text.find("s.team") != std::string::npos) {
    const float qsz = std::min(right_w - sx(80.0f), full_h - sy(260.0f));
    draw_qr_in_box(right_x + (right_w - qsz) * 0.5f, qy + sy(56.0f), qsz);
  } else {
    const float qbtn_y = top_y + full_h - sy(74.0f);
    const float content_x = right_x + sx(24.0f);
    const float content_w = right_w - sx(48.0f);
    float y = qy + sy(58.0f);
    if (g_steam_signed_in && y + sy(84.0f) < qbtn_y) {
      text_out_line(g_f_nav, content_x, y, content_w, sy(28.0f), L"Friends", g_br_white);
      y += sy(32.0f);
      int shown = 0;
      for (const auto& friend_info : g_steam_friends) {
        if (friend_info.state == 0 || y + sy(62.0f) >= qbtn_y || shown == 4) continue;
        const auto line = friend_info.game_name.empty() ? friend_info.persona
            : friend_info.persona + L"  ·  " + friend_info.game_name;
        text_out_line(g_f_small, content_x, y, content_w, sy(23.0f), line.c_str(), g_br_sub);
        y += sy(24.0f);
        ++shown;
      }
      if (!shown) {
        text_out_line(g_f_small, content_x, y, content_w, sy(24.0f),
                      L"No online friends reported", g_br_dim);
        y += sy(24.0f);
      }
      if (!g_steam_achs.empty() && y + sy(50.0f) < qbtn_y) {
        const auto unlocked = std::count_if(g_steam_achs.begin(), g_steam_achs.end(),
            [](const auto& achievement) { return achievement.achieved; });
        const auto label = L"Achievement activity  ·  " + std::to_wstring(unlocked) +
                           L" of " + std::to_wstring(g_steam_achs.size()) + L" unlocked";
        text_out_line(g_f_small, content_x, y + sy(10.0f), content_w, sy(24.0f),
                      label.c_str(), g_br_accent);
      }
    }
    fill_rrect(right_x + sx(24.0f), qbtn_y, right_w - sx(48.0f), sy(46.0f),
               sy(23.0f), g_br_glass2);
    stroke_rrect(right_x + sx(24.0f), qbtn_y, right_w - sx(48.0f), sy(46.0f),
                 sy(23.0f), g_br_border_hi, sc(1.1f));
    if (g_controller_active) {
      draw_xbox_btn(right_x + sx(52.0f), qbtn_y + sy(23.0f), sc(11.0f), L'Y',
                    g_br_xbox_y, true);
    }
    text_out(g_f_nav, right_x + sx(24.0f), qbtn_y, right_w - sx(48.0f),
             sy(46.0f),
             g_steam_signed_in ? L"Re-Link Steam QR Code"
                               : L"Sign in with Steam Mobile QR",
             g_br_white, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  draw_controller_footer({{L'A', L"Download / Play"},
                          {L'Y', L"Steam QR Login"},
                          {L'X', L"Refresh"},
                          {L'B', L"Back"}});
  if (!g_controller_active) {
    text_out_line(g_f_small, pad_x + sx(18.0f), top_y + full_h - sy(24.0f),
                  left_w - sx(36.0f), sy(20.0f),
                  L"Left / Right switches library views", g_br_dim);
  }
}

// ============================================================================
// SCREEN 4: TOOLS & LAN FILE TRANSFER (QR Code + Live Progress + System + Logs)
// ============================================================================
void draw_tools_screen() {
  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float full_w = g_w - 2.0f * pad_x;
  const float full_h = g_h - top_y - sy(54.0f);
  const float gap = sx(18.0f);

  const float left_w = full_w * 0.42f;
  const float right_x = pad_x + left_w + gap;
  const float right_w = full_w - left_w - gap;

  // Left Card: LAN File Transfer & Scannable QR Code (NEVER CLIPPED!)
  draw_glass_card(pad_x, top_y, left_w, full_h, 24.0f);
  text_out(g_f_h2, pad_x + sx(26.0f), top_y + sy(22.0f), left_w - sx(52.0f),
           sy(34.0f), L"LAN File Transfer", g_br_white);
  text_out(g_f_small, pad_x + sx(26.0f), top_y + sy(56.0f), left_w - sx(52.0f),
           sy(42.0f),
           L"Scan with your phone or open this URL on your PC to upload games, .zip files, cover art, or download from URL.",
           g_br_dim);

  // Big clear URL pill
  const float url_y = top_y + sy(106.0f);
  fill_rrect(pad_x + sx(24.0f), url_y, left_w - sx(48.0f), sy(44.0f),
             sy(22.0f), g_br_glass2);
  stroke_rrect(pad_x + sx(24.0f), url_y, left_w - sx(48.0f), sy(44.0f),
               sy(22.0f), g_br_accent, sc(1.2f));
  text_out(g_f_nav, pad_x + sx(24.0f), url_y, left_w - sx(48.0f), sy(44.0f),
           g_transfer_url.empty() ? L"Starting LAN Server..."
                                  : g_transfer_url.c_str(),
           g_br_accent, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  // Perfectly centered crisp QR code
  const float qr_top = url_y + sy(58.0f);
  const float qr_avail_h = full_h - sy(274.0f);
  const float qr_box = std::min(left_w - sx(68.0f), qr_avail_h);
  if (g_qr_size > 0 && qr_box > 40.0f) {
    draw_qr_in_box(pad_x + (left_w - qr_box) * 0.5f, qr_top, qr_box);
  }

  // Live Upload Progress Bar & Status at bottom of Left Card
  const float st_y = top_y + full_h - sy(96.0f);
  if (g_upload_total > 0 && g_upload_done <= g_upload_total) {
    const float pct = (float)std::clamp(
        (double)g_upload_done / (double)g_upload_total, 0.0, 1.0);
    wchar_t upb[160];
    swprintf(upb, 160, L"Transferring: %.1f / %.1f MB (%d%%) %s",
             (double)g_upload_done / 1048576.0,
             (double)g_upload_total / 1048576.0, (int)(pct * 100.0f),
             g_upload_label.c_str());
    text_out(g_f_small, pad_x + sx(24.0f), st_y, left_w - sx(48.0f), sy(24.0f),
             upb, g_br_white);
    fill_rrect(pad_x + sx(24.0f), st_y + sy(28.0f), left_w - sx(48.0f),
               sy(14.0f), sy(7.0f), g_br_pill_bg);
    fill_rrect(pad_x + sx(24.0f), st_y + sy(28.0f),
               std::max(sy(14.0f), (left_w - sx(48.0f)) * pct), sy(14.0f),
               sy(7.0f), g_br_accent);
  } else {
    text_out(g_f_small, pad_x + sx(24.0f), st_y + sy(4.0f), left_w - sx(48.0f),
             sy(40.0f),
             g_transfer_status.empty() ? L"Server listening on port 9127"
                                       : g_transfer_status.c_str(),
             g_br_sub, DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  if (!g_control_hint.empty()) {
    text_out(g_f_small, pad_x + sx(24.0f), top_y + full_h - sy(34.0f),
             left_w - sx(48.0f), sy(24.0f), g_control_hint.c_str(), g_br_dim,
             DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  // Right Column Top: Interactive Xbox Console Tools (selectable + clickable!)
  const float sys_h = sy(322.0f);
  draw_glass_card(right_x, top_y, right_w, sys_h, 24.0f);
  text_out(g_f_nav, right_x + sx(24.0f), top_y + sy(16.0f), right_w * 0.45f,
           sy(28.0f), L"Console Tools", g_br_white);
  if (!g_tool_result.empty()) {
    text_out(g_f_small, right_x + right_w * 0.44f, top_y + sy(18.0f),
             right_w * 0.56f - sx(24.0f), sy(26.0f), g_tool_result.c_str(),
             g_br_accent, DWRITE_TEXT_ALIGNMENT_TRAILING,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  const float trow_h = sy(38.0f);
  const float tlist_y = top_y + sy(50.0f);
  const int tmax_vis = std::max(1, (int)((sys_h - sy(62.0f)) / trow_h));
  const int tcount = (int)g_tools.size();
  const int tstart =
      tcount > tmax_vis
          ? std::clamp(g_tool_selected - tmax_vis / 2, 0, tcount - tmax_vis)
          : 0;
  for (int i = tstart; i < tcount && (i - tstart) < tmax_vis; ++i) {
    const float ry = tlist_y + (i - tstart) * trow_h;
    const float rx = right_x + sx(18.0f);
    const float rw = right_w - sx(36.0f);
    const bool sel = (i == g_tool_selected);
    if (sel) {
      fill_rrect(rx, ry, rw, trow_h - sy(5.0f), sc(12.0f), g_br_glass2);
      stroke_rrect(rx, ry, rw, trow_h - sy(5.0f), sc(12.0f), g_br_white, sc(1.6f));
    } else {
      fill_rrect(rx, ry, rw, trow_h - sy(5.0f), sc(12.0f),
                 ptr_in(rx, ry, rw, trow_h - sy(5.0f)) ? g_br_glass2 : g_br_pill_bg);
    }
    text_out_line(g_f_small, rx + sx(14.0f), ry, rw * 0.35f, trow_h - sy(5.0f),
                  g_tools[i].title.c_str(), sel ? g_br_white : g_br_sub,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_out_line(g_f_small, rx + rw * 0.37f, ry, rw * 0.42f, trow_h - sy(5.0f),
                  g_tools[i].desc.c_str(), g_br_dim,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float bw = sx(94.0f);
    const float bh = sy(24.0f);
    const float bx = rx + rw - bw - sx(10.0f);
    const float by = ry + (trow_h - sy(5.0f) - bh) * 0.5f;
    if (sel || g_tools[i].badge == L"ON") {
      draw_champagne_pill(bx, by, bw, bh);
      text_out_line(g_f_small, bx + sx(4.0f), by, bw - sx(8.0f), bh,
                    g_tools[i].badge.c_str(), g_br_ink,
                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
      fill_rrect(bx, by, bw, bh, bh * 0.5f, g_br_glass);
      text_out_line(g_f_small, bx + sx(4.0f), by, bw - sx(8.0f), bh,
                    g_tools[i].badge.c_str(), g_br_sub,
                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
  }

  // Right Column Bottom: Live Console Log Tail
  const float log_y = top_y + sys_h + gap;
  const float log_h = full_h - sys_h - gap;
  draw_glass_card(right_x, log_y, right_w, log_h, 24.0f);
  text_out(g_f_nav, right_x + sx(24.0f), log_y + sy(16.0f), right_w - sx(48.0f),
           sy(28.0f), L"Live Console Log", g_br_white);

  const float lrow_h = sy(22.0f);
  const int max_rows = std::max(1, (int)((log_h - sy(58.0f)) / lrow_h));
  const int total_logs = (int)g_log_tail.size();
  int max_off = std::max(0, total_logs - max_rows);
  g_log_scroll_offset = std::clamp(g_log_scroll_offset, 0, max_off);
  int start = std::max(0, total_logs - max_rows - g_log_scroll_offset);
  for (int i = 0; i < max_rows && (start + i) < total_logs; ++i) {
    text_out(g_f_mono, right_x + sx(22.0f), log_y + sy(48.0f) + i * lrow_h,
             right_w - sx(44.0f), lrow_h, g_log_tail[start + i].c_str(),
             g_br_sub);
  }

  draw_controller_footer({{L'A', L"Run Selected Tool"},
                          {L'X', L"Scroll Log Up"},
                          {L'Y', L"Scroll Log Down"},
                          {L'B', L"Back to Home"}});
}

// ============================================================================
// SCREEN 5: SETTINGS (2-Column Xbox Console Settings Menu)
// ============================================================================
void draw_settings_screen() {
  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float full_w = g_w - 2.0f * pad_x;
  const float full_h = g_h - top_y - sy(54.0f);
  const float gap = sx(18.0f);

  const float left_w = full_w * 0.25f;
  const float right_x = pad_x + left_w + gap;
  const float right_w = full_w - left_w - gap;

  // Left Summary Rail
  draw_glass_card(pad_x, top_y, left_w, full_h, 24.0f);
  text_out(g_f_h2, pad_x + sx(22.0f), top_y + sy(22.0f), left_w - sx(44.0f),
           sy(34.0f), L"System Settings", g_br_white);
  text_out(g_f_small, pad_x + sx(22.0f), top_y + sy(58.0f), left_w - sx(44.0f),
           sy(42.0f), L"Tiramisu UWP Runtime & Xbox Dev Mode Config", g_br_dim);

  const int section = selected_settings_section();
  for (int i = 0; i < static_cast<int>(std::size(k_settings_sections)); ++i) {
    const float x = pad_x + sx(18.0f);
    const float y = top_y + sy(116.0f + 48.0f * i);
    const float w = left_w - sx(36.0f);
    if (i == section) draw_champagne_pill(x, y, w, sy(40.0f));
    else fill_rrect(x, y, w, sy(40.0f), sy(20.0f),
                    ptr_in(x, y, w, sy(40.0f)) ? g_br_glass2 : g_br_pill_bg);
    text_out_line(g_f_body, x + sx(16.0f), y, w - sx(32.0f), sy(40.0f),
                  k_settings_sections[i].title, i == section ? g_br_ink : g_br_white,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  const float rbtn_y = top_y + full_h - sy(144.0f);
  const bool rhov = ptr_in(pad_x + sx(18.0f), rbtn_y, left_w - sx(36.0f), sy(40.0f));
  fill_rrect(pad_x + sx(18.0f), rbtn_y, left_w - sx(36.0f), sy(40.0f),
             sy(20.0f), g_br_glass2);
  stroke_rrect(pad_x + sx(18.0f), rbtn_y, left_w - sx(36.0f), sy(40.0f),
               sy(20.0f), rhov ? g_br_white : g_br_border_hi, sc(1.0f));
  if (g_controller_active) {
    draw_xbox_btn(pad_x + sx(40.0f), rbtn_y + sy(20.0f), sc(10.0f), L'Y',
                  g_br_xbox_y, true);
  }
  text_out(g_f_small, pad_x + sx(g_controller_active ? 54.0f : 18.0f), rbtn_y,
           left_w - sx(g_controller_active ? 72.0f : 36.0f), sy(40.0f),
           L"Reset Defaults", g_br_white, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  text_out(g_f_small, pad_x + sx(22.0f), top_y + full_h - sy(92.0f),
           left_w - sx(44.0f), sy(72.0f),
           g_sysinfo.empty() ? L"Xbox Series X|S Dev Mode\nChanges save automatically."
                             : (g_sysinfo + L"\nChanges save automatically.").c_str(),
           g_br_dim);

  // Right Settings Rows Card
  draw_glass_card(right_x, top_y, right_w, full_h, 24.0f);
  if (g_set_rows.empty()) {
    text_out(g_f_body, right_x + sx(26.0f), top_y + sy(26.0f),
             right_w - sx(52.0f), sy(40.0f), L"Loading settings...", g_br_dim);
    return;
  }

  const auto& group = k_settings_sections[section];
  text_out_line(g_f_nav, right_x + sx(24.0f), top_y + sy(18.0f),
                right_w - sx(48.0f), sy(28.0f), group.title, g_br_white);
  text_out(g_f_small, right_x + sx(24.0f), top_y + sy(52.0f),
           right_w - sx(48.0f), sy(28.0f), group.description, g_br_dim);
  const float row_h = sy(58.0f);
  int slot = 0;
  for (int i : group.rows) {
    if (i >= static_cast<int>(g_set_rows.size())) continue;
    const float ry = top_y + sy(90.0f) + slot++ * row_h;
    const float rx = right_x + sx(20.0f);
    const float rw = right_w - sx(40.0f);
    const bool sel = (i == g_set_selected);
    const bool hov = !sel && ptr_in(rx, ry, rw, row_h - sy(6.0f));

    if (sel) {
      fill_rrect(rx, ry, rw, row_h - sy(6.0f), sc(14.0f), g_br_glass2);
      stroke_rrect(rx, ry, rw, row_h - sy(6.0f), sc(14.0f), g_br_white, sc(1.8f));
    } else if (hov) {
      fill_rrect(rx, ry, rw, row_h - sy(6.0f), sc(14.0f), g_br_pill_bg);
      stroke_rrect(rx, ry, rw, row_h - sy(6.0f), sc(14.0f), g_br_border_hi, sc(1.0f));
    }

    text_out_line(g_f_body, rx + sx(18.0f), ry, rw - sx(232.0f), row_h - sy(6.0f),
                  g_set_rows[i].label, sel ? g_br_white : g_br_sub,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const float val_w = sx(188.0f);
    const float val_h = sy(32.0f);
    const float vx = rx + rw - val_w - sx(16.0f);
    const float vy = ry + (row_h - sy(6.0f) - val_h) * 0.5f;
    std::wstring disp_val = g_set_rows[i].value;
    if (g_set_rows[i].is_bool) {
      const bool on = (wcscmp(g_set_rows[i].value, L"1") == 0 ||
                       wcscmp(g_set_rows[i].value, L"ON") == 0);
      disp_val = on ? L"ON" : L"OFF";
    }
    const bool is_on = (disp_val == L"ON");
    if (sel && (g_set_rows[i].is_int || g_set_rows[i].is_choice)) {
      disp_val = L"◀  " + disp_val + L"  ▶";
    }
    if (is_on || sel) {
      draw_champagne_pill(vx, vy, val_w, val_h);
      text_out_line(g_f_small, vx + sx(8.0f), vy, val_w - sx(16.0f), val_h,
                    disp_val.c_str(), g_br_ink,
                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
      fill_rrect(vx, vy, val_w, val_h, val_h * 0.5f, g_br_pill_bg);
      text_out_line(g_f_small, vx + sx(8.0f), vy, val_w - sx(16.0f), val_h,
                    disp_val.c_str(), g_br_sub,
                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
  }

  if (g_set_selected >= 0 &&
      g_set_selected < static_cast<int>(std::size(k_setting_help))) {
    const float help_y = top_y + sy(104.0f) + slot * row_h;
    text_out(g_f_small, right_x + sx(24.0f), help_y,
             right_w - sx(48.0f), sy(64.0f), k_setting_help[g_set_selected], g_br_accent);
  }

  draw_controller_footer({{L'A', L"Change setting"}, {L'X', L"Next section"},
                          {L'Y', L"Reset defaults"}, {L'B', L"Back"}});

}

// ============================================================================
// IN-GAME STEAM COMMUNITY & QUICK OVERLAY (Matching Obsidian & Champagne)
// ============================================================================
void draw_quick_menu() {
  const float pw = sx(1040.0f);
  const float ph = sy(630.0f);
  const float px = (g_w - pw) * 0.5f;
  const float py = (g_h - ph) * 0.5f;

  // Dim backdrop over frozen game frame
  ID2D1SolidColorBrush* dim = nullptr;
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.03f, 0.05f, 0.72f), &dim);
  if (dim) {
    g_ctx->FillRectangle(D2D1::RectF(0, 0, g_w, g_h), dim);
    dim->Release();
  }

  draw_glass_card(px, py, pw, ph, 26.0f, false);
  stroke_rrect(px, py, pw, ph, sc(26.0f), g_br_accent, sc(1.6f));

  // Header
  text_out(g_f_h2, px + sx(28.0f), py + sy(22.0f), pw * 0.55f, sy(34.0f),
           L"TIRAMISU IN-GAME OVERLAY", g_br_white);
  wchar_t acct_buf[160];
  if (g_steam_signed_in) {
    swprintf(acct_buf, 160, L"%s (SteamID %llu)  ·  In-Game",
             g_steam_persona.empty() ? L"Steam User" : g_steam_persona.c_str(),
             (unsigned long long)g_steam_id64);
  } else {
    swprintf(acct_buf, 160, L"Xbox Dev Mode Session  ·  In-Game");
  }
  text_out(g_f_small, px + sx(28.0f), py + sy(56.0f), pw * 0.55f, sy(22.0f),
           acct_buf, g_br_accent);

  const float fps_val = g_fps > 0.5f ? g_fps : 60.0f;
  wchar_t perf[96];
  swprintf(perf, 96, L"%.1f FPS  ·  %.1f ms", fps_val, 1000.0f / fps_val);
  text_out(g_f_nav, px + pw - sx(280.0f), py + sy(28.0f), sx(250.0f), sy(30.0f),
           perf, g_br_accent, DWRITE_TEXT_ALIGNMENT_TRAILING);

  g_ctx->DrawLine({px + sx(24.0f), py + sy(90.0f)},
                  {px + pw - sx(24.0f), py + sy(90.0f)}, g_br_border, sc(1.0f));

  const float col_y = py + sy(108.0f);
  const float col_h = ph - sy(172.0f);
  const float left_w = pw * 0.39f;
  const float right_x = px + left_w + sx(36.0f);
  const float right_w = pw - left_w - sx(60.0f);

  int online_friends = 0;
  for (const auto& f : g_steam_friends)
    if (f.state > 0 || !f.game_name.empty()) ++online_friends;
  int ach_unlocked = 0;
  for (const auto& a : g_steam_achs)
    if (a.achieved) ++ach_unlocked;

  wchar_t flab[64], alab[64];
  swprintf(flab, 64, L"Friends (%d Online)", online_friends);
  swprintf(alab, 64, L"Achievements (%d/%zu)", ach_unlocked, g_steam_achs.size());
  const wchar_t* nav_items[6] = {
      L"▶  Resume Game",       flab,
      alab,                    L"Toggle Pad-as-Mouse",
      L"Refresh Steam Profile", L"⏏  Exit to Dashboard"};

  const float row_h = sy(56.0f);
  for (int i = 0; i < 6; ++i) {
    const float ry = col_y + i * row_h;
    const bool sel = (i == g_qm_selected);
    if (sel) {
      draw_champagne_pill(px + sx(24.0f), ry, left_w - sx(12.0f), row_h - sy(8.0f));
      text_out(g_f_body, px + sx(44.0f), ry, left_w - sx(48.0f), row_h - sy(8.0f),
               nav_items[i], g_br_ink, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
      fill_rrect(px + sx(24.0f), ry, left_w - sx(12.0f), row_h - sy(8.0f),
                 (row_h - sy(8.0f)) * 0.5f, g_br_pill_bg);
      text_out(g_f_body, px + sx(44.0f), ry, left_w - sx(48.0f), row_h - sy(8.0f),
               nav_items[i], g_br_sub, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
  }

  // Right Column: Live Friends & Achievements
  draw_glass_card(right_x, col_y, right_w, col_h, 20.0f);
  text_out(g_f_small, right_x + sx(20.0f), col_y + sy(16.0f), right_w - sx(40.0f),
           sy(24.0f), L"STEAM FRIENDS", g_br_accent);
  float fy = col_y + sy(48.0f);
  if (g_steam_friends.empty()) {
    text_out(g_f_small, right_x + sx(20.0f), fy, right_w - sx(40.0f), sy(48.0f),
             g_steam_signed_in ? L"No Steam friends currently online."
                               : L"Sign in on the Steam tab to view your friends here.",
             g_br_dim);
    fy += sy(56.0f);
  } else {
    for (size_t i = 0; i < g_steam_friends.size() && i < 5; ++i) {
      const auto& fr = g_steam_friends[i];
      wchar_t fb[160];
      if (!fr.game_name.empty())
        swprintf(fb, 160, L"●  %s — Playing %s", fr.persona.c_str(), fr.game_name.c_str());
      else if (fr.state > 0)
        swprintf(fb, 160, L"●  %s — Online", fr.persona.c_str());
      else
        swprintf(fb, 160, L"○  %s — Offline", fr.persona.c_str());
      text_out(g_f_small, right_x + sx(20.0f), fy, right_w - sx(40.0f), sy(28.0f),
               fb, (!fr.game_name.empty() || fr.state > 0) ? g_br_white : g_br_dim);
      fy += sy(32.0f);
    }
  }

  const float ay = col_y + col_h - sy(140.0f);
  g_ctx->DrawLine({right_x + sx(20.0f), ay - sy(10.0f)},
                  {right_x + right_w - sx(20.0f), ay - sy(10.0f)}, g_br_border, sc(1.0f));
  wchar_t ah[96];
  if (g_steam_achs.empty())
    swprintf(ah, 96, L"ACHIEVEMENTS (NONE FOR ACTIVE SESSION)");
  else
    swprintf(ah, 96, L"ACHIEVEMENTS (%d / %zu UNLOCKED)", ach_unlocked,
             g_steam_achs.size());
  text_out(g_f_small, right_x + sx(20.0f), ay, right_w - sx(40.0f), sy(24.0f),
           ah, g_br_accent);
  for (size_t i = 0; i < g_steam_achs.size() && i < 2; ++i) {
    const auto& a = g_steam_achs[i];
    wchar_t ab[160];
    swprintf(ab, 160, a.achieved ? L"★  %s (Unlocked)" : L"☆  %s (Locked)",
             a.name.c_str());
    text_out(g_f_small, right_x + sx(20.0f), ay + sy(32.0f) + i * sy(28.0f),
             right_w - sx(40.0f), sy(26.0f), ab,
             a.achieved ? g_br_white : g_br_dim);
  }

  // Footer
  text_out(g_f_small, px + sx(28.0f), py + ph - sy(44.0f), pw - sx(56.0f),
           sy(28.0f),
           g_controller_active
               ? L"D-Pad: Navigate   ·   (A) Select   ·   (B) Resume   ·   Back+Start: Exit Game"
               : L"Up/Down: Navigate   ·   Enter: Select   ·   Esc: Resume Game",
           g_br_dim);
}

void draw_fps_overlay() {
  if (g_fps <= 0.5f) return;
  wchar_t buf[32];
  swprintf(buf, 32, L"%.0f FPS", g_fps);
  const float fx = g_w - sx(138.0f);
  const float fy = g_h - sy(38.0f);
  fill_rrect(fx, fy, sx(94.0f), sy(26.0f), sy(13.0f), g_br_glass);
  stroke_rrect(fx, fy, sx(94.0f), sy(26.0f), sy(13.0f), g_br_border, sc(1.0f));
  text_out(g_f_small, fx, fy, sx(94.0f), sy(26.0f), buf,
           g_br_accent, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void draw_sysinfo_overlay() {
  if (g_sysinfo.empty()) return;
  const float w = sx(360.0f);
  const float h = sy(26.0f);
  const float x = g_w - sx(148.0f) - w;
  const float y = g_h - sy(38.0f);
  fill_rrect(x, y, w, h, h * 0.5f, g_br_glass);
  stroke_rrect(x, y, w, h, h * 0.5f, g_br_border, sc(1.0f));
  text_out(g_f_small, x + sx(12.0f), y, w - sx(24.0f), h, g_sysinfo.c_str(),
           g_br_sub, DWRITE_TEXT_ALIGNMENT_CENTER,
           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void draw_toast_overlay() {
  if (g_toast_ttl <= 0.0f || g_toast_msg.empty()) return;
  const float alpha = std::clamp(g_toast_ttl / 0.45f, 0.0f, 1.0f);
  const float tw = sx(480.0f);
  const float th = sy(42.0f);
  const float tx = (g_w - tw) * 0.5f;
  const float ty = sy(94.0f);
  ID2D1SolidColorBrush* bg = nullptr;
  ID2D1SolidColorBrush* bd = nullptr;
  ID2D1SolidColorBrush* fg = nullptr;
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.10f, 0.13f, 0.94f * alpha), &bg);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.796f, 0.729f, 0.710f, 0.85f * alpha), &bd);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.97f, 0.96f, 0.96f, alpha), &fg);
  if (bg && bd && fg) {
    fill_rrect(tx, ty, tw, th, th * 0.5f, bg);
    stroke_rrect(tx, ty, tw, th, th * 0.5f, bd, sc(1.4f));
    text_out(g_f_small, tx + sx(18.0f), ty, tw - sx(36.0f), th,
             g_toast_msg.c_str(), fg, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  if (bg) bg->Release();
  if (bd) bd->Release();
  if (fg) fg->Release();
}

bool prepare_vector_cursor() {
  if (!g_factory || !g_ctx || !g_br_white || !g_br_ink) return false;
  if (!g_cursor_shape) {
    if (FAILED(g_factory->CreatePathGeometry(&g_cursor_shape))) return false;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(g_cursor_shape->Open(&sink))) {
      g_cursor_shape->Release();
      g_cursor_shape = nullptr;
      return false;
    }
    constexpr D2D1_POINT_2F points[] = {
        {0.f, 0.f}, {0.f, 15.5f}, {4.0f, 12.0f}, {7.0f, 17.5f},
        {9.0f, 16.4f}, {6.2f, 11.2f}, {11.5f, 11.2f},
    };
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points + 1, static_cast<UINT32>(std::size(points) - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    const HRESULT result = sink->Close();
    sink->Release();
    if (FAILED(result)) {
      g_cursor_shape->Release();
      g_cursor_shape = nullptr;
      return false;
    }
  }
  return true;
}

void draw_vector_cursor() {
  if (!g_cursor_visible || g_ptr_x < 0 || g_ptr_y < 0 || !prepare_vector_cursor()) return;
  const float size = static_cast<float>(cursor_height(g_h)) / 18.f;
  D2D1_MATRIX_3X2_F previous;
  g_ctx->GetTransform(&previous);
  g_ctx->SetTransform(D2D1::Matrix3x2F(size, 0.f, 0.f, size,
                                      static_cast<float>(g_ptr_x), static_cast<float>(g_ptr_y)));
  g_ctx->FillGeometry(g_cursor_shape, g_br_white);
  g_ctx->DrawGeometry(g_cursor_shape, g_br_ink, 1.15f);
  g_ctx->SetTransform(previous);
}
} // namespace

// ============================================================================
// PUBLIC API
// ============================================================================
void ui_steam_nav_up() {
  if (g_steam_selected > 0) {
    --g_steam_selected;
    ui_sfx_move();
  }
}
void ui_steam_nav_down() {
  if (g_steam_selected + 1 < (int)g_steam_items.size()) {
    ++g_steam_selected;
    ui_sfx_move();
  }
}
unsigned ui_steam_selected_app() {
  return g_steam_selected >= 0 && g_steam_selected < static_cast<int>(g_steam_items.size())
             ? g_steam_items[g_steam_selected].app_id : 0;
}

void ui_steam_cycle_filter(int direction) {
  const unsigned selected_app = ui_steam_selected_app();
  g_steam_filter = (g_steam_filter + direction + 3) % 3;
  g_steam_selected = 0;
  filter_steam_items(selected_app);
  ui_sfx_move();
}

void ui_set_steam_account(bool signed_in, uint64_t steamid64,
                          std::wstring persona, std::wstring status_line) {
  g_steam_signed_in = signed_in;
  g_steam_id64 = steamid64;
  g_steam_persona = std::move(persona);
  g_steam_status_line = std::move(status_line);
}

void ui_set_steam_items(const std::vector<ui_steam_item>& items) {
  const unsigned selected_app = ui_steam_selected_app();
  g_steam_library = items;
  g_steam_installed = std::count_if(items.begin(), items.end(),
      [](const auto& item) { return item.installed; });
  g_steam_downloading = std::count_if(items.begin(), items.end(),
      [](const auto& item) { return item.downloading || item.queued; });
  filter_steam_items(selected_app);
}

void ui_set_steam_friends(const std::vector<ui_steam_friend>& friends) {
  g_steam_friends = friends;
}

void ui_set_steam_achievements(const std::vector<ui_steam_ach>& achs) {
  g_steam_achs = achs;
}

void ui_quick_menu_nav_up() {
  if (g_qm_selected > 0) {
    --g_qm_selected;
    ui_sfx_move();
  }
}
void ui_quick_menu_nav_down() {
  if (g_qm_selected < 5) {
    ++g_qm_selected;
    ui_sfx_move();
  }
}
int ui_quick_menu_selected() { return g_qm_selected; }

void ui_set_input_mode(bool controller_active, bool pad_connected) {
  g_controller_active = controller_active;
  g_pad_connected = pad_connected;
}

void ui_set_pointer_pos(int x, int y) {
  g_ptr_x = x;
  g_ptr_y = y;
}

void ui_set_cursor_visible(bool visible) { g_cursor_visible = visible; }
bool ui_can_draw_cursor() { return prepare_vector_cursor(); }

int ui_settings_step(int direction) {
  const auto& rows = k_settings_sections[selected_settings_section()].rows;
  const auto current = std::find(rows.begin(), rows.end(), g_set_selected);
  const int index = current == rows.end() ? 0 : static_cast<int>(current - rows.begin());
  return rows[std::clamp(index + direction, 0, static_cast<int>(rows.size()) - 1)];
}

int ui_settings_next_section() {
  const int next = (selected_settings_section() + 1) % static_cast<int>(std::size(k_settings_sections));
  return k_settings_sections[next].rows.front();
}


void ui_toast(std::wstring message, float duration_sec) {
  g_toast_msg = std::move(message);
  g_toast_max = std::max(0.5f, duration_sec);
  g_toast_ttl = g_toast_max;
}

void ui_set_storage(double used_gb, double total_gb) {
  g_storage_used_gb = used_gb;
  g_storage_total_gb = total_gb;
}

void ui_set_upload_progress(uint64_t done_bytes, uint64_t total_bytes,
                            std::wstring label) {
  g_upload_done = done_bytes;
  g_upload_total = total_bytes;
  g_upload_label = std::move(label);
}

void ui_set_tools(const std::vector<ui_tool_item>& tools, int selected,
                  std::wstring last_result) {
  g_tools = tools;
  if (!g_tools.empty()) {
    g_tool_selected = std::clamp(selected, 0, (int)g_tools.size() - 1);
  } else {
    g_tool_selected = 0;
  }
  g_tool_result = std::move(last_result);
}

void ui_tools_nav_up() {
  if (g_tool_selected > 0) {
    --g_tool_selected;
    ui_sfx_move();
  }
}

void ui_tools_nav_down() {
  if (g_tool_selected + 1 < (int)g_tools.size()) {
    ++g_tool_selected;
    ui_sfx_move();
  }
}

int ui_tools_selected() { return g_tool_selected; }

void ui_log_scroll(int delta) {
  g_log_scroll_offset = std::max(0, g_log_scroll_offset + delta);
}

void ui_set_delete_confirm(int game_index) { g_delete_confirm_idx = game_index; }

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v) {
  g_w = (float)width;
  g_h = (float)height;
  g_factory = (ID2D1Factory1*)d2d_factory_v;
  g_d2d_device = (ID2D1Device*)d2d_device_v;
  g_dw = (IDWriteFactory*)dwrite_factory_v;
  g_swap = (IDXGISwapChain1*)swapchain_v;
  if (g_factory) g_factory->AddRef();
  if (g_dw) g_dw->AddRef();
  if (g_d2d_device) g_d2d_device->AddRef();
  if (g_swap) g_swap->AddRef();

  g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx);

  g_ctx->CreateSolidColorBrush(D2D1::ColorF(1.000f, 1.000f, 1.000f, 1.0f), &g_br_white);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.875f, 0.812f, 0.788f, 1.0f), &g_br_accent);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.788f, 0.710f, 0.682f, 1.0f), &g_br_accent2);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.078f, 0.075f, 0.094f, 1.0f), &g_br_ink);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.895f, 0.885f, 0.905f, 1.0f), &g_br_sub);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.675f, 0.660f, 0.700f, 1.0f), &g_br_dim);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.110f, 0.118f, 0.149f, 0.86f), &g_br_glass);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.165f, 0.176f, 0.220f, 0.92f), &g_br_glass2);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), &g_br_border);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f), &g_br_border_hi);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &g_br_pill_bg);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.62f), &g_br_shadow);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.208f, 0.780f, 0.349f, 1.0f), &g_br_xbox_a);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.898f, 0.282f, 0.302f, 1.0f), &g_br_xbox_b);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.169f, 0.541f, 1.000f, 1.0f), &g_br_xbox_x);
  g_ctx->CreateSolidColorBrush(D2D1::ColorF(0.961f, 0.718f, 0.000f, 1.0f), &g_br_xbox_y);

  const float fscale = std::min(g_w / 1440.0f, g_h / 810.0f);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         50.0f * fscale, L"en-US", &g_f_hero);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         36.0f * fscale, L"en-US", &g_f_hero_md);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         44.0f * fscale, L"en-US", &g_f_stat);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         24.0f * fscale, L"en-US", &g_f_h2);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         18.0f * fscale, L"en-US", &g_f_nav);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         16.5f * fscale, L"en-US", &g_f_body);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         14.0f * fscale, L"en-US", &g_f_small);
  g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         13.5f * fscale, L"en-US", &g_f_mono);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         12.5f * fscale, L"en-US", &g_f_btn);

  // Upload embedded Tiramisu mascot badge bitmap (avatar_bear.png)
  D2D1_BITMAP_PROPERTIES mp = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  g_ctx->CreateBitmap(D2D1::SizeU(k_mascot_w, k_mascot_h), k_mascot_bgra,
                      k_mascot_w * 4, mp, &g_mascot_bmp);
}

void ui_shutdown() {
  for (auto& kv : g_cover_bmps) {
    if (kv.second.bmp) kv.second.bmp->Release();
  }
  g_cover_bmps.clear();
  for (auto& kv : g_hero_bmps) {
    if (kv.second.bmp) kv.second.bmp->Release();
  }
  g_hero_bmps.clear();
  auto rel = [](auto*& p) {
    if (p) { p->Release(); p = nullptr; }
  };
  rel(g_boot_bmp);
  rel(g_mascot_bmp);
  rel(g_br_white); rel(g_br_accent); rel(g_br_accent2); rel(g_br_ink);
  rel(g_br_sub); rel(g_br_dim); rel(g_br_glass); rel(g_br_glass2);
  rel(g_br_border); rel(g_br_border_hi); rel(g_br_pill_bg); rel(g_br_shadow);
  rel(g_br_xbox_a); rel(g_br_xbox_b); rel(g_br_xbox_x); rel(g_br_xbox_y);
  rel(g_f_hero); rel(g_f_hero_md); rel(g_f_stat); rel(g_f_h2); rel(g_f_nav);
  rel(g_f_body); rel(g_f_small); rel(g_f_mono); rel(g_f_btn);
  rel(g_cursor_shape);
  rel(g_ctx); rel(g_dw); rel(g_d2d_device); rel(g_swap); rel(g_factory);
}

void ui_set_games(const std::vector<ui_game>& games, int selected) {
  const int prev_sel = g_selected;
  g_games = games;
  g_selected = selected;
  if (!g_games.empty() && g_selected >= 0 && g_selected < (int)g_games.size()) {
    const std::string& next_key = g_games[g_selected].key_utf8;
    if (g_hero_cur_key.empty()) {
      g_hero_cur_key = next_key;
      g_hero_xfade_t = 1.0f;
    } else if (next_key != g_hero_cur_key) {
      g_hero_prev_key = g_hero_cur_key;
      g_hero_cur_key = next_key;
      g_hero_xfade_t = 0.0f;
      if (prev_sel != g_selected) ui_sfx_move();
    }
  }
}
void ui_set_lines(const std::vector<std::wstring>& lines) { g_lines = lines; }
void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished) {
  g_setup_steps = steps;
  g_setup_done_count = done_count;
  g_setup_finished = finished;
}
void ui_dismiss_first_run() {
  g_first_run = false;
  if (g_screen == UI_TOOLS) g_screen = UI_SHELF;
}
void ui_set_control_hint(std::wstring hint) { g_control_hint.swap(hint); }
void ui_set_transfer(std::wstring url, std::wstring status) {
  g_transfer_url.swap(url);
  g_transfer_status.swap(status);
}
void ui_set_screen(ui_screen s) {
  if (g_screen != s) {
    g_screen = s;
    g_tab_t = 0.0f;
    if (s < UI_TAB_COUNT && g_intro_t > 0.15f) ui_sfx_tab();
  }
}
void ui_set_quick_menu(bool visible) {
  g_quick_menu = visible;
  g_screen = visible ? UI_QUICKMENU : UI_SHELF;
}
void ui_set_fps(float fps) { g_fps = fps; }
void ui_set_sysinfo(const std::wstring& info) { g_sysinfo = info; }
void ui_set_settings(const ui_setting* rows, int count, int selected) {
  const bool moved = !g_set_rows.empty() && selected != g_set_selected;
  g_set_rows.assign(rows, rows + (count > 0 ? count : 0));
  g_set_selected = selected;
  if (moved) ui_sfx_move();
}
void ui_set_log_tail(const std::vector<std::wstring>& lines) { g_log_tail = lines; }

void ui_set_qr(const std::string& qr_text) {
  if (g_qr_text == qr_text) return;
  g_qr_text = qr_text;
  g_qr_size = 0;
  if (!qr_text.empty()) {
    uint8_t work[qrcodegen_BUFFER_LEN_MAX];
    if (qrcodegen_encodeText(qr_text.c_str(), work, g_qr, qrcodegen_Ecc_MEDIUM,
                             qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                             qrcodegen_Mask_AUTO, true)) {
      g_qr_size = qrcodegen_getSize(g_qr);
    }
  }
}

ui_click ui_hit_test(int x, int y) {
  auto hits = [x, y](float l, float t, float w, float h) {
    return x >= l && y >= t && x < l + w && y < t + h;
  };
  if (g_screen == UI_QUICKMENU) {
    const float pw = sx(1040.0f);
    const float ph = sy(630.0f);
    const float px = (g_w - pw) * 0.5f;
    const float py = (g_h - ph) * 0.5f;
    const float col_y = py + sy(108.0f);
    const float left_w = pw * 0.39f;
    const float row_h = sy(56.0f);
    for (int i = 0; i < 6; ++i) {
      const float ry = col_y + i * row_h;
      if (hits(px + sx(24.0f), ry, left_w - sx(12.0f), row_h - sy(8.0f))) {
        g_qm_selected = i;
        return {ui_click_type::quick_menu_item, i};
      }
    }
    return {ui_click_type::none, 0};
  }

  // Top capsule nav bar tabs
  const nav_layout nl = get_nav_layout();
  for (int i = 0; i < UI_TAB_COUNT; ++i) {
    if (hits(nl.nav_x + i * nl.tab_w, nl.nav_y, nl.tab_w, nl.nav_h)) {
      return {ui_click_type::tab, i};
    }
  }
  // Clicking Wi-Fi icon jumps to Tools/Transfer
  if (hits(nl.wifi_cx - sc(26.0f), nl.wifi_cy - sc(26.0f), sc(52.0f), sc(52.0f))) {
    return {ui_click_type::open_transfer, 0};
  }

  const float pad_x = sx(44.0f);
  const float top_y = sy(108.0f);
  const float gap = sx(18.0f);
  const float full_w = g_w - 2.0f * pad_x;

  if (g_screen == UI_SHELF) {
    const float hero_w = full_w * 0.742f;
    const float hero_h = sy(368.0f);
    const float right_x = pad_x + hero_w + gap;
    const float right_w = full_w - hero_w - gap;
    const float card_h = (hero_h - gap) * 0.5f;

    // Right cards: Library -> tab 1, Storage -> Tools tab 3
    if (hits(right_x, top_y, right_w, card_h)) {
      return {ui_click_type::tab, UI_LIBRARY};
    }
    if (hits(right_x, top_y + card_h + gap, right_w, card_h)) {
      return {ui_click_type::open_transfer, 0};
    }

    // Hero buttons
    const float hp_x = pad_x + sx(32.0f);
    const float act_y = top_y + hero_h - sy(78.0f);
    const float play_w = sx(184.0f), play_h = sy(50.0f);
    if (hits(hp_x, act_y, play_w, play_h)) {
      return g_games.empty() ? ui_click{ui_click_type::open_transfer, 0}
                             : ui_click{ui_click_type::play, g_selected};
    }
    const float c1_x = hp_x + play_w + sx(16.0f);
    if (hits(c1_x, act_y, play_h, play_h)) {
      return {ui_click_type::cycle_exe, g_selected};
    }
    const float c2_x = c1_x + play_h + sx(14.0f);
    if (hits(c2_x, act_y, play_h, play_h)) {
      return {ui_click_type::open_transfer, 0};
    }

    // Continue Playing 4-card widescreen row (matches draw_home_screen)
    const float cards_y = top_y + hero_h + sy(64.0f);
    const int max_cards = 4;
    const float card_gap = sx(18.0f);
    const float card_w = (full_w - (float)(max_cards - 1) * card_gap) / (float)max_cards;
    const float card_h2 = sy(168.0f);
    const int total = (int)g_games.size();
    int start = (total > max_cards) ? std::clamp(g_selected - 1, 0, total - max_cards) : 0;
    for (int slot = 0; slot < max_cards && (start + slot) < total; ++slot) {
      const float cx = pad_x + slot * (card_w + card_gap);
      if (hits(cx, cards_y - sy(6.0f), card_w, card_h2 + sy(6.0f))) {
        const int idx = start + slot;
        return (idx == g_selected) ? ui_click{ui_click_type::play, idx}
                                   : ui_click{ui_click_type::select_game, idx};
      }
    }
  } else if (g_screen == UI_LIBRARY) {
    const float full_h = g_h - top_y - sy(54.0f);
    const float left_w = full_w * 0.25f;
    const float right_x = pad_x + left_w + gap;
    const float right_w = full_w - left_w - gap;
    if (hits(pad_x + sx(18.0f), top_y + sy(190.0f), left_w - sx(36.0f), sy(40.0f)))
      return {ui_click_type::tab, UI_TOOLS};
    if (hits(pad_x + sx(18.0f), top_y + sy(238.0f), left_w - sx(36.0f), sy(40.0f)))
      return {ui_click_type::tab, UI_STEAM};
    const float insp_h = sy(196.0f);
    const float by = top_y + insp_h - sy(58.0f);
    if (hits(right_x + sx(26.0f), by, sx(152.0f), sy(40.0f))) {
      return {ui_click_type::play, g_selected};
    }
    if (hits(right_x + sx(190.0f), by, sx(168.0f), sy(40.0f))) {
      return {ui_click_type::cycle_exe, g_selected};
    }
    if (hits(right_x + sx(370.0f), by, sx(184.0f), sy(40.0f))) {
      return {ui_click_type::cycle_input, g_selected};
    }
    if (hits(right_x + sx(566.0f), by, sx(168.0f), sy(40.0f))) {
      return {ui_click_type::delete_game, g_selected};
    }
    const float grid_y = top_y + insp_h + gap;
    const float grid_h = full_h - insp_h - gap;
    const int cols = 4;
    const float gpad = sx(18.0f), cgap = sx(16.0f);
    const float cw = (right_w - 2.0f * gpad - (cols - 1) * cgap) / (float)cols;
    const float ch = cw / 1.92f;
    const int vis_rows = std::max(1, (int)((grid_h - 2.0f * gpad + cgap) / (ch + cgap)));
    const int start_row = std::max(0, (g_selected / cols) - vis_rows + 1);
    for (int i = start_row * cols; i < (int)g_games.size(); ++i) {
      const int r = (i / cols) - start_row, c = i % cols;
      if (r >= vis_rows) break;
      const float cx = right_x + gpad + c * (cw + cgap);
      const float cy = grid_y + gpad + r * (ch + cgap);
      if (hits(cx, cy, cw, ch)) {
        return (i == g_selected) ? ui_click{ui_click_type::play, i}
                                 : ui_click{ui_click_type::select_game, i};
      }
    }
  } else if (g_screen == UI_STEAM) {
    const float full_h = g_h - top_y - sy(54.0f);
    const float left_w = full_w * 0.54f;
    const float right_x = pad_x + left_w + gap;
    const float right_w = full_w - left_w - gap;
    const float filter_w = (left_w - sx(48.0f)) / 3;
    for (int i = 0; i < 3; ++i) {
      const float x = pad_x + sx(16.0f) + i * (filter_w + sx(8.0f));
      if (hits(x, top_y + sy(62.0f), filter_w, sy(36.0f))) {
        if (i != g_steam_filter) ui_steam_cycle_filter(i - g_steam_filter);
        return {ui_click_type::select_steam, 0};
      }
    }
    const float list_y0 = top_y + sy(112.0f);
    const float row_h = sy(68.0f);
    const int max_vis = std::max(1, (int)((full_h - sy(128.0f)) / row_h));
    int start = ((int)g_steam_items.size() > max_vis)
                    ? std::clamp(g_steam_selected - max_vis / 2, 0,
                                 (int)g_steam_items.size() - max_vis)
                    : 0;
    for (int i = start; i < (int)g_steam_items.size() && (i - start) < max_vis; ++i) {
      const float ry = list_y0 + (i - start) * row_h;
      if (hits(pad_x + sx(16.0f), ry, left_w - sx(32.0f), row_h - sy(8.0f))) {
        g_steam_selected = i;
        return {ui_click_type::select_steam, i};
      }
    }
    if (hits(right_x + sx(24.0f), top_y + sy(104.0f), right_w - sx(48.0f), sy(48.0f))) {
      return {ui_click_type::steam_action, g_steam_selected};
    }
    if (!(g_qr_size > 0 && g_qr_text.find("s.team") != std::string::npos) &&
        hits(right_x + sx(24.0f), top_y + full_h - sy(74.0f), right_w - sx(48.0f), sy(46.0f))) {
      return {ui_click_type::steam_qr, 0};
    }
  } else if (g_screen == UI_TOOLS) {
    const float left_w = full_w * 0.42f;
    const float right_x = pad_x + left_w + gap;
    const float right_w = full_w - left_w - gap;
    const float sys_h = sy(322.0f);
    const float trow_h = sy(38.0f);
    const float tlist_y = top_y + sy(50.0f);
    const int tmax_vis = std::max(1, (int)((sys_h - sy(62.0f)) / trow_h));
    const int tcount = (int)g_tools.size();
    const int tstart =
        tcount > tmax_vis
            ? std::clamp(g_tool_selected - tmax_vis / 2, 0, tcount - tmax_vis)
            : 0;
    for (int i = tstart; i < tcount && (i - tstart) < tmax_vis; ++i) {
      const float ry = tlist_y + (i - tstart) * trow_h;
      if (hits(right_x + sx(18.0f), ry, right_w - sx(36.0f), trow_h - sy(5.0f))) {
        g_tool_selected = i;
        return {ui_click_type::run_tool, i};
      }
    }
  } else if (g_screen == UI_SETTINGS) {
    const float full_h = g_h - top_y - sy(54.0f);
    const float left_w = full_w * 0.25f;
    const float right_x = pad_x + left_w + gap;
    const float right_w = full_w - left_w - gap;
    if (hits(pad_x + sx(18.0f), top_y + full_h - sy(144.0f),
             left_w - sx(36.0f), sy(40.0f))) {
      return {ui_click_type::reset_settings, 0};
    }
    for (int i = 0; i < static_cast<int>(std::size(k_settings_sections)); ++i) {
      if (hits(pad_x + sx(18.0f), top_y + sy(116.0f + 48.0f * i),
               left_w - sx(36.0f), sy(40.0f))) {
        return {ui_click_type::select_settings_section, k_settings_sections[i].rows.front()};
      }
    }
    const float row_h = sy(58.0f);
    const auto& group = k_settings_sections[selected_settings_section()];
    int slot = 0;
    for (int i : group.rows) {
      if (i >= static_cast<int>(g_set_rows.size())) continue;
      const float ry = top_y + sy(90.0f) + slot++ * row_h;
      if (hits(right_x + sx(20.0f), ry, right_w - sx(40.0f), row_h - sy(6.0f))) {
        return {ui_click_type::select_setting, i};
      }
    }
  }
  return {ui_click_type::none, 0};
}

void ui_render() {
  if (!g_ctx || !g_swap) return;
  const ULONGLONG now_ms = GetTickCount64();
  const float dt = (g_last_frame_ms != 0 && now_ms > g_last_frame_ms)
                       ? std::clamp((float)(now_ms - g_last_frame_ms) * 0.001f,
                                    0.001f, 0.05f)
                       : 0.016f;
  g_last_frame_ms = now_ms;
  g_anim_time += dt;
  if (g_hero_xfade_t < 1.0f) {
    g_hero_xfade_t = std::min(1.0f, g_hero_xfade_t + dt / 0.34f);
  }
  if (g_toast_ttl > 0.0f) {
    g_toast_ttl -= dt;
    if (g_toast_ttl < 0.0f) g_toast_ttl = 0.0f;
  }

  ID2D1Image* old_target = nullptr;
  g_ctx->GetTarget(&old_target);
  ID2D1Bitmap1* bmp = nullptr;
  IDXGISurface* surf = nullptr;
  if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) {
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    bp.bitmapOptions =
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bp.dpiX = 96.0f;
    bp.dpiY = 96.0f;
    if (FAILED(g_ctx->CreateBitmapFromDxgiSurface(surf, &bp, &bmp)) || !bmp) {
      surf->Release();
      if (old_target) old_target->Release();
      return;
    }
    g_ctx->SetTarget(bmp);
    g_ctx->BeginDraw();

    // Check if the full-screen HD Tiramisu startup boot animation is playing
    std::vector<uint8_t> boot_bgra;
    unsigned bw = 0, bh = 0;
    uint64_t bver = 0;
    float bop = 0.0f;
    const bool boot_active =
        boot_anim_get_frame(&boot_bgra, &bw, &bh, &bver, &bop);
    if (!boot_active && g_boot_bmp) {
      g_boot_bmp->Release();
      g_boot_bmp = nullptr;
      g_boot_frame_ver = 0;
    }

    // Hold the dashboard entrance choreography at t=0 while the boot animation
    // is opaque, then trigger the smooth staggered drop-in the exact moment the
    // boot animation begins dissolving (or is skipped)!
    if (boot_active && bop >= 0.995f) {
      g_boot_was_active = true;
      g_intro_t = 0.0f;
      g_tab_t = 0.0f;
    } else {
      if (g_boot_was_active) {
        g_boot_was_active = false;
        g_intro_t = 0.0f;
        g_tab_t = 0.0f;
      }
      g_intro_t = std::min(10.0f, g_intro_t + dt);
      g_tab_t = std::min(10.0f, g_tab_t + dt);
    }

    // Draw dashboard underneath whenever boot animation isn't 100% opaque
    // (gives a silky 0.45s crossfade dissolve straight into the choreographed entrance!)
    if (!boot_active || bop < 0.995f) {
      if (g_screen == UI_QUICKMENU) {
        draw_quick_menu();
      } else {
        draw_ambient_background();
        draw_top_navigation();
        switch (g_screen) {
          case UI_SHELF:
            draw_home_screen();
            break;
          case UI_LIBRARY: {
            anim_scope tab_a(g_tab_t, 0.00f, 0.38f, 0.0f, sy(22.0f), 0.985f,
                             g_w * 0.5f, g_h * 0.5f);
            if (tab_a.visible) draw_library_screen();
            break;
          }
          case UI_STEAM: {
            anim_scope tab_a(g_tab_t, 0.00f, 0.38f, 0.0f, sy(22.0f), 0.985f,
                             g_w * 0.5f, g_h * 0.5f);
            if (tab_a.visible) draw_steam_screen();
            break;
          }
          case UI_TOOLS: {
            anim_scope tab_a(g_tab_t, 0.00f, 0.38f, 0.0f, sy(22.0f), 0.985f,
                             g_w * 0.5f, g_h * 0.5f);
            if (tab_a.visible) draw_tools_screen();
            break;
          }
          case UI_SETTINGS: {
            anim_scope tab_a(g_tab_t, 0.00f, 0.38f, 0.0f, sy(22.0f), 0.985f,
                             g_w * 0.5f, g_h * 0.5f);
            if (tab_a.visible) draw_settings_screen();
            break;
          }
          default:
            draw_home_screen();
            break;
        }
        {
          anim_scope ov_a(g_intro_t, 0.45f, 0.35f, 0.0f, sy(14.0f));
          if (ov_a.visible) {
            if (longan::trt_settings_show_sysinfo()) draw_sysinfo_overlay();
            if (longan::trt_settings_show_fps()) draw_fps_overlay();
          }
        }
        draw_toast_overlay();
      }
      if (!boot_active) draw_vector_cursor();
    }

    if (boot_active && bw > 0 && bh > 0) {
      if (!g_boot_bmp || g_boot_frame_ver != bver) {
        if (g_boot_bmp) {
          g_boot_bmp->Release();
          g_boot_bmp = nullptr;
        }
        D2D1_BITMAP_PROPERTIES bp2 = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(g_ctx->CreateBitmap(D2D1::SizeU(bw, bh), boot_bgra.data(),
                                          bw * 4, bp2, &g_boot_bmp))) {
          g_boot_frame_ver = bver;
        }
      }
      if (g_boot_bmp) {
        if (bop >= 0.995f) g_ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        D2D1_RECT_F dst{0.0f, 0.0f, g_w, g_h};
        D2D1_RECT_F src{0.0f, 0.0f, (float)bw, (float)bh};
        g_ctx->DrawBitmap(g_boot_bmp, &dst, bop,
                          D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
      }
    }

    g_ctx->EndDraw();
    bmp->Release();
    surf->Release();
    g_ctx->SetTarget(old_target);
    if (old_target) old_target->Release();
  }
}

} // namespace longan
