// tiramisu/tests/test_settings.cpp
// Offline gate for the host settings layer: INI round-trip, clamping,
// unknown-key tolerance, and the bridge-facing getters the trt layer calls.
// The trt bridge hooks (trt_GetVersionExW etc.) declare these weak, so the
// real implementations here must be picked up when linked together.
#include "../src/host/settings.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                            \
  do {                                                              \
    if (cond) {                                                     \
      ++g_pass;                                                     \
      printf("  ok: %s\n", (msg));                                  \
    } else {                                                        \
      ++g_fail;                                                     \
      printf("  FAIL: %s (line %d)\n", (msg), __LINE__);            \
    }                                                               \
  } while (0)

static std::string read_file(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return std::string(buf, n);
}

int main() {
  printf("=== Host Settings: persist, clamp, bridge hooks ===\n");

#ifdef _WIN32
  char tmpdir[MAX_PATH];
  GetTempPathA(MAX_PATH, tmpdir);
  const std::string root = std::string(tmpdir) + "trt_settings_test";
  _mkdir(root.c_str());
#else
  const std::string root = "/tmp/opencode/trt_settings_test";
  mkdir(root.c_str(), 0755);
#endif

  // 1. defaults when no file exists
  longan::settings_load(root);
  const auto d = longan::settings_get();
  CHECK(d.auto_launch.empty(), "defaults with no settings.ini has empty auto_launch");
  CHECK(d.mouse_speed == 50 && d.wheel_speed == 50 && d.pad_vibration &&
            d.write_watch && d.os_major == 10 && d.os_build == 19045 &&
            d.default_input == "auto" && d.log_lines == 14,
        "defaults with no settings.ini present");

  // 2. store -> save -> load round-trip
  longan::host_settings s{};
  s.auto_launch = "Sekiro";
  s.default_input = "pad2kbm";
  s.mouse_speed = 88;
  s.wheel_speed = 12;
  s.pad_vibration = false;
  s.os_major = 10;
  s.os_minor = 0;
  s.os_build = 22000;
  s.win11_personality = true;
  s.write_watch = false;
  s.steam_auth = false;
  s.show_qr = false;
  s.log_lines = 30;
  longan::settings_store(s, root);
  CHECK(read_file(root + "/settings.ini").find("mouse_speed=88") !=
            std::string::npos,
        "settings.ini written");
  longan::settings_load(root);
  const auto r = longan::settings_get();
  CHECK(r.auto_launch == "Sekiro", "round-trip restores auto_launch");
  CHECK(r.default_input == "pad2kbm" && r.mouse_speed == 88 &&
            r.wheel_speed == 12 && !r.pad_vibration && !r.write_watch &&
            !r.steam_auth && !r.show_qr && r.log_lines == 30 &&
            r.win11_personality,
        "round-trip restores every field");

  // 3. bridge getters see the same values
  int mj = 0, mn = 0, bd = 0;
  longan::trt_settings_os(&mj, &mn, &bd);
  CHECK(mj == 10 && mn == 0 && bd == 22000,
        "trt_settings_os reports stored (Win11) build");
  CHECK(longan::trt_settings_write_watch() == false,
        "trt_settings_write_watch reports stored false");
  CHECK(longan::trt_settings_pad_vibration() == false,
        "trt_settings_pad_vibration reports stored false");

  // 4. partial INI: unknown keys ignored, missing keys keep defaults
  {
    FILE* f = fopen((root + "/settings.ini").c_str(), "wb");
    fputs("# comment\nmouse_speed=33\nfuture_key=whatever\nwheel_speed=66\n",
          f);
    fclose(f);
  }
  const int applied = longan::settings_load(root);
  CHECK(applied >= 2, "partial ini applies present keys");
  CHECK(longan::settings_get().mouse_speed == 33 &&
            longan::settings_get().wheel_speed == 66,
        "partial ini values applied");
  CHECK(longan::settings_get().default_input == "pad2kbm",
        "missing keys keep previously-loaded values");

  // 5. clamping on apply and on store
  longan::host_settings wild{};
  wild.mouse_speed = -5;
  wild.wheel_speed = 5000;
  wild.log_lines = 9999;
  wild.os_major = 99;
  wild.default_input = "bogus";
  longan::settings_store(wild, root);
  const auto c = longan::settings_get();
  CHECK(c.mouse_speed == 1 && c.wheel_speed == 100 && c.log_lines == 40 &&
            c.os_major == 12 && c.default_input == "auto",
        "store clamps out-of-range values and bad choices");

  // 6. garbage ini never fatal
  {
    FILE* f = fopen((root + "/settings.ini").c_str(), "wb");
    fputs("===\nnoequals\n=\nmouse_speed=20\n", f);
    fclose(f);
  }
  longan::settings_load(root);
  CHECK(longan::settings_get().mouse_speed == 20,
        "garbage lines skipped, valid line still applied");

#ifdef _WIN32
  RemoveDirectoryA(root.c_str());
#else
  (void)system(("rm -rf " + root).c_str());
#endif

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
