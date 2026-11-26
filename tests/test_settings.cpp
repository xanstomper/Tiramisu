// tiramisu/tests/test_settings.cpp
// Offline gate for the host settings layer: INI round-trip, clamping,
// unknown-key tolerance, and the bridge-facing getters the trt layer calls.
// The trt bridge hooks (trt_GetVersionExW etc.) declare these weak, so the
// real implementations here must be picked up when linked together.
#include "../src/host/settings.h"
#include "../src/trt/quality_policy.h"

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
  CHECK(d.mouse_speed == 50 && d.wheel_speed == 50 && d.pad_vibration &&
            d.write_watch && d.os_major == 10 && d.os_build == 19045 &&
            d.default_input == "auto" && d.log_lines == 14,
        "defaults with no settings.ini present");

  // 2. store -> save -> load round-trip
  longan::host_settings s{};
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
  CHECK(applied == 2, "partial ini counts recognized keys only");
  CHECK(longan::settings_get().mouse_speed == 33 &&
            longan::settings_get().wheel_speed == 66,
        "partial ini values applied");
  CHECK(longan::settings_get().default_input == "auto",
        "missing keys restore built-in defaults");

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

  // 6. per-title launch args survive the global file round-trip
  {
    longan::host_settings a{};
    a.launch_args = "-windowed -novsync";
    longan::settings_store(a, root);
    longan::settings_load(root);
    CHECK(longan::settings_get().launch_args == "-windowed -novsync",
          "launch_args round-trips through settings.ini");
    CHECK(read_file(root + "/settings.ini").find("launch_args=-windowed") !=
              std::string::npos,
          "launch_args is written out for the dashboard to edit");
  }

  // 7. per-title profile: writable overrides and launch isolation
  {
    const std::string game = root + "/game";
    const std::string titles = root + "/titles";
    // the stub writer creates the directories for us (portable)
    CHECK(longan::settings_write_profile_stub(titles, "demo") == 1,
          "starter profile written to the per-title store");
    {
      FILE* f = fopen((titles + "/tiramisu.profile.ini").c_str(), "wb");
      fputs("launch_args=-d3d11\nframe_rate_cap=30\n", f);
      fclose(f);
    }
    const int n = longan::settings_load_profile(game, titles);
    CHECK(n >= 2 && longan::settings_get().launch_args == "-d3d11" &&
              longan::settings_get().frame_rate_cap == 30,
          "profile in the per-title store overrides global settings");
    // A shipped profile may supply defaults for a read-only game folder.
    CHECK(longan::settings_write_profile_stub(game, "game") == 1,
          "starter profile written beside the exe");
    CHECK(longan::settings_write_profile_stub(titles, "demo") == 0,
          "an existing per-title profile is never overwritten");
    {
      FILE* f = fopen((game + "/tiramisu.profile.ini").c_str(), "wb");
      fputs("launch_args=\"-dx12 -fullscreen\"\ntexture_quality=1\n", f);
      fclose(f);
    }
    longan::settings_load_profile(game, titles);
    CHECK(longan::settings_get().launch_args == "-d3d11" &&
              longan::settings_get().texture_quality == 1,
          "user profile overrides the shipped launch arguments");
    auto saved = longan::settings_get_global();
    CHECK(saved.frame_rate_cap != 30 && saved.launch_args == "-windowed -novsync",
          "global settings editor excludes the active profile");
    const auto expected_cap = saved.frame_rate_cap;
    saved.audio_buffer_ms = 1000;
    CHECK(longan::settings_store(saved, root) &&
              longan::settings_get().frame_rate_cap == 30 &&
              longan::settings_get().audio_buffer_ms == 50,
          "saving defaults validates values and keeps the current profile");
    longan::settings_clear_profile();
    CHECK(longan::settings_get().frame_rate_cap == expected_cap &&
              longan::settings_get().launch_args == "-windowed -novsync",
          "clearing a profile restores the saved launch defaults");
    const auto left = longan::settings_title_profile_dir(root, "games/A/demo");
    const auto right = longan::settings_title_profile_dir(root, "games/B/demo");
    CHECK(left != right && longan::settings_write_user_profile(left, "mouse_speed=4\n") &&
              longan::settings_read_user_profile(right).empty(),
          "same-named games have separate user profiles");
    CHECK(!longan::settings_store(saved, root + "/missing") &&
              longan::settings_get_global().audio_buffer_ms == 50,
          "failed save leaves the last successfully saved settings active");
  }

  // 8. garbage ini never fatal
  {
    FILE* f = fopen((root + "/settings.ini").c_str(), "wb");
    fputs("===\nnoequals\n=\nmouse_speed=20\n", f);
    fclose(f);
  }
  longan::settings_load(root);
  CHECK(longan::settings_get().mouse_speed == 20,
        "garbage lines skipped, valid line still applied");

  // 9. required_services: persisted, and a typo surfaced instead of ignored
  {
    longan::host_settings s = longan::settings_get();
    s.required_services = "xinput,wasapi,steam";
    longan::settings_store(s, root);
    std::string ini;
    if (FILE* f = fopen((root + "/settings.ini").c_str(), "rb")) {
      char buf[1024];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) ini.append(buf, n);
      fclose(f);
    }
    CHECK(ini.find("required_services=xinput,wasapi,steam") != std::string::npos,
          "required_services is written out for the dashboard to edit");
    CHECK(longan::settings_unknown_services("xinput,steam") == 0,
          "known required services produce no warning");
    std::string unknown;
    CHECK(longan::settings_unknown_services("xinput,gamepass", &unknown) == 1 &&
              unknown == "gamepass",
          "an unrecognised required service is reported by name");
    CHECK(longan::settings_unknown_services("", &unknown) == 0 && unknown.empty(),
          "an empty service list is not a warning");
  }

  // 10. environment overrides beat the file and clamp like the ini does
  {
    FILE* f = fopen((root + "/settings.ini").c_str(), "wb");
    fputs("frame_rate_cap=45\nlaunch_args=--from-file\n", f);
    fclose(f);
    setenv("TIRAMISU_FRAME_RATE_CAP", "30", 1);
    setenv("TIRAMISU_MOUSE_SPEED", "9999", 1);
    setenv("TIRAMISU_LAUNCH_ARGS", "-windowed", 1);
    const int applied = longan::settings_load(root);
    CHECK(applied >= 3 && longan::settings_get().frame_rate_cap == 30,
          "env override beats settings.ini");
    CHECK(longan::settings_get().mouse_speed == 100,
          "a bad env value clamps instead of poisoning the session");
    CHECK(longan::settings_get().launch_args == "-windowed",
          "env override supplies launch arguments");
    unsetenv("TIRAMISU_FRAME_RATE_CAP");
    unsetenv("TIRAMISU_MOUSE_SPEED");
    unsetenv("TIRAMISU_LAUNCH_ARGS");
    longan::settings_load(root);
    CHECK(longan::settings_get().frame_rate_cap == 45 &&
              longan::settings_get().launch_args == "--from-file",
          "clearing the env override restores the file values");
  }

  // ---- 11. shader cache directory handed to the bridge -------------------
  // The bridge takes this through a weak symbol; if it hands back nothing the
  // persistent cache silently never engages and every launch re-translates.
  {
    printf("11. shader cache directory\n");
    longan::settings_load(root);
    const std::string cache_dir = longan::trt_settings_shader_cache_dir();
    CHECK(!cache_dir.empty(), "cache directory is reported to the bridge");
    CHECK(cache_dir.find(root) == 0, "cache directory lives under LocalState");
    CHECK(cache_dir.find("shaders") != std::string::npos,
          "cache directory is the shader cache");
    CHECK(cache_dir.find("settings.ini") == std::string::npos,
          "cache directory is a directory, not the settings file");
  }

  // ---- 12. texture quality policy ---------------------------------------
  // texture_quality had no consumer; the D3D12 sampler record is where it now
  // acts, so the mapping must be monotonic, bounded, and a no-op by default.
  {
    printf("12. texture quality policy\n");
    CHECK(trt_quality_aniso_cap(0) == 1, "low tier caps anisotropy at 1");
    CHECK(trt_quality_aniso_cap(1) == 4, "medium tier caps anisotropy at 4");
    CHECK(trt_quality_aniso_cap(2) == 16, "high (default) allows the hardware max");
    CHECK(trt_quality_aniso_cap(3) == 16, "ultra is the hardware max");
    CHECK(trt_quality_aniso_cap(9) == 16, "out-of-range tier behaves as default");
    CHECK(trt_quality_clamp_aniso(2, 16) == 16, "default leaves 16x alone");
    CHECK(trt_quality_clamp_aniso(1, 16) == 4, "medium lowers 16x to 4x");
    CHECK(trt_quality_clamp_aniso(0, 8) == 1, "low lowers 8x to 1x");
    CHECK(trt_quality_clamp_aniso(3, 2) == 2, "a tier never raises the guest request");
    CHECK(trt_quality_clamp_aniso(2, 0) == 1, "zero anisotropy becomes 1");
    CHECK(trt_quality_clamp_aniso(2, 99) == 16, "invalid anisotropy falls back to the cap");
    CHECK(trt_quality_aniso_clamped(1, 16), "a real clamp is reported");
    CHECK(!trt_quality_aniso_clamped(2, 16), "the default tier reports no clamp");
    longan::settings_load(root);
    CHECK(longan::trt_settings_texture_quality_int() == 2,
          "bridge getter reports the live quality tier");
    longan::host_settings sq = longan::settings_get();
    sq.texture_quality = 0;
    longan::settings_store(sq, root);
    CHECK(longan::trt_settings_texture_quality_int() == 0,
          "changing the tier reaches the bridge getter");
    sq.texture_quality = 2;
    longan::settings_store(sq, root);
  }

#ifdef _WIN32
  RemoveDirectoryA(root.c_str());
#else
  (void)system(("rm -rf " + root).c_str());
#endif

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
