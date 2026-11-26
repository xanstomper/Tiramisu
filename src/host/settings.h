// tiramisu/src/host/settings.h
// Tiramisu host settings: persisted to LocalState\settings.ini as plain
// `key=value` text. The guest-facing knobs (reported OS version, GC
// write-watch override) are consumed by the trt bridge layer via the
// trt_settings_* getters — a game that checks the OS version or gates its
// allocator on a registry surface gets what the user set, not a constant.
//
// Pure std::string/file I/O: offline-gateable on Linux, no WinRT.
#ifndef TIRAMISU_SETTINGS_H
#define TIRAMISU_SETTINGS_H

#include <string>

namespace longan {

struct host_settings {
  // ---- game defaults ----
  std::string default_input = "auto";  // auto | controller | kbm | pad2kbm
  // Per-title launch arguments (also settable in a per-title profile file).
  // A game-shipped tiramisu_args.txt still wins over both.
  std::string launch_args;
  // Comma-separated host services a title expects ("xinput,wasapi,steam").
  // A title that needs one we do not have should say so in the log before it
  // fails somewhere confusing; a typo in the list is caught the same way.
  std::string required_services;
  // Optional UNC or mounted-directory root for game folders streamed from a
  // PC share; scanned alongside LocalState and packaged network-games.
  std::string network_games_root;
  int mouse_speed = 50;                // gamepad-as-mouse speed, 1..100
  int wheel_speed = 50;                // pad dpad wheel speed, 1..100
  bool pad_vibration = true;           // let guests rumble the pad

  // ---- OS personality reported to guests (games gate on this) ----
  int os_major = 10;
  int os_minor = 0;
  int os_build = 19045;                // Win10 22H2
  bool win11_personality = false;      // 10.0.22000+ (some engines require)

  // ---- compatibility surfaces ----
  bool write_watch = true;             // GetWriteWatch honest impl (GC)
  bool steam_auth = true;              // allow Steam QR link (own account)
  bool fsb_decode = false;             // let FMOD Studio decode FSB5 on console
                                       // (off: heap corruption history, Series S)
  bool crash_report = false;            // opt-in crash reporting (default off)
  bool show_qr = true;                 // QR overlay on the TV dashboard
  int log_lines = 14;                  // dashboard log tail lines, 4..40

  // ---- on-screen overlay (new) ----
  bool show_fps = true;                // FPS counter overlay in-game
  bool show_sysinfo = false;           // CPU/GPU/RAM overlay
  bool quick_menu_enabled = true;      // Shift+Tab opens quick menu

  // ---- performance & tuning ----
  int resolution_scale = 100;          // render resolution %, 50..200
  int frame_rate_cap = 60;             // 0=unlimited, 30, 60, 120
  int memory_budget_mb = 4096;         // guest heap cap, 1024..8192
  bool vsync = true;                   // present with VSync
  bool shader_cache = true;             // cache compiled SPIR-V->HLSL
  int audio_buffer_ms = 20;            // WASAPI buffer, 10..50
  int input_deadzone = 15;             // gamepad deadzone %, 0..50
  int texture_quality = 2;             // 0=low, 1=med, 2=high, 3=ultra

  std::string to_ini() const;
  // Apply parsed ini on top of the current values (missing keys keep
  // defaults). Returns the number of keys applied (0 for an empty file).
  int apply_ini(const std::string& text);
};

// Load from <local_state>\settings.ini (missing file = defaults) and store
// as the live settings. Returns the applied-key count.
int settings_load(const std::string& local_state);
// Apply TIRAMISU_<KEY> environment overrides on top of whatever is loaded
// (for a launcher that needs one knob changed for a single run). Returns the
// number of overrides applied. Values go through the same validation as the
// ini, so a bad env value clamps instead of poisoning the session.
int settings_apply_env();
// Apply the game's profile, then its writable override, over global
// settings. Environment overrides have final precedence. Returns the
// number of profile and environment keys applied.
int settings_load_profile(const std::string& game_dir,
                          const std::string& user_profile_dir = "");
// Write a commented starter profile to <dir>/tiramisu.profile.ini, creating
// <dir> if needed. An existing file is never touched. Returns 1 when written,
// 0 when one was already there, -1 on I/O failure.
int settings_write_profile_stub(const std::string& dir,
                                const std::string& title);
// Which ids in a comma-separated required_services list this host does not
// recognise. Returns the count and fills unknown with them (empty when all
// are known). Pure: used by the host log line and by the offline tests.
int settings_unknown_services(const std::string& list,
                              std::string* unknown = nullptr);
// Persist global defaults and recompute the effective launch settings.
bool settings_store(const host_settings& s, const std::string& local_state);
host_settings settings_get_global();
void settings_clear_profile();
std::string settings_title_profile_dir(const std::string& local_state,
                                      const std::string& source_path);
std::string settings_read_user_profile(const std::string& profile_dir);
bool settings_write_user_profile(const std::string& profile_dir, const std::string& text);
host_settings settings_get();

// ---- consumed by the trt bridge layer ----
// OS version the guest sees (GetVersionExW / GetVersionExW bridge reads it).
void trt_settings_os(int* major, int* minor, int* build);
// Whether the write-watch bridge answers with the real implementation
// (true) or forces the honest fail (false; user disabled GWW).
bool trt_settings_write_watch();
// Whether guest pad-rumble requests reach the pad (false = swallow).
bool trt_settings_pad_vibration();

// ---- overlay getters (new) ----
bool trt_settings_show_fps();
bool trt_settings_show_sysinfo();
bool trt_settings_quick_menu_enabled();

// ---- performance/tuning getters (new) ----
int trt_settings_resolution_scale();   // render resolution %, 50..200
int trt_settings_frame_rate_cap();     // 0=unlimited, 30, 60, 120
int trt_settings_memory_budget_mb();   // guest heap cap MB
bool trt_settings_vsync();
bool trt_settings_fsb_decode();
bool trt_settings_crash_report();
bool trt_settings_shader_cache();
int trt_settings_audio_buffer_ms();
int trt_settings_input_deadzone();
int trt_settings_texture_quality();
// Directory for the persistent SPIR-V->HLSL cache (<LocalState>\cache\shaders).
// C linkage so the bridge can take it as a weak symbol.
extern "C" const char* trt_settings_shader_cache_dir();
// Texture quality tier (0=low .. 3=ultra) for the D3D12 sampler policy. C
// linkage for the same weak-symbol reason as the cache directory above.
extern "C" int trt_settings_texture_quality_int();

} // namespace longan

#endif
