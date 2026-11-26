// tiramisu/src/host/console_detect.cpp
// Console Generation & Hardware Auto-Detection Engine.
#include "console_detect.h"
#include "settings.h"
#include "host_log.h"
#include "../trt/trt.h"
#include "../trt/thunk.h"
#include "../trt/bridge_esram.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#include <winrt/base.h>
#include <winrt/windows.security.exchangeactivesyncprovisioning.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Storage.h>
#else
#include <sys/sysinfo.h>
#endif

namespace longan {

namespace {

std::mutex g_detect_mu;
console_hardware_info g_active_info{};
bool g_active_info_initialized = false;

// CPU feature detection via CPUID
bool cpu_has_avx2() {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
  int info[4]{0};
  __cpuidex(info, 7, 0);
  return (static_cast<unsigned int>(info[1]) & (1u << 5)) != 0;
#else
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  __asm__ __volatile__("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(7), "c"(0));
  return (ebx & (1u << 5)) != 0; // AVX2 is bit 5 of EBX in leaf 7 sub-leaf 0
#endif
#else
  return false;
#endif
}

bool running_on_xbox() {
#ifdef _WIN32
  try {
    using namespace winrt::Windows::System::Profile;
    auto family = AnalyticsInfo::VersionInfo().DeviceFamily();
    return std::wstring(family.c_str()) == L"Windows.Xbox";
  } catch (...) {
    return false;
  }
#else
  return false;
#endif
}

int cpu_amd_family() {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
  int info[4]{0};
  __cpuidex(info, 1, 0);
  unsigned int eax = static_cast<unsigned int>(info[0]);
#else
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  __asm__ __volatile__("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(1), "c"(0));
#endif
  unsigned int base_family = (eax >> 8) & 0xFu;
  unsigned int ext_family = (eax >> 20) & 0xFFu;
  if (base_family == 0xFu) {
    return static_cast<int>(base_family + ext_family);
  }
  return static_cast<int>(base_family);
#else
  return 0;
#endif
}

uint64_t get_total_physical_ram_bytes() {
#ifdef _WIN32
  MEMORYSTATUSEX ms{sizeof(MEMORYSTATUSEX)};
  if (GlobalMemoryStatusEx(&ms)) {
    return ms.ullTotalPhys;
  }
  return 8589934592ULL; // 8 GB fallback
#else
  struct sysinfo si{};
  if (sysinfo(&si) == 0) {
    return static_cast<uint64_t>(si.totalram) * static_cast<uint64_t>(si.mem_unit);
  }
  return 8589934592ULL;
#endif
}

std::string lower_strip(const std::string& s) {
  std::string res;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ') {
      res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return res;
}

// Simple JSON tweak finder for LocalState
[[maybe_unused]] static std::string check_localstate_tweak(const std::string& local_state) {
  if (local_state.empty()) return "";
  const char* candidates[] = {
      "\\console_tweak_one.json",
      "\\console_tweak_one_s.json",
      "\\console_tweak_one_x.json",
      "\\console_tweak_series_s.json",
      "\\console_tweak_series_x.json",
      "\\console_tweak.json",
      "/console_tweak_one.json",
      "/console_tweak_one_s.json",
      "/console_tweak_one_x.json",
      "/console_tweak_series_s.json",
      "/console_tweak_series_x.json",
      "/console_tweak.json"
  };
  for (const char* sub : candidates) {
    std::string p = local_state + sub;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) continue;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* key = "\"family\":";
    char* pos = strstr(buf, key);
    if (pos) {
      char* q1 = strchr(pos + strlen(key), '"');
      if (q1) {
        char* q2 = strchr(q1 + 1, '"');
        if (q2) {
          return std::string(q1 + 1, q2 - (q1 + 1));
        }
      }
    }
  }
  return "";
}

} // namespace

xbox_console_generation parse_console_generation_string(const char* str) {
  if (!str) return xbox_console_generation::unknown;
  std::string s = lower_strip(str);

  if (s.find("series x") != std::string::npos || s == "series_x" || s == "anaconda") {
    return xbox_console_generation::xbox_series_x;
  }
  if (s.find("series s") != std::string::npos || s == "series_s" || s == "lockhart") {
    return xbox_console_generation::xbox_series_s;
  }
  if (s.find("one x") != std::string::npos || s == "one_x" || s.find("scorpio") != std::string::npos) {
    return xbox_console_generation::xbox_one_x;
  }
  if (s.find("one s") != std::string::npos || s == "one_s" || s.find("edmonton") != std::string::npos) {
    return xbox_console_generation::xbox_one_s;
  }
  if (s.find("one") != std::string::npos || s == "one" || s == "xbox_one" || s.find("durango") != std::string::npos) {
    return xbox_console_generation::xbox_one;
  }
  if (s.find("pc") != std::string::npos || s.find("desktop") != std::string::npos || s.find("generic") != std::string::npos) {
    return xbox_console_generation::pc_desktop;
  }
  return xbox_console_generation::unknown;
}

console_hardware_info get_console_info_for_generation(xbox_console_generation gen) {
  console_hardware_info info{};
  info.generation = gen;

  switch (gen) {
    case xbox_console_generation::xbox_one:
      info.generation_name = "Xbox One";
      info.family_id = "one";
      info.codename = "Durango";
      info.cpu_arch = "AMD Jaguar";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 1.75f;
      info.has_avx2 = false;
      info.gpu_arch = "GCN 1.1";
      info.gpu_compute_units = 12;
      info.gpu_tflops = 1.31f;
      info.max_d3d_feature_level = 0xb000; // D3D_FEATURE_LEVEL_11_0
      info.has_esram = true;
      info.esram_size_mb = 32;
      info.total_ram_mb = 8192;
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 80;
      info.recommended_fps_cap = 30;
      info.recommended_tex_quality = 1;
      info.d3d12_allowed = false;
      info.requires_conservative_input = true;
      info.is_last_gen = true;
      break;

    case xbox_console_generation::xbox_one_s:
      info.generation_name = "Xbox One S";
      info.family_id = "one_s";
      info.codename = "Edmonton";
      info.cpu_arch = "AMD Jaguar";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 1.75f;
      info.has_avx2 = false;
      info.gpu_arch = "GCN 1.1 (HDR)";
      info.gpu_compute_units = 12;
      info.gpu_tflops = 1.40f;
      // Edmonton is the same GCN 1.1 part as Durango with a higher GPU clock;
      // 11_1 needs GCN 2+ tiled-resource caps, so this was a data bug.
      info.max_d3d_feature_level = 0xb000; // D3D_FEATURE_LEVEL_11_0
      info.has_esram = true;
      info.esram_size_mb = 32;
      info.total_ram_mb = 8192;
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 85;
      info.recommended_fps_cap = 60;
      info.recommended_tex_quality = 1;
      info.d3d12_allowed = false;
      info.requires_conservative_input = true;
      info.is_last_gen = true;
      break;

    case xbox_console_generation::xbox_one_x:
      info.generation_name = "Xbox One X";
      info.family_id = "one_x";
      info.codename = "Scorpio";
      info.cpu_arch = "AMD Jaguar Evolved";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 2.30f;
      info.has_avx2 = false;
      info.gpu_arch = "GCN 4.0 (Polaris)";
      info.gpu_compute_units = 40;
      info.gpu_tflops = 6.00f;
      info.max_d3d_feature_level = 0xb100; // D3D_FEATURE_LEVEL_11_1
      info.has_esram = false;
      info.esram_size_mb = 0;
      info.total_ram_mb = 12288;
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 100;
      info.recommended_fps_cap = 60;
      info.recommended_tex_quality = 2;
      info.d3d12_allowed = true; // experimental
      info.requires_conservative_input = false;
      info.is_last_gen = true;
      break;

    case xbox_console_generation::xbox_series_s:
      info.generation_name = "Xbox Series S";
      info.family_id = "series_s";
      info.codename = "Lockhart";
      info.cpu_arch = "AMD Zen 2";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 3.60f;
      info.has_avx2 = true;
      info.gpu_arch = "RDNA 2";
      info.gpu_compute_units = 20;
      info.gpu_tflops = 4.00f;
      info.max_d3d_feature_level = 0xc000; // D3D_FEATURE_LEVEL_12_0
      info.has_esram = false;
      info.esram_size_mb = 0;
      info.total_ram_mb = 10240;
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 100;
      info.recommended_fps_cap = 60;
      info.recommended_tex_quality = 2;
      info.d3d12_allowed = true;
      info.requires_conservative_input = false;
      info.is_last_gen = false;
      break;

    case xbox_console_generation::xbox_series_x:
      info.generation_name = "Xbox Series X";
      info.family_id = "series_x";
      info.codename = "Anaconda";
      info.cpu_arch = "AMD Zen 2";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 3.80f;
      info.has_avx2 = true;
      info.gpu_arch = "RDNA 2";
      info.gpu_compute_units = 52;
      info.gpu_tflops = 12.15f;
      info.max_d3d_feature_level = 0xc100; // D3D_FEATURE_LEVEL_12_1
      info.has_esram = false;
      info.esram_size_mb = 0;
      info.total_ram_mb = 16384;
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 100;
      info.recommended_fps_cap = 60;
      info.recommended_tex_quality = 3;
      info.d3d12_allowed = true;
      info.requires_conservative_input = false;
      info.is_last_gen = false;
      break;

    case xbox_console_generation::pc_desktop:
    default:
      info.generation_name = "PC / Host Environment";
      info.family_id = "pc";
      info.codename = "Generic";
      info.cpu_arch = "x86_64";
      info.cpu_cores = 8;
      info.cpu_clock_ghz = 3.00f;
      info.has_avx2 = cpu_has_avx2();
      info.gpu_arch = "Host GPU";
      info.gpu_compute_units = 16;
      info.gpu_tflops = 4.00f;
      info.max_d3d_feature_level = 0xb100;
      info.has_esram = false;
      info.esram_size_mb = 0;
      info.total_ram_mb = static_cast<int>(get_total_physical_ram_bytes() / (1024ULL * 1024ULL));
      info.dev_mode_game_budget_mb = 5120;
      info.recommended_res_scale = 100;
      info.recommended_fps_cap = 60;
      info.recommended_tex_quality = 2;
      info.d3d12_allowed = true;
      info.requires_conservative_input = false;
      info.is_last_gen = false;
      break;
  }
  return info;
}

namespace {

xbox_console_generation detect_console_generation_impl() {
  // Caller holds g_detect_mu (or is the one-time initializer).
  // 1. Check environment variable override
  const char* env_gen = getenv("TIRAMISU_CONSOLE_FAMILY");
  if (env_gen && *env_gen) {
    xbox_console_generation g = parse_console_generation_string(env_gen);
    if (g != xbox_console_generation::unknown) return g;
  }

  // 2. Check Device Portal tweak file in LocalState if available
#ifdef _WIN32
  try {
    using namespace winrt::Windows::Storage;
    std::wstring ws = ApplicationData::Current().LocalFolder().Path().c_str();
    std::string ls(ws.begin(), ws.end());
    std::string tw = check_localstate_tweak(ls);
    if (!tw.empty()) {
      xbox_console_generation g = parse_console_generation_string(tw.c_str());
      if (g != xbox_console_generation::unknown) return g;
    }
  } catch (...) {}
#endif

  // 3. Hardware fingerprinting via CPUID and physical RAM size
  const bool has_avx2 = cpu_has_avx2();
  const int amd_fam = cpu_amd_family();
  const uint64_t ram_bytes = get_total_physical_ram_bytes();
  const uint64_t ram_mb = ram_bytes / (1024ULL * 1024ULL);

#ifdef _WIN32
  // Check WinRT EasClientDeviceInformation for explicit product string
  try {
    using namespace winrt::Windows::Security::ExchangeActiveSyncProvisioning;
    EasClientDeviceInformation devInfo;
    std::wstring prod_ws = devInfo.SystemProductName().c_str();
    std::string prod_str(prod_ws.begin(), prod_ws.end());
    xbox_console_generation g = parse_console_generation_string(prod_str.c_str());
    if (g != xbox_console_generation::unknown) return g;

    std::wstring sku_ws = devInfo.SystemSku().c_str();
    std::string sku_str(sku_ws.begin(), sku_ws.end());
    if (sku_str.find("1681") != std::string::npos) return xbox_console_generation::xbox_one_s;
    if (sku_str.find("1787") != std::string::npos) return xbox_console_generation::xbox_one_x;
    if (sku_str.find("1540") != std::string::npos) return xbox_console_generation::xbox_one;
    if (sku_str.find("1882") != std::string::npos) return xbox_console_generation::xbox_series_x;
    if (sku_str.find("1883") != std::string::npos) return xbox_console_generation::xbox_series_s;
  } catch (...) {}
#endif

  // CPUID/RAM fingerprinting is only meaningful inside the Xbox device family.
  // Without this gate, a non-AVX2 desktop/laptop would look like an Xbox One S.
  if (!running_on_xbox()) return xbox_console_generation::pc_desktop;

  // Best-effort heuristic: AMD Jaguar (Family 0x16 or no AVX2) -> One family.
  if (amd_fam == 0x16 || !has_avx2) {
    if (ram_mb > 10500) {
      return xbox_console_generation::xbox_one_x; // 12 GB GDDR5
    }
    // Differentiate Xbox One S vs Original Xbox One (Durango)
    // One S typically has slightly higher clock or reports HDR capability
    return xbox_console_generation::xbox_one_s;
  }

  // If CPU is AMD Zen 2 (Family 0x17 with AVX2) -> Xbox Series family!
  if (has_avx2 && (amd_fam == 0x17 || amd_fam == 0x19)) {
    if (ram_mb > 13000) {
      return xbox_console_generation::xbox_series_x; // 16 GB GDDR6
    }
    return xbox_console_generation::xbox_series_s;   // 10 GB GDDR6
  }

  return xbox_console_generation::pc_desktop;
}

} // namespace

xbox_console_generation detect_console_generation() {
  std::lock_guard<std::mutex> lk(g_detect_mu);
  return detect_console_generation_impl();
}

console_hardware_info get_detected_console_info() {
  std::lock_guard<std::mutex> lk(g_detect_mu);
  if (!g_active_info_initialized) {
    xbox_console_generation gen = detect_console_generation_impl();
    g_active_info = get_console_info_for_generation(gen);
    g_active_info_initialized = true;
  }
  return g_active_info;
}

unsigned console_guest_memory_budget_mb(unsigned configured_mb, uint64_t app_usage,
                                        uint64_t app_limit) {
  if (!app_limit) return configured_mb;
  constexpr uint64_t mib = 1024ULL * 1024ULL;
  const uint64_t headroom_mb = app_usage < app_limit ? (app_limit - app_usage) / mib : 0;
  if (!headroom_mb) return 1;  // Minimum supported by the guest memory governor.
  return headroom_mb < configured_mb ? static_cast<unsigned>(headroom_mb) : configured_mb;
}

void apply_console_hardware_profile(const console_hardware_info& info, const std::string& local_state) {
  char line[256];
  snprintf(line, sizeof(line),
           "[console_detect] Target hardware: %s (%s, %s), CPU: %s %d-core %.2f GHz (AVX2=%d), GPU: %s %.2f TF, RAM: %d MB (ESRAM=%d MB)",
           info.generation_name, info.family_id, info.codename,
           info.cpu_arch, info.cpu_cores, info.cpu_clock_ghz, info.has_avx2 ? 1 : 0,
           info.gpu_arch, info.gpu_tflops, info.total_ram_mb, info.has_esram ? info.esram_size_mb : 0);
  trt_core_log(line);

  // 1. Enforce memory budget on the runtime governor
  trt_mem_budget_set_mb(static_cast<uint32_t>(info.dev_mode_game_budget_mb));
  snprintf(line, sizeof(line),
           "[console_detect] Configured memory governor: %d MB budget for %s (prevents platform OOM kill)",
           info.dev_mode_game_budget_mb, info.generation_name);
  trt_core_log(line);

  // 2. Initialize the ESRAM budget ledger. This is accounting only: UWP
  // D3D11 does not expose explicit ESRAM allocation, so the driver still
  // decides actual residency.
  trt_esram_init(info.has_esram ? static_cast<uint32_t>(info.esram_size_mb) : 0);
  if (info.has_esram) {
    snprintf(line, sizeof(line),
             "[console_detect] ESRAM budget ledger active: 32 MB tracked for %s (driver owns residency)",
             info.generation_name);
    trt_core_log(line);
  }

  // 3. Update host settings defaults if settings.ini has not explicitly overridden them
  host_settings cur = settings_get();
  bool modified = false;

  if (cur.memory_budget_mb > info.dev_mode_game_budget_mb) {
    cur.memory_budget_mb = info.dev_mode_game_budget_mb;
    modified = true;
  }
  if (info.is_last_gen &&
      (cur.frame_rate_cap == 0 || cur.frame_rate_cap > info.recommended_fps_cap)) {
    cur.frame_rate_cap = info.recommended_fps_cap;
    modified = true;
  }
  if (info.is_last_gen && cur.texture_quality > info.recommended_tex_quality) {
    cur.texture_quality = info.recommended_tex_quality;
    modified = true;
  }
  if (cur.resolution_scale > info.recommended_res_scale) {
    cur.resolution_scale = info.recommended_res_scale;
    modified = true;
  }
  if (modified) {
    settings_store(cur, local_state);
  }
}

int console_feature_levels(xbox_console_generation gen, unsigned* out,
                           int cap) {
  if (!out || cap <= 0) return 0;
  const console_hardware_info info = get_console_info_for_generation(gen);
  // D3D11CreateDevice understands 11_1..9_1 only. The Series family reports
  // 12_0/12_1 for the D3D12 path; that is not a D3D11 level.
  unsigned ceiling = info.max_d3d_feature_level;
  constexpr unsigned k_d3d11_ceiling = 0xb100u;  // D3D_FEATURE_LEVEL_11_1
  if (ceiling > k_d3d11_ceiling) ceiling = k_d3d11_ceiling;
  static const unsigned k_levels[] = {
      0xb100u,  // 11_1
      0xb000u,  // 11_0
      0xa100u,  // 10_1
      0xa000u,  // 10_0
  };
  int n = 0;
  for (unsigned lvl : k_levels) {
    if (lvl <= ceiling && n < cap) out[n++] = lvl;
  }
  if (n == 0 && cap > 0) out[n++] = k_d3d11_ceiling;
  return n;
}

} // namespace longan
