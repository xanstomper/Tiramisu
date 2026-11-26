// tiramisu/src/trt/bridge_esram.h
// 32 MB ESRAM budget ledger for Xbox One (Durango) and Xbox One S (Edmonton).
//
// This is accounting, not residency control: UWP D3D11 does not expose explicit
// ESRAM allocation, so the graphics driver still decides which resources live
// in the 32 MB on-die SRAM. The ledger tracks frame-critical render targets and
// depth-stencil surfaces that are good ESRAM candidates, enforces the 32 MB
// budget, and reports spillover. It does not by itself make a resource faster.
#ifndef TIRAMISU_BRIDGE_ESRAM_H
#define TIRAMISU_BRIDGE_ESRAM_H

#include "trt.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Memory tier flags
#define TRT_MEM_TIER_DEFAULT  0x00  // Main system DDR3/GDDR5/GDDR6
#define TRT_MEM_TIER_ESRAM    0x01  // High-speed 32 MB embedded SRAM
#define TRT_MEM_TIER_STREAM   0x02  // Streaming asset (textures, audio banks, bulk files)

// Initialize ESRAM pool (size_mb=32 on Xbox One/One S, 0 on One X/Series S/X)
void trt_esram_init(uint32_t size_mb);

// Returns true if ESRAM hardware is present and active
int trt_esram_is_active(void);

// Classifies whether a buffer is an ESRAM budget candidate based on type and size.
// RenderTargets (D3D11_BIND_RENDER_TARGET=0x20) and DepthStencils (0x40) <= 16 MB qualify for accounting.
int trt_esram_should_place(uint32_t bind_flags, uint64_t byte_size);

// Allocates a tracking slot in the ESRAM budget ledger
int trt_esram_alloc(uint64_t bytes, const char* debug_name);

// Frees bytes back to the ESRAM budget ledger
void trt_esram_free(uint64_t bytes);

// Query ESRAM telemetry: returns 0 on success
int trt_esram_get_stats(uint64_t* out_used_bytes, uint64_t* out_total_bytes, uint64_t* out_alloc_count);

// Reset ESRAM ledger
void trt_esram_reset(void);

// Calculates surface byte size for typical DXGI render/depth formats
uint64_t trt_esram_calculate_surface_bytes(uint32_t width, uint32_t height, uint32_t dxgi_format);

// Qualifies surface and allocates slot in ESRAM ledger if surface qualifies
int trt_esram_qualify_and_alloc(uint32_t width, uint32_t height, uint32_t dxgi_format, uint32_t bind_flags, const char* name);

// Releases surface from ESRAM ledger
void trt_esram_release_surface(uint32_t width, uint32_t height, uint32_t dxgi_format);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_ESRAM_H
