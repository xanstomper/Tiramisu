// tiramisu/src/host/swapchain.cpp
// D3D11 & D3D12 flip-model swapchain management on a CoreWindow.
// Host UI always uses D3D11 + D2D1; when a guest game requests a D3D12
// swapchain via IDXGIFactory::CreateSwapChain*(command_queue, ...), the
// CoreWindow swapchain is cleanly rebound to that ID3D12CommandQueue and
// restored to D3D11 when the guest exits back to the launcher.
#include "swapchain.h"
#include "cursor_policy.h"
#include "cursor_art.h"
#include "console_detect.h"
#include "host_log.h"
#include "d3d9_backend.hpp"
#include "gl_backend.hpp"
#include "vk_backend.hpp"

#include "../trt/bridge_d3d.h"
#include "../trt/bridge_d3d12.h"
#include "../trt/bridge_d3dx.h"
#include "../trt/input.h"
#include <algorithm>

#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "settings.h"
#include <atomic>
#include <chrono>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace longan {

namespace {
winrt::com_ptr<IUnknown> g_window;
winrt::com_ptr<ID3D11Device1> g_device;
winrt::com_ptr<ID3D11DeviceContext1> g_ctx;
winrt::com_ptr<IDXGISwapChain1> g_swap;
winrt::com_ptr<IDXGISwapChain3> g_swap3;
winrt::com_ptr<ID3D11RenderTargetView> g_rtv;
winrt::com_ptr<ID3D11Texture2D> g_guest_backbuffer;
winrt::com_ptr<ID3D11Query> g_guest_draw_query;
bool g_guest_draw_query_active = false;
bool g_guest_draw_query_pending = false;
std::atomic<unsigned> g_guest_draw_state{0}, g_guest_draw_samples{0};
std::atomic<unsigned> g_guest_nonempty_draw_samples{0};
unsigned g_guest_draw_samples_remaining = 120;
unsigned g_guest_draw_pause = 0;
std::atomic<unsigned long long> g_guest_draw_vertices{0}, g_guest_draw_pixels{0};
std::atomic<int> g_guest_draw_result{0};
constexpr unsigned kGuestProbeGrid = 4;
struct surface_sample {
  winrt::com_ptr<ID3D11Texture2D> texture;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool pending = false;
  unsigned lit = 0;
};
surface_sample g_guest_surface_probe, g_host_surface_probe, g_other_surface_probe;
std::atomic<unsigned> g_surface_samples{0}, g_guest_lit_pixels{0}, g_host_lit_pixels{0};
std::atomic<unsigned> g_surface_guest_rtv{0}, g_surface_other_rtv{0};
std::atomic<unsigned> g_other_surface_samples{0}, g_other_lit_pixels{0};
std::atomic<unsigned> g_other_width{0}, g_other_height{0}, g_other_format{0};
std::atomic<int> g_other_surface_result{0};
surface_sample g_shader_input_surface;
std::atomic<unsigned> g_shader_input_snapshots{0}, g_shader_input_bound{0};
std::atomic<unsigned> g_shader_input_samples{0}, g_shader_input_lit{0};
std::atomic<unsigned> g_shader_input_slot{0}, g_shader_input_width{0};
std::atomic<unsigned> g_shader_input_height{0}, g_shader_input_format{0};
std::atomic<int> g_shader_input_result{0};
std::atomic<int> g_surface_probe_result{0};
unsigned g_surface_probe_interval = 0;
winrt::com_ptr<IDXGIFactory2> g_factory;
winrt::com_ptr<IDXGIFactory6> g_factory6;
winrt::com_ptr<ID3D12Device> g_d3d12_device;
winrt::com_ptr<IDXGISwapChain3> g_d3d12_swap;
bool g_in_d3d12 = false;
std::atomic<bool> g_guest_cursor_visible{false};
std::atomic<uint64_t> g_guest_cursor_present_ms{0};
unsigned g_w = 1280, g_h = 720;
unsigned g_backbuffer_width = 1280, g_backbuffer_height = 720;
unsigned g_fl = D3D_FEATURE_LEVEL_11_0;

const char* k_vs = R"(struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };
VSOut main(uint id : SV_VertexID) {
  VSOut o;
  // Clockwise winding in D3D11 NDC (+Y up) so default CULL_BACK never culls!
  float2 quad[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
  float2 uv[3]   = { float2(0,1),   float2(0,-1), float2(2,1) };
  o.p = float4(quad[id], 0, 1);
  o.uv = uv[id];
  return o;
})";

// Ultra-Crisp 60 FPS Sony PSP / PS3 XMB Silk Wave Shader.
// Lifted neutral studio matte-black canvas (#0F1013 -> #191B1F — true neutral
// black, NOT crushed pitch-black, ZERO pink/purple/navy) with 5 high-definition
// layered translucent silver-white silk ribbons, crisp luminous crests, caustic
// fold sheen, and zero-banding dithering.
const char* k_ps = R"(
cbuffer BgParams : register(b0) {
  float4 uTimeRes; // x = time (sec), y = width, z = height, w = 0
};
struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };

// Compute one high-definition PSP XMB silk ribbon:
// returns (translucent silk veil, razor-sharp luminous crest, caustic sheen)
float3 pspRibbon(float2 uv, float t, float baseY, float amp, float freq,
                 float speed, float phase, float thickness) {
  float x = uv.x * 6.2831853;
  // Multi-harmonic organic wave with gentle phase modulation
  float ph = phase + 0.35 * sin(t * (speed * 0.37) + uv.x * 1.4);
  float y0 = baseY
           + amp * 0.54 * sin(x * freq + t * speed + ph)
           + amp * 0.28 * sin(x * (freq * 1.68) - t * (speed * 0.64) + phase * 1.8)
           + amp * 0.12 * cos(x * (freq * 2.55) + t * (speed * 1.18) - phase * 0.7)
           + amp * 0.06 * sin(x * (freq * 3.80) - t * (speed * 1.55) + phase * 2.3);
  float d = uv.y - y0;

  // Translucent layered silk drapery flowing below the wave crest
  float downVeil = exp(-pow(max(d, 0.0) / thickness, 1.85)) * smoothstep(-0.016, 0.014, d);
  // Soft volumetric glow hugging both sides of the ribbon spine
  float softGlow = exp(-abs(d) / (thickness * 0.26)) * 0.52;
  // Razor-sharp anti-aliased PSP pearlescent crest filament
  float crest = exp(-pow(d / 0.0028, 2.0));
  // Secondary ultra-fine mirror highlight slightly offset beneath crest
  float subCrest = exp(-pow((d - 0.009) / 0.0045, 2.0)) * 0.42;

  // Flowing specular light traveling along the folds of the silk
  float caustic = 0.65 + 0.35 * pow(0.5 + 0.5 * sin(x * (freq * 1.45) + t * (speed * 0.92) + phase), 2.0);
  float body = (downVeil * 0.68 + softGlow * 0.32) * caustic;
  float edge = (crest + subCrest) * (0.76 + 0.24 * caustic);
  return float3(body, edge, caustic * downVeil);
}

float4 main(VSOut i) : SV_Target {
  float t = uTimeRes.x * 0.58;
  float2 uv = i.uv;

  // Neutral lifted studio matte-black gradient (black, NOT pitch-crushed dark black!)
  float3 colTop = float3(0.058, 0.060, 0.066);  // #0F0F11 soft studio black
  float3 colMid = float3(0.102, 0.106, 0.116);  // #1A1B1E elevated neutral charcoal
  float3 colBot = float3(0.052, 0.054, 0.060);  // #0D0E0F matte obsidian
  float midBand = exp(-pow((uv.y - 0.56) / 0.38, 2.0));
  float3 col = lerp(lerp(colTop, colBot, uv.y), colMid, midBand);

  // Subtle neutral silver studio key-light breathing behind the ribbons
  float keyGlow = exp(-pow((uv.y - 0.58) / 0.26, 2.0)) *
                  (0.78 + 0.22 * sin(uv.x * 3.14159 + t * 0.32));
  col += float3(0.048, 0.050, 0.056) * keyGlow;

  // Ribbon 1 (Deep background atmospheric mist — cool neutral silver)
  float3 w1 = pspRibbon(uv, t, 0.44, 0.080, 0.62, 0.36, 0.3, 0.19);
  col += float3(0.28, 0.29, 0.32) * w1.x * 0.28;
  col += float3(0.62, 0.64, 0.68) * w1.y * 0.16;

  // Ribbon 2 (Upper-mid drifting silk — brushed titanium-silver)
  float3 w2 = pspRibbon(uv, t, 0.53, 0.088, 0.78, -0.48, 1.9, 0.15);
  col += float3(0.38, 0.39, 0.42) * w2.x * 0.32;
  col += float3(0.82, 0.84, 0.87) * w2.y * 0.28;

  // Ribbon 3 (Primary iconic PSP hero silk wave — brilliant liquid pearl-white)
  float3 w3 = pspRibbon(uv, t, 0.61, 0.074, 0.92, 0.62, 4.1, 0.12);
  col += float3(0.50, 0.51, 0.54) * w3.x * 0.38;
  col += float3(0.96, 0.97, 0.99) * w3.y * 0.50;
  col += float3(0.22, 0.23, 0.26) * w3.z * 0.16;

  // Ribbon 4 (Foreground counter-wave — crisp platinum filament)
  float3 w4 = pspRibbon(uv, t, 0.70, 0.058, 1.10, -0.74, 0.9, 0.092);
  col += float3(0.46, 0.47, 0.49) * w4.x * 0.30;
  col += float3(0.90, 0.91, 0.93) * w4.y * 0.38;

  // Ribbon 5 (Fine lower-screen whisper ribbon for full-frame balance)
  float3 w5 = pspRibbon(uv, t, 0.82, 0.040, 1.32, 0.85, 2.7, 0.070);
  col += float3(0.36, 0.37, 0.39) * w5.x * 0.22;
  col += float3(0.78, 0.79, 0.82) * w5.y * 0.24;

  // Gentle studio edge falloff (keeps corners rich matte-black, never crushed)
  float2 vp = uv - 0.5;
  float vig = 1.0 - dot(vp * float2(0.46, 0.58), vp * float2(0.46, 0.58));
  col *= saturate(0.80 + 0.24 * vig);

  // Interleaved gradient noise dither (eliminates 8-bit dark-gradient banding)
  float2 px = uv * uTimeRes.yz;
  float ign = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
  col += (ign - 0.5) / 255.0;

  return float4(saturate(col), 1.0);
})";

int TRT_GUEST_ABI host_d3d12_create_device(void* adapter, u32 min_fl,
                                           const void* riid, void** out_dev) {
  static const IID k_dev_iid = __uuidof(ID3D12Device);
  const HRESULT hr = ::D3D12CreateDevice(
      (IUnknown*)adapter, (D3D_FEATURE_LEVEL)min_fl,
      riid ? *(const IID*)riid : k_dev_iid, out_dev);
  if (SUCCEEDED(hr) && out_dev && *out_dev && !g_d3d12_device) {
    ((IUnknown*)(*out_dev))->QueryInterface(__uuidof(ID3D12Device),
                                            g_d3d12_device.put_void());
  }
  return (int)hr;
}

int TRT_GUEST_ABI host_d3d12_get_debug(const void* riid, void** out_dbg) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12GetDebugInterface(*(const IID*)riid, out_dbg);
}

int TRT_GUEST_ABI host_d3d12_ser_root(const void* desc, u32 ver,
                                      void** out_blob, void** out_err) {
  return (int)::D3D12SerializeRootSignature(
      (const D3D12_ROOT_SIGNATURE_DESC*)desc, (D3D_ROOT_SIGNATURE_VERSION)ver,
      (ID3DBlob**)out_blob, (ID3DBlob**)out_err);
}

int TRT_GUEST_ABI host_d3d12_ser_vroot(const void* desc, void** out_blob,
                                       void** out_err) {
  return (int)::D3D12SerializeVersionedRootSignature(
      (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*)desc,
      (ID3DBlob**)out_blob, (ID3DBlob**)out_err);
}

int TRT_GUEST_ABI host_d3d12_deser_root(const void* src, u64 sz,
                                        const void* riid, void** out_des) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12CreateRootSignatureDeserializer(src, (SIZE_T)sz,
                                                   *(const IID*)riid, out_des);
}

int TRT_GUEST_ABI host_d3d12_deser_vroot(const void* src, u64 sz,
                                         const void* riid, void** out_des) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12CreateVersionedRootSignatureDeserializer(
      src, (SIZE_T)sz, *(const IID*)riid, out_des);
}

int TRT_GUEST_ABI host_d3d12_get_iface(const void* rclsid, const void* riid,
                                       void** out_iface) {
  if (!rclsid || !riid) return (int)E_INVALIDARG;
  return (int)::D3D12GetInterface(*(const IID*)rclsid, *(const IID*)riid,
                                  out_iface);
}

int TRT_GUEST_ABI host_d3d12_exp(u32 n, const void* iids, void* cfg,
                                 const u32* sizes) {
  return (int)::D3D12EnableExperimentalFeatures(
      n, (const IID*)iids, cfg, const_cast<UINT*>(sizes));
}

DXGI_FORMAT sanitize_flip_fmt(u32 fmt) {
  switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

// Release the launcher's reference without touching guest shaders, resources,
// or render targets. An outstanding view owned by the guest is DXGI's error
// to report when ResizeBuffers runs.
void unbind_launcher_target() {
  if (!g_rtv) return;
  ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  winrt::com_ptr<ID3D11DepthStencilView> depth;
  g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, depth.put());
  UINT count = 0;
  bool changed = false;
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
    if (views[i] == g_rtv.get()) {
      views[i]->Release();
      views[i] = nullptr;
      changed = true;
    }
    if (views[i]) count = i + 1;
  }
  if (changed) {
    g_ctx->OMSetRenderTargetsAndUnorderedAccessViews(
        count, views, depth.get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  }
  for (auto* view : views)
    if (view) view->Release();
}

// A flip-model swapchain takes an UNORM format. D3D11 still permits an
// sRGB render-target view on that buffer when the guest requests sRGB.
HRESULT resize_d3d11_backbuffers(u32 width, u32 height, u32 format) {
  if (!g_swap || !g_ctx || !g_device) return E_FAIL;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  HRESULT hr = g_swap->GetDesc1(&desc);
  if (FAILED(hr)) return hr;
  const auto wanted = format == DXGI_FORMAT_UNKNOWN ? desc.Format : sanitize_flip_fmt(format);
  const bool reconfigure = desc.Format != wanted || desc.Width != width || desc.Height != height;
  if (!reconfigure && g_rtv) return S_OK;

  if (reconfigure) {
    unbind_launcher_target();
    g_rtv = nullptr;
    g_ctx->Flush();
    hr = g_swap->ResizeBuffers(0, width, height, wanted, desc.Flags);
  }
  if (SUCCEEDED(hr)) {
    g_backbuffer_width = width;
    g_backbuffer_height = height;
  }
  winrt::com_ptr<ID3D11Texture2D> back;
  const HRESULT buffer_hr = g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
  const HRESULT view_hr = FAILED(buffer_hr) ? buffer_hr :
      g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put());
  d3d9_rt::g_rt.rtv = g_rtv.get();
  gl_rt::g_glrt.main_rtv = g_rtv.get();
  vk_rt::g_vkrt.main_rtv = g_rtv.get();
  if (SUCCEEDED(hr) && FAILED(view_hr)) hr = view_hr;
  char message[120];
  snprintf(message, sizeof(message), "d3d: CoreWindow buffers %ux%u format %u, hr=0x%08lx",
           width, height, static_cast<unsigned>(wanted),
           static_cast<unsigned long>(hr));
  log_line(message);
  return hr;
}

void release_guest_backbuffer() {
  if (g_guest_backbuffer && trt_esram_is_active()) {
    D3D11_TEXTURE2D_DESC desc{};
    g_guest_backbuffer->GetDesc(&desc);
    trt_esram_release_surface(desc.Width, desc.Height, desc.Format);
  }
  g_guest_backbuffer = nullptr;
}

int TRT_GUEST_ABI configure_d3d11_swapchain(u32 width, u32 height, u32 format,
                                          u32 buffer_count, u32 swap_effect) {
  if (!g_swap || !g_device || !width || !height) return E_INVALIDARG;
  if (!trt_gpu_d3d11_uses_stable_backbuffer(buffer_count, swap_effect)) {
    const HRESULT hr = resize_d3d11_backbuffers(width, height, format);
    if (SUCCEEDED(hr)) release_guest_backbuffer();
    return hr;
  }

  // A desktop DISCARD chain exposes only buffer zero as an accessible
  // render target, even when BufferCount is two. Keep that texture bound
  // across presents and copy it into the rotating CoreWindow buffers.
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (FAILED(g_swap->GetDesc1(&desc))) return E_FAIL;
  const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  const auto native_format = format == DXGI_FORMAT_UNKNOWN ? desc.Format : sanitize_flip_fmt(format);
  DXGI_FORMAT guest_format = srgb ? static_cast<DXGI_FORMAT>(format) : native_format;
  if (g_guest_backbuffer) {
    D3D11_TEXTURE2D_DESC current{};
    g_guest_backbuffer->GetDesc(&current);
    if (format == DXGI_FORMAT_UNKNOWN) guest_format = current.Format;
    if (current.Width == width && current.Height == height && current.Format == guest_format)
      return resize_d3d11_backbuffers(width, height, format);
  }
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = width;
  texture.Height = height;
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = guest_format;
  texture.SampleDesc.Count = 1;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  winrt::com_ptr<ID3D11Texture2D> target;
  const HRESULT created = g_device->CreateTexture2D(&texture, nullptr, target.put());
  if (FAILED(created)) return created;
  const HRESULT resized = resize_d3d11_backbuffers(width, height, format);
  if (FAILED(resized)) return resized;
  release_guest_backbuffer();
  if (trt_esram_is_active()) {
    trt_esram_qualify_and_alloc(texture.Width, texture.Height, texture.Format, texture.BindFlags, "CoreWindow_backbuffer");
  }
  g_guest_backbuffer = std::move(target);
  log_line("d3d: using a stable guest backbuffer over the CoreWindow flip chain");
  return S_OK;
}

int TRT_GUEST_ABI get_d3d11_backbuffer(u32 index, const void* iid, void** out) {
  if (!g_guest_backbuffer) return E_NOTIMPL;
  if (out) *out = nullptr;
  if (index != 0 || !out || !iid) return DXGI_ERROR_INVALID_CALL;
  return g_guest_backbuffer->QueryInterface(*static_cast<const IID*>(iid), out);
}

// Paint a small arrow on the presentation surface. ClearView writes just
// these rectangles and leaves the guest's shaders and bindings intact.
void draw_guest_cursor() {
  if (!g_ctx || !g_rtv || !g_w || !g_h) return;
  if (!g_guest_cursor_visible.load()) return;
  int x, y;
  if (!trt_cursor_snapshot(&x, &y)) return;
  x = static_cast<int>(static_cast<i64>(x) * g_backbuffer_width / g_w);
  y = static_cast<int>(static_cast<i64>(y) * g_backbuffer_height / g_h);
  const int target_width = static_cast<int>(g_backbuffer_width);
  const int target_height = static_cast<int>(g_backbuffer_height);
  constexpr const char* rows[] = {
      "#", "##", "#o#", "#oo#", "#ooo#", "#oooo#", "#ooooo#", "#oooooo#",
      "#ooooooo#", "#oooooooo#", "#ooooooooo#", "#ooooo#####",
      "#oo#oo#", "#o##oo#", "##..#oo#", "#...#oo#", ".....###", ".....##",
  };
  constexpr int height = static_cast<int>(std::size(rows));
  constexpr int width = 11;
  const int pixels_high = cursor_height(target_height);
  const int pixels_wide = (width * pixels_high + height - 1) / height;
  D3D11_RECT dark[cursor_height(2160) * width], light[cursor_height(2160) * width];
  unsigned n_dark = 0, n_light = 0;
  for (int dy = 0; dy < pixels_high && y + dy < target_height; ++dy) {
    if (y + dy < 0) continue;
    const char* row = rows[dy * height / pixels_high];
    const int len = static_cast<int>(strlen(row));
    for (int dx = 0; dx < pixels_wide && x + dx < target_width;) {
      const int first = dx;
      const auto get = [&](int col) {
        const int src = col * width / pixels_wide;
        return src < len ? row[src] : '.';
      };
      const char color = get(dx++);
      while (dx < pixels_wide && x + dx < target_width && get(dx) == color) ++dx;
      if (color == '.' || x + dx <= 0) continue;
      const D3D11_RECT rect{std::max(0, x + first), y + dy, x + dx, y + dy + 1};
      if (color == '#') dark[n_dark++] = rect;
      else light[n_light++] = rect;
    }
  }
  const float black[4] = {0.015f, 0.015f, 0.015f, 1.f};
  const float fill[4] = {0.98f, 0.98f, 0.98f, 1.f};
  if (n_dark) g_ctx->ClearView(g_rtv.get(), black, dark, n_dark);
  if (n_light) g_ctx->ClearView(g_rtv.get(), fill, light, n_light);
}

void end_guest_draw_sample() {
  if (g_guest_draw_query_active) {
    g_ctx->End(g_guest_draw_query.get());
    g_guest_draw_query_active = false;
    g_guest_draw_query_pending = true;
  }
  if (!g_guest_draw_query_pending) return;

  D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
  const HRESULT hr = g_ctx->GetData(g_guest_draw_query.get(), &stats, sizeof(stats),
                                   D3D11_ASYNC_GETDATA_DONOTFLUSH);
  if (hr == S_FALSE) return;
  g_guest_draw_query_pending = false;
  if (FAILED(hr)) {
    g_guest_draw_result.store(hr);
    g_guest_draw_state.store(3);
    return;
  }
  g_guest_draw_samples.fetch_add(1);
  --g_guest_draw_samples_remaining;
  if (stats.IAVertices || stats.PSInvocations) {
    if (g_guest_nonempty_draw_samples.fetch_add(1) == 0) {
      g_guest_draw_vertices.store(stats.IAVertices);
      g_guest_draw_pixels.store(stats.PSInvocations);
      g_guest_draw_samples_remaining = 31;
    }
    g_guest_draw_state.store(2);
  }
  if (g_guest_draw_state.load() == 2) g_guest_draw_pause = 120;
  else if (!g_guest_draw_samples_remaining) g_guest_draw_state.store(4);
}

void begin_guest_draw_sample() {
  if (!g_guest_draw_samples_remaining || g_guest_draw_state.load() >= 3 ||
      g_guest_draw_query_pending) return;
  if (g_guest_draw_pause) {
    --g_guest_draw_pause;
    return;
  }
  if (!g_guest_draw_query) {
    const D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    const HRESULT hr = g_device->CreateQuery(&desc, g_guest_draw_query.put());
    if (FAILED(hr)) {
      g_guest_draw_result.store(hr);
      g_guest_draw_state.store(3);
      return;
    }
  }
  // Start after drawing the host cursor. Only guest work between presents
  // contributes to the next result. Query polling never flushes or waits.
  g_ctx->Begin(g_guest_draw_query.get());
  g_guest_draw_query_active = true;
  if (g_guest_draw_state.load() == 0) g_guest_draw_state.store(1);
}

HRESULT enqueue_surface_sample(surface_sample& sample, ID3D11Texture2D* source,
                               DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN) {
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  const bool typeless = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
  const DXGI_FORMAT format = typeless ? view_format : desc.Format;
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      break;
    default: return E_NOTIMPL;
  }
  if (desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height) return E_NOTIMPL;

  if (!sample.texture || sample.format != format) {
    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = staging.Height = kGuestProbeGrid;
    staging.MipLevels = staging.ArraySize = 1;
    staging.Format = format;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> texture;
    const HRESULT hr = g_device->CreateTexture2D(&staging, nullptr, texture.put());
    if (FAILED(hr)) return hr;
    sample.texture = std::move(texture);
    sample.format = format;
  }

  // Sample across the image. Queue only sixteen one-pixel copies, and never
  // transfer the image itself to the diagnostics endpoint.
  for (unsigned y = 0; y < kGuestProbeGrid; ++y) {
    for (unsigned x = 0; x < kGuestProbeGrid; ++x) {
      const unsigned px = (2 * x + 1) * desc.Width / (2 * kGuestProbeGrid);
      const unsigned py = (2 * y + 1) * desc.Height / (2 * kGuestProbeGrid);
      const D3D11_BOX region{px, py, 0, px + 1, py + 1, 1};
      g_ctx->CopySubresourceRegion(sample.texture.get(), 0, x, y, 0, source, 0, &region);
    }
  }
  sample.pending = true;
  return S_OK;
}

HRESULT poll_surface_sample(surface_sample& sample) {
  if (!sample.pending) return S_OK;
  D3D11_MAPPED_SUBRESOURCE map{};
  const HRESULT hr = g_ctx->Map(sample.texture.get(), 0, D3D11_MAP_READ,
                                D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return S_FALSE;
  if (FAILED(hr)) return hr;
  sample.lit = trt_gpu_count_lit_rgb32(map.pData, kGuestProbeGrid,
                                      kGuestProbeGrid, map.RowPitch);
  g_ctx->Unmap(sample.texture.get(), 0);
  sample.pending = false;
  return S_OK;
}

void sample_other_render_target(ID3D11RenderTargetView* view, ID3D11Resource* resource) {
  if (g_other_surface_probe.pending || !resource) return;
  winrt::com_ptr<ID3D11Texture2D> texture;
  HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), texture.put_void());
  if (FAILED(hr)) {
    g_other_surface_result.store(hr);
    return;
  }
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  g_other_width.store(desc.Width);
  g_other_height.store(desc.Height);
  g_other_format.store(desc.Format);
  D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
  view->GetDesc(&view_desc);
  hr = E_NOTIMPL;
  if (view_desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
      view_desc.Texture2D.MipSlice == 0) {
    hr = enqueue_surface_sample(g_other_surface_probe, texture.get(), view_desc.Format);
  }
  g_other_surface_result.store(hr);
}

void sample_guest_shader_input() {
  if (g_shader_input_surface.pending) return;
  g_shader_input_snapshots.fetch_add(1);
  ID3D11ShaderResourceView* bound[4]{};
  g_ctx->PSGetShaderResources(0, static_cast<UINT>(std::size(bound)), bound);
  bool chosen = false;
  for (unsigned slot = 0; slot < std::size(bound); ++slot) {
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    view.attach(bound[slot]);  // PSGetShaderResources retained every bound view.
    if (!view) continue;
    g_shader_input_bound.fetch_add(1);
    if (chosen) continue;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    view->GetDesc(&srv);
    if (srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        srv.Texture2D.MostDetailedMip != 0) continue;
    winrt::com_ptr<ID3D11Resource> resource;
    view->GetResource(resource.put());
    auto texture = resource.try_as<ID3D11Texture2D>();
    if (!texture) continue;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    g_shader_input_slot.store(slot);
    g_shader_input_width.store(desc.Width);
    g_shader_input_height.store(desc.Height);
    g_shader_input_format.store(srv.Format);
    const HRESULT hr = enqueue_surface_sample(g_shader_input_surface, texture.get(), srv.Format);
    g_shader_input_result.store(hr);
    chosen = SUCCEEDED(hr);
  }
}

void measure_surface_pair(ID3D11Texture2D* guest, ID3D11Texture2D* host) {
  if (g_shader_input_surface.pending) {
    const HRESULT hr = poll_surface_sample(g_shader_input_surface);
    if (hr == S_OK) {
      g_shader_input_lit.store(std::max(g_shader_input_lit.load(), g_shader_input_surface.lit));
      g_shader_input_samples.fetch_add(1);
    } else if (FAILED(hr)) {
      g_shader_input_surface.pending = false;
      g_shader_input_result.store(hr);
    }
  }
  if (g_other_surface_probe.pending) {
    const HRESULT hr = poll_surface_sample(g_other_surface_probe);
    if (hr == S_OK) {
      g_other_lit_pixels.store(std::max(g_other_lit_pixels.load(), g_other_surface_probe.lit));
      g_other_surface_samples.fetch_add(1);
    } else if (FAILED(hr)) {
      g_other_surface_probe.pending = false;
      g_other_surface_result.store(hr);
    }
  }
  if (g_surface_probe_result.load() < 0 || g_surface_samples.load() >= 32) return;
  if (g_guest_surface_probe.pending || g_host_surface_probe.pending) {
    const HRESULT guest_hr = poll_surface_sample(g_guest_surface_probe);
    const HRESULT host_hr = poll_surface_sample(g_host_surface_probe);
    if (FAILED(guest_hr) || FAILED(host_hr)) {
      g_surface_probe_result.store(FAILED(guest_hr) ? guest_hr : host_hr);
      return;
    }
    if (guest_hr == S_FALSE || host_hr == S_FALSE) return;
    g_guest_lit_pixels.store(std::max(g_guest_lit_pixels.load(), g_guest_surface_probe.lit));
    g_host_lit_pixels.store(std::max(g_host_lit_pixels.load(), g_host_surface_probe.lit));
    g_surface_samples.fetch_add(1);
    g_surface_probe_interval = 120;
    return;
  }

  // Sample about once every two seconds at 60 Hz. The copy is observed before
  // the cursor overlay, so a lit cursor cannot be mistaken for game pixels.
  if (g_surface_probe_interval) {
    --g_surface_probe_interval;
    return;
  }
  const HRESULT guest_hr = enqueue_surface_sample(g_guest_surface_probe, guest);
  const HRESULT host_hr = SUCCEEDED(guest_hr)
      ? enqueue_surface_sample(g_host_surface_probe, host) : guest_hr;
  if (FAILED(host_hr)) {
    g_surface_probe_result.store(host_hr);
    return;
  }
  sample_guest_shader_input();
  // Snapshot slot zero at Present without disturbing the guest context. A
  // completed rendering pass may already have unbound its render target.
  winrt::com_ptr<ID3D11RenderTargetView> view;
  g_ctx->OMGetRenderTargets(1, view.put(), nullptr);
  if (view) {
    winrt::com_ptr<ID3D11Resource> resource;
    view->GetResource(resource.put());
    if (resource.get() == static_cast<ID3D11Resource*>(guest)) {
      g_surface_guest_rtv.fetch_add(1);
    } else {
      g_surface_other_rtv.fetch_add(1);
      sample_other_render_target(view.get(), resource.get());
    }
  }
}

void TRT_GUEST_ABI prepare_d3d11_present() {
  if (!g_swap || !g_ctx || !g_device) return;
  end_guest_draw_sample();
  if (g_guest_backbuffer) {
    winrt::com_ptr<ID3D11Texture2D> back;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) {
      g_ctx->CopyResource(back.get(), g_guest_backbuffer.get());
      measure_surface_pair(g_guest_backbuffer.get(), back.get());
    }
  }
  draw_guest_pointer();
  begin_guest_draw_sample();
}

// Host GPU submission for replayed D3D12 command lists. This issues real
// D3D11 DrawInstanced calls on the host context (the device that owns the
// CoreWindow swapchain, possibly D3D11on12). It is a genuine host draw
// submission; guest PSO/root-signature translation remains a separate step.
static int TRT_GUEST_ABI host_d3d12_submit(const trt_d3d12_submit_desc* sd) {
  if (!sd) return (int)E_INVALIDARG;
  if (!g_ctx || g_in_d3d12) return (int)E_FAIL;  // no D3D11 path while D3D12 swap is active
  for (u32 i = 0; i < sd->count && sd->draws; ++i) {
    const auto& d = sd->draws[i];
    g_ctx->DrawInstanced(d.vertices, d.instances,
                         d.start_vertex, d.start_instance);
  }
  return (int)S_OK;
}

void publish_gpu_hooks() {
  trt_gpu_hooks hooks{};
  hooks.device = g_device.get();
  hooks.context = g_ctx.get();
  hooks.swapchain = g_swap.get();
  hooks.factory = g_factory.get();
  hooks.feature_level = g_fl;
  hooks.width = g_w;
  hooks.height = g_h;
  hooks.factory6 = g_factory6.get();
  hooks.swapchain3 = g_swap3.get();
  hooks.d3d12_device = g_d3d12_device.get();
  hooks.d3d12_swapchain = g_d3d12_swap.get();
  hooks.acquire_d3d12_swapchain = [](void* queue, u32 w, u32 h, u32 fmt,
                                     u32 count, void** out_sc) -> int TRT_GUEST_ABI {
    if (!queue || !g_window || !g_factory) return (int)E_INVALIDARG;
    g_rtv = nullptr;
    g_swap3 = nullptr;
    g_swap = nullptr;
    g_d3d12_swap = nullptr;
    if (g_ctx) g_ctx->Flush();

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = w ? w : g_w;
    sc.Height = h ? h : g_h;
    sc.Format = sanitize_flip_fmt(fmt);
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = (count < 2) ? 2 : (count > 4 ? 4 : count);
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    winrt::com_ptr<IDXGISwapChain1> sc1;
    const HRESULT hr = g_factory->CreateSwapChainForCoreWindow(
        (IUnknown*)queue, g_window.get(), &sc, nullptr, sc1.put());
    if (FAILED(hr)) return (int)hr;
    g_d3d12_swap = sc1.try_as<IDXGISwapChain3>();
    g_in_d3d12 = true;
    if (out_sc) *out_sc = g_d3d12_swap ? (void*)g_d3d12_swap.get() : (void*)sc1.get();
    return S_OK;
  };
  hooks.restore_d3d11_swapchain = &swapchain_restore_d3d11;
  hooks.configure_d3d11_swapchain = &configure_d3d11_swapchain;
  hooks.get_d3d11_backbuffer = &get_d3d11_backbuffer;
  hooks.prepare_d3d11_present = &prepare_d3d11_present;
  hooks.d3d11on12_create = [](void* dev, u32 flags, const void* levels,
                              u32 count, void* const* queues, u32 num_queues,
                              u32 node_mask, void** out_dev, void** out_ctx,
                              u32* out_level) -> int TRT_GUEST_ABI {
    return (int)::D3D11On12CreateDevice(
        (IUnknown*)dev, flags, (const D3D_FEATURE_LEVEL*)levels, count,
        (IUnknown* const*)queues, num_queues, node_mask,
        (ID3D11Device**)out_dev, (ID3D11DeviceContext**)out_ctx,
        (D3D_FEATURE_LEVEL*)out_level);
  };
  trt_gpu_set_hooks(&hooks);
}

HRESULT create_d3d11_corewindow_swapchain() {
  if (!g_device || !g_factory || !g_window) return E_FAIL;
  g_rtv = nullptr;
  g_swap3 = nullptr;
  g_swap = nullptr;
  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = g_w;
  sc.Height = g_h;
  sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 2;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sc.Flags = 0;
  const HRESULT hr = g_factory->CreateSwapChainForCoreWindow(
      g_device.get(), g_window.get(), &sc, nullptr, g_swap.put());
  if (FAILED(hr)) return hr;
  g_backbuffer_width = sc.Width;
  g_backbuffer_height = sc.Height;
  g_swap3 = g_swap.try_as<IDXGISwapChain3>();
  winrt::com_ptr<ID3D11Texture2D> back;
  g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
  if (back) g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put());
  return S_OK;
}
} // namespace

void swapchain_reset_guest_draw_probe() {
  g_guest_cursor_present_ms.store(0);
  g_guest_cursor_visible.store(false);
  if (g_guest_draw_query_active && g_ctx) g_ctx->End(g_guest_draw_query.get());
  g_guest_draw_query = nullptr;
  g_guest_draw_query_active = false;
  g_guest_draw_query_pending = false;
  g_guest_draw_samples.store(0);
  g_guest_nonempty_draw_samples.store(0);
  g_guest_draw_samples_remaining = 120;
  g_guest_draw_pause = 0;
  g_guest_draw_vertices.store(0);
  g_guest_draw_pixels.store(0);
  g_guest_draw_result.store(0);
  g_guest_draw_state.store(0);
  g_guest_surface_probe = {};
  g_host_surface_probe = {};
  g_other_surface_probe = {};
  g_surface_samples.store(0);
  g_guest_lit_pixels.store(0);
  g_host_lit_pixels.store(0);
  g_surface_probe_result.store(0);
  g_surface_guest_rtv.store(0);
  g_surface_other_rtv.store(0);
  g_other_surface_samples.store(0);
  g_other_lit_pixels.store(0);
  g_other_width.store(0);
  g_other_height.store(0);
  g_other_format.store(0);
  g_other_surface_result.store(0);
  g_shader_input_surface = {};
  g_shader_input_snapshots.store(0);
  g_shader_input_bound.store(0);
  g_shader_input_samples.store(0);
  g_shader_input_lit.store(0);
  g_shader_input_slot.store(0);
  g_shader_input_width.store(0);
  g_shader_input_height.store(0);
  g_shader_input_format.store(0);
  g_shader_input_result.store(0);
  g_surface_probe_interval = 0;
}

guest_shader_input_probe swapchain_guest_shader_input_probe() {
  return {g_shader_input_snapshots.load(), g_shader_input_bound.load(),
          g_shader_input_samples.load(), g_shader_input_lit.load(),
          g_shader_input_slot.load(), g_shader_input_width.load(),
          g_shader_input_height.load(), g_shader_input_format.load(),
          g_shader_input_result.load()};
}

guest_draw_probe swapchain_guest_draw_probe() {
  return {g_guest_draw_state.load(), g_guest_draw_samples.load(),
          g_guest_draw_vertices.load(), g_guest_draw_pixels.load(),
          g_guest_draw_result.load(), g_surface_samples.load(),
          g_guest_lit_pixels.load(), g_host_lit_pixels.load(), g_surface_probe_result.load(),
          g_guest_nonempty_draw_samples.load(), g_surface_guest_rtv.load(),
          g_surface_other_rtv.load(), g_other_surface_samples.load(), g_other_lit_pixels.load(),
          g_other_width.load(), g_other_height.load(), g_other_format.load(),
          g_other_surface_result.load()};
}


void draw_guest_pointer() {
  g_guest_cursor_present_ms.store(g_rtv ? GetTickCount64() : 0);
  draw_guest_cursor();
}

bool guest_cursor_overlay_ready(uint64_t now) {
  return pointer_event_recent(now, g_guest_cursor_present_ms.load(), 2000);
}

void set_guest_cursor_visible(bool visible) { g_guest_cursor_visible.store(visible); }

swapchain_result swapchain_create(winrt::Windows::UI::Core::CoreWindow const& w) {
  swapchain_result r{};
  auto bounds = w.Bounds();
  // Settings knob: resolution_scale (%) renders below native size and lets
  // DXGI scale up — the main GPU headroom lever on Series S-class targets.
  const int scale = trt_settings_resolution_scale();
  g_w = (unsigned)bounds.Width;
  g_h = (unsigned)bounds.Height;
  if (scale > 0 && scale != 100) {
    g_w = (unsigned)((u64)g_w * (u64)scale / 100ull);
    g_h = (unsigned)((u64)g_h * (u64)scale / 100ull);
    if (g_w < 320) g_w = 320;
    if (g_h < 180) g_h = 180;
  }
  r.width = g_w;
  r.height = g_h;
  trt_input_display_size(static_cast<int>(g_w), static_cast<int>(g_h));
  winrt::copy_from_abi(g_window, winrt::get_abi(w));

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  // Ask for what THIS console can actually give. The One family is GCN 1.1 and
  // caps at 11_0; requesting 11_1 there only obscures the level the device came
  // up at (the runtime would silently pick 11_0 while the log said 11_1 was
  // requested). console_feature_levels is offline-tested.
  const longan::console_hardware_info cons_info =
      longan::get_detected_console_info();
  unsigned level_values[6] = {};
  const int level_count =
      longan::console_feature_levels(cons_info.generation, level_values, 6);
  D3D_FEATURE_LEVEL levels[6] = {};
  for (int i = 0; i < level_count; ++i)
    levels[i] = static_cast<D3D_FEATURE_LEVEL>(level_values[i]);
  {
    char line[256];
    snprintf(line, sizeof(line),
             "[swapchain] console=%s (%s) last_gen=%d avx2=%d esram=%dMB "
             "budget=%dMB d3d11_levels=%d top=0x%x",
             cons_info.generation_name, cons_info.family_id,
             cons_info.is_last_gen ? 1 : 0, cons_info.has_avx2 ? 1 : 0,
             cons_info.esram_size_mb, cons_info.dev_mode_game_budget_mb,
             level_count, level_count ? level_values[0] : 0u);
    trt_core_log(line);
  }
  D3D_FEATURE_LEVEL achieved{};
  winrt::com_ptr<ID3D11Device> dev;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  r.device_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  flags, levels, (UINT)level_count,
                                  D3D11_SDK_VERSION, dev.put(), &achieved,
                                  ctx.put());
  if (FAILED(r.device_hr)) return r;
  g_fl = (unsigned)achieved;
  r.feature_level = g_fl;
  g_device = dev.as<ID3D11Device1>();
  g_ctx = ctx.as<ID3D11DeviceContext1>();
  // Video-memory baseline at boot: is the 3.8GB video usage seen in
  // TEX-OOM-DIAG (ultrapool) a driver/system reservation, or game-
  // allocated? Logged once at device creation, before any game runs.
  // .199: uses trt_core_log (the .197/.198 log_line version never
  // logged) + a probe line + per-step failure logging.
  {
    trt_core_log("[swapchain] VIDEO-BASELINE probing...");
    IDXGIDevice* dxgi_dev0 = nullptr;
    HRESULT hr_qi = dev ? dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev0) : E_POINTER;
    if (SUCCEEDED(hr_qi) && dxgi_dev0) {
      IDXGIAdapter* adapter0 = nullptr;
      HRESULT hr_ad = dxgi_dev0->GetAdapter(&adapter0);
      if (SUCCEEDED(hr_ad) && adapter0) {
        IDXGIAdapter3* adapter30 = nullptr;
        HRESULT hr_a3 = adapter0->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter30);
        if (SUCCEEDED(hr_a3) && adapter30) {
          DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
          HRESULT hr_vmi = adapter30->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi);
          if (SUCCEEDED(hr_vmi)) {
            // .222 DIAG: BOTH segments + driver reservation at
            // boot, matching VIDEO-SAMPLE -- is the ~3.7GB a
            // reservation present from boot, and in which
            // segment?
            DXGI_QUERY_VIDEO_MEMORY_INFO vmi_nl{};
            const bool ok_nl = SUCCEEDED(adapter30->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vmi_nl));
            char vb[512];
            snprintf(vb, sizeof(vb),
                     "[swapchain] VIDEO-BASELINE local budget=%lluMB usage=%lluMB avail=%lluMB resv=%lluMB nonlocal budget=%lldMB usage=%lldMB avail=%lldMB resv=%lldMB",
                     (unsigned long long)(vmi.Budget >> 20), (unsigned long long)(vmi.CurrentUsage >> 20),
                     (unsigned long long)(vmi.AvailableForReservation >> 20),
                     (unsigned long long)(vmi.CurrentReservation >> 20),
                     ok_nl ? (long long)(vmi_nl.Budget >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.CurrentUsage >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.AvailableForReservation >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.CurrentReservation >> 20) : -1ll);
            trt_core_log(vb);
          } else {
            char vb[128];
            snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE QueryVideoMemoryInfo failed hr=0x%08x", (unsigned)hr_vmi);
            trt_core_log(vb);
          }
          adapter30->Release();
        } else {
          char vb[128];
          snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE IDXGIAdapter3 QI failed hr=0x%08x", (unsigned)hr_a3);
          trt_core_log(vb);
        }
        adapter0->Release();
      } else {
        char vb[128];
        snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE GetAdapter failed hr=0x%08x", (unsigned)hr_ad);
        trt_core_log(vb);
      }
      dxgi_dev0->Release();
    } else {
      char vb[160];
      snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE IDXGIDevice QI failed hr=0x%08x dev=%p", (unsigned)hr_qi, (void*)dev.get());
      trt_core_log(vb);
    }
  }

  winrt::com_ptr<IDXGIDevice1> dxgi_dev;
  g_device.as(dxgi_dev);
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(adapter.put());
  adapter->GetParent(__uuidof(IDXGIFactory2), g_factory.put_void());
  if (g_factory) g_factory6 = g_factory.try_as<IDXGIFactory6>();

  r.sc_hr = create_d3d11_corewindow_swapchain();
  if (FAILED(r.sc_hr)) return r;

  trt_d3d12_hooks d12{};
  d12.create_device = &host_d3d12_create_device;
  d12.get_debug_interface = &host_d3d12_get_debug;
  d12.serialize_root_sig = &host_d3d12_ser_root;
  d12.serialize_versioned_root_sig = &host_d3d12_ser_vroot;
  d12.create_root_deserializer = &host_d3d12_deser_root;
  d12.create_versioned_root_deserializer = &host_d3d12_deser_vroot;
  d12.get_interface = &host_d3d12_get_iface;
  d12.enable_experimental = &host_d3d12_exp;
  d12.submit = &host_d3d12_submit;
  trt_d3d12_set_hooks(&d12);

  publish_gpu_hooks();
  trt_d3dx_set_compiler((void*)&D3DCompile);
  return r;
}

void swapchain_restore_d3d11() {
  if (!g_in_d3d12) return;
  g_d3d12_swap = nullptr;
  g_in_d3d12 = false;
  create_d3d11_corewindow_swapchain();
  publish_gpu_hooks();
}

void swapchain_restore_launcher_format() {
  release_guest_backbuffer();
  const HRESULT hr = resize_d3d11_backbuffers(g_w, g_h, DXGI_FORMAT_B8G8R8A8_UNORM);
  if (FAILED(hr)) log_line("d3d: could not restore launcher backbuffers");
}

void swapchain_render_frame(float t) {
  if (!g_ctx || !g_rtv || g_in_d3d12) return;
  static winrt::com_ptr<ID3D11VertexShader> vs;
  static winrt::com_ptr<ID3D11PixelShader> ps;
  static winrt::com_ptr<ID3D11Buffer> cb;
  static bool compiled = false;
  if (!compiled) {
    compiled = true;
    winrt::com_ptr<ID3DBlob> vb, pb, err;
    if (SUCCEEDED(D3DCompile(k_vs, strlen(k_vs), nullptr, nullptr, nullptr,
                             "main", "vs_5_0", 0, 0, vb.put(), err.put())))
      g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(),
                                   nullptr, vs.put());
    if (SUCCEEDED(D3DCompile(k_ps, strlen(k_ps), nullptr, nullptr, nullptr,
                             "main", "ps_5_0", 0, 0, pb.put(), err.put()))) {
      g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                  nullptr, ps.put());
      log_line("swapchain: liquid-silk background shader compiled OK");
    } else if (err && err->GetBufferPointer()) {
      char em[256];
      snprintf(em, sizeof(em), "swapchain: bg PS compile error: %.200s",
               (const char*)err->GetBufferPointer());
      log_line(em);
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&bd, nullptr, cb.put());
  }
  const float clear[4] = {0.026f, 0.023f, 0.034f, 1.0f};
  g_ctx->ClearRenderTargetView(g_rtv.get(), clear);
  if (vs && ps) {
    if (cb) {
      float params[4] = {t, (float)g_w, (float)g_h, 0.0f};
      g_ctx->UpdateSubresource(cb.get(), 0, nullptr, params, 0, 0);
      ID3D11Buffer* cbs[1] = {cb.get()};
      g_ctx->PSSetConstantBuffers(0, 1, cbs);
    }
    ID3D11RenderTargetView* rtv = g_rtv.get();
    D3D11_VIEWPORT vp{0.f, 0.f, (float)g_w, (float)g_h, 0.f, 1.f};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(vs.get(), nullptr, 0);
    g_ctx->PSSetShader(ps.get(), nullptr, 0);
    g_ctx->Draw(3, 0);
  }
}

void swapchain_bind_d3d9() {
  d3d9_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
                g_w, g_h);
  gl_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
              g_w, g_h);
  vk_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
              g_w, g_h);
}

void swapchain_present() {
  if (!g_swap || g_in_d3d12) return;
  // Settings knobs (community wave): vsync off -> Present(0), frame-rate cap
  // -> minimum frame time enforced on the UI thread's present cadence.
  const bool vsync = trt_settings_vsync();
  const int cap = trt_settings_frame_rate_cap();
  static std::chrono::steady_clock::time_point last_present{};
  auto now = std::chrono::steady_clock::now();
  if (cap > 0) {
    const auto min_dt = std::chrono::milliseconds(1000 / cap);
    auto since = now - last_present;
    if (since < min_dt) {
      const auto wait = min_dt - since;
      Sleep((unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(
                wait)
                .count());
      now = std::chrono::steady_clock::now();
    }
  }
  last_present = now;
  g_swap->Present(vsync ? 1 : 0, 0);
}

void* swapchain_dxgi() { return g_swap.get(); }
void* swapchain_d3d() { return g_device.get(); }

void swapchain_shutdown() {
  d3d9_rt::shutdown();
  gl_rt::shutdown();
  vk_rt::shutdown();
  g_d3d12_swap = nullptr;
  g_d3d12_device = nullptr;
  g_guest_backbuffer = nullptr;
  g_rtv = nullptr;
  g_swap3 = nullptr;
  g_swap = nullptr;
  g_ctx = nullptr;
  g_device = nullptr;
  g_factory6 = nullptr;
  g_factory = nullptr;
  g_window = nullptr;
}

} // namespace longan
