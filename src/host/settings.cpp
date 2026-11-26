// tiramisu/src/host/settings.cpp
#include "settings.h"

#include "../trt/trt.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace longan {

namespace {

std::mutex g_mu;
host_settings g_live;

#ifdef _WIN32
const char k_sep = '\\';
#else
const char k_sep = '/';  // offline-test path (Linux)
#endif

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

} // namespace

std::string host_settings::to_ini() const {
  char buf[640];
  std::string s;
  s += "# Tiramisu host settings — edited from the dashboard (TV or web).\n";
  s += "# Missing keys keep their built-in defaults.\n";
  snprintf(buf, sizeof(buf), "default_input=%s\n", default_input.c_str());
  s += buf;
  if (!auto_launch.empty()) {
    snprintf(buf, sizeof(buf), "auto_launch=%s\n", auto_launch.c_str());
    s += buf;
  }
  snprintf(buf, sizeof(buf), "mouse_speed=%d\nwheel_speed=%d\n",
           mouse_speed, wheel_speed);
  s += buf;
  s += pad_vibration ? "pad_vibration=1\n" : "pad_vibration=0\n";
  snprintf(buf, sizeof(buf), "os_major=%d\nos_minor=%d\nos_build=%d\n",
           os_major, os_minor, os_build);
  s += buf;
  s += win11_personality ? "win11_personality=1\n" : "win11_personality=0\n";
  s += write_watch ? "write_watch=1\n" : "write_watch=0\n";
  s += steam_auth ? "steam_auth=1\n" : "steam_auth=0\n";
  s += show_qr ? "show_qr=1\n" : "show_qr=0\n";
  s += show_fps ? "show_fps=1\n" : "show_fps=0\n";
  s += show_sysinfo ? "show_sysinfo=1\n" : "show_sysinfo=0\n";
  s += quick_menu_enabled ? "quick_menu_enabled=1\n" : "quick_menu_enabled=0\n";
  snprintf(buf, sizeof(buf), "log_lines=%d\n", log_lines);
  s += buf;
  snprintf(buf, sizeof(buf),
           "resolution_scale=%d\nframe_rate_cap=%d\nmemory_budget_mb=%d\n",
           resolution_scale, frame_rate_cap, memory_budget_mb);
  s += buf;
  s += vsync ? "vsync=1\n" : "vsync=0\n";
  s += shader_cache ? "shader_cache=1\n" : "shader_cache=0\n";
  snprintf(buf, sizeof(buf),
           "audio_buffer_ms=%d\ninput_deadzone=%d\ntexture_quality=%d\n",
           audio_buffer_ms, input_deadzone, texture_quality);
  s += buf;
  return s;
}

int host_settings::apply_ini(const std::string& text) {
  int applied = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key.empty()) continue;
    ++applied;
    if (key == "default_input") {
      if (val == "auto" || val == "controller" || val == "kbm" ||
          val == "pad2kbm")
        default_input = val;
    } else if (key == "auto_launch") {
      auto_launch = val;
    } else if (key == "mouse_speed") {
      mouse_speed = clampi(atoi(val.c_str()), 1, 100);
    } else if (key == "wheel_speed") {
      wheel_speed = clampi(atoi(val.c_str()), 1, 100);
    } else if (key == "pad_vibration") {
      pad_vibration = (val == "1" || val == "true");
    } else if (key == "os_major") {
      os_major = clampi(atoi(val.c_str()), 5, 12);
    } else if (key == "os_minor") {
      os_minor = clampi(atoi(val.c_str()), 0, 99);
    } else if (key == "os_build") {
      os_build = clampi(atoi(val.c_str()), 0, 99999);
    } else if (key == "win11_personality") {
      win11_personality = (val == "1" || val == "true");
    } else if (key == "write_watch") {
      write_watch = (val == "1" || val == "true");
    } else if (key == "steam_auth") {
      steam_auth = (val == "1" || val == "true");
    } else if (key == "show_qr") {
      show_qr = (val == "1" || val == "true");
    } else if (key == "show_fps") {
      show_fps = (val == "1" || val == "true");
    } else if (key == "show_sysinfo") {
      show_sysinfo = (val == "1" || val == "true");
    } else if (key == "quick_menu_enabled") {
      quick_menu_enabled = (val == "1" || val == "true");
    } else if (key == "log_lines") {
      log_lines = clampi(atoi(val.c_str()), 4, 40);
    } else if (key == "resolution_scale") {
      resolution_scale = clampi(atoi(val.c_str()), 50, 200);
    } else if (key == "frame_rate_cap") {
      frame_rate_cap = clampi(atoi(val.c_str()), 0, 120);
    } else if (key == "memory_budget_mb") {
      memory_budget_mb = clampi(atoi(val.c_str()), 1024, 8192);
    } else if (key == "vsync") {
      vsync = (val == "1" || val == "true");
    } else if (key == "shader_cache") {
      shader_cache = (val == "1" || val == "true");
    } else if (key == "audio_buffer_ms") {
      audio_buffer_ms = clampi(atoi(val.c_str()), 10, 50);
    } else if (key == "input_deadzone") {
      input_deadzone = clampi(atoi(val.c_str()), 0, 50);
    } else if (key == "texture_quality") {
      texture_quality = clampi(atoi(val.c_str()), 0, 3);
    }
    // unknown keys are kept in the file but ignored — never fatal
  }
  return applied;
}

int settings_load(const std::string& local_state) {
  FILE* f = fopen((local_state + k_sep + "settings.ini").c_str(), "rb");
  if (!f) return 0;
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  std::lock_guard<std::mutex> lk(g_mu);
  const int applied = g_live.apply_ini(text);
  return applied;
}

int settings_save(const std::string& local_state) {
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string text = g_live.to_ini();
  FILE* f = fopen((local_state + k_sep + "settings.ini").c_str(), "wb");
  if (!f) return -1;
  const size_t w = fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return (w == text.size()) ? 0 : -1;
}

int settings_load_profile(const std::string& game_dir) {
  std::string path = game_dir;
  // normalize separators
  for (char& c : path) if (c == '/') c = '\\';
  if (!path.empty() && path.back() != '\\') path += '\\';
  path += "tiramisu.profile.ini";

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);

  std::lock_guard<std::mutex> lk(g_mu);
  const int applied = g_live.apply_ini(text);
  return applied;
}

void settings_store(const host_settings& s, const std::string& local_state) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    host_settings c = s;  // clamp the same way apply_ini does
    c.mouse_speed = clampi(c.mouse_speed, 1, 100);
    c.wheel_speed = clampi(c.wheel_speed, 1, 100);
    c.os_major = clampi(c.os_major, 5, 12);
    c.os_minor = clampi(c.os_minor, 0, 99);
    c.os_build = clampi(c.os_build, 0, 99999);
    c.log_lines = clampi(c.log_lines, 4, 40);
    if (c.default_input != "auto" && c.default_input != "controller" &&
        c.default_input != "kbm" && c.default_input != "pad2kbm")
      c.default_input = "auto";
    g_live = c;
  }
  settings_save(local_state);
}

host_settings settings_get() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live;  // by value: callers must not race a concurrent settings_store
}

// ---- bridge-facing getters (lock-free snapshot of the ints that matter) ----
void trt_settings_os(int* major, int* minor, int* build) {
  std::lock_guard<std::mutex> lk(g_mu);
  int mj = g_live.os_major, mn = g_live.os_minor, bd = g_live.os_build;
  if (g_live.win11_personality && bd < 22000) bd = 22000;
  if (major) *major = mj;
  if (minor) *minor = mn;
  if (build) *build = bd;
}

bool trt_settings_write_watch() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.write_watch;
}

bool trt_settings_pad_vibration() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.pad_vibration;
}

bool trt_settings_show_fps() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.show_fps;
}

bool trt_settings_show_sysinfo() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.show_sysinfo;
}

bool trt_settings_quick_menu_enabled() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.quick_menu_enabled;
}

// Weak-linked wrapper consumed by bridge_vulkan's shader cache
// (bridge_vulkan.cpp calls this through a weak symbol; offline test binaries
// without this file resolve it to null and keep the cache enabled).
extern "C" int trt_settings_shader_cache_int() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.shader_cache ? 1 : 0;
}

int trt_settings_resolution_scale() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.resolution_scale;
}

int trt_settings_frame_rate_cap() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.frame_rate_cap;
}

int trt_settings_memory_budget_mb() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.memory_budget_mb;
}

bool trt_settings_vsync() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.vsync;
}

bool trt_settings_shader_cache() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.shader_cache;
}

int trt_settings_audio_buffer_ms() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.audio_buffer_ms;
}

int trt_settings_input_deadzone() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.input_deadzone;
}

int trt_settings_texture_quality() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.texture_quality;
}

} // namespace longan
