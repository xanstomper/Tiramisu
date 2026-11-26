// tiramisu/src/host/vk_backend.hpp
// UWP Direct3D 11 execution backend for Vulkan 1.0-1.3 (bridge_vulkan.h).
// Header-only matching src/host/gl_backend.hpp; bound in src/host/swapchain.cpp.
#pragma once

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

#include "../trt/bridge_vulkan.h"
#include "../trt/spirv_hlsl.h"
#include "../trt/thunk.h"
#include "../trt/bridge_esram.h"
#include "shader_cache_util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

// Guest memory-budget ledger (bridge_membudget.cpp); declared extern "C"
// here as in main.cpp:1034 / bridge_kernel32.cpp:75.
extern "C" void trt_mem_budget_report(char* line, unsigned cap);
// .239 DIAG: bridge memory-budget ledger committed bytes
// (bridge_membudget.cpp) -- VIDEO-SAMPLE ledger=.
extern "C" u64 trt_mem_budget_committed(void);
// .223 DIAG: cross-TU diagnostic accessors (defined in
// bridge_vulkan.cpp / bridge_kernel32.cpp; declared here,
// not in trt.h, so trt.h stays untouched). All are
// lock-free atomics -- safe to call from the watchdog
// thread and from the present hook while g_vk_mu is held.
extern "C" u64 trt_vk_alloc_live_bytes(void);
extern "C" u64 trt_vk_alloc_live_n(void);
extern "C" u64 trt_vk_imgs_live_n(void);
extern "C" u64 trt_vk_imgs_est_bytes(void);
extern "C" u64 trt_vk_presents(void);
extern "C" u64 trt_vk_biggest_alloc_bytes(void);
// .242 DIAG: bridged NtAllocateVirtualMemory(Ex) totals
// (bridge_ntdll.cpp) -- VIDEO-SAMPLE ntalloc_calls=/ntalloc_mb=.
extern "C" u64 trt_ntalloc_calls(void);
extern "C" u64 trt_ntalloc_bytes(void);
extern "C" u64 trt_vk_hostvec_live_bytes(void);
extern "C" u64 trt_video_app_mem_mb(void);
extern "C" u64 gap_trace_seq(void);
// .244 DIAG: last bridge-issued D3D11 call (defined in
// bridge_thread.cpp, outside the anonymous namespace +
// extern "C" -- the .233 cross-TU lesson; declared here,
// not in trt.h, so trt.h stays untouched). d3d11_note_last
// records the call name (a string literal) + the
// GetTickCount64 tick; d3d11_last_call reads both back
// (atomics -- safe from the sampler/video threads). The
// GAP-IP-SAMPLE line and VIDEO-SAMPLE print the name + its
// age, so the terminal wait can be tied to the last D3D11
// call the bridge issued.
extern "C" void d3d11_note_last(const char* name);
extern "C" void d3d11_last_call(const char** name_out,
                                unsigned long long* tick_ms_out);

namespace vk_rt {

struct pipe_rec {
  bool is_compute = false;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11ComputeShader* cs = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11BlendState* bs = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  uint32_t vb_strides[8] = {};
  std::vector<uint8_t> vs_blob;
};

struct buf_rec {
  ID3D11Buffer* buf = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  uint32_t usage = 0;
  uint64_t bytes = 0;
  std::vector<uint8_t> shadow;
};

struct img_rec {
  trt_vk_image_desc desc{};
  // DXGI format of the created texture, recorded so free_img_rec() can
  // release the matching ESRAM surface. desc.vk_format is a Vulkan format,
  // not the DXGI format trt_esram_release_surface expects.
  uint32_t tex2d_fmt = 0;
  ID3D11Texture2D* tex2d = nullptr;
  ID3D11Texture3D* tex3d = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  // .221 DIAG: estimated D3D11 bytes of the live tex2d/tex3d
  // (see d3d_tex_note_created); subtracted in free_img_rec.
  u64 d3d_bytes = 0;
};

struct vk_state {
  bool initialized = false;
  ID3D11Device1* dev = nullptr;
  ID3D11DeviceContext1* ctx = nullptr;
  IDXGISwapChain1* swap = nullptr;
  ID3D11RenderTargetView* main_rtv = nullptr;
  ID3D11Texture2D* ds_tex = nullptr;
  ID3D11DepthStencilView* main_dsv = nullptr;
  ID3D11Buffer* push_cb = nullptr;
  uint32_t push_cb_cap = 0;
  ID3D11Buffer* slice_cbs[14] = {};
  uint32_t slice_cb_caps[14] = {};
  UINT width = 1280;
  UINT height = 720;

  std::map<uint64_t, pipe_rec> pipes;
  std::map<uint64_t, buf_rec> bufs;
  std::map<uint64_t, img_rec> imgs;
  std::map<uint64_t, ID3D11SamplerState*> samps;
};

inline vk_state g_vkrt;

// .244 DIAG: D3D11 call/ret counters -- which D3D11 call is the main
// thread parked inside, and which call's count tracks the 4.3GB region
// growth. Every bridge-issued D3D11 call in this header increments
// its _c counter before the call and its _r counter after it returns,
// and records the call name via d3d11_note_last (bridge_thread.cpp).
// The terminal state names the waiter: main parks in
// d3d11on12.dll+0x3ad16 waiting on a host Event (a D3D12 completion
// the layer waits on -- .243). If one pair shows calls = rets + 1 at
// the terminal sample, main is parked INSIDE that D3D11 call (the
// layer waits for the D3D12 fence inside it). If ALL pairs balance,
// main parked without an outstanding bridge D3D11 call -- the wait
// was entered from a non-vk_backend path, and .245 chases that.
// The per-interval deltas on VIDEO-SAMPLE (dtex2d/dupd/dpres vs
// privn) name the allocator: creation (CreateTexture2D) vs the
// upload path (UpdateSubresource). Atomics: the render thread
// increments, the video thread reads.
struct d3d11_call_counts {
  std::atomic<unsigned long long> tex2d_c{0};
  std::atomic<unsigned long long> tex2d_r{0};
  std::atomic<unsigned long long> tex3d_c{0};
  std::atomic<unsigned long long> tex3d_r{0};
  std::atomic<unsigned long long> srv_c{0};
  std::atomic<unsigned long long> srv_r{0};
  std::atomic<unsigned long long> rtv_c{0};
  std::atomic<unsigned long long> rtv_r{0};
  std::atomic<unsigned long long> dsv_c{0};
  std::atomic<unsigned long long> dsv_r{0};
  std::atomic<unsigned long long> uav_c{0};
  std::atomic<unsigned long long> uav_r{0};
  std::atomic<unsigned long long> upd_c{0};
  std::atomic<unsigned long long> upd_r{0};
  std::atomic<unsigned long long> map_c{0};
  std::atomic<unsigned long long> map_r{0};
  std::atomic<unsigned long long> unmap_c{0};
  std::atomic<unsigned long long> unmap_r{0};
  std::atomic<unsigned long long> copy_c{0};
  std::atomic<unsigned long long> copy_r{0};
  std::atomic<unsigned long long> pres_c{0};
  std::atomic<unsigned long long> pres_r{0};
  std::atomic<unsigned long long> sync_c{0};
  std::atomic<unsigned long long> newtex_c{0};
  std::atomic<unsigned long long> draw_c{0};  // .245 DIAG: DrawIndexedInstanced/DrawInstanced
  std::atomic<unsigned long long> draw_r{0};
  std::atomic<unsigned long long> disp_c{0};  // .245 DIAG: Dispatch
  std::atomic<unsigned long long> disp_r{0};
  std::atomic<unsigned long long> state_c{0};  // .245 DIAG: setters/clears/OMSet/IASet/RSSet/VS/PS/CSSet*
  std::atomic<unsigned long long> state_r{0};
  std::atomic<unsigned long long> flush_c{0};  // .245 DIAG: Flush per submit
  std::atomic<unsigned long long> flush_r{0};
  std::atomic<unsigned long long> texskip_c{0};  // .245 DIAG: texture creation skipped, device removed
};
inline d3d11_call_counts g_d3d11c;

// Plain snapshot of the counters (one atomic load each).
struct d3d11_counts_snap {
  unsigned long long tex2d_c, tex2d_r, tex3d_c, tex3d_r, srv_c,
      srv_r, rtv_c, rtv_r, dsv_c, dsv_r, uav_c, uav_r, upd_c,
      upd_r, map_c, map_r, unmap_c, unmap_r, copy_c, copy_r,
      pres_c, pres_r, sync_c, newtex_c, draw_c, draw_r,
      disp_c, disp_r, state_c, state_r, flush_c, flush_r,
      texskip_c;
};
inline d3d11_counts_snap d3d11_snapshot() {
  d3d11_counts_snap d{};
  d.tex2d_c = g_d3d11c.tex2d_c.load(std::memory_order_relaxed);
  d.tex2d_r = g_d3d11c.tex2d_r.load(std::memory_order_relaxed);
  d.tex3d_c = g_d3d11c.tex3d_c.load(std::memory_order_relaxed);
  d.tex3d_r = g_d3d11c.tex3d_r.load(std::memory_order_relaxed);
  d.srv_c = g_d3d11c.srv_c.load(std::memory_order_relaxed);
  d.srv_r = g_d3d11c.srv_r.load(std::memory_order_relaxed);
  d.rtv_c = g_d3d11c.rtv_c.load(std::memory_order_relaxed);
  d.rtv_r = g_d3d11c.rtv_r.load(std::memory_order_relaxed);
  d.dsv_c = g_d3d11c.dsv_c.load(std::memory_order_relaxed);
  d.dsv_r = g_d3d11c.dsv_r.load(std::memory_order_relaxed);
  d.uav_c = g_d3d11c.uav_c.load(std::memory_order_relaxed);
  d.uav_r = g_d3d11c.uav_r.load(std::memory_order_relaxed);
  d.upd_c = g_d3d11c.upd_c.load(std::memory_order_relaxed);
  d.upd_r = g_d3d11c.upd_r.load(std::memory_order_relaxed);
  d.map_c = g_d3d11c.map_c.load(std::memory_order_relaxed);
  d.map_r = g_d3d11c.map_r.load(std::memory_order_relaxed);
  d.unmap_c = g_d3d11c.unmap_c.load(std::memory_order_relaxed);
  d.unmap_r = g_d3d11c.unmap_r.load(std::memory_order_relaxed);
  d.copy_c = g_d3d11c.copy_c.load(std::memory_order_relaxed);
  d.copy_r = g_d3d11c.copy_r.load(std::memory_order_relaxed);
  d.pres_c = g_d3d11c.pres_c.load(std::memory_order_relaxed);
  d.pres_r = g_d3d11c.pres_r.load(std::memory_order_relaxed);
  d.sync_c = g_d3d11c.sync_c.load(std::memory_order_relaxed);
  d.newtex_c = g_d3d11c.newtex_c.load(std::memory_order_relaxed);
  d.draw_c = g_d3d11c.draw_c.load(std::memory_order_relaxed);
  d.draw_r = g_d3d11c.draw_r.load(std::memory_order_relaxed);
  d.disp_c = g_d3d11c.disp_c.load(std::memory_order_relaxed);
  d.disp_r = g_d3d11c.disp_r.load(std::memory_order_relaxed);
  d.state_c = g_d3d11c.state_c.load(std::memory_order_relaxed);
  d.state_r = g_d3d11c.state_r.load(std::memory_order_relaxed);
  d.flush_c = g_d3d11c.flush_c.load(std::memory_order_relaxed);
  d.flush_r = g_d3d11c.flush_r.load(std::memory_order_relaxed);
  d.texskip_c = g_d3d11c.texskip_c.load(std::memory_order_relaxed);
  return d;
}

// One-line "calls/rets" table for VIDEO-SAMPLE / VIDEO-BIGJUMP.
inline void d3d11_format_line(char* buf, size_t cap,
                               const d3d11_counts_snap& d) {
  snprintf(buf, cap,
           "tex2d=%llu/%llu tex3d=%llu/%llu upd=%llu/%llu "
           "srv=%llu/%llu rtv=%llu/%llu dsv=%llu/%llu "
           "uav=%llu/%llu map=%llu/%llu copy=%llu/%llu "
           "pres=%llu/%llu sync=%llu newtex=%llu "
           "draw=%llu/%llu disp=%llu/%llu state=%llu/%llu "
           "flush=%llu/%llu texskip=%llu",
           d.tex2d_c, d.tex2d_r, d.tex3d_c, d.tex3d_r, d.upd_c,
           d.upd_r, d.srv_c, d.srv_r, d.rtv_c, d.rtv_r, d.dsv_c,
           d.dsv_r, d.uav_c, d.uav_r, d.map_c, d.map_r, d.copy_c,
           d.copy_r, d.pres_c, d.pres_r, d.sync_c, d.newtex_c,
           d.draw_c, d.draw_r, d.disp_c, d.disp_r, d.state_c,
           d.state_r, d.flush_c, d.flush_r, d.texskip_c);
}

inline uint64_t fnv64_mix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}

inline bool is_depth_vk_fmt(uint32_t vk_fmt) {
  return vk_fmt == 124u || vk_fmt == 125u || vk_fmt == 126u ||
         vk_fmt == 127u || vk_fmt == 128u || vk_fmt == 129u || vk_fmt == 130u;
}

inline bool is_bc_vk_fmt(uint32_t vk_fmt) {
  return vk_fmt >= 131u && vk_fmt <= 146u;
}

inline DXGI_FORMAT vk_to_dxgi_fmt(uint32_t vk_fmt, bool typeless_depth = false) {
  switch (vk_fmt) {
    case 9u: return DXGI_FORMAT_R8_UNORM;
    case 10u: return DXGI_FORMAT_R8_SNORM;
    case 13u: return DXGI_FORMAT_R8_UINT;
    case 14u: return DXGI_FORMAT_R8_SINT;
    case 16u: return DXGI_FORMAT_R8G8_UNORM;
    case 17u: return DXGI_FORMAT_R8G8_SNORM;
    case 20u: return DXGI_FORMAT_R8G8_UINT;
    case 21u: return DXGI_FORMAT_R8G8_SINT;
    case 37u: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 38u: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case 41u: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 42u: return DXGI_FORMAT_R8G8B8A8_SINT;
    case 43u: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 44u: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 50u: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 64u: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 70u: return DXGI_FORMAT_R16_UNORM;
    case 74u: return DXGI_FORMAT_R16_UINT;
    case 75u: return DXGI_FORMAT_R16_SINT;
    case 76u: return DXGI_FORMAT_R16_FLOAT;
    case 77u: return DXGI_FORMAT_R16G16_UNORM;
    case 78u: return DXGI_FORMAT_R16G16_SNORM;
    case 81u: return DXGI_FORMAT_R16G16_UINT;
    case 82u: return DXGI_FORMAT_R16G16_SINT;
    case 83u: return DXGI_FORMAT_R16G16_FLOAT;
    case 91u: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 92u: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 95u: return DXGI_FORMAT_R16G16B16A16_UINT;
    case 96u: return DXGI_FORMAT_R16G16B16A16_SINT;
    case 97u: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 98u: return DXGI_FORMAT_R32_UINT;
    case 99u: return DXGI_FORMAT_R32_SINT;
    case 100u: return DXGI_FORMAT_R32_FLOAT;
    case 101u: return DXGI_FORMAT_R32G32_UINT;
    case 102u: return DXGI_FORMAT_R32G32_SINT;
    case 103u: return DXGI_FORMAT_R32G32_FLOAT;
    case 104u: return DXGI_FORMAT_R32G32B32A32_UINT;
    case 105u: return DXGI_FORMAT_R32G32B32A32_SINT;
    case 106u: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 107u: return DXGI_FORMAT_R32G32B32A32_UINT;
    case 108u: return DXGI_FORMAT_R32G32B32A32_SINT;
    case 109u: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 122u: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 124u: return typeless_depth ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_D16_UNORM;
    case 126u: return typeless_depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    case 129u: return typeless_depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 130u: return typeless_depth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case 131u: case 133u: return DXGI_FORMAT_BC1_UNORM;
    case 132u: case 134u: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case 135u: return DXGI_FORMAT_BC2_UNORM;
    case 136u: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case 137u: return DXGI_FORMAT_BC3_UNORM;
    case 138u: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case 139u: return DXGI_FORMAT_BC4_UNORM;
    case 140u: return DXGI_FORMAT_BC4_SNORM;
    case 141u: return DXGI_FORMAT_BC5_UNORM;
    case 142u: return DXGI_FORMAT_BC5_SNORM;
    case 143u: return DXGI_FORMAT_BC6H_UF16;
    case 144u: return DXGI_FORMAT_BC6H_SF16;
    case 145u: return DXGI_FORMAT_BC7_UNORM;
    case 146u: return DXGI_FORMAT_BC7_UNORM_SRGB;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

inline DXGI_FORMAT depth_srv_fmt(uint32_t vk_fmt) {
  switch (vk_fmt) {
    case 124u: return DXGI_FORMAT_R16_UNORM;
    case 126u: return DXGI_FORMAT_R32_FLOAT;
    case 130u: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  }
}

inline uint32_t vk_fmt_bpp(uint32_t vk_fmt) {
  switch (vk_fmt) {
    case 9u: case 10u: case 13u: case 14u: return 1;
    case 16u: case 17u: case 20u: case 21u:
    case 70u: case 74u: case 75u: case 76u: case 124u: return 2;
    case 77u: case 78u: case 81u: case 82u: case 83u:
    case 98u: case 99u: case 100u: case 126u: case 129u: return 4;
    case 91u: case 92u: case 95u: case 96u: case 97u:
    case 101u: case 102u: case 103u: case 130u: return 8;
    case 104u: case 105u: case 106u: return 12;
    case 107u: case 108u: case 109u: return 16;
    default: return 4;
  }
}

inline void calc_img_pitch(uint32_t vk_fmt, UINT w, UINT h, UINT* row_pitch, UINT* slice_pitch) {
  if (is_bc_vk_fmt(vk_fmt)) {
    const UINT bx = std::max<UINT>(1u, (w + 3u) / 4u);
    const UINT by = std::max<UINT>(1u, (h + 3u) / 4u);
    const UINT block_sz = (vk_fmt <= 134u || vk_fmt == 139u || vk_fmt == 140u) ? 8u : 16u;
    *row_pitch = bx * block_sz;
    *slice_pitch = (*row_pitch) * by;
  } else {
    *row_pitch = std::max<UINT>(1u, w) * vk_fmt_bpp(vk_fmt);
    *slice_pitch = (*row_pitch) * std::max<UINT>(1u, h);
  }
}

inline D3D11_COMPARISON_FUNC vk_to_cmp(uint32_t op) {
  switch (op) {
    case 0u: return D3D11_COMPARISON_NEVER;
    case 1u: return D3D11_COMPARISON_LESS;
    case 2u: return D3D11_COMPARISON_EQUAL;
    case 3u: return D3D11_COMPARISON_LESS_EQUAL;
    case 4u: return D3D11_COMPARISON_GREATER;
    case 5u: return D3D11_COMPARISON_NOT_EQUAL;
    case 6u: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
  }
}

inline D3D11_BLEND vk_to_blend(uint32_t f, bool alpha) {
  switch (f) {
    case 0u: return D3D11_BLEND_ZERO;
    case 1u: return D3D11_BLEND_ONE;
    case 2u: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 3u: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 4u: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 5u: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 6u: return D3D11_BLEND_SRC_ALPHA;
    case 7u: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8u: return D3D11_BLEND_DEST_ALPHA;
    case 9u: return D3D11_BLEND_INV_DEST_ALPHA;
    case 10u: return D3D11_BLEND_BLEND_FACTOR;
    case 11u: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 14u: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 15u: return alpha ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC1_COLOR;
    case 16u: return alpha ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC1_COLOR;
    case 17u: return D3D11_BLEND_SRC1_ALPHA;
    case 18u: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return D3D11_BLEND_ONE;
  }
}

inline D3D11_BLEND_OP vk_to_blend_op(uint32_t op) {
  switch (op) {
    case 1u: return D3D11_BLEND_OP_SUBTRACT;
    case 2u: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 3u: return D3D11_BLEND_OP_MIN;
    case 4u: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
  }
}

inline D3D11_PRIMITIVE_TOPOLOGY vk_to_topo(uint32_t t) {
  switch (t) {
    case 0u: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 1u: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 2u: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 4u: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
}

inline D3D11_TEXTURE_ADDRESS_MODE vk_to_addr(uint32_t a) {
  switch (a) {
    case 1u: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2u: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3u: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 4u: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

inline void free_pipe_rec(pipe_rec& p) {
  if (p.vs) p.vs->Release();
  if (p.ps) p.ps->Release();
  if (p.cs) p.cs->Release();
  if (p.il) p.il->Release();
  if (p.rs) p.rs->Release();
  if (p.dss) p.dss->Release();
  if (p.bs) p.bs->Release();
  p.vs = nullptr;
  p.ps = nullptr;
  p.cs = nullptr;
  p.il = nullptr;
  p.rs = nullptr;
  p.dss = nullptr;
  p.bs = nullptr;
}

// .221 DIAG: D3D11 creation-size ledger -- the ~3.7GB unlogged
// video consumer. Every CreateTexture2D/3D and CreateBuffer in
// this backend feeds these counters; the live texture total is
// decremented in free_img_rec, the live buffer total is the
// bufs-map sum (b.bytes, as in TEX-OOM-DIAG). If tex_live stays
// ~50MB while VIDEO-SAMPLE climbs to 3.8GB, the consumer is
// OUTSIDE D3D11 creation (a driver reservation, the swapchain,
// or a Godot-side allocation the bridge never sees). If tex_live
// tracks VIDEO-SAMPLE, the top-1 texture + periodic summary name
// it. Plain statics, like the rest of this header: hk_sync_* is
// called from the single render-thread path that already
// mutates the imgs/bufs maps without a lock.
static u64 g_d3d_tex_live_bytes = 0;
static u64 g_d3d_tex_created_bytes = 0;
static u64 g_d3d_tex_created_n = 0;
static u64 g_d3d_buf_created_bytes = 0;
static u64 g_d3d_buf_created_n = 0;
static u64 g_d3d_top_bytes = 0;
static u32 g_d3d_top_w = 0, g_d3d_top_h = 0, g_d3d_top_d = 0;
static u32 g_d3d_top_fmt = 0;
// .222 DIAG: remaining video consumers -- the main
// depth-stencil texture (ds_tex, created in bind()) and the
// DXGI swapchain backbuffers (created by the swapchain,
// never through CreateTexture2D, so the .221 ledger never
// saw them). Plus the last VIDEO-SAMPLE local usage, so
// d3d_sizes_log can print the unaccounted residual (video
// usage minus every consumer the bridge logs).
static u64 g_d3d_ds_bytes = 0;
static u64 g_d3d_swap_bytes = 0;
static u32 g_d3d_swap_buffers = 0;
static u32 g_d3d_swap_w = 0, g_d3d_swap_h = 0, g_d3d_swap_fmt = 0;
static u64 g_video_last_local_mb = 0;
static bool g_video_sample_seen = false;
// .223 DIAG: cached buf_live total -- the .222 VIDEO-SAMPLE
// read g_vkrt.bufs (a std::map) from the watchdog thread
// while the render thread inserts/erases it: a data race
// (iterator invalidation + torn reads). The render thread
// now publishes a single atomic total (refreshed at every
// bufs mutation + inside d3d_sizes_log); the watchdog
// reads ONLY this cached value, never the map. Same
// pattern as the gap CPU feed.
// .239 DIAG: cached buffer-shadow total -- the CPU copies
// hk_sync_image/buffer keep per buffer (buf_rec::shadow),
// summed alongside buf_live in the same render-thread walk
// (VIDEO-SAMPLE shadow=). Same atomic pattern as buf_live:
// the watchdog reads ONLY the cache, never the map.
static std::atomic<u64> g_d3d_shadow_live_cached{0};
static std::atomic<u64> g_d3d_buf_live_cached{0};
static inline void d3d_buf_live_refresh() {
  u64 total = 0;
  u64 shadow_total = 0;
  for (const auto& kv : g_vkrt.bufs) {
    total += kv.second.bytes;
    shadow_total += kv.second.shadow.size();
  }
  g_d3d_buf_live_cached.store(total, std::memory_order_relaxed);
  g_d3d_shadow_live_cached.store(shadow_total, std::memory_order_relaxed);
}
static inline u32 dxgi_bpp(DXGI_FORMAT fmt) {
  switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
      return 8;
    case DXGI_FORMAT_R16_FLOAT:
      return 2;
    default:
      return 4;
  }
}
static inline u64 d3d_tex_est_bytes(u32 vk_fmt, UINT w, UINT h, UINT d,
                                    UINT mips, UINT layers) {
  u64 total = 0;
  for (UINT m = 0; m < mips; ++m) {
    const UINT mw = std::max<UINT>(1u, w >> m);
    const UINT mh = std::max<UINT>(1u, h >> m);
    const UINT md = std::max<UINT>(1u, d >> m);
    UINT rp = 0, sp = 0;
    calc_img_pitch(vk_fmt, mw, mh, &rp, &sp);
    total += (u64)sp * (u64)layers * (u64)md;
  }
  return total;
}
static inline void d3d_sizes_log(const char* why) {
  // .223 DIAG: refresh the cached buf_live total (render
  // thread) and read the cache -- d3d_sizes_log runs on the
  // render thread, so the map walk is safe here, but every
  // reader goes through the one cached value.
  d3d_buf_live_refresh();
  const u64 buf_live =
      g_d3d_buf_live_cached.load(std::memory_order_relaxed);
  // .222 DIAG: unaccounted residual -- video usage from the
  // last VIDEO-SAMPLE minus every consumer the bridge logs.
  // If resid stays ~3.6GB, the consumer is a driver/segment
  // reservation the bridge cannot see (check VIDEO-SAMPLE
  // resv=); if resid is ~0, the ledger accounts for it.
  long long resid_mb = -1;
  if (g_video_sample_seen) {
    const u64 accounted = g_d3d_tex_live_bytes + buf_live +
        g_d3d_ds_bytes + g_d3d_swap_bytes;
    resid_mb = (long long)g_video_last_local_mb -
        (long long)(accounted >> 20);
  }
  char sb[512];
  snprintf(sb, sizeof(sb),
           "D3D11-SIZES %s tex_live=%lluMB tex_created=%lluMB tex_n=%llu buf_live=%lluMB buf_created=%lluMB buf_n=%llu top=%ux%ux%u fmt=%u %lluMB ds=%lluMB swap=%lluMB bufs=%u resid=%lldMB",
           why,
           (unsigned long long)(g_d3d_tex_live_bytes >> 20),
           (unsigned long long)(g_d3d_tex_created_bytes >> 20),
           (unsigned long long)g_d3d_tex_created_n,
           (unsigned long long)(buf_live >> 20),
           (unsigned long long)(g_d3d_buf_created_bytes >> 20),
           (unsigned long long)g_d3d_buf_created_n,
           g_d3d_top_w, g_d3d_top_h, g_d3d_top_d, g_d3d_top_fmt,
           (unsigned long long)(g_d3d_top_bytes >> 20),
           (unsigned long long)(g_d3d_ds_bytes >> 20),
           (unsigned long long)(g_d3d_swap_bytes >> 20),
           g_d3d_swap_buffers, resid_mb);
  trt_core_log(sb);
}
// Records one texture creation; returns its estimated bytes so
// the caller can store them in img_rec::d3d_bytes.
static inline u64 d3d_tex_note_created(u32 vk_fmt, UINT w, UINT h, UINT d,
                                       UINT mips, UINT layers) {
  const u64 bytes = d3d_tex_est_bytes(vk_fmt, w, h, d, mips, layers);
  g_d3d_tex_live_bytes += bytes;
  g_d3d_tex_created_bytes += bytes;
  ++g_d3d_tex_created_n;
  if (bytes > g_d3d_top_bytes) {
    g_d3d_top_bytes = bytes;
    g_d3d_top_w = w; g_d3d_top_h = h; g_d3d_top_d = d; g_d3d_top_fmt = vk_fmt;
  }
  if (bytes >= (256ull << 20)) {
    char bb[256];
    snprintf(bb, sizeof(bb),
             "D3D11-SIZES BIGTEX %ux%ux%u fmt=%u mips=%u layers=%u %lluMB",
             w, h, d, vk_fmt, mips, layers,
             (unsigned long long)(bytes >> 20));
    trt_core_log(bb);
  }
  if ((g_d3d_tex_created_n % 256) == 0) d3d_sizes_log("periodic");
  return bytes;
}

inline void free_buf_rec(buf_rec& b) {
  if (b.srv) b.srv->Release();
  if (b.uav) b.uav->Release();
  if (b.cb) b.cb->Release();
  if (b.buf) b.buf->Release();
  b.srv = nullptr;
  b.uav = nullptr;
  b.cb = nullptr;
  b.buf = nullptr;
}

inline void free_img_rec(img_rec& im) {
  // .221 DIAG: keep the live texture total honest.
  if (im.d3d_bytes) {
    g_d3d_tex_live_bytes = (g_d3d_tex_live_bytes >= im.d3d_bytes)
        ? g_d3d_tex_live_bytes - im.d3d_bytes : 0;
    im.d3d_bytes = 0;
  }
  if (trt_esram_is_active() && (im.rtv || im.dsv)) {
    trt_esram_release_surface(im.desc.w, im.desc.h, (uint32_t)im.tex2d_fmt);
  }
  if (im.srv) im.srv->Release();
  if (im.rtv) im.rtv->Release();
  if (im.dsv) im.dsv->Release();
  if (im.uav) im.uav->Release();
  if (im.tex2d) im.tex2d->Release();
  if (im.tex3d) im.tex3d->Release();
  im.srv = nullptr;
  im.rtv = nullptr;
  im.dsv = nullptr;
  im.uav = nullptr;
  im.tex2d = nullptr;
  im.tex3d = nullptr;
}

inline bool compile_hlsl_blob(const char* src, uint32_t len, const char* profile,
                              std::vector<uint8_t>& out_blob) {
  if (!src || !len) return false;
  ID3DBlob* code = nullptr;
  if (longan::d3d_compile_cached(src, len, profile, "main", &code) && code) {
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    out_blob.assign(p, p + code->GetBufferSize());
    code->Release();
    return true;
  }
  ID3DBlob* errs = nullptr;
  HRESULT hr = D3DCompile(src, len, nullptr, nullptr, nullptr, "main", profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errs);
  if (FAILED(hr) || !code) {
    if (errs) {
      const char* emsg = (const char*)errs->GetBufferPointer();
      // Print all lines of the error message to catch both warnings and errors
      const char* ep = emsg;
      while (*ep) {
        const char* next = strchr(ep, '\n');
        int len = next ? (int)(next - ep) : (int)strlen(ep);
        if (len > 0) {
          char lbuf[512];
          snprintf(lbuf, sizeof(lbuf), "[vk_backend] %s: %.*s", profile, std::min(len, 480), ep);
          trt_core_log(lbuf);
        }
        if (!next) break;
        ep = next + 1;
      }

      // Extract line number from error
      int err_line = 0;
      const char* err_ptr = strstr(emsg, "error ");
      const char* lp = err_ptr ? strchr(err_ptr, '(') : strchr(emsg, '(');
      if (!lp && err_ptr) {
        // Look backwards for '('
        const char* b = err_ptr;
        while (b > emsg && *b != '(' && *b != '\n') b--;
        if (*b == '(') lp = b;
      }
      if (lp) err_line = atoi(lp + 1);
      if (err_line > 0) {
        int cur_line = 1;
        const char* p = src;
        const char* end = src + len;
        while (p < end) {
          const char* le = (const char*)memchr(p, '\n', end - p);
          if (!le) le = end;
          if (cur_line >= err_line - 3 && cur_line <= err_line + 3) {
            char lbuf[512];
            int llen = std::min<int>((int)(le - p), 480);
            snprintf(lbuf, sizeof(lbuf), "  L%d%s: %.*s",
                     cur_line, (cur_line == err_line) ? " >>>" : "    ", llen, p);
            trt_core_log(lbuf);
          }
          if (cur_line > err_line + 3) break;
          p = (le < end) ? le + 1 : end;
          cur_line++;
        }
      }
      errs->Release();
    }
    if (code) code->Release();
    return false;
  }
  if (errs) errs->Release();
  out_blob.resize(code->GetBufferSize());
  memcpy(out_blob.data(), code->GetBufferPointer(), code->GetBufferSize());
  code->Release();
  return true;
}

inline bool ensure_dyn_cb(ID3D11Buffer*& cb, uint32_t& cap, uint32_t want_bytes,
                          const void* data, uint32_t data_bytes) {
  if (!g_vkrt.dev || !g_vkrt.ctx) return false;
  const uint32_t aligned = std::max<uint32_t>(16u, (want_bytes + 15u) & ~15u);
  if (!cb || cap < aligned) {
    if (cb) { cb->Release(); cb = nullptr; }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = aligned;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_vkrt.dev->CreateBuffer(&bd, nullptr, &cb))) return false;
    // .221 DIAG: D3D11 creation-size ledger.
    g_d3d_buf_created_bytes += aligned;
    ++g_d3d_buf_created_n;
    cap = aligned;
  }
  if (data && data_bytes) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    ++g_d3d11c.map_c;  // .244 DIAG: D3D11 call/ret pair (see g_d3d11c)
    d3d11_note_last("Map");
    if (SUCCEEDED(g_vkrt.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      const uint32_t cp = std::min<uint32_t>(cap, data_bytes);
      memset(ms.pData, 0, cap);
      memcpy(ms.pData, data, cp);
      ++g_d3d11c.unmap_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("Unmap");
      g_vkrt.ctx->Unmap(cb, 0);
      ++g_d3d11c.unmap_r;
    }
    ++g_d3d11c.map_r;
  }
  return true;
}

inline ID3D11SamplerState* get_or_create_sampler(uint32_t minf, uint32_t magf,
                                                 uint32_t addru, uint32_t addrv) {
  uint64_t key = 0x517cc1b727220a95ull;
  key = fnv64_mix(key, minf);
  key = fnv64_mix(key, magf);
  key = fnv64_mix(key, addru);
  key = fnv64_mix(key, addrv);
  auto it = g_vkrt.samps.find(key);
  if (it != g_vkrt.samps.end()) return it->second;

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = (minf || magf) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                             : D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = vk_to_addr(addru);
  sd.AddressV = vk_to_addr(addrv);
  sd.AddressW = sd.AddressU;
  sd.MaxAnisotropy = 1;
  sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState* ss = nullptr;
  g_vkrt.dev->CreateSamplerState(&sd, &ss);
  g_vkrt.samps[key] = ss;
  return ss;
}

inline u32 hk_create_pipeline(const trt_vk_pipeline_desc* desc, void*) {
  if (!g_vkrt.initialized || !g_vkrt.dev || !desc) return 0;
  pipe_rec rec{};
  rec.is_compute = desc->is_compute != 0;

  if (rec.is_compute) {
    std::vector<uint8_t> blob;
    if (!compile_hlsl_blob(desc->cs_hlsl, desc->cs_len, "cs_5_0", blob)) return 0;
    if (FAILED(g_vkrt.dev->CreateComputeShader(blob.data(), blob.size(), nullptr, &rec.cs))) {
      return 0;
    }
    auto it = g_vkrt.pipes.find(desc->pipeline_id);
    if (it != g_vkrt.pipes.end()) free_pipe_rec(it->second);
    g_vkrt.pipes[desc->pipeline_id] = std::move(rec);
    return 1;
  }

  std::vector<uint8_t> ps_blob;
  if (!compile_hlsl_blob(desc->vs_hlsl, desc->vs_len, "vs_5_0", rec.vs_blob)) return 0;
  if (FAILED(g_vkrt.dev->CreateVertexShader(rec.vs_blob.data(), rec.vs_blob.size(), nullptr, &rec.vs))) {
    free_pipe_rec(rec);
    return 0;
  }
  if (desc->ps_hlsl && desc->ps_len) {
    if (!compile_hlsl_blob(desc->ps_hlsl, desc->ps_len, "ps_5_0", ps_blob) ||
        FAILED(g_vkrt.dev->CreatePixelShader(ps_blob.data(), ps_blob.size(), nullptr, &rec.ps))) {
      free_pipe_rec(rec);
      return 0;
    }
  }

  uint32_t inst_rates[8] = {};
  for (u32 i = 0; i < desc->vtx_bind_count && desc->vtx_binds; ++i) {
    const auto& vb = desc->vtx_binds[i];
    if (vb.binding < 8u) {
      rec.vb_strides[vb.binding] = vb.stride;
      inst_rates[vb.binding] = vb.input_rate;
    }
  }

  if (desc->vtx_attr_count > 0 && desc->vtx_attrs) {
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    elems.reserve(desc->vtx_attr_count);
    for (u32 i = 0; i < desc->vtx_attr_count; ++i) {
      const auto& a = desc->vtx_attrs[i];
      if (a.binding >= 8u) continue;
      D3D11_INPUT_ELEMENT_DESC e{};
      e.SemanticName = "TEXCOORD";
      e.SemanticIndex = a.location;
      e.Format = vk_to_dxgi_fmt(a.vk_format);
      e.InputSlot = a.binding;
      e.AlignedByteOffset = a.offset;
      e.InputSlotClass = (inst_rates[a.binding] == 1u)
                             ? D3D11_INPUT_PER_INSTANCE_DATA
                             : D3D11_INPUT_PER_VERTEX_DATA;
      e.InstanceDataStepRate = (inst_rates[a.binding] == 1u) ? 1u : 0u;
      elems.push_back(e);
    }
    if (!elems.empty()) {
      g_vkrt.dev->CreateInputLayout(elems.data(), (UINT)elems.size(),
                                    rec.vs_blob.data(), rec.vs_blob.size(), &rec.il);
    }
  }

  rec.topo = vk_to_topo(desc->topology);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = (desc->polygon_mode == 1u) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  if (desc->cull_mode == 1u) rd.CullMode = D3D11_CULL_FRONT;
  else if (desc->cull_mode == 2u) rd.CullMode = D3D11_CULL_BACK;
  else rd.CullMode = D3D11_CULL_NONE;
  rd.FrontCounterClockwise = (desc->front_face == 0u) ? TRUE : FALSE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = TRUE;
  g_vkrt.dev->CreateRasterizerState(&rd, &rec.rs);

  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = desc->depth_test ? TRUE : FALSE;
  dsd.DepthWriteMask = desc->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                         : D3D11_DEPTH_WRITE_MASK_ZERO;
  dsd.DepthFunc = vk_to_cmp(desc->depth_op);
  g_vkrt.dev->CreateDepthStencilState(&dsd, &rec.dss);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = desc->blend_en ? TRUE : FALSE;
  bd.RenderTarget[0].SrcBlend = vk_to_blend(desc->src_rgb, false);
  bd.RenderTarget[0].DestBlend = vk_to_blend(desc->dst_rgb, false);
  bd.RenderTarget[0].BlendOp = vk_to_blend_op(desc->op_rgb);
  bd.RenderTarget[0].SrcBlendAlpha = vk_to_blend(desc->src_a, true);
  bd.RenderTarget[0].DestBlendAlpha = vk_to_blend(desc->dst_a, true);
  bd.RenderTarget[0].BlendOpAlpha = vk_to_blend_op(desc->op_a);
  bd.RenderTarget[0].RenderTargetWriteMask = desc->color_mask & 0xFu;
  g_vkrt.dev->CreateBlendState(&bd, &rec.bs);

  auto it = g_vkrt.pipes.find(desc->pipeline_id);
  if (it != g_vkrt.pipes.end()) free_pipe_rec(it->second);
  g_vkrt.pipes[desc->pipeline_id] = std::move(rec);
  return 1;
}

inline void hk_destroy_pipeline(u64 pipeline_id, void*) {
  auto it = g_vkrt.pipes.find(pipeline_id);
  if (it != g_vkrt.pipes.end()) {
    free_pipe_rec(it->second);
    g_vkrt.pipes.erase(it);
  }
}

inline u32 hk_sync_buffer(u64 buf_id, u32 usage, const void* data, u64 bytes, void*) {
  if (!g_vkrt.initialized || !g_vkrt.dev) return 0;
  if (bytes > (16ull * 1024ull * 1024ull)) {
    char bb[128];
    snprintf(bb, sizeof(bb), "[vk_backend] hk_sync_buffer LARGE buf=%llu bytes=%llu usage=0x%x",
             (unsigned long long)buf_id, (unsigned long long)bytes, usage);
    trt_core_log(bb);
  }
  const uint64_t sz = std::max<uint64_t>(16ull, bytes);
  buf_rec& b = g_vkrt.bufs[buf_id];

  bool need_realloc = !b.buf || b.bytes < sz || b.usage != usage;
  if (need_realloc) {
    free_buf_rec(b);
    b.usage = usage;
    b.bytes = sz;

    const bool is_cb = (usage & 0x10u) != 0u;      // VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
    const bool is_storage = (usage & 0x20u) != 0u; // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    UINT bind_flags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    if (is_storage || !is_cb) bind_flags |= D3D11_BIND_SHADER_RESOURCE;
    if (is_storage) bind_flags |= D3D11_BIND_UNORDERED_ACCESS;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)std::min<uint64_t>(sz, 0x7FFFFFFFu);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind_flags;
    if (bind_flags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) {
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      bd.ByteWidth = (bd.ByteWidth + 3u) & ~3u;
    }

    std::vector<uint8_t> init_pad;
    D3D11_SUBRESOURCE_DATA init{};
    // Prefer UpdateSubresource over a full-size init_pad copy (the
    // transient 3rd copy of every buffer). Fall back to init data
    // only if no immediate context is available.
    if (data && bytes && !g_vkrt.ctx) {
      init_pad.assign(bd.ByteWidth, 0);
      memcpy(init_pad.data(), data, (size_t)std::min<uint64_t>(bytes, bd.ByteWidth));
      init.pSysMem = init_pad.data();
    }
    g_vkrt.dev->CreateBuffer(&bd, init.pSysMem ? &init : nullptr, &b.buf);
    // .221 DIAG: D3D11 creation-size ledger.
    if (b.buf) {
      g_d3d_buf_created_bytes += bd.ByteWidth;
      ++g_d3d_buf_created_n;
    }
    if (b.buf && data && bytes && g_vkrt.ctx && !init.pSysMem) {
      D3D11_BOX box{0, 0, 0, (UINT)std::min<uint64_t>(bytes, bd.ByteWidth), 1, 1};
      ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("UpdateSubresource");
      g_vkrt.ctx->UpdateSubresource(b.buf, 0, &box, data, 0, 0);
      ++g_d3d11c.upd_r;
    }

    if (b.buf && (bind_flags & D3D11_BIND_SHADER_RESOURCE)) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sd.BufferEx.NumElements = std::max<UINT>(1u, bd.ByteWidth / 4u);
      sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      ++g_d3d11c.srv_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("CreateShaderResourceView");
      g_vkrt.dev->CreateShaderResourceView(b.buf, &sd, &b.srv);
      ++g_d3d11c.srv_r;
    }
    if (b.buf && (bind_flags & D3D11_BIND_UNORDERED_ACCESS)) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = std::max<UINT>(1u, bd.ByteWidth / 4u);
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      ++g_d3d11c.uav_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("CreateUnorderedAccessView");
      g_vkrt.dev->CreateUnorderedAccessView(b.buf, &ud, &b.uav);
      ++g_d3d11c.uav_r;
    }
    if (is_cb) {
      D3D11_BUFFER_DESC cbd{};
      cbd.ByteWidth = (UINT)std::min<uint64_t>((sz + 15ull) & ~15ull, 65536ull);
      cbd.Usage = D3D11_USAGE_DEFAULT;
      cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (data && bytes && !g_vkrt.ctx) {
        init_pad.assign(cbd.ByteWidth, 0);
        memcpy(init_pad.data(), data, (size_t)std::min<uint64_t>(bytes, cbd.ByteWidth));
        init.pSysMem = init_pad.data();
        g_vkrt.dev->CreateBuffer(&cbd, &init, &b.cb);
        // .221 DIAG: D3D11 creation-size ledger.
        if (b.cb) {
          g_d3d_buf_created_bytes += cbd.ByteWidth;
          ++g_d3d_buf_created_n;
        }
      } else {
        g_vkrt.dev->CreateBuffer(&cbd, nullptr, &b.cb);
        // .221 DIAG: D3D11 creation-size ledger.
        if (b.cb) {
          g_d3d_buf_created_bytes += cbd.ByteWidth;
          ++g_d3d_buf_created_n;
        }
        if (b.cb && data && bytes && g_vkrt.ctx) {
          if (bytes >= cbd.ByteWidth) {
            ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
            d3d11_note_last("UpdateSubresource");
            g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, data, 0, 0);
            ++g_d3d11c.upd_r;
          } else {
            std::vector<uint8_t> pad(cbd.ByteWidth, 0);
            memcpy(pad.data(), data, (size_t)bytes);
            ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
            d3d11_note_last("UpdateSubresource");
            g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, pad.data(), 0, 0);
            ++g_d3d11c.upd_r;
          }
        }
      }
    }
  } else if (data && bytes && g_vkrt.ctx) {
    if (b.buf) {
      D3D11_BOX box{0, 0, 0, (UINT)std::min<uint64_t>(bytes, b.bytes), 1, 1};
      ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("UpdateSubresource");
      g_vkrt.ctx->UpdateSubresource(b.buf, 0, &box, data, 0, 0);
      ++g_d3d11c.upd_r;
    }
    if (b.cb && bytes <= 65536ull) {
      D3D11_BUFFER_DESC cbd{};
      b.cb->GetDesc(&cbd);
      if (bytes >= cbd.ByteWidth) {
        ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("UpdateSubresource");
        g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, data, 0, 0);
        ++g_d3d11c.upd_r;
      } else {
        std::vector<uint8_t> pad(cbd.ByteWidth, 0);
        memcpy(pad.data(), data, (size_t)bytes);
        ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("UpdateSubresource");
        g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, pad.data(), 0, 0);
        ++g_d3d11c.upd_r;
      }
    }
  }

  if (data && bytes) {
    if (b.shadow.size() < bytes) b.shadow.resize((size_t)bytes);
    memcpy(b.shadow.data(), data, (size_t)bytes);
  }
  // .223 DIAG: publish the new buf_live total (bufs mutated
  // above: insert / realloc / usage change).
  d3d_buf_live_refresh();
  return 1;
}

inline void hk_destroy_buffer(u64 buf_id, void*) {
  auto it = g_vkrt.bufs.find(buf_id);
  if (it != g_vkrt.bufs.end()) {
    free_buf_rec(it->second);
    g_vkrt.bufs.erase(it);
  }
  // .223 DIAG: publish the new buf_live total.
  d3d_buf_live_refresh();
}

inline u32 hk_sync_image(u64 img_id, const trt_vk_image_desc* desc, const void* data,
                         u64 bytes, u32 mip, u32 layer, void*) {
  if (!g_vkrt.initialized || !g_vkrt.dev || !desc) return 0;
  ++g_d3d11c.sync_c;  // .244 DIAG: hk_sync_image entries (per-texture call sequence)
  img_rec& im = g_vkrt.imgs[img_id];
  if (desc->is_swapchain) {
    free_img_rec(im);
    im.desc = *desc;
    return 1;
  }

  const bool same = im.tex2d && im.desc.w == desc->w && im.desc.h == desc->h &&
                    im.desc.depth == desc->depth && im.desc.mips == desc->mips &&
                    im.desc.layers == desc->layers &&
                    im.desc.vk_format == desc->vk_format &&
                    im.desc.img_type == desc->img_type &&
                    im.desc.usage == desc->usage &&
                    im.desc.flags == desc->flags;
  if (!same) {
    free_img_rec(im);
    im.desc = *desc;
    ++g_d3d11c.newtex_c;  // .244 DIAG: new texture description (CreateTexture2D/3D follows)
    // .245 DIAG+MITIGATION: once the device is removed, further
    // texture creations fail AND still grow driver regions (.244:
    // 726 failed creations after removal). Skip the creation and
    // take the existing FAILED(hr) path with hr/hr3d =
    // DXGI_ERROR_DEVICE_REMOVED (views/uploads are skipped
    // because im.tex2d/tex3d stay null).
    const HRESULT devrem_now =
        g_vkrt.dev ? g_vkrt.dev->GetDeviceRemovedReason() : S_OK;
    if (devrem_now != S_OK) ++g_d3d11c.texskip_c;
    const bool depth = is_depth_vk_fmt(desc->vk_format);
    const bool is_cube = ((desc->flags & 0x10u) != 0u) && desc->layers >= 6u;

    UINT bind_flags = D3D11_BIND_SHADER_RESOURCE;
    if (depth) {
      bind_flags |= D3D11_BIND_DEPTH_STENCIL;
    } else if (!is_bc_vk_fmt(desc->vk_format)) {
      if (desc->usage & 0x10u) bind_flags |= D3D11_BIND_RENDER_TARGET;
      if (desc->usage & 0x08u) bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    UINT max_mips = 1;
    UINT max_dim = std::max<UINT>(desc->w, desc->h);
    while (max_dim > 1) { max_mips++; max_dim >>= 1; }
    const UINT mips = std::min<UINT>(std::max<UINT>(1u, desc->mips), max_mips);
    const UINT layers = std::max<UINT>(1u, desc->layers);
    const DXGI_FORMAT tex_fmt = vk_to_dxgi_fmt(desc->vk_format, depth);

    if (desc->img_type == 2u) {
      D3D11_TEXTURE3D_DESC td{};
      td.Width = std::max<UINT>(1u, desc->w);
      td.Height = std::max<UINT>(1u, desc->h);
      td.Depth = std::max<UINT>(1u, desc->depth);
      td.MipLevels = mips;
      td.Format = tex_fmt;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = bind_flags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
      HRESULT hr3d = S_OK;
      if (devrem_now != S_OK) {
        hr3d = (HRESULT)0x887A0006;  // DXGI_ERROR_DEVICE_REMOVED
      } else {
        ++g_d3d11c.tex3d_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("CreateTexture3D");
        hr3d = g_vkrt.dev->CreateTexture3D(&td, nullptr, &im.tex3d);
        ++g_d3d11c.tex3d_r;
      }
      if (FAILED(hr3d)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[vk_backend] CreateTexture3D failed vk_fmt=%u dxgi=%u %ux%ux%u mips=%u bind=0x%x hr=0x%08x",
                 desc->vk_format, (unsigned)tex_fmt, td.Width, td.Height, td.Depth, mips, bind_flags, (unsigned)hr3d);
        trt_core_log(buf);
      }
      if (SUCCEEDED(hr3d) && im.tex3d) {
        // .221 DIAG: D3D11 creation-size ledger.
        im.d3d_bytes = d3d_tex_note_created(desc->vk_format, td.Width,
                                            td.Height, td.Depth, mips, 1u);
        if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
          ++g_d3d11c.srv_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateShaderResourceView");
          g_vkrt.dev->CreateShaderResourceView(im.tex3d, nullptr, &im.srv);
          ++g_d3d11c.srv_r;
        }
        if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
          ++g_d3d11c.uav_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateUnorderedAccessView");
          g_vkrt.dev->CreateUnorderedAccessView(im.tex3d, nullptr, &im.uav);
          ++g_d3d11c.uav_r;
        }
      }
    } else {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = std::max<UINT>(1u, desc->w);
      td.Height = std::max<UINT>(1u, desc->h);
      td.MipLevels = mips;
      td.ArraySize = layers;
      td.Format = tex_fmt;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = bind_flags;
      if (is_cube) td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
      HRESULT hr = S_OK;
      if (devrem_now != S_OK) {
        hr = (HRESULT)0x887A0006;  // DXGI_ERROR_DEVICE_REMOVED
      } else {
        ++g_d3d11c.tex2d_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("CreateTexture2D");
        hr = g_vkrt.dev->CreateTexture2D(&td, nullptr, &im.tex2d);
        ++g_d3d11c.tex2d_r;
        if (FAILED(hr) && (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
          td.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
          bind_flags = td.BindFlags;
          ++g_d3d11c.tex2d_c;  // .244 DIAG: D3D11 call/ret pair (UAV retry)
          d3d11_note_last("CreateTexture2D");
          hr = g_vkrt.dev->CreateTexture2D(&td, nullptr, &im.tex2d);
          ++g_d3d11c.tex2d_r;
        }
      }
      if (FAILED(hr)) {
        HRESULT rem = g_vkrt.dev ? g_vkrt.dev->GetDeviceRemovedReason() : S_OK;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[vk_backend] CreateTexture2D failed vk_fmt=%u dxgi=%u %ux%u mips=%u layers=%u bind=0x%x hr=0x%08x rem=0x%08x",
                 desc->vk_format, (unsigned)tex_fmt, td.Width, td.Height, mips, layers, bind_flags, (unsigned)hr, (unsigned)rem);
        trt_core_log(buf);
        // .219 DIAG: tex-fail correlation -- count this failure and
        // pair it with the main-thread CV-wait count so far. If the
        // main thread's AFAFC0 waits only start after a burst of
        // failures, the OOM is upstream of the stall: the frame task
        // waits on a texture that can never arrive.
        {
          const uint64_t tex_n = gap_trace_note_tex_fail();
          char tb[128];
          snprintf(tb, sizeof(tb), "gap: texfail#%llu maincvwaits=%llu",
                   (unsigned long long)tex_n,
                   (unsigned long long)gap_trace_main_cv_waits());
          trt_core_log(tb);
        }
        // First-failure census: is this app memory, device memory, or a leak?
        static bool tex_oom_diag_done = false;
        if (!tex_oom_diag_done) {
          tex_oom_diag_done = true;
          MEMORYSTATUSEX ms{};
          ms.dwLength = sizeof(ms);
          GlobalMemoryStatusEx(&ms);
          size_t n_img = g_vkrt.imgs.size(), n_buf = g_vkrt.bufs.size(), n_pipe = g_vkrt.pipes.size();
          uint64_t sum_buf = 0, sum_img = 0;
          for (const auto& kv : g_vkrt.bufs) sum_buf += kv.second.bytes;
          for (const auto& kv : g_vkrt.imgs) sum_img += (uint64_t)kv.second.desc.w * kv.second.desc.h * 4;
          char db[512];
          snprintf(db, sizeof(db),
                   "[vk_backend] TEX-OOM-DIAG avail_phys=%lluMB total_phys=%lluMB imgs=%zu(~%lluMB) bufs=%zu(%lluMB) pipes=%zu",
                   (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullTotalPhys >> 20),
                   n_img, (unsigned long long)(sum_img >> 20), n_buf, (unsigned long long)(sum_buf >> 20), n_pipe);
          trt_core_log(db);
          // .221 DIAG: D3D11 creation totals at the OOM point --
          // if tex_live is ~50MB here, the 3.8GB video usage
          // did NOT come from D3D11 texture creation.
          d3d_sizes_log("oom");
          // Guest ledger at the OOM point: the decisive H1 vs H2 line.
          // ledger ~=300-800MB => the 3.8GB video usage is the consumer
          // (H1); ledger ~=3.9GB => Godot's heap is the consumer (H2).
          char db2[512];
          trt_mem_budget_report(db2, sizeof(db2));
          char db2p[640];
          snprintf(db2p, sizeof(db2p), "[vk_backend] TEX-OOM-DIAG ledger: %s", db2);
          trt_core_log(db2p);
          // Video-memory budget vs usage, via the adapter (IDXGIAdapter3
          // owns QueryVideoMemoryInfo; it is not a device method).
          IDXGIDevice* dxgi_dev = nullptr;
          if (g_vkrt.dev && SUCCEEDED(g_vkrt.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev)) && dxgi_dev) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
              IDXGIAdapter3* adapter3 = nullptr;
              if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3)) && adapter3) {
                DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) {
                  char vb[256];
                  snprintf(vb, sizeof(vb),
                           "[vk_backend] TEX-OOM-DIAG video LOCAL budget=%lluMB usage=%lluMB avail=%lluMB",
                           (unsigned long long)(vmi.Budget >> 20), (unsigned long long)(vmi.CurrentUsage >> 20),
                           (unsigned long long)(vmi.AvailableForReservation >> 20));
                  trt_core_log(vb);
                }
                adapter3->Release();
              }
              adapter->Release();
            }
            dxgi_dev->Release();
          }
        }
      }
      if (SUCCEEDED(hr) && im.tex2d) {
        im.tex2d_fmt = (uint32_t)td.Format;
        // .221 DIAG: D3D11 creation-size ledger.
        im.d3d_bytes = d3d_tex_note_created(desc->vk_format, td.Width,
                                            td.Height, 1u, mips, layers);
        if (trt_esram_is_active() && (bind_flags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
          trt_esram_qualify_and_alloc(td.Width, td.Height, (uint32_t)td.Format, bind_flags, "vk_tex2d");
        }
        if (bind_flags & D3D11_BIND_SHADER_RESOURCE) {
          D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
          sd.Format = depth ? depth_srv_fmt(desc->vk_format) : tex_fmt;
          if (is_cube) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = mips;
          } else if (layers > 1u) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = mips;
            sd.Texture2DArray.ArraySize = layers;
          } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = mips;
          }
          ++g_d3d11c.srv_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateShaderResourceView");
          g_vkrt.dev->CreateShaderResourceView(im.tex2d, &sd, &im.srv);
          ++g_d3d11c.srv_r;
        }
        if (bind_flags & D3D11_BIND_RENDER_TARGET) {
          ++g_d3d11c.rtv_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateRenderTargetView");
          g_vkrt.dev->CreateRenderTargetView(im.tex2d, nullptr, &im.rtv);
          ++g_d3d11c.rtv_r;
        }
        if (bind_flags & D3D11_BIND_DEPTH_STENCIL) {
          D3D11_DEPTH_STENCIL_VIEW_DESC dsd{};
          dsd.Format = vk_to_dxgi_fmt(desc->vk_format, false);
          dsd.ViewDimension = (layers > 1u) ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY
                                            : D3D11_DSV_DIMENSION_TEXTURE2D;
          if (layers > 1u) dsd.Texture2DArray.ArraySize = 1;
          ++g_d3d11c.dsv_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateDepthStencilView");
          g_vkrt.dev->CreateDepthStencilView(im.tex2d, &dsd, &im.dsv);
          ++g_d3d11c.dsv_r;
        }
        if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) {
          ++g_d3d11c.uav_c;  // .244 DIAG: D3D11 call/ret pair
          d3d11_note_last("CreateUnorderedAccessView");
          g_vkrt.dev->CreateUnorderedAccessView(im.tex2d, nullptr, &im.uav);
          ++g_d3d11c.uav_r;
        }
      }
    }
  }

  if (data && bytes && g_vkrt.ctx && (im.tex2d || im.tex3d)) {
    const UINT mips = std::max<UINT>(1u, desc->mips);
    const UINT mw = std::max<UINT>(1u, desc->w >> mip);
    const UINT mh = std::max<UINT>(1u, desc->h >> mip);
    const UINT sub = D3D11CalcSubresource(mip, layer, mips);
    ID3D11Resource* res = im.tex3d ? (ID3D11Resource*)im.tex3d : (ID3D11Resource*)im.tex2d;
    if ((desc->vk_format == 104u || desc->vk_format == 105u || desc->vk_format == 106u) && data) {
      std::vector<uint32_t> converted((size_t)mw * (size_t)mh * 4u);
      const uint32_t* src3 = (const uint32_t*)data;
      const size_t total_px = (size_t)mw * (size_t)mh;
      for (size_t px = 0; px < total_px && (px + 1) * 3 * sizeof(uint32_t) <= bytes; ++px) {
        converted[px * 4 + 0] = src3[px * 3 + 0];
        converted[px * 4 + 1] = src3[px * 3 + 1];
        converted[px * 4 + 2] = src3[px * 3 + 2];
        converted[px * 4 + 3] = (desc->vk_format == 106u) ? 0x3F800000u /* 1.0f */ : 1u;
      }
      UINT rp = mw * 16u;
      UINT sp = rp * mh;
      ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("UpdateSubresource");
      g_vkrt.ctx->UpdateSubresource(res, sub, nullptr, converted.data(), rp, sp);
      ++g_d3d11c.upd_r;
    } else {
      UINT rp = 0, sp = 0;
      calc_img_pitch(desc->vk_format, mw, mh, &rp, &sp);
      if (bytes < (u64)sp) {
        std::vector<uint8_t> pad((size_t)sp, 0);
        memcpy(pad.data(), data, (size_t)bytes);
        ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("UpdateSubresource");
        g_vkrt.ctx->UpdateSubresource(res, sub, nullptr, pad.data(), rp, sp);
        ++g_d3d11c.upd_r;
      } else {
        ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
        d3d11_note_last("UpdateSubresource");
        g_vkrt.ctx->UpdateSubresource(res, sub, nullptr, data, rp, sp);
        ++g_d3d11c.upd_r;
      }
    }
  }
  return 1;
}

inline void hk_destroy_image(u64 img_id, void*) {
  auto it = g_vkrt.imgs.find(img_id);
  if (it != g_vkrt.imgs.end()) {
    free_img_rec(it->second);
    g_vkrt.imgs.erase(it);
  }
}

inline void hk_begin_render_pass(const trt_vk_rp_begin* rp, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !rp) return;

  ID3D11ShaderResourceView* null_srvs[16] = {};
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("VSSetShaderResources");
  g_vkrt.ctx->VSSetShaderResources(0, 16, null_srvs);
  ++g_d3d11c.state_r;
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("PSSetShaderResources");
  g_vkrt.ctx->PSSetShaderResources(0, 16, null_srvs);
  ++g_d3d11c.state_r;

  ID3D11RenderTargetView* rtvs[8] = {};
  UINT rtv_cnt = std::min<UINT>(8u, rp->color_count);
  bool uses_swapchain = false;
  for (UINT i = 0; i < rtv_cnt; ++i) {
    const u64 id = rp->color_imgs[i];
    auto it = g_vkrt.imgs.find(id);
    if (it != g_vkrt.imgs.end()) {
      if (it->second.desc.is_swapchain) {
        rtvs[i] = g_vkrt.main_rtv;
        uses_swapchain = true;
      } else {
        rtvs[i] = it->second.rtv;
      }
    }
    if (rtvs[i] && (rp->clear_color_mask & (1u << i))) {
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("ClearRenderTargetView");
      g_vkrt.ctx->ClearRenderTargetView(rtvs[i], rp->clear_rgba[i]);
      ++g_d3d11c.state_r;
    }
  }
  if (rtv_cnt == 0 && !rp->depth_img) {
    rtvs[0] = g_vkrt.main_rtv;
    rtv_cnt = 1;
    uses_swapchain = true;
  }

  ID3D11DepthStencilView* dsv = nullptr;
  if (rp->depth_img) {
    auto it = g_vkrt.imgs.find(rp->depth_img);
    if (it != g_vkrt.imgs.end()) dsv = it->second.dsv;
  } else if (uses_swapchain) {
    dsv = g_vkrt.main_dsv;
    if (dsv) {
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("ClearDepthStencilView");
      g_vkrt.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
      ++g_d3d11c.state_r;
    }
  }
  if (dsv && (rp->clear_depth || rp->clear_stencil)) {
    UINT cf = 0;
    if (rp->clear_depth) cf |= D3D11_CLEAR_DEPTH;
    if (rp->clear_stencil) cf |= D3D11_CLEAR_STENCIL;
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("ClearDepthStencilView");
    g_vkrt.ctx->ClearDepthStencilView(dsv, cf, rp->depth_val, (UINT8)rp->stencil_val);
    ++g_d3d11c.state_r;
  }

  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("OMSetRenderTargets");
  g_vkrt.ctx->OMSetRenderTargets(rtv_cnt, rtvs, dsv);
  ++g_d3d11c.state_r;

  D3D11_VIEWPORT vp{};
  vp.TopLeftX = (FLOAT)rp->area_x;
  vp.TopLeftY = (FLOAT)rp->area_y;
  vp.Width = (FLOAT)(rp->area_w ? rp->area_w : g_vkrt.width);
  vp.Height = (FLOAT)(rp->area_h ? rp->area_h : g_vkrt.height);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("RSSetViewports");
  g_vkrt.ctx->RSSetViewports(1, &vp);
  ++g_d3d11c.state_r;

  D3D11_RECT sc{rp->area_x, rp->area_y,
                rp->area_x + (LONG)(rp->area_w ? rp->area_w : g_vkrt.width),
                rp->area_y + (LONG)(rp->area_h ? rp->area_h : g_vkrt.height)};
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("RSSetScissorRects");
  g_vkrt.ctx->RSSetScissorRects(1, &sc);
  ++g_d3d11c.state_r;
}

inline void hk_end_render_pass(void*) {
  // Keep bound targets until next pass or present.
}

inline void bind_descriptor_table(const trt_vk_desc_binding* bindings, u32 count,
                                  bool is_compute, std::vector<u32>* uav_slots_to_clear) {
  if (!bindings || !count || !g_vkrt.ctx) return;

  for (u32 i = 0; i < count; ++i) {
    const auto& b = bindings[i];
    const bool to_cs = is_compute || ((b.stage_mask & 0x20u) != 0u);
    const bool to_vs = !is_compute && (b.stage_mask == 0u || (b.stage_mask & 0x01u) != 0u);
    const bool to_ps = !is_compute && (b.stage_mask == 0u || (b.stage_mask & 0x10u) != 0u);
    const UINT reg = b.hlsl_reg;

    auto bind_cb = [&](ID3D11Buffer* cb) {
      if (!cb || reg >= 14u) return;
      if (to_cs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("CSSetConstantBuffers");
        g_vkrt.ctx->CSSetConstantBuffers(reg, 1, &cb);
        ++g_d3d11c.state_r;
      }
      if (to_vs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("VSSetConstantBuffers");
        g_vkrt.ctx->VSSetConstantBuffers(reg, 1, &cb);
        ++g_d3d11c.state_r;
      }
      if (to_ps) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("PSSetConstantBuffers");
        g_vkrt.ctx->PSSetConstantBuffers(reg, 1, &cb);
        ++g_d3d11c.state_r;
      }
    };
    auto bind_srv = [&](ID3D11ShaderResourceView* srv) {
      if (reg >= 128u) return;
      if (to_cs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("CSSetShaderResources");
        g_vkrt.ctx->CSSetShaderResources(reg, 1, &srv);
        ++g_d3d11c.state_r;
      }
      if (to_vs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("VSSetShaderResources");
        g_vkrt.ctx->VSSetShaderResources(reg, 1, &srv);
        ++g_d3d11c.state_r;
      }
      if (to_ps) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("PSSetShaderResources");
        g_vkrt.ctx->PSSetShaderResources(reg, 1, &srv);
        ++g_d3d11c.state_r;
      }
    };
    auto bind_smp = [&](UINT sreg, ID3D11SamplerState* smp) {
      if (!smp || sreg >= 16u) return;
      if (to_cs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("CSSetSamplers");
        g_vkrt.ctx->CSSetSamplers(sreg, 1, &smp);
        ++g_d3d11c.state_r;
      }
      if (to_vs) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("VSSetSamplers");
        g_vkrt.ctx->VSSetSamplers(sreg, 1, &smp);
        ++g_d3d11c.state_r;
      }
      if (to_ps) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("PSSetSamplers");
        g_vkrt.ctx->PSSetSamplers(sreg, 1, &smp);
        ++g_d3d11c.state_r;
      }
    };

    if (b.kind == SPV_BIND_CBV) {
      auto bit = g_vkrt.bufs.find(b.buf_id);
      if (bit != g_vkrt.bufs.end()) {
        const auto& rec = bit->second;
        const bool whole = (b.buf_off == 0) &&
                           (b.buf_range == 0 || b.buf_range >= rec.bytes);
        if (whole && rec.cb) {
          bind_cb(rec.cb);
        } else if (reg < 14u && b.buf_off < rec.shadow.size()) {
          u64 avail = (u64)(rec.shadow.size() - b.buf_off);
          u32 want = (u32)std::min<u64>(
              b.buf_range ? b.buf_range : avail, std::min<u64>(avail, 65536ull));
          if (ensure_dyn_cb(g_vkrt.slice_cbs[reg], g_vkrt.slice_cb_caps[reg], want,
                            rec.shadow.data() + b.buf_off, want)) {
            bind_cb(g_vkrt.slice_cbs[reg]);
          }
        }
      }
    } else if (b.kind == SPV_BIND_SRV_BUFFER) {
      auto bit = g_vkrt.bufs.find(b.buf_id);
      if (bit != g_vkrt.bufs.end()) bind_srv(bit->second.srv);
    } else if (b.kind == SPV_BIND_UAV_BUFFER) {
      auto bit = g_vkrt.bufs.find(b.buf_id);
      if (bit != g_vkrt.bufs.end() && bit->second.uav && is_compute && reg < 8u) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("CSSetUnorderedAccessViews");
        g_vkrt.ctx->CSSetUnorderedAccessViews(reg, 1, &bit->second.uav, nullptr);
        ++g_d3d11c.state_r;
        if (uav_slots_to_clear) uav_slots_to_clear->push_back(reg);
      }
    } else if (b.kind == SPV_BIND_TEXTURE) {
      auto iit = g_vkrt.imgs.find(b.img_id);
      if (iit != g_vkrt.imgs.end()) bind_srv(iit->second.srv);
    } else if (b.kind == SPV_BIND_SAMPLER) {
      bind_smp(reg, get_or_create_sampler(b.min_filter, b.mag_filter, b.addr_u, b.addr_v));
    } else if (b.kind == SPV_BIND_COMBINED_IMG_SMP) {
      auto iit = g_vkrt.imgs.find(b.img_id);
      if (iit != g_vkrt.imgs.end()) bind_srv(iit->second.srv);
      bind_smp(b.smp_reg, get_or_create_sampler(b.min_filter, b.mag_filter, b.addr_u, b.addr_v));
    } else if (b.kind == SPV_BIND_STORAGE_IMG) {
      auto iit = g_vkrt.imgs.find(b.img_id);
      if (iit != g_vkrt.imgs.end() && iit->second.uav && is_compute && reg < 8u) {
        ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
        d3d11_note_last("CSSetUnorderedAccessViews");
        g_vkrt.ctx->CSSetUnorderedAccessViews(reg, 1, &iit->second.uav, nullptr);
        ++g_d3d11c.state_r;
        if (uav_slots_to_clear) uav_slots_to_clear->push_back(reg);
      }
    }
  }
}

inline void hk_draw(const trt_vk_draw_cmd* cmd, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !cmd) return;
  auto pit = g_vkrt.pipes.find(cmd->pipeline_id);
  if (pit == g_vkrt.pipes.end()) return;
  const pipe_rec& p = pit->second;
  if (!p.vs) return;

  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("VSSetShader");
  g_vkrt.ctx->VSSetShader(p.vs, nullptr, 0);
  ++g_d3d11c.state_r;
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("PSSetShader");
  g_vkrt.ctx->PSSetShader(p.ps, nullptr, 0);
  ++g_d3d11c.state_r;
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("IASetInputLayout");
  g_vkrt.ctx->IASetInputLayout(p.il);
  ++g_d3d11c.state_r;
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("IASetPrimitiveTopology");
  g_vkrt.ctx->IASetPrimitiveTopology(p.topo);
  ++g_d3d11c.state_r;
  if (p.rs) {
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("RSSetState");
    g_vkrt.ctx->RSSetState(p.rs);
    ++g_d3d11c.state_r;
  }
  if (p.dss) {
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("OMSetDepthStencilState");
    g_vkrt.ctx->OMSetDepthStencilState(p.dss, 0);
    ++g_d3d11c.state_r;
  }
  if (p.bs) {
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("OMSetBlendState");
    g_vkrt.ctx->OMSetBlendState(p.bs, cmd->blend_const, 0xFFFFFFFFu);
    ++g_d3d11c.state_r;
  }

  if (cmd->has_vp && cmd->vp_w > 0.0f && fabsf(cmd->vp_h) > 0.0f) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = cmd->vp_x;
    vp.TopLeftY = (cmd->vp_h < 0.0f) ? (cmd->vp_y + cmd->vp_h) : cmd->vp_y;
    vp.Width = cmd->vp_w;
    vp.Height = fabsf(cmd->vp_h);
    vp.MinDepth = cmd->vp_min_z;
    vp.MaxDepth = cmd->vp_max_z;
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("RSSetViewports");
    g_vkrt.ctx->RSSetViewports(1, &vp);
    ++g_d3d11c.state_r;
  }
  if (cmd->has_sc && cmd->sc_w > 0u && cmd->sc_h > 0u) {
    D3D11_RECT sc{cmd->sc_x, cmd->sc_y,
                  cmd->sc_x + (LONG)cmd->sc_w, cmd->sc_y + (LONG)cmd->sc_h};
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("RSSetScissorRects");
    g_vkrt.ctx->RSSetScissorRects(1, &sc);
    ++g_d3d11c.state_r;
  }

  if (cmd->vb_count > 0) {
    ID3D11Buffer* vbs[8] = {};
    UINT strides[8] = {};
    UINT offs[8] = {};
    const UINT cnt = std::min<UINT>(8u, cmd->vb_count);
    for (UINT i = 0; i < cnt; ++i) {
      auto bit = g_vkrt.bufs.find(cmd->vb_ids[i]);
      if (bit != g_vkrt.bufs.end()) vbs[i] = bit->second.buf;
      strides[i] = p.vb_strides[i];
      offs[i] = (UINT)cmd->vb_offs[i];
    }
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("IASetVertexBuffers");
    g_vkrt.ctx->IASetVertexBuffers(0, cnt, vbs, strides, offs);
    ++g_d3d11c.state_r;
  }

  if (cmd->indexed) {
    ID3D11Buffer* ib = nullptr;
    auto bit = g_vkrt.bufs.find(cmd->ib_id);
    if (bit != g_vkrt.bufs.end()) ib = bit->second.buf;
    if (!ib) return;
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("IASetIndexBuffer");
    g_vkrt.ctx->IASetIndexBuffer(
        ib, cmd->idx_is_u32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
        (UINT)cmd->ib_off);
    ++g_d3d11c.state_r;
  }

  if (cmd->push_data && cmd->push_bytes && cmd->push_reg < 14u) {
    if (ensure_dyn_cb(g_vkrt.push_cb, g_vkrt.push_cb_cap, cmd->push_bytes,
                      cmd->push_data, cmd->push_bytes)) {
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("VSSetConstantBuffers");
      g_vkrt.ctx->VSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
      ++g_d3d11c.state_r;
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("PSSetConstantBuffers");
      g_vkrt.ctx->PSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
      ++g_d3d11c.state_r;
    }
  }

  bind_descriptor_table(cmd->bindings, cmd->binding_count, false, nullptr);

  if (cmd->indexed) {
    ++g_d3d11c.draw_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("DrawIndexedInstanced");
    g_vkrt.ctx->DrawIndexedInstanced(
        cmd->vtx_count, std::max<u32>(1u, cmd->inst_count),
        cmd->first_vtx_or_idx, cmd->vtx_off, cmd->first_inst);
    ++g_d3d11c.draw_r;
  } else {
    ++g_d3d11c.draw_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("DrawInstanced");
    g_vkrt.ctx->DrawInstanced(
        cmd->vtx_count, std::max<u32>(1u, cmd->inst_count),
        cmd->first_vtx_or_idx, cmd->first_inst);
    ++g_d3d11c.draw_r;
  }
}

inline void hk_dispatch(const trt_vk_dispatch_cmd* cmd, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !cmd) return;
  auto pit = g_vkrt.pipes.find(cmd->pipeline_id);
  if (pit == g_vkrt.pipes.end() || !pit->second.cs) return;

  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("CSSetShader");
  g_vkrt.ctx->CSSetShader(pit->second.cs, nullptr, 0);
  ++g_d3d11c.state_r;
  if (cmd->push_data && cmd->push_bytes && cmd->push_reg < 14u) {
    if (ensure_dyn_cb(g_vkrt.push_cb, g_vkrt.push_cb_cap, cmd->push_bytes,
                      cmd->push_data, cmd->push_bytes)) {
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("CSSetConstantBuffers");
      g_vkrt.ctx->CSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
      ++g_d3d11c.state_r;
    }
  }

  std::vector<u32> uav_slots;
  bind_descriptor_table(cmd->bindings, cmd->binding_count, true, &uav_slots);

  ++g_d3d11c.disp_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("Dispatch");
  g_vkrt.ctx->Dispatch(std::max<u32>(1u, cmd->gx), std::max<u32>(1u, cmd->gy),
                       std::max<u32>(1u, cmd->gz));
  ++g_d3d11c.disp_r;

  ID3D11UnorderedAccessView* null_uav = nullptr;
  for (u32 slot : uav_slots) {
    ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
    d3d11_note_last("CSSetUnorderedAccessViews");
    g_vkrt.ctx->CSSetUnorderedAccessViews(slot, 1, &null_uav, nullptr);
    ++g_d3d11c.state_r;
  }
  ID3D11ShaderResourceView* null_srvs[16] = {};
  ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("CSSetShaderResources");
  g_vkrt.ctx->CSSetShaderResources(0, 16, null_srvs);
  ++g_d3d11c.state_r;
}

inline void hk_copy_buffer(u64 src_buf, u64 dst_buf, u64 src_off, u64 dst_off,
                           u64 size, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !size) return;
  auto sit = g_vkrt.bufs.find(src_buf);
  auto dit = g_vkrt.bufs.find(dst_buf);
  if (sit == g_vkrt.bufs.end() || dit == g_vkrt.bufs.end()) return;
  if (sit->second.buf && dit->second.buf) {
    D3D11_BOX box{(UINT)src_off, 0, 0, (UINT)(src_off + size), 1, 1};
    ++g_d3d11c.copy_c;  // .244 DIAG: D3D11 call/ret pair
    d3d11_note_last("CopySubresourceRegion");
    g_vkrt.ctx->CopySubresourceRegion(dit->second.buf, 0, (UINT)dst_off, 0, 0,
                                      sit->second.buf, 0, &box);
    ++g_d3d11c.copy_r;
  }
  if (src_off < sit->second.shadow.size()) {
    u64 cp = std::min<u64>(size, (u64)(sit->second.shadow.size() - src_off));
    if (dit->second.shadow.size() < dst_off + cp) {
      dit->second.shadow.resize((size_t)(dst_off + cp), 0);
    }
    memcpy(dit->second.shadow.data() + dst_off,
           sit->second.shadow.data() + src_off, (size_t)cp);
    if (dit->second.cb && dst_off == 0) {
      ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("UpdateSubresource");
      g_vkrt.ctx->UpdateSubresource(dit->second.cb, 0, nullptr,
                                    dit->second.shadow.data(), 0, 0);
      ++g_d3d11c.upd_r;
    }
  }
}

inline void hk_copy_buf_to_img(u64 src_buf, u64 dst_img, u64 buf_off, u32 w,
                               u32 h, u32 mip, u32 layer, const void* cpu_bytes,
                               u64 byte_len, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx) return;
  auto iit = g_vkrt.imgs.find(dst_img);
  if (iit == g_vkrt.imgs.end()) return;
  const img_rec& im = iit->second;
  if (!im.tex2d && !im.tex3d) return;

  const void* src_ptr = cpu_bytes;
  u64 src_len = byte_len;
  if (!src_ptr) {
    auto bit = g_vkrt.bufs.find(src_buf);
    if (bit != g_vkrt.bufs.end() && buf_off < bit->second.shadow.size()) {
      src_ptr = bit->second.shadow.data() + buf_off;
      src_len = (u64)(bit->second.shadow.size() - buf_off);
    }
  }
  if (!src_ptr || !src_len) return;

  const UINT mips = std::max<UINT>(1u, im.desc.mips);
  const UINT mw = std::max<UINT>(1u, im.desc.w >> mip);
  const UINT mh = std::max<UINT>(1u, im.desc.h >> mip);
  const UINT pw = w ? std::min<UINT>(w, mw) : mw;
  const UINT ph = h ? std::min<UINT>(h, mh) : mh;
  const UINT sub = D3D11CalcSubresource(mip, layer, mips);
  ID3D11Resource* res = im.tex3d ? (ID3D11Resource*)im.tex3d : (ID3D11Resource*)im.tex2d;

  D3D11_BOX box{0, 0, 0, pw, ph, 1};
  const D3D11_BOX* pbox = (pw != mw || ph != mh) ? &box : nullptr;

  if ((im.desc.vk_format == 104u || im.desc.vk_format == 105u || im.desc.vk_format == 106u) && src_ptr) {
    std::vector<uint32_t> converted((size_t)pw * (size_t)ph * 4u);
    const uint32_t* src3 = (const uint32_t*)src_ptr;
    const size_t total_px = (size_t)pw * (size_t)ph;
    for (size_t px = 0; px < total_px && (px + 1) * 3 * sizeof(uint32_t) <= src_len; ++px) {
      converted[px * 4 + 0] = src3[px * 3 + 0];
      converted[px * 4 + 1] = src3[px * 3 + 1];
      converted[px * 4 + 2] = src3[px * 3 + 2];
      converted[px * 4 + 3] = (im.desc.vk_format == 106u) ? 0x3F800000u /* 1.0f */ : 1u;
    }
    UINT rp = pw * 16u;
    UINT sp = rp * ph;
    ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
    d3d11_note_last("UpdateSubresource");
    g_vkrt.ctx->UpdateSubresource(res, sub, pbox, converted.data(), rp, sp);
    ++g_d3d11c.upd_r;
    return;
  }

  UINT rp = 0, sp = 0;
  calc_img_pitch(im.desc.vk_format, pw, ph, &rp, &sp);
  if (src_len < (u64)sp) {
    std::vector<uint8_t> pad((size_t)sp, 0);
    memcpy(pad.data(), src_ptr, (size_t)src_len);
    ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
    d3d11_note_last("UpdateSubresource");
    g_vkrt.ctx->UpdateSubresource(res, sub, pbox, pad.data(), rp, sp);
    ++g_d3d11c.upd_r;
  } else {
    ++g_d3d11c.upd_c;  // .244 DIAG: D3D11 call/ret pair
    d3d11_note_last("UpdateSubresource");
    g_vkrt.ctx->UpdateSubresource(res, sub, pbox, src_ptr, rp, sp);
    ++g_d3d11c.upd_r;
  }
}

inline void hk_present(u64, u32, void*) {
  if (g_vkrt.swap) {
    longan::draw_pointer(false);
    ++g_d3d11c.pres_c;  // .244 DIAG: D3D11 call/ret pair (a returned Present = the D3D12 queue drained at least this far)
    d3d11_note_last("Present");
    g_vkrt.swap->Present(1, 0);
    ++g_d3d11c.pres_r;
    if (g_vkrt.ctx && g_vkrt.main_rtv) {
      ++g_d3d11c.state_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
      d3d11_note_last("OMSetRenderTargets");
      g_vkrt.ctx->OMSetRenderTargets(1, &g_vkrt.main_rtv, g_vkrt.main_dsv);
      ++g_d3d11c.state_r;
    }
  }
}

// .245 DIAG: Flush the immediate context (called per vkQueueSubmit
// via the hooks). Counts + note_last like every other D3D11 call.
inline void hk_flush(void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx) return;
  ++g_d3d11c.flush_c;  // .245 DIAG: D3D11 call/ret pair (draw/state)
  d3d11_note_last("Flush");
  g_vkrt.ctx->Flush();
  ++g_d3d11c.flush_r;
}

inline void shutdown() {
  trt_vk_set_hooks(nullptr);
  for (auto& kv : g_vkrt.pipes) free_pipe_rec(kv.second);
  for (auto& kv : g_vkrt.bufs) free_buf_rec(kv.second);
  for (auto& kv : g_vkrt.imgs) free_img_rec(kv.second);
  for (auto& kv : g_vkrt.samps) if (kv.second) kv.second->Release();
  g_vkrt.pipes.clear();
  g_vkrt.bufs.clear();
  g_vkrt.imgs.clear();
  // .223 DIAG: buf_live is 0 after the clear.
  d3d_buf_live_refresh();
  g_vkrt.samps.clear();
  if (g_vkrt.push_cb) { g_vkrt.push_cb->Release(); g_vkrt.push_cb = nullptr; }
  g_vkrt.push_cb_cap = 0;
  for (int i = 0; i < 14; ++i) {
    if (g_vkrt.slice_cbs[i]) {
      g_vkrt.slice_cbs[i]->Release();
      g_vkrt.slice_cbs[i] = nullptr;
    }
    g_vkrt.slice_cb_caps[i] = 0;
  }
  if (g_vkrt.main_dsv) { g_vkrt.main_dsv->Release(); g_vkrt.main_dsv = nullptr; }
  if (g_vkrt.ds_tex) { g_vkrt.ds_tex->Release(); g_vkrt.ds_tex = nullptr; }
  // .222 DIAG: ds/swap counters die with the textures.
  g_d3d_ds_bytes = 0;
  g_d3d_swap_bytes = 0;
  g_d3d_swap_buffers = 0;
  g_vkrt.initialized = false;
}

inline void bind(ID3D11Device1* dev, ID3D11DeviceContext1* ctx,
                 IDXGISwapChain1* swap, ID3D11RenderTargetView* rtv,
                 UINT w, UINT h) {
  shutdown();
  g_vkrt.dev = dev;
  g_vkrt.ctx = ctx;
  g_vkrt.swap = swap;
  g_vkrt.main_rtv = rtv;
  g_vkrt.width = w ? w : 1280u;
  g_vkrt.height = h ? h : 720u;
  if (dev && g_vkrt.width && g_vkrt.height) {
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = g_vkrt.width;
    dd.Height = g_vkrt.height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ++g_d3d11c.tex2d_c;  // .244 DIAG: D3D11 call/ret pair
    d3d11_note_last("CreateTexture2D");
    const HRESULT hr_ds = dev->CreateTexture2D(&dd, nullptr, &g_vkrt.ds_tex);
    ++g_d3d11c.tex2d_r;
    if (SUCCEEDED(hr_ds)) {
      ++g_d3d11c.dsv_c;  // .244 DIAG: D3D11 call/ret pair
      d3d11_note_last("CreateDepthStencilView");
      dev->CreateDepthStencilView(g_vkrt.ds_tex, nullptr, &g_vkrt.main_dsv);
      ++g_d3d11c.dsv_r;
      // .222 DIAG: main depth-stencil texture -- D24+S8 is
      // 4 bytes/px. One buffer at bind size (1920x1080 is
      // ~8MB: small, but it is a logged consumer now).
      g_d3d_ds_bytes = (u64)g_vkrt.width * (u64)g_vkrt.height * 4ull;
    } else {
      g_d3d_ds_bytes = 0;
    }
  }
  // .222 DIAG: swapchain backbuffers -- DXGI-owned, never
  // through CreateTexture2D, so the .221 ledger never saw
  // them. Size from the real swapchain desc (BufferCount x
  // W x H x bpp). The D3D11 path uses 2 x B8G8R8A8 (4 B/px),
  // so 1920x1080 is ~16MB; a 3.7GB consumer cannot hide here,
  // but the line proves it instead of assuming it.
  g_d3d_swap_bytes = 0;
  g_d3d_swap_buffers = 0;
  if (swap) {
    DXGI_SWAP_CHAIN_DESC sd{};
    if (SUCCEEDED(swap->GetDesc(&sd))) {
      g_d3d_swap_buffers = sd.BufferCount;
      g_d3d_swap_w = sd.BufferDesc.Width;
      g_d3d_swap_h = sd.BufferDesc.Height;
      g_d3d_swap_fmt = (u32)sd.BufferDesc.Format;
      g_d3d_swap_bytes = (u64)sd.BufferCount *
          (u64)sd.BufferDesc.Width * (u64)sd.BufferDesc.Height *
          (u64)dxgi_bpp(sd.BufferDesc.Format);
      char swb[256];
      snprintf(swb, sizeof(swb),
               "D3D11-SIZES SWAPCHAIN buffers=%u %ux%u fmt=%u %lluMB",
               g_d3d_swap_buffers, g_d3d_swap_w, g_d3d_swap_h,
               g_d3d_swap_fmt,
               (unsigned long long)(g_d3d_swap_bytes >> 20));
      trt_core_log(swb);
    }
  }
  g_vkrt.initialized = (dev && ctx && rtv);

  trt_vk_hooks hk{};
  hk.create_pipeline = &hk_create_pipeline;
  hk.destroy_pipeline = &hk_destroy_pipeline;
  hk.sync_buffer = &hk_sync_buffer;
  hk.destroy_buffer = &hk_destroy_buffer;
  hk.sync_image = &hk_sync_image;
  hk.destroy_image = &hk_destroy_image;
  hk.begin_render_pass = &hk_begin_render_pass;
  hk.end_render_pass = &hk_end_render_pass;
  hk.draw = &hk_draw;
  hk.dispatch = &hk_dispatch;
  hk.copy_buffer = &hk_copy_buffer;
  hk.copy_buf_to_img = &hk_copy_buf_to_img;
  hk.present = &hk_present;
  hk.flush = &hk_flush;  // .245 DIAG: Flush per submit
  trt_vk_set_hooks(&hk);
}

}  // namespace vk_rt

// .220 DIAG: periodic video-memory sample -- answers whether the
// 3809MB video usage at TEX-OOM time is a fixed baseline (a
// driver/system reservation present from boot) or growth during
// the run. Same QueryVideoMemoryInfo source as TEX-OOM-DIAG.
// Called from the watchdog (main.cpp, snapshot-only) every ~5s.
// Defined here (single-TU header, bound in swapchain.cpp) so no
// new trt.h declaration is needed; main.cpp declares it locally.
// .241 DIAG: one of the three largest MEM_PRIVATE committed
// regions (base + MB) -- WHERE the ~4.3GB untracked consumer
// sits. The .240 split proved the consumer is private
// committed memory outside any heap (heap 532MB, priv
// 4904MB); the valloc caller log (snapshot-only kernel32
// delta) names WHO reserves it if it goes through the
// bridge, and these bases bound WHERE it sits if it bypasses
// the bridge (direct NtAllocateVirtualMemory via the real
// ntdll, or host driver allocations).
struct trt_mem_privtop {
  unsigned long long base = 0;
  unsigned long long mb = 0;
};

// .240 DIAG: split appresid (VIDEO-SAMPLE) into guest-heap
// vs the rest. The .239 run left a ~4.9GB untracked CPU
// consumer (appresid +4865MB, hostvec 96MB max, shadow 1MB,
// ledger 470MB, tex_live 151MB) -- most likely the guest
// heap, but the bridge could not see it. Two independent
// measures, both taken on the watchdog thread at
// VIDEO-SAMPLE cadence (~5s): (1) HeapWalk over every
// process heap sums live busy blocks = the guest heap
// (Godot mallocs through the process heap; the bridge
// budget ledger only sees bridged allocs -- contrast
// Sekiro, whose 4668MB ledger ~= its 4840MB appmem).
// (2) VirtualQuery walk sums committed bytes by type
// (MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED) -- private
// committed minus the heap total is driver CPU + direct
// VirtualAlloc + stacks. HeapWalk is time-capped (150ms,
// checked every 4096 entries) and the whole walk runs
// under one top-level __try: a corrupted guest heap
// reports partial=1, never kills the watchdog thread.
// All locals are POD (no C2712: __try + destructibles
// does not compile). Out-params are MB (>> 20).
// HeapWalk is desktop-only and NOT declared in the
// UWP-partition windows.h this TU sees (VirtualQuery /
// GetProcessHeaps / GetTickCount64 resolve, HeapWalk does
// not), although kernel32.dll exports it and the link
// already resolves other kernel32 imports
// (GetThreadContext, RtlCaptureContext). Declare it
// locally with its true signature instead of pulling in
// a desktop header. PROCESS_HEAP_ENTRY itself IS
// declared (the .240 build failed only on the function
// name, at the call site).
extern "C" BOOL WINAPI HeapWalk(HANDLE hHeap,
                                 LPPROCESS_HEAP_ENTRY lpEntry);
// .243 DIAG: private-region census -- total committed MEM_PRIVATE
// region count + counts in size buckets (<1MB / 1-16MB / 16-64MB /
// >=64MB), via the new priv_n / priv_bk[4] out-params. The .242
// privtop top-3 are 75/64/32MB while priv is 4903MB: the 4.3GB
// is spread over MANY regions, and the bucket shape says whether
// they are heap-like small blocks (driver sub-allocator / direct
// small VirtualAllocs) or a few huge reservations.
static inline void trt_mem_split240(unsigned long long* heap_mb,
                                     unsigned* heap_part,
                                     unsigned long long* priv_mb,
                                     unsigned long long* img_mb,
                                     unsigned long long* map_mb,
                                    trt_mem_privtop* top3,
                                     unsigned* priv_n,
                                     unsigned* priv_bk) {
  unsigned long long heap_b = 0;
  unsigned part = 0;
  unsigned long long priv_b = 0;
  unsigned long long img_b = 0;
  unsigned long long map_b = 0;
  unsigned priv_cnt = 0;  // .243 DIAG: region census
  unsigned priv_bk0 = 0, priv_bk1 = 0, priv_bk2 = 0, priv_bk3 = 0;
  // .241 DIAG: top-3 private-region bases + exact byte sizes
  // (compared exact; converted to MB at the end).
  unsigned long long top_base[3] = {};
  unsigned long long top_sz[3] = {};
#if defined(_WIN32)
  __try {
    // (2) VirtualQuery walk first (no locks, cannot block).
    MEMORY_BASIC_INFORMATION mbi{};
    unsigned char* addr = 0;
    for (unsigned it = 0; it < 2000000u; ++it) {
      const SIZE_T r = ::VirtualQuery(addr, &mbi, sizeof(mbi));
      if (!r) break;
      if (mbi.State == MEM_COMMIT) {
        if (mbi.Type == MEM_PRIVATE) {
          priv_b += (unsigned long long)mbi.RegionSize;
          // .241 DIAG: track the 3 largest private regions
          // (descending by exact byte size).
          const unsigned long long rsz =
              (unsigned long long)mbi.RegionSize;
          const unsigned long long rbase =
              (unsigned long long)(uintptr_t)mbi.BaseAddress;
          if (rsz > top_sz[0]) {
            top_base[2] = top_base[1];
            top_sz[2] = top_sz[1];
            top_base[1] = top_base[0];
            top_sz[1] = top_sz[0];
            top_base[0] = rbase;
            top_sz[0] = rsz;
          } else if (rsz > top_sz[1]) {
            top_base[2] = top_base[1];
            top_sz[2] = top_sz[1];
            top_base[1] = rbase;
            top_sz[1] = rsz;
          } else if (rsz > top_sz[2]) {
            top_base[2] = rbase;
            top_sz[2] = rsz;
          }
          // .243 DIAG: region census (see the signature note).
          ++priv_cnt;
          if (rsz < (1ULL << 20))
            ++priv_bk0;
          else if (rsz < (16ULL << 20))
            ++priv_bk1;
          else if (rsz < (64ULL << 20))
            ++priv_bk2;
          else
            ++priv_bk3;
        } else if (mbi.Type == MEM_IMAGE)
          img_b += (unsigned long long)mbi.RegionSize;
        else if (mbi.Type == MEM_MAPPED)
          map_b += (unsigned long long)mbi.RegionSize;
      }
      unsigned char* next =
          (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
      if (next <= addr) break;
      addr = next;
    }
    // (1) HeapWalk, time-capped.
    HANDLE heaps[64];
    const DWORD nheaps = ::GetProcessHeaps(64, heaps);
    if (nheaps == 0 || nheaps > 64) part = 1;
    const unsigned long long t0 =
        (unsigned long long)::GetTickCount64();
    const DWORD nwalk = nheaps > 64 ? 64 : nheaps;
    for (DWORD h = 0; h < nwalk; ++h) {
      PROCESS_HEAP_ENTRY e{};
      unsigned entries = 0;
      while (::HeapWalk(heaps[h], &e)) {
        if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY)
          heap_b += (unsigned long long)e.cbData;
        if ((++entries & 0xFFFu) == 0) {
          if ((unsigned long long)::GetTickCount64() - t0 > 150ull) {
            part = 1;
            break;
          }
        }
      }
      if (part) break;
      if (::GetLastError() != ERROR_NO_MORE_ITEMS) part = 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    part = 1;
  }
#endif
  if (heap_mb) *heap_mb = heap_b >> 20;
  if (heap_part) *heap_part = part;
  if (priv_mb) *priv_mb = priv_b >> 20;
  if (img_mb) *img_mb = img_b >> 20;
  if (map_mb) *map_mb = map_b >> 20;
  if (top3) {
    for (int i = 0; i < 3; ++i) {
      top3[i].base = top_base[i];
      top3[i].mb = top_sz[i] >> 20;
    }
  }
  if (priv_n) *priv_n = priv_cnt;  // .243 DIAG: region census
  if (priv_bk) {
    priv_bk[0] = priv_bk0;
    priv_bk[1] = priv_bk1;
    priv_bk[2] = priv_bk2;
    priv_bk[3] = priv_bk3;
  }
}

extern "C" void trt_video_sample_log() {
  if (!vk_rt::g_vkrt.dev) return;
  IDXGIDevice* dxgi_dev = nullptr;
  if (!SUCCEEDED(vk_rt::g_vkrt.dev->QueryInterface(__uuidof(IDXGIDevice),
                                                    (void**)&dxgi_dev)) ||
      !dxgi_dev) {
    return;
  }
  IDXGIAdapter* adapter = nullptr;
  if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                           (void**)&adapter3)) &&
        adapter3) {
      DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
      DXGI_QUERY_VIDEO_MEMORY_INFO vmi_nl{};
      const bool ok_local = SUCCEEDED(adapter3->QueryVideoMemoryInfo(
          0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi));
      const bool ok_nl = SUCCEEDED(adapter3->QueryVideoMemoryInfo(
          0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vmi_nl));
      if (ok_local) {
        // .223 DIAG: (a) buf_live is now the RENDER-THREAD
        // cached atomic (see d3d_buf_live_refresh) -- the
        // .222 code walked g_vkrt.bufs from this watchdog
        // thread while the render thread mutated the map
        // (data race). (b) Per-interval deltas since the
        // previous sample (tex/buf/vk/presents/bridged-calls/
        // appmem) + the app working set, so a ~2GB jump is
        // correlated with WHAT HAPPENED between the samples,
        // not just the totals at the sample. (c)
        // VIDEO-BIGJUMP companion line on jumps >= 500MB
        // with the largest single allocations. A static
        // mutex serialises the watchdog (5s cadence) and
        // the one-shot frame-1 sample from the present hook.
        static std::mutex s_sample_mu;
        std::lock_guard<std::mutex> sample_lk(s_sample_mu);
        static unsigned s_sample_n = 0;
        static unsigned long long s_prev_local_mb = 0;
        ++s_sample_n;
        const unsigned long long cur_local_mb =
            (unsigned long long)(vmi.CurrentUsage >> 20);
        const long long jump_mb = (s_sample_n == 1)
            ? 0
            : (long long)cur_local_mb - (long long)s_prev_local_mb;
        s_prev_local_mb = cur_local_mb;
        vk_rt::g_video_last_local_mb = cur_local_mb;
        vk_rt::g_video_sample_seen = true;
        const u64 buf_live =
            vk_rt::g_d3d_buf_live_cached.load(std::memory_order_relaxed);
        // .223 DIAG: per-interval deltas -- what happened
        // between this sample and the previous one.
        static unsigned long long s_prev_tex_n = 0;
        static unsigned long long s_prev_tex_created_mb = 0;
        static unsigned long long s_prev_vkalloc_mb = 0;
        static unsigned long long s_prev_vkimg_n = 0;
        static unsigned long long s_prev_presents = 0;
        static unsigned long long s_prev_calls = 0;
        static unsigned long long s_prev_appmem_mb = 0;
        static unsigned long long s_prev_tex2d_c = 0;  // .244 DIAG
        static unsigned long long s_prev_upd_c = 0;  // .244 DIAG
        static unsigned long long s_prev_pres_c = 0;  // .244 DIAG
        static unsigned long long s_prev_draw_c = 0;  // .245 DIAG
        static unsigned long long s_prev_state_c = 0;  // .245 DIAG
        static unsigned long long s_prev_flush_c = 0;  // .245 DIAG
        static unsigned s_prev_devrem = 0;  // .245 DIAG
        const unsigned long long tex_n =
            (unsigned long long)vk_rt::g_d3d_tex_created_n;
        const unsigned long long tex_created_mb =
            (unsigned long long)(vk_rt::g_d3d_tex_created_bytes >> 20);
        const unsigned long long vkalloc_mb =
            (unsigned long long)(trt_vk_alloc_live_bytes() >> 20);
        const unsigned long long vkalloc_n =
            (unsigned long long)trt_vk_alloc_live_n();
        const unsigned long long vkimg_n =
            (unsigned long long)trt_vk_imgs_live_n();
        const unsigned long long vkimg_est_mb =
            (unsigned long long)(trt_vk_imgs_est_bytes() >> 20);
        const unsigned long long presents =
            (unsigned long long)trt_vk_presents();
        const unsigned long long calls =
            (unsigned long long)gap_trace_seq();
        const unsigned long long appmem_mb =
            (unsigned long long)trt_video_app_mem_mb();
        // .239 DIAG: CPU-side bridge totals + the app-memory
        // residual. appresid = AppMemoryUsage minus every
        // CPU byte the bridge accounts for (host vectors +
        // buffer shadows): what is left is the guest heap +
        // driver CPU memory + untracked bridge CPU -- the
        // ~4.6GB consumer, named by size on every sample.
        // (Video-side residual is the existing D3D11-SIZES
        // resid=.) ledger = the bridge memory-budget
        // committed bytes (266MB at the first .238 texfail
        // while video was already 3809MB).
        const unsigned long long hostvec_mb =
            (unsigned long long)(trt_vk_hostvec_live_bytes() >> 20);
        const unsigned long long shadow_mb =
            (unsigned long long)(vk_rt::g_d3d_shadow_live_cached.load(
                                     std::memory_order_relaxed) >>
                                 20);
        const unsigned long long ledger_mb =
            (unsigned long long)(trt_mem_budget_committed() >> 20);
        const long long appresid_mb =
            (long long)appmem_mb - (long long)hostvec_mb -
            (long long)shadow_mb;
        // .240 DIAG: split appresid into guest-heap vs the
        // rest (driver CPU + direct VirtualAlloc + stacks).
        // heap = HeapWalk live busy bytes over every
        // process heap; priv/img/map = VirtualQuery
        // committed bytes by type. See trt_mem_split240.
        unsigned long long heap_mb = 0, priv_mb = 0, img_mb = 0,
            map_mb = 0;
        unsigned heap_part = 0;
        // .241 DIAG: top-3 largest private regions (see
        // trt_mem_split240 / trt_mem_privtop).
        // .243 DIAG: private-region census (see trt_mem_split240).
        trt_mem_privtop privtop[3] = {};
        unsigned priv_n = 0;
        unsigned priv_bk[4] = {};
        trt_mem_split240(&heap_mb, &heap_part, &priv_mb, &img_mb,
                         &map_mb, privtop, &priv_n, priv_bk);
        const long long apres_other_mb =
            appresid_mb - (long long)heap_mb;
        const bool first_sample = (s_sample_n == 1);
        const long long dtex_n = first_sample
            ? 0 : (long long)tex_n - (long long)s_prev_tex_n;
        const long long dtex_mb = first_sample
            ? 0 : (long long)tex_created_mb - (long long)s_prev_tex_created_mb;
        const long long dvkalloc_mb = first_sample
            ? 0 : (long long)vkalloc_mb - (long long)s_prev_vkalloc_mb;
        const long long dvkimg = first_sample
            ? 0 : (long long)vkimg_n - (long long)s_prev_vkimg_n;
        const long long dpres = first_sample
            ? 0 : (long long)presents - (long long)s_prev_presents;
        const long long dcalls = first_sample
            ? 0 : (long long)calls - (long long)s_prev_calls;
        const long long dappmem_mb = first_sample
            ? 0 : (long long)appmem_mb - (long long)s_prev_appmem_mb;
        s_prev_tex_n = tex_n;
        s_prev_tex_created_mb = tex_created_mb;
        s_prev_vkalloc_mb = vkalloc_mb;
        s_prev_vkimg_n = vkimg_n;
        s_prev_presents = presents;
        s_prev_calls = calls;
        s_prev_appmem_mb = appmem_mb;
        // .244 DIAG: D3D11 call/ret table + per-interval deltas. If
        // the main thread parks inside a bridge-issued D3D11 call,
        // that call shows calls = rets + 1 at the terminal sample
        // while every other pair balances -- that names the D3D11
        // entry the d3d11on12 layer is waiting inside. The
        // per-interval deltas (dtex2d/dupd/dpres) correlate with
        // the privn growth per interval: if dupd tracks the
        // +700-950 regions per BIGJUMP while dtex2d tracks only
        // the +83-136 new textures, the regions come from the
        // upload path (UpdateSubresource staging retained per
        // call), not from creation. devrem =
        // GetDeviceRemovedReason at sample time (S_OK = 0: the
        // device is alive; the D3D12 queue signalled at least as
        // far as the last returned Present). lastd3d11 = the last
        // bridge-issued D3D11 call + its age (d3d11_note_last).
        const vk_rt::d3d11_counts_snap d3d = vk_rt::d3d11_snapshot();
        const long long dtex2d_c = first_sample
            ? 0 : (long long)d3d.tex2d_c - (long long)s_prev_tex2d_c;
        const long long dupd_c = first_sample
            ? 0 : (long long)d3d.upd_c - (long long)s_prev_upd_c;
        const long long dpres_c = first_sample
            ? 0 : (long long)d3d.pres_c - (long long)s_prev_pres_c;
        s_prev_tex2d_c = d3d.tex2d_c;
        s_prev_upd_c = d3d.upd_c;
        s_prev_pres_c = d3d.pres_c;
        char d3d_line[640];
        vk_rt::d3d11_format_line(d3d_line, sizeof(d3d_line), d3d);
        const unsigned devrem =
            vk_rt::g_vkrt.dev
                ? (unsigned)vk_rt::g_vkrt.dev->GetDeviceRemovedReason()
                : 0;
        const char* last_name = "none";
        unsigned long long last_tick = 0;
        d3d11_last_call(&last_name, &last_tick);
        const unsigned long long last_age_ms =
            last_tick ? (GetTickCount64() - last_tick) : 0;
        // .245 DIAG: draw/state/flush per-interval deltas (is
        // the draw/state path still issuing calls in the
        // terminal intervals, or is main parked?).
        const long long ddraw_c = first_sample
            ? 0 : (long long)d3d.draw_c - (long long)s_prev_draw_c;
        const long long dstate_c = first_sample
            ? 0 : (long long)d3d.state_c - (long long)s_prev_state_c;
        const long long dflush_c = first_sample
            ? 0 : (long long)d3d.flush_c - (long long)s_prev_flush_c;
        s_prev_draw_c = d3d.draw_c;
        s_prev_state_c = d3d.state_c;
        s_prev_flush_c = d3d.flush_c;
        // .245 DIAG: DEVREM-FLIP -- the device was removed
        // between the previous sample and this one. Log the
        // transition with the video usage, privn, the interval
        // deltas, and the last bridge D3D11 call -- this names
        // the removal point (video over-budget? a bad call?
        // unsubmitted-work growth?).
        if (s_prev_devrem == 0 && devrem != 0) {
          char flip[512];
          snprintf(flip, sizeof(flip),
                   "[vk_backend] DEVREM-FLIP #%u devrem=0x%08x video=%lluMB privn=%u dtex2d=%+lld dupd=%+lld ddraw=%+lld dstate=%+lld dflush=%+lld lastd3d11=%s %llums",
                   s_sample_n, devrem, cur_local_mb, priv_n,
                   dtex2d_c, dupd_c, ddraw_c, dstate_c, dflush_c,
                   last_name ? last_name : "none", last_age_ms);
          trt_core_log(flip);
        }
        s_prev_devrem = devrem;
        char vb[2304];  // .245 DIAG: +ddraw=/dstate=/dflush=
        snprintf(vb, sizeof(vb),
                 "[vk_backend] VIDEO-SAMPLE #%u local budget=%lluMB usage=%lluMB avail=%lluMB resv=%lluMB nonlocal budget=%lldMB usage=%lldMB avail=%lldMB resv=%lldMB jump=%+lldMB tex_live=%lluMB tex_created=%lluMB tex_n=%llu buf_live=%lluMB ds=%lluMB swap=%lluMB appmem=%lluMB dappmem=%+lldMB vkalloc=%lluMB vkalloc_n=%llu vkimg_n=%llu vkimg_est=%lluMB presents=%llu calls=%llu dtex_n=%+lld dtex=%+lldMB dvkalloc=%+lldMB dvkimg=%+lld dpres=%+lld dcalls=%+lld hostvec=%lluMB shadow=%lluMB ledger=%lluMB appresid=%+lldMB heap=%lluMB heap_part=%u priv=%lluMB img=%lluMB map=%lluMB apres_other=%+lldMB privtop1=0x%llx:%lluMB privtop2=0x%llx:%lluMB privtop3=0x%llx:%lluMB ntalloc_calls=%llu ntalloc_mb=%llu privn=%u privb0=%u privb1=%u privb2=%u privb3=%u d3d11=%s dtex2d=%+lld dupd=%+lld dpres=%+lld ddraw=%+lld dstate=%+lld dflush=%+lld devrem=0x%08x lastd3d11=%s %llums",
                 s_sample_n,
                 (unsigned long long)(vmi.Budget >> 20),
                 cur_local_mb,
                 (unsigned long long)(vmi.AvailableForReservation >> 20),
                 (unsigned long long)(vmi.CurrentReservation >> 20),
                 ok_nl ? (long long)(vmi_nl.Budget >> 20) : -1ll,
                 ok_nl ? (long long)(vmi_nl.CurrentUsage >> 20) : -1ll,
                 ok_nl ? (long long)(vmi_nl.AvailableForReservation >> 20) : -1ll,
                 ok_nl ? (long long)(vmi_nl.CurrentReservation >> 20) : -1ll,
                 jump_mb,
                 (unsigned long long)(vk_rt::g_d3d_tex_live_bytes >> 20),
                 tex_created_mb,
                 tex_n,
                 (unsigned long long)(buf_live >> 20),
                 (unsigned long long)(vk_rt::g_d3d_ds_bytes >> 20),
                 (unsigned long long)(vk_rt::g_d3d_swap_bytes >> 20),
                 appmem_mb, dappmem_mb,
                 vkalloc_mb, vkalloc_n, vkimg_n, vkimg_est_mb,
                 presents, calls,
                 dtex_n, dtex_mb, dvkalloc_mb, dvkimg, dpres, dcalls,
                 hostvec_mb, shadow_mb, ledger_mb, appresid_mb,
                 heap_mb, heap_part, priv_mb, img_mb, map_mb,
                 apres_other_mb, privtop[0].base, privtop[0].mb,
                 privtop[1].base, privtop[1].mb, privtop[2].base,
                 privtop[2].mb, trt_ntalloc_calls(),
                 (unsigned long long)(trt_ntalloc_bytes() >> 20),
                 priv_n, priv_bk[0], priv_bk[1], priv_bk[2],
                 priv_bk[3], d3d_line, dtex2d_c, dupd_c, dpres_c,
                 ddraw_c, dstate_c, dflush_c, devrem,
                 last_name ? last_name : "none", last_age_ms);
        trt_core_log(vb);
        // .223 DIAG: VIDEO-BIGJUMP -- on jumps >= 500MB name
        // the largest single allocations alive: if the top
        // D3D11 texture and the biggest live Vulkan alloc
        // are MBs while the jump is GBs, no bridge-visible
        // allocation explains it and the consumer is
        // driver-internal heap growth in the LOCAL segment
        // (resv=0 already rules out a reservation).
        if (jump_mb >= 500) {
          char bj[1536];  // .244 DIAG: +d3d11=/devrem=
          snprintf(bj, sizeof(bj),
                   "[vk_backend] VIDEO-BIGJUMP #%u jump=%+lldMB appmem=%lluMB dappmem=%+lldMB tex_n=%llu dtex_n=%+lld tex_created=%lluMB dtex=%+lldMB vkalloc=%lluMB dvkalloc=%+lldMB vkimg_n=%llu dvkimg=%+lld presents=%llu toptex=%lluMB %ux%ux%u fmt=%u biggestvk=%lluMB hostvec=%lluMB appresid=%+lldMB heap=%lluMB apres_other=%+lldMB d3d11=%s devrem=0x%08x",
                   s_sample_n, jump_mb, appmem_mb, dappmem_mb,
                   tex_n, dtex_n, tex_created_mb, dtex_mb,
                   vkalloc_mb, dvkalloc_mb, vkimg_n, dvkimg, presents,
                   (unsigned long long)(vk_rt::g_d3d_top_bytes >> 20),
                   vk_rt::g_d3d_top_w, vk_rt::g_d3d_top_h, vk_rt::g_d3d_top_d,
                   vk_rt::g_d3d_top_fmt,
                   (unsigned long long)(trt_vk_biggest_alloc_bytes() >> 20),
                   hostvec_mb, appresid_mb, heap_mb, apres_other_mb,
                   d3d_line, devrem);
          trt_core_log(bj);
        }
      }
      adapter3->Release();
    }
    adapter->Release();
  }
  dxgi_dev->Release();
}
