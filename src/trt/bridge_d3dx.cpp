// tiramisu/src/trt/bridge_d3dx.cpp
// D3DX10/D3DX11 bridge (M7). See bridge_d3dx.h.
//
// ID3D11Device vtable slots (Windows SDK d3d11.h, ID3D11Device inherits
// IUnknown = 3 slots, then in declaration order):
//   0 QI  1 AddRef  2 Release
//   3 CreateBuffer          4 CreateTexture1D     5 CreateTexture2D
//   6 CreateTexture3D       7 CreateSRV           8 CreateUAV
//   9 CreateRTV            10 CreateDSV           11 CreateInputLayout
//  12 CreateVertexShader   13 CreateGS
//  14 CreateGSWithSO       15 CreatePS            16 CreateHS
//  17 CreateDS             18 CreateCS            19 CreateClassLinkage
//  20 CreateBlendState     21 CreateDSState       22 CreateRSState
//  23 CreateSamplerState   24 CreateQuery         25 CreatePredicate
//  26 CreateCounter        27 CreateDeferredContext
//  (28 OpenSharedResource, 29 CheckFormatSupport, ... 47 GetFeatureLevel)
//
// ID3D11DeviceContext (for the SRV-vs-sRGB story we don't need: the view
// desc is ours) — only used if the game passes a context; not needed.
//
// The texture upload: the guest's PNG/DDS bytes parse to RGBA8 through
// bridge_img, we fill a D3D11_TEXTURE2D_DESC + D3D11_SUBRESOURCE_DATA
// (both plain structs we define byte-exact) and call the device's
// CreateTexture2D slot directly — the call marshals as Win64 because the
// guest ABI is Win64 on both sides of this bridge.
//
// D3DCompile: the console's real d3dcompiler_47 is a SYSTEM-route DLL —
// forward by name (module table maps d3dcompiler_43/46/47 -> 47). What we
// add here is D3DX11CompileFromFile, which reads the file then calls the
// same pipeline: we read the bytes and hand them to the SYSTEM d3dcompiler
// through its real export (resolved at boot via GetProcAddress on the
// loaded module).
#include "bridge_d3dx.h"
#include "bridge_img.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

namespace {

constexpr int k_e_notimpl = 0x80004001;
constexpr int k_e_fail = 0x80004005;
constexpr int k_e_invalidarg = 0x80070057;

// unique name: bridge_img.cpp also defines log_line (extern "C" +
// anonymous namespace still emits a global symbol name on this toolchain)
namespace d3dxlog {
void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) d3dxlog::line(s_)

// ---------------------------------------------------------------------------
// ID3D11Device vtable call helpers (the guest is Win64; the host device's
// vtable is the console's real one — same ABI, direct call)
// ---------------------------------------------------------------------------
using create_tex2d_t = int (TRT_GUEST_ABI *)(void* device,
                                             const void* desc,
                                             const void* initial_data,
                                             void** out_tex);
using create_srv_t = int (TRT_GUEST_ABI *)(void* device, void* resource,
                                           const void* desc, void** out_view);
using get_feature_level_t = u32 (TRT_GUEST_ABI *)(void* device);

void* dev_vtbl(void* device) { return *(void**)device; }

int dev_create_texture2d(void* device, const void* desc,
                         const void* initial, void** out_tex) {
  void* v = dev_vtbl(device);
  if (!v) return k_e_fail;
  return ((create_tex2d_t)((void**)v)[5])(device, desc, initial, out_tex);
}
int dev_create_srv(void* device, void* resource, const void* desc,
                   void** out_view) {
  void* v = dev_vtbl(device);
  if (!v) return k_e_fail;
  return ((create_srv_t)((void**)v)[7])(device, resource, desc, out_view);
}

// ---------------------------------------------------------------------------
// D3D11_TEXTURE2D_DESC (byte-exact, x64: 4x u32 + DXGI_FORMAT u32 +
// 2x u32 + 4x u32 = 48 bytes) and D3D11_SUBRESOURCE_DATA (2 pointers + u32
// + pad = 24 bytes). We build them by hand so no Windows header is needed.
// ---------------------------------------------------------------------------
struct tex2d_desc {
  u32 width;
  u32 height;
  u32 mip_levels;
  u32 array_size;
  u32 format;        // DXGI_FORMAT: B8G8R8A8_UNORM = 87, R8G8B8A8_UNORM = 28
  u32 sample_count;
  u32 sample_quality;
  u32 usage;         // D3D11_USAGE_IMMUTABLE = 1 (initial data present)
  u32 bind_flags;    // D3D11_BIND_SHADER_RESOURCE = 8
  u32 cpu_access;    // 0
  u32 misc;          // 0
};
static_assert(sizeof(tex2d_desc) == 44, "D3D11_TEXTURE2D_DESC layout");

struct subres_data {
  const void* p_sys_mem;
  u32 sys_mem_pitch;
  u32 sys_mem_slice_pitch;
};
static_assert(sizeof(subres_data) == 16, "D3D11_SUBRESOURCE_DATA layout");

// ---------------------------------------------------------------------------
// container -> RGBA8 (DDS + PNG via bridge_img; BMP/TGA inline like d3dx9)
// ---------------------------------------------------------------------------
struct img_px {
  u32 w, h;
  u8* rgba;  // malloc'd
};

int parse_any(const void* data, u32 bytes, img_px* out) {
  const u8* d = (const u8*)data;
  // PNG / DDS first
  trt_img img{};
  if (bytes >= 8 && (memcmp(d, "\x89PNG", 4) == 0 || memcmp(d, "DDS ", 4) == 0)) {
    const int rc = trt_img_parse(d, bytes, &img);
    if (rc == 0) {
      out->w = img.w;
      out->h = img.h;
      out->rgba = img.rgba;
      return 0;
    }
    return rc;
  }
  // BMP 32/24-bit
  if (bytes >= 54 && d[0] == 'B' && d[1] == 'M') {
    const u32 off = *(const u32*)(d + 10);
    const u32 w = *(const u32*)(d + 18);
    const i32 h32 = *(const i32*)(d + 22);
    const u16 bpp = *(const u16*)(d + 28);
    if (w && h32 != 0 && (bpp == 24 || bpp == 32) && off < bytes) {
      const u32 h = (u32)(h32 < 0 ? -h32 : h32);
      u8* rgba = (u8*)malloc((size_t)w * h * 4);
      for (u32 y = 0; y < h; ++y) {
        // BMP rows are bottom-up when h32 > 0
        const u32 src_y = h32 > 0 ? (h - 1 - y) : y;
        const u8* src = d + off + (size_t)src_y * w * (bpp / 8);
        u8* dst = rgba + (size_t)y * w * 4;
        for (u32 x = 0; x < w; ++x) {
          dst[x*4+0] = src[x*(bpp/8)+2];
          dst[x*4+1] = src[x*(bpp/8)+1];
          dst[x*4+2] = src[x*(bpp/8)+0];
          dst[x*4+3] = bpp == 32 ? src[x*4+3] : (u8)0xFF;
        }
      }
      char line[80];
      snprintf(line, sizeof(line), "d3dx: BMP %ux%u parsed", w, h);
      log_line(line);
      out->w = w; out->h = h; out->rgba = rgba;
      return 0;
    }
  }
  // TGA type 2
  if (bytes >= 18 && d[1] == 2 && (d[16] == 24 || d[16] == 32)) {
    const u32 w = (u32)d[12] | ((u32)d[13] << 8);
    const u32 h = (u32)d[14] | ((u32)d[15] << 8);
    const u32 bpp = d[16] >= 32 ? 4 : 3;
    const u8* px = d + 18 + d[0];
    if (w && h && (size_t)(px - d) + (size_t)w * h * bpp <= bytes) {
      u8* rgba = (u8*)malloc((size_t)w * h * 4);
      for (u32 y = 0; y < h; ++y) {
        const u8* src = px + (size_t)y * w * bpp;
        u8* dst = rgba + (size_t)y * w * 4;
        for (u32 x = 0; x < w; ++x) {
          dst[x*4+0] = src[x*bpp+2];
          dst[x*4+1] = src[x*bpp+1];
          dst[x*4+2] = src[x*bpp+0];
          dst[x*4+3] = bpp == 4 ? src[x*4+3] : (u8)0xFF;
        }
      }
      char line[80];
      snprintf(line, sizeof(line), "d3dx: TGA %ux%u parsed", w, h);
      log_line(line);
      out->w = w; out->h = h; out->rgba = rgba;
      return 0;
    }
  }
  log_line("d3dx: container not PNG/DDS/BMP/TGA (JPG fails named)");
  return k_e_fail;
}

// image-info pre-scan struct: D3DX11_IMAGE_INFO (x64): Width, Height,
// Depth, MipLevels, MiscFlags(4x u32), ArraySize, Format, ResourceType,
// CPUAccessFlags, Usage, BindFlags, MiscFlags2 = 13 u32 = 52 -> pad to 56.
// We emit the first 8 fields games read: width/height/depth/mip/format/
// resource type. Games read image_info->Width/Height/Format mostly.
struct img_info {
  u32 width;
  u32 height;
  u32 depth;
  u32 mip_levels;
  u32 misc_flags;
  u32 array_size;
  u32 format;         // DXGI_FORMAT
  u32 resource_type;  // D3D11_RESOURCE_DIMENSION_TEXTURE2D = 4
  u32 cpu_access_flags;
  u32 usage;
  u32 bind_flags;
  u32 misc_flags2;
};

// DXGI_FORMAT constants we emit
constexpr u32 k_dxgi_b8g8r8a8 = 87;
constexpr u32 k_dxgi_r8g8b8a8 = 28;

void fill_info(const img_px& p, img_info* info) {
  memset(info, 0, sizeof(*info));
  info->width = p.w;
  info->height = p.h;
  info->depth = 1;
  info->mip_levels = 1;
  info->array_size = 1;
  info->format = k_dxgi_r8g8b8a8;
  info->resource_type = 4;  // TEXTURE2D
  info->bind_flags = 8;     // SHADER_RESOURCE
}

// shared: create RGBA8 texture (+ optional SRV) through the guest's device
int create_from_memory(void* device, const void* data, u32 bytes,
                       void** out_tex, void** out_srv, int log_what) {
  if (!device || !data || !bytes || (!out_tex && !out_srv))
    return k_e_invalidarg;
  if (out_tex) *out_tex = nullptr;
  if (out_srv) *out_srv = nullptr;
  img_px p{};
  const int rc = parse_any(data, bytes, &p);
  if (rc != 0) return rc;  // named failure from the parser
  if (!out_tex) { free(p.rgba); return 0; }  // info-only callers

  tex2d_desc desc{};
  desc.width = p.w;
  desc.height = p.h;
  desc.mip_levels = 1;
  desc.array_size = 1;
  desc.format = k_dxgi_r8g8b8a8;
  desc.sample_count = 1;
  desc.usage = 1;  // IMMUTABLE
  desc.bind_flags = 8;
  subres_data sd{};
  sd.p_sys_mem = p.rgba;
  sd.sys_mem_pitch = p.w * 4;
  void* tex = nullptr;
  const int hr = dev_create_texture2d(device, &desc, &sd, &tex);
  free(p.rgba);
  char line[128];
  snprintf(line, sizeof(line),
           "d3dx: %s CreateTexture2D %ux%u hr=0x%x tex=%p", log_what ? "mem" : "file",
           p.w, p.h, hr, tex);
  log_line(line);
  if (hr != 0) return hr;
  if (out_tex) *out_tex = tex;
  if (out_srv && tex) {
    // D3D11_SRV_DIMENSION_TEXTURE2D = 4, first element 0, mip count -1.
    // D3D11_TEX2D_SRV = { MostDetailedMip u32, MipLevels u32 } = 8 bytes;
    // D3D11_SHADER_RESOURCE_VIEW_DESC = { Format u32, ViewDimension u32,
    // union 8 bytes } = 16 bytes. All zeros + dimension 4 works for a
    // full mip chain view of a single-mip texture.
    u8 srv_desc[16];
    memset(srv_desc, 0, sizeof(srv_desc));
    ((u32*)srv_desc)[1] = 4;  // ViewDimension = TEXTURE2D
    const int hr2 = dev_create_srv(device, tex, srv_desc, out_srv);
    if (hr2 != 0) {
      char l2[96];
      snprintf(l2, sizeof(l2), "d3dx: CreateSRV hr=0x%x (texture kept)", hr2);
      log_line(l2);
      // the texture is still valid; only the view failed
    }
  }
  return 0;
}

// file reading through the VFS (guest paths like C:\game\ui.png resolve)
u8* read_guest_file_utf8(const char* path, u32* size) {
  FILE* os = fopen(path, "rb");
  if (!os) {
    char line[1152];
    snprintf(line, sizeof(line), "d3dx: file open failed: %s", path);
    log_line(line);
    return nullptr;
  }
  fseek(os, 0, SEEK_END);
  const long n = ftell(os);
  fseek(os, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)(n > 0 ? n : 1));
  const size_t got = fread(buf, 1, (size_t)n, os);
  fclose(os);
  *size = (u32)got;
  return buf;
}

u8* read_guest_file_utf16(const u16* path, u32* size) {
  char utf8[1024];
  size_t i = 0;
  while (i + 1 < sizeof(utf8) && path[i]) {
    utf8[i] = (char)(path[i] & 0xFF);  // narrow: game paths are ASCII/ANSI
    ++i;
  }
  utf8[i] = 0;
  return read_guest_file_utf8(utf8, size);
}

}  // namespace

// ---------------------------------------------------------------------------
// exports — d3dx11 first (the current generation), d3dx10 aliases it
// ---------------------------------------------------------------------------

// D3DX11GetImageInfoFromMemory(src, size, info)
int TRT_GUEST_ABI dx11_GetImageInfoFromMemory(const void* data, u32 bytes,
                                              void* info) {
  if (!data || !bytes || !info) return k_e_invalidarg;
  img_px p{};
  const int rc = parse_any(data, bytes, &p);
  if (rc != 0) return rc;
  free(p.rgba);
  fill_info(p, (img_info*)info);
  return 0;
}

int TRT_GUEST_ABI dx11_GetImageInfoFromFileW(const u16* path, void* info) {
  u32 bytes = 0;
  u8* data = path ? read_guest_file_utf16(path, &bytes) : nullptr;
  if (!data) return k_e_fail;
  const int rc = dx11_GetImageInfoFromMemory(data, bytes, info);
  free(data);
  return rc;
}

int TRT_GUEST_ABI dx11_GetImageInfoFromFileA(const char* path, void* info) {
  u32 bytes = 0;
  u8* data = path ? read_guest_file_utf8(path, &bytes) : nullptr;
  if (!data) return k_e_fail;
  const int rc = dx11_GetImageInfoFromMemory(data, bytes, info);
  free(data);
  return rc;
}

// D3DX11CreateTextureFromMemory(device, src, size, load_info, tex, hr)
int TRT_GUEST_ABI dx11_CreateTextureFromMemory(void* device, const void* data,
                                               u32 bytes, void* load_info,
                                               void** out_tex, void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  return create_from_memory(device, data, bytes, out_tex, nullptr, 1);
}

// D3DX11CreateTextureFromFileW(device, path, load_info, tex, hr)
int TRT_GUEST_ABI dx11_CreateTextureFromFileW(void* device, const u16* path,
                                              void* load_info, void** out_tex,
                                              void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  if (!path) return k_e_invalidarg;
  u32 bytes = 0;
  u8* data = read_guest_file_utf16(path, &bytes);
  if (!data) return 0x80070002;  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
  const int rc = create_from_memory(device, data, bytes, out_tex, nullptr, 0);
  free(data);
  return rc;
}

int TRT_GUEST_ABI dx11_CreateTextureFromFileA(void* device, const char* path,
                                              void* load_info, void** out_tex,
                                              void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  if (!path) return k_e_invalidarg;
  u32 bytes = 0;
  u8* data = read_guest_file_utf8(path, &bytes);
  if (!data) return 0x80070002;
  const int rc = create_from_memory(device, data, bytes, out_tex, nullptr, 0);
  free(data);
  return rc;
}

// D3DX11CreateShaderResourceViewFromMemory(device, src, size, load_info,
//                                          tex_out, srv_out, hr)
int TRT_GUEST_ABI dx11_CreateSRVFromMemory(void* device, const void* data,
                                           u32 bytes, void* load_info,
                                           void** out_tex, void** out_srv,
                                           void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  return create_from_memory(device, data, bytes, out_tex, out_srv, 1);
}

int TRT_GUEST_ABI dx11_CreateSRVFromFileW(void* device, const u16* path,
                                          void* load_info, void** out_tex,
                                          void** out_srv, void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  if (!path) return k_e_invalidarg;
  u32 bytes = 0;
  u8* data = read_guest_file_utf16(path, &bytes);
  if (!data) return 0x80070002;
  const int rc = create_from_memory(device, data, bytes, out_tex, out_srv, 0);
  free(data);
  return rc;
}

int TRT_GUEST_ABI dx11_CreateSRVFromFileA(void* device, const char* path,
                                          void* load_info, void** out_tex,
                                          void** out_srv, void* hr_out) {
  (void)load_info;
  if (hr_out) *(int*)hr_out = 0;
  if (!path) return k_e_invalidarg;
  u32 bytes = 0;
  u8* data = read_guest_file_utf8(path, &bytes);
  if (!data) return 0x80070002;
  const int rc = create_from_memory(device, data, bytes, out_tex, out_srv, 0);
  free(data);
  return rc;
}

// D3DX10 same names/shapes one generation earlier — alias everything
int TRT_GUEST_ABI dx10_GetImageInfoFromMemory(const void* data, u32 bytes,
                                              void* info) {
  return dx11_GetImageInfoFromMemory(data, bytes, info);
}
int TRT_GUEST_ABI dx10_CreateTextureFromMemory(void* device, const void* data,
                                               u32 bytes, void* load_info,
                                               void** out_tex, void* hr_out) {
  return dx11_CreateTextureFromMemory(device, data, bytes, load_info, out_tex,
                                      hr_out);
}
int TRT_GUEST_ABI dx10_CreateTextureFromFileW(void* device, const u16* path,
                                              void* load_info, void** out_tex,
                                              void* hr_out) {
  return dx11_CreateTextureFromFileW(device, path, load_info, out_tex, hr_out);
}
int TRT_GUEST_ABI dx10_CreateSRVFromMemory(void* device, const void* data,
                                           u32 bytes, void* load_info,
                                           void** out_tex, void** out_srv,
                                           void* hr_out) {
  return dx11_CreateSRVFromMemory(device, data, bytes, load_info, out_tex,
                                  out_srv, hr_out);
}

// D3DX11CompileFromFile: read the file, hand the source to a real
// D3DCompile. The host sets the compiler through trt_d3dx_set_compiler
// (it links d3dcompiler.lib and compiles its own shaders already); a
// guest-imported D3DCompile goes straight to the console's SYSTEM
// d3dcompiler_47 and never reaches here.
using d3dcompile_t = int (TRT_GUEST_ABI *)(const void*, size_t, const void*,
                                           const void*, const void*, const char*,
                                           const char*, u32, u32, void**,
                                           void**);
d3dcompile_t g_real_d3dcompile = nullptr;

void trt_d3dx_set_compiler(void* fn) { g_real_d3dcompile = (d3dcompile_t)fn; }

int TRT_GUEST_ABI dx11_CompileFromFileW(const u16* path, const void* defines,
                                        const void* includes,
                                        const char* entry, const char* target,
                                        u32 flags, u32 flags2, void** out_code,
                                        void** out_errors) {
  if (!path || !out_code) return k_e_invalidarg;
  *out_code = nullptr;
  if (out_errors) *out_errors = nullptr;
  u32 bytes = 0;
  u8* src = read_guest_file_utf16(path, &bytes);
  if (!src) return 0x80070002;
  if (!g_real_d3dcompile) {
    // the loader bound d3dcompiler_47 as a delay-import stub; the real
    // module is loaded via LoadLibrary in the boot path. Use the module
    // table's resolver: GetProcAddress-equivalent through pe_exports.
    log_line(
        "d3dx: CompileFromFile - no real d3dcompiler bound (shader compiles "
        "need the SYSTEM d3dcompiler_47 loaded by the host first)");
    free(src);
    return k_e_notimpl;
  }
  const int hr = g_real_d3dcompile(src, bytes, defines, includes, nullptr,
                                   entry, target, flags, flags2, out_code,
                                   out_errors);
  free(src);
  char line[128];
  snprintf(line, sizeof(line), "d3dx: CompileFromFile(%s) hr=0x%x",
           target ? target : "?", hr);
  log_line(line);
  return hr;
}

// D3DX11SaveTextureToFileW: texture dumps to disk are not implemented;
// answer E_NOTIMPL instead of killing the load (Sekiro audit).
int TRT_GUEST_ABI dx11_SaveTextureToFileW(void*, void*, u32, const u16*) {
  log_line("d3dx: SaveTextureToFileW -> E_NOTIMPL (no texture dump)");
  return k_e_notimpl;
}

const trt_named_fn k_d3dx_fns[] = {
    {"D3DX11GetImageInfoFromMemory", (void*)&dx11_GetImageInfoFromMemory},
    {"D3DX11GetImageInfoFromFileW", (void*)&dx11_GetImageInfoFromFileW},
    {"D3DX11GetImageInfoFromFileA", (void*)&dx11_GetImageInfoFromFileA},
    {"D3DX11CreateTextureFromMemory", (void*)&dx11_CreateTextureFromMemory},
    {"D3DX11CreateTextureFromFileW", (void*)&dx11_CreateTextureFromFileW},
    {"D3DX11CreateTextureFromFileA", (void*)&dx11_CreateTextureFromFileA},
    {"D3DX11CreateShaderResourceViewFromMemory",
     (void*)&dx11_CreateSRVFromMemory},
    {"D3DX11CreateShaderResourceViewFromFileW", (void*)&dx11_CreateSRVFromFileW},
    {"D3DX11CreateShaderResourceViewFromFileA", (void*)&dx11_CreateSRVFromFileA},
    {"D3DX11CompileFromFile", (void*)&dx11_CompileFromFileW},
    {"D3DX11SaveTextureToFileW", (void*)&dx11_SaveTextureToFileW},
    // d3dx10 aliases
    {"D3DX10GetImageInfoFromMemory", (void*)&dx10_GetImageInfoFromMemory},
    {"D3DX10CreateTextureFromMemory", (void*)&dx10_CreateTextureFromMemory},
    {"D3DX10CreateTextureFromFileW", (void*)&dx10_CreateTextureFromFileW},
    {"D3DX10CreateShaderResourceViewFromMemory",
     (void*)&dx10_CreateSRVFromMemory},
};

int trt_bridge_register_d3dx(void) {
  // d3dx11: the D3D11 generation (43 = the redist era, 47 = the last)
  trt_module_register("d3dx11_43.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx11_47.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  // d3dx10: the D3D10 generation (33..39 redist-era version spread + 43)
  trt_module_register("d3dx10_43.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_39.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_38.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_37.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_36.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_35.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_34.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  trt_module_register("d3dx10_33.dll", TRT_ROUTE_BUILTIN, k_d3dx_fns,
                      (int)(sizeof(k_d3dx_fns) / sizeof(k_d3dx_fns[0])));
  return 0;
}

}  // extern "C"
