// tiramisu/tests/test_console_profile.cpp
// Offline gate for the last-generation console profile table and the D3D11
// feature-level clamp that swapchain_create() consumes.
//
// No One/One S/One X hardware is reachable from this gate, so nothing here is
// on-device proof. What IS provable offline is exactly what the host reads:
// family ids, the last-gen flags, RAM/ESRAM sizes, the 5120 MB Dev Mode budget,
// and the levels the device is asked for per family.
#include "../src/host/console_detect.h"
#include "../src/trt/trt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, name)                                               \
  do {                                                                  \
    if (cond) {                                                         \
      ++g_passed;                                                       \
    } else {                                                            \
      ++g_failed;                                                       \
      printf("FAIL: %s (line %d)\n", name, __LINE__);                    \
    }                                                                   \
  } while (0)

int main() {
  using namespace longan;

  struct row {
    xbox_console_generation gen;
    const char* family;
    bool last_gen;
    bool avx2;
    int esram_mb;
    int ram_mb;
    unsigned ceiling;
  };
  static const row rows[] = {
      {xbox_console_generation::xbox_one, "one", true, false, 32, 8192, 0xb000u},
      {xbox_console_generation::xbox_one_s, "one_s", true, false, 32, 8192,
       0xb000u},
      {xbox_console_generation::xbox_one_x, "one_x", true, false, 0, 12288,
       0xb100u},
      {xbox_console_generation::xbox_series_s, "series_s", false, true, 0,
       10240, 0xc000u},
      {xbox_console_generation::xbox_series_x, "series_x", false, true, 0,
       16384, 0xc100u},
  };

  constexpr uint64_t mib = 1024ULL * 1024ULL;
  CHECK(console_guest_memory_budget_mb(5120, 240 * mib, 1024 * mib) == 784,
        "restricted Dev Mode budget subtracts the host working set");
  CHECK(console_guest_memory_budget_mb(5120, 1024 * mib, 1024 * mib) == 1,
        "exhausted app quota clamps to the minimum guest budget");
  CHECK(console_guest_memory_budget_mb(5120, 2048 * mib, 0) == 5120,
        "unknown platform quota preserves the configured budget");
  CHECK(console_guest_memory_budget_mb(1024, 240 * mib, 5120 * mib) == 1024,
        "available memory cannot raise the configured guest budget");

  char msg[160];
  for (const row& r : rows) {
    const console_hardware_info i = get_console_info_for_generation(r.gen);
    snprintf(msg, sizeof(msg), "%s: family_id == \"%s\"", r.family, r.family);
    CHECK(i.family_id && strcmp(i.family_id, r.family) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: is_last_gen == %d", r.family,
             r.last_gen ? 1 : 0);
    CHECK(i.is_last_gen == r.last_gen, msg);
    snprintf(msg, sizeof(msg), "%s: has_avx2 == %d", r.family,
             r.avx2 ? 1 : 0);
    CHECK(i.has_avx2 == r.avx2, msg);
    snprintf(msg, sizeof(msg), "%s: esram_size_mb == %d", r.family,
             r.esram_mb);
    CHECK(i.esram_size_mb == r.esram_mb, msg);
    CHECK(i.has_esram == (r.esram_mb != 0), "has_esram matches esram_size_mb");
    snprintf(msg, sizeof(msg), "%s: total_ram_mb == %d", r.family, r.ram_mb);
    CHECK(i.total_ram_mb == r.ram_mb, msg);
    snprintf(msg, sizeof(msg), "%s: Dev Mode budget is 5120 MB", r.family);
    CHECK(i.dev_mode_game_budget_mb == 5120, msg);
    snprintf(msg, sizeof(msg), "%s: table ceiling 0x%x", r.family,
             r.ceiling);
    CHECK(i.max_d3d_feature_level == r.ceiling, msg);
    snprintf(msg, sizeof(msg), "%s: cpu_cores == 8", r.family);
    CHECK(i.cpu_cores == 8, msg);
  }

  // One (Durango) and One S (Edmonton) are GCN 1.1: D3D11 tops out at 11_0 and
  // the request list must never contain 11_1. This was a data bug for One S
  // (the table claimed 0xb100) and is the exact class of error that only shows
  // up as a wrong feature level on real hardware.
  for (xbox_console_generation gen :
       {xbox_console_generation::xbox_one,
        xbox_console_generation::xbox_one_s}) {
    unsigned lv[8] = {};
    const int n = console_feature_levels(gen, lv, 8);
    CHECK(n >= 3, "One-family requests at least 11_0/10_1/10_0");
    CHECK(n > 0 && lv[0] == 0xb000u, "One-family top level is 11_0");
    bool has_11_1 = false;
    for (int i = 0; i < n; ++i)
      if (lv[i] == 0xb100u) has_11_1 = true;
    CHECK(!has_11_1, "One-family never requests D3D_FEATURE_LEVEL_11_1");
    CHECK(n > 1 && lv[1] == 0xa100u, "One-family falls back to 10_1");
  }

  // One X (Scorpio, GCN 4.0) can ask for 11_1.
  {
    unsigned lv[8] = {};
    const int n = console_feature_levels(xbox_console_generation::xbox_one_x,
                                         lv, 8);
    CHECK(n > 0 && lv[0] == 0xb100u, "One X top level is 11_1");
  }

  // Series reports 12_0/12_1 for the D3D12 path; the D3D11 list clamps to 11_1
  // (D3D11CreateDevice does not understand 0xc000/0xc100).
  for (xbox_console_generation gen :
       {xbox_console_generation::xbox_series_s,
        xbox_console_generation::xbox_series_x,
        xbox_console_generation::pc_desktop}) {
    unsigned lv[8] = {};
    const int n = console_feature_levels(gen, lv, 8);
    CHECK(n > 0 && lv[0] == 0xb100u,
          "Series/PC D3D11 list starts at 11_1 (12_x clamped)");
    bool bad = false;
    for (int i = 0; i < n; ++i)
      if (lv[i] > 0xb100u) bad = true;
    CHECK(!bad, "no D3D12 level leaks into the D3D11 request list");
  }

  // Capacity is respected and the list is never empty.
  {
    unsigned lv[8] = {};
    CHECK(console_feature_levels(xbox_console_generation::xbox_series_x, lv,
                                 2) == 2,
          "console_feature_levels respects a 2-entry buffer");
    CHECK(console_feature_levels(xbox_console_generation::xbox_one, nullptr,
                                 8) == 0,
          "console_feature_levels(NULL) -> 0");
    CHECK(console_feature_levels(xbox_console_generation::xbox_one, lv, 0) == 0,
          "console_feature_levels(cap 0) -> 0");
    // An unrecognised device falls back to the PC profile, so it must still
    // produce a usable list rather than an empty one.
    const int un = console_feature_levels(xbox_console_generation::unknown, lv, 8);
    CHECK(un == 4 && lv[0] == 0xb100u,
          "unknown generation falls back to the full PC list");
  }

  // The environment override is the same path Device Portal tweaks use, and it
  // is what a desktop test can drive. First call wins (result is cached).
  setenv("TIRAMISU_CONSOLE_FAMILY", "one_x", 1);
  const console_hardware_info det = get_detected_console_info();
  CHECK(strcmp(det.family_id, "one_x") == 0,
        "TIRAMISU_CONSOLE_FAMILY=one_x drives detection");
  CHECK(det.is_last_gen && !det.has_avx2,
        "detected One X is last-gen without AVX2");

  // What apply_console_hardware_profile() hands the runtime governor and the
  // ESRAM ledger for a One-family console.
  trt_mem_budget_set_mb((u32)det.dev_mode_game_budget_mb);
  CHECK(trt_mem_budget_get_mb() == 5120u,
        "memory governor takes the 5120 MB Dev Mode budget");
  trt_esram_init(0);
  trt_esram_init(det.has_esram ? (u32)det.esram_size_mb : 0u);
  CHECK(trt_esram_is_active() == 0,
        "One X has no ESRAM ledger (Scorpio is unified GDDR5)");
  const console_hardware_info one_s =
      get_console_info_for_generation(xbox_console_generation::xbox_one_s);
  trt_esram_init((u32)one_s.esram_size_mb);
  u64 used = 0, total = 0, count = 0;
  const int active = trt_esram_is_active();
  trt_esram_get_stats(&used, &total, &count);  // fills out params, no status
  CHECK(active == 1 && total == 32ull * 1024ull * 1024ull && used == 0,
        "One S opens a real 32 MB ESRAM budget ledger");

  printf("RESULT: %s (%d passed, %d failed)\n",
         g_failed == 0 ? "ALL PASS" : "FAILURES", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
