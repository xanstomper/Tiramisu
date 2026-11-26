// tiramisu/tests/test_esram.cpp
// Unit tests for the 32 MB ESRAM budget ledger (src/trt/bridge_esram.cpp).
// Verifies surface byte calculations, qualification heuristics, 32 MB capacity limits,
// spillover accounting, and surface release arithmetic for Xbox One and One S.
// This does not test hardware residency: UWP D3D11 leaves that to the driver.
#include "../src/trt/bridge_esram.h"
#include <cstdio>
#include <cstdint>
#include <cassert>

extern "C" {
void trt_core_log(const char* line) { (void)line; }
}

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) {                                                      \
      printf("  PASS %s\n", msg);                                    \
    } else {                                                         \
      printf("  FAIL %s\n", msg);                                    \
      ++g_failures;                                                  \
    }                                                                \
  } while (0)

} // namespace

int main() {
  printf("tiramisu 32 MB ESRAM budget ledger suite\n");

  // 1. Initial / inactive state
  trt_esram_init(0);
  CHECK(trt_esram_is_active() == 0, "ESRAM inactive when size is 0 MB (Scorpio/Series S/X mode)");
  CHECK(trt_esram_alloc(1024, "test_alloc") == -1, "alloc refuses when ESRAM is inactive");

  // 2. Activation for Durango / Edmonton (32 MB)
  trt_esram_init(32);
  CHECK(trt_esram_is_active() == 1, "ESRAM active when initialized with 32 MB");

  uint64_t used = 0, total = 0, count = 0;
  trt_esram_get_stats(&used, &total, &count);
  CHECK(total == 32ULL * 1024ULL * 1024ULL, "total capacity is exactly 32 MB (33554432 bytes)");
  CHECK(used == 0, "initial used bytes is 0");
  CHECK(count == 0, "initial allocation count is 0");

  // 3. Surface byte calculation tests across DXGI formats
  CHECK(trt_esram_calculate_surface_bytes(0, 1080, 28) == 0, "0 width yields 0 bytes");
  CHECK(trt_esram_calculate_surface_bytes(1920, 0, 28) == 0, "0 height yields 0 bytes");

  // 1920x1080 RGBA8 (4 bpp): 1920 * 1080 * 4 = 8,294,400 bytes (~7.91 MB)
  const uint64_t fhd_rgba8 = trt_esram_calculate_surface_bytes(1920, 1080, 28);
  CHECK(fhd_rgba8 == 8294400ULL, "1080p RGBA8 is 8,294,400 bytes");

  // 1920x1080 R16G16B16A16_FLOAT (8 bpp): 1920 * 1080 * 8 = 16,588,800 bytes (~15.82 MB)
  const uint64_t fhd_rgba16f = trt_esram_calculate_surface_bytes(1920, 1080, 10);
  CHECK(fhd_rgba16f == 16588800ULL, "1080p RGBA16F is 16,588,800 bytes");

  // 1920x1080 R32G32B32A32_FLOAT (16 bpp): 1920 * 1080 * 16 = 33,177,600 bytes (~31.64 MB)
  const uint64_t fhd_rgba32f = trt_esram_calculate_surface_bytes(1920, 1080, 2);
  CHECK(fhd_rgba32f == 33177600ULL, "1080p RGBA32F is 33,177,600 bytes");

  // 1280x720 B5G6R5 (2 bpp): 1280 * 720 * 2 = 1,843,200 bytes
  CHECK(trt_esram_calculate_surface_bytes(1280, 720, 53) == 1843200ULL, "720p D16_UNORM is 1,843,200 bytes");

  // 1280x720 R8_UNORM (1 bpp): 1280 * 720 * 1 = 921,600 bytes
  CHECK(trt_esram_calculate_surface_bytes(1280, 720, 61) == 921600ULL, "720p A8_UNORM is 921,600 bytes");

  // 4. Placement heuristics (trt_esram_should_place)
  // Non-render target / depth surface (e.g. shader resource only = 0x8) -> does NOT qualify (streams to DDR3)
  CHECK(trt_esram_should_place(0x08, fhd_rgba8) == 0, "pure shader resource does not qualify for ESRAM");

  // Render target (0x20) of 8.29 MB -> qualifies
  CHECK(trt_esram_should_place(0x20, fhd_rgba8) == 1, "1080p render target qualifies for ESRAM");

  // Depth-stencil (0x40) of 8.29 MB -> qualifies
  CHECK(trt_esram_should_place(0x40, fhd_rgba8) == 1, "1080p depth-stencil qualifies for ESRAM");

  // Single surface exceeding 16 MB ceiling (> half the pool, e.g. RGBA32F at 33.17 MB) -> does NOT qualify
  CHECK(trt_esram_should_place(0x20, fhd_rgba32f) == 0, "surface > 16 MB ceiling spills to DDR3");

  // 5. Allocation and spillover arithmetic
  // First surface: 8.29 MB fits
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 28, 0x20, "ColorBackbuffer") == 0, "ColorBackbuffer allocated in ESRAM");
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8, "used bytes equals first surface size");
  CHECK(count == 1, "alloc count is 1");

  // Second surface: 1080p D24S8 depth buffer (format 45, 4 bpp -> 8.29 MB) fits (total ~16.58 MB)
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 45, 0x40, "DepthStencil") == 0, "DepthStencil allocated in ESRAM");
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8 * 2, "used bytes equals two surfaces (~16.58 MB)");
  CHECK(count == 2, "alloc count is 2");

  // Third surface: 1080p HDR buffer (8.29 MB) fits (total ~24.88 MB)
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 28, 0x20, "HDRCompose") == 0, "HDRCompose allocated in ESRAM");
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8 * 3, "used bytes equals three surfaces (~24.88 MB)");

  // Fourth surface: 1080p resolve buffer (8.29 MB) fits (total 33.18 MB <= 32 MB pool of 33.55 MB)
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 28, 0x20, "PostProcessResolve") == 0, "fourth 1080p surface fits in ESRAM (33.18 MB <= 33.55 MB)");
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8 * 4, "used bytes equals four surfaces (~33.18 MB)");

  // Fifth surface: 8.29 MB would exceed 32 MB (33.18 + 8.29 = 41.47 MB > 33.55 MB) -> spills safely to DDR3!
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 28, 0x20, "SpilloverSurface") == -1, "surface exceeding remaining ESRAM capacity spills safely to DDR3 (-1)");
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8 * 4, "spillover surface leaves ESRAM ledger unchanged");

  // 6. Surface release arithmetic
  trt_esram_release_surface(1920, 1080, 28);
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == fhd_rgba8 * 3, "releasing surface discharges exactly 8.29 MB from ESRAM");

  // Re-allocating after release now succeeds
  CHECK(trt_esram_qualify_and_alloc(1920, 1080, 28, 0x20, "PostProcessBuffer") == 0, "surface allocates successfully into freed ESRAM window");

  // 7. Reset ledger
  trt_esram_reset();
  trt_esram_get_stats(&used, &total, &count);
  CHECK(used == 0, "reset clears used ESRAM bytes to 0");
  CHECK(count == 0, "reset clears alloc count to 0");

  printf("RESULT: %s (%d failures)\n", g_failures ? "FAILURES" : "ALL PASS", g_failures);
  return g_failures ? 1 : 0;
}
