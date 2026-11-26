// tiramisu/src/host/gl_backend.hpp
// UWP Direct3D 11 execution backend for OpenGL (bridge_opengl32.h).
// Header-only matching src/host/d3d9_backend.hpp; bound in src/host/swapchain.cpp.
#pragma once

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "../trt/bridge_opengl32.h"
#include "../trt/bridge_esram.h"
#include "shader_cache_util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace gl_rt {

struct prog_rec {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11ComputeShader* cs = nullptr;
  ID3D11Buffer* vs_cb = nullptr;
  ID3D11Buffer* ps_cb = nullptr;
  ID3D11Buffer* cs_cb = nullptr;
  uint32_t vs_cb_cap = 0;
  uint32_t ps_cb_cap = 0;
  uint32_t cs_cb_cap = 0;
  std::vector<uint8_t> vs_blob;
};

struct buf_rec {
  ID3D11Buffer* buf = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  uint32_t bytes = 0;
  uint32_t kind = 0;
};

struct tex_rec {
  trt_gl_tex_desc desc{};
  ID3D11Texture2D* tex2d = nullptr;
  ID3D11Texture3D* tex3d = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
};

struct fbo_rec {
  uint32_t color_tex[8] = {};
  uint32_t color_count = 0;
  uint32_t depth_tex = 0;
  uint32_t w = 0;
  uint32_t h = 0;
};

struct gl_state {
  bool initialized = false;
  ID3D11Device1* dev = nullptr;
  ID3D11DeviceContext1* ctx = nullptr;
  IDXGISwapChain1* swap = nullptr;
  ID3D11RenderTargetView* main_rtv = nullptr;
  ID3D11Texture2D* ds_tex = nullptr;
  ID3D11DepthStencilView* main_dsv = nullptr;
  ID3D11Buffer* dyn_vb = nullptr;
  ID3D11Buffer* dyn_ib = nullptr;
  uint32_t dyn_vb_cap = 0;
  uint32_t dyn_ib_cap = 0;
  UINT width = 1280;
  UINT height = 720;
  uint32_t active_fbo = 0;

  std::map<uint32_t, prog_rec> progs;
  std::map<uint32_t, buf_rec> bufs;
  std::map<uint32_t, tex_rec> texs;
  std::map<uint32_t, fbo_rec> fbos;
  std::map<uint64_t, ID3D11InputLayout*> ils;
  std::map<uint64_t, ID3D11SamplerState*> samps;
  std::map<uint64_t, ID3D11RasterizerState*> rss;
  std::map<uint64_t, ID3D11DepthStencilState*> dsss;
  std::map<uint64_t, ID3D11BlendState*> bss;
};

inline gl_state g_glrt;

inline uint64_t fnv64_mix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}

inline DXGI_FORMAT gl_to_dxgi_fmt(uint32_t f, bool typeless_depth = false) {
  switch (f) {
    case TRT_GL_TEX_BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TRT_GL_TEX_SRGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TRT_GL_TEX_RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TRT_GL_TEX_RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TRT_GL_TEX_R32F: return DXGI_FORMAT_R32_FLOAT;
    case TRT_GL_TEX_D24S8:
      return typeless_depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case TRT_GL_TEX_BC1: return DXGI_FORMAT_BC1_UNORM;
    case TRT_GL_TEX_BC2: return DXGI_FORMAT_BC2_UNORM;
    case TRT_GL_TEX_BC3: return DXGI_FORMAT_BC3_UNORM;
    case TRT_GL_TEX_R8: return DXGI_FORMAT_R8_UNORM;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

inline uint32_t dxgi_bpp(uint32_t f) {
  switch (f) {
    case TRT_GL_TEX_RGBA16F: return 8;
    case TRT_GL_TEX_RGBA32F: return 16;
    case TRT_GL_TEX_R8: return 1;
    default: return 4;
  }
}

inline DXGI_FORMAT attrib_to_dxgi(const trt_gl_attrib_binding& a) {
  const uint32_t c = std::max<uint32_t>(1u, std::min<uint32_t>(4u, a.components));
  if (a.gl_type == 0x1406u) { // GL_FLOAT
    static const DXGI_FORMAT f[4] = {
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    return f[c - 1];
  }
  if (a.gl_type == 0x1401u) { // GL_UNSIGNED_BYTE
    if (a.is_int) return (c == 4) ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8_UINT;
    if (c == 1) return DXGI_FORMAT_R8_UNORM;
    if (c == 2) return DXGI_FORMAT_R8G8_UNORM;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  if (a.gl_type == 0x1400u) { // GL_BYTE
    return (c <= 2) ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8B8A8_SNORM;
  }
  if (a.gl_type == 0x1402u) { // GL_SHORT
    return (c <= 2) ? (a.is_int ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16_SNORM)
                    : (a.is_int ? DXGI_FORMAT_R16G16B16A16_SINT : DXGI_FORMAT_R16G16B16A16_SNORM);
  }
  if (a.gl_type == 0x1403u) { // GL_UNSIGNED_SHORT
    return (c <= 2) ? (a.is_int ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R16G16_UNORM)
                    : (a.is_int ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R16G16B16A16_UNORM);
  }
  if (a.gl_type == 0x1404u) { // GL_INT
    static const DXGI_FORMAT f[4] = {
        DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT,
        DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT};
    return f[c - 1];
  }
  if (a.gl_type == 0x1405u) { // GL_UNSIGNED_INT
    static const DXGI_FORMAT f[4] = {
        DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
    return f[c - 1];
  }
  return DXGI_FORMAT_R32G32B32A32_FLOAT;
}

inline D3D11_COMPARISON_FUNC gl_to_cmp(uint32_t f) {
  switch (f) {
    case 0x0200u: return D3D11_COMPARISON_NEVER;
    case 0x0201u: return D3D11_COMPARISON_LESS;
    case 0x0202u: return D3D11_COMPARISON_EQUAL;
    case 0x0203u: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x0204u: return D3D11_COMPARISON_GREATER;
    case 0x0205u: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x0206u: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
  }
}

inline D3D11_BLEND gl_to_blend(uint32_t b, bool alpha) {
  switch (b) {
    case 0u: return D3D11_BLEND_ZERO;
    case 1u: return D3D11_BLEND_ONE;
    case 0x0300u: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301u: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302u: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303u: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304u: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305u: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306u: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307u: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x8001u: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002u: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
  }
}

inline D3D11_BLEND_OP gl_to_blend_op(uint32_t eq) {
  switch (eq) {
    case 0x800Au: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800Bu: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007u: return D3D11_BLEND_OP_MIN;
    case 0x8008u: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
  }
}

inline D3D11_STENCIL_OP gl_to_stencil_op(uint32_t op) {
  switch (op) {
    case 0u: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01u: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02u: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03u: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150Au: return D3D11_STENCIL_OP_INVERT;
    case 0x8507u: return D3D11_STENCIL_OP_INCR;
    case 0x8508u: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
  }
}

inline void ensure_dyn_buf(ID3D11Buffer*& buf, uint32_t& cap, uint32_t need, UINT bind) {
  if (!g_glrt.dev) return;
  if (buf && cap >= need) return;
  if (buf) { buf->Release(); buf = nullptr; }
  cap = std::max(need, 65536u);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = (bind & D3D11_BIND_CONSTANT_BUFFER) ? ((cap + 15u) & ~15u) : cap;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = bind;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_glrt.dev->CreateBuffer(&bd, nullptr, &buf);
}

inline void bind_active_rt(uint32_t fbo, UINT& out_w, UINT& out_h) {
  g_glrt.active_fbo = fbo;
  if (fbo == 0 || !g_glrt.fbos.count(fbo)) {
    out_w = g_glrt.width ? g_glrt.width : 1280u;
    out_h = g_glrt.height ? g_glrt.height : 720u;
    g_glrt.ctx->OMSetRenderTargets(1, &g_glrt.main_rtv, g_glrt.main_dsv);
    return;
  }
  const auto& f = g_glrt.fbos[fbo];
  ID3D11RenderTargetView* rtvs[8] = {};
  uint32_t rc = std::min<uint32_t>(8u, f.color_count);
  out_w = g_glrt.width;
  out_h = g_glrt.height;
  for (uint32_t i = 0; i < rc; ++i) {
    auto it = g_glrt.texs.find(f.color_tex[i]);
    if (it != g_glrt.texs.end() && it->second.rtv) {
      rtvs[i] = it->second.rtv;
      if (it->second.desc.w) out_w = it->second.desc.w;
      if (it->second.desc.h) out_h = it->second.desc.h;
    }
  }
  ID3D11DepthStencilView* dsv = nullptr;
  if (f.depth_tex) {
    auto it = g_glrt.texs.find(f.depth_tex);
    if (it != g_glrt.texs.end() && it->second.dsv) {
      dsv = it->second.dsv;
      if (rc == 0 && it->second.desc.w) { out_w = it->second.desc.w; out_h = it->second.desc.h; }
    }
  }
  g_glrt.ctx->OMSetRenderTargets(rc, rtvs, dsv);
}

inline void hk_clear(u32 mask, const float rgba[4], float depth, int stencil, void*) {
  if (!g_glrt.initialized || !g_glrt.ctx) return;
  UINT rw = g_glrt.width, rh = g_glrt.height;
  bind_active_rt(g_glrt.active_fbo, rw, rh);
  (void)rw; (void)rh;
  ID3D11RenderTargetView* rtvs[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  g_glrt.ctx->OMGetRenderTargets(8, rtvs, &dsv);
  if (mask & 1u) {
    for (int i = 0; i < 8; ++i) if (rtvs[i]) g_glrt.ctx->ClearRenderTargetView(rtvs[i], rgba);
  }
  if ((mask & 6u) && dsv) {
    UINT fl = ((mask & 2u) ? D3D11_CLEAR_DEPTH : 0u) | ((mask & 4u) ? D3D11_CLEAR_STENCIL : 0u);
    g_glrt.ctx->ClearDepthStencilView(dsv, fl, depth, (UINT8)stencil);
  }
  for (int i = 0; i < 8; ++i) if (rtvs[i]) rtvs[i]->Release();
  if (dsv) dsv->Release();
}

inline void hk_swap_buffers(u64, void*) {
  if (!g_glrt.initialized || !g_glrt.swap) return;
  longan::draw_guest_pointer();
  g_glrt.swap->Present(1, 0);
}

inline u32 hk_compile_program(u32 gl_prog, const char* vs_hlsl, u32 vs_len,
                              const char* ps_hlsl, u32 ps_len, const char* cs_hlsl,
                              u32 cs_len, void*) {
  if (!g_glrt.initialized || !g_glrt.dev) return 0;
  auto& pr = g_glrt.progs[gl_prog];
  if (pr.vs) { pr.vs->Release(); pr.vs = nullptr; }
  if (pr.ps) { pr.ps->Release(); pr.ps = nullptr; }
  if (pr.cs) { pr.cs->Release(); pr.cs = nullptr; }
  pr.vs_blob.clear();
  for (auto it = g_glrt.ils.begin(); it != g_glrt.ils.end();) {
    if ((uint32_t)(it->first >> 32) == gl_prog) {
      if (it->second) it->second->Release();
      it = g_glrt.ils.erase(it);
    } else ++it;
  }

  auto compile_stage = [](const char* src, u32 len, const char* tgt, ID3DBlob** out) -> bool {
    if (!src || !len) return true;
    return longan::d3d_compile_cached(src, len, tgt, "main", out);
  };
  if (vs_hlsl && vs_len) {
    ID3DBlob* blob = nullptr;
    if (!compile_stage(vs_hlsl, vs_len, "vs_5_0", &blob)) return 0;
    pr.vs_blob.assign((const uint8_t*)blob->GetBufferPointer(),
                      (const uint8_t*)blob->GetBufferPointer() + blob->GetBufferSize());
    HRESULT hr = g_glrt.dev->CreateVertexShader(blob->GetBufferPointer(),
                                                blob->GetBufferSize(), nullptr, &pr.vs);
    blob->Release();
    if (FAILED(hr)) return 0;
  }
  if (ps_hlsl && ps_len) {
    ID3DBlob* blob = nullptr;
    if (!compile_stage(ps_hlsl, ps_len, "ps_5_0", &blob)) return 0;
    HRESULT hr = g_glrt.dev->CreatePixelShader(blob->GetBufferPointer(),
                                               blob->GetBufferSize(), nullptr, &pr.ps);
    blob->Release();
    if (FAILED(hr)) return 0;
  }
  if (cs_hlsl && cs_len) {
    ID3DBlob* blob = nullptr;
    if (!compile_stage(cs_hlsl, cs_len, "cs_5_0", &blob)) return 0;
    HRESULT hr = g_glrt.dev->CreateComputeShader(blob->GetBufferPointer(),
                                                 blob->GetBufferSize(), nullptr, &pr.cs);
    blob->Release();
    if (FAILED(hr)) return 0;
  }
  return 1;
}

inline void hk_delete_program(u32 gl_prog, void*) {
  auto it = g_glrt.progs.find(gl_prog);
  if (it == g_glrt.progs.end()) return;
  auto& pr = it->second;
  if (pr.vs) pr.vs->Release();
  if (pr.ps) pr.ps->Release();
  if (pr.cs) pr.cs->Release();
  if (pr.vs_cb) pr.vs_cb->Release();
  if (pr.ps_cb) pr.ps_cb->Release();
  if (pr.cs_cb) pr.cs_cb->Release();
  g_glrt.progs.erase(it);
}

inline u32 hk_sync_buffer(u32 gl_buf, u32 kind, const void* data, u32 bytes,
                          u32 version, void*) {
  (void)version;
  if (!g_glrt.initialized || !g_glrt.dev) return 0;
  auto& br = g_glrt.bufs[gl_buf];
  const uint32_t need = (kind == TRT_GL_BUF_UNIFORM) ? ((std::max(bytes, 16u) + 15u) & ~15u)
                                                     : std::max(bytes, 16u);
  if (!br.buf || br.bytes < need || br.kind != kind) {
    if (br.srv) { br.srv->Release(); br.srv = nullptr; }
    if (br.uav) { br.uav->Release(); br.uav = nullptr; }
    if (br.buf) { br.buf->Release(); br.buf = nullptr; }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = need;
    if (kind == TRT_GL_BUF_STORAGE) {
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    } else {
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (kind == TRT_GL_BUF_INDEX) bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
      else if (kind == TRT_GL_BUF_UNIFORM) bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      else bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    }
    D3D11_SUBRESOURCE_DATA init{};
    if (data && bytes >= need) init.pSysMem = data;
    if (FAILED(g_glrt.dev->CreateBuffer(&bd, init.pSysMem ? &init : nullptr, &br.buf))) {
      return 0;
    }
    br.bytes = need;
    br.kind = kind;
  }
  if (data && bytes > 0) {
    if (kind == TRT_GL_BUF_STORAGE) {
      D3D11_BOX box{0, 0, 0, std::min(bytes, br.bytes), 1, 1};
      g_glrt.ctx->UpdateSubresource(br.buf, 0, &box, data, 0, 0);
    } else {
      D3D11_MAPPED_SUBRESOURCE mr{};
      if (SUCCEEDED(g_glrt.ctx->Map(br.buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
        memcpy(mr.pData, data, std::min(bytes, br.bytes));
        g_glrt.ctx->Unmap(br.buf, 0);
      }
    }
  }
  return 1;
}

inline void hk_delete_buffer(u32 gl_buf, void*) {
  auto it = g_glrt.bufs.find(gl_buf);
  if (it == g_glrt.bufs.end()) return;
  if (it->second.srv) it->second.srv->Release();
  if (it->second.uav) it->second.uav->Release();
  if (it->second.buf) it->second.buf->Release();
  g_glrt.bufs.erase(it);
}

inline void free_tex_rec(tex_rec& tr) {
  if (trt_esram_is_active() && (tr.rtv || tr.dsv)) {
    trt_esram_release_surface(tr.desc.w, tr.desc.h, (uint32_t)gl_to_dxgi_fmt(tr.desc.fmt, tr.dsv != nullptr));
  }
  if (tr.srv) { tr.srv->Release(); tr.srv = nullptr; }
  if (tr.rtv) { tr.rtv->Release(); tr.rtv = nullptr; }
  if (tr.dsv) { tr.dsv->Release(); tr.dsv = nullptr; }
  if (tr.uav) { tr.uav->Release(); tr.uav = nullptr; }
  if (tr.tex2d) { tr.tex2d->Release(); tr.tex2d = nullptr; }
  if (tr.tex3d) { tr.tex3d->Release(); tr.tex3d = nullptr; }
}

inline u32 hk_sync_texture(u32 gl_tex, const trt_gl_tex_desc* desc, const void* data,
                           u32 bytes, u32 mip, u32 layer, u32, void*) {
  if (!g_glrt.initialized || !g_glrt.dev || !desc || !desc->w || !desc->h) return 0;
  auto& tr = g_glrt.texs[gl_tex];
  const bool need_recreate = (!tr.tex2d && !tr.tex3d) ||
      tr.desc.w != desc->w || tr.desc.h != desc->h ||
      tr.desc.fmt != desc->fmt || tr.desc.target != desc->target;
  if (need_recreate) {
    free_tex_rec(tr);
    tr.desc = *desc;
    const bool is_ds = (desc->fmt == TRT_GL_TEX_D24S8);
    const bool is_bc = (desc->fmt == TRT_GL_TEX_BC1 || desc->fmt == TRT_GL_TEX_BC2 ||
                        desc->fmt == TRT_GL_TEX_BC3);
    DXGI_FORMAT fmt = gl_to_dxgi_fmt(desc->fmt, is_ds);
    if (desc->target == TRT_GL_TARGET_3D) {
      D3D11_TEXTURE3D_DESC td{};
      td.Width = desc->w; td.Height = desc->h; td.Depth = std::max(1u, desc->depth);
      td.MipLevels = 1; td.Format = fmt;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (SUCCEEDED(g_glrt.dev->CreateTexture3D(&td, nullptr, &tr.tex3d))) {
        g_glrt.dev->CreateShaderResourceView(tr.tex3d, nullptr, &tr.srv);
      }
    } else {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = desc->w; td.Height = desc->h;
      td.MipLevels = std::max(1u, desc->mip_count);
      td.ArraySize = (desc->target == TRT_GL_TARGET_CUBE) ? 6u : 1u;
      td.Format = fmt;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (is_ds) td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
      else if (!is_bc) {
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (desc->fmt == TRT_GL_TEX_RGBA8 && td.MipLevels > 1) {
          td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        if (desc->fmt == TRT_GL_TEX_RGBA8 || desc->fmt == TRT_GL_TEX_RGBA16F ||
            desc->fmt == TRT_GL_TEX_RGBA32F || desc->fmt == TRT_GL_TEX_R32F) {
          if (desc->target != TRT_GL_TARGET_CUBE && td.MipLevels == 1) {
            td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
          }
        }
      }
      if (desc->target == TRT_GL_TARGET_CUBE) td.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
      if (FAILED(g_glrt.dev->CreateTexture2D(&td, nullptr, &tr.tex2d))) return 0;

      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = is_ds ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : fmt;
      sd.ViewDimension = (desc->target == TRT_GL_TARGET_CUBE)
                             ? D3D11_SRV_DIMENSION_TEXTURECUBE
                             : D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = td.MipLevels;
      g_glrt.dev->CreateShaderResourceView(tr.tex2d, &sd, &tr.srv);
      if (is_ds) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g_glrt.dev->CreateDepthStencilView(tr.tex2d, &dd, &tr.dsv);
      } else if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
        g_glrt.dev->CreateRenderTargetView(tr.tex2d, nullptr, &tr.rtv);
      }
      if (trt_esram_is_active() && (td.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        trt_esram_qualify_and_alloc(td.Width, td.Height, (uint32_t)td.Format, td.BindFlags, "gl_surface");
      }
      if (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
        g_glrt.dev->CreateUnorderedAccessView(tr.tex2d, nullptr, &tr.uav);
      }
    }
  }
  if (data && bytes > 0 && tr.tex2d) {
    UINT mw = std::max(1u, desc->w >> mip);
    UINT pitch = mw * dxgi_bpp(desc->fmt);
    if (desc->fmt == TRT_GL_TEX_BC1) pitch = std::max(1u, (mw + 3u) / 4u) * 8u;
    else if (desc->fmt == TRT_GL_TEX_BC2 || desc->fmt == TRT_GL_TEX_BC3) {
      pitch = std::max(1u, (mw + 3u) / 4u) * 16u;
    }
    const UINT sub = D3D11CalcSubresource(mip, layer, std::max(1u, tr.desc.mip_count));
    g_glrt.ctx->UpdateSubresource(tr.tex2d, sub, nullptr, data, pitch, 0);
  }
  return 1;
}

inline void hk_gen_mipmaps(u32 gl_tex, void*) {
  auto it = g_glrt.texs.find(gl_tex);
  if (it != g_glrt.texs.end() && it->second.srv) {
    g_glrt.ctx->GenerateMips(it->second.srv);
  }
}

inline void hk_delete_texture(u32 gl_tex, void*) {
  auto it = g_glrt.texs.find(gl_tex);
  if (it == g_glrt.texs.end()) return;
  free_tex_rec(it->second);
  g_glrt.texs.erase(it);
}

inline u32 hk_sync_fbo(u32 gl_fbo, const u32* color_tex, const u32*, const u32*,
                       u32 color_count, u32 depth_tex, u32, void*) {
  auto& f = g_glrt.fbos[gl_fbo];
  f.color_count = std::min<u32>(8u, color_count);
  for (uint32_t i = 0; i < f.color_count; ++i) f.color_tex[i] = color_tex ? color_tex[i] : 0u;
  f.depth_tex = depth_tex;
  return 1;
}

inline void hk_delete_fbo(u32 gl_fbo, void*) {
  if (g_glrt.active_fbo == gl_fbo) g_glrt.active_fbo = 0;
  g_glrt.fbos.erase(gl_fbo);
}

inline void hk_read_pixels(u32 fbo, int x, int y, int w, int h, void* out_rgba8, void*) {
  if (!g_glrt.initialized || !out_rgba8 || w <= 0 || h <= 0) return;
  memset(out_rgba8, 0, (size_t)w * (size_t)h * 4u);
  ID3D11Texture2D* src_tex = nullptr;
  bool release_src = false;
  if (fbo && g_glrt.fbos.count(fbo) && g_glrt.fbos[fbo].color_count > 0) {
    auto it = g_glrt.texs.find(g_glrt.fbos[fbo].color_tex[0]);
    if (it != g_glrt.texs.end()) src_tex = it->second.tex2d;
  } else if (g_glrt.swap) {
    g_glrt.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&src_tex);
    release_src = (src_tex != nullptr);
  }
  if (!src_tex) return;
  D3D11_TEXTURE2D_DESC td{};
  src_tex->GetDesc(&td);
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
  td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ID3D11Texture2D* stg = nullptr;
  if (SUCCEEDED(g_glrt.dev->CreateTexture2D(&td, nullptr, &stg))) {
    int sy = std::max(0, (int)g_glrt.height - (y + h));
    D3D11_BOX box{(UINT)std::max(0, x), (UINT)sy, 0,
                  (UINT)std::max(1, x + w), (UINT)std::max(1, sy + h), 1};
    g_glrt.ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, src_tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mr{};
    if (SUCCEEDED(g_glrt.ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mr))) {
      for (int row = 0; row < h; ++row) {
        // Flip back to OpenGL bottom-to-top row order
        memcpy((uint8_t*)out_rgba8 + (size_t)(h - 1 - row) * (size_t)w * 4u,
               (const uint8_t*)mr.pData + (size_t)row * mr.RowPitch, (size_t)w * 4u);
      }
      g_glrt.ctx->Unmap(stg, 0);
    }
    stg->Release();
  }
  if (release_src) src_tex->Release();
}

inline ID3D11SamplerState* get_sampler(const trt_gl_sampler_slot& s) {
  uint64_t key = fnv64_mix(fnv64_mix(fnv64_mix(s.min_filter, s.mag_filter),
                                     s.wrap_s),
                           ((uint64_t)s.wrap_t << 32) | s.compare_mode);
  auto it = g_glrt.samps.find(key);
  if (it != g_glrt.samps.end()) return it->second;
  D3D11_SAMPLER_DESC sd{};
  const bool lin = (s.mag_filter == 0x2601u || s.min_filter == 0x2601u ||
                    s.min_filter == 0x2703u);
  if (s.compare_mode) sd.Filter = lin ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                                      : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  else sd.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
  auto wrap = [](uint32_t w) {
    if (w == 0x812Fu) return D3D11_TEXTURE_ADDRESS_CLAMP;
    if (w == 0x8370u) return D3D11_TEXTURE_ADDRESS_MIRROR;
    return D3D11_TEXTURE_ADDRESS_WRAP;
  };
  sd.AddressU = wrap(s.wrap_s); sd.AddressV = wrap(s.wrap_t); sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState* ss = nullptr;
  g_glrt.dev->CreateSamplerState(&sd, &ss);
  g_glrt.samps[key] = ss;
  return ss;
}

inline void apply_render_states(const trt_gl_render_state& rs, UINT rw, UINT rh) {
  D3D11_VIEWPORT vp{};
  vp.TopLeftX = (float)rs.vp[0];
  vp.TopLeftY = (float)rh - (float)(rs.vp[1] + rs.vp[3]);
  vp.Width = (float)std::max(1, rs.vp[2]);
  vp.Height = (float)std::max(1, rs.vp[3]);
  vp.MinDepth = rs.depth_min;
  vp.MaxDepth = rs.depth_max;
  g_glrt.ctx->RSSetViewports(1, &vp);

  uint64_t rkey = ((uint64_t)rs.wireframe << 0) | ((uint64_t)rs.cull_en << 1) |
                  ((uint64_t)rs.front_ccw << 2) | ((uint64_t)rs.scissor_en << 3) |
                  ((uint64_t)rs.cull_face << 16);
  auto rit = g_glrt.rss.find(rkey);
  if (rit == g_glrt.rss.end()) {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = rs.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    if (!rs.cull_en) rd.CullMode = D3D11_CULL_NONE;
    else if (rs.cull_face == 0x0404u) rd.CullMode = D3D11_CULL_FRONT;
    else rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = rs.front_ccw ? TRUE : FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = rs.scissor_en ? TRUE : FALSE;
    ID3D11RasterizerState* r = nullptr;
    g_glrt.dev->CreateRasterizerState(&rd, &r);
    g_glrt.rss[rkey] = r;
    rit = g_glrt.rss.find(rkey);
  }
  g_glrt.ctx->RSSetState(rit->second);
  if (rs.scissor_en) {
    LONG sy = (LONG)rh - (LONG)(rs.sc[1] + rs.sc[3]);
    D3D11_RECT rc{std::max<LONG>(0, rs.sc[0]), std::max<LONG>(0, sy),
                  std::max<LONG>(1, rs.sc[0] + rs.sc[2]),
                  std::max<LONG>(1, sy + rs.sc[3])};
    g_glrt.ctx->RSSetScissorRects(1, &rc);
  }

  uint64_t dkey = ((uint64_t)rs.depth_test << 0) | ((uint64_t)rs.depth_write << 1) |
                  ((uint64_t)rs.stencil_en << 2) | ((uint64_t)rs.depth_func << 16);
  dkey = fnv64_mix(dkey, ((uint64_t)rs.stencil_func << 32) | rs.stencil_sfail);
  auto dit = g_glrt.dsss.find(dkey);
  if (dit == g_glrt.dsss.end()) {
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = rs.depth_test ? TRUE : FALSE;
    dd.DepthWriteMask = rs.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = gl_to_cmp(rs.depth_func);
    dd.StencilEnable = rs.stencil_en ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)rs.stencil_mask;
    dd.StencilWriteMask = (UINT8)rs.stencil_wmask;
    dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = gl_to_cmp(rs.stencil_func);
    dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = gl_to_stencil_op(rs.stencil_sfail);
    dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp = gl_to_stencil_op(rs.stencil_dpfail);
    dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = gl_to_stencil_op(rs.stencil_dppass);
    ID3D11DepthStencilState* ds = nullptr;
    g_glrt.dev->CreateDepthStencilState(&dd, &ds);
    g_glrt.dsss[dkey] = ds;
    dit = g_glrt.dsss.find(dkey);
  }
  g_glrt.ctx->OMSetDepthStencilState(dit->second, (UINT)rs.stencil_ref);

  uint64_t bkey = fnv64_mix(rs.blend_en | ((uint64_t)rs.color_mask << 8),
                            ((uint64_t)rs.blend_src_rgb << 32) | rs.blend_dst_rgb);
  bkey = fnv64_mix(bkey, ((uint64_t)rs.blend_src_a << 32) | rs.blend_dst_a);
  bkey = fnv64_mix(bkey, ((uint64_t)rs.blend_eq_rgb << 32) | rs.blend_eq_a);
  auto bit = g_glrt.bss.find(bkey);
  if (bit == g_glrt.bss.end()) {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = rs.blend_en ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = gl_to_blend(rs.blend_src_rgb, false);
    bd.RenderTarget[0].DestBlend = gl_to_blend(rs.blend_dst_rgb, false);
    bd.RenderTarget[0].BlendOp = gl_to_blend_op(rs.blend_eq_rgb);
    bd.RenderTarget[0].SrcBlendAlpha = gl_to_blend(rs.blend_src_a, true);
    bd.RenderTarget[0].DestBlendAlpha = gl_to_blend(rs.blend_dst_a, true);
    bd.RenderTarget[0].BlendOpAlpha = gl_to_blend_op(rs.blend_eq_a);
    bd.RenderTarget[0].RenderTargetWriteMask = rs.color_mask & 0x0Fu;
    ID3D11BlendState* bs = nullptr;
    g_glrt.dev->CreateBlendState(&bd, &bs);
    g_glrt.bss[bkey] = bs;
    bit = g_glrt.bss.find(bkey);
  }
  g_glrt.ctx->OMSetBlendState(bit->second, rs.blend_const, 0xFFFFFFFFu);
}

inline void hk_draw(const trt_gl_draw_cmd* cmd, void*) {
  if (!g_glrt.initialized || !g_glrt.ctx || !cmd) return;
  auto pit = g_glrt.progs.find(cmd->gl_prog);
  if (pit == g_glrt.progs.end() || !pit->second.vs || !pit->second.ps) return;
  prog_rec& pr = pit->second;

  UINT rw = g_glrt.width, rh = g_glrt.height;
  bind_active_rt(cmd->active_fbo, rw, rh);
  apply_render_states(cmd->rs, rw, rh);

  g_glrt.ctx->VSSetShader(pr.vs, nullptr, 0);
  g_glrt.ctx->PSSetShader(pr.ps, nullptr, 0);

  auto upload_cb = [](ID3D11Buffer*& cb, uint32_t& cap, const void* src, uint32_t bytes) {
    if (!src || !bytes) return;
    uint32_t need = (bytes + 15u) & ~15u;
    ensure_dyn_buf(cb, cap, need, D3D11_BIND_CONSTANT_BUFFER);
    if (!cb) return;
    D3D11_MAPPED_SUBRESOURCE mr{};
    if (SUCCEEDED(g_glrt.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
      memcpy(mr.pData, src, bytes);
      g_glrt.ctx->Unmap(cb, 0);
    }
  };
  upload_cb(pr.vs_cb, pr.vs_cb_cap, cmd->vs_cb0, cmd->vs_cb0_bytes);
  upload_cb(pr.ps_cb, pr.ps_cb_cap, cmd->ps_cb0, cmd->ps_cb0_bytes);
  if (pr.vs_cb) g_glrt.ctx->VSSetConstantBuffers(0, 1, &pr.vs_cb);
  if (pr.ps_cb) g_glrt.ctx->PSSetConstantBuffers(0, 1, &pr.ps_cb);

  for (u32 u = 0; u < cmd->ubo_count && u < 14; ++u) {
    if (!cmd->ubos[u].gl_buf) continue;
    auto bit = g_glrt.bufs.find(cmd->ubos[u].gl_buf);
    if (bit != g_glrt.bufs.end() && bit->second.buf &&
        bit->second.kind == TRT_GL_BUF_UNIFORM) {
      g_glrt.ctx->VSSetConstantBuffers(u + 1, 1, &bit->second.buf);
      g_glrt.ctx->PSSetConstantBuffers(u + 1, 1, &bit->second.buf);
    }
  }

  auto bind_texs = [](bool vs_stage, const trt_gl_sampler_slot* sl, u32 cnt) {
    if (!cnt) return;
    ID3D11ShaderResourceView* srvs[16] = {};
    ID3D11SamplerState* smps[16] = {};
    for (u32 i = 0; i < cnt && i < 16; ++i) {
      if (sl[i].gl_tex && g_glrt.texs.count(sl[i].gl_tex)) {
        srvs[i] = g_glrt.texs[sl[i].gl_tex].srv;
      }
      smps[i] = get_sampler(sl[i]);
    }
    if (vs_stage) {
      g_glrt.ctx->VSSetShaderResources(0, cnt, srvs);
      g_glrt.ctx->VSSetSamplers(0, cnt, smps);
    } else {
      g_glrt.ctx->PSSetShaderResources(0, cnt, srvs);
      g_glrt.ctx->PSSetSamplers(0, cnt, smps);
    }
  };
  bind_texs(true, cmd->vs_tex, cmd->vs_tex_count);
  bind_texs(false, cmd->ps_tex, cmd->ps_tex_count);

  if (cmd->inline_vb && cmd->inline_vb_bytes) {
    ensure_dyn_buf(g_glrt.dyn_vb, g_glrt.dyn_vb_cap, cmd->inline_vb_bytes,
                   D3D11_BIND_VERTEX_BUFFER);
    if (g_glrt.dyn_vb) {
      D3D11_MAPPED_SUBRESOURCE mr{};
      if (SUCCEEDED(g_glrt.ctx->Map(g_glrt.dyn_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
        memcpy(mr.pData, cmd->inline_vb, cmd->inline_vb_bytes);
        g_glrt.ctx->Unmap(g_glrt.dyn_vb, 0);
      }
    }
  }

  uint64_t il_key = ((uint64_t)cmd->gl_prog << 32);
  D3D11_INPUT_ELEMENT_DESC descs[16] = {};
  ID3D11Buffer* vbs[16] = {};
  UINT strides[16] = {};
  UINT offsets[16] = {};
  UINT nslots = 0;
  if (cmd->attribs && cmd->attrib_count) {
    nslots = std::min<UINT>(16u, cmd->attrib_count);
    for (UINT i = 0; i < nslots; ++i) {
      const auto& a = cmd->attribs[i];
      descs[i].SemanticName = "TEXCOORD";
      descs[i].SemanticIndex = a.location;
      descs[i].Format = attrib_to_dxgi(a);
      descs[i].InputSlot = i;
      descs[i].AlignedByteOffset = 0;
      if (a.divisor == 0xFEu) {
        descs[i].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        descs[i].InstanceDataStepRate = 0;
      } else if (a.divisor > 0) {
        descs[i].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        descs[i].InstanceDataStepRate = a.divisor;
      } else {
        descs[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        descs[i].InstanceDataStepRate = 0;
      }
      if (a.vbo_gl == 0xFFFFFFFFu) vbs[i] = g_glrt.dyn_vb;
      else if (g_glrt.bufs.count(a.vbo_gl)) vbs[i] = g_glrt.bufs[a.vbo_gl].buf;
      strides[i] = a.stride;
      offsets[i] = a.offset;
      il_key = fnv64_mix(il_key, ((uint64_t)a.location << 40) |
                                     ((uint64_t)descs[i].Format << 16) |
                                     ((uint64_t)a.divisor << 8) | i);
    }
  }

  ID3D11InputLayout* il = nullptr;
  auto il_it = g_glrt.ils.find(il_key);
  if (il_it != g_glrt.ils.end()) il = il_it->second;
  else if (nslots > 0 && !pr.vs_blob.empty()) {
    g_glrt.dev->CreateInputLayout(descs, nslots, pr.vs_blob.data(),
                                  pr.vs_blob.size(), &il);
    g_glrt.ils[il_key] = il;
  }
  g_glrt.ctx->IASetInputLayout(il);
  if (nslots > 0) g_glrt.ctx->IASetVertexBuffers(0, nslots, vbs, strides, offsets);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (cmd->prim_mode == TRT_GL_PRIM_POINTS) topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  else if (cmd->prim_mode == TRT_GL_PRIM_LINES) topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
  else if (cmd->prim_mode == TRT_GL_PRIM_LINE_STRIP) topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
  else if (cmd->prim_mode == TRT_GL_PRIM_TRIANGLE_STRIP) topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  g_glrt.ctx->IASetPrimitiveTopology(topo);

  if (cmd->indexed) {
    DXGI_FORMAT ifmt = (cmd->idx_type == 0x1405u) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    ID3D11Buffer* ib = nullptr;
    UINT ioff = 0;
    if (cmd->use_inline_idx && cmd->inline_idx && cmd->inline_idx_bytes) {
      ensure_dyn_buf(g_glrt.dyn_ib, g_glrt.dyn_ib_cap, cmd->inline_idx_bytes,
                     D3D11_BIND_INDEX_BUFFER);
      if (g_glrt.dyn_ib) {
        D3D11_MAPPED_SUBRESOURCE mr{};
        if (SUCCEEDED(g_glrt.ctx->Map(g_glrt.dyn_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
          memcpy(mr.pData, cmd->inline_idx, cmd->inline_idx_bytes);
          g_glrt.ctx->Unmap(g_glrt.dyn_ib, 0);
        }
        ib = g_glrt.dyn_ib;
      }
    } else if (cmd->ibo_gl && g_glrt.bufs.count(cmd->ibo_gl)) {
      ib = g_glrt.bufs[cmd->ibo_gl].buf;
      ioff = (UINT)cmd->ibo_offset;
    }
    if (!ib) return;
    g_glrt.ctx->IASetIndexBuffer(ib, ifmt, ioff);
    if (cmd->instance_count > 1) {
      g_glrt.ctx->DrawIndexedInstanced(cmd->count, cmd->instance_count, 0, cmd->base_vertex, 0);
    } else {
      g_glrt.ctx->DrawIndexed(cmd->count, 0, cmd->base_vertex);
    }
  } else {
    if (cmd->instance_count > 1) {
      g_glrt.ctx->DrawInstanced(cmd->count, cmd->instance_count, cmd->first, 0);
    } else {
      g_glrt.ctx->Draw(cmd->count, cmd->first);
    }
  }
}

inline void hk_dispatch_compute(u32 gl_prog, u32 gx, u32 gy, u32 gz,
                                const void* cs_cb0, u32 cs_cb0_bytes,
                                const trt_gl_sampler_slot* texs, u32 tex_count,
                                const trt_gl_ubo_slot* ubos, u32 ubo_count,
                                const u32* image_uavs, u32 image_count, void*) {
  if (!g_glrt.initialized || !g_glrt.ctx) return;
  auto pit = g_glrt.progs.find(gl_prog);
  if (pit == g_glrt.progs.end() || !pit->second.cs) return;
  prog_rec& pr = pit->second;
  g_glrt.ctx->CSSetShader(pr.cs, nullptr, 0);
  if (cs_cb0 && cs_cb0_bytes) {
    uint32_t need = (cs_cb0_bytes + 15u) & ~15u;
    ensure_dyn_buf(pr.cs_cb, pr.cs_cb_cap, need, D3D11_BIND_CONSTANT_BUFFER);
    if (pr.cs_cb) {
      D3D11_MAPPED_SUBRESOURCE mr{};
      if (SUCCEEDED(g_glrt.ctx->Map(pr.cs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
        memcpy(mr.pData, cs_cb0, cs_cb0_bytes);
        g_glrt.ctx->Unmap(pr.cs_cb, 0);
      }
      g_glrt.ctx->CSSetConstantBuffers(0, 1, &pr.cs_cb);
    }
  }
  for (u32 u = 0; u < ubo_count && u < 14; ++u) {
    if (ubos && ubos[u].gl_buf && g_glrt.bufs.count(ubos[u].gl_buf)) {
      ID3D11Buffer* b = g_glrt.bufs[ubos[u].gl_buf].buf;
      g_glrt.ctx->CSSetConstantBuffers(u + 1, 1, &b);
    }
  }
  if (texs && tex_count) {
    ID3D11ShaderResourceView* srvs[16] = {};
    for (u32 i = 0; i < tex_count && i < 16; ++i) {
      if (texs[i].gl_tex && g_glrt.texs.count(texs[i].gl_tex)) srvs[i] = g_glrt.texs[texs[i].gl_tex].srv;
    }
    g_glrt.ctx->CSSetShaderResources(0, tex_count, srvs);
  }
  if (image_uavs && image_count) {
    ID3D11UnorderedAccessView* uavs[8] = {};
    u32 cnt = std::min<u32>(8u, image_count);
    for (u32 i = 0; i < cnt; ++i) {
      if (image_uavs[i] && g_glrt.texs.count(image_uavs[i])) uavs[i] = g_glrt.texs[image_uavs[i]].uav;
    }
    g_glrt.ctx->CSSetUnorderedAccessViews(0, cnt, uavs, nullptr);
    g_glrt.ctx->Dispatch(gx, gy, gz);
    ID3D11UnorderedAccessView* nulls[8] = {};
    g_glrt.ctx->CSSetUnorderedAccessViews(0, cnt, nulls, nullptr);
  } else {
    g_glrt.ctx->Dispatch(gx, gy, gz);
  }
}

inline void shutdown() {
  trt_gl_set_hooks(nullptr);
  for (auto& kv : g_glrt.ils) if (kv.second) kv.second->Release();
  for (auto& kv : g_glrt.samps) if (kv.second) kv.second->Release();
  for (auto& kv : g_glrt.rss) if (kv.second) kv.second->Release();
  for (auto& kv : g_glrt.dsss) if (kv.second) kv.second->Release();
  for (auto& kv : g_glrt.bss) if (kv.second) kv.second->Release();
  for (auto& kv : g_glrt.texs) free_tex_rec(kv.second);
  for (auto& kv : g_glrt.bufs) {
    if (kv.second.srv) kv.second.srv->Release();
    if (kv.second.uav) kv.second.uav->Release();
    if (kv.second.buf) kv.second.buf->Release();
  }
  for (auto& kv : g_glrt.progs) {
    if (kv.second.vs) kv.second.vs->Release();
    if (kv.second.ps) kv.second.ps->Release();
    if (kv.second.cs) kv.second.cs->Release();
    if (kv.second.vs_cb) kv.second.vs_cb->Release();
    if (kv.second.ps_cb) kv.second.ps_cb->Release();
    if (kv.second.cs_cb) kv.second.cs_cb->Release();
  }
  g_glrt.ils.clear(); g_glrt.samps.clear(); g_glrt.rss.clear();
  g_glrt.dsss.clear(); g_glrt.bss.clear(); g_glrt.texs.clear();
  g_glrt.bufs.clear(); g_glrt.progs.clear(); g_glrt.fbos.clear();
  if (g_glrt.dyn_vb) { g_glrt.dyn_vb->Release(); g_glrt.dyn_vb = nullptr; }
  if (g_glrt.dyn_ib) { g_glrt.dyn_ib->Release(); g_glrt.dyn_ib = nullptr; }
  if (g_glrt.main_dsv) { g_glrt.main_dsv->Release(); g_glrt.main_dsv = nullptr; }
  if (g_glrt.ds_tex) { g_glrt.ds_tex->Release(); g_glrt.ds_tex = nullptr; }
  g_glrt.initialized = false;
}

inline void bind(ID3D11Device1* dev, ID3D11DeviceContext1* ctx,
                 IDXGISwapChain1* swap, ID3D11RenderTargetView* rtv,
                 UINT w, UINT h) {
  shutdown();
  g_glrt.dev = dev;
  g_glrt.ctx = ctx;
  g_glrt.swap = swap;
  g_glrt.main_rtv = rtv;
  g_glrt.width = w ? w : 1280u;
  g_glrt.height = h ? h : 720u;
  if (dev && g_glrt.width && g_glrt.height) {
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = g_glrt.width; dd.Height = g_glrt.height;
    dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &g_glrt.ds_tex))) {
      dev->CreateDepthStencilView(g_glrt.ds_tex, nullptr, &g_glrt.main_dsv);
    }
  }
  g_glrt.initialized = (dev && ctx && rtv);

  trt_gl_hooks hk{};
  hk.clear = &hk_clear;
  hk.swap_buffers = &hk_swap_buffers;
  hk.compile_program = &hk_compile_program;
  hk.delete_program = &hk_delete_program;
  hk.sync_buffer = &hk_sync_buffer;
  hk.delete_buffer = &hk_delete_buffer;
  hk.sync_texture = &hk_sync_texture;
  hk.gen_mipmaps = &hk_gen_mipmaps;
  hk.delete_texture = &hk_delete_texture;
  hk.sync_fbo = &hk_sync_fbo;
  hk.delete_fbo = &hk_delete_fbo;
  hk.read_pixels = &hk_read_pixels;
  hk.draw = &hk_draw;
  hk.dispatch_compute = &hk_dispatch_compute;
  trt_gl_set_hooks(g_glrt.initialized && swap ? &hk : nullptr);
}

}  // namespace gl_rt
