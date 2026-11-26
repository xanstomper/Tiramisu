// tiramisu/src/host/console_detect.h
// Console generation detection and profile defaults.
// Best-effort identification for Original Xbox One / Durango, Xbox One S /
// Edmonton, Xbox One X / Scorpio, Xbox Series S / Lockhart, and Xbox Series X /
// Anaconda. The CPUID/RAM heuristics are not a hardware proof; profile defaults
// must be validated on each console family before being called supported.
#ifndef TIRAMISU_CONSOLE_DETECT_H
#define TIRAMISU_CONSOLE_DETECT_H

#include <cstdint>
#include <string>

namespace longan {

enum class xbox_console_generation {
  unknown = 0,
  xbox_one,       // Original 2013 Xbox One (Durango, 8-core Jaguar 1.75 GHz, GCN 1.31 TF, 8 GB DDR3 + 32 MB ESRAM)
  xbox_one_s,     // Xbox One S (Edmonton, 8-core Jaguar 1.75 GHz, GCN 1.40 TF HDR, 8 GB DDR3 + 32 MB ESRAM)
  xbox_one_x,     // Xbox One X (Scorpio, 8-core Jaguar Evolved 2.3 GHz, GCN 6.0 TF, 12 GB GDDR5)
  xbox_series_s,  // Xbox Series S (Lockhart, 8-core Zen 2 3.6 GHz, RDNA2 4.0 TF, 10 GB GDDR6)
  xbox_series_x,  // Xbox Series X (Anaconda, 8-core Zen 2 3.8 GHz, RDNA2 12.15 TF, 16 GB GDDR6)
  pc_desktop      // Windows/Linux PC or Virtual Machine
};

struct console_hardware_info {
  xbox_console_generation generation;
  const char* generation_name;        // "Xbox One", "Xbox One S", "Xbox One X", etc.
  const char* family_id;              // "one", "one_s", "one_x", "series_s", "series_x", "pc"
  const char* codename;               // "Durango", "Edmonton", "Scorpio", "Lockhart", "Anaconda"
  const char* cpu_arch;               // "AMD Jaguar", "AMD Jaguar Evolved", "AMD Zen 2"
  int cpu_cores;                      // 8
  float cpu_clock_ghz;                // 1.75, 2.30, 3.60, 3.80
  bool has_avx2;                      // false for Jaguar, true for Zen 2
  const char* gpu_arch;               // "GCN 1.1", "GCN 1.1 (HDR)", "GCN 4.0 (Polaris)", "RDNA 2"
  int gpu_compute_units;              // 12, 12, 40, 20, 52
  float gpu_tflops;                   // 1.31, 1.40, 6.00, 4.00, 12.15
  unsigned int max_d3d_feature_level; // e.g. 0xb000 (11_0), 0xb100 (11_1), 0xc000 (12_0)
  bool has_esram;                     // true for One & One S
  int esram_size_mb;                  // 32 for One & One S, 0 for others
  int total_ram_mb;                   // 8192, 12288, 10240, 16384
  int dev_mode_game_budget_mb;        // 5120 MB (5 GB) across all Xbox consoles in Game Dev Mode
  int recommended_res_scale;          // 85 for One, 100 for One S/X/Series
  int recommended_fps_cap;            // 30 for One, 60 for others
  int recommended_tex_quality;        // 1 (medium) for One/One S, 2 (high) for One X
  bool d3d12_allowed;                 // false for One/One S, experimental for One X, true for Series
  bool requires_conservative_input;   // true for One/One S (older OS build GameInput fallback)
  bool is_last_gen;                   // true for One, One S, One X
};

// Autodetects the current console generation using CPUID, memory metrics, device info, and portal config
xbox_console_generation detect_console_generation();

// Get full hardware details for the detected console
console_hardware_info get_detected_console_info();

// Get hardware details for a specific generation
console_hardware_info get_console_info_for_generation(xbox_console_generation gen);

// Parses human or model string to generation enum
xbox_console_generation parse_console_generation_string(const char* str);

// D3D11 feature levels to request for a generation, highest first and capped by
// that console's ceiling (D3D_FEATURE_LEVEL_* values, 0xb000 == 11_0). The
// One family never asks for 11_1: Durango/Edmonton are GCN 1.1 parts and an
// 11_1 request only hides the real level the device came up at. D3D12 levels
// (0xc000/0xc100) are clamped to the D3D11 ceiling because this list feeds
// D3D11CreateDevice. Returns the number of entries written (never 0 when a
// buffer is supplied).
int console_feature_levels(xbox_console_generation gen, unsigned* out, int cap);

// Limit new guest allocations to the application's measured free memory.
unsigned console_guest_memory_budget_mb(unsigned configured_mb, uint64_t app_usage,
                                        uint64_t app_limit);

// Applies hardware profile to host settings and runtime ledgers
void apply_console_hardware_profile(const console_hardware_info& info, const std::string& local_state);

} // namespace longan

#endif // TIRAMISU_CONSOLE_DETECT_H
