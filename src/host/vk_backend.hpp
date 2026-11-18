// tiramisu/src/host/vk_backend.hpp
// UWP Direct3D 11 execution backend for Vulkan 1.0-1.3 (bridge_vulkan.h).
// Header-only matching src/host/gl_backend.hpp; bound in src/host/swapchain.cpp.
#pragma once

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "../trt/bridge_vulkan.h"
#include "../trt/spirv_hlsl.h"
#include "../trt/thunk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

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
  ID3D11Texture2D* tex2d = nullptr;
  ID3D11Texture3D* tex3d = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
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
    case 104u: return DXGI_FORMAT_R32G32B32_UINT;
    case 105u: return DXGI_FORMAT_R32G32B32_SINT;
    case 106u: return DXGI_FORMAT_R32G32B32_FLOAT;
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
  ID3DBlob* errs = nullptr;
  HRESULT hr = D3DCompile(src, len, nullptr, nullptr, nullptr, "main", profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errs);
  if (FAILED(hr) || !code) {
    if (errs) {
      char buf[384];
      snprintf(buf, sizeof(buf), "[vk_backend] %s compile failed: %.300s",
               profile, (const char*)errs->GetBufferPointer());
      trt_core_log(buf);
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
    cap = aligned;
  }
  if (data && data_bytes) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(g_vkrt.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      const uint32_t cp = std::min<uint32_t>(cap, data_bytes);
      memset(ms.pData, 0, cap);
      memcpy(ms.pData, data, cp);
      g_vkrt.ctx->Unmap(cb, 0);
    }
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
    if (data && bytes) {
      init_pad.assign(bd.ByteWidth, 0);
      memcpy(init_pad.data(), data, (size_t)std::min<uint64_t>(bytes, bd.ByteWidth));
      init.pSysMem = init_pad.data();
    }
    g_vkrt.dev->CreateBuffer(&bd, init.pSysMem ? &init : nullptr, &b.buf);

    if (b.buf && (bind_flags & D3D11_BIND_SHADER_RESOURCE)) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sd.BufferEx.NumElements = std::max<UINT>(1u, bd.ByteWidth / 4u);
      sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      g_vkrt.dev->CreateShaderResourceView(b.buf, &sd, &b.srv);
    }
    if (b.buf && (bind_flags & D3D11_BIND_UNORDERED_ACCESS)) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = std::max<UINT>(1u, bd.ByteWidth / 4u);
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      g_vkrt.dev->CreateUnorderedAccessView(b.buf, &ud, &b.uav);
    }
    if (is_cb) {
      D3D11_BUFFER_DESC cbd{};
      cbd.ByteWidth = (UINT)std::min<uint64_t>((sz + 15ull) & ~15ull, 65536ull);
      cbd.Usage = D3D11_USAGE_DEFAULT;
      cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (data && bytes) {
        init_pad.assign(cbd.ByteWidth, 0);
        memcpy(init_pad.data(), data, (size_t)std::min<uint64_t>(bytes, cbd.ByteWidth));
        init.pSysMem = init_pad.data();
        g_vkrt.dev->CreateBuffer(&cbd, &init, &b.cb);
      } else {
        g_vkrt.dev->CreateBuffer(&cbd, nullptr, &b.cb);
      }
    }
  } else if (data && bytes && g_vkrt.ctx) {
    if (b.buf) {
      D3D11_BOX box{0, 0, 0, (UINT)std::min<uint64_t>(bytes, b.bytes), 1, 1};
      g_vkrt.ctx->UpdateSubresource(b.buf, 0, &box, data, 0, 0);
    }
    if (b.cb && bytes <= 65536ull) {
      D3D11_BUFFER_DESC cbd{};
      b.cb->GetDesc(&cbd);
      if (bytes >= cbd.ByteWidth) {
        g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, data, 0, 0);
      } else {
        std::vector<uint8_t> pad(cbd.ByteWidth, 0);
        memcpy(pad.data(), data, (size_t)bytes);
        g_vkrt.ctx->UpdateSubresource(b.cb, 0, nullptr, pad.data(), 0, 0);
      }
    }
  }

  if (data && bytes) {
    if (b.shadow.size() < bytes) b.shadow.resize((size_t)bytes);
    memcpy(b.shadow.data(), data, (size_t)bytes);
  }
  return 1;
}

inline void hk_destroy_buffer(u64 buf_id, void*) {
  auto it = g_vkrt.bufs.find(buf_id);
  if (it != g_vkrt.bufs.end()) {
    free_buf_rec(it->second);
    g_vkrt.bufs.erase(it);
  }
}

inline u32 hk_sync_image(u64 img_id, const trt_vk_image_desc* desc, const void* data,
                         u64 bytes, u32 mip, u32 layer, void*) {
  if (!g_vkrt.initialized || !g_vkrt.dev || !desc) return 0;
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
    const bool depth = is_depth_vk_fmt(desc->vk_format) || ((desc->usage & 0x20u) != 0u);
    const bool is_cube = ((desc->flags & 0x10u) != 0u) && desc->layers >= 6u;
    const bool want_srv = !is_bc_vk_fmt(desc->vk_format) || !depth;
    const bool want_rtv = !depth && !is_bc_vk_fmt(desc->vk_format) && ((desc->usage & 0x10u) != 0u);
    const bool want_uav = !depth && !is_bc_vk_fmt(desc->vk_format) && ((desc->usage & 0x08u) != 0u);

    UINT bind_flags = 0;
    if (want_srv) bind_flags |= D3D11_BIND_SHADER_RESOURCE;
    if (depth) bind_flags |= D3D11_BIND_DEPTH_STENCIL;
    if (want_rtv) bind_flags |= D3D11_BIND_RENDER_TARGET;
    if (want_uav) bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
    if (!bind_flags) bind_flags = D3D11_BIND_SHADER_RESOURCE;

    const UINT mips = std::max<UINT>(1u, desc->mips);
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
      if (SUCCEEDED(g_vkrt.dev->CreateTexture3D(&td, nullptr, &im.tex3d)) && im.tex3d) {
        if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
          g_vkrt.dev->CreateShaderResourceView(im.tex3d, nullptr, &im.srv);
        }
        if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
          g_vkrt.dev->CreateUnorderedAccessView(im.tex3d, nullptr, &im.uav);
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
      if (SUCCEEDED(g_vkrt.dev->CreateTexture2D(&td, nullptr, &im.tex2d)) && im.tex2d) {
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
          g_vkrt.dev->CreateShaderResourceView(im.tex2d, &sd, &im.srv);
        }
        if (bind_flags & D3D11_BIND_RENDER_TARGET) {
          g_vkrt.dev->CreateRenderTargetView(im.tex2d, nullptr, &im.rtv);
        }
        if (bind_flags & D3D11_BIND_DEPTH_STENCIL) {
          D3D11_DEPTH_STENCIL_VIEW_DESC dsd{};
          dsd.Format = vk_to_dxgi_fmt(desc->vk_format, false);
          dsd.ViewDimension = (layers > 1u) ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY
                                            : D3D11_DSV_DIMENSION_TEXTURE2D;
          if (layers > 1u) dsd.Texture2DArray.ArraySize = 1;
          g_vkrt.dev->CreateDepthStencilView(im.tex2d, &dsd, &im.dsv);
        }
        if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) {
          g_vkrt.dev->CreateUnorderedAccessView(im.tex2d, nullptr, &im.uav);
        }
      }
    }
  }

  if (data && bytes && g_vkrt.ctx && (im.tex2d || im.tex3d)) {
    const UINT mips = std::max<UINT>(1u, desc->mips);
    const UINT mw = std::max<UINT>(1u, desc->w >> mip);
    const UINT mh = std::max<UINT>(1u, desc->h >> mip);
    UINT rp = 0, sp = 0;
    calc_img_pitch(desc->vk_format, mw, mh, &rp, &sp);
    const UINT sub = D3D11CalcSubresource(mip, layer, mips);
    ID3D11Resource* res = im.tex3d ? (ID3D11Resource*)im.tex3d : (ID3D11Resource*)im.tex2d;
    g_vkrt.ctx->UpdateSubresource(res, sub, nullptr, data, rp, sp);
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
  g_vkrt.ctx->VSSetShaderResources(0, 16, null_srvs);
  g_vkrt.ctx->PSSetShaderResources(0, 16, null_srvs);

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
      g_vkrt.ctx->ClearRenderTargetView(rtvs[i], rp->clear_rgba[i]);
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
  }
  if (dsv && (rp->clear_depth || rp->clear_stencil)) {
    UINT cf = 0;
    if (rp->clear_depth) cf |= D3D11_CLEAR_DEPTH;
    if (rp->clear_stencil) cf |= D3D11_CLEAR_STENCIL;
    g_vkrt.ctx->ClearDepthStencilView(dsv, cf, rp->depth_val, (UINT8)rp->stencil_val);
  }

  g_vkrt.ctx->OMSetRenderTargets(rtv_cnt, rtvs, dsv);

  D3D11_VIEWPORT vp{};
  vp.TopLeftX = (FLOAT)rp->area_x;
  vp.TopLeftY = (FLOAT)rp->area_y;
  vp.Width = (FLOAT)(rp->area_w ? rp->area_w : g_vkrt.width);
  vp.Height = (FLOAT)(rp->area_h ? rp->area_h : g_vkrt.height);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  g_vkrt.ctx->RSSetViewports(1, &vp);

  D3D11_RECT sc{rp->area_x, rp->area_y,
                rp->area_x + (LONG)(rp->area_w ? rp->area_w : g_vkrt.width),
                rp->area_y + (LONG)(rp->area_h ? rp->area_h : g_vkrt.height)};
  g_vkrt.ctx->RSSetScissorRects(1, &sc);
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
      if (to_cs) g_vkrt.ctx->CSSetConstantBuffers(reg, 1, &cb);
      if (to_vs) g_vkrt.ctx->VSSetConstantBuffers(reg, 1, &cb);
      if (to_ps) g_vkrt.ctx->PSSetConstantBuffers(reg, 1, &cb);
    };
    auto bind_srv = [&](ID3D11ShaderResourceView* srv) {
      if (reg >= 32u) return;
      if (to_cs) g_vkrt.ctx->CSSetShaderResources(reg, 1, &srv);
      if (to_vs) g_vkrt.ctx->VSSetShaderResources(reg, 1, &srv);
      if (to_ps) g_vkrt.ctx->PSSetShaderResources(reg, 1, &srv);
    };
    auto bind_smp = [&](UINT sreg, ID3D11SamplerState* smp) {
      if (!smp || sreg >= 16u) return;
      if (to_cs) g_vkrt.ctx->CSSetSamplers(sreg, 1, &smp);
      if (to_vs) g_vkrt.ctx->VSSetSamplers(sreg, 1, &smp);
      if (to_ps) g_vkrt.ctx->PSSetSamplers(sreg, 1, &smp);
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
        g_vkrt.ctx->CSSetUnorderedAccessViews(reg, 1, &bit->second.uav, nullptr);
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
        g_vkrt.ctx->CSSetUnorderedAccessViews(reg, 1, &iit->second.uav, nullptr);
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

  g_vkrt.ctx->VSSetShader(p.vs, nullptr, 0);
  g_vkrt.ctx->PSSetShader(p.ps, nullptr, 0);
  g_vkrt.ctx->IASetInputLayout(p.il);
  g_vkrt.ctx->IASetPrimitiveTopology(p.topo);
  if (p.rs) g_vkrt.ctx->RSSetState(p.rs);
  if (p.dss) g_vkrt.ctx->OMSetDepthStencilState(p.dss, 0);
  if (p.bs) g_vkrt.ctx->OMSetBlendState(p.bs, cmd->blend_const, 0xFFFFFFFFu);

  if (cmd->has_vp && cmd->vp_w > 0.0f && fabsf(cmd->vp_h) > 0.0f) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = cmd->vp_x;
    vp.TopLeftY = (cmd->vp_h < 0.0f) ? (cmd->vp_y + cmd->vp_h) : cmd->vp_y;
    vp.Width = cmd->vp_w;
    vp.Height = fabsf(cmd->vp_h);
    vp.MinDepth = cmd->vp_min_z;
    vp.MaxDepth = cmd->vp_max_z;
    g_vkrt.ctx->RSSetViewports(1, &vp);
  }
  if (cmd->has_sc && cmd->sc_w > 0u && cmd->sc_h > 0u) {
    D3D11_RECT sc{cmd->sc_x, cmd->sc_y,
                  cmd->sc_x + (LONG)cmd->sc_w, cmd->sc_y + (LONG)cmd->sc_h};
    g_vkrt.ctx->RSSetScissorRects(1, &sc);
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
    g_vkrt.ctx->IASetVertexBuffers(0, cnt, vbs, strides, offs);
  }

  if (cmd->indexed) {
    ID3D11Buffer* ib = nullptr;
    auto bit = g_vkrt.bufs.find(cmd->ib_id);
    if (bit != g_vkrt.bufs.end()) ib = bit->second.buf;
    if (!ib) return;
    g_vkrt.ctx->IASetIndexBuffer(
        ib, cmd->idx_is_u32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
        (UINT)cmd->ib_off);
  }

  if (cmd->push_data && cmd->push_bytes && cmd->push_reg < 14u) {
    if (ensure_dyn_cb(g_vkrt.push_cb, g_vkrt.push_cb_cap, cmd->push_bytes,
                      cmd->push_data, cmd->push_bytes)) {
      g_vkrt.ctx->VSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
      g_vkrt.ctx->PSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
    }
  }

  bind_descriptor_table(cmd->bindings, cmd->binding_count, false, nullptr);

  if (cmd->indexed) {
    g_vkrt.ctx->DrawIndexedInstanced(
        cmd->vtx_count, std::max<u32>(1u, cmd->inst_count),
        cmd->first_vtx_or_idx, cmd->vtx_off, cmd->first_inst);
  } else {
    g_vkrt.ctx->DrawInstanced(
        cmd->vtx_count, std::max<u32>(1u, cmd->inst_count),
        cmd->first_vtx_or_idx, cmd->first_inst);
  }
}

inline void hk_dispatch(const trt_vk_dispatch_cmd* cmd, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !cmd) return;
  auto pit = g_vkrt.pipes.find(cmd->pipeline_id);
  if (pit == g_vkrt.pipes.end() || !pit->second.cs) return;

  g_vkrt.ctx->CSSetShader(pit->second.cs, nullptr, 0);
  if (cmd->push_data && cmd->push_bytes && cmd->push_reg < 14u) {
    if (ensure_dyn_cb(g_vkrt.push_cb, g_vkrt.push_cb_cap, cmd->push_bytes,
                      cmd->push_data, cmd->push_bytes)) {
      g_vkrt.ctx->CSSetConstantBuffers(cmd->push_reg, 1, &g_vkrt.push_cb);
    }
  }

  std::vector<u32> uav_slots;
  bind_descriptor_table(cmd->bindings, cmd->binding_count, true, &uav_slots);

  g_vkrt.ctx->Dispatch(std::max<u32>(1u, cmd->gx), std::max<u32>(1u, cmd->gy),
                       std::max<u32>(1u, cmd->gz));

  ID3D11UnorderedAccessView* null_uav = nullptr;
  for (u32 slot : uav_slots) {
    g_vkrt.ctx->CSSetUnorderedAccessViews(slot, 1, &null_uav, nullptr);
  }
  ID3D11ShaderResourceView* null_srvs[16] = {};
  g_vkrt.ctx->CSSetShaderResources(0, 16, null_srvs);
}

inline void hk_copy_buffer(u64 src_buf, u64 dst_buf, u64 src_off, u64 dst_off,
                           u64 size, void*) {
  if (!g_vkrt.initialized || !g_vkrt.ctx || !size) return;
  auto sit = g_vkrt.bufs.find(src_buf);
  auto dit = g_vkrt.bufs.find(dst_buf);
  if (sit == g_vkrt.bufs.end() || dit == g_vkrt.bufs.end()) return;
  if (sit->second.buf && dit->second.buf) {
    D3D11_BOX box{(UINT)src_off, 0, 0, (UINT)(src_off + size), 1, 1};
    g_vkrt.ctx->CopySubresourceRegion(dit->second.buf, 0, (UINT)dst_off, 0, 0,
                                      sit->second.buf, 0, &box);
  }
  if (src_off < sit->second.shadow.size()) {
    u64 cp = std::min<u64>(size, (u64)(sit->second.shadow.size() - src_off));
    if (dit->second.shadow.size() < dst_off + cp) {
      dit->second.shadow.resize((size_t)(dst_off + cp), 0);
    }
    memcpy(dit->second.shadow.data() + dst_off,
           sit->second.shadow.data() + src_off, (size_t)cp);
    if (dit->second.cb && dst_off == 0) {
      g_vkrt.ctx->UpdateSubresource(dit->second.cb, 0, nullptr,
                                    dit->second.shadow.data(), 0, 0);
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
  UINT rp = 0, sp = 0;
  calc_img_pitch(im.desc.vk_format, w ? w : im.desc.w, h ? h : im.desc.h, &rp, &sp);
  const UINT sub = D3D11CalcSubresource(mip, layer, mips);
  ID3D11Resource* res = im.tex3d ? (ID3D11Resource*)im.tex3d : (ID3D11Resource*)im.tex2d;
  g_vkrt.ctx->UpdateSubresource(res, sub, nullptr, src_ptr, rp, sp);
}

inline void hk_present(u64, u32, void*) {
  if (g_vkrt.swap) {
    longan::draw_pointer(false);
    g_vkrt.swap->Present(1, 0);
  }
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
    if (SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &g_vkrt.ds_tex))) {
      dev->CreateDepthStencilView(g_vkrt.ds_tex, nullptr, &g_vkrt.main_dsv);
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
  trt_vk_set_hooks(&hk);
}

}  // namespace vk_rt
