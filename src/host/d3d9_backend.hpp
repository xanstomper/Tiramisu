// tiramisu/src/host/d3d9_backend.hpp
// Real D3D9/D3D8/DDraw -> D3D11 rendering backend for the console host.
// Included by swapchain.cpp and bound to bridge_d3d9 / bridge_d3d8 /
// bridge_ddraw via trt_d3d9_set_hooks.
#ifndef TIRAMISU_D3D9_BACKEND_HPP
#define TIRAMISU_D3D9_BACKEND_HPP

#include "../trt/bridge_d3d9.h"
#include "../trt/bridge_d3d9_meta.h"
#include "../trt/bridge_esram.h"
#include "shader_cache_util.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace longan {
namespace d3d9_rt {

struct norm_vert {
  float x, y, z, w;
  uint32_t color;  // D3DCOLOR AARRGGBB -> DXGI_FORMAT_B8G8R8A8_UNORM
  float u, v;
};

struct alignas(16) cb_data {
  float mvp[16];
  float vp[4];     // x, y, w, h
  float flags[4];  // x=is_rhw, y=use_tex, z=0, w=0
};

struct gpu_res {
  bool alive = false;
  uint32_t kind = 0;  // 1 tex, 2 vb, 3 ib
  uint32_t w = 0, h = 0, mip_levels = 1, fmt = 0;
  uint32_t bytes = 0;
  uint32_t lock_level = 0;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  std::vector<uint8_t> cpu_data;
  std::vector<uint8_t> scratch;
};

inline void mat_identity(float* m) {
  memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

inline void mat_mul(float* out, const float* a, const float* b) {
  float tmp[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      tmp[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] +
                       a[r * 4 + 1] * b[1 * 4 + c] +
                       a[r * 4 + 2] * b[2 * 4 + c] +
                       a[r * 4 + 3] * b[3 * 4 + c];
    }
  }
  memcpy(out, tmp, sizeof(tmp));
}

inline D3D11_BLEND map_blend(uint32_t b, D3D11_BLEND fallback) {
  switch (b) {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 3: return D3D11_BLEND_SRC_COLOR;
    case 4: return D3D11_BLEND_INV_SRC_COLOR;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return D3D11_BLEND_DEST_ALPHA;
    case 8: return D3D11_BLEND_INV_DEST_ALPHA;
    case 9: return D3D11_BLEND_DEST_COLOR;
    case 10: return D3D11_BLEND_INV_DEST_COLOR;
    case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return fallback;
  }
}

struct backend_state {
  ID3D11Device1* dev = nullptr;
  ID3D11DeviceContext1* ctx = nullptr;
  IDXGISwapChain1* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* depth_tex = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  uint32_t sc_w = 1280, sc_h = 720;

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* dyn_vb = nullptr;
  uint32_t dyn_vb_bytes = 0;
  ID3D11Buffer* dyn_ib = nullptr;
  uint32_t dyn_ib_bytes = 0;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11ShaderResourceView* white_srv = nullptr;
  ID3D11Texture2D* white_tex = nullptr;

  ID3D11BlendState* bs_opaque = nullptr;
  ID3D11BlendState* bs_alpha = nullptr;
  ID3D11BlendState* bs_add = nullptr;
  ID3D11BlendState* bs_custom = nullptr;
  uint32_t bs_src = 5, bs_dst = 6;
  ID3D11DepthStencilState* dss_rw = nullptr;
  ID3D11DepthStencilState* dss_ro = nullptr;
  ID3D11DepthStencilState* dss_off = nullptr;
  ID3D11RasterizerState* rs_none = nullptr;
  ID3D11RasterizerState* rs_cw = nullptr;
  ID3D11RasterizerState* rs_ccw = nullptr;

  std::vector<gpu_res> res;
  std::vector<norm_vert> vscratch;
  std::vector<uint8_t> iscratch;

  float world[16], view[16], proj[16];
  d3d9_viewport vp{};
  uint32_t fvf = 0;
  uint32_t stream_stride[2] = {0, 0};
  d3d9_handle bound_tex0 = 0;
  bool alpha_blend = false;
  uint32_t src_blend = 2;   // ONE
  uint32_t dst_blend = 1;   // ZERO
  uint32_t cull_mode = 3;   // D3DCULL_CCW
  bool z_enable = true;
  bool z_write = true;
  bool initialized = false;
};

inline backend_state g_rt;

inline uint32_t alloc_res(uint32_t kind) {
  for (size_t i = 1; i < g_rt.res.size(); ++i) {
    if (!g_rt.res[i].alive) {
      g_rt.res[i] = gpu_res{};
      g_rt.res[i].alive = true;
      g_rt.res[i].kind = kind;
      return (uint32_t)i;
    }
  }
  if (g_rt.res.empty()) g_rt.res.resize(1);
  g_rt.res.emplace_back();
  g_rt.res.back().alive = true;
  g_rt.res.back().kind = kind;
  return (uint32_t)(g_rt.res.size() - 1);
}

inline gpu_res* get_res(d3d9_handle h) {
  if (!h || h >= g_rt.res.size()) return nullptr;
  return g_rt.res[h].alive ? &g_rt.res[h] : nullptr;
}

inline void free_res(gpu_res* r) {
  if (!r) return;
  if (r->tex && trt_esram_is_active()) {
    D3D11_TEXTURE2D_DESC desc{};
    r->tex->GetDesc(&desc);
    if (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) {
      trt_esram_release_surface(desc.Width, desc.Height, (uint32_t)desc.Format);
    }
  }
  if (r->srv) { r->srv->Release(); r->srv = nullptr; }
  if (r->tex) { r->tex->Release(); r->tex = nullptr; }
  r->cpu_data.clear();
  r->scratch.clear();
  r->alive = false;
}

inline DXGI_FORMAT map_dxgi_fmt(uint32_t d3dfmt, bool* needs_expand) {
  *needs_expand = false;
  switch (d3dfmt) {
    case 21: return DXGI_FORMAT_B8G8R8A8_UNORM;  // A8R8G8B8
    case 22: return DXGI_FORMAT_B8G8R8X8_UNORM;  // X8R8G8B8
    case 32: return DXGI_FORMAT_R8G8B8A8_UNORM;  // A8B8G8R8
    case 33: return DXGI_FORMAT_R8G8B8A8_UNORM;  // X8B8G8R8
    case 0x31545844u: return DXGI_FORMAT_BC1_UNORM;  // DXT1
    case 0x33545844u: return DXGI_FORMAT_BC2_UNORM;  // DXT3
    case 0x35545844u: return DXGI_FORMAT_BC3_UNORM;  // DXT5
    default:
      *needs_expand = true;
      return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

inline uint32_t mip_dim(uint32_t v, uint32_t level) {
  const uint32_t r = (level < 32) ? (v >> level) : 1u;
  return r ? r : 1u;
}

inline void ensure_dyn_vb(uint32_t bytes) {
  if (bytes <= g_rt.dyn_vb_bytes && g_rt.dyn_vb) return;
  if (g_rt.dyn_vb) { g_rt.dyn_vb->Release(); g_rt.dyn_vb = nullptr; }
  uint32_t alloc_sz = bytes < 65536u ? 65536u : ((bytes + 4095u) & ~4095u);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = alloc_sz;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (SUCCEEDED(g_rt.dev->CreateBuffer(&bd, nullptr, &g_rt.dyn_vb)))
    g_rt.dyn_vb_bytes = alloc_sz;
  else
    g_rt.dyn_vb_bytes = 0;
}

inline void ensure_dyn_ib(uint32_t bytes) {
  if (bytes <= g_rt.dyn_ib_bytes && g_rt.dyn_ib) return;
  if (g_rt.dyn_ib) { g_rt.dyn_ib->Release(); g_rt.dyn_ib = nullptr; }
  uint32_t alloc_sz = bytes < 65536u ? 65536u : ((bytes + 4095u) & ~4095u);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = alloc_sz;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (SUCCEEDED(g_rt.dev->CreateBuffer(&bd, nullptr, &g_rt.dyn_ib)))
    g_rt.dyn_ib_bytes = alloc_sz;
  else
    g_rt.dyn_ib_bytes = 0;
}

inline uint32_t fvf_stride(uint32_t fvf) {
  uint32_t s = 0;
  const uint32_t pos = fvf & 0x0Eu;
  if (pos == 0x04u) s += 16;       // XYZRHW
  else if (pos >= 0x06u) s += 12 + ((pos - 4u) / 2u) * 4u;  // XYZBn
  else if (pos == 0x02u) s += 12;  // XYZ
  if (fvf & 0x10u) s += 12;        // NORMAL
  if (fvf & 0x20u) s += 4;         // PSIZE
  if (fvf & 0x40u) s += 4;         // DIFFUSE
  if (fvf & 0x80u) s += 4;         // SPECULAR
  const uint32_t ntex = (fvf >> 8) & 0xFu;
  for (uint32_t t = 0; t < ntex; ++t) {
    const uint32_t code = (fvf >> (16 + t * 2)) & 0x3u;
    const uint32_t nf = (code == 0) ? 2 : (code == 1) ? 3 : (code == 2) ? 4 : 1;
    s += nf * 4;
  }
  return s;
}

inline void decode_fvf_vertices(const uint8_t* src, uint32_t avail_bytes,
                                uint32_t count, uint32_t stride,
                                uint32_t fvf) {
  if (!fvf) {
    if (stride == 28) fvf = 0x144u;       // XYZRHW | DIFFUSE | TEX1
    else if (stride == 24) fvf = 0x142u;  // XYZ | DIFFUSE | TEX1
    else if (stride == 20) fvf = 0x102u;  // XYZ | TEX1
    else if (stride == 16) fvf = 0x042u;  // XYZ | DIFFUSE
    else fvf = 0x002u;                    // XYZ
  }
  const uint32_t calc = fvf_stride(fvf);
  if (!stride) stride = calc ? calc : 12;

  const uint32_t pos_kind = fvf & 0x0Eu;
  const bool has_rhw = (pos_kind == 0x04u);
  uint32_t off_normal = 0;
  if (has_rhw) off_normal = 16;
  else if (pos_kind >= 0x06u) off_normal = 12 + ((pos_kind - 4u) / 2u) * 4u;
  else if (pos_kind == 0x02u) off_normal = 12;

  uint32_t off_diff = off_normal + ((fvf & 0x10u) ? 12u : 0u) +
                      ((fvf & 0x20u) ? 4u : 0u);
  const bool has_diff = (fvf & 0x40u) != 0;
  const uint32_t off_uv = off_diff + (has_diff ? 4u : 0u) +
                          ((fvf & 0x80u) ? 4u : 0u);
  const bool has_uv = ((fvf >> 8) & 0xFu) > 0;

  g_rt.vscratch.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    norm_vert& d = g_rt.vscratch[i];
    d = {0.f, 0.f, 0.f, has_rhw ? 1.f : 1.f, 0xFFFFFFFFu, 0.f, 0.f};
    const uint32_t base = i * stride;
    if (base + stride > avail_bytes && base + 12 > avail_bytes) break;
    const uint8_t* p = src + base;
    if (pos_kind) {
      float xyzw[4] = {0, 0, 0, 1};
      const uint32_t copy_b = has_rhw ? 16u : 12u;
      if (base + copy_b <= avail_bytes) memcpy(xyzw, p, copy_b);
      d.x = xyzw[0]; d.y = xyzw[1]; d.z = xyzw[2]; d.w = xyzw[3];
    }
    if (has_diff && base + off_diff + 4 <= avail_bytes)
      memcpy(&d.color, p + off_diff, 4);
    if (has_uv && base + off_uv + 8 <= avail_bytes) {
      memcpy(&d.u, p + off_uv, 4);
      memcpy(&d.v, p + off_uv + 4, 4);
    }
  }
}

inline void apply_pipeline_state(uint32_t prim_type, bool is_rhw) {
  ID3D11RenderTargetView* rt = g_rt.rtv;
  g_rt.ctx->OMSetRenderTargets(1, &rt, g_rt.dsv);

  D3D11_VIEWPORT dv{};
  dv.TopLeftX = (float)g_rt.vp.x;
  dv.TopLeftY = (float)g_rt.vp.y;
  dv.Width = (float)(g_rt.vp.w ? g_rt.vp.w : g_rt.sc_w);
  dv.Height = (float)(g_rt.vp.h ? g_rt.vp.h : g_rt.sc_h);
  dv.MinDepth = g_rt.vp.z_min;
  dv.MaxDepth = g_rt.vp.z_max > g_rt.vp.z_min ? g_rt.vp.z_max : 1.0f;
  g_rt.ctx->RSSetViewports(1, &dv);

  ID3D11RasterizerState* rs = g_rt.rs_ccw;
  if (g_rt.cull_mode == 1) rs = g_rt.rs_none;
  else if (g_rt.cull_mode == 2) rs = g_rt.rs_cw;
  g_rt.ctx->RSSetState(rs);

  ID3D11DepthStencilState* dss = g_rt.dss_off;
  if (!is_rhw && g_rt.z_enable) dss = g_rt.z_write ? g_rt.dss_rw : g_rt.dss_ro;
  g_rt.ctx->OMSetDepthStencilState(dss, 0);

  ID3D11BlendState* bs = g_rt.bs_opaque;
  if (g_rt.alpha_blend) {
    if (g_rt.src_blend == 5 && g_rt.dst_blend == 6) bs = g_rt.bs_alpha;
    else if (g_rt.src_blend == 5 && g_rt.dst_blend == 2) bs = g_rt.bs_add;
    else {
      if (!g_rt.bs_custom || g_rt.bs_src != g_rt.src_blend ||
          g_rt.bs_dst != g_rt.dst_blend) {
        if (g_rt.bs_custom) { g_rt.bs_custom->Release(); g_rt.bs_custom = nullptr; }
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = map_blend(g_rt.src_blend, D3D11_BLEND_SRC_ALPHA);
        bd.RenderTarget[0].DestBlend = map_blend(g_rt.dst_blend, D3D11_BLEND_INV_SRC_ALPHA);
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_rt.dev->CreateBlendState(&bd, &g_rt.bs_custom);
        g_rt.bs_src = g_rt.src_blend;
        g_rt.bs_dst = g_rt.dst_blend;
      }
      if (g_rt.bs_custom) bs = g_rt.bs_custom;
    }
  }
  const float bf[4] = {1, 1, 1, 1};
  g_rt.ctx->OMSetBlendState(bs, bf, 0xFFFFFFFFu);

  cb_data cb{};
  float mv[16];
  mat_mul(mv, g_rt.world, g_rt.view);
  mat_mul(cb.mvp, mv, g_rt.proj);
  cb.vp[0] = dv.TopLeftX;
  cb.vp[1] = dv.TopLeftY;
  cb.vp[2] = dv.Width > 0.f ? dv.Width : (float)g_rt.sc_w;
  cb.vp[3] = dv.Height > 0.f ? dv.Height : (float)g_rt.sc_h;
  cb.flags[0] = is_rhw ? 1.0f : 0.0f;

  ID3D11ShaderResourceView* srv = g_rt.white_srv;
  const gpu_res* tres = get_res(g_rt.bound_tex0);
  if (tres && tres->srv) {
    srv = tres->srv;
    cb.flags[1] = 1.0f;
  }
  g_rt.ctx->UpdateSubresource(g_rt.cb, 0, nullptr, &cb, 0, 0);

  g_rt.ctx->IASetInputLayout(g_rt.il);
  g_rt.ctx->VSSetShader(g_rt.vs, nullptr, 0);
  g_rt.ctx->VSSetConstantBuffers(0, 1, &g_rt.cb);
  g_rt.ctx->PSSetShader(g_rt.ps, nullptr, 0);
  g_rt.ctx->PSSetConstantBuffers(0, 1, &g_rt.cb);
  g_rt.ctx->PSSetSamplers(0, 1, &g_rt.sampler);
  g_rt.ctx->PSSetShaderResources(0, 1, &srv);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (prim_type == 1) topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  else if (prim_type == 2) topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
  else if (prim_type == 3) topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
  else if (prim_type == 5) topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  g_rt.ctx->IASetPrimitiveTopology(topo);
}

inline void submit_verts_and_draw(uint32_t prim_type,
                                  const void* indices,
                                  uint32_t idx32,
                                  uint32_t start_idx,
                                  int32_t base_vertex,
                                  uint32_t prim_count) {
  if (g_rt.vscratch.empty() || !prim_count) return;
  // Convert triangle fan cleanly to triangle list since D3D11 has no fan topology
  if (prim_type == 6 && !indices) {
    std::vector<norm_vert> fan;
    fan.reserve(prim_count * 3);
    const size_t nv = g_rt.vscratch.size();
    for (uint32_t i = 0; i < prim_count; ++i) {
      const size_t i1 = i + 1, i2 = i + 2;
      if (i2 >= nv) break;
      fan.push_back(g_rt.vscratch[0]);
      fan.push_back(g_rt.vscratch[i1]);
      fan.push_back(g_rt.vscratch[i2]);
    }
    g_rt.vscratch.swap(fan);
    prim_type = 4;
    g_rt.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }

  const uint32_t vbytes = (uint32_t)(g_rt.vscratch.size() * sizeof(norm_vert));
  ensure_dyn_vb(vbytes);
  if (!g_rt.dyn_vb) return;
  D3D11_MAPPED_SUBRESOURCE mv{};
  if (FAILED(g_rt.ctx->Map(g_rt.dyn_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mv)))
    return;
  memcpy(mv.pData, g_rt.vscratch.data(), vbytes);
  g_rt.ctx->Unmap(g_rt.dyn_vb, 0);

  const UINT stride = (UINT)sizeof(norm_vert), offset = 0;
  g_rt.ctx->IASetVertexBuffers(0, 1, &g_rt.dyn_vb, &stride, &offset);

  const uint32_t icount = trt_d3d9_prim_verts(prim_type, prim_count);
  if (indices) {
    const uint32_t elem_sz = idx32 ? 4u : 2u;
    const uint32_t ibytes = (start_idx + icount) * elem_sz;
    ensure_dyn_ib(ibytes);
    if (!g_rt.dyn_ib) return;
    D3D11_MAPPED_SUBRESOURCE mi{};
    if (FAILED(g_rt.ctx->Map(g_rt.dyn_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mi)))
      return;
    memcpy(mi.pData, indices, ibytes);
    g_rt.ctx->Unmap(g_rt.dyn_ib, 0);
    g_rt.ctx->IASetIndexBuffer(
        g_rt.dyn_ib, idx32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
    g_rt.ctx->DrawIndexed(icount, start_idx, base_vertex);
  } else {
    g_rt.ctx->Draw((UINT)g_rt.vscratch.size(), 0);
  }
}

// ---- Wave 2B extensions (MojoShader HLSL, decls, MRT, DS, scissor, cube/vol) ----
constexpr uint32_t k_shader_base = 0x100000u;
constexpr uint32_t k_decl_base = 0x200000u;

struct w2b_tex_info {
  bool active = false;
  uint32_t kind = 1;           // 1=2D, 8=Cube, 9=Volume
  uint32_t depth_or_faces = 1;
  uint32_t usage = 0;
  bool is_depth = false;
  bool is_rt = false;
  bool expand = false;
  DXGI_FORMAT dxgi_fmt = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture3D* tex3d = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
};

struct w2b_buf_info {
  bool active = false;
  bool is_index = false;
  bool idx32 = false;
  bool dirty = false;
  uint32_t bytes = 0;
  ID3D11Buffer* gpu_buf = nullptr;
};

struct w2b_shader {
  bool alive = false;
  uint32_t is_ps = 0;
  const void* parsed = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* cb = nullptr;
  uint32_t cb_bytes = 0;
  uint32_t sampler_mask = 0;
  std::vector<uint8_t> vs_blob;
  std::vector<uint8_t> cb_cpu;
};

struct w2b_decl {
  bool alive = false;
  std::vector<d3d9_vert_elem> elems;
};

struct w2b_il_entry {
  uint64_t key = 0;
  ID3D11InputLayout* il = nullptr;
};
struct w2b_samp_entry {
  uint32_t key = 0;
  ID3D11SamplerState* s = nullptr;
};
struct w2b_rs_entry {
  uint32_t key = 0;
  ID3D11RasterizerState* rs = nullptr;
};
struct w2b_dss_entry {
  uint32_t key = 0;
  ID3D11DepthStencilState* dss = nullptr;
};
struct w2b_bs_entry {
  uint64_t key = 0;
  ID3D11BlendState* bs = nullptr;
};

struct w2b_state {
  std::vector<w2b_tex_info> texs;
  std::vector<w2b_buf_info> bufs;
  std::vector<w2b_shader> shaders;
  std::vector<w2b_decl> decls;
  std::vector<w2b_il_entry> ils;
  std::vector<w2b_samp_entry> samps;
  std::vector<w2b_rs_entry> rss;
  std::vector<w2b_dss_entry> dsss;
  std::vector<w2b_bs_entry> bss;

  d3d9_handle bound_vs = 0;
  d3d9_handle bound_ps = 0;
  d3d9_handle bound_decl = 0;
  d3d9_handle bound_tex[16] = {};
  d3d9_handle cur_rt[4] = {};
  d3d9_handle cur_ds = 0;

  bool scissor_enable = false;
  int32_t scissor_rect[4] = {0, 0, 1280, 720};

  uint32_t z_func = 4;             // D3DRS_ZFUNC (23) -> D3DCMP_LESSEQUAL
  uint32_t fill_mode = 3;          // D3DRS_FILLMODE (8) -> SOLID
  uint32_t blend_op = 1;           // D3DRS_BLENDOP (171) -> ADD
  uint32_t sep_alpha = 0;          // D3DRS_SEPARATEALPHABLENDENABLE (206)
  uint32_t src_blend_a = 2;        // D3DRS_SRCBLENDALPHA (207) -> ONE
  uint32_t dst_blend_a = 1;        // D3DRS_DESTBLENDALPHA (208) -> ZERO
  uint32_t color_write = 0xFu;     // D3DRS_COLORWRITEENABLE (168)
  uint32_t stencil_en = 0;         // D3DRS_STENCILENABLE (52)

  uint32_t samp[16][16] = {};
  float vs_cf[512 * 4] = {};
  int32_t vs_ci[32 * 4] = {};
  int vs_cb[32] = {};
  float ps_cf[512 * 4] = {};
  int32_t ps_ci[32 * 4] = {};
  int ps_cb[32] = {};
  std::vector<uint32_t> fan_ib;
};
inline w2b_state g_w2b;

inline void w2b_init_defaults() {
  for (int s = 0; s < 16; ++s) {
    g_w2b.samp[s][1] = 1;  // ADDRESSU = WRAP
    g_w2b.samp[s][2] = 1;  // ADDRESSV = WRAP
    g_w2b.samp[s][3] = 1;  // ADDRESSW = WRAP
    g_w2b.samp[s][5] = 2;  // MAGFILTER = LINEAR
    g_w2b.samp[s][6] = 2;  // MINFILTER = LINEAR
    g_w2b.samp[s][7] = 2;  // MIPFILTER = LINEAR
    g_w2b.samp[s][10] = 1; // MAXANISOTROPY
  }
  g_w2b.z_func = 4;
  g_w2b.fill_mode = 3;
  g_w2b.blend_op = 1;
  g_w2b.sep_alpha = 0;
  g_w2b.src_blend_a = 2;
  g_w2b.dst_blend_a = 1;
  g_w2b.color_write = 0xFu;
  g_w2b.stencil_en = 0;
  g_w2b.scissor_enable = false;
  g_w2b.scissor_rect[0] = 0;
  g_w2b.scissor_rect[1] = 0;
  g_w2b.scissor_rect[2] = (int32_t)(g_rt.sc_w ? g_rt.sc_w : 1280u);
  g_w2b.scissor_rect[3] = (int32_t)(g_rt.sc_h ? g_rt.sc_h : 720u);
}

inline void w2b_free_tex(uint32_t h) {
  if (h >= g_w2b.texs.size()) return;
  auto& t = g_w2b.texs[h];
  if (t.rtv) { t.rtv->Release(); t.rtv = nullptr; }
  if (t.dsv) { t.dsv->Release(); t.dsv = nullptr; }
  if (t.tex3d) { t.tex3d->Release(); t.tex3d = nullptr; }
  t.active = false;
}

inline void w2b_free_buf(uint32_t h) {
  if (h >= g_w2b.bufs.size()) return;
  auto& b = g_w2b.bufs[h];
  if (b.gpu_buf) { b.gpu_buf->Release(); b.gpu_buf = nullptr; }
  b.active = false;
}

inline void w2b_shutdown() {
  for (size_t i = 0; i < g_w2b.texs.size(); ++i) w2b_free_tex((uint32_t)i);
  g_w2b.texs.clear();
  for (size_t i = 0; i < g_w2b.bufs.size(); ++i) w2b_free_buf((uint32_t)i);
  g_w2b.bufs.clear();
  for (auto& s : g_w2b.shaders) {
    if (s.vs) s.vs->Release();
    if (s.ps) s.ps->Release();
    if (s.cb) s.cb->Release();
  }
  g_w2b.shaders.clear();
  g_w2b.decls.clear();
  for (auto& e : g_w2b.ils) if (e.il) e.il->Release();
  g_w2b.ils.clear();
  for (auto& e : g_w2b.samps) if (e.s) e.s->Release();
  g_w2b.samps.clear();
  for (auto& e : g_w2b.rss) if (e.rs) e.rs->Release();
  g_w2b.rss.clear();
  for (auto& e : g_w2b.dsss) if (e.dss) e.dss->Release();
  g_w2b.dsss.clear();
  for (auto& e : g_w2b.bss) if (e.bs) e.bs->Release();
  g_w2b.bss.clear();
  g_w2b.bound_vs = 0;
  g_w2b.bound_ps = 0;
  g_w2b.bound_decl = 0;
  memset(g_w2b.bound_tex, 0, sizeof(g_w2b.bound_tex));
  memset(g_w2b.cur_rt, 0, sizeof(g_w2b.cur_rt));
  g_w2b.cur_ds = 0;
  memset(g_w2b.vs_cf, 0, sizeof(g_w2b.vs_cf));
  memset(g_w2b.vs_ci, 0, sizeof(g_w2b.vs_ci));
  memset(g_w2b.vs_cb, 0, sizeof(g_w2b.vs_cb));
  memset(g_w2b.ps_cf, 0, sizeof(g_w2b.ps_cf));
  memset(g_w2b.ps_ci, 0, sizeof(g_w2b.ps_ci));
  memset(g_w2b.ps_cb, 0, sizeof(g_w2b.ps_cb));
  g_w2b.fan_ib.clear();
}

inline DXGI_FORMAT w2b_resolve_tex_fmt(uint32_t fmt, uint32_t usage,
                                       bool* out_is_depth,
                                       DXGI_FORMAT* out_dsv_fmt,
                                       DXGI_FORMAT* out_srv_fmt,
                                       bool* out_expand) {
  *out_is_depth = false;
  *out_dsv_fmt = DXGI_FORMAT_UNKNOWN;
  *out_srv_fmt = DXGI_FORMAT_UNKNOWN;
  *out_expand = false;
  if (fmt == 80u) { // D16
    *out_is_depth = true;
    *out_dsv_fmt = DXGI_FORMAT_D16_UNORM;
    *out_srv_fmt = DXGI_FORMAT_R16_UNORM;
    return DXGI_FORMAT_R16_TYPELESS;
  }
  if (fmt == 71u || fmt == 82u) { // D32 / D32F_LOCKABLE
    *out_is_depth = true;
    *out_dsv_fmt = DXGI_FORMAT_D32_FLOAT;
    *out_srv_fmt = DXGI_FORMAT_R32_FLOAT;
    return DXGI_FORMAT_R32_TYPELESS;
  }
  if (fmt == 75u || fmt == 77u || fmt == 73u || fmt == 79u || (usage & 2u)) {
    *out_is_depth = true;
    *out_dsv_fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
    *out_srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    return DXGI_FORMAT_R24G8_TYPELESS;
  }
  DXGI_FORMAT extra = DXGI_FORMAT_UNKNOWN;
  switch (fmt) {
    case 31u: case 35u: extra = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case 32u: case 33u: extra = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case 34u: extra = DXGI_FORMAT_R16G16_UNORM; break;
    case 60u: extra = DXGI_FORMAT_R8G8_SNORM; break;
    case 81u: extra = DXGI_FORMAT_R16_UNORM; break;
    case 111u: extra = DXGI_FORMAT_R16_FLOAT; break;
    case 112u: extra = DXGI_FORMAT_R16G16_FLOAT; break;
    case 113u: extra = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case 114u: extra = DXGI_FORMAT_R32_FLOAT; break;
    case 115u: extra = DXGI_FORMAT_R32G32_FLOAT; break;
    case 116u: extra = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    default: break;
  }
  if (extra != DXGI_FORMAT_UNKNOWN) {
    *out_srv_fmt = extra;
    return extra;
  }
  DXGI_FORMAT df = map_dxgi_fmt(fmt, out_expand);
  *out_srv_fmt = df;
  return df;
}

inline void w2b_expand_pixels(uint32_t fmt, uint32_t lw, uint32_t lh,
                              const uint8_t* src_base, uint32_t spitch,
                              std::vector<uint8_t>& out_bgra) {
  out_bgra.resize((size_t)lw * lh * 4u);
  for (uint32_t y = 0; y < lh; ++y) {
    const uint8_t* src = src_base + (size_t)y * spitch;
    uint32_t* dst = (uint32_t*)(out_bgra.data() + (size_t)y * lw * 4u);
    for (uint32_t x = 0; x < lw; ++x) {
      if (fmt == 23u) {
        const uint16_t v = *(const uint16_t*)(src + x * 2u);
        const uint32_t R = ((v >> 11) & 0x1Fu) * 255u / 31u;
        const uint32_t G = ((v >> 5) & 0x3Fu) * 255u / 63u;
        const uint32_t B = (v & 0x1Fu) * 255u / 31u;
        dst[x] = 0xFF000000u | (R << 16) | (G << 8) | B;
      } else if (fmt == 25u || fmt == 24u) {
        const uint16_t v = *(const uint16_t*)(src + x * 2u);
        const uint32_t A = (fmt == 24u || (v & 0x8000u)) ? 0xFFu : 0u;
        const uint32_t R = ((v >> 10) & 0x1Fu) * 255u / 31u;
        const uint32_t G = ((v >> 5) & 0x1Fu) * 255u / 31u;
        const uint32_t B = (v & 0x1Fu) * 255u / 31u;
        dst[x] = (A << 24) | (R << 16) | (G << 8) | B;
      } else if (fmt == 26u) {
        const uint16_t v = *(const uint16_t*)(src + x * 2u);
        const uint32_t A = ((v >> 12) & 0xFu) * 17u;
        const uint32_t R = ((v >> 8) & 0xFu) * 17u;
        const uint32_t G = ((v >> 4) & 0xFu) * 17u;
        const uint32_t B = (v & 0xFu) * 17u;
        dst[x] = (A << 24) | (R << 16) | (G << 8) | B;
      } else if (fmt == 20u) {
        dst[x] = 0xFF000000u | ((uint32_t)src[x * 3u + 2] << 16) |
                 ((uint32_t)src[x * 3u + 1] << 8) | src[x * 3u];
      } else if (fmt == 28u) {
        dst[x] = ((uint32_t)src[x] << 24) | 0x00FFFFFFu;
      } else if (fmt == 50u) {
        const uint32_t v = src[x];
        dst[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
      } else if (fmt == 51u) {
        const uint32_t l = src[x * 2u];
        const uint32_t a = src[x * 2u + 1];
        dst[x] = (a << 24) | (l << 16) | (l << 8) | l;
      } else {
        dst[x] = 0xFFFFFFFFu;
      }
    }
  }
}

inline DXGI_FORMAT w2b_decl_dxgi(uint8_t type) {
  switch (type) {
    case 0:  return DXGI_FORMAT_R32_FLOAT;
    case 1:  return DXGI_FORMAT_R32G32_FLOAT;
    case 2:  return DXGI_FORMAT_R32G32B32_FLOAT;
    case 3:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 4:  return DXGI_FORMAT_B8G8R8A8_UNORM;     // D3DCOLOR -> RGBA in shader
    case 5:  return DXGI_FORMAT_R8G8B8A8_UINT;
    case 6:  return DXGI_FORMAT_R16G16_SINT;
    case 7:  return DXGI_FORMAT_R16G16B16A16_SINT;
    case 8:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 9:  return DXGI_FORMAT_R16G16_SNORM;
    case 10: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 11: return DXGI_FORMAT_R16G16_UNORM;
    case 12: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 13: return DXGI_FORMAT_R10G10B10A2_UINT;
    case 14: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 15: return DXGI_FORMAT_R16G16_FLOAT;
    case 16: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  }
}

inline const char* w2b_decl_semantic(uint8_t usage) {
  switch (usage) {
    case 0:  return "POSITION";
    case 1:  return "BLENDWEIGHT";
    case 2:  return "BLENDINDICES";
    case 3:  return "NORMAL";
    case 4:  return "PSIZE";
    case 5:  return "TEXCOORD";
    case 6:  return "TANGENT";
    case 7:  return "BINORMAL";
    case 8:  return "TESSFACTOR";
    case 9:  return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return "TEXCOORD";
  }
}

inline void w2b_fvf_to_elems(uint32_t fvf, std::vector<d3d9_vert_elem>& out) {
  out.clear();
  uint16_t off = 0;
  auto push = [&](uint8_t t, uint8_t u, uint8_t idx, uint16_t sz) {
    d3d9_vert_elem e{0, off, t, 0, u, idx};
    out.push_back(e);
    off += sz;
  };
  const uint32_t pos = fvf & 0x0Eu;
  if (pos == 0x04u) {
    push(3, 9, 0, 16); // POSITIONT FLOAT4
  } else if (pos != 0u) {
    push(2, 0, 0, 12); // POSITION FLOAT3
    if (pos >= 0x06u) {
      const uint32_t nb = ((pos - 0x06u) >> 1) + 1u; // XYZB1..XYZB5
      if (fvf & 0x1000u) {
        if (nb > 1) push((uint8_t)(nb - 2), 1, 0, (uint16_t)((nb - 1) * 4));
        push(5, 2, 0, 4); // UBYTE4 BLENDINDICES
      } else {
        push((uint8_t)(nb - 1), 1, 0, (uint16_t)(nb * 4));
      }
    }
  }
  if (fvf & 0x10u) push(2, 3, 0, 12);  // NORMAL
  if (fvf & 0x20u) push(0, 4, 0, 4);   // PSIZE
  if (fvf & 0x40u) push(4, 10, 0, 4);  // DIFFUSE COLOR0
  if (fvf & 0x80u) push(4, 10, 1, 4);  // SPECULAR COLOR1
  const uint32_t ntex = (fvf >> 8) & 0xFu;
  for (uint32_t i = 0; i < ntex && i < 8u; ++i) {
    const uint32_t szbits = (fvf >> (16u + i * 2u)) & 0x3u;
    static const uint8_t k_t[4] = {1, 2, 3, 0};     // 2, 3, 4, 1 floats
    static const uint16_t k_b[4] = {8, 12, 16, 4};
    push(k_t[szbits], 5, (uint8_t)i, k_b[szbits]);
  }
}

inline ID3D11InputLayout* w2b_get_input_layout(uint32_t vs_h) {
  if (vs_h < k_shader_base) return nullptr;
  const uint32_t sidx = vs_h - k_shader_base;
  if (sidx >= g_w2b.shaders.size() || !g_w2b.shaders[sidx].alive ||
      g_w2b.shaders[sidx].vs_blob.empty())
    return nullptr;
  const uint32_t decl_key = g_w2b.bound_decl ? (uint32_t)g_w2b.bound_decl
                                             : (0x80000000u | g_rt.fvf);
  const uint64_t key = ((uint64_t)decl_key << 32) | vs_h;
  for (const auto& e : g_w2b.ils) if (e.key == key) return e.il;

  std::vector<d3d9_vert_elem> fvf_elems;
  const d3d9_vert_elem* src = nullptr;
  size_t count = 0;
  if (g_w2b.bound_decl >= k_decl_base) {
    const uint32_t didx = g_w2b.bound_decl - k_decl_base;
    if (didx < g_w2b.decls.size() && g_w2b.decls[didx].alive) {
      src = g_w2b.decls[didx].elems.data();
      count = g_w2b.decls[didx].elems.size();
    }
  }
  if (!src) {
    w2b_fvf_to_elems(g_rt.fvf, fvf_elems);
    src = fvf_elems.data();
    count = fvf_elems.size();
  }
  std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
  descs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const d3d9_vert_elem& ve = src[i];
    if (ve.stream == 0xFFu || ve.type == 17u) break;
    D3D11_INPUT_ELEMENT_DESC d{};
    d.SemanticName = w2b_decl_semantic(ve.usage);
    d.SemanticIndex = ve.usage_index;
    d.Format = w2b_decl_dxgi(ve.type);
    d.InputSlot = ve.stream < 8u ? ve.stream : 0u;
    d.AlignedByteOffset = ve.offset;
    d.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    descs.push_back(d);
  }
  if (descs.empty()) return nullptr;
  ID3D11InputLayout* il = nullptr;
  if (FAILED(g_rt.dev->CreateInputLayout(
          descs.data(), (UINT)descs.size(),
          g_w2b.shaders[sidx].vs_blob.data(),
          g_w2b.shaders[sidx].vs_blob.size(), &il))) {
    return nullptr;
  }
  g_w2b.ils.push_back({key, il});
  return il;
}

inline D3D11_TEXTURE_ADDRESS_MODE w2b_addr(uint32_t a) {
  switch (a) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

inline ID3D11SamplerState* w2b_get_sampler(uint32_t stage) {
  const uint32_t au = g_w2b.samp[stage][1] & 7u;
  const uint32_t av = g_w2b.samp[stage][2] & 7u;
  const uint32_t aw = g_w2b.samp[stage][3] & 7u;
  const uint32_t mag = g_w2b.samp[stage][5] & 7u;
  const uint32_t minf = g_w2b.samp[stage][6] & 7u;
  const uint32_t mip = g_w2b.samp[stage][7] & 7u;
  const uint32_t aniso = g_w2b.samp[stage][10] & 0x1Fu;
  const uint32_t key = au | (av << 3) | (aw << 6) | (mag << 9) |
                       (minf << 12) | (mip << 15) | (aniso << 18);
  for (const auto& e : g_w2b.samps) if (e.key == key) return e.s;
  D3D11_SAMPLER_DESC sd{};
  if (minf == 3 || mag == 3) {
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
  } else {
    const int min_lin = (minf >= 2) ? 1 : 0;
    const int mag_lin = (mag >= 2) ? 1 : 0;
    const int mip_lin = (mip >= 2) ? 1 : 0;
    sd.Filter = (D3D11_FILTER)((min_lin << 4) | (mag_lin << 2) | mip_lin);
  }
  sd.AddressU = w2b_addr(au);
  sd.AddressV = w2b_addr(av);
  sd.AddressW = w2b_addr(aw);
  sd.MaxAnisotropy = aniso ? aniso : 1u;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState* st = nullptr;
  g_rt.dev->CreateSamplerState(&sd, &st);
  if (st) g_w2b.samps.push_back({key, st});
  return st ? st : g_rt.sampler;
}

inline ID3D11RasterizerState* w2b_get_rasterizer() {
  const uint32_t cull = g_rt.cull_mode & 3u;
  const uint32_t fill = (g_w2b.fill_mode == 2u) ? 1u : 0u;
  const uint32_t sc = g_w2b.scissor_enable ? 1u : 0u;
  const uint32_t key = cull | (fill << 2) | (sc << 3);
  for (const auto& e : g_w2b.rss) if (e.key == key) return e.rs;
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = fill ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  if (cull == 1) rd.CullMode = D3D11_CULL_NONE;
  else if (cull == 3) { rd.CullMode = D3D11_CULL_BACK; rd.FrontCounterClockwise = TRUE; }
  else { rd.CullMode = D3D11_CULL_BACK; rd.FrontCounterClockwise = FALSE; }
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = sc ? TRUE : FALSE;
  ID3D11RasterizerState* rs = nullptr;
  g_rt.dev->CreateRasterizerState(&rd, &rs);
  if (rs) g_w2b.rss.push_back({key, rs});
  return rs;
}

inline D3D11_COMPARISON_FUNC w2b_cmp(uint32_t f) {
  switch (f) {
    case 1: return D3D11_COMPARISON_NEVER;
    case 2: return D3D11_COMPARISON_LESS;
    case 3: return D3D11_COMPARISON_EQUAL;
    case 4: return D3D11_COMPARISON_LESS_EQUAL;
    case 5: return D3D11_COMPARISON_GREATER;
    case 6: return D3D11_COMPARISON_NOT_EQUAL;
    case 7: return D3D11_COMPARISON_GREATER_EQUAL;
    case 8: return D3D11_COMPARISON_ALWAYS;
    default: return D3D11_COMPARISON_LESS_EQUAL;
  }
}

inline ID3D11DepthStencilState* w2b_get_dss(bool is_rhw) {
  const uint32_t zen = (!is_rhw && g_rt.z_enable) ? 1u : 0u;
  const uint32_t zw = (zen && g_rt.z_write) ? 1u : 0u;
  const uint32_t zf = g_w2b.z_func & 0xFu;
  const uint32_t sten = g_w2b.stencil_en ? 1u : 0u;
  const uint32_t key = zen | (zw << 1) | (zf << 2) | (sten << 6);
  for (const auto& e : g_w2b.dsss) if (e.key == key) return e.dss;
  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = zen ? TRUE : FALSE;
  dsd.DepthWriteMask = zw ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  dsd.DepthFunc = w2b_cmp(zf);
  dsd.StencilEnable = sten ? TRUE : FALSE;
  dsd.StencilReadMask = 0xFF;
  dsd.StencilWriteMask = 0xFF;
  dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                   D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
  dsd.BackFace = dsd.FrontFace;
  ID3D11DepthStencilState* dss = nullptr;
  g_rt.dev->CreateDepthStencilState(&dsd, &dss);
  if (dss) g_w2b.dsss.push_back({key, dss});
  return dss;
}

inline D3D11_BLEND_OP w2b_bop(uint32_t op) {
  switch (op) {
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
  }
}

inline ID3D11BlendState* w2b_get_bs() {
  const uint32_t ab = g_rt.alpha_blend ? 1u : 0u;
  const uint32_t sb = g_rt.src_blend & 0x1Fu;
  const uint32_t db = g_rt.dst_blend & 0x1Fu;
  const uint32_t bo = g_w2b.blend_op & 7u;
  const uint32_t sa = g_w2b.sep_alpha ? (g_w2b.src_blend_a & 0x1Fu) : sb;
  const uint32_t da = g_w2b.sep_alpha ? (g_w2b.dst_blend_a & 0x1Fu) : db;
  const uint32_t cw = g_w2b.color_write & 0xFu;
  const uint64_t key = (uint64_t)ab | ((uint64_t)sb << 1) | ((uint64_t)db << 6) |
                       ((uint64_t)bo << 11) | ((uint64_t)sa << 14) |
                       ((uint64_t)da << 19) | ((uint64_t)cw << 24);
  for (const auto& e : g_w2b.bss) if (e.key == key) return e.bs;
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = ab ? TRUE : FALSE;
  bd.RenderTarget[0].SrcBlend = map_blend(sb, D3D11_BLEND_SRC_ALPHA);
  bd.RenderTarget[0].DestBlend = map_blend(db, D3D11_BLEND_INV_SRC_ALPHA);
  bd.RenderTarget[0].BlendOp = w2b_bop(bo);
  bd.RenderTarget[0].SrcBlendAlpha = map_blend(sa, D3D11_BLEND_ONE);
  bd.RenderTarget[0].DestBlendAlpha = map_blend(da, D3D11_BLEND_INV_SRC_ALPHA);
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)cw;
  ID3D11BlendState* bs = nullptr;
  g_rt.dev->CreateBlendState(&bd, &bs);
  if (bs) g_w2b.bss.push_back({key, bs});
  return bs;
}

inline void w2b_bind_rts_and_scissor() {
  ID3D11RenderTargetView* rts[4] = {};
  uint32_t num_rt = 1;
  for (uint32_t i = 0; i < 4u; ++i) {
    const d3d9_handle h = g_w2b.cur_rt[i];
    if (h == 0u) {
      rts[i] = (i == 0u) ? g_rt.rtv : nullptr;
    } else if (h < g_w2b.texs.size() && g_w2b.texs[h].rtv) {
      rts[i] = g_w2b.texs[h].rtv;
    }
    if (rts[i]) num_rt = i + 1u;
  }
  ID3D11DepthStencilView* dsv = g_rt.dsv;
  if (g_w2b.cur_ds != 0u && g_w2b.cur_ds < g_w2b.texs.size() &&
      g_w2b.texs[g_w2b.cur_ds].dsv) {
    dsv = g_w2b.texs[g_w2b.cur_ds].dsv;
  }
  g_rt.ctx->OMSetRenderTargets(num_rt, rts, dsv);
  if (g_w2b.scissor_enable) {
    D3D11_RECT rc{};
    rc.left = g_w2b.scissor_rect[0] > 0 ? g_w2b.scissor_rect[0] : 0;
    rc.top = g_w2b.scissor_rect[1] > 0 ? g_w2b.scissor_rect[1] : 0;
    rc.right = g_w2b.scissor_rect[2] > rc.left ? g_w2b.scissor_rect[2] : rc.left;
    rc.bottom = g_w2b.scissor_rect[3] > rc.top ? g_w2b.scissor_rect[3] : rc.top;
    g_rt.ctx->RSSetScissorRects(1, &rc);
  }
}

inline void w2b_ensure_gpu_buf(uint32_t h) {
  const gpu_res* r = get_res(h);
  if (!r || h >= g_w2b.bufs.size() || r->cpu_data.empty()) return;
  w2b_buf_info& bi = g_w2b.bufs[h];
  if (!bi.gpu_buf) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)r->cpu_data.size();
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bi.is_index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_rt.dev->CreateBuffer(&bd, nullptr, &bi.gpu_buf);
    bi.dirty = true;
  }
  if (bi.gpu_buf && bi.dirty) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(g_rt.ctx->Map(bi.gpu_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      memcpy(ms.pData, r->cpu_data.data(), r->cpu_data.size());
      g_rt.ctx->Unmap(bi.gpu_buf, 0);
      bi.dirty = false;
    }
  }
}

inline void w2b_upload_shader_cb(w2b_shader& sh, bool is_ps) {
  if (!sh.cb || !sh.cb_bytes || !sh.parsed) return;
  const float* cf = is_ps ? g_w2b.ps_cf : g_w2b.vs_cf;
  const int32_t* ci = is_ps ? g_w2b.ps_ci : g_w2b.vs_ci;
  const int* cb = is_ps ? g_w2b.ps_cb : g_w2b.vs_cb;
  sh.cb_cpu.resize(sh.cb_bytes);
  trt_d3d9_mojo_pack_cb(sh.parsed, cf, ci, cb, sh.cb_cpu.data(), sh.cb_bytes);
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (SUCCEEDED(g_rt.ctx->Map(sh.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    memcpy(ms.pData, sh.cb_cpu.data(), sh.cb_bytes);
    g_rt.ctx->Unmap(sh.cb, 0);
  }
}

inline void w2b_apply_draw_state(uint32_t prim_type, bool is_rhw, bool custom_vs) {
  w2b_bind_rts_and_scissor();
  if (ID3D11RasterizerState* rs = w2b_get_rasterizer()) g_rt.ctx->RSSetState(rs);
  if (ID3D11DepthStencilState* dss = w2b_get_dss(is_rhw))
    g_rt.ctx->OMSetDepthStencilState(dss, 0);
  if (ID3D11BlendState* bs = w2b_get_bs()) {
    const float bf[4] = {1, 1, 1, 1};
    g_rt.ctx->OMSetBlendState(bs, bf, 0xFFFFFFFFu);
  }
  ID3D11ShaderResourceView* srvs[16] = {};
  ID3D11SamplerState* samps[16] = {};
  for (uint32_t i = 0; i < 16u; ++i) {
    const gpu_res* tr = get_res(g_w2b.bound_tex[i]);
    srvs[i] = (tr && tr->srv) ? tr->srv : (i == 0u ? g_rt.white_srv : nullptr);
    samps[i] = w2b_get_sampler(i);
  }
  g_rt.ctx->PSSetShaderResources(0, 16, srvs);
  g_rt.ctx->PSSetSamplers(0, 16, samps);
  g_rt.ctx->VSSetShaderResources(0, 4, srvs);
  g_rt.ctx->VSSetSamplers(0, 4, samps);

  if (custom_vs && g_w2b.bound_vs >= k_shader_base) {
    const uint32_t idx = g_w2b.bound_vs - k_shader_base;
    if (idx < g_w2b.shaders.size() && g_w2b.shaders[idx].vs) {
      w2b_shader& sh = g_w2b.shaders[idx];
      ID3D11InputLayout* il = w2b_get_input_layout(g_w2b.bound_vs);
      g_rt.ctx->IASetInputLayout(il ? il : g_rt.il);
      g_rt.ctx->VSSetShader(sh.vs, nullptr, 0);
      w2b_upload_shader_cb(sh, false);
      if (sh.cb) g_rt.ctx->VSSetConstantBuffers(0, 1, &sh.cb);
    }
  }
  if (g_w2b.bound_ps >= k_shader_base) {
    const uint32_t idx = g_w2b.bound_ps - k_shader_base;
    if (idx < g_w2b.shaders.size() && g_w2b.shaders[idx].ps) {
      w2b_shader& sh = g_w2b.shaders[idx];
      g_rt.ctx->PSSetShader(sh.ps, nullptr, 0);
      w2b_upload_shader_cb(sh, true);
      if (sh.cb) g_rt.ctx->PSSetConstantBuffers(0, 1, &sh.cb);
    }
  }
  (void)prim_type;
}

inline d3d9_handle w2b_create_tex_common(uint32_t kind, uint32_t w, uint32_t h,
                                         uint32_t depth_or_faces, uint32_t levels,
                                         uint32_t usage, uint32_t fmt) {
  if (!g_rt.initialized || !w || !h) return 0;
  const uint32_t id = alloc_res(1);
  gpu_res* r = &g_rt.res[id];
  r->w = w;
  r->h = h;
  r->fmt = fmt;
  uint32_t mips = levels;
  if (mips == 0) {
    uint32_t mx = w > h ? w : h;
    if (kind == 9 && depth_or_faces > mx) mx = depth_or_faces;
    mips = 1;
    while (mx > 1) { mx >>= 1; ++mips; }
  }
  r->mip_levels = mips;

  bool is_depth = false, expand = false;
  DXGI_FORMAT dsv_fmt = DXGI_FORMAT_UNKNOWN, srv_fmt = DXGI_FORMAT_UNKNOWN;
  const DXGI_FORMAT dfmt =
      w2b_resolve_tex_fmt(fmt, usage, &is_depth, &dsv_fmt, &srv_fmt, &expand);

  if (id >= g_w2b.texs.size()) g_w2b.texs.resize((size_t)id + 1u);
  w2b_tex_info& ti = g_w2b.texs[id];
  ti = {};
  ti.active = true;
  ti.kind = kind;
  ti.depth_or_faces = depth_or_faces ? depth_or_faces : 1u;
  ti.usage = usage;
  ti.is_depth = is_depth;
  ti.is_rt = (!is_depth && (usage & 1u) != 0u);
  ti.expand = expand;
  ti.dxgi_fmt = dfmt;

  const bool is_bcn = (fmt == 0x31545844u || fmt == 0x33545844u || fmt == 0x35545844u);
  UINT bind_flags = D3D11_BIND_SHADER_RESOURCE;
  if (is_depth) bind_flags |= D3D11_BIND_DEPTH_STENCIL;
  else if (ti.is_rt && !is_bcn) bind_flags |= D3D11_BIND_RENDER_TARGET;

  if (kind == 9) {
    D3D11_TEXTURE3D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.Depth = ti.depth_or_faces;
    td.MipLevels = mips;
    td.Format = dfmt;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_rt.dev->CreateTexture3D(&td, nullptr, &ti.tex3d))) {
      w2b_free_tex(id);
      free_res(r);
      return 0;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srv_fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    sd.Texture3D.MipLevels = mips;
    if (FAILED(g_rt.dev->CreateShaderResourceView(ti.tex3d, &sd, &r->srv))) {
      w2b_free_tex(id);
      free_res(r);
      return 0;
    }
    return id;
  }

  const bool is_cube = (kind == 8);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = mips;
  td.ArraySize = is_cube ? 6u : 1u;
  td.Format = dfmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = bind_flags;
  td.MiscFlags = is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
  if (FAILED(g_rt.dev->CreateTexture2D(&td, nullptr, &r->tex))) {
    // Fallback without SRV bind if driver refuses SRV on depth surface
    if (is_depth) {
      td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      td.Format = dsv_fmt;
      if (FAILED(g_rt.dev->CreateTexture2D(&td, nullptr, &r->tex))) {
        w2b_free_tex(id);
        free_res(r);
        return 0;
      }
    } else {
      w2b_free_tex(id);
      free_res(r);
      return 0;
    }
  }
  if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srv_fmt;
    sd.ViewDimension = is_cube ? D3D11_SRV_DIMENSION_TEXTURECUBE
                               : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (is_cube) sd.TextureCube.MipLevels = mips;
    else sd.Texture2D.MipLevels = mips;
    g_rt.dev->CreateShaderResourceView(r->tex, &sd, &r->srv);
  }
  if (is_depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = dsv_fmt;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_rt.dev->CreateDepthStencilView(r->tex, &dd, &ti.dsv);
  } else if (ti.is_rt && !is_bcn) {
    g_rt.dev->CreateRenderTargetView(r->tex, nullptr, &ti.rtv);
  }
  if (trt_esram_is_active() && (td.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
    trt_esram_qualify_and_alloc(td.Width, td.Height, (uint32_t)td.Format, td.BindFlags, "d3d9_surface");
  }
  return id;
}

// ---- Wave 2B d3d9_hooks callbacks ----
inline d3d9_handle hk_create_tex_ex(u32 kind, u32 w, u32 h, u32 depth_or_faces,
                                    u32 mip_levels, u32 usage, u32 fmt, void*) {
  return w2b_create_tex_common(kind, w, h, depth_or_faces, mip_levels, usage, fmt);
}

inline void hk_unlock_sub(d3d9_handle h, u32 face_or_slice, u32 level,
                          u32 row_pitch, u32 slice_pitch, const void* bits,
                          u32 bytes, void*) {
  (void)bytes;
  if (!g_rt.initialized || !bits) return;
  gpu_res* r = get_res(h);
  if (!r || level >= r->mip_levels) return;
  const w2b_tex_info* ti = (h < g_w2b.texs.size()) ? &g_w2b.texs[h] : nullptr;
  const uint32_t lw = mip_dim(r->w, level);
  const uint32_t lh = mip_dim(r->h, level);
  if (ti && ti->kind == 9 && ti->tex3d) {
    g_rt.ctx->UpdateSubresource(ti->tex3d, level, nullptr, bits,
                                (UINT)row_pitch, (UINT)slice_pitch);
    return;
  }
  if (!r->tex) return;
  const UINT subres = (ti && ti->kind == 8)
                          ? D3D11CalcSubresource(level, face_or_slice, r->mip_levels)
                          : level;
  if (ti && ti->expand) {
    w2b_expand_pixels(r->fmt, lw, lh, (const uint8_t*)bits, row_pitch, r->scratch);
    g_rt.ctx->UpdateSubresource(r->tex, subres, nullptr, r->scratch.data(),
                                lw * 4u, 0);
  } else {
    g_rt.ctx->UpdateSubresource(r->tex, subres, nullptr, bits, (UINT)row_pitch, 0);
  }
}

inline d3d9_handle hk_create_shader(u32 is_ps, u32, u32, const char* hlsl,
                                    u32 hlsl_len, const void* parsed, void*) {
  if (!g_rt.initialized || !hlsl || !hlsl_len) return 0;
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  const char* profile5 = is_ps ? "ps_5_0" : "vs_5_0";
  if (!longan::d3d_compile_cached(hlsl, hlsl_len, profile5, "main", &blob) || !blob) {
    HRESULT hr = D3DCompile(hlsl, hlsl_len, nullptr, nullptr, nullptr, "main",
                            is_ps ? "ps_4_0" : "vs_4_0", 0, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
      if (err) err->Release();
      if (blob) blob->Release();
      return 0;
    }
  }
  if (err) err->Release();

  w2b_shader sh{};
  sh.alive = true;
  sh.is_ps = is_ps;
  sh.parsed = parsed;
  if (is_ps) {
    if (FAILED(g_rt.dev->CreatePixelShader(blob->GetBufferPointer(),
                                           blob->GetBufferSize(), nullptr, &sh.ps))) {
      blob->Release();
      return 0;
    }
  } else {
    if (FAILED(g_rt.dev->CreateVertexShader(blob->GetBufferPointer(),
                                            blob->GetBufferSize(), nullptr, &sh.vs))) {
      blob->Release();
      return 0;
    }
    const uint8_t* p = (const uint8_t*)blob->GetBufferPointer();
    sh.vs_blob.assign(p, p + blob->GetBufferSize());
  }
  blob->Release();

  sh.cb_bytes = trt_d3d9_mojo_cb_info(parsed, &sh.sampler_mask);
  if (sh.cb_bytes > 0u) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (sh.cb_bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_rt.dev->CreateBuffer(&bd, nullptr, &sh.cb);
  }
  g_w2b.shaders.push_back(std::move(sh));
  return k_shader_base + (uint32_t)(g_w2b.shaders.size() - 1u);
}

inline void hk_set_shader(u32 is_ps, d3d9_handle shader, void*) {
  if (is_ps) g_w2b.bound_ps = shader;
  else g_w2b.bound_vs = shader;
}

inline void hk_set_shader_const_f(u32 is_ps, u32 start, const float* data,
                                  u32 count, void*) {
  if (!data || start >= 512u) return;
  const u32 n = (start + count <= 512u) ? count : (512u - start);
  float* dst = is_ps ? g_w2b.ps_cf : g_w2b.vs_cf;
  memcpy(dst + start * 4u, data, n * 16u);
}

inline void hk_set_shader_const_i(u32 is_ps, u32 start, const i32* data,
                                  u32 count, void*) {
  if (!data || start >= 32u) return;
  const u32 n = (start + count <= 32u) ? count : (32u - start);
  int32_t* dst = is_ps ? g_w2b.ps_ci : g_w2b.vs_ci;
  memcpy(dst + start * 4u, data, n * 16u);
}

inline void hk_set_shader_const_b(u32 is_ps, u32 start, const int* data,
                                  u32 count, void*) {
  if (!data || start >= 32u) return;
  const u32 n = (start + count <= 32u) ? count : (32u - start);
  int* dst = is_ps ? g_w2b.ps_cb : g_w2b.vs_cb;
  memcpy(dst + start, data, n * sizeof(int));
}

inline d3d9_handle hk_create_decl(const void* elems_v, u32 count, void*) {
  const d3d9_vert_elem* elems = (const d3d9_vert_elem*)elems_v;
  if (!elems || !count) return 0;
  w2b_decl d{};
  d.alive = true;
  d.elems.assign(elems, elems + count);
  g_w2b.decls.push_back(std::move(d));
  return k_decl_base + (uint32_t)(g_w2b.decls.size() - 1u);
}

inline void hk_set_decl(d3d9_handle decl, void*) { g_w2b.bound_decl = decl; }

inline void hk_set_render_target(u32 index, d3d9_handle rt, void*) {
  if (index < 4u) g_w2b.cur_rt[index] = rt;
}

inline void hk_set_depth_stencil(d3d9_handle ds, void*) { g_w2b.cur_ds = ds; }

inline void hk_set_scissor(i32 enable, i32 left, i32 top, i32 right, i32 bottom,
                           void*) {
  g_w2b.scissor_enable = (enable != 0);
  g_w2b.scissor_rect[0] = left;
  g_w2b.scissor_rect[1] = top;
  g_w2b.scissor_rect[2] = right;
  g_w2b.scissor_rect[3] = bottom;
}

inline void hk_copy_rect(d3d9_handle dst_tex, u32 dst_level, i32 dx, i32 dy,
                         d3d9_handle src_tex, u32 src_level, i32 sx, i32 sy,
                         i32 sw, i32 sh, void*) {
  if (!g_rt.initialized || sw <= 0 || sh <= 0) return;
  ID3D11Texture2D* src = nullptr;
  ID3D11Texture2D* dst = nullptr;
  bool src_bb = false, dst_bb = false;
  if (src_tex == 0u) {
    if (g_rt.swap && SUCCEEDED(g_rt.swap->GetBuffer(0, IID_PPV_ARGS(&src))))
      src_bb = true;
  } else if (const gpu_res* sr = get_res(src_tex)) {
    src = sr->tex;
  }
  if (dst_tex == 0u) {
    if (g_rt.swap && SUCCEEDED(g_rt.swap->GetBuffer(0, IID_PPV_ARGS(&dst))))
      dst_bb = true;
  } else if (const gpu_res* dr = get_res(dst_tex)) {
    dst = dr->tex;
  }
  if (src && dst) {
    D3D11_BOX box{(UINT)(sx > 0 ? sx : 0), (UINT)(sy > 0 ? sy : 0), 0,
                  (UINT)(sx + sw), (UINT)(sy + sh), 1};
    g_rt.ctx->CopySubresourceRegion(dst, dst_level, (UINT)(dx > 0 ? dx : 0),
                                    (UINT)(dy > 0 ? dy : 0), 0, src, src_level,
                                    &box);
  }
  if (src_bb && src) src->Release();
  if (dst_bb && dst) dst->Release();
}

inline void hk_fill_rect(d3d9_handle surf, u32 level, i32 rx, i32 ry, i32 rw,
                         i32 rh, u32 color, void*) {
  (void)level;
  if (!g_rt.initialized || rw <= 0 || rh <= 0) return;
  ID3D11RenderTargetView* rtv = nullptr;
  if (surf == 0u) rtv = g_rt.rtv;
  else if (surf < g_w2b.texs.size()) rtv = g_w2b.texs[surf].rtv;
  if (!rtv) return;
  const float rgba[4] = {
      ((color >> 16) & 0xFFu) / 255.0f,
      ((color >> 8) & 0xFFu) / 255.0f,
      (color & 0xFFu) / 255.0f,
      ((color >> 24) & 0xFFu) / 255.0f,
  };
  D3D11_RECT rc{rx, ry, rx + rw, ry + rh};
  g_rt.ctx->ClearView(rtv, rgba, &rc, 1);
}

inline void hk_set_index_fmt(d3d9_handle ib, u32 idx32, void*) {
  if (!ib) return;
  if (ib >= g_w2b.bufs.size()) g_w2b.bufs.resize((size_t)ib + 1u);
  g_w2b.bufs[ib].idx32 = (idx32 != 0u);
}

// ---- d3d9_hooks callbacks ----
inline d3d9_handle hk_create_tex(u32 w, u32 h, u32 levels, u32 usage, u32 fmt,
                                 void*) {
  return w2b_create_tex_common(1u, w, h, 1u, levels, usage, fmt);
}

inline d3d9_handle hk_create_buf(u32 bytes, u32 is_index, void*) {
  if (!g_rt.initialized || !bytes) return 0;
  const uint32_t id = alloc_res(is_index ? 3u : 2u);
  g_rt.res[id].bytes = bytes;
  g_rt.res[id].cpu_data.resize(bytes, 0);
  if (id >= g_w2b.bufs.size()) g_w2b.bufs.resize((size_t)id + 1u);
  g_w2b.bufs[id].active = true;
  g_w2b.bufs[id].is_index = (is_index != 0u);
  g_w2b.bufs[id].bytes = bytes;
  g_w2b.bufs[id].dirty = true;
  return id;
}

inline void* hk_lock(d3d9_handle h, u32 level, u32 offset, u32 bytes, void*) {
  (void)bytes;
  gpu_res* r = get_res(h);
  if (!r) return nullptr;
  if (r->kind == 2 || r->kind == 3) {
    if (offset >= r->cpu_data.size()) return nullptr;
    if (h < g_w2b.bufs.size()) g_w2b.bufs[h].dirty = true;
    return r->cpu_data.data() + offset;
  }
  if (r->kind == 1) {
    // Let cubemaps and 3D volumes flow through tex_LockRect -> unlock_sub with face/slice
    if (h < g_w2b.texs.size() && g_w2b.texs[h].kind != 1u) return nullptr;
    const uint32_t lw = mip_dim(r->w, level);
    const uint32_t lh = mip_dim(r->h, level);
    r->lock_level = level;
    const i32 pitch = trt_d3d9_row_pitch(lw, r->fmt);
    uint32_t rows = lh;
    if (r->fmt == 0x31545844u || r->fmt == 0x33545844u || r->fmt == 0x35545844u)
      rows = (lh + 3u) / 4u;
    r->cpu_data.resize((size_t)pitch * rows);
    return r->cpu_data.data();
  }
  return nullptr;
}

inline void hk_unlock(d3d9_handle h, void*) {
  gpu_res* r = get_res(h);
  if (!r) return;
  if (r->kind == 2 || r->kind == 3) {
    if (h < g_w2b.bufs.size()) g_w2b.bufs[h].dirty = true;
    return;
  }
  if (r->kind != 1 || !r->tex || r->cpu_data.empty()) return;
  if (r->lock_level >= r->mip_levels) return;
  const uint32_t lw = mip_dim(r->w, r->lock_level);
  const uint32_t lh = mip_dim(r->h, r->lock_level);
  bool expand = false;
  map_dxgi_fmt(r->fmt, &expand);
  if (!expand) {
    const i32 pitch = trt_d3d9_row_pitch(lw, r->fmt);
    g_rt.ctx->UpdateSubresource(r->tex, r->lock_level, nullptr,
                                r->cpu_data.data(), (UINT)pitch, 0);
    return;
  }
  w2b_expand_pixels(r->fmt, lw, lh, r->cpu_data.data(),
                    (uint32_t)trt_d3d9_row_pitch(lw, r->fmt), r->scratch);
  g_rt.ctx->UpdateSubresource(r->tex, r->lock_level, nullptr,
                              r->scratch.data(), lw * 4, 0);
}

inline void hk_release(d3d9_handle h, void*) {
  if (h >= k_decl_base) {
    const uint32_t idx = h - k_decl_base;
    if (idx < g_w2b.decls.size()) g_w2b.decls[idx] = {};
    for (size_t i = g_w2b.ils.size(); i > 0; --i) {
      if ((uint32_t)(g_w2b.ils[i - 1].key >> 32) == h) {
        if (g_w2b.ils[i - 1].il) g_w2b.ils[i - 1].il->Release();
        g_w2b.ils.erase(g_w2b.ils.begin() + (ptrdiff_t)(i - 1));
      }
    }
    return;
  }
  if (h >= k_shader_base) {
    const uint32_t idx = h - k_shader_base;
    if (idx < g_w2b.shaders.size()) {
      auto& sh = g_w2b.shaders[idx];
      if (sh.vs) sh.vs->Release();
      if (sh.ps) sh.ps->Release();
      if (sh.cb) sh.cb->Release();
      sh = {};
    }
    for (size_t i = g_w2b.ils.size(); i > 0; --i) {
      if ((uint32_t)(g_w2b.ils[i - 1].key & 0xffffffffu) == h) {
        if (g_w2b.ils[i - 1].il) g_w2b.ils[i - 1].il->Release();
        g_w2b.ils.erase(g_w2b.ils.begin() + (ptrdiff_t)(i - 1));
      }
    }
    return;
  }
  w2b_free_tex(h);
  w2b_free_buf(h);
  free_res(get_res(h));
}

inline void hk_clear(u32 flags, u32 color, float z, u32 stencil, void*) {
  if (!g_rt.initialized) return;
  w2b_bind_rts_and_scissor();
  if (flags & 1u) {
    const float rgba[4] = {
        ((color >> 16) & 0xFFu) / 255.0f,
        ((color >> 8) & 0xFFu) / 255.0f,
        (color & 0xFFu) / 255.0f,
        ((color >> 24) & 0xFFu) / 255.0f,
    };
    for (uint32_t i = 0; i < 4u; ++i) {
      ID3D11RenderTargetView* rt = nullptr;
      const d3d9_handle h = g_w2b.cur_rt[i];
      if (h == 0u) rt = (i == 0u) ? g_rt.rtv : nullptr;
      else if (h < g_w2b.texs.size()) rt = g_w2b.texs[h].rtv;
      if (rt) g_rt.ctx->ClearRenderTargetView(rt, rgba);
    }
  }
  if (flags & 6u) {
    ID3D11DepthStencilView* dsv = g_rt.dsv;
    if (g_w2b.cur_ds != 0u && g_w2b.cur_ds < g_w2b.texs.size() &&
        g_w2b.texs[g_w2b.cur_ds].dsv) {
      dsv = g_w2b.texs[g_w2b.cur_ds].dsv;
    }
    if (dsv) {
      UINT dflags = 0;
      if (flags & 2u) dflags |= D3D11_CLEAR_DEPTH;
      if (flags & 4u) dflags |= D3D11_CLEAR_STENCIL;
      g_rt.ctx->ClearDepthStencilView(dsv, dflags, z, (UINT8)stencil);
    }
  }
}

inline void hk_present(void*) {
  if (!g_rt.initialized || !g_rt.swap) return;
  longan::draw_pointer(false);
  g_rt.swap->Present(1, 0);
}

inline void hk_set_viewport(const d3d9_viewport* vp, void*) {
  if (vp) g_rt.vp = *vp;
}
inline void hk_set_texture(u32 stage, d3d9_handle h, void*) {
  if (stage < 16u) g_w2b.bound_tex[stage] = h;
  if (stage == 0) g_rt.bound_tex0 = h;
}
inline void hk_set_transform(u32 kind, const float* m4x4, void*) {
  if (!m4x4) return;
  if (kind == 256 || kind == 1) memcpy(g_rt.world, m4x4, 64);
  else if (kind == 2) memcpy(g_rt.view, m4x4, 64);
  else if (kind == 3) memcpy(g_rt.proj, m4x4, 64);
}
inline void hk_set_fvf(u32 fvf, void*) { g_rt.fvf = fvf; }
inline void hk_set_rs(u32 state, u32 value, void*) {
  if (state == 7) g_rt.z_enable = (value != 0);
  else if (state == 8) g_w2b.fill_mode = value;
  else if (state == 14) g_rt.z_write = (value != 0);
  else if (state == 19) g_rt.src_blend = value;
  else if (state == 20) g_rt.dst_blend = value;
  else if (state == 22) g_rt.cull_mode = value;
  else if (state == 23) g_w2b.z_func = value;
  else if (state == 27) g_rt.alpha_blend = (value != 0);
  else if (state == 52) g_w2b.stencil_en = (value != 0);
  else if (state == 168) g_w2b.color_write = value & 0xFu;
  else if (state == 171) g_w2b.blend_op = value;
  else if (state == 206) g_w2b.sep_alpha = (value != 0);
  else if (state == 207) g_w2b.src_blend_a = value;
  else if (state == 208) g_w2b.dst_blend_a = value;
}
inline void hk_set_sampler(u32 stage, u32 state, u32 value, void*) {
  if (stage < 16u && state < 16u) g_w2b.samp[stage][state] = value;
}
inline void hk_set_stream_stride(u32 stream, u32 stride, void*) {
  if (stream < 2) g_rt.stream_stride[stream] = stride;
}

inline void hk_draw(u32 prim, d3d9_handle vb, d3d9_handle ib, u32 base_idx,
                    u32 min_idx, u32 num_verts, u32 start_idx,
                    u32 prim_count, void*) {
  if (!g_rt.initialized || !prim_count) return;
  const bool custom_vs = (g_w2b.bound_vs >= k_shader_base);
  const bool is_rhw = !custom_vs && ((g_rt.fvf & 0x0Eu) == 0x04u);

  if (custom_vs) {
    apply_pipeline_state(prim, false);
    w2b_apply_draw_state(prim, false, true);
    ID3D11Buffer* vbs[8] = {};
    UINT strides[8] = {}, offsets[8] = {};
    for (u32 s = 0; s < 8u; ++s) {
      d3d9_handle svb = 0;
      u32 soff = 0, sstr = 0, sfreq = 0;
      if (trt_d3d9_get_stream(s, &svb, &soff, &sstr, &sfreq) && svb) {
        w2b_ensure_gpu_buf(svb);
        if (svb < g_w2b.bufs.size()) vbs[s] = g_w2b.bufs[svb].gpu_buf;
        strides[s] = sstr;
        offsets[s] = soff;
      }
    }
    if (!vbs[0] && vb) {
      w2b_ensure_gpu_buf(vb);
      if (vb < g_w2b.bufs.size()) vbs[0] = g_w2b.bufs[vb].gpu_buf;
      strides[0] = g_rt.stream_stride[0];
    }
    g_rt.ctx->IASetVertexBuffers(0, 8, vbs, strides, offsets);
    const gpu_res* ires = get_res(ib);
    const uint32_t icount = trt_d3d9_prim_verts(prim, prim_count);
    if (ires && !ires->cpu_data.empty()) {
      w2b_ensure_gpu_buf(ib);
      const bool idx32 = (ib < g_w2b.bufs.size()) ? g_w2b.bufs[ib].idx32 : false;
      ID3D11Buffer* gib = (ib < g_w2b.bufs.size()) ? g_w2b.bufs[ib].gpu_buf : nullptr;
      if (gib) {
        g_rt.ctx->IASetIndexBuffer(
            gib, idx32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
        g_rt.ctx->DrawIndexed(icount, start_idx, (INT)base_idx);
      }
    } else if (prim == 6u) {
      g_w2b.fan_ib.resize((size_t)prim_count * 3u);
      for (uint32_t i = 0; i < prim_count; ++i) {
        g_w2b.fan_ib[i * 3u + 0u] = min_idx;
        g_w2b.fan_ib[i * 3u + 1u] = min_idx + i + 1u;
        g_w2b.fan_ib[i * 3u + 2u] = min_idx + i + 2u;
      }
      const uint32_t ibytes = (uint32_t)(g_w2b.fan_ib.size() * 4u);
      ensure_dyn_ib(ibytes);
      D3D11_MAPPED_SUBRESOURCE mi{};
      if (g_rt.dyn_ib &&
          SUCCEEDED(g_rt.ctx->Map(g_rt.dyn_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mi))) {
        memcpy(mi.pData, g_w2b.fan_ib.data(), ibytes);
        g_rt.ctx->Unmap(g_rt.dyn_ib, 0);
        g_rt.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_rt.ctx->IASetIndexBuffer(g_rt.dyn_ib, DXGI_FORMAT_R32_UINT, 0);
        g_rt.ctx->DrawIndexed(prim_count * 3u, 0, 0);
      }
    } else {
      g_rt.ctx->Draw(icount, min_idx);
    }
    return;
  }

  const gpu_res* vres = get_res(vb);
  if (!vres || vres->cpu_data.empty()) return;
  const uint32_t stride = g_rt.stream_stride[0];
  const uint32_t count = min_idx + num_verts;
  if (!count) return;
  decode_fvf_vertices(vres->cpu_data.data(), (uint32_t)vres->cpu_data.size(),
                      count, stride, g_rt.fvf);
  apply_pipeline_state(prim, is_rhw);
  w2b_apply_draw_state(prim, is_rhw, false);

  const gpu_res* ires = get_res(ib);
  if (ires && !ires->cpu_data.empty()) {
    const uint32_t idx32 = (ib < g_w2b.bufs.size() && g_w2b.bufs[ib].idx32) ? 1u : 0u;
    const uint32_t elem_sz = idx32 ? 4u : 2u;
    const uint32_t icount = trt_d3d9_prim_verts(prim, prim_count);
    const size_t need_bytes = ((size_t)start_idx + (size_t)icount) * (size_t)elem_sz;
    if (need_bytes <= ires->cpu_data.size()) {
      submit_verts_and_draw(prim, ires->cpu_data.data(), idx32, start_idx,
                            (int32_t)base_idx, prim_count);
    }
  } else {
    submit_verts_and_draw(prim, nullptr, 0, 0, 0, prim_count);
  }
}

inline void hk_draw_up(u32 prim, const void* verts, u32 vstride, u32 num_verts,
                       const void* indices, u32 index_fmt32, u32 index_count,
                       u32 prim_count, void*) {
  if (!g_rt.initialized || !verts || !vstride || !num_verts || !prim_count)
    return;
  (void)index_count;
  const bool custom_vs = (g_w2b.bound_vs >= k_shader_base);
  const bool is_rhw = !custom_vs && ((g_rt.fvf & 0x0Eu) == 0x04u);

  if (custom_vs) {
    apply_pipeline_state(prim, false);
    w2b_apply_draw_state(prim, false, true);
    const uint32_t vbytes = num_verts * vstride;
    ensure_dyn_vb(vbytes);
    if (!g_rt.dyn_vb) return;
    D3D11_MAPPED_SUBRESOURCE mv{};
    if (FAILED(g_rt.ctx->Map(g_rt.dyn_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mv)))
      return;
    memcpy(mv.pData, verts, vbytes);
    g_rt.ctx->Unmap(g_rt.dyn_vb, 0);
    const UINT stride = (UINT)vstride, offset = 0;
    g_rt.ctx->IASetVertexBuffers(0, 1, &g_rt.dyn_vb, &stride, &offset);
    const uint32_t icount = trt_d3d9_prim_verts(prim, prim_count);
    if (indices) {
      const uint32_t ibytes = icount * (index_fmt32 ? 4u : 2u);
      ensure_dyn_ib(ibytes);
      D3D11_MAPPED_SUBRESOURCE mi{};
      if (g_rt.dyn_ib &&
          SUCCEEDED(g_rt.ctx->Map(g_rt.dyn_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mi))) {
        memcpy(mi.pData, indices, ibytes);
        g_rt.ctx->Unmap(g_rt.dyn_ib, 0);
        g_rt.ctx->IASetIndexBuffer(
            g_rt.dyn_ib, index_fmt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
        g_rt.ctx->DrawIndexed(icount, 0, 0);
      }
    } else if (prim == 6u) {
      g_w2b.fan_ib.resize((size_t)prim_count * 3u);
      for (uint32_t i = 0; i < prim_count; ++i) {
        g_w2b.fan_ib[i * 3u + 0u] = 0u;
        g_w2b.fan_ib[i * 3u + 1u] = i + 1u;
        g_w2b.fan_ib[i * 3u + 2u] = i + 2u;
      }
      const uint32_t ibytes = (uint32_t)(g_w2b.fan_ib.size() * 4u);
      ensure_dyn_ib(ibytes);
      D3D11_MAPPED_SUBRESOURCE mi{};
      if (g_rt.dyn_ib &&
          SUCCEEDED(g_rt.ctx->Map(g_rt.dyn_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mi))) {
        memcpy(mi.pData, g_w2b.fan_ib.data(), ibytes);
        g_rt.ctx->Unmap(g_rt.dyn_ib, 0);
        g_rt.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_rt.ctx->IASetIndexBuffer(g_rt.dyn_ib, DXGI_FORMAT_R32_UINT, 0);
        g_rt.ctx->DrawIndexed(prim_count * 3u, 0, 0);
      }
    } else {
      g_rt.ctx->Draw(icount, 0);
    }
    return;
  }

  decode_fvf_vertices((const uint8_t*)verts, num_verts * vstride, num_verts,
                      vstride, g_rt.fvf);
  apply_pipeline_state(prim, is_rhw);
  w2b_apply_draw_state(prim, is_rhw, false);
  submit_verts_and_draw(prim, indices, index_fmt32, 0, 0, prim_count);
}

inline void shutdown() {
  w2b_shutdown();
  for (size_t i = 1; i < g_rt.res.size(); ++i) free_res(&g_rt.res[i]);
  g_rt.res.clear();
  auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
  rel(g_rt.bs_custom);
  rel(g_rt.bs_opaque);
  rel(g_rt.bs_alpha);
  rel(g_rt.bs_add);
  rel(g_rt.dss_rw);
  rel(g_rt.dss_ro);
  rel(g_rt.dss_off);
  rel(g_rt.rs_none);
  rel(g_rt.rs_cw);
  rel(g_rt.rs_ccw);
  rel(g_rt.white_srv);
  rel(g_rt.white_tex);
  rel(g_rt.sampler);
  rel(g_rt.dyn_ib);
  rel(g_rt.dyn_vb);
  rel(g_rt.cb);
  rel(g_rt.il);
  rel(g_rt.ps);
  rel(g_rt.vs);
  rel(g_rt.dsv);
  rel(g_rt.depth_tex);
  g_rt.dev = nullptr;
  g_rt.ctx = nullptr;
  g_rt.swap = nullptr;
  g_rt.rtv = nullptr;
  g_rt.initialized = false;
}

inline void bind(ID3D11Device1* dev, ID3D11DeviceContext1* ctx,
                 IDXGISwapChain1* swap, ID3D11RenderTargetView* rtv,
                 uint32_t w, uint32_t h) {
  shutdown();
  if (!dev || !ctx || !swap || !rtv) return;
  g_rt.dev = dev;
  g_rt.ctx = ctx;
  g_rt.swap = swap;
  g_rt.rtv = rtv;
  g_rt.sc_w = w ? w : 1280;
  g_rt.sc_h = h ? h : 720;
  mat_identity(g_rt.world);
  mat_identity(g_rt.view);
  mat_identity(g_rt.proj);
  g_rt.vp = {0, 0, g_rt.sc_w, g_rt.sc_h, 0.0f, 1.0f};

  static const char* k_vs_src = R"(
cbuffer CB0 : register(b0) {
  row_major float4x4 mvp;
  float4 vp;
  float4 flags;
};
struct VSIn {
  float4 pos : POSITION;
  float4 col : COLOR;
  float2 uv  : TEXCOORD0;
};
struct VSOut {
  float4 pos : SV_POSITION;
  float4 col : COLOR;
  float2 uv  : TEXCOORD0;
};
VSOut main(VSIn i) {
  VSOut o;
  if (flags.x > 0.5) {
    float x = ((i.pos.x - vp.x) / vp.z) * 2.0 - 1.0;
    float y = 1.0 - ((i.pos.y - vp.y) / vp.w) * 2.0;
    o.pos = float4(x, y, saturate(i.pos.z), 1.0);
  } else {
    o.pos = mul(float4(i.pos.xyz, 1.0), mvp);
  }
  o.col = i.col;
  o.uv = i.uv;
  return o;
})";
  static const char* k_ps_src = R"(
cbuffer CB0 : register(b0) {
  row_major float4x4 mvp;
  float4 vp;
  float4 flags;
};
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
struct VSOut {
  float4 pos : SV_POSITION;
  float4 col : COLOR;
  float2 uv  : TEXCOORD0;
};
float4 main(VSOut i) : SV_TARGET {
  float4 c = i.col;
  if (flags.y > 0.5) c *= tex0.Sample(samp0, i.uv);
  return c;
})";

  ID3DBlob* vsb = nullptr;
  ID3DBlob* psb = nullptr;
  if (FAILED(D3DCompile(k_vs_src, strlen(k_vs_src), nullptr, nullptr, nullptr,
                        "main", "vs_4_0", 0, 0, &vsb, nullptr)) ||
      FAILED(D3DCompile(k_ps_src, strlen(k_ps_src), nullptr, nullptr, nullptr,
                        "main", "ps_4_0", 0, 0, &psb, nullptr))) {
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    shutdown();
    return;
  }
  dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                          nullptr, &g_rt.vs);
  dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                         nullptr, &g_rt.ps);
  const D3D11_INPUT_ELEMENT_DESC ied[3] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  dev->CreateInputLayout(ied, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(),
                         &g_rt.il);
  vsb->Release();
  psb->Release();

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(cb_data);
  cbd.Usage = D3D11_USAGE_DEFAULT;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  dev->CreateBuffer(&cbd, nullptr, &g_rt.cb);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sd, &g_rt.sampler);

  const uint32_t white = 0xFFFFFFFFu;
  D3D11_TEXTURE2D_DESC wtd{};
  wtd.Width = 1; wtd.Height = 1; wtd.MipLevels = 1; wtd.ArraySize = 1;
  wtd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  wtd.SampleDesc.Count = 1;
  wtd.Usage = D3D11_USAGE_IMMUTABLE;
  wtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA wsr{&white, 4, 0};
  if (SUCCEEDED(dev->CreateTexture2D(&wtd, &wsr, &g_rt.white_tex)))
    dev->CreateShaderResourceView(g_rt.white_tex, nullptr, &g_rt.white_srv);

  D3D11_TEXTURE2D_DESC dtd{};
  dtd.Width = g_rt.sc_w; dtd.Height = g_rt.sc_h;
  dtd.MipLevels = 1; dtd.ArraySize = 1;
  dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dtd.SampleDesc.Count = 1;
  dtd.Usage = D3D11_USAGE_DEFAULT;
  dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, &g_rt.depth_tex)))
    dev->CreateDepthStencilView(g_rt.depth_tex, nullptr, &g_rt.dsv);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bd, &g_rt.bs_opaque);
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  dev->CreateBlendState(&bd, &g_rt.bs_alpha);
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  dev->CreateBlendState(&bd, &g_rt.bs_add);

  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dev->CreateDepthStencilState(&dsd, &g_rt.dss_rw);
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dev->CreateDepthStencilState(&dsd, &g_rt.dss_ro);
  dsd.DepthEnable = FALSE;
  dev->CreateDepthStencilState(&dsd, &g_rt.dss_off);

  D3D11_RASTERIZER_DESC rsd{};
  rsd.FillMode = D3D11_FILL_SOLID;
  rsd.DepthClipEnable = TRUE;
  rsd.CullMode = D3D11_CULL_NONE;
  dev->CreateRasterizerState(&rsd, &g_rt.rs_none);
  rsd.CullMode = D3D11_CULL_BACK;
  rsd.FrontCounterClockwise = FALSE;
  dev->CreateRasterizerState(&rsd, &g_rt.rs_ccw);
  rsd.FrontCounterClockwise = TRUE;
  dev->CreateRasterizerState(&rsd, &g_rt.rs_cw);

  g_rt.initialized = true;

  d3d9_hooks hk{};
  hk.width = g_rt.sc_w;
  hk.height = g_rt.sc_h;
  hk.create_tex = &hk_create_tex;
  hk.create_buf = &hk_create_buf;
  hk.lock = &hk_lock;
  hk.unlock = &hk_unlock;
  hk.release = &hk_release;
  hk.clear = &hk_clear;
  hk.present = &hk_present;
  hk.set_viewport = &hk_set_viewport;
  hk.set_texture = &hk_set_texture;
  hk.set_transform = &hk_set_transform;
  hk.set_fvf = &hk_set_fvf;
  hk.set_rs = &hk_set_rs;
  hk.set_sampler = &hk_set_sampler;
  w2b_init_defaults();
  hk.draw = &hk_draw;
  hk.user = &g_rt;
  hk.set_stream_stride = &hk_set_stream_stride;
  hk.draw_up = &hk_draw_up;
  hk.create_shader = &hk_create_shader;
  hk.set_shader = &hk_set_shader;
  hk.set_shader_const_f = &hk_set_shader_const_f;
  hk.set_shader_const_i = &hk_set_shader_const_i;
  hk.set_shader_const_b = &hk_set_shader_const_b;
  hk.create_decl = &hk_create_decl;
  hk.set_decl = &hk_set_decl;
  hk.create_tex_ex = &hk_create_tex_ex;
  hk.unlock_sub = &hk_unlock_sub;
  hk.set_render_target = &hk_set_render_target;
  hk.set_depth_stencil = &hk_set_depth_stencil;
  hk.set_scissor = &hk_set_scissor;
  hk.copy_rect = &hk_copy_rect;
  hk.fill_rect = &hk_fill_rect;
  hk.set_index_fmt = &hk_set_index_fmt;
  trt_d3d9_set_hooks(&hk);
}

}  // namespace d3d9_rt
}  // namespace longan

#endif
