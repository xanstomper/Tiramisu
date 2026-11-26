// tiramisu/src/trt/bridge_d3d9.cpp
// The D3D9 bridge (M5). See bridge_d3d9.h for the design: SDK-exact COM
// vtables, plain-C host hooks, fail-visible stubs.
//
// Vtable slot orders verified against the Windows SDK's d3d9.h:
//   IDirect3D9            = 17 slots (CreateDevice = 16)
//   IDirect3DDevice9      = 119 slots (Present = 17, CreateTexture = 23,
//                             SetRenderState = 57, DrawIndexedPrimitive = 82,
//                             DrawPrimitiveUP = 83, SetStreamSource = 100)
//   IDirect3D9Ex          = 22 slots (base 17 + GetAdapterModeCountEx = 17,
//                             EnumAdapterModesEx = 18, GetAdapterDisplayModeEx
//                             = 19, CreateDeviceEx = 20, GetAdapterLUID = 21)
//   IDirect3DDevice9Ex    = 133 slots (base 119 + SetConvolutionMonoKernel =
//                             119, ComposeRects = 120, PresentEx = 121,
//                             Get/SetGPUThreadPriority = 122/123, WaitForVBlank
//                             = 124, CheckResourceResidency = 125,
//                             SetMaximumFrameLatency = 126, CheckDeviceState =
//                             127, CreateRenderTargetEx = 128,
//                             CreateOffscreenPlainSurfaceEx = 129,
//                             CreateDepthStencilSurfaceEx = 130, ResetEx = 131,
//                             GetDisplayModeEx = 132)
// XNA 4.0 (Celeste) uses the Ex interfaces on Vista+ — without them it calls
// CreateDeviceEx at slot 20 of a 17-slot vtable and reads past the end.
//   IDirect3DTexture9     = 22 slots (LockRect = 19, UnlockRect = 20)
//   IDirect3DVertexBuffer9 / IndexBuffer9 = 13 (Lock = 11, Unlock = 12)
//   IDirect3DSurface9 (Resource9) = 11
//   Vertex/PixelShader9, VertexDeclaration9 = 5
//
// Semantics follow Microsoft's D3D9On12 (MIT): D3DRS_* render-state values
// pass through verbatim; the host backend maps them to D3D11, just as
// 9on12 maps them to D3D12.
#include "bridge_d3d9.h"
#include "bridge_d3d9_meta.h"
#include "trt.h"
#include "thunk.h"
#include "../../third_party/mojoshader/mojoshader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

constexpr int D3D_OK_ = 0;
constexpr int D3DERR_NOTFOUND_ = (int)0x88760866;
constexpr int D3DERR_NOTIMPL_ = (int)0x8876086A;
constexpr int D3DERR_INVALIDCALL_ = (int)0x8876086C;

// ---- hooks ----
d3d9_hooks g_hooks;

// self-test observation: (prim_type<<16)|prim_count of the last draw
u32 g_last_draw = 0;

// ---- shared COM header: every object starts with this ----
struct d9_obj {
  const void** vtbl;
  u32 refcount;
  u32 handle;     // GPU object this wraps (0 = pure interface / sub-view)
  u32 kind;       // 1 tex, 2 vb, 3 ib, 4 surf/rt/ds, 5 vs, 6 ps, 7 decl, 8 cube, 9 voltex, 10 vol, 11 sb
  u32 w, h, fmt;
  u32 sub_face;
  u32 sub_level;
  void* parent_tex;
  void* extra;
};

// Free-list object pool (never invalidates live pointers returned to guests)
std::vector<d9_obj*> g_obj_free;
u32 g_live_objs = 0;

d9_obj* obj_alloc(const void** vtbl, u32 handle, u32 kind,
                  u32 w = 0, u32 h = 0, u32 fmt = 0) {
  d9_obj* o = nullptr;
  if (!g_obj_free.empty()) {
    o = g_obj_free.back();
    g_obj_free.pop_back();
  } else {
    o = (d9_obj*)calloc(1, sizeof(d9_obj));
    if (!o) return nullptr;
  }
  o->vtbl = vtbl;
  o->refcount = 1;
  o->handle = handle;
  o->kind = kind;
  o->w = w;
  o->h = h;
  o->fmt = fmt;
  o->sub_face = 0;
  o->sub_level = 0;
  o->parent_tex = nullptr;
  o->extra = nullptr;
  ++g_live_objs;
  return o;
}

void obj_free(d9_obj* o) {
  if (!o) return;
  memset(o, 0, sizeof(*o));
  if (g_live_objs) --g_live_objs;
  g_obj_free.push_back(o);
}

// GPU-object metadata (always tracked whether handle comes from backend or offline pool)
struct gpu_obj {
  u32 alive = 0;
  u32 kind = 0;
  u32 w = 0, h = 0, depth = 1, levels = 1, bytes = 0, usage = 0, fmt = 0;
  u32 offline = 0;
  u32 lock_face = 0, lock_level = 0, is_index32 = 0;
  std::vector<u32> bytecode;
  const MOJOSHADER_parseData* parsed = nullptr;
  std::vector<d3d9_vert_elem> decl_elems;
  std::vector<u8> cpu_copy;
};

std::vector<gpu_obj> g_gpu(1);  // index 0 reserved
std::vector<u32> g_gpu_free;

gpu_obj* gpu_bind(u32 h, u32 kind, u32 w, u32 hh, u32 depth, u32 levels,
                  u32 bytes, u32 usage, u32 fmt, u32 is_offline) {
  if (!h) return nullptr;
  if (h >= g_gpu.size()) g_gpu.resize((size_t)h + 1);
  gpu_obj& g = g_gpu[h];
  if (g.parsed) {
    MOJOSHADER_freeParseData(g.parsed);
    g.parsed = nullptr;
  }
  g.alive = 1;
  g.kind = kind;
  g.w = w;
  g.h = hh;
  g.depth = depth ? depth : 1;
  g.levels = levels ? levels : 1;
  g.bytes = bytes;
  g.usage = usage;
  g.fmt = fmt;
  g.offline = is_offline;
  g.lock_face = 0;
  g.lock_level = 0;
  g.is_index32 = (kind == 3 && fmt == 102) ? 1u : 0u;
  g.bytecode.clear();
  g.decl_elems.clear();
  g.cpu_copy.clear();
  return &g;
}

u32 gpu_alloc(u32 kind, u32 w, u32 hh, u32 levels, u32 bytes, u32 usage, u32 fmt,
              u32 depth = 1) {
  u32 h = 0;
  if (!g_gpu_free.empty()) {
    h = g_gpu_free.back();
    g_gpu_free.pop_back();
  } else {
    h = (u32)g_gpu.size();
    g_gpu.emplace_back();
  }
  gpu_bind(h, kind, w, hh, depth, levels, bytes, usage, fmt, 1);
  return h;
}

gpu_obj* gpu_at(u32 h) {
  return (h && h < g_gpu.size() && g_gpu[h].alive) ? &g_gpu[h] : nullptr;
}

void gpu_free_meta(u32 h) {
  if (!h || h >= g_gpu.size() || !g_gpu[h].alive) return;
  gpu_obj& g = g_gpu[h];
  if (g.parsed) {
    MOJOSHADER_freeParseData(g.parsed);
    g.parsed = nullptr;
  }
  g.bytecode.clear();
  g.decl_elems.clear();
  g.cpu_copy.clear();
  g.alive = 0;
  if (g.offline) g_gpu_free.push_back(h);
}

// singleton interfaces the guest holds for the process lifetime
d9_obj* g_d3d9_root = nullptr;   // IDirect3D9
d9_obj* g_dev = nullptr;         // IDirect3DDevice9
d9_obj* g_d3d9ex_root = nullptr;  // IDirect3D9Ex (22-slot vtable)
d9_obj* g_devex = nullptr;        // IDirect3DDevice9Ex (133-slot vtable)
d9_obj* g_backbuffer = nullptr;  // IDirect3DSurface9
d9_obj* g_default_ds = nullptr;  // IDirect3DSurface9

// SetMaximumFrameLatency value (accepted; the console backend presents
// synchronously through the swapchain, so there is no queue to bound)
u32 g_frame_latency = 0;

// active draw state marshalled to the host at draw time
u32 g_stream_vb[8] = {};
d9_obj* g_stream_vb_obj[8] = {};
u32 g_stream_stride[8] = {};
u32 g_stream_offset[8] = {};
u32 g_stream_freq[8] = {};
u32 g_index_ib = 0;
d9_obj* g_index_ib_obj = nullptr;
u32 g_cur_fvf = 0;
d9_obj* g_cur_decl = nullptr;
d9_obj* g_cur_vs = nullptr;
d9_obj* g_cur_ps = nullptr;
d9_obj* g_cur_rt[4] = {};
d9_obj* g_cur_ds = nullptr;
d9_obj* g_cur_tex[16] = {};
u32 g_rs[256] = {};
i32 g_scissor[4] = {0, 0, 1280, 720};
d3d9_viewport g_vp = {0, 0, 1280, 720, 0.0f, 1.0f};
float g_transforms[260][16] = {};
float g_vs_const_f[512 * 4] = {};
i32 g_vs_const_i[32 * 4] = {};
i32 g_vs_const_b[32] = {};
float g_ps_const_f[512 * 4] = {};
i32 g_ps_const_i[32 * 4] = {};
i32 g_ps_const_b[32] = {};
bool g_state_inited = false;

void init_default_state() {
  if (g_state_inited) return;
  g_state_inited = true;
  for (u32 i = 0; i < 8; ++i) g_stream_freq[i] = 1;
  for (u32 i = 0; i < 260; ++i) {
    g_transforms[i][0] = g_transforms[i][5] = g_transforms[i][10] =
        g_transforms[i][15] = 1.0f;
  }
  g_rs[7] = 1;   // ZENABLE
  g_rs[14] = 1;  // ZWRITEENABLE
  g_rs[22] = 2;  // CULLMODE = D3DCULL_CCW
  g_rs[23] = 4;  // ZFUNC = D3DCMP_LESSEQUAL
  g_rs[137] = 1; // LIGHTING
}

// StateBlock capture storage
struct d9_state_block {
  u32 type = 1;  // D3DSBT_ALL=1, PIXEL=2, VERTEX=3
  bool has_fvf = false;
  u32 fvf = 0;
  d9_obj* decl = nullptr;
  d9_obj* vs = nullptr;
  d9_obj* ps = nullptr;
  bool has_vs = false, has_ps = false, has_decl = false;
  u8 rs_mask[256] = {};
  u32 rs[256] = {};
  u8 tex_mask[16] = {};
  d9_obj* tex[16] = {};
  bool has_vs_cf = false;
  float vs_cf[256 * 4] = {};
  bool has_ps_cf = false;
  float ps_cf[224 * 4] = {};
};
d9_state_block* g_recording_sb = nullptr;

// scratch backing for locks when no host backend is bound (offline tests)
u8 g_scratch[1 << 20];

// DX9 SM1..SM3 bytecode byte-length scanner (stops safely at 0x0000FFFF)
u32 scan_dx9_shader_bytes(const u32* code) {
  if (!code) return 0;
  const u32 ver = code[0];
  const u16 prefix = (u16)(ver >> 16);
  if (prefix != 0xFFFEu && prefix != 0xFFFFu) return 0;
  const u32 maj = (ver >> 8) & 0xFFu;
  constexpr u32 k_max_dwords = 65536u;
  u32 i = 1;
  while (i < k_max_dwords) {
    const u32 tok = code[i++];
    if (tok == 0x0000FFFFu) return i * 4u;
    const u16 op = (u16)(tok & 0xFFFFu);
    if (op == 0xFFFEu) {  // D3DSIO_COMMENT
      const u32 len = (tok >> 16) & 0x7FFFu;
      if (i + len >= k_max_dwords) return 0;
      i += len;
      continue;
    }
    if (op == 0xFFFDu) continue;  // phase
    if (maj >= 2u) {
      const u32 len = (tok >> 24) & 0x0Fu;
      if (i + len >= k_max_dwords) return 0;
      i += len;
    } else {
      if (op == 0x0051u || op == 0x0052u) {  // DEF / DEFI
        if (i + 5u >= k_max_dwords) return 0;
        i += 5u;
      } else if (op == 0x0053u || op == 0x001Fu) {  // DEFB / DCL
        if (i + 2u >= k_max_dwords) return 0;
        i += 2u;
      } else {
        while (i < k_max_dwords && (code[i] & 0x80000000u) != 0u) ++i;
      }
    }
  }
  return 0;
}

struct d9_shader_blob {
  std::vector<u32> bytecode;
  const MOJOSHADER_parseData* parsed = nullptr;
};

struct d9_decl_blob {
  std::vector<d3d9_vert_elem> elems;
};

struct d9_surf_cpu {
  u32 w = 0, h = 0, depth = 1, pitch = 0, fmt = 0;
  std::vector<u8> pixels;
};

}  // namespace

extern "C" const void* trt_d3d9_mojo_parse(const u32* bytecode, u32 byte_len,
                                           u32* out_byte_len) {
  if (!bytecode) return nullptr;
  const u32 nbytes = byte_len ? byte_len : scan_dx9_shader_bytes(bytecode);
  if (out_byte_len) *out_byte_len = nbytes;
  if (nbytes < 8u) return nullptr;
  return MOJOSHADER_parse(MOJOSHADER_PROFILE_HLSL, "main",
                          (const unsigned char*)bytecode, nbytes,
                          nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
}

extern "C" void trt_d3d9_mojo_free(const void* parsed) {
  if (parsed) MOJOSHADER_freeParseData((const MOJOSHADER_parseData*)parsed);
}

extern "C" int trt_d3d9_mojo_extract(const void* parsed_v,
                                     d3d9_mojo_layout* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  const MOJOSHADER_parseData* pd = (const MOJOSHADER_parseData*)parsed_v;
  if (!pd || pd->error_count != 0) return 0;
  out->is_ps = (pd->shader_type == MOJOSHADER_TYPE_PIXEL) ? 1u : 0u;

  // Match emit_HLSL_finalize() / emit_HLSL_uniform() ordering:
  // 1) non-constant relative arrays allocate contiguous vec4 slots first;
  // 2) scalar/vec4 float uniforms, int4 uniforms, bool uniforms follow.
  for (int i = 0; i < pd->uniform_count; ++i) {
    const MOJOSHADER_uniform& u = pd->uniforms[i];
    if (u.constant) continue;
    if (u.array_count > 0 && u.type == MOJOSHADER_UNIFORM_FLOAT) {
      const u16 base = (u16)out->f4_count;
      if (out->entry_count < 512u) {
        d3d9_mojo_entry& e = out->entries[out->entry_count++];
        e.kind = 0;
        e.is_array = 1;
        e.reg_index = (u16)u.index;
        e.cb_slot = base;
        e.array_count = (u16)u.array_count;
      }
      // Bake any DEF constants that fall inside this indexed array.
      for (int c = 0; c < pd->constant_count && out->def_count < 128u; ++c) {
        const MOJOSHADER_constant& mc = pd->constants[c];
        if (mc.type == MOJOSHADER_UNIFORM_FLOAT &&
            mc.index >= u.index && mc.index < u.index + u.array_count) {
          d3d9_mojo_def& df = out->defs[out->def_count++];
          df.cb_slot = (u16)(base + (mc.index - u.index));
          memcpy(df.val, mc.value.f, sizeof(df.val));
        }
      }
      out->f4_count += (u32)u.array_count;
    }
  }
  for (int i = 0; i < pd->uniform_count; ++i) {
    const MOJOSHADER_uniform& u = pd->uniforms[i];
    if (u.constant || u.array_count > 0) continue;
    if (out->entry_count >= 512u) break;
    d3d9_mojo_entry& e = out->entries[out->entry_count++];
    e.is_array = 0;
    e.reg_index = (u16)u.index;
    e.array_count = 1;
    if (u.type == MOJOSHADER_UNIFORM_FLOAT) {
      e.kind = 0;
      e.cb_slot = (u16)out->f4_count++;
    } else if (u.type == MOJOSHADER_UNIFORM_INT) {
      e.kind = 1;
      e.cb_slot = (u16)out->i4_count++;
    } else {
      e.kind = 2;
      e.cb_slot = (u16)out->b_count++;
    }
  }
  for (int i = 0; i < pd->sampler_count; ++i) {
    const int st = pd->samplers[i].index;
    if (st >= 0 && st < 16) out->sampler_mask |= (1u << (u32)st);
    if (pd->samplers[i].texbem) out->f4_count += 2u;
  }
  return 1;
}

extern "C" int trt_d3d9_get_stream(u32 stream, d3d9_handle* vb, u32* offset,
                                   u32* stride, u32* freq) {
  if (stream >= 8u) return 0;
  if (vb) *vb = g_stream_vb[stream];
  if (offset) *offset = g_stream_offset[stream];
  if (stride) *stride = g_stream_stride[stream];
  if (freq) *freq = g_stream_freq[stream];
  return 1;
}

extern "C" u32 trt_d3d9_get_rs(u32 state) {
  return state < 256u ? g_rs[state] : 0u;
}

extern "C" u32 trt_d3d9_mojo_cb_info(const void* parsed,
                                     u32* out_sampler_mask) {
  d3d9_mojo_layout lay;
  if (!trt_d3d9_mojo_extract(parsed, &lay)) {
    if (out_sampler_mask) *out_sampler_mask = 0;
    return 0u;
  }
  if (out_sampler_mask) *out_sampler_mask = lay.sampler_mask;
  return (lay.f4_count + lay.i4_count + lay.b_count) * 16u;
}

extern "C" int trt_d3d9_mojo_pack_cb(const void* parsed, const float* cf,
                                     const i32* ci, const int* cb,
                                     void* dst_cb, u32 dst_bytes) {
  d3d9_mojo_layout lay;
  if (!trt_d3d9_mojo_extract(parsed, &lay) || !dst_cb) return 0;
  const u32 need = (lay.f4_count + lay.i4_count + lay.b_count) * 16u;
  if (need == 0u) return 1;
  if (dst_bytes < need) return 0;
  memset(dst_cb, 0, need);
  u8* base = (u8*)dst_cb;
  u8* f4_ptr = base;
  u8* i4_ptr = base + (size_t)lay.f4_count * 16u;
  u8* b_ptr = i4_ptr + (size_t)lay.i4_count * 16u;
  for (u32 i = 0; i < lay.entry_count; ++i) {
    const d3d9_mojo_entry& e = lay.entries[i];
    if (e.kind == 0) {
      if (!cf) continue;
      for (u32 k = 0; k < e.array_count; ++k) {
        const u32 r = (u32)e.reg_index + k;
        if (r < 512u && (u32)e.cb_slot + k < lay.f4_count) {
          memcpy(f4_ptr + ((size_t)e.cb_slot + k) * 16u, cf + (size_t)r * 4u, 16u);
        }
      }
    } else if (e.kind == 1) {
      if (!ci) continue;
      if (e.reg_index < 32u && e.cb_slot < lay.i4_count) {
        memcpy(i4_ptr + (size_t)e.cb_slot * 16u, ci + (size_t)e.reg_index * 4u, 16u);
      }
    } else {
      if (!cb) continue;
      if (e.reg_index < 32u && e.cb_slot < lay.b_count) {
        u32 v = cb[e.reg_index] ? 1u : 0u;
        memcpy(b_ptr + (size_t)e.cb_slot * 16u, &v, 4u);
      }
    }
  }
  for (u32 i = 0; i < lay.def_count; ++i) {
    const d3d9_mojo_def& df = lay.defs[i];
    if (df.cb_slot < lay.f4_count) {
      memcpy(f4_ptr + (size_t)df.cb_slot * 16u, df.val, 16u);
    }
  }
  return 1;
}

// forward declarations: the vtables live at the bottom (SDK order), the
// functions they point at are defined above them
extern "C" {
extern const void* g_d3d9_vtbl[17];
extern const void* g_dev9_vtbl[119];
extern const void* g_d3d9ex_vtbl[22];
extern const void* g_dev9ex_vtbl[133];
extern const void* g_tex9_vtbl[22];
extern const void* g_voltex9_vtbl[22];
extern const void* g_cube9_vtbl[22];
extern const void* g_vb9_vtbl[13];
extern const void* g_ib9_vtbl[13];
extern const void* g_vs9_vtbl[5];
extern const void* g_ps9_vtbl[5];
extern const void* g_decl9_vtbl[5];
extern const void* g_surf9_vtbl[17];
extern const void* g_offscreen_surf9_vtbl[17];
extern const void* g_vol9_vtbl[11];
extern const void* g_sb9_vtbl[6];
// Builds the Ex vtables once from the base vtables (defined below, next to
// the vtable tables); called from Direct3DCreate9Ex / CreateDeviceEx and
// from trt_bridge_register_d3d9.
void ensure_ex_vtbls();
}

extern "C" {

// ---- IUnknown ----
int TRT_GUEST_ABI d9_QI(void* self, const void*, void** ppv) {
  if (!ppv) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  *ppv = self;
  ++o->refcount;
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d9_AR(void* self) { return ++((d9_obj*)self)->refcount; }
u32 TRT_GUEST_ABI d9_Rel(void* self) {
  d9_obj* o = (d9_obj*)self;
  if (o->refcount) --o->refcount;
  if (o->refcount == 0 && o != (d9_obj*)g_d3d9_root &&
      o != (d9_obj*)g_dev && o != (d9_obj*)g_d3d9ex_root &&
      o != (d9_obj*)g_devex && o != (d9_obj*)g_backbuffer &&
      o != (d9_obj*)g_default_ds) {
    if (o->parent_tex) {
      d9_Rel(o->parent_tex);
    } else {
      if ((o->kind == 5 || o->kind == 6) && o->extra) {
        d9_shader_blob* sb = (d9_shader_blob*)o->extra;
        if (sb->parsed) MOJOSHADER_freeParseData(sb->parsed);
        delete sb;
        o->extra = nullptr;
      } else if (o->kind == 7 && o->extra) {
        delete (d9_decl_blob*)o->extra;
        o->extra = nullptr;
      } else if ((o->kind == 4 || o->kind == 9 || o->kind == 10) && o->extra) {
        delete (d9_surf_cpu*)o->extra;
        o->extra = nullptr;
      } else if (o->kind == 11 && o->extra) {
        delete (d9_state_block*)o->extra;
        o->extra = nullptr;
      }
      if (g_hooks.release && o->handle) g_hooks.release(o->handle, g_hooks.user);
      gpu_free_meta(o->handle);
    }
    obj_free(o);
    return 0;
  }
  return o->refcount;
}

// ---- IDirect3D9 (17 slots) ----
int TRT_GUEST_ABI d9_RegisterSoftwareDevice(void*, void*) {
  return D3DERR_NOTIMPL_;
}
u32 TRT_GUEST_ABI d9_GetAdapterCount(void*) { return 1; }
int TRT_GUEST_ABI d9_GetAdapterIdentifier(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  // D3DADAPTER_IDENTIFIER9: Driver[512] at 0, Description[512] at 0x200
  memset(out, 0, 0x2A0);
  memcpy((char*)out, "Tiramisu9On11", 14);
  memcpy((char*)out + 0x200, "Tiramisu D3D9 -> D3D11 bridge", 30);
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d9_GetAdapterModeCount(void*, u32, u32) { return 1; }
int TRT_GUEST_ABI d9_EnumAdapterModes(void*, u32, u32 fmt, u32 mode, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (mode > 0) return D3DERR_INVALIDCALL_;  // exactly one mode
  // D3DDISPLAYMODE { UINT Width, Height, RefreshRate; FORMAT Format; }
  u32* p = (u32*)out;
  p[0] = g_hooks.width;
  p[1] = g_hooks.height;
  p[2] = 60;
  p[3] = fmt;
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_GetAdapterDisplayMode(void*, u32 adapter, void* out) {
  return d9_EnumAdapterModes(nullptr, adapter, 21 /*D3DFMT_X8R8G8B8*/, 0, out);
}
int TRT_GUEST_ABI d9_CheckDeviceType(void*, u32, u32, u32, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceFormat(void*, u32, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceMultiSampleType(void*, u32, u32, int, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDepthStencilMatch(void*, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceFormatConversion(void*, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_GetDeviceCaps(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  memset(out, 0, 0x200);
  u32* p = (u32*)out;
  p[0x10 / 4] = 0x00000020;  // DevCaps: D3DDEVCAPS_HWTRANSFORMANDLIGHT-ish
  p[0x54 / 4] = 8192;        // MaxTextureWidth
  p[0x58 / 4] = 8192;        // MaxTextureHeight
  p[0x8C / 4] = 65535;       // MaxPrimitiveCount
  p[0x94 / 4] = 8;           // MaxTextureBlendStages
  p[0x98 / 4] = 8;           // MaxSimultaneousTextures
  p[0xE4 / 4] = 256;         // MaxVertexShaderConst
  p[0xE8 / 4] = 0x00030000;  // VertexShaderVersion 3_0
  p[0xEC / 4] = 0x00030000;  // PixelShaderVersion 3_0
  return D3D_OK_;
}
void* TRT_GUEST_ABI d9_GetAdapterMonitor(void*, u32) { return (void*)0x11; }

// Direct3DCreate9(SDKVersion) -> IDirect3D9*  (module entry point)
void* TRT_GUEST_ABI trt_Direct3DCreate9(u32 sdk_version) {
  char line[128];
  snprintf(line, sizeof(line), "d3d9: Direct3DCreate9(sdk=%u) -> bridge",
           sdk_version);
  trt_core_log(line);
  if (!g_d3d9_root) {
    g_d3d9_root = obj_alloc(g_d3d9_vtbl, 0, 0);
    if (!g_d3d9_root) return nullptr;
  }
  ++g_d3d9_root->refcount;
  return g_d3d9_root;
}
int TRT_GUEST_ABI trt_Direct3DCreate9Ex(u32 sdk_version, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  char line[128];
  snprintf(line, sizeof(line), "d3d9: Direct3DCreate9Ex(sdk=%u) -> bridge Ex",
           sdk_version);
  trt_core_log(line);
  // The Ex object gets the 22-slot vtable: returning the plain 17-slot
  // object here (as before) let callers invoke CreateDeviceEx at slot 20
  // past the end of the vtable.
  ensure_ex_vtbls();
  if (!g_d3d9ex_root) {
    g_d3d9ex_root = obj_alloc(g_d3d9ex_vtbl, 0, 0);
    if (!g_d3d9ex_root) return D3DERR_NOTIMPL_;
  }
  ++g_d3d9ex_root->refcount;
  *out = g_d3d9ex_root;
  return D3D_OK_;
}

// Shared device creation for CreateDevice / CreateDeviceEx. The two flavors
// share all state, the backbuffer and the depth-stencil surface; they differ
// only in the device object's vtable (119 vs 133 slots) and singleton.
int d9_CreateDeviceImpl(u32 dev_type, u32 flags, void* pp, void** out_dev,
                        bool ex) {
  char line[192];
  snprintf(line, sizeof(line),
           "d3d9: CreateDevice%s(type=%u flags=0x%x) -> bridge device",
           ex ? "Ex" : "", dev_type, flags);
  trt_core_log(line);
  if (!out_dev || !pp) return D3DERR_INVALIDCALL_;
  if (!g_hooks.present) return D3DERR_NOTIMPL_;  // no host backend bound
  init_default_state();
  // D3DPRESENT_PARAMETERS: +0 BackBufferWidth, +4 Height
  u32* p = (u32*)pp;
  const u32 bw = p[0] ? p[0] : (g_hooks.width ? g_hooks.width : 1280u);
  const u32 bh = p[1] ? p[1] : (g_hooks.height ? g_hooks.height : 720u);
  g_vp = {0, 0, bw, bh, 0.f, 1.f};
  g_scissor[0] = 0; g_scissor[1] = 0; g_scissor[2] = (i32)bw; g_scissor[3] = (i32)bh;
  if (g_hooks.set_viewport) g_hooks.set_viewport(&g_vp, g_hooks.user);
  if (!g_backbuffer) g_backbuffer = obj_alloc(g_surf9_vtbl, 0, 4, bw, bh, 21);
  else { g_backbuffer->w = bw; g_backbuffer->h = bh; }
  if (!g_default_ds) g_default_ds = obj_alloc(g_surf9_vtbl, 0, 4, bw, bh, 75 /*D24S8*/);
  else { g_default_ds->w = bw; g_default_ds->h = bh; }
  if (ex) {
    ensure_ex_vtbls();
    if (!g_devex) {
      g_devex = obj_alloc(g_dev9ex_vtbl, 0, 0);
      if (!g_devex) return D3DERR_NOTIMPL_;
    }
    ++g_devex->refcount;
    *out_dev = g_devex;
  } else {
    if (!g_dev) {
      g_dev = obj_alloc(g_dev9_vtbl, 0, 0);
      if (!g_dev) return D3DERR_NOTIMPL_;
    }
    ++g_dev->refcount;
    *out_dev = g_dev;
  }
  return D3D_OK_;
}

// CreateDevice(adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS*, out_dev)
int TRT_GUEST_ABI d9_CreateDevice(void*, u32 adapter, u32 dev_type, void* hwnd,
                                  u32 flags, void* pp, void** out_dev) {
  (void)adapter; (void)hwnd;
  return d9_CreateDeviceImpl(dev_type, flags, pp, out_dev, false);
}

// ---- IDirect3D9Ex (root slots 17-21) ----
// D3DDISPLAYMODEEX { UINT Width, Height, RefreshRate; D3DFORMAT Format;
//                    D3DSCANLINEORDERING ScanLineOrdering; } = 20 bytes
void fill_display_mode_ex(void* out, u32 fmt) {
  u32* p = (u32*)out;
  p[0] = g_hooks.width ? g_hooks.width : 1280u;
  p[1] = g_hooks.height ? g_hooks.height : 720u;
  p[2] = 60;
  p[3] = fmt ? fmt : 21u /*D3DFMT_X8R8G8B8*/;
  p[4] = 0;  // D3DSCANLINEORDERING_UNKNOWN
}

// GetAdapterModeCountEx(adapter, D3DDISPLAYMODEFILTER*) — one mode, like the
// non-Ex enumeration (the console has a single display mode).
u32 TRT_GUEST_ABI d9_GetAdapterModeCountEx(void*, u32, const void*) {
  return 1;
}
// EnumAdapterModesEx(adapter, filter, mode, D3DDISPLAYMODEEX*)
// D3DDISPLAYMODEFILTER { UINT Size; D3DFORMAT Format;
//                        D3DSCANLINEORDERING ScanLineOrdering; }
int TRT_GUEST_ABI d9_EnumAdapterModesEx(void*, u32, const void* filter, u32 mode,
                                        void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (mode > 0) return D3DERR_INVALIDCALL_;  // exactly one mode
  const u32 fmt = filter ? ((const u32*)filter)[1] : 0u;
  fill_display_mode_ex(out, fmt);
  return D3D_OK_;
}
// GetAdapterDisplayModeEx(adapter, D3DDISPLAYMODEEX*, D3DDISPLAYROTATION*)
int TRT_GUEST_ABI d9_GetAdapterDisplayModeEx(void*, u32, void* mode_out,
                                             void* rot_out) {
  if (!mode_out) return D3DERR_INVALIDCALL_;
  fill_display_mode_ex(mode_out, 21u /*D3DFMT_X8R8G8B8*/);
  if (rot_out) *(u32*)rot_out = 0;  // D3DDISPLAYROTATION_IDENTITY
  return D3D_OK_;
}
// CreateDeviceEx(adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS*,
//                D3DDISPLAYMODEEX* fullscreen_mode, out_dev)
int TRT_GUEST_ABI d9_CreateDeviceEx(void*, u32 adapter, u32 dev_type, void* hwnd,
                                    u32 flags, void* pp, void* fullscreen_mode,
                                    void** out_dev) {
  (void)adapter; (void)hwnd; (void)fullscreen_mode;  // always windowed here
  return d9_CreateDeviceImpl(dev_type, flags, pp, out_dev, true);
}
// GetAdapterLUID(adapter, LUID*) — the console backend has no adapter LUID;
// a zero LUID is the honest value (D3D9On12 does the same for its adapter).
int TRT_GUEST_ABI d9_GetAdapterLUID(void*, u32, void* luid_out) {
  if (!luid_out) return D3DERR_INVALIDCALL_;
  ((u32*)luid_out)[0] = 0;
  ((u32*)luid_out)[1] = 0;
  return D3D_OK_;
}

// ---- resource creation (device slots 23-30, 86, 91, 106) ----
int TRT_GUEST_ABI dev_CreateTexture(void*, u32 w, u32 h, u32 levels, u32 usage,
                                    u32 fmt, u32 pool, void** out) {
  (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 lv = levels ? levels : 1u;
  const u32 hnd = g_hooks.create_tex
                      ? g_hooks.create_tex(w, h, lv, usage, fmt, g_hooks.user)
                      : gpu_alloc(1, w, h, lv, 0, usage, fmt);
  if (hnd && hnd < 65536u && !gpu_at(hnd)) {
    gpu_bind(hnd, 1, w, h, 1, lv, 0, usage, fmt, 0);
  }
  d9_obj* o = obj_alloc(g_tex9_vtbl, hnd, 1, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  o->sub_level = lv;
  *out = o;
  char line[160];
  snprintf(line, sizeof(line), "d3d9: CreateTexture(%ux%u fmt=%u) -> h%u", w,
           h, fmt, hnd);
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateVolumeTexture(void*, u32 w, u32 h, u32 depth,
                                          u32 levels, u32 usage, u32 fmt,
                                          u32 pool, void** out, void**) {
  (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 lv = levels ? levels : 1u;
  const u32 d = depth ? depth : 1u;
  const u32 hnd = g_hooks.create_tex_ex
                      ? g_hooks.create_tex_ex(9, w, h, d, lv, usage, fmt,
                                              g_hooks.user)
                      : gpu_alloc(9, w, h, lv, 0, usage, fmt, d);
  if (hnd && hnd < 65536u && !gpu_at(hnd)) {
    gpu_bind(hnd, 9, w, h, d, lv, 0, usage, fmt, 0);
  }
  d9_obj* o = obj_alloc(g_voltex9_vtbl, hnd, 9, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  o->sub_face = d;
  o->sub_level = lv;
  auto* cpu = new d9_surf_cpu();
  cpu->w = w; cpu->h = h; cpu->depth = d;
  cpu->pitch = (u32)trt_d3d9_row_pitch(w, fmt);
  cpu->fmt = fmt;
  o->extra = cpu;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateCubeTexture(void*, u32 edge, u32 levels, u32 usage,
                                        u32 fmt, u32 pool, void** out, void**) {
  (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 lv = levels ? levels : 1u;
  const u32 hnd = g_hooks.create_tex_ex
                      ? g_hooks.create_tex_ex(8, edge, edge, 6, lv, usage, fmt,
                                              g_hooks.user)
                      : gpu_alloc(8, edge, edge, lv, 0, usage, fmt, 6);
  if (hnd && hnd < 65536u && !gpu_at(hnd)) {
    gpu_bind(hnd, 8, edge, edge, 6, lv, 0, usage, fmt, 0);
  }
  d9_obj* o = obj_alloc(g_cube9_vtbl, hnd, 8, edge, edge, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  o->sub_face = 6;
  o->sub_level = lv;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateVertexBuffer(void*, u32 bytes, u32 usage, u32 fvf,
                                         u32 pool, void** out, void** handle) {
  (void)pool; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_buf
                      ? g_hooks.create_buf(bytes, 0, g_hooks.user)
                      : gpu_alloc(2, 0, 0, 1, bytes, usage, fvf);
  d9_obj* o = obj_alloc(g_vb9_vtbl, hnd, 2);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateIndexBuffer(void*, u32 bytes, u32 usage, u32 fmt,
                                        u32 pool, void** out, void** handle) {
  (void)pool; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_buf
                      ? g_hooks.create_buf(bytes, 1, g_hooks.user)
                      : gpu_alloc(3, 0, 0, 1, bytes, usage, fmt);
  if (hnd && hnd < 65536u && !gpu_at(hnd)) {
    gpu_bind(hnd, 3, 0, 0, 1, 1, bytes, usage, fmt, 0);
  }
  if (g_hooks.set_index_fmt && hnd) {
    g_hooks.set_index_fmt(hnd, fmt == 102u ? 1u : 0u, g_hooks.user);
  }
  d9_obj* o = obj_alloc(g_ib9_vtbl, hnd, 3, 0, 0, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateRenderTarget(void*, u32 w, u32 h, u32 fmt, u32 multi,
                                         u32 msq, int windowed, void** out,
                                         void** handle) {
  (void)multi; (void)msq; (void)windowed; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_tex
                      ? g_hooks.create_tex(w, h, 1, 1 /*RENDERTARGET*/, fmt,
                                           g_hooks.user)
                      : gpu_alloc(4, w, h, 1, 0, 1, fmt);
  d9_obj* o = obj_alloc(g_surf9_vtbl, hnd, 4, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateDepthStencilSurface(void*, u32 w, u32 h,
                                                u32 fmt, u32 multi, u32 msq,
                                                int discard, int zenable,
                                                void** out) {
  (void)multi; (void)msq; (void)discard; (void)zenable;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_tex
                      ? g_hooks.create_tex(w, h, 1, 2u /*DEPTHSTENCIL*/, fmt,
                                           g_hooks.user)
                      : gpu_alloc(4, w, h, 1, 0, 2u, fmt);
  d9_obj* o = obj_alloc(g_surf9_vtbl, hnd, 4, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateOffscreenPlainSurface(void*, u32 w, u32 h, u32 fmt,
                                                  u32 pool, void** out, void**) {
  (void)pool;
  if (!out || !w || !h) return D3DERR_INVALIDCALL_;
  const u32 pitch = (u32)trt_d3d9_row_pitch(w, fmt);
  d9_obj* o = obj_alloc(g_offscreen_surf9_vtbl, 0, 4, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  auto* cpu = new d9_surf_cpu();
  cpu->w = w; cpu->h = h; cpu->depth = 1; cpu->pitch = pitch; cpu->fmt = fmt;
  cpu->pixels.resize((size_t)pitch * h, 0);
  o->extra = cpu;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateShaderCommon(u32 is_ps, const u32* code, void** out) {
  if (!out || !code) return D3DERR_INVALIDCALL_;
  const u32 bytes = scan_dx9_shader_bytes(code);
  if (bytes < 8u) return D3DERR_INVALIDCALL_;
  const MOJOSHADER_parseData* pd = MOJOSHADER_parse(
      MOJOSHADER_PROFILE_HLSL, "main", (const unsigned char*)code, bytes,
      nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
  u32 hnd = 0;
  if (g_hooks.create_shader && pd && pd->error_count == 0 && pd->output) {
    hnd = g_hooks.create_shader(is_ps, (u32)pd->major_ver, (u32)pd->minor_ver,
                                pd->output, (u32)pd->output_len, pd,
                                g_hooks.user);
  }
  d9_obj* o = obj_alloc(is_ps ? g_ps9_vtbl : g_vs9_vtbl, hnd, is_ps ? 6u : 5u);
  if (!o) {
    if (pd) MOJOSHADER_freeParseData(pd);
    return D3DERR_NOTIMPL_;
  }
  auto* sb = new d9_shader_blob();
  sb->bytecode.assign(code, code + (bytes / 4u));
  sb->parsed = pd;
  o->extra = sb;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateVertexShader(void*, const u32* code, void** out) {
  return dev_CreateShaderCommon(0u, code, out);
}
int TRT_GUEST_ABI dev_CreatePixelShader(void*, const u32* code, void** out) {
  return dev_CreateShaderCommon(1u, code, out);
}
int TRT_GUEST_ABI dev_CreateVertexDeclaration(void*, const void* elems_v,
                                              void** out) {
  if (!out || !elems_v) return D3DERR_INVALIDCALL_;
  const d3d9_vert_elem* in = (const d3d9_vert_elem*)elems_v;
  u32 n = 0;
  while (n < 64u) {
    if (in[n].stream == 0xFFu) { ++n; break; }
    ++n;
  }
  if (n == 0) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_decl
                      ? g_hooks.create_decl(in, n, g_hooks.user)
                      : 0u;
  d9_obj* o = obj_alloc(g_decl9_vtbl, hnd, 7);
  if (!o) return D3DERR_NOTIMPL_;
  auto* db = new d9_decl_blob();
  db->elems.assign(in, in + n);
  o->extra = db;
  *out = o;
  return D3D_OK_;
}

// ---- state marshalling & StateBlock ----
void sb_capture_block(d9_state_block* sb, u32 type) {
  if (!sb) return;
  sb->type = type;
  if (type == 1u /*ALL*/ || type == 3u /*VERTEX*/) {
    sb->has_fvf = true; sb->fvf = g_cur_fvf;
    sb->has_decl = true; sb->decl = g_cur_decl;
    sb->has_vs = true; sb->vs = g_cur_vs;
    sb->has_vs_cf = true;
    memcpy(sb->vs_cf, g_vs_const_f, sizeof(sb->vs_cf));
  }
  if (type == 1u /*ALL*/ || type == 2u /*PIXEL*/) {
    sb->has_ps = true; sb->ps = g_cur_ps;
    sb->has_ps_cf = true;
    memcpy(sb->ps_cf, g_ps_const_f, sizeof(sb->ps_cf));
    for (u32 i = 0; i < 16u; ++i) { sb->tex_mask[i] = 1u; sb->tex[i] = g_cur_tex[i]; }
  }
  for (u32 i = 0; i < 256u; ++i) {
    sb->rs_mask[i] = 1u;
    sb->rs[i] = g_rs[i];
  }
}

int TRT_GUEST_ABI dev_SetTransform(void*, u32 kind, const void* m) {
  if (!m) return D3DERR_INVALIDCALL_;
  const u32 idx = (kind < 260u) ? kind : 0u;
  memcpy(g_transforms[idx], m, 64);
  if (g_hooks.set_transform) g_hooks.set_transform(kind, (const float*)m, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetTransform(void*, u32 kind, void* m) {
  if (!m) return D3DERR_INVALIDCALL_;
  const u32 idx = (kind < 260u) ? kind : 0u;
  memcpy(m, g_transforms[idx], 64);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_MultiplyTransform(void* self, u32 kind, const void* m_v) {
  if (!m_v) return D3DERR_INVALIDCALL_;
  const u32 idx = (kind < 260u) ? kind : 0u;
  const float* a = (const float*)m_v;
  const float* b = g_transforms[idx];
  float r[16] = {};
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      for (int k = 0; k < 4; ++k)
        r[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
  return dev_SetTransform(self, kind, r);
}
int TRT_GUEST_ABI dev_SetViewport(void*, const void* vp) {
  if (!vp) return D3DERR_INVALIDCALL_;
  memcpy(&g_vp, vp, sizeof(g_vp));
  if (g_hooks.set_viewport) g_hooks.set_viewport(&g_vp, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetViewport(void*, void* vp) {
  if (!vp) return D3DERR_INVALIDCALL_;
  memcpy(vp, &g_vp, sizeof(g_vp));
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetRenderState(void*, u32 state, u32 value) {
  if (g_recording_sb) {
    if (state < 256u) {
      g_recording_sb->rs_mask[state] = 1u;
      g_recording_sb->rs[state] = value;
    }
    return D3D_OK_;
  }
  if (state < 256u) g_rs[state] = value;
  if (g_hooks.set_rs) g_hooks.set_rs(state, value, g_hooks.user);
  if (state == 174u /*D3DRS_SCISSORTESTENABLE*/ && g_hooks.set_scissor) {
    g_hooks.set_scissor(value != 0u ? 1 : 0, g_scissor[0], g_scissor[1],
                        g_scissor[2], g_scissor[3], g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetRenderState(void*, u32 state, u32* out) {
  if (out) *out = (state < 256u) ? g_rs[state] : 0u;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetTexture(void*, u32 stage, void* tex) {
  if (g_recording_sb) {
    if (stage < 16u) {
      g_recording_sb->tex_mask[stage] = 1u;
      g_recording_sb->tex[stage] = (d9_obj*)tex;
    }
    return D3D_OK_;
  }
  if (stage < 16u) g_cur_tex[stage] = (d9_obj*)tex;
  const u32 hnd = tex ? ((d9_obj*)tex)->handle : 0;
  if (g_hooks.set_texture) g_hooks.set_texture(stage, hnd, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetTexture(void*, u32 stage, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  *out = (stage < 16u) ? g_cur_tex[stage] : nullptr;
  if (*out) ++((d9_obj*)*out)->refcount;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetSamplerState(void*, u32 stage, u32 state, u32 value) {
  if (g_hooks.set_sampler) g_hooks.set_sampler(stage, state, value, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetTextureStageState(void*, u32 stage, u32 state, u32 value) {
  if (g_hooks.set_sampler) g_hooks.set_sampler(stage, state, value, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetFVF(void*, u32 fvf) {
  if (g_recording_sb) {
    g_recording_sb->has_fvf = true;
    g_recording_sb->fvf = fvf;
    return D3D_OK_;
  }
  g_cur_fvf = fvf;
  if (g_hooks.set_fvf) g_hooks.set_fvf(fvf, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexDeclaration(void*, void* decl) {
  if (g_recording_sb) {
    g_recording_sb->has_decl = true;
    g_recording_sb->decl = (d9_obj*)decl;
    return D3D_OK_;
  }
  g_cur_decl = (d9_obj*)decl;
  if (g_hooks.set_decl)
    g_hooks.set_decl(decl ? ((d9_obj*)decl)->handle : 0u, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetVertexDeclaration(void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_cur_decl;
  if (g_cur_decl) ++g_cur_decl->refcount;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexShader(void*, void* vs) {
  if (g_recording_sb) {
    g_recording_sb->has_vs = true;
    g_recording_sb->vs = (d9_obj*)vs;
    return D3D_OK_;
  }
  g_cur_vs = (d9_obj*)vs;
  if (g_hooks.set_shader)
    g_hooks.set_shader(0u, vs ? ((d9_obj*)vs)->handle : 0u, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetVertexShader(void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_cur_vs;
  if (g_cur_vs) ++g_cur_vs->refcount;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetPixelShader(void*, void* ps) {
  if (g_recording_sb) {
    g_recording_sb->has_ps = true;
    g_recording_sb->ps = (d9_obj*)ps;
    return D3D_OK_;
  }
  g_cur_ps = (d9_obj*)ps;
  if (g_hooks.set_shader)
    g_hooks.set_shader(1u, ps ? ((d9_obj*)ps)->handle : 0u, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetPixelShader(void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_cur_ps;
  if (g_cur_ps) ++g_cur_ps->refcount;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexShaderConstantF(void*, u32 start, const float* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 512u) {
    const u32 n = (start + count <= 512u) ? count : (512u - start);
    if (g_recording_sb) {
      g_recording_sb->has_vs_cf = true;
      if (start < 256u) {
        const u32 nc = (start + n <= 256u) ? n : (256u - start);
        memcpy(g_recording_sb->vs_cf + start * 4u, d, nc * 16u);
      }
      return D3D_OK_;
    }
    memcpy(g_vs_const_f + start * 4u, d, n * 16u);
  }
  if (g_hooks.set_shader_const_f)
    g_hooks.set_shader_const_f(0u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetVertexShaderConstantF(void*, u32 start, float* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 512u) {
    const u32 n = (start + count <= 512u) ? count : (512u - start);
    memcpy(d, g_vs_const_f + start * 4u, n * 16u);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetPixelShaderConstantF(void*, u32 start, const float* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 512u) {
    const u32 n = (start + count <= 512u) ? count : (512u - start);
    if (g_recording_sb) {
      g_recording_sb->has_ps_cf = true;
      if (start < 224u) {
        const u32 nc = (start + n <= 224u) ? n : (224u - start);
        memcpy(g_recording_sb->ps_cf + start * 4u, d, nc * 16u);
      }
      return D3D_OK_;
    }
    memcpy(g_ps_const_f + start * 4u, d, n * 16u);
  }
  if (g_hooks.set_shader_const_f)
    g_hooks.set_shader_const_f(1u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetPixelShaderConstantF(void*, u32 start, float* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 512u) {
    const u32 n = (start + count <= 512u) ? count : (512u - start);
    memcpy(d, g_ps_const_f + start * 4u, n * 16u);
  }
  return D3D_OK_;
}

// ---- StateBlock methods & device creation ----
int TRT_GUEST_ABI sb_Capture(void* self) {
  d9_obj* o = (d9_obj*)self;
  d9_state_block* sb = (d9_state_block*)o->extra;
  if (!sb) return D3DERR_INVALIDCALL_;
  sb_capture_block(sb, sb->type);
  return D3D_OK_;
}
int TRT_GUEST_ABI sb_Apply(void* self) {
  d9_obj* o = (d9_obj*)self;
  const d9_state_block* sb = (const d9_state_block*)o->extra;
  if (!sb) return D3DERR_INVALIDCALL_;
  d9_state_block* prev = g_recording_sb;
  g_recording_sb = nullptr;
  for (u32 i = 0; i < 256u; ++i) {
    if (sb->rs_mask[i]) dev_SetRenderState(nullptr, i, sb->rs[i]);
  }
  for (u32 i = 0; i < 16u; ++i) {
    if (sb->tex_mask[i]) dev_SetTexture(nullptr, i, sb->tex[i]);
  }
  if (sb->has_fvf) dev_SetFVF(nullptr, sb->fvf);
  if (sb->has_decl) dev_SetVertexDeclaration(nullptr, sb->decl);
  if (sb->has_vs) dev_SetVertexShader(nullptr, sb->vs);
  if (sb->has_ps) dev_SetPixelShader(nullptr, sb->ps);
  if (sb->has_vs_cf) dev_SetVertexShaderConstantF(nullptr, 0u, sb->vs_cf, 256u);
  if (sb->has_ps_cf) dev_SetPixelShaderConstantF(nullptr, 0u, sb->ps_cf, 224u);
  g_recording_sb = prev;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateStateBlock(void*, u32 type, void** out) {
  if (!out || type < 1u || type > 3u) return D3DERR_INVALIDCALL_;
  auto* sb = new d9_state_block();
  sb_capture_block(sb, type);
  d9_obj* o = obj_alloc(g_sb9_vtbl, 0, 11);
  if (!o) { delete sb; return D3DERR_NOTIMPL_; }
  o->extra = sb;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_BeginStateBlock(void*) {
  if (g_recording_sb) return D3DERR_INVALIDCALL_;
  g_recording_sb = new d9_state_block();
  g_recording_sb->type = 1u;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_EndStateBlock(void*, void** out) {
  if (!g_recording_sb) return D3DERR_INVALIDCALL_;
  d9_state_block* sb = g_recording_sb;
  g_recording_sb = nullptr;
  if (!out) { delete sb; return D3DERR_INVALIDCALL_; }
  d9_obj* o = obj_alloc(g_sb9_vtbl, 0, 11);
  if (!o) { delete sb; return D3DERR_NOTIMPL_; }
  o->extra = sb;
  *out = o;
  return D3D_OK_;
}

// ---- frame lifecycle ----
int TRT_GUEST_ABI dev_BeginScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_EndScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_Clear(void*, u32, const void*, u32 flags, u32 color,
                            float z, u32 stencil) {
  if (g_hooks.clear) g_hooks.clear(flags, color, z, stencil, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_Present(void*, const void*, const void*, const void*,
                              void*) {
  if (g_hooks.present) g_hooks.present(g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetRenderTarget(void*, u32 index, void* rt) {
  if (index >= 4u) return D3DERR_INVALIDCALL_;
  g_cur_rt[index] = (d9_obj*)rt;
  if (g_hooks.set_render_target)
    g_hooks.set_render_target(index, rt ? ((d9_obj*)rt)->handle : 0u, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetRenderTarget(void*, u32 index, void** out) {
  if (!out || index >= 4u) return D3DERR_INVALIDCALL_;
  d9_obj* rt = g_cur_rt[index];
  if (!rt && index == 0u) rt = g_backbuffer;
  if (!rt) { *out = nullptr; return D3DERR_NOTFOUND_; }
  ++rt->refcount;
  *out = rt;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetDepthStencilSurface(void*, void* ds) {
  g_cur_ds = (d9_obj*)ds;
  if (g_hooks.set_depth_stencil)
    g_hooks.set_depth_stencil(ds ? ((d9_obj*)ds)->handle : 0u, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetDepthStencilSurface(void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* ds = g_cur_ds ? g_cur_ds : g_default_ds;
  if (!ds) { *out = nullptr; return D3DERR_NOTFOUND_; }
  ++ds->refcount;
  *out = ds;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_Reset(void*, void* pp) {
  char line[96];
  snprintf(line, sizeof(line), "d3d9: Reset -> D3D_OK (device-lost recovery)");
  trt_core_log(line);
  g_state_inited = false;
  init_default_state();
  if (pp) {
    u32* p = (u32*)pp;
    const u32 bw = p[0] ? p[0] : (g_hooks.width ? g_hooks.width : 1280u);
    const u32 bh = p[1] ? p[1] : (g_hooks.height ? g_hooks.height : 720u);
    g_vp = {0, 0, bw, bh, 0.f, 1.f};
    g_scissor[0] = 0; g_scissor[1] = 0; g_scissor[2] = (i32)bw; g_scissor[3] = (i32)bh;
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetBackBuffer(void*, u32, u32, u32, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (!g_backbuffer) {
    g_backbuffer = obj_alloc(g_surf9_vtbl, 0, 4);
    if (!g_backbuffer) return D3DERR_NOTIMPL_;
  }
  ++g_backbuffer->refcount;
  *out = g_backbuffer;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_TestCooperativeLevel(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetDirect3D(void* self, void** out) {
  // An Ex device answers with the Ex root: its vtable is a strict superset
  // of the plain one, so returning the plain root here would let the caller
  // invoke CreateDeviceEx past the end of a 17-slot vtable.
  d9_obj* root =
      (self == (void*)g_devex && g_d3d9ex_root) ? g_d3d9ex_root : g_d3d9_root;
  if (!out || !root) return D3DERR_INVALIDCALL_;
  ++root->refcount;
  *out = root;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetDeviceCaps(void* self, void* out) {
  (void)self;
  return d9_GetDeviceCaps(nullptr, 0, 0, out);
}
void TRT_GUEST_ABI dev_SetCursorPosition(void*, u32, u32, u32) {}
int TRT_GUEST_ABI dev_ShowCursor(void*, int b) { return b; }
int TRT_GUEST_ABI dev_SetCursorProperties(void*, u32, u32, void*) {
  return D3D_OK_;
}
u32 TRT_GUEST_ABI dev_GetAvailableTextureMem(void*) { return 0x10000000; }
int TRT_GUEST_ABI dev_EvictManagedResources(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetDisplayMode(void*, u32 swapchain, void* out) {
  (void)swapchain;
  return d9_GetAdapterDisplayMode(nullptr, 0, out);
}

// ---- IDirect3DDevice9Ex (device slots 119-132) ----
// XNA 4.0's device-lost path calls ResetEx + CheckDeviceState; the rest is
// either a thin delegate onto the matching non-Ex method or an honest
// NOTIMPL for something the console backend cannot observe (residency,
// convolution kernels, rect composition).
int TRT_GUEST_ABI devex_SetConvolutionMonoKernel(void*, u32, u32, const float*,
                                                 const float*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI devex_ComposeRects(void*, void*, void*, void*, u32, void*,
                                     u32, i32, i32) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI devex_PresentEx(void*, const void*, const void*, const void*,
                                  const void*, u32) {
  if (g_hooks.present) g_hooks.present(g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI devex_GetGPUThreadPriority(void*, i32* prio) {
  if (!prio) return D3DERR_INVALIDCALL_;
  *prio = 0;  // the console backend has no GPU thread priority to report
  return D3D_OK_;
}
int TRT_GUEST_ABI devex_SetGPUThreadPriority(void*, i32) { return D3D_OK_; }
int TRT_GUEST_ABI devex_WaitForVBlank(void*, u32) {
  // No vsync-wait primitive on the console backend; Present already syncs
  // through the swapchain, so there is nothing to wait for.
  return D3D_OK_;
}
int TRT_GUEST_ABI devex_CheckResourceResidency(void*, void**, u32) {
  return D3DERR_NOTIMPL_;  // residency is not observable through the hooks
}
int TRT_GUEST_ABI devex_SetMaximumFrameLatency(void*, u32 max_latency) {
  g_frame_latency = max_latency;
  return D3D_OK_;
}
int TRT_GUEST_ABI devex_CheckDeviceState(void*, void*) {
  return D3D_OK_;  // the device is never lost in this model (cf. slot 3)
}
int TRT_GUEST_ABI devex_CreateRenderTargetEx(void* self, u32 w, u32 h, u32 fmt,
                                             u32 multi, u32 msq, int lockable,
                                             void** out, void** shared,
                                             u32 usage) {
  (void)lockable; (void)shared; (void)usage;  // no shared handles on console
  return dev_CreateRenderTarget(self, w, h, fmt, multi, msq, 1 /*windowed*/,
                                out, nullptr);
}
int TRT_GUEST_ABI devex_CreateOffscreenPlainSurfaceEx(void* self, u32 w, u32 h,
                                                      u32 fmt, u32 pool,
                                                      void** out, void** shared,
                                                      u32 usage) {
  (void)shared; (void)usage;
  return dev_CreateOffscreenPlainSurface(self, w, h, fmt, pool, out, nullptr);
}
int TRT_GUEST_ABI devex_CreateDepthStencilSurfaceEx(void* self, u32 w, u32 h,
                                                    u32 fmt, u32 multi, u32 msq,
                                                    int discard, void** out,
                                                    void** shared, u32 usage) {
  (void)shared; (void)usage;
  return dev_CreateDepthStencilSurface(self, w, h, fmt, multi, msq, discard,
                                       1 /*zenable*/, out);
}
int TRT_GUEST_ABI devex_ResetEx(void* self, void* pp, void* fullscreen_mode) {
  (void)fullscreen_mode;  // always windowed here
  return dev_Reset(self, pp);
}
int TRT_GUEST_ABI devex_GetDisplayModeEx(void*, u32, void* mode_out,
                                         void* rot_out) {
  if (!mode_out) return D3DERR_INVALIDCALL_;
  fill_display_mode_ex(mode_out, 21u /*D3DFMT_X8R8G8B8*/);
  if (rot_out) *(u32*)rot_out = 0;  // D3DDISPLAYROTATION_IDENTITY
  return D3D_OK_;
}

// ---- draws ----
int TRT_GUEST_ABI dev_DrawPrimitive(void*, u32 prim, u32 start_v, u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, g_stream_vb[0], 0, 0, start_v, count * 3 + 1, 0, count,
                 g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawIndexedPrimitive(void*, u32 prim, i32 base_idx,
                                           u32 min_idx, u32 num_verts,
                                           u32 start_idx, u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, g_stream_vb[0], g_index_ib, (u32)base_idx, min_idx,
                 num_verts, start_idx, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawPrimitiveUP(void*, u32 prim, u32 count,
                                      const void* verts, u32 stride) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw_up && verts && stride) {
    g_hooks.draw_up(prim, verts, stride, trt_d3d9_prim_verts(prim, count),
                    nullptr, 0, 0, count, g_hooks.user);
  } else if (g_hooks.draw) {
    g_hooks.draw(prim, 0, 0, 0, 0, count * 3 + 1, 0, count, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawIndexedPrimitiveUP(void*, u32 prim, u32 min_idx,
                                             u32 num_verts, u32 count,
                                             const void* indices, u32 idx_fmt,
                                             const void* verts, u32 stride) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw_up && verts && stride) {
    const u32 icount = indices ? trt_d3d9_prim_verts(prim, count) : 0;
    g_hooks.draw_up(prim, verts, stride, min_idx + num_verts, indices,
                    idx_fmt == 102 ? 1u : 0u, icount, count, g_hooks.user);
  } else if (g_hooks.draw) {
    g_hooks.draw(prim, 0, 0, 0, min_idx, num_verts, 0, count, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetStreamSource(void*, u32 stream, void* vb, u32 offset,
                                      u32 stride) {
  if (stream < 8u) {
    g_stream_vb_obj[stream] = (d9_obj*)vb;
    g_stream_vb[stream] = vb ? ((d9_obj*)vb)->handle : 0u;
    g_stream_offset[stream] = offset;
    g_stream_stride[stream] = stride;
  }
  if (g_hooks.set_stream_stride)
    g_hooks.set_stream_stride(stream, stride, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetStreamSource(void*, u32 stream, void** vb_out,
                                      u32* offset_out, u32* stride_out) {
  if (!vb_out || stream >= 8u) return D3DERR_INVALIDCALL_;
  *vb_out = g_stream_vb_obj[stream];
  if (*vb_out) ++((d9_obj*)*vb_out)->refcount;
  if (offset_out) *offset_out = g_stream_offset[stream];
  if (stride_out) *stride_out = g_stream_stride[stream];
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetIndices(void*, void* ib) {
  g_index_ib_obj = (d9_obj*)ib;
  g_index_ib = ib ? ((d9_obj*)ib)->handle : 0u;
  if (g_hooks.set_index_fmt && ib) {
    const u32 idx32 = (((d9_obj*)ib)->fmt == 102u) ? 1u : 0u;
    g_hooks.set_index_fmt(g_index_ib, idx32, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetIndices(void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_index_ib_obj;
  if (g_index_ib_obj) ++g_index_ib_obj->refcount;
  return D3D_OK_;
}

// ---- surface copies & fills (slots 30, 31, 32, 34, 35) ----
void copy_cpu_surface_data(d9_obj* dst, d9_obj* src) {
  if (!dst || !src) return;
  const gpu_obj* sg = gpu_at(src->handle);
  gpu_obj* dg = gpu_at(dst->handle);
  if (sg && dg && !sg->cpu_copy.empty()) {
    dg->cpu_copy = sg->cpu_copy;
  } else if (src->extra && (src->kind == 4 || src->kind == 9)) {
    const auto* sc = (const d9_surf_cpu*)src->extra;
    if (dg && !sc->pixels.empty()) dg->cpu_copy = sc->pixels;
    else if (dst->extra && (dst->kind == 4 || dst->kind == 9)) {
      ((d9_surf_cpu*)dst->extra)->pixels = sc->pixels;
    }
  } else if (sg && !sg->cpu_copy.empty() && dst->extra && dst->kind == 4) {
    ((d9_surf_cpu*)dst->extra)->pixels = sg->cpu_copy;
  }
}

int TRT_GUEST_ABI dev_UpdateSurface(void*, void* src_v, const void* srect_v,
                                    void* dst_v, const void* dpt_v) {
  if (!src_v || !dst_v) return D3DERR_INVALIDCALL_;
  d9_obj* src = (d9_obj*)src_v;
  d9_obj* dst = (d9_obj*)dst_v;
  copy_cpu_surface_data(dst, src);
  if (g_hooks.copy_rect) {
    const i32* sr = (const i32*)srect_v;
    const i32* dp = (const i32*)dpt_v;
    const int sx = sr ? sr[0] : 0;
    const int sy = sr ? sr[1] : 0;
    const int sw = sr ? (sr[2] - sr[0]) : (int)src->w;
    const int sh = sr ? (sr[3] - sr[1]) : (int)src->h;
    const int dx = dp ? dp[0] : 0;
    const int dy = dp ? dp[1] : 0;
    g_hooks.copy_rect(dst->handle, dst->sub_level, dx, dy, src->handle,
                      src->sub_level, sx, sy, sw, sh, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_UpdateTexture(void*, void* src_v, void* dst_v) {
  if (!src_v || !dst_v) return D3DERR_INVALIDCALL_;
  d9_obj* src = (d9_obj*)src_v;
  d9_obj* dst = (d9_obj*)dst_v;
  copy_cpu_surface_data(dst, src);
  if (g_hooks.copy_rect) {
    g_hooks.copy_rect(dst->handle, 0, 0, 0, src->handle, 0, 0, 0,
                      (int)src->w, (int)src->h, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetRenderTargetData(void*, void* src_v, void* dst_v) {
  if (!src_v || !dst_v) return D3DERR_INVALIDCALL_;
  d9_obj* src = (d9_obj*)src_v;
  d9_obj* dst = (d9_obj*)dst_v;
  copy_cpu_surface_data(dst, src);
  if (g_hooks.copy_rect) {
    g_hooks.copy_rect(dst->handle, dst->sub_level, 0, 0, src->handle,
                      src->sub_level, 0, 0, (int)src->w, (int)src->h,
                      g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_StretchRect(void*, void* src_v, const void* srect_v,
                                  void* dst_v, const void* drect_v, u32) {
  if (!src_v || !dst_v) return D3DERR_INVALIDCALL_;
  d9_obj* src = (d9_obj*)src_v;
  d9_obj* dst = (d9_obj*)dst_v;
  copy_cpu_surface_data(dst, src);
  if (g_hooks.copy_rect) {
    const i32* sr = (const i32*)srect_v;
    const i32* dr = (const i32*)drect_v;
    g_hooks.copy_rect(dst->handle, dst->sub_level, dr ? dr[0] : 0,
                      dr ? dr[1] : 0, src->handle, src->sub_level,
                      sr ? sr[0] : 0, sr ? sr[1] : 0,
                      sr ? (sr[2] - sr[0]) : (int)src->w,
                      sr ? (sr[3] - sr[1]) : (int)src->h, g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_ColorFill(void*, void* surf_v, const void* rect_v,
                                u32 color) {
  if (!surf_v) return D3DERR_INVALIDCALL_;
  d9_obj* s = (d9_obj*)surf_v;
  const i32* r = (const i32*)rect_v;
  const int rx = r ? r[0] : 0;
  const int ry = r ? r[1] : 0;
  const int rw = r ? (r[2] - r[0]) : (int)s->w;
  const int rh = r ? (r[3] - r[1]) : (int)s->h;
  if (s->extra && s->kind == 4) {
    auto* cpu = (d9_surf_cpu*)s->extra;
    if (!cpu->pixels.empty() && cpu->pitch >= 4u) {
      for (int y = ry; y < ry + rh && y < (int)cpu->h; ++y) {
        if (y < 0) continue;
        u32* row = (u32*)(cpu->pixels.data() + (size_t)y * cpu->pitch);
        for (int x = rx; x < rx + rw && x < (int)cpu->w; ++x) {
          if (x >= 0) row[x] = color;
        }
      }
    }
  }
  if (g_hooks.fill_rect) {
    g_hooks.fill_rect(s->handle, s->sub_level, rx, ry, rw, rh, color,
                      g_hooks.user);
  }
  return D3D_OK_;
}

// ---- locks. With a host backend bound the backend owns the memory; the
// hook returns the pointer. Offline, a shared scratch buffer serves so the
// guest can write and read back. ----
int TRT_GUEST_ABI buf_Lock(void* self, u32 offset, u32 bytes, void** data,
                           u32) {
  if (!data) return D3DERR_INVALIDCALL_;
  if (g_hooks.lock) {
    *data = g_hooks.lock(((d9_obj*)self)->handle, 0, offset, bytes, g_hooks.user);
    if (*data) return D3D_OK_;
  }
  if (offset >= sizeof(g_scratch)) return D3DERR_INVALIDCALL_;
  if (bytes == 0 || offset + bytes > sizeof(g_scratch))
    bytes = (u32)sizeof(g_scratch) - offset;
  *data = g_scratch + offset;
  return D3D_OK_;
}
int TRT_GUEST_ABI buf_Unlock(void* self) {
  if (g_hooks.unlock) g_hooks.unlock(((d9_obj*)self)->handle, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_LockRect(void* self, u32 level, void* lr, const void*,
                               u32) {
  if (!lr) return D3DERR_INVALIDCALL_;
  // D3DLOCKED_RECT { INT Pitch; void* pBits; } — x64: pBits at +8
  d9_obj* o = (d9_obj*)self;
  i32* p = (i32*)lr;
  const u32 mw = (o->w && level < 32) ? ((o->w >> level) ? (o->w >> level) : 1u) : 0u;
  const u32 mh = (o->h && level < 32) ? ((o->h >> level) ? (o->h >> level) : 1u) : 1u;
  const i32 pitch = trt_d3d9_row_pitch(mw, o->fmt);
  void* bits = nullptr;
  if (g_hooks.lock) {
    bits = g_hooks.lock(o->handle, level, 0, 0, g_hooks.user);
  }
  if (!bits) {
    if (gpu_obj* g = gpu_at(o->handle)) {
      const size_t need = (size_t)(pitch > 0 ? pitch : 4096) * mh;
      if (need > 0 && need <= (64u << 20)) {
        if (g->cpu_copy.size() < need) g->cpu_copy.resize(need, 0);
        g->lock_level = level;
        bits = g->cpu_copy.data();
      }
    }
    if (!bits) bits = g_scratch;
  }
  p[0] = pitch;
  *(void**)(p + 2) = bits;
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_UnlockRect(void* self, u32 level) {
  d9_obj* o = (d9_obj*)self;
  if (g_hooks.unlock) g_hooks.unlock(o->handle, g_hooks.user);
  if (g_hooks.unlock_sub) {
    if (const gpu_obj* g = gpu_at(o->handle)) {
      const u32 mw = (o->w && level < 32) ? ((o->w >> level) ? (o->w >> level) : 1u) : 0u;
      const i32 pitch = trt_d3d9_row_pitch(mw, o->fmt);
      if (!g->cpu_copy.empty()) {
        g_hooks.unlock_sub(o->handle, o->sub_face, level, (u32)pitch, 0u,
                           g->cpu_copy.data(), (u32)g->cpu_copy.size(),
                           g_hooks.user);
      }
    }
  }
  return D3D_OK_;
}

// ---- texture/resource shared slots ----
int TRT_GUEST_ABI res_GetDevice(void*, void** out) {
  // Prefer the Ex device when one exists: its vtable is a superset of the
  // plain device's, so plain callers still work while Ex callers (XNA calls
  // GetDevice on its textures, then ResetEx on the result) don't overrun a
  // 119-slot vtable.
  d9_obj* dev = g_devex ? g_devex : g_dev;
  if (!out || !dev) return D3DERR_INVALIDCALL_;
  ++dev->refcount;
  *out = dev;
  return D3D_OK_;
}
int TRT_GUEST_ABI res_SetPrivateData(void*, const void*, const void*, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI res_GetPrivateData(void*, const void*, void*, u32*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI res_FreePrivateData(void*, const void*) { return D3D_OK_; }
u32 TRT_GUEST_ABI res_SetPriority(void*, u32 p) { return p; }
u32 TRT_GUEST_ABI res_GetPriority(void*) { return 0; }
void TRT_GUEST_ABI res_PreLoad(void*) {}
u32 TRT_GUEST_ABI res_GetType(void* self) {
  // D3DRESOURCETYPE (d3d9types.h): SURFACE=1, TEXTURE=3, VERTEXBUFFER=6,
  // INDEXBUFFER=7, VOLUMETEXTURE=4, CUBETEXTURE=5, VOLUME=2
  const d9_obj* o = (d9_obj*)self;
  switch (o->kind) {
    case 1: return 3;  // D3DRTYPE_TEXTURE
    case 2: return 6;  // D3DRTYPE_VERTEXBUFFER
    case 3: return 7;  // D3DRTYPE_INDEXBUFFER
    case 4: return 1;  // D3DRTYPE_SURFACE
    case 8: return 5;  // D3DRTYPE_CUBETEXTURE
    case 9: return 4;  // D3DRTYPE_VOLUMETEXTURE
    case 10: return 2; // D3DRTYPE_VOLUME
    default: return 3;
  }
}
u32 TRT_GUEST_ABI tex_SetLOD(void*, u32 lod) { return lod; }
u32 TRT_GUEST_ABI tex_GetLOD(void*) { return 0; }
u32 TRT_GUEST_ABI tex_GetLevelCount(void* self) {
  d9_obj* o = (d9_obj*)self;
  const gpu_obj* g = gpu_at(o->handle);
  return g ? g->levels : (o->sub_level ? o->sub_level : 1u);
}
u32 TRT_GUEST_ABI tex_SetAutoGenFilterType(void*, u32 t) { return t; }
u32 TRT_GUEST_ABI tex_GetAutoGenFilterType(void*) {
  return 2;  // D3DTEXF_LINEAR
}
void TRT_GUEST_ABI tex_GenerateMipSubLevels(void*) {}
int TRT_GUEST_ABI tex_GetLevelDesc(void* self, u32 level, void* desc) {
  if (!desc) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  const u32 lv_cnt = tex_GetLevelCount(self);
  if (level >= lv_cnt) return D3DERR_INVALIDCALL_;
  const gpu_obj* g = gpu_at(o->handle);
  u32 mw = (o->w >> level) ? (o->w >> level) : 1u;
  u32 mh = (o->h >> level) ? (o->h >> level) : 1u;
  if (!o->w) mw = 0;
  if (!o->h) mh = 0;
  u32* d = (u32*)desc;
  d[0] = o->fmt;
  d[1] = 1;  // D3DRTYPE_SURFACE
  d[2] = g ? g->usage : 0u;
  d[3] = 0;  // D3DPOOL_DEFAULT
  d[4] = 0;  // MultiSampleType
  d[5] = 0;  // MultiSampleQuality
  d[6] = mw;
  d[7] = mh;
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_GetSurfaceLevel(void* self, u32 level, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* parent = (d9_obj*)self;
  const u32 lv_cnt = tex_GetLevelCount(self);
  if (level >= lv_cnt) { *out = nullptr; return D3DERR_INVALIDCALL_; }
  u32 mw = (parent->w >> level) ? (parent->w >> level) : 1u;
  u32 mh = (parent->h >> level) ? (parent->h >> level) : 1u;
  if (!parent->w) mw = 0;
  if (!parent->h) mh = 0;
  d9_obj* sub = obj_alloc(g_surf9_vtbl, parent->handle, 4, mw, mh, parent->fmt);
  if (!sub) return D3DERR_NOTIMPL_;
  ++parent->refcount;
  sub->parent_tex = parent;
  sub->sub_level = level;
  *out = sub;
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_AddDirtyRect(void*, const void*) { return D3D_OK_; }

// Cube texture methods
int TRT_GUEST_ABI cube_GetCubeMapSurface(void* self, u32 face, u32 level,
                                         void** out) {
  if (!out || face >= 6u) return D3DERR_INVALIDCALL_;
  d9_obj* parent = (d9_obj*)self;
  const u32 lv_cnt = tex_GetLevelCount(self);
  if (level >= lv_cnt) { *out = nullptr; return D3DERR_INVALIDCALL_; }
  u32 mw = (parent->w >> level) ? (parent->w >> level) : 1u;
  u32 mh = (parent->h >> level) ? (parent->h >> level) : 1u;
  d9_obj* sub = obj_alloc(g_surf9_vtbl, parent->handle, 4, mw, mh, parent->fmt);
  if (!sub) return D3DERR_NOTIMPL_;
  ++parent->refcount;
  sub->parent_tex = parent;
  sub->sub_face = face;
  sub->sub_level = level;
  *out = sub;
  return D3D_OK_;
}
int TRT_GUEST_ABI cube_LockRect(void* self, u32 face, u32 level, void* lr,
                                const void* r, u32 flags) {
  ((d9_obj*)self)->sub_face = face;
  return tex_LockRect(self, level, lr, r, flags);
}
int TRT_GUEST_ABI cube_UnlockRect(void* self, u32 face, u32 level) {
  ((d9_obj*)self)->sub_face = face;
  return tex_UnlockRect(self, level);
}
int TRT_GUEST_ABI cube_AddDirtyRect(void*, u32, const void*) { return D3D_OK_; }

// Volume & VolumeTexture methods
int TRT_GUEST_ABI voltex_GetLevelDesc(void* self, u32 level, void* desc) {
  if (!desc) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  const u32 lv_cnt = tex_GetLevelCount(self);
  if (level >= lv_cnt) return D3DERR_INVALIDCALL_;
  const gpu_obj* g = gpu_at(o->handle);
  const u32 d0 = g ? g->depth : (o->sub_face ? o->sub_face : 1u);
  u32* out = (u32*)desc;
  out[0] = o->fmt;
  out[1] = 2u;  // D3DRTYPE_VOLUME
  out[2] = g ? g->usage : 0u;
  out[3] = 0u;
  out[4] = (o->w >> level) ? (o->w >> level) : 1u;
  out[5] = (o->h >> level) ? (o->h >> level) : 1u;
  out[6] = (d0 >> level) ? (d0 >> level) : 1u;
  return D3D_OK_;
}
int TRT_GUEST_ABI vol_GetDesc(void* self, void* desc) {
  d9_obj* o = (d9_obj*)self;
  return o->parent_tex ? voltex_GetLevelDesc(o->parent_tex, o->sub_level, desc)
                       : D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI voltex_LockBox(void* self, u32 level, void* lb, const void*,
                                 u32) {
  if (!lb) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  const u32 mw = (o->w >> level) ? (o->w >> level) : 1u;
  const u32 mh = (o->h >> level) ? (o->h >> level) : 1u;
  const u32 rp = (u32)trt_d3d9_row_pitch(mw, o->fmt);
  const u32 sp = rp * mh;
  void* bits = g_scratch;
  if (d9_surf_cpu* cpu = (d9_surf_cpu*)o->extra) {
    const size_t need = (size_t)sp * (cpu->depth ? cpu->depth : 1u);
    if (need > 0 && need <= (64u << 20)) {
      if (cpu->pixels.size() < need) cpu->pixels.resize(need, 0);
      bits = cpu->pixels.data();
    }
  }
  u32* p = (u32*)lb;
  p[0] = rp;
  p[1] = sp;
  *(void**)(p + 2) = bits;
  return D3D_OK_;
}
int TRT_GUEST_ABI voltex_UnlockBox(void* self, u32 level) {
  d9_obj* o = (d9_obj*)self;
  if (g_hooks.unlock_sub && o && o->handle) {
    if (const d9_surf_cpu* cpu = (const d9_surf_cpu*)o->extra) {
      if (!cpu->pixels.empty()) {
        const u32 mw = (o->w >> level) ? (o->w >> level) : 1u;
        const u32 mh = (o->h >> level) ? (o->h >> level) : 1u;
        const u32 rp = (u32)trt_d3d9_row_pitch(mw, o->fmt);
        const u32 sp = rp * mh;
        g_hooks.unlock_sub(o->handle, 0u, level, rp, sp, cpu->pixels.data(),
                           (u32)cpu->pixels.size(), g_hooks.user);
      }
    }
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI voltex_AddDirtyBox(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI vol_LockBox(void* self, void* lb, const void* box, u32 flags) {
  d9_obj* o = (d9_obj*)self;
  return o->parent_tex ? voltex_LockBox(o->parent_tex, o->sub_level, lb, box, flags)
                       : D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI vol_UnlockBox(void* self) {
  d9_obj* o = (d9_obj*)self;
  return o->parent_tex ? voltex_UnlockBox(o->parent_tex, o->sub_level)
                       : D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI voltex_GetVolumeLevel(void* self, u32 level, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* parent = (d9_obj*)self;
  if (level >= tex_GetLevelCount(self)) { *out = nullptr; return D3DERR_INVALIDCALL_; }
  d9_obj* sub = obj_alloc(g_vol9_vtbl, parent->handle, 10, parent->w, parent->h, parent->fmt);
  if (!sub) return D3DERR_NOTIMPL_;
  ++parent->refcount;
  sub->parent_tex = parent;
  sub->sub_level = level;
  *out = sub;
  return D3D_OK_;
}

int TRT_GUEST_ABI shader_GetFunction(void* self, void* data, u32* size_out) {
  if (!size_out) return D3DERR_INVALIDCALL_;
  const d9_obj* o = (const d9_obj*)self;
  const d9_shader_blob* sb = (const d9_shader_blob*)o->extra;
  if (!sb) return D3DERR_NOTIMPL_;
  const u32 sz = (u32)(sb->bytecode.size() * 4u);
  if (!data) {
    *size_out = sz;
    return D3D_OK_;
  }
  if (*size_out < sz) {
    *size_out = sz;
    return D3DERR_INVALIDCALL_;
  }
  if (sz > 0) memcpy(data, sb->bytecode.data(), sz);
  *size_out = sz;
  return D3D_OK_;
}
int TRT_GUEST_ABI decl_GetDeclaration(void* self, void* elems_out,
                                      u32* count_out) {
  if (!count_out) return D3DERR_INVALIDCALL_;
  const d9_obj* o = (const d9_obj*)self;
  const d9_decl_blob* db = (const d9_decl_blob*)o->extra;
  if (!db) return D3DERR_NOTIMPL_;
  const u32 cnt = (u32)db->elems.size();
  *count_out = cnt;
  if (elems_out && cnt > 0) {
    memcpy(elems_out, db->elems.data(), cnt * sizeof(d3d9_vert_elem));
  }
  return D3D_OK_;
}

// ---- remaining device slots (only genuinely absent legacy/query slots stubbed) ----
#define DEV_STUB_FN(n, name)                                              \
  int TRT_GUEST_ABI dev_stub_##n(void* self) {                            \
    (void)self;                                                           \
    char line[96];                                                        \
    snprintf(line, sizeof(line), "d3d9: device slot %u (%s) called "      \
                                 "(unimplemented)", n, name);             \
    trt_core_log(line);                                                   \
    return D3DERR_NOTIMPL_;                                               \
  }

DEV_STUB_FN(9, "GetCreationParameters")
DEV_STUB_FN(13, "CreateAdditionalSwapChain")
DEV_STUB_FN(14, "GetSwapChain")
DEV_STUB_FN(19, "GetRasterStatus")
DEV_STUB_FN(22, "GetGammaRamp")
DEV_STUB_FN(33, "GetFrontBufferData")
DEV_STUB_FN(50, "GetMaterial")
DEV_STUB_FN(52, "GetLight")
DEV_STUB_FN(54, "GetLightEnable")
DEV_STUB_FN(56, "GetClipPlane")
DEV_STUB_FN(63, "GetClipStatus")
DEV_STUB_FN(66, "GetTextureStageState")
DEV_STUB_FN(68, "GetSamplerState")
DEV_STUB_FN(72, "GetPaletteEntries")
DEV_STUB_FN(74, "GetCurrentTexturePalette")
DEV_STUB_FN(85, "ProcessVertices")
DEV_STUB_FN(115, "DrawRectPatch")
DEV_STUB_FN(116, "DrawTriPatch")
DEV_STUB_FN(117, "DeletePatch")
DEV_STUB_FN(118, "CreateQuery")
int TRT_GUEST_ABI dev_SetDialogBoxMode(void*, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetGammaRamp(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetMaterial(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetLight(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_LightEnable(void*, u32, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetClipPlane(void*, u32, const float*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetClipStatus(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_ValidateDevice(void*, u32*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetPaletteEntries(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetCurrentTexturePalette(void*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetScissorRect(void*, const void* rect_v) {
  if (!rect_v) return D3DERR_INVALIDCALL_;
  memcpy(g_scissor, rect_v, 16);
  if (g_hooks.set_scissor) {
    g_hooks.set_scissor(g_rs[174] != 0u ? 1 : 0, g_scissor[0], g_scissor[1],
                        g_scissor[2], g_scissor[3], g_hooks.user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetScissorRect(void*, void* rect_v) {
  if (!rect_v) return D3DERR_INVALIDCALL_;
  memcpy(rect_v, g_scissor, 16);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetSoftwareVertexProcessing(void*, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetSoftwareVertexProcessing(void*) { return 0; }
void TRT_GUEST_ABI dev_SetNPatchMode(void*, float) {}
float TRT_GUEST_ABI dev_GetNPatchMode(void*) { return 0.f; }
int TRT_GUEST_ABI dev_GetFVF(void*, u32* out) {
  if (out) *out = g_cur_fvf;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexShaderConstantI(void*, u32 start, const int* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(g_vs_const_i + start * 4u, d, n * 16u);
  }
  if (g_hooks.set_shader_const_i)
    g_hooks.set_shader_const_i(0u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetVertexShaderConstantI(void*, u32 start, int* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(d, g_vs_const_i + start * 4u, n * 16u);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexShaderConstantB(void*, u32 start, const int* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(g_vs_const_b + start, d, n * 4u);
  }
  if (g_hooks.set_shader_const_b)
    g_hooks.set_shader_const_b(0u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetVertexShaderConstantB(void*, u32 start, int* d,
                                               u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(d, g_vs_const_b + start, n * 4u);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetStreamSourceFreq(void*, u32 stream, u32 setting) {
  if (stream < 8u) g_stream_freq[stream] = setting;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetStreamSourceFreq(void*, u32 stream, u32* out) {
  if (!out || stream >= 8u) return D3DERR_INVALIDCALL_;
  *out = g_stream_freq[stream];
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetPixelShaderConstantI(void*, u32 start, const int* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(g_ps_const_i + start * 4u, d, n * 16u);
  }
  if (g_hooks.set_shader_const_i)
    g_hooks.set_shader_const_i(1u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetPixelShaderConstantI(void*, u32 start, int* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(d, g_ps_const_i + start * 4u, n * 16u);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetPixelShaderConstantB(void*, u32 start, const int* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(g_ps_const_b + start, d, n * 4u);
  }
  if (g_hooks.set_shader_const_b)
    g_hooks.set_shader_const_b(1u, start, d, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetPixelShaderConstantB(void*, u32 start, int* d,
                                              u32 count) {
  if (!d) return D3DERR_INVALIDCALL_;
  if (start < 32u) {
    const u32 n = (start + count <= 32u) ? count : (32u - start);
    memcpy(d, g_ps_const_b + start, n * 4u);
  }
  return D3D_OK_;
}
u32 TRT_GUEST_ABI dev_GetNumberOfSwapChains(void*) { return 1u; }

// ---- Ex vtables (built once from the base vtables) ----
// IDirect3D9Ex = IDirect3D9 (17 slots) + 5 Ex slots; IDirect3DDevice9Ex =
// IDirect3DDevice9 (119 slots) + 14 Ex slots. Copying the base entries (not
// re-listing them) keeps the two vtables in lock-step by construction.
bool g_ex_vtbls_ready = false;
void ensure_ex_vtbls() {
  if (g_ex_vtbls_ready) return;
  memcpy((void*)g_d3d9ex_vtbl, g_d3d9_vtbl, sizeof(g_d3d9_vtbl));
  g_d3d9ex_vtbl[17] = (const void*)&d9_GetAdapterModeCountEx;
  g_d3d9ex_vtbl[18] = (const void*)&d9_EnumAdapterModesEx;
  g_d3d9ex_vtbl[19] = (const void*)&d9_GetAdapterDisplayModeEx;
  g_d3d9ex_vtbl[20] = (const void*)&d9_CreateDeviceEx;
  g_d3d9ex_vtbl[21] = (const void*)&d9_GetAdapterLUID;
  memcpy((void*)g_dev9ex_vtbl, g_dev9_vtbl, sizeof(g_dev9_vtbl));
  g_dev9ex_vtbl[119] = (const void*)&devex_SetConvolutionMonoKernel;
  g_dev9ex_vtbl[120] = (const void*)&devex_ComposeRects;
  g_dev9ex_vtbl[121] = (const void*)&devex_PresentEx;
  g_dev9ex_vtbl[122] = (const void*)&devex_GetGPUThreadPriority;
  g_dev9ex_vtbl[123] = (const void*)&devex_SetGPUThreadPriority;
  g_dev9ex_vtbl[124] = (const void*)&devex_WaitForVBlank;
  g_dev9ex_vtbl[125] = (const void*)&devex_CheckResourceResidency;
  g_dev9ex_vtbl[126] = (const void*)&devex_SetMaximumFrameLatency;
  g_dev9ex_vtbl[127] = (const void*)&devex_CheckDeviceState;
  g_dev9ex_vtbl[128] = (const void*)&devex_CreateRenderTargetEx;
  g_dev9ex_vtbl[129] = (const void*)&devex_CreateOffscreenPlainSurfaceEx;
  g_dev9ex_vtbl[130] = (const void*)&devex_CreateDepthStencilSurfaceEx;
  g_dev9ex_vtbl[131] = (const void*)&devex_ResetEx;
  g_dev9ex_vtbl[132] = (const void*)&devex_GetDisplayModeEx;
  g_ex_vtbls_ready = true;
}

// ---- vtables (SDK order) ----
const void* g_d3d9_vtbl[17] = {
    (const void*)&d9_QI,
    (const void*)&d9_AR,
    (const void*)&d9_Rel,
    (const void*)&d9_RegisterSoftwareDevice,          // 3
    (const void*)&d9_GetAdapterCount,                 // 4
    (const void*)&d9_GetAdapterIdentifier,            // 5
    (const void*)&d9_GetAdapterModeCount,             // 6
    (const void*)&d9_EnumAdapterModes,                // 7
    (const void*)&d9_GetAdapterDisplayMode,           // 8
    (const void*)&d9_CheckDeviceType,                 // 9
    (const void*)&d9_CheckDeviceFormat,               // 10
    (const void*)&d9_CheckDeviceMultiSampleType,      // 11
    (const void*)&d9_CheckDepthStencilMatch,          // 12
    (const void*)&d9_CheckDeviceFormatConversion,     // 13
    (const void*)&d9_GetDeviceCaps,                   // 14
    (const void*)&d9_GetAdapterMonitor,               // 15
    (const void*)&d9_CreateDevice,                    // 16
};

// Ex vtables: zero-initialised here, filled by ensure_ex_vtbls() before
// any Ex object is handed out (see above).
const void* g_d3d9ex_vtbl[22];

const void* g_dev9_vtbl[119] = {
    (const void*)&d9_QI,                              // 0
    (const void*)&d9_AR,                              // 1
    (const void*)&d9_Rel,                             // 2
    (const void*)&dev_TestCooperativeLevel,           // 3
    (const void*)&dev_GetAvailableTextureMem,         // 4
    (const void*)&dev_EvictManagedResources,          // 5
    (const void*)&dev_GetDirect3D,                    // 6
    (const void*)&dev_GetDeviceCaps,                  // 7
    (const void*)&dev_GetDisplayMode,                 // 8
    (const void*)&dev_stub_9,          // 9
    (const void*)&dev_SetCursorProperties,            // 10
    (const void*)&dev_SetCursorPosition,              // 11
    (const void*)&dev_ShowCursor,                     // 12
    (const void*)&dev_stub_13,      // 13
    (const void*)&dev_stub_14,                   // 14
    (const void*)&dev_GetNumberOfSwapChains,          // 15
    (const void*)&dev_Reset,                          // 16
    (const void*)&dev_Present,                        // 17
    (const void*)&dev_GetBackBuffer,                  // 18
    (const void*)&dev_stub_19,                // 19
    (const void*)&dev_SetDialogBoxMode,               // 20
    (const void*)&dev_SetGammaRamp,                   // 21
    (const void*)&dev_stub_22,                   // 22
    (const void*)&dev_CreateTexture,                  // 23
    (const void*)&dev_CreateVolumeTexture,            // 24
    (const void*)&dev_CreateCubeTexture,              // 25
    (const void*)&dev_CreateVertexBuffer,             // 26
    (const void*)&dev_CreateIndexBuffer,              // 27
    (const void*)&dev_CreateRenderTarget,             // 28
    (const void*)&dev_CreateDepthStencilSurface,      // 29
    (const void*)&dev_UpdateSurface,                  // 30
    (const void*)&dev_UpdateTexture,                  // 31
    (const void*)&dev_GetRenderTargetData,            // 32
    (const void*)&dev_stub_33,             // 33
    (const void*)&dev_StretchRect,                    // 34
    (const void*)&dev_ColorFill,                      // 35
    (const void*)&dev_CreateOffscreenPlainSurface,    // 36
    (const void*)&dev_SetRenderTarget,                // 37
    (const void*)&dev_GetRenderTarget,                // 38
    (const void*)&dev_SetDepthStencilSurface,         // 39
    (const void*)&dev_GetDepthStencilSurface,         // 40
    (const void*)&dev_BeginScene,                     // 41
    (const void*)&dev_EndScene,                       // 42
    (const void*)&dev_Clear,                          // 43
    (const void*)&dev_SetTransform,                   // 44
    (const void*)&dev_GetTransform,                   // 45
    (const void*)&dev_MultiplyTransform,              // 46
    (const void*)&dev_SetViewport,                    // 47
    (const void*)&dev_GetViewport,                    // 48
    (const void*)&dev_SetMaterial,                    // 49
    (const void*)&dev_stub_50,                    // 50
    (const void*)&dev_SetLight,                       // 51
    (const void*)&dev_stub_52,                       // 52
    (const void*)&dev_LightEnable,                    // 53
    (const void*)&dev_stub_54,  // 54 GetLightEnable
    (const void*)&dev_SetClipPlane,                   // 55
    (const void*)&dev_stub_56,                   // 56
    (const void*)&dev_SetRenderState,                 // 57
    (const void*)&dev_GetRenderState,                 // 58
    (const void*)&dev_CreateStateBlock,               // 59
    (const void*)&dev_BeginStateBlock,                // 60
    (const void*)&dev_EndStateBlock,                  // 61
    (const void*)&dev_SetClipStatus,                  // 62
    (const void*)&dev_stub_63,                  // 63
    (const void*)&dev_GetTexture,                     // 64
    (const void*)&dev_SetTexture,                     // 65
    (const void*)&dev_stub_66,           // 66
    (const void*)&dev_SetTextureStageState,           // 67
    (const void*)&dev_stub_68,                // 68
    (const void*)&dev_SetSamplerState,                // 69
    (const void*)&dev_ValidateDevice,                 // 70
    (const void*)&dev_SetPaletteEntries,              // 71
    (const void*)&dev_stub_72,              // 72
    (const void*)&dev_SetCurrentTexturePalette,       // 73
    (const void*)&dev_stub_74,       // 74
    (const void*)&dev_SetScissorRect,                 // 75
    (const void*)&dev_GetScissorRect,                 // 76
    (const void*)&dev_SetSoftwareVertexProcessing,    // 77
    (const void*)&dev_GetSoftwareVertexProcessing,    // 78
    (const void*)&dev_SetNPatchMode,                  // 79
    (const void*)&dev_GetNPatchMode,                  // 80
    (const void*)&dev_DrawPrimitive,                  // 81
    (const void*)&dev_DrawIndexedPrimitive,           // 82
    (const void*)&dev_DrawPrimitiveUP,                // 83
    (const void*)&dev_DrawIndexedPrimitiveUP,         // 84
    (const void*)&dev_stub_85,                // 85
    (const void*)&dev_CreateVertexDeclaration,        // 86
    (const void*)&dev_SetVertexDeclaration,           // 87
    (const void*)&dev_GetVertexDeclaration,           // 88
    (const void*)&dev_SetFVF,                         // 89
    (const void*)&dev_GetFVF,                         // 90
    (const void*)&dev_CreateVertexShader,             // 91
    (const void*)&dev_SetVertexShader,                // 92
    (const void*)&dev_GetVertexShader,                // 93
    (const void*)&dev_SetVertexShaderConstantF,       // 94
    (const void*)&dev_GetVertexShaderConstantF,       // 95
    (const void*)&dev_SetVertexShaderConstantI,       // 96
    (const void*)&dev_GetVertexShaderConstantI,       // 97
    (const void*)&dev_SetVertexShaderConstantB,       // 98
    (const void*)&dev_GetVertexShaderConstantB,       // 99
    (const void*)&dev_SetStreamSource,                // 100
    (const void*)&dev_GetStreamSource,                // 101
    (const void*)&dev_SetStreamSourceFreq,            // 102
    (const void*)&dev_GetStreamSourceFreq,            // 103
    (const void*)&dev_SetIndices,                     // 104
    (const void*)&dev_GetIndices,                     // 105
    (const void*)&dev_CreatePixelShader,              // 106
    (const void*)&dev_SetPixelShader,                 // 107
    (const void*)&dev_GetPixelShader,                 // 108
    (const void*)&dev_SetPixelShaderConstantF,        // 109
    (const void*)&dev_GetPixelShaderConstantF,        // 110
    (const void*)&dev_SetPixelShaderConstantI,        // 111
    (const void*)&dev_GetPixelShaderConstantI,        // 112
    (const void*)&dev_SetPixelShaderConstantB,        // 113
    (const void*)&dev_GetPixelShaderConstantB,        // 114
    (const void*)&dev_stub_115,                  // 115
    (const void*)&dev_stub_116,                   // 116
    (const void*)&dev_stub_117,                    // 117
    (const void*)&dev_stub_118,                    // 118
};

const void* g_dev9ex_vtbl[133];

const void* g_tex9_vtbl[22] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&res_SetPrivateData,     // 4
    (const void*)&res_GetPrivateData,     // 5
    (const void*)&res_FreePrivateData,    // 6
    (const void*)&res_SetPriority,        // 7
    (const void*)&res_GetPriority,        // 8
    (const void*)&res_PreLoad,            // 9
    (const void*)&res_GetType,            // 10
    (const void*)&tex_SetLOD,             // 11
    (const void*)&tex_GetLOD,             // 12
    (const void*)&tex_GetLevelCount,      // 13
    (const void*)&tex_SetAutoGenFilterType,  // 14
    (const void*)&tex_GetAutoGenFilterType,  // 15
    (const void*)&tex_GenerateMipSubLevels,  // 16
    (const void*)&tex_GetLevelDesc,       // 17
    (const void*)&tex_GetSurfaceLevel,    // 18
    (const void*)&tex_LockRect,           // 19
    (const void*)&tex_UnlockRect,         // 20
    (const void*)&tex_AddDirtyRect,       // 21
};

const void* g_vb9_vtbl[13] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&buf_Lock,    // 11
    (const void*)&buf_Unlock,  // 12
};

const void* g_ib9_vtbl[13] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&buf_Lock, (const void*)&buf_Unlock,
};

const void* g_vs9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,
    (const void*)&shader_GetFunction,  // 4
};
const void* g_ps9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&shader_GetFunction,
};
const void* g_decl9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&decl_GetDeclaration,
};

// ---- IDirect3DSurface9 slots 11..16 ----
int TRT_GUEST_ABI surf_GetContainer(void* self, const void*, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  if (o->parent_tex) {
    d9_obj* parent = (d9_obj*)o->parent_tex;
    ++parent->refcount;
    *out = parent;
    return D3D_OK_;
  }
  *out = nullptr;
  return (int)0x80004002;  // E_NOINTERFACE
}
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MSType, MSQuality, Width, Height
int TRT_GUEST_ABI surf_GetDesc(void* self, void* desc) {
  if (!desc) return D3DERR_INVALIDCALL_;
  const d9_obj* o = (const d9_obj*)self;
  u32* d = (u32*)desc;
  d[0] = o->fmt;
  d[1] = 1;  // D3DRTYPE_SURFACE
  if (o->extra && o->kind == 4) {
    d[2] = 0;  // offscreen plain surface
    d[3] = 3;  // D3DPOOL_SYSTEMMEM
  } else if (o->parent_tex) {
    const gpu_obj* g = gpu_at(o->handle);
    d[2] = g ? g->usage : 0u;
    d[3] = 0;
  } else {
    d[2] = 1;  // D3DUSAGE_RENDERTARGET
    d[3] = 0;  // D3DPOOL_DEFAULT
  }
  d[4] = 0;  // D3DMULTISAMPLE_NONE
  d[5] = 0;
  d[6] = o->w;
  d[7] = o->h;
  return D3D_OK_;
}
// Texture-backed sub-surfaces (except RT/DS textures) forward LockRect to their
// parent texture; parentless render-target/backbuffer surfaces fail D3DERR_INVALIDCALL.
int TRT_GUEST_ABI surf_LockRect(void* self, void* lr, const void* r, u32 flags) {
  d9_obj* o = (d9_obj*)self;
  if (o->parent_tex) {
    d9_obj* p = (d9_obj*)o->parent_tex;
    const gpu_obj* g = gpu_at(p->handle);
    if (!g || !(g->usage & 3u)) {
      if (p->kind == 8)
        return cube_LockRect(p, o->sub_face, o->sub_level, lr, r, flags);
      return tex_LockRect(p, o->sub_level, lr, r, flags);
    }
  }
  if (lr) memset(lr, 0, 16);
  return D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI surf_UnlockRect(void* self) {
  d9_obj* o = (d9_obj*)self;
  if (o->parent_tex) {
    d9_obj* p = (d9_obj*)o->parent_tex;
    const gpu_obj* g = gpu_at(p->handle);
    if (!g || !(g->usage & 3u)) {
      if (p->kind == 8) return cube_UnlockRect(p, o->sub_face, o->sub_level);
      return tex_UnlockRect(p, o->sub_level);
    }
  }
  return D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI off_LockRect(void* self, void* lr, const void*, u32) {
  if (!lr) return D3DERR_INVALIDCALL_;
  memset(lr, 0, 16);
  d9_obj* o = (d9_obj*)self;
  d9_surf_cpu* cpu = (d9_surf_cpu*)o->extra;
  if (!cpu) return D3DERR_INVALIDCALL_;
  if (cpu->pixels.empty()) {
    const u32 p = cpu->pitch ? cpu->pitch : (u32)trt_d3d9_row_pitch(o->w, o->fmt);
    cpu->pitch = p;
    cpu->pixels.resize((size_t)p * (o->h ? o->h : 1u), 0);
  }
  i32* out = (i32*)lr;
  out[0] = (i32)cpu->pitch;
  *(void**)(out + 2) = cpu->pixels.data();
  return D3D_OK_;
}
int TRT_GUEST_ABI off_UnlockRect(void*) { return D3D_OK_; }
int TRT_GUEST_ABI surf_GetDC(void*, void** hdc) {
  if (hdc) *hdc = nullptr;
  return D3DERR_INVALIDCALL_;
}
int TRT_GUEST_ABI surf_ReleaseDC(void*, void*) { return D3DERR_INVALIDCALL_; }

const void* g_surf9_vtbl[17] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&surf_GetContainer, (const void*)&surf_GetDesc,    // 11, 12
    (const void*)&surf_LockRect, (const void*)&surf_UnlockRect,     // 13, 14
    (const void*)&surf_GetDC, (const void*)&surf_ReleaseDC,         // 15, 16
};

const void* g_offscreen_surf9_vtbl[17] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&surf_GetContainer, (const void*)&surf_GetDesc,    // 11, 12
    (const void*)&off_LockRect, (const void*)&off_UnlockRect,       // 13, 14
    (const void*)&surf_GetDC, (const void*)&surf_ReleaseDC,         // 15, 16
};

const void* g_voltex9_vtbl[22] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&res_SetPrivateData,     // 4
    (const void*)&res_GetPrivateData,     // 5
    (const void*)&res_FreePrivateData,    // 6
    (const void*)&res_SetPriority,        // 7
    (const void*)&res_GetPriority,        // 8
    (const void*)&res_PreLoad,            // 9
    (const void*)&res_GetType,            // 10
    (const void*)&tex_SetLOD,             // 11
    (const void*)&tex_GetLOD,             // 12
    (const void*)&tex_GetLevelCount,      // 13
    (const void*)&tex_SetAutoGenFilterType,  // 14
    (const void*)&tex_GetAutoGenFilterType,  // 15
    (const void*)&tex_GenerateMipSubLevels,  // 16
    (const void*)&voltex_GetLevelDesc,    // 17
    (const void*)&voltex_GetVolumeLevel,  // 18
    (const void*)&voltex_LockBox,         // 19
    (const void*)&voltex_UnlockBox,       // 20
    (const void*)&voltex_AddDirtyBox,     // 21
};

const void* g_cube9_vtbl[22] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&res_SetPrivateData,     // 4
    (const void*)&res_GetPrivateData,     // 5
    (const void*)&res_FreePrivateData,    // 6
    (const void*)&res_SetPriority,        // 7
    (const void*)&res_GetPriority,        // 8
    (const void*)&res_PreLoad,            // 9
    (const void*)&res_GetType,            // 10
    (const void*)&tex_SetLOD,             // 11
    (const void*)&tex_GetLOD,             // 12
    (const void*)&tex_GetLevelCount,      // 13
    (const void*)&tex_SetAutoGenFilterType,  // 14
    (const void*)&tex_GetAutoGenFilterType,  // 15
    (const void*)&tex_GenerateMipSubLevels,  // 16
    (const void*)&tex_GetLevelDesc,       // 17
    (const void*)&cube_GetCubeMapSurface, // 18
    (const void*)&cube_LockRect,          // 19
    (const void*)&cube_UnlockRect,        // 20
    (const void*)&cube_AddDirtyRect,      // 21
};

const void* g_vol9_vtbl[11] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&res_SetPrivateData,     // 4
    (const void*)&res_GetPrivateData,     // 5
    (const void*)&res_FreePrivateData,    // 6
    (const void*)&surf_GetContainer,      // 7
    (const void*)&vol_GetDesc,            // 8
    (const void*)&vol_LockBox,            // 9
    (const void*)&vol_UnlockBox,          // 10
};

const void* g_sb9_vtbl[6] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&sb_Capture,             // 4
    (const void*)&sb_Apply,               // 5
};

// D3DPERF_GetStatus: no profiling tools are attached on the console, so
// there is no perf status to report (Sekiro audit: sekiro.exe imports it
// for PIX markers).
u32 TRT_GUEST_ABI trt_D3DPERF_GetStatus(void) { return 0; }

int trt_bridge_register_d3d9(void) {
  ensure_ex_vtbls();  // Ex objects are servable from the first call
  static const trt_named_fn k_fns[] = {
      {"Direct3DCreate9", (void*)&trt_Direct3DCreate9},
      {"Direct3DCreate9Ex", (void*)&trt_Direct3DCreate9Ex},
      {"D3DPERF_GetStatus", (void*)&trt_D3DPERF_GetStatus},
  };
  trt_module_register("d3d9.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3d9core.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

u32 trt_d3d9_last_draw(void) { return g_last_draw; }

void trt_d3d9_set_hooks(const d3d9_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}
const d3d9_hooks* trt_d3d9_hooks_get(void) { return &g_hooks; }

}  // extern "C"
