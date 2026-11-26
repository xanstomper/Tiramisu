// tiramisu/tests/test_d3d.cpp
// The graphics bridge, proven offline.
//
// The host's D3D11 device/context/swapchain are fakes here (there is no GPU
// on the Linux test box), but they are REAL COM-shaped objects: the guest
// calls them through the standard vtable slots, so the test proves the
// wiring the console needs:
//
//   D3D11CreateDevice  -> the HOST's device and context
//   CreateDXGIFactory1 -> a factory that enumerates adapters and hands out
//                         the HOST's swapchain instead of failing on the
//                         guest's fake HWND
//   ... then the guest renders and presents into that swapchain
//
// The vtable slots below are the SDK's (extracted from d3d11.h/dxgi1_2.h):
// IUnknown is 3 slots, then the interface's own methods in declaration order.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
void* TRT_STDCALL trt_CreateWindowExW(u32, const wchar_t*, const wchar_t*, u32,
                                      int, int, int, int, void*, void*, void*, void*);
int TRT_STDCALL trt_SetWindowPos(void*, void*, i32, i32, i32, i32, u32);
int TRT_GUEST_ABI trt_D3D11CreateDevice(void* adapter, u32 driver_type, void* software,
                                        u32 flags, const void* feature_levels,
                                        u32 feature_level_count, u32 sdk_version,
                                        void** out_device, u32* out_level, void** out_context);
int TRT_GUEST_ABI trt_D3D11CreateDeviceAndSwapChain(void* adapter, u32 driver_type,
                                        void* software, u32 flags,
                                        const void* feature_levels,
                                        u32 feature_level_count, u32 sdk_version,
                                        void* swapchain_desc, void** out_swapchain,
                                        void** out_device, u32* out_level,
                                        void** out_context);
int TRT_GUEST_ABI trt_CreateDXGIFactory2(u32 flags, void* riid, void** out_factory);
int TRT_GUEST_ABI trt_DXGIGetDebugInterface1(u32 flags, void* riid, void** out_debug);
int TRT_GUEST_ABI trt_D3D11On12CreateDevice(
    void* device, u32 flags, const void* levels, u32 level_count,
    void* const* queues, u32 num_queues, u32 node_mask,
    void** out_device, void** out_ctx, u32* out_level);
}

// ---------------- mmap backend ----------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

// named log-line capture (the feature-level contract is diagnosed by log)
char g_log_capture[64][256];
int g_log_capture_used = 0;
int log_contains(const char* needle) {
  for (int i = 0; i < g_log_capture_used; ++i)
    if (strstr(g_log_capture[i], needle)) return 1;
  return 0;
}

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = nullptr;
  if (preferred_base) {
    // ASan's shadow gap swallows the guest's usual preferred bases
    // (0x140000000 is INSIDE it — measured "unknown-crash" false positives):
    // under sanitizers, let the kernel pick and let the loader relocate.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    (defined(__clang__) && __has_feature(address_sanitizer)))
    const uintptr_t gap_lo = 0x00008fff7000ull, gap_hi = 0x02008fff6fffull;
    if (preferred_base >= gap_lo && preferred_base <= gap_hi) preferred_base = 0;
#endif
    if (preferred_base)
      p = mmap((void*)(uintptr_t)preferred_base, bytes, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!p || p == MAP_FAILED)
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}
void linux_log(const char* line) {
  // capture for contract assertions (feature-level negotiation logs)
  if (g_log_capture_used < (int)(sizeof(g_log_capture) / sizeof(g_log_capture[0]))) {
    snprintf(g_log_capture[g_log_capture_used++], sizeof(g_log_capture[0]),
             "%s", line);
  }
  printf("  [trt] %s\n", line);
}

// ---------------- the fake host GPU ----------------
// Every fake object is { vtable pointer } - the COM layout the guest expects.
struct com_object { const void** vtbl; };

// what the guest actually reached
int g_created_rtv = 0;
int g_cleared = 0;
int g_drew = 0;
int g_presented = 0;
int g_enumerated_adapter = 0;

// fake object identities (the guest must land on THESE)
com_object g_device_obj = {nullptr};
com_object g_context_obj = {nullptr};
com_object g_swapchain_obj = {nullptr};
com_object g_real_factory_obj = {nullptr};
com_object g_adapter_obj = {nullptr};
com_object g_texture_obj = {nullptr};
com_object g_rtv_obj = {nullptr};

extern "C" void* g_fake_context;
extern "C" void* g_fake_device;
extern "C" void* g_fake_swapchain;
extern "C" void* g_fake_texture;
extern "C" void* g_fake_rtv;
extern "C" void* g_fake_adapter;

// generic stub: every SDK slot returns S_OK; the interesting ones are below
extern "C" TRT_GUEST_ABI int fake_generic_hr(void*, void*, void*, void*) { return 0; }

static u32 g_requested_width, g_requested_height;
static u32 g_requested_swapchain_format;
static u32 g_requested_swapchain_buffers;
static u32 g_requested_swap_effect;
static unsigned g_prepare_presents;
static int TRT_GUEST_ABI get_alternate_backbuffer(u32 index, const void*, void** out) {
  if (!out || index != 0) return (int)0x887a0001u;
  *out = g_fake_rtv;
  return 0;
}
static void TRT_GUEST_ABI prepare_present() {
  ++g_prepare_presents;
}
static int g_swapchain_format_result;
static int TRT_GUEST_ABI set_swapchain_format(u32 width, u32 height, u32 format,
                                              u32 buffer_count, u32 swap_effect) {
  g_requested_width = width;
  g_requested_height = height;
  g_requested_swapchain_format = format;
  g_requested_swapchain_buffers = buffer_count;
  g_requested_swap_effect = swap_effect;
  return g_swapchain_format_result;
}

static unsigned g_added_refs;
extern "C" TRT_GUEST_ABI u32 fake_AddRef(void*) {
  return ++g_added_refs;
}

// --- device ---
extern "C" TRT_GUEST_ABI int fake_dev_CreateRenderTargetView(void* self, void* back,
                                               void* desc, void** out) {
  (void)self; (void)back; (void)desc;
  ++g_created_rtv;
  if (out) *out = g_fake_rtv;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_dev_GetImmediateContext(void* self, void** out) {
  (void)self;
  if (out) *out = g_fake_context;
  return 0;
}

// ID3D11Device: 3 (IUnknown) + 40 own methods = 43 slots
const void* g_dev_vtbl[43] = {
    [0] = (const void*)&fake_generic_hr,   // QueryInterface
    [1] = (const void*)&fake_AddRef,   // AddRef
    [2] = (const void*)&fake_generic_hr,   // Release
    [3] = (const void*)&fake_generic_hr,   // CreateBuffer
    [4] = (const void*)&fake_generic_hr,   // CreateTexture1D
    [5] = (const void*)&fake_generic_hr,   // CreateTexture2D
    [6] = (const void*)&fake_generic_hr,   // CreateTexture3D
    [7] = (const void*)&fake_generic_hr,   // CreateShaderResourceView
    [8] = (const void*)&fake_generic_hr,   // CreateUnorderedAccessView
    [9] = (const void*)&fake_dev_CreateRenderTargetView,
    [10] = (const void*)&fake_generic_hr,  // CreateDepthStencilView
    [11] = (const void*)&fake_generic_hr,  // CreateInputLayout
    [12] = (const void*)&fake_generic_hr,  // CreateVertexShader
    [13] = (const void*)&fake_generic_hr,  // CreateGeometryShader
    [14] = (const void*)&fake_generic_hr,  // CreateGeometryShaderWithStreamOutput
    [15] = (const void*)&fake_generic_hr,  // CreatePixelShader
    [16] = (const void*)&fake_generic_hr,  // CreateHullShader
    [17] = (const void*)&fake_generic_hr,  // CreateDomainShader
    [18] = (const void*)&fake_generic_hr,  // CreateComputeShader
    [19] = (const void*)&fake_generic_hr,  // CreateClassLinkage
    [20] = (const void*)&fake_generic_hr,  // CreateBlendState
    [21] = (const void*)&fake_generic_hr,  // CreateDepthStencilState
    [22] = (const void*)&fake_generic_hr,  // CreateRasterizerState
    [23] = (const void*)&fake_generic_hr,  // CreateSamplerState
    [24] = (const void*)&fake_generic_hr,  // CreateQuery
    [25] = (const void*)&fake_generic_hr,  // CreatePredicate
    [26] = (const void*)&fake_generic_hr,  // CreateCounter
    [27] = (const void*)&fake_generic_hr,  // CreateDeferredContext
    [28] = (const void*)&fake_generic_hr,  // OpenSharedResource
    [29] = (const void*)&fake_generic_hr,  // CheckFormatSupport
    [30] = (const void*)&fake_generic_hr,  // CheckMultisampleQualityLevels
    [31] = (const void*)&fake_generic_hr,  // CheckCounterInfo
    [32] = (const void*)&fake_generic_hr,  // CheckCounter
    [33] = (const void*)&fake_generic_hr,  // CheckFeatureSupport
    [34] = (const void*)&fake_generic_hr,  // GetPrivateData
    [35] = (const void*)&fake_generic_hr,  // SetPrivateData
    [36] = (const void*)&fake_generic_hr,  // SetPrivateDataInterface
    [37] = (const void*)&fake_generic_hr,  // GetFeatureLevel
    [38] = (const void*)&fake_generic_hr,  // GetCreationFlags
    [39] = (const void*)&fake_generic_hr,  // GetDeviceRemovedReason
    [40] = (const void*)&fake_dev_GetImmediateContext,
    [41] = (const void*)&fake_generic_hr,  // SetExceptionMode
    [42] = (const void*)&fake_generic_hr,  // GetExceptionMode
};

// --- device context ---
extern "C" TRT_GUEST_ABI void fake_ctx_ClearRenderTargetView(void*, void*, const float*) {
  ++g_cleared;
}
extern "C" TRT_GUEST_ABI void fake_ctx_Draw(void*, u32, u32) { ++g_drew; }
extern "C" TRT_GUEST_ABI void fake_ctx_OMSetRenderTargets(void*, u32, void**, void**) {}
extern "C" TRT_GUEST_ABI void fake_ctx_VSSetShader(void*, void*, void**, u32) {}
extern "C" TRT_GUEST_ABI void fake_ctx_PSSetShader(void*, void*, void**, u32) {}
extern "C" TRT_GUEST_ABI void fake_ctx_IASetPrimitiveTopology(void*, u32) {}

// ID3D11DeviceContext: 3 + 112 = 115 slots
const void* g_ctx_vtbl[115] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_AddRef,
    [2] = (const void*)&fake_generic_hr,
    [9] = (const void*)&fake_ctx_PSSetShader,           // PSSetShader
    [11] = (const void*)&fake_ctx_VSSetShader,          // VSSetShader
    [13] = (const void*)&fake_ctx_Draw,                 // Draw
    [24] = (const void*)&fake_ctx_IASetPrimitiveTopology,
    [33] = (const void*)&fake_ctx_OMSetRenderTargets,   // OMSetRenderTargets
    [50] = (const void*)&fake_ctx_ClearRenderTargetView,
};

// --- swapchain ---
extern "C" TRT_GUEST_ABI int fake_sc_GetBuffer(void* self, u32 index, const void* iid,
                                  void** out) {
  (void)self; (void)index; (void)iid;
  if (out) *out = g_fake_texture;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_sc_Present(void* self, u32 sync, u32 flags) {
  (void)self; (void)sync; (void)flags;
  ++g_presented;
  return 0;
}

// IDXGISwapChain: 3 + 15 = 18 slots
const void* g_sc_vtbl[18] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_AddRef,
    [2] = (const void*)&fake_generic_hr,
    [8] = (const void*)&fake_sc_Present,   // Present
    [9] = (const void*)&fake_sc_GetBuffer, // GetBuffer
};

// --- the console's real factory (the one the shim forwards to) ---
extern "C" TRT_GUEST_ABI int fake_real_EnumAdapters1(void* self, u32 index, void** adapter) {
  (void)self; (void)index;
  ++g_enumerated_adapter;
  if (adapter) *adapter = g_fake_adapter;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_real_EnumAdapters(void* self, u32 index, void** adapter) {
  (void)self; (void)index;
  ++g_enumerated_adapter;
  if (adapter) *adapter = g_fake_adapter;
  return 0;
}
// IDXGIFactory2: 3 + 22 = 25 slots
const void* g_real_factory_vtbl[25] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_generic_hr,
    [2] = (const void*)&fake_generic_hr,
    [7] = (const void*)&fake_real_EnumAdapters,
    [12] = (const void*)&fake_real_EnumAdapters1,
};

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

} // namespace

extern "C" void* g_fake_context = (void*)&g_context_obj;
extern "C" void* g_fake_device = (void*)&g_device_obj;
extern "C" void* g_fake_swapchain = (void*)&g_swapchain_obj;
extern "C" void* g_fake_texture = (void*)&g_texture_obj;
extern "C" void* g_fake_rtv = (void*)&g_rtv_obj;
extern "C" void* g_fake_adapter = (void*)&g_adapter_obj;

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_d3d <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  // publish the "host GPU" to the bridge
  trt_gpu_hooks hooks{};
  hooks.device = g_fake_device;
  hooks.context = g_fake_context;
  hooks.swapchain = g_fake_swapchain;
  hooks.factory = &g_real_factory_obj;
  hooks.feature_level = 0xb000;  // D3D_FEATURE_LEVEL_11_0 (Series-class host)
  hooks.width = 1280;
  hooks.height = 720;
  trt_gpu_set_hooks(&hooks);

  // wire the fake COM objects to their vtables
  g_device_obj.vtbl = g_dev_vtbl;
  g_context_obj.vtbl = g_ctx_vtbl;
  g_swapchain_obj.vtbl = g_sc_vtbl;
  g_real_factory_obj.vtbl = g_real_factory_vtbl;

  printf("tiramisu D3D11/DXGI bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (d3d11/dxgi imports bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);
  CHECK(ret == 0xFFull, "guest completed the whole D3D11 frame");

  CHECK(g_enumerated_adapter == 1,
        "adapter enumeration reached the console's real factory");
  CHECK(g_created_rtv == 1, "guest created a render target on the host device");
  CHECK(g_cleared == 1, "guest cleared the host's render target");
  CHECK(g_drew == 1, "guest issued a draw on the host's context");
  CHECK(g_presented == 1,
        "guest presented into the HOST's swapchain (not its own)");

  // ---- Stage 3: D3D11 feature-level contract (direct bridge calls) ----
  printf("\nfeature-level contract (Windows semantics):\n");
  g_log_capture_used = 0;
  void* dev = nullptr;
  void* ctx = nullptr;
  u32 got_level = 0;

  // {12_1, 11_1, 11_0} on an 11_0 host -> falls through to 11_0, S_OK
  const u32 req_modern[] = {0xc100, 0xb100, 0xb000};
  g_added_refs = 0;
  int hr = trt_D3D11CreateDevice(nullptr, 1, nullptr, 0, req_modern, 3, 7,
                                 &dev, &got_level, &ctx);
  CHECK(hr == 0 && got_level == 0xb000,
        "request {12_1, 11_1, 11_0} on FL 11_0 host -> S_OK, FL 0xb000");
  CHECK(g_added_refs == 2, "D3D11CreateDevice retains the returned device and context");
  CHECK(log_contains("d3d: guest requests FL 0xC100, host max 0xB000 - skipped") &&
            log_contains("d3d: guest requests FL 0xB100, host max 0xB000 - skipped"),
        "skipped levels each get the named skip log");

  // {11_1} only on an 11_0 host -> E_INVALIDARG + the named unmet log
  g_log_capture_used = 0;
  const u32 req_111[] = {0xb100};
  got_level = 0xdeadbeef;
  hr = trt_D3D11CreateDevice(nullptr, 1, nullptr, 0, req_111, 1, 7, &dev,
                             &got_level, &ctx);
  CHECK((u32)hr == 0x80070057u,
        "request {11_1} only on FL 11_0 host -> E_INVALIDARG");
  CHECK(got_level == 0xdeadbeef, "out_level untouched on contract failure");
  CHECK(log_contains("d3d: guest minimum FL 0xB100 unmet (host max 0xB000) -> "
                     "E_INVALIDARG"),
        "unmet minimum logs the modern-engine diagnosis line");

  // NULL request -> host real level, S_OK (existing behavior kept)
  got_level = 0;
  hr = trt_D3D11CreateDevice(nullptr, 1, nullptr, 0, nullptr, 0, 7, &dev,
                             &got_level, &ctx);
  CHECK(hr == 0 && got_level == 0xb000,
        "NULL request -> S_OK with the host's real level");

  // {10_1, 10_0} -> first supported is 10_1
  const u32 req_legacy[] = {0xa100, 0xa000};
  got_level = 0;
  hr = trt_D3D11CreateDevice(nullptr, 1, nullptr, 0, req_legacy, 2, 7, &dev,
                             &got_level, &ctx);
  CHECK(hr == 0 && got_level == 0xa100,
        "request {10_1, 10_0} -> S_OK with FL 0xa100 (first supported)");
  CHECK(log_contains("d3d: FL contract -> 0xA100 (requested 2 levels)"),
        "final pick logged as the contract line");

  // Readback accounting ignores texture pitch padding and alpha-only black.
  const u8 pixels[24] = {
      0, 0, 0, 255, 120, 0, 0, 0, 255, 255, 255, 255,
      0, 0, 200, 255, 12, 12, 12, 255, 255, 255, 255, 255
  };
  CHECK(trt_gpu_count_lit_rgb32(pixels, 2, 2, 12) == 2,
        "surface probe counts RGB content and excludes alpha and row padding");
  CHECK(trt_gpu_count_lit_rgb32(pixels, 4, 2, 12) == 0,
        "surface probe refuses an invalid row pitch");

  // Combined creation must deliver the same desktop compatibility proxy
  // as the factory, including the virtual backbuffer and present hooks.
  void* sc = nullptr;
  const u32 req_asc[] = {0xb100, 0xb000};
  u32 desc[18] = {};
  desc[0] = 1024;
  desc[1] = 576;
  desc[4] = 29;  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
  desc[10] = 2;
  desc[15] = 0;   // DXGI_SWAP_EFFECT_DISCARD
  hooks.configure_d3d11_swapchain = &set_swapchain_format;
  hooks.get_d3d11_backbuffer = &get_alternate_backbuffer;
  hooks.prepare_d3d11_present = &prepare_present;
  trt_gpu_set_hooks(&hooks);
  trt_gpu_reset_render_stats();
  g_added_refs = 0;
  got_level = 0;
  hr = trt_D3D11CreateDeviceAndSwapChain(nullptr, 1, nullptr, 0, req_asc, 2, 7,
                                         desc, &sc, &dev, &got_level, &ctx);
  CHECK(hr == 0 && got_level == 0xb000 && sc && sc != g_fake_swapchain,
        "combined creation returns the compatibility proxy");
  CHECK(g_requested_width == 1024 && g_requested_height == 576 &&
            g_requested_swapchain_format == 29 && g_requested_swapchain_buffers == 2 &&
            g_requested_swap_effect == 0,
        "combined creation forwards guest dimensions, format, buffer count and swap effect");
  CHECK(trt_gpu_d3d11_uses_stable_backbuffer(2, 0) &&
            !trt_gpu_d3d11_uses_stable_backbuffer(2, 3),
        "desktop DISCARD gets a stable backbuffer while native flip keeps rotating");
  CHECK(g_added_refs == 2, "combined creation retains the device and context");
  if (sc && sc != g_fake_swapchain) {
    const void** vt = *(const void***)sc;
    using buffer_fn = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
    using present_fn = int (TRT_GUEST_ABI *)(void*, u32, u32);
    using get_desc_fn = int (TRT_GUEST_ABI *)(void*, void*);
    using resize_fn = int (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32);
    using index_fn = u32 (TRT_GUEST_ABI *)(void*);
    u32 reported[18]{};
    CHECK(((get_desc_fn)vt[12])(sc, reported) == 0 &&
              reported[0] == 1024 && reported[1] == 576 &&
              reported[4] == 29 && reported[10] == 2 && reported[15] == 0,
          "GetDesc describes the guest sRGB desktop chain");
    CHECK(((index_fn)vt[36])(sc) == 0, "stable backbuffer stays at index zero");
    CHECK(((resize_fn)vt[13])(sc, 0, 1280, 720, 0, 0) == 0 &&
              g_requested_width == 1280 && g_requested_height == 720 &&
              g_requested_swapchain_buffers == 2 && g_requested_swap_effect == 0,
          "ResizeBuffers retains the guest count and swap effect for zero count");
    void* buffer = nullptr;
    CHECK(((buffer_fn)vt[9])(sc, 0, nullptr, &buffer) == 0 && buffer == g_fake_rtv,
          "combined swapchain exposes the virtual guest backbuffer");
    CHECK(((present_fn)vt[8])(sc, 0, 1u) == 0 && g_prepare_presents == 0,
          "combined swapchain skips preparation for a test present");
    CHECK(((present_fn)vt[8])(sc, 0, 0) == 0 && g_prepare_presents == 1,
          "combined swapchain copies the virtual surface for a real present");
  }
  trt_gpu_render_stats stats{};
  trt_gpu_get_render_stats(&stats);
  CHECK(stats.d3d11_device_calls == 1 && stats.d3d11_swapchains == 1 &&
            stats.d3d11_get_buffer_calls == 1 && stats.d3d11_present_calls == 1 &&
            stats.d3d11_successful_presents == 1 &&
            stats.d3d11_stable_backbuffer == 1 && stats.d3d11_buffer_count == 2,
        "render counters exclude test presents and report the virtual surface");
  CHECK(stats.d3d11_width == 1280 && stats.d3d11_height == 720,
        "render counters report the last successful guest size");

  g_swapchain_format_result = (int)0x80070057u;
  g_added_refs = 0;
  hr = trt_D3D11CreateDeviceAndSwapChain(nullptr, 1, nullptr, 0, req_asc, 2, 7,
                                         desc, &sc, &dev, nullptr, &ctx);
  CHECK(hr == g_swapchain_format_result && !sc && !dev && !ctx && g_added_refs == 0,
        "failed combined creation clears outputs and does not retain COM objects");
  g_swapchain_format_result = 0;
  g_prepare_presents = 0;
  g_presented = 1;  // Restore the count from the guest frame above.
  hooks.configure_d3d11_swapchain = nullptr;
  hooks.get_d3d11_backbuffer = nullptr;
  hooks.prepare_d3d11_present = nullptr;
  trt_gpu_set_hooks(&hooks);

  // AndSwapChain: {9_1} -> 9_1
  const u32 req_91[] = {0x9100};
  got_level = 0;
  hr = trt_D3D11CreateDeviceAndSwapChain(nullptr, 1, nullptr, 0, req_91, 1, 7,
                                         nullptr, &sc, &dev, &got_level, &ctx);
  CHECK(hr == 0 && got_level == 0x9100,
        "AndSwapChain {9_1} -> S_OK, FL 0x9100");

  // ---- DXGI 3-arg CreateDXGIFactory2, 4-arg CreateSwapChain, UWP safety ----
  void* dxgi_fac = nullptr;
  hr = trt_CreateDXGIFactory2(0, nullptr, &dxgi_fac);
  CHECK(hr == 0 && dxgi_fac != nullptr,
        "CreateDXGIFactory2(0, riid, out) 3-arg returns wrapped factory");
  if (dxgi_fac) {
    const void** fvt = *(const void***)dxgi_fac;
    using enum_t = int (TRT_GUEST_ABI *)(void*, u32, void**);
    using csc_t = int (TRT_GUEST_ABI *)(void*, void*, void*, void**);
    using hwnd_sc_fn = int (TRT_GUEST_ABI *)(void*, void*, void*, void*, void*, void*, void**);
    void* adp = nullptr;
    const int ehr = ((enum_t)fvt[7])(dxgi_fac, 0, &adp);
    CHECK(ehr == 0 && adp == g_fake_adapter && g_enumerated_adapter == 2,
          "IDXGIFactory::EnumAdapters delegates via TRT_GUEST_ABI");

    void* proxy_sc = nullptr;
    const int schr = ((csc_t)fvt[10])(dxgi_fac, dev, nullptr, &proxy_sc);
    CHECK(schr == 0 && proxy_sc != nullptr,
          "IDXGIFactory::CreateSwapChain (4-arg) yields proxy swapchain");
    if (proxy_sc) {
      const void** svt = *(const void***)proxy_sc;
      using fs_t = int (TRT_GUEST_ABI *)(void*, int, void*);
      using rb_t = int (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32);
      using pres_t = int (TRT_GUEST_ABI *)(void*, u32, u32);
      CHECK(((fs_t)svt[10])(proxy_sc, 1, nullptr) == 0,
            "IDXGISwapChain::SetFullscreenState(TRUE) -> S_OK on CoreWindow");
      CHECK(((rb_t)svt[13])(proxy_sc, 2, 1920, 1080, 87, 0) == 0,
            "IDXGISwapChain::ResizeBuffers handles fixed CoreWindow cleanly");
      CHECK(((pres_t)svt[8])(proxy_sc, 0, 0x200u) == 0 && g_presented == 2,
            "IDXGISwapChain::Present strips ALLOW_TEARING and forwards");
      hooks.configure_d3d11_swapchain = &set_swapchain_format;
      trt_gpu_set_hooks(&hooks);
      const u32 rgba_srgb = 29;
      CHECK(((rb_t)svt[13])(proxy_sc, 1, 1920, 1080, rgba_srgb, 0) == 0 &&
                g_requested_swapchain_format == rgba_srgb &&
                g_requested_swapchain_buffers == 1,
            "D3D11 ResizeBuffers forwards the format and buffer count");
      hooks.get_d3d11_backbuffer = &get_alternate_backbuffer;
      hooks.prepare_d3d11_present = &prepare_present;
      trt_gpu_set_hooks(&hooks);
      using get_buffer_t = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
      void* guest_backbuffer = nullptr;
      CHECK(((get_buffer_t)svt[9])(proxy_sc, 0, nullptr, &guest_backbuffer) == 0 &&
                guest_backbuffer == g_fake_rtv,
            "D3D11 GetBuffer uses the host-provided guest texture");
      ((pres_t)svt[8])(proxy_sc, 0, 1u);
      CHECK(g_prepare_presents == 0, "Present test skips the host copy");
      ((pres_t)svt[8])(proxy_sc, 0, 0);
      CHECK(g_prepare_presents == 1, "Present copies from the guest texture");
      g_swapchain_format_result = (int)0x80070057u;
      CHECK(((rb_t)svt[13])(proxy_sc, 2, 1920, 1080, rgba_srgb, 0) ==
                g_swapchain_format_result,
            "D3D11 ResizeBuffers preserves a host format error");
      g_swapchain_format_result = 0;
      void* window = trt_CreateWindowExW(0, nullptr, nullptr, 0, 0, 0, 800, 600,
                                           nullptr, nullptr, nullptr, nullptr);
      u32 window_desc[12]{};
      window_desc[2] = 87;
      window_desc[7] = 1;
      void* window_chain = nullptr;
      const int window_hr = ((hwnd_sc_fn)fvt[15])(dxgi_fac, hooks.device, window,
          window_desc, nullptr, nullptr, &window_chain);
      CHECK(window_hr == 0 && window_chain && g_requested_width == 800 &&
                g_requested_height == 600,
            "zero-sized HWND swap chain uses the guest window client dimensions");
      if (window_chain) {
        const auto* window_vt = *static_cast<const void***>(window_chain);
        using get_hwnd_fn = int (TRT_GUEST_ABI *)(void*, void**);
        void* reported_window = nullptr;
        CHECK(((get_hwnd_fn)window_vt[20])(window_chain, &reported_window) == 0 &&
                  reported_window == window,
              "GetHwnd returns the HWND associated with the guest swap chain");
        trt_SetWindowPos(window, nullptr, 0, 0, 640, 360, 0);
        CHECK(((rb_t)window_vt[13])(window_chain, 0, 0, 0, 0, 0) == 0 &&
                  g_requested_width == 640 && g_requested_height == 360,
              "zero-sized ResizeBuffers follows the resized guest window");
        trt_SetWindowPos(window, nullptr, 0, 0, 0, 0, 0);
        CHECK(((rb_t)window_vt[13])(window_chain, 0, 0, 0, 0, 0) == 0 &&
                  g_requested_width == 640 && g_requested_height == 360,
              "an empty guest window retains the last usable swap-chain size");
      }
      hooks.configure_d3d11_swapchain = nullptr;
      hooks.get_d3d11_backbuffer = nullptr;
      hooks.prepare_d3d11_present = nullptr;
      trt_gpu_set_hooks(&hooks);
    }
  }

  void* dbg_if = (void*)(uintptr_t)0xdeadbeef;
  CHECK((u32)trt_DXGIGetDebugInterface1(0, nullptr, &dbg_if) == 0x80004002u &&
            dbg_if == nullptr,
        "DXGIGetDebugInterface1 -> E_NOINTERFACE fail-visible");
  void* on12_dev = (void*)(uintptr_t)0xdeadbeef;
  CHECK((u32)trt_D3D11On12CreateDevice(nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                       &on12_dev, nullptr, nullptr) == 0x80004001u &&
            on12_dev == nullptr,
        "D3D11On12CreateDevice -> E_NOTIMPL fail-visible");

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
