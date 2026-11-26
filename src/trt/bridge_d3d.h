// tiramisu/src/trt/bridge_d3d.h
// The graphics bridge: a PC game's D3D11/DXGI calls have to end up on the
// host's CoreWindow swapchain.
//
// The console has no HWND, so a game's own CreateSwapChainForHwnd(fakeHWND)
// cannot work, and UWP has no D3D9 at all. The bridge therefore:
//   - hands the guest the HOST's ID3D11Device/ID3D11DeviceContext, so the
//     guest's resources live in the same device as the host's swapchain;
//   - hands the guest a factory whose CreateSwapChain* returns the HOST's
//     swapchain (the guest then renders into the host's backbuffer);
//   - forwards everything else (adapter enumeration, window association)
//     to the console's real DXGI factory.
//
// The device and context are passed through. The factory and swapchain
// translate desktop DXGI calls for the host CoreWindow.
#ifndef TIRAMISU_BRIDGE_D3D_H
#define TIRAMISU_BRIDGE_D3D_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the host provides (real COM pointers).
typedef struct trt_gpu_hooks {
  void* device;      // ID3D11Device*
  void* context;     // ID3D11DeviceContext*
  void* swapchain;   // IDXGISwapChain1* (over the CoreWindow)
  void* factory;     // IDXGIFactory2* (the console's real one)
  unsigned feature_level;
  unsigned width, height;
  // Optional Wave 2A host hooks (null in offline tests):
  void* factory6;        // IDXGIFactory6* (if host DXGI >= 1.6)
  void* swapchain3;      // IDXGISwapChain3* for active D3D11 CoreWindow swapchain
  void* d3d12_device;    // ID3D12Device*
  void* d3d12_swapchain; // IDXGISwapChain3* bound to active D3D12 command queue
  int (TRT_GUEST_ABI *acquire_d3d12_swapchain)(void* queue, u32 width, u32 height,
                                               u32 fmt, u32 count,
                                               void** out_swapchain);
  void (*restore_d3d11_swapchain)(void);
  int (TRT_GUEST_ABI *d3d11on12_create)(void* device, u32 flags,
                                          const void* levels, u32 level_count,
                                          void* const* queues, u32 num_queues,
                                          u32 node_mask, void** out_device,
                                          void** out_ctx, u32* out_level);
  int (TRT_GUEST_ABI *configure_d3d11_swapchain)(u32 width, u32 height,
                                                    u32 format, u32 buffer_count,
                                                    u32 swap_effect);
  int (TRT_GUEST_ABI *get_d3d11_backbuffer)(u32 index, const void* iid, void** out);
  void (TRT_GUEST_ABI *prepare_d3d11_present)(void);
} trt_gpu_hooks;

void trt_gpu_set_hooks(const trt_gpu_hooks* hooks);
const trt_gpu_hooks* trt_gpu_hooks_get(void);

// Host capability probes for the launch preflight (0 = absent):
//   host_device    - an ID3D11Device + ID3D11DeviceContext pair is installed
//   host_d3d12     - an ID3D12Device is installed (0 = D3D12 is recorded only)
//   host_swapchain - a window swapchain exists, so frames reach a window
int trt_gpu_host_device_present(void);
int trt_gpu_host_d3d12_present(void);
int trt_gpu_host_swapchain_present(void);

// D3D11 bridge activity for the current guest launch.
typedef struct trt_gpu_render_stats {
  u32 d3d11_device_calls;
  u32 d3d11_swapchains;
  u32 d3d11_get_buffer_calls;
  u32 d3d11_present_calls;
  u32 d3d11_successful_presents;
  int d3d11_last_present_result;
  u32 d3d11_swap_effect;
  u32 d3d11_buffer_count;
  u32 d3d11_stable_backbuffer;
  u32 d3d11_width, d3d11_height;
} trt_gpu_render_stats;

typedef struct trt_gpu_resize_stats {
  u32 target_calls, target_width, target_height;
  int target_result;
  u32 buffer_calls, buffer_requested_width, buffer_requested_height;
  u32 buffer_client_width, buffer_client_height, buffer_present_count;
  int buffer_result;
} trt_gpu_resize_stats;

void trt_gpu_get_resize_stats(trt_gpu_resize_stats* stats);

int trt_gpu_d3d11_uses_stable_backbuffer(u32 buffer_count, u32 swap_effect);
// Count nonblack RGB/BGR pixels in a CPU readback without counting alpha or padding.
u32 trt_gpu_count_lit_rgb32(const void* pixels, u32 width, u32 height, u32 row_pitch);

void trt_gpu_reset_render_stats(void);
void trt_gpu_get_render_stats(trt_gpu_render_stats* stats);

// Register d3d11.dll / dxgi.dll as BUILTIN with the bridge functions.
int trt_bridge_register_d3d(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_D3D_H
