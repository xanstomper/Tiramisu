// tiramisu/src/trt/bridge_esram.cpp
// 32 MB ESRAM budget ledger for Xbox One (Durango) and Xbox One S (Edmonton).
#include "bridge_esram.h"
#include "thunk.h"

#include <atomic>
#include <cstdio>
#include <cstdint>

namespace {

std::atomic<uint64_t> g_esram_capacity{0};
std::atomic<uint64_t> g_esram_used{0};
std::atomic<uint64_t> g_esram_alloc_count{0};
std::atomic<uint64_t> g_esram_hits{0};
std::atomic<uint64_t> g_esram_spills{0};

} // namespace

extern "C" {

void trt_esram_init(uint32_t size_mb) {
  uint64_t bytes = static_cast<uint64_t>(size_mb) * 1024ULL * 1024ULL;
  g_esram_capacity.store(bytes);
  g_esram_used.store(0);
  g_esram_alloc_count.store(0);
  g_esram_hits.store(0);
  g_esram_spills.store(0);

  if (size_mb > 0) {
    char line[160];
    snprintf(line, sizeof(line),
             "[trt_esram] Initialized %u MB ESRAM budget ledger (accounting only; driver owns residency)",
             size_mb);
    trt_core_log(line);
  }
}

int trt_esram_is_active(void) {
  return g_esram_capacity.load() > 0 ? 1 : 0;
}

int trt_esram_should_place(uint32_t bind_flags, uint64_t byte_size) {
  if (g_esram_capacity.load() == 0) return 0;
  // D3D11_BIND_RENDER_TARGET = 0x20, D3D11_BIND_DEPTH_STENCIL = 0x40
  const bool is_render_surface = (bind_flags & 0x20u) != 0 || (bind_flags & 0x40u) != 0;
  if (!is_render_surface) return 0;

  // Maximum single surface threshold for budget accounting is 16 MB (half the pool)
  const uint64_t k_max_surface_bytes = 16ULL * 1024ULL * 1024ULL;
  if (byte_size > k_max_surface_bytes) return 0;

  const uint64_t cap = g_esram_capacity.load();
  const uint64_t used = g_esram_used.load();
  return (used + byte_size <= cap) ? 1 : 0;
}

int trt_esram_alloc(uint64_t bytes, const char* debug_name) {
  const uint64_t cap = g_esram_capacity.load();
  if (cap == 0 || bytes == 0) return -1;

  uint64_t cur = g_esram_used.load();
  do {
    if (cur + bytes > cap) {
      g_esram_spills.fetch_add(1);
      return -1; // Spills to main DDR3 memory
    }
  } while (!g_esram_used.compare_exchange_weak(cur, cur + bytes));

  g_esram_alloc_count.fetch_add(1);
  g_esram_hits.fetch_add(1);

  if (debug_name) {
    char line[192];
    snprintf(line, sizeof(line),
             "[trt_esram] Accounted '%s' (%llu KB) against the ESRAM budget [%llu / %llu MB used]",
             debug_name,
             static_cast<unsigned long long>(bytes / 1024ULL),
             static_cast<unsigned long long>((cur + bytes) / (1024ULL * 1024ULL)),
             static_cast<unsigned long long>(cap / (1024ULL * 1024ULL)));
    trt_core_log(line);
  }
  return 0;
}

void trt_esram_free(uint64_t bytes) {
  if (bytes == 0) return;
  uint64_t cur = g_esram_used.load();
  uint64_t nxt = 0;
  do {
    nxt = (cur > bytes) ? cur - bytes : 0;
  } while (!g_esram_used.compare_exchange_weak(cur, nxt));
}

int trt_esram_get_stats(uint64_t* out_used_bytes, uint64_t* out_total_bytes, uint64_t* out_alloc_count) {
  if (out_used_bytes) *out_used_bytes = g_esram_used.load();
  if (out_total_bytes) *out_total_bytes = g_esram_capacity.load();
  if (out_alloc_count) *out_alloc_count = g_esram_alloc_count.load();
  return 0;
}

void trt_esram_reset(void) {
  g_esram_used.store(0);
  g_esram_alloc_count.store(0);
}

uint64_t trt_esram_calculate_surface_bytes(uint32_t width, uint32_t height, uint32_t dxgi_format) {
  if (width == 0 || height == 0) return 0;
  uint32_t bpp = 4; // Default 32-bpp (RGBA8, D24S8, D32F, etc.)
  switch (dxgi_format) {
    case 2:  // DXGI_FORMAT_R32G32B32A32_FLOAT / TYPELESS
      bpp = 16;
      break;
    case 10: // DXGI_FORMAT_R16G16B16A16_FLOAT
    case 11: // DXGI_FORMAT_R16G16B16A16_UNORM
    case 20: // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
      bpp = 8;
      break;
    case 53: // DXGI_FORMAT_D16_UNORM
    case 54: // DXGI_FORMAT_R16_UNORM
    case 55: // DXGI_FORMAT_R16_UINT
    case 56: // DXGI_FORMAT_R16_SINT
    case 57: // DXGI_FORMAT_R16_TYPELESS
      bpp = 2;
      break;
    case 61: // DXGI_FORMAT_A8_UNORM
      bpp = 1;
      break;
    default:
      bpp = 4;
      break;
  }
  return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(bpp);
}

int trt_esram_qualify_and_alloc(uint32_t width, uint32_t height, uint32_t dxgi_format, uint32_t bind_flags, const char* name) {
  if (!trt_esram_is_active()) return -1;
  uint64_t bytes = trt_esram_calculate_surface_bytes(width, height, dxgi_format);
  if (!trt_esram_should_place(bind_flags, bytes)) return -1;
  return trt_esram_alloc(bytes, name);
}

void trt_esram_release_surface(uint32_t width, uint32_t height, uint32_t dxgi_format) {
  if (!trt_esram_is_active()) return;
  uint64_t bytes = trt_esram_calculate_surface_bytes(width, height, dxgi_format);
  trt_esram_free(bytes);
}

} // extern "C"
