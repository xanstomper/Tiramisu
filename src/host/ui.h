// tiramisu/src/host/ui.h
// Obsidian & Champagne Glassmorphic Xbox Series X|S Dashboard UI.
#ifndef TIRAMISU_UI_H
#define TIRAMISU_UI_H

#include <cstdint>
#include <string>
#include <vector>
#include "vendor/qrcodegen.h"

namespace longan {

struct ui_game {
  std::wstring name;
  std::wstring exe;
  std::wstring meta;         // e.g. input mode / appid
  std::wstring engine_type;  // Direct3D 11 | Vulkan | Unity | Mono / FNA | ...
  std::string key_utf8;      // UTF-8 game key for cover_art lookup
};

// 5 primary capsule tabs matching the Tiramisu Series X|S dashboard design:
// Home | Library | Steam | Tools (Transfer & Logs) | Settings
enum ui_screen {
  UI_SHELF = 0,     // Home (Hero spotlight + Library/Storage cards + Continue Playing)
  UI_LIBRARY = 1,   // Full cover-art library grid & inspector
  UI_STEAM = 2,     // On-console Steam library, QR auth & native depot downloader
  UI_TOOLS = 3,     // LAN File Transfer + QR Code + System Health + Live Log
  UI_SETTINGS = 4,  // Xbox Console & Runtime Settings
  UI_TAB_COUNT = 5,
  UI_QUICKMENU = UI_TAB_COUNT,  // In-game Steam Community & Quick Overlay
  // Legacy aliases kept so older callers stay readable:
  UI_SETUP = UI_TOOLS,
  UI_LOG = UI_TOOLS
};

struct ui_steam_item {
  std::wstring title;
  unsigned app_id;
  std::wstring size_str;
  bool installed;
  bool downloading;
  bool queued = false;
  float download_pct;
  std::wstring status_str;
  std::wstring install_dir;
};

struct ui_steam_friend {
  std::wstring persona;
  int state;  // 0=Offline, 1=Online, 3=Away, 6=In-Game
  std::wstring game_name;
};

struct ui_steam_ach {
  std::wstring name;
  bool achieved;
  unsigned cur_progress;
  unsigned max_progress;
};

void ui_steam_nav_up();
void ui_steam_nav_down();
unsigned ui_steam_selected_app();
void ui_steam_cycle_filter(int direction);
void ui_set_steam_account(bool signed_in, uint64_t steamid64,
                          std::wstring persona, std::wstring status_line);
void ui_set_steam_items(const std::vector<ui_steam_item>& items);
void ui_set_steam_friends(const std::vector<ui_steam_friend>& friends);
void ui_set_steam_achievements(const std::vector<ui_steam_ach>& achs);
void ui_quick_menu_nav_up();
void ui_quick_menu_nav_down();
int ui_quick_menu_selected();

// Adaptive input mode: when controller_active == true, Xbox controller button
// badges (A/B/X/Y/LB/RB) are drawn on screen; when KBM input is detected,
// controller badges are hidden until controller input resumes.
void ui_set_input_mode(bool controller_active, bool pad_connected);

// Live mouse/pointer position & visibility for crisp D2D vector cursor
void ui_set_pointer_pos(int x, int y);
void ui_set_cursor_visible(bool visible);
bool ui_can_draw_cursor();
int ui_settings_step(int direction);
int ui_settings_next_section();

// Sleek top-center glassmorphic toast banner (auto-fades after duration_sec)
void ui_toast(std::wstring message, float duration_sec = 3.2f);

// Real Xbox storage telemetry (from GetDiskFreeSpaceExA)
void ui_set_storage(double used_gb, double total_gb);

// Live LAN file transfer progress bar telemetry
void ui_set_upload_progress(uint64_t done_bytes, uint64_t total_bytes,
                            std::wstring label);

// Interactive tools on the Tools screen
struct ui_tool_item {
  std::wstring title;
  std::wstring desc;
  std::wstring badge;
};
void ui_set_tools(const std::vector<ui_tool_item>& tools, int selected,
                  std::wstring last_result);
void ui_tools_nav_up();
void ui_tools_nav_down();
int ui_tools_selected();

// One editable setting row on the Settings screen
struct ui_setting {
  wchar_t label[64];
  wchar_t value[64];
  bool is_bool;
  bool is_choice;
  bool is_int;
};

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v);
void ui_shutdown();
void ui_set_games(const std::vector<ui_game>& games, int selected);
void ui_set_lines(const std::vector<std::wstring>& lines);
void ui_set_qr(const std::string& qr_text);  // empty = hide QR
void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished);
void ui_dismiss_first_run();
void ui_set_transfer(std::wstring url, std::wstring status);
void ui_set_control_hint(std::wstring hint);
void ui_set_screen(ui_screen s);
void ui_set_settings(const ui_setting* rows, int count, int selected);
void ui_set_quick_menu(bool visible);
void ui_set_fps(float fps);
void ui_set_sysinfo(const std::wstring& info);
void ui_set_log_tail(const std::vector<std::wstring>& lines);
void ui_log_scroll(int delta);
void ui_set_delete_confirm(int game_index);

enum class ui_click_type {
  none,
  tab,
  open_home,
  play,
  select_game,
  cycle_exe,
  cycle_input,
  delete_game,
  open_transfer,
  select_setting,
  select_settings_section,
  reset_settings,
  select_steam,
  steam_action,
  steam_qr,
  run_tool,
  quick_menu_item
};
struct ui_click {
  ui_click_type type;
  int index;
};
ui_click ui_hit_test(int x, int y);
void ui_render();

} // namespace longan

#endif
