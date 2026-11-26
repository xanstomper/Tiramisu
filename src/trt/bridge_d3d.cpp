// tiramisu/src/trt/bridge_d3d.cpp
// The D3D11/DXGI bridge (see bridge_d3d.h).
//
// Only two things need shimming:
//   1. D3D11CreateDevice / D3D11CreateDeviceAndSwapChain: the guest must
//      get the HOST's device and context, otherwise its resources live in
//      a device that cannot touch the host's swapchain.
//   2. CreateDXGIFactory*: the guest's factory must hand out the HOST's
//      swapchain, because the guest's window is a fake HWND and the
//      console has no real window for a flip swapchain.
//
// The factory shim is a COM object: the guest calls it through the
// standard vtable, so the vtable layout has to match IDXGIFactory2
// exactly (IUnknown + IDXGIObject + IDXGIFactory + IDXGIFactory1 +
// IDXGIFactory2 = 25 slots).
#include "bridge_d3d.h"
#include "bridge_d3d12.h"
#include "input.h"
#include "thunk.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <mutex>

extern "C" int trt_user32_client_size(void* hwnd, i32* width, i32* height);
extern "C" int TRT_STDCALL trt_SetWindowPos(void* hwnd, void* after, i32 x, i32 y,
                                             i32 width, i32 height, u32 flags);

namespace {

trt_gpu_hooks g_hooks;

void* retain_com_object(void* object) {
  if (object) {
    const auto vtable = *static_cast<const void***>(object);
    using add_ref = u32 (TRT_GUEST_ABI *)(void*);
    if (vtable && vtable[1]) reinterpret_cast<add_ref>(const_cast<void*>(vtable[1]))(object);
  }
  return object;
}

constexpr int TRT_E_NOTIMPL = 0x80004001;  // E_NOTIMPL
constexpr int TRT_E_NOINTERFACE = 0x80004002;
constexpr int TRT_E_INVALIDARG = 0x80070057;  // E_INVALIDARG (TRT_-prefixed: never a Win32 macro name)

// Windows semantics: D3D11CreateDevice walks pFeatureLevels IN ORDER and
// returns the FIRST level the device actually supports. We mirror that with
// the host's real capability (g_hooks.feature_level). Constants are TRT_-
// prefixed (never D3D_FEATURE_LEVEL_*: winnt.h/wingdi.h macro collisions are
// invisible to the offline gate).
constexpr u32 TRT_FL_9_1 = 0x9100;
constexpr u32 TRT_FL_9_2 = 0x9200;
constexpr u32 TRT_FL_9_3 = 0x9300;
constexpr u32 TRT_FL_10_0 = 0xa000;
constexpr u32 TRT_FL_10_1 = 0xa100;
constexpr u32 TRT_FL_11_0 = 0xb000;
constexpr u32 TRT_FL_11_1 = 0xb100;
constexpr u32 TRT_FL_12_0 = 0xc000;
constexpr u32 TRT_FL_12_1 = 0xc100;

// Apply the feature-level contract against the host's real level.
// Returns S_OK and writes the negotiated level, or E_INVALIDARG when the
// guest's minimum cannot be met (a modern engine booting past the console's
// GPU fails visibly, never silently at a lower level).
static int trt_apply_fl_contract(const void* feature_levels, u32 count,
                                 u32* out_level) {
  const u32 host_max = g_hooks.feature_level;
  if (feature_levels && count) {
    const u32* levels = (const u32*)feature_levels;
    for (u32 i = 0; i < count; ++i) {
      const u32 requested = levels[i];
      // Only the SDK's defined levels are contract-legal; anything else is
      // an invalid request (Windows validates the array the same way).
      const bool known = requested == TRT_FL_9_1 || requested == TRT_FL_9_2 ||
                         requested == TRT_FL_9_3 || requested == TRT_FL_10_0 ||
                         requested == TRT_FL_10_1 || requested == TRT_FL_11_0 ||
                         requested == TRT_FL_11_1 || requested == TRT_FL_12_0 ||
                         requested == TRT_FL_12_1;
      if (!known) return TRT_E_INVALIDARG;
      if (requested <= host_max) {
        if (out_level) *out_level = requested;
        char line[192];
        snprintf(line, sizeof(line),
                 "d3d: FL contract -> 0x%04X (requested %u levels)", requested,
                 count);
        trt_core_log(line);
        return 0;  // S_OK
      }
      char line[192];
      snprintf(line, sizeof(line),
               "d3d: guest requests FL 0x%04X, host max 0x%04X - skipped",
               requested, host_max);
      trt_core_log(line);
    }
    // every requested level is above the host's capability
    if (out_level) *out_level = 0;
    char line[224];
    snprintf(line, sizeof(line),
             "d3d: guest minimum FL 0x%04X unmet (host max 0x%04X) -> "
             "E_INVALIDARG (modern engine needs newer GPU than console "
             "provides)",
             levels[0], host_max);
    trt_core_log(line);
    return TRT_E_INVALIDARG;
  }
  // pFeatureLevels == NULL: Windows semantics are "give me the best you have"
  if (out_level) *out_level = host_max;
  return 0;  // S_OK
}

// ---------------------------------------------------------------------------
// the fake swapchain: proxies IDXGISwapChain{,1} to the host CoreWindow
// swapchain while shielding guest games from desktop-only calls that fail on
// UWP CoreWindow (SetFullscreenState, ResizeTarget, ALLOW_TEARING, ResizeBuffers
// while host holds a reference, Release dropping refcount to 0).
// ---------------------------------------------------------------------------
bool g_sc_is_d3d12 = false;
u32 g_sc_backbuf_idx = 0;
u32 g_sc_buf_count = 2;
u32 g_sc_width = 1280;
u32 g_sc_height = 720;
u32 g_sc_colorspace = 0;
std::atomic<bool> g_sc_presented{false};

// These counters describe the current launch and carry no game data.
std::atomic<u32> g_d3d11_devices{0}, g_d3d11_swapchains{0}, g_d3d11_buffers{0};
std::atomic<u32> g_d3d11_presents{0}, g_d3d11_visible_presents{0};
std::atomic<int> g_d3d11_last_present{0};
std::atomic<u32> g_d3d11_swap_effect{3}, g_d3d11_buffer_count{2};
std::atomic<u32> g_d3d11_stable_backbuffer{0};
u32 g_d3d11_guest_format = 0;
std::atomic<u32> g_d3d11_width{0}, g_d3d11_height{0};
void* g_d3d11_hwnd = nullptr;
std::mutex g_resize_stats_mutex;
trt_gpu_resize_stats g_resize_stats{};

void* desktop_swapchain_window(const u32* desc) {
  void* window = nullptr;
  // DXGI_SWAP_CHAIN_DESC::OutputWindow at offset 48 in the Win64 ABI.
  if (desc) memcpy(&window, desc + 12, sizeof(window));
  return window;
}

int configure_guest_swapchain(u32 width, u32 height, u32 fmt, u32 count,
                              u32 effect, void* hwnd, bool creating) {
  const u32 requested_width = width, requested_height = height;
  i32 client_width = 0, client_height = 0;
  if (hwnd && (!width || !height))
    trt_user32_client_size(hwnd, &client_width, &client_height);
  if (!width) {
    width = client_width > 0 ? static_cast<u32>(client_width) :
        (creating ? g_hooks.width : g_d3d11_width.load());
    if (!width) width = g_hooks.width ? g_hooks.width : 1280;
  }
  if (!height) {
    height = client_height > 0 ? static_cast<u32>(client_height) :
        (creating ? g_hooks.height : g_d3d11_height.load());
    if (!height) height = g_hooks.height ? g_hooks.height : 720;
  }
  const u32 effective_count = count ? count : (creating ? 2 : g_d3d11_buffer_count.load());
  const int hr = g_hooks.configure_d3d11_swapchain
      ? g_hooks.configure_d3d11_swapchain(width, height, fmt, effective_count, effect) : 0;
  if (!creating) {
    std::lock_guard<std::mutex> lock(g_resize_stats_mutex);
    ++g_resize_stats.buffer_calls;
    g_resize_stats.buffer_requested_width = requested_width;
    g_resize_stats.buffer_requested_height = requested_height;
    g_resize_stats.buffer_client_width = static_cast<u32>(client_width);
    g_resize_stats.buffer_client_height = static_cast<u32>(client_height);
    g_resize_stats.buffer_present_count = g_d3d11_visible_presents.load();
    g_resize_stats.buffer_result = hr;
  }
  if (hr < 0) return hr;
  g_d3d11_width.store(width);
  g_d3d11_height.store(height);
  g_d3d11_hwnd = hwnd;
  if (fmt || creating) g_d3d11_guest_format = fmt;
  g_d3d11_swap_effect.store(effect);
  g_d3d11_buffer_count.store(effective_count);
  g_d3d11_stable_backbuffer.store(g_hooks.configure_d3d11_swapchain &&
      trt_gpu_d3d11_uses_stable_backbuffer(effective_count, effect));
  return 0;
}

void describe_guest_backbuffer(u32* dimensions, u32* format, u32* count, u32* effect) {
  if (g_sc_is_d3d12) return;
  const u32 width = g_d3d11_width.load(), height = g_d3d11_height.load();
  if (width && height) {
    dimensions[0] = width;
    dimensions[1] = height;
  }
  if (!g_d3d11_stable_backbuffer.load()) return;
  // Only replace formats the host can create without substituting another.
  switch (g_d3d11_guest_format) {
    case 10: case 24: case 28: case 29: case 87: case 91:
      *format = g_d3d11_guest_format;
      break;
    default: break;
  }
  *count = g_d3d11_buffer_count.load();
  *effect = g_d3d11_swap_effect.load();
}

void record_present(u32 flags, int hr) {
  if (flags & 1u /* DXGI_PRESENT_TEST */) return;
  if (!g_sc_is_d3d12) {
    g_d3d11_last_present.store(hr, std::memory_order_relaxed);
    if (hr == 0) g_d3d11_visible_presents.fetch_add(1, std::memory_order_relaxed);
    g_d3d11_presents.fetch_add(1, std::memory_order_relaxed);
  }
  if (hr == 0 && !g_sc_presented.exchange(true))
    trt_core_log("d3d: guest presented its first frame");
}

struct fake_swapchain {
  const void** vtbl;
  u32 refcount;
};

static void* active_sc() {
  if (g_sc_is_d3d12 && g_hooks.d3d12_swapchain) return g_hooks.d3d12_swapchain;
  return g_sc_is_d3d12 ? nullptr : g_hooks.swapchain;
}

static void* active_sc3() {
  return g_sc_is_d3d12 ? g_hooks.d3d12_swapchain : g_hooks.swapchain3;
}

static void* real_sc_slot(int index) {
  void* sc = (index >= 29) ? active_sc3() : active_sc();
  if (!sc) return nullptr;
  const void** vtbl = *(const void***)sc;
  return vtbl ? (void*)vtbl[index] : nullptr;
}

static bool guid_eq(const void* g, u32 d1, u16 d2, u16 d3,
                    u8 b0, u8 b1, u8 b2, u8 b3, u8 b4, u8 b5, u8 b6, u8 b7) {
  if (!g) return false;
  struct raw_guid { u32 d1; u16 d2; u16 d3; u8 d4[8]; };
  const raw_guid* r = (const raw_guid*)g;
  return r->d1 == d1 && r->d2 == d2 && r->d3 == d3 &&
         r->d4[0] == b0 && r->d4[1] == b1 && r->d4[2] == b2 && r->d4[3] == b3 &&
         r->d4[4] == b4 && r->d4[5] == b5 && r->d4[6] == b6 && r->d4[7] == b7;
}

static bool is_swapchain_iid(const void* riid) {
  // IUnknown, IDXGIObject, IDXGIDeviceSubObject
  if (guid_eq(riid, 0x00000000u, 0x0000, 0x0000, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46)) return true;
  if (guid_eq(riid, 0xAEC22FB8u, 0x76F3, 0x4639, 0x9B,0xE0,0x28,0xEB,0x43,0xA6,0x7A,0x2E)) return true;
  if (guid_eq(riid, 0x3D3E0379u, 0xF9DE, 0x4D58, 0xBB,0x6C,0x18,0xD6,0x29,0x92,0xF1,0xA6)) return true;
  // IDXGISwapChain .. IDXGISwapChain4
  if (guid_eq(riid, 0x310D36A0u, 0xD2E7, 0x4C0A, 0xAA,0x04,0x6A,0x9D,0x23,0xB8,0x88,0x6A)) return true;
  if (guid_eq(riid, 0x790A45F7u, 0x0D42, 0x4876, 0x98,0x3A,0x0A,0x55,0xCF,0xE6,0xF4,0xAA)) return true;
  if (guid_eq(riid, 0xA8BE2AC4u, 0x199F, 0x4946, 0xB3,0x31,0x79,0x59,0x9F,0xB9,0x8D,0xE7)) return true;
  if (guid_eq(riid, 0x94D99BDBu, 0xF1F8, 0x4AB0, 0xB2,0x36,0x7D,0xA0,0x17,0x0E,0xDA,0xB1)) return true;
  if (guid_eq(riid, 0x3D585D5Au, 0xBD4A, 0x489E, 0xB1,0xF4,0x3D,0xBC,0xB6,0x45,0x2F,0xFB)) return true;
  return false;
}

int TRT_GUEST_ABI fake_sc_QueryInterface(void* self, const void* riid, void** ppv) {
  if (!ppv) return TRT_E_NOINTERFACE;
  if (!riid || is_swapchain_iid(riid)) {
    *ppv = self;
    ++((fake_swapchain*)self)->refcount;
    return 0;
  }
  void* sc = active_sc();
  void* slot = real_sc_slot(0);
  if (!slot || !sc) { *ppv = nullptr; return TRT_E_NOINTERFACE; }
  using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, void**);
  return ((fn_t)slot)(sc, riid, ppv);
}
u32 TRT_GUEST_ABI fake_sc_AddRef(void* self) {
  return ++((fake_swapchain*)self)->refcount;
}
u32 TRT_GUEST_ABI fake_sc_Release(void* self) {
  fake_swapchain* sc = (fake_swapchain*)self;
  if (sc->refcount > 1) --sc->refcount;  // host owns real swapchain
  return sc->refcount;
}
int TRT_GUEST_ABI fake_sc_SetPrivateData(void* self, const void* name, u32 sz, const void* data) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(3);
  if (!slot || !sc) return TRT_E_NOTIMPL;
  using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, u32, const void*);
  return ((fn_t)slot)(sc, name, sz, data);
}
int TRT_GUEST_ABI fake_sc_SetPrivateDataInterface(void* self, const void* name, void* unk) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(4);
  if (!slot || !sc) return TRT_E_NOTIMPL;
  using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, void*);
  return ((fn_t)slot)(sc, name, unk);
}
int TRT_GUEST_ABI fake_sc_GetPrivateData(void* self, const void* name, u32* sz, void* data) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(5);
  if (!slot || !sc) return TRT_E_NOTIMPL;
  using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, u32*, void*);
  return ((fn_t)slot)(sc, name, sz, data);
}
int TRT_GUEST_ABI fake_sc_GetParent(void* self, const void* riid, void** parent) {
  (void)self; (void)riid; (void)parent;
  return TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_GetDevice(void* self, const void* riid, void** dev) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(7);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, void**);
    return ((fn_t)slot)(sc, riid, dev);
  }
  if (!dev) return TRT_E_NOINTERFACE;
  *dev = g_sc_is_d3d12 ? g_hooks.d3d12_device : g_hooks.device;
  return *dev ? 0 : TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_Present(void* self, u32 sync, u32 flags) {
  (void)self;
  // DXGI_PRESENT_ALLOW_TEARING (0x200) fails on UWP FLIP_SEQUENTIAL swapchains
  const u32 safe_flags = flags & ~0x200u;
  void* sc = active_sc();
  void* slot = real_sc_slot(8);
  if (slot && sc) {
    if (!g_sc_is_d3d12 && !(safe_flags & 1u /*DXGI_PRESENT_TEST*/) &&
        g_hooks.prepare_d3d11_present)
      g_hooks.prepare_d3d11_present();
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32);
    const int hr = ((fn_t)slot)(sc, sync, safe_flags);
    record_present(safe_flags, hr);
    return hr;
  }
  if (g_sc_is_d3d12) {
    g_sc_backbuf_idx = (g_sc_backbuf_idx + 1u) % (g_sc_buf_count ? g_sc_buf_count : 2u);
    return 0;
  }
  return TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_GetBuffer(void* self, u32 buffer, const void* riid, void** out) {
  (void)self;
  if (!g_sc_is_d3d12) g_d3d11_buffers.fetch_add(1, std::memory_order_relaxed);
  void* sc = active_sc();
  void* slot = real_sc_slot(9);
  if (!g_sc_is_d3d12 && g_hooks.get_d3d11_backbuffer) {
    const int hr = g_hooks.get_d3d11_backbuffer(buffer, riid, out);
    if (hr != TRT_E_NOTIMPL) return hr;
  }
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
    return ((fn_t)slot)(sc, buffer, riid, out);
  }
  if (g_sc_is_d3d12) {
    return trt_d3d12_offline_backbuffer(
        buffer, g_sc_width ? g_sc_width : 1280,
        g_sc_height ? g_sc_height : 720, riid, out);
  }
  return TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_SetFullscreenState(void* self, int fullscreen, void* target) {
  (void)self; (void)target;
  char line[128];
  snprintf(line, sizeof(line),
           "trt: IDXGISwapChain::SetFullscreenState(%d) -> S_OK (CoreWindow)",
           fullscreen);
  trt_core_log(line);
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetFullscreenState(void* self, int* fullscreen, void** target) {
  (void)self;
  if (fullscreen) *fullscreen = 0;
  if (target) *target = nullptr;
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetDesc(void* self, void* desc) {
  (void)self;
  if (!desc) return TRT_E_INVALIDARG;
  u32* d = static_cast<u32*>(desc);
  void* sc = active_sc();
  void* slot = real_sc_slot(12);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, void*);
    const int hr = ((fn_t)slot)(sc, desc);
    if (hr >= 0) {
      describe_guest_backbuffer(d, &d[4], &d[10], &d[15]);
      if (!g_sc_is_d3d12 && g_d3d11_hwnd)
        memcpy(d + 12, &g_d3d11_hwnd, sizeof(g_d3d11_hwnd));
    }
    return hr;
  }
  // DXGI_SWAP_CHAIN_DESC layout (72 bytes on x64)
  memset(d, 0, 72);
  d[0] = g_sc_width ? g_sc_width : (g_hooks.width ? g_hooks.width : 1280);
  d[1] = g_sc_height ? g_sc_height : (g_hooks.height ? g_hooks.height : 720);
  d[2] = 60; d[3] = 1;
  d[4] = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM
  d[7] = 1;    // SampleDesc.Count
  d[9] = 0x20; // DXGI_USAGE_RENDER_TARGET_OUTPUT
  d[10] = g_sc_buf_count ? g_sc_buf_count : 2u;  // BufferCount
  d[14] = 1;   // Windowed = TRUE
  describe_guest_backbuffer(d, &d[4], &d[10], &d[15]);
  if (!g_sc_is_d3d12 && g_d3d11_hwnd)
    memcpy(d + 12, &g_d3d11_hwnd, sizeof(g_d3d11_hwnd));
  return 0;
}
int TRT_GUEST_ABI fake_sc_ResizeBuffers(void* self, u32 count, u32 w, u32 h, u32 fmt, u32 flags) {
  (void)self;
  if (!g_sc_is_d3d12 && g_hooks.configure_d3d11_swapchain)
    return configure_guest_swapchain(w, h, fmt, count, g_d3d11_swap_effect.load(),
                                     g_d3d11_hwnd, false);
  void* sc = active_sc();
  void* slot = real_sc_slot(13);
  int hr = 0;
  if (g_sc_is_d3d12) {
    if (count >= 2) g_sc_buf_count = count;
    if (w) g_sc_width = w;
    if (h) g_sc_height = h;
    g_sc_backbuf_idx = 0;
  }
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32);
    // Strip ALLOW_MODE_SWITCH (0x2) & ALLOW_TEARING (0x800) before host resize
    hr = ((fn_t)slot)(sc, count, w, h, fmt, flags & ~0x802u);
  }
  if (hr < 0) {
    char line[160];
    snprintf(line, sizeof(line),
             "trt: IDXGISwapChain::ResizeBuffers(%ux%u fmt=%u) host hr=0x%08x -> "
             "S_OK (CoreWindow fixed size)",
             w, h, fmt, (u32)hr);
    trt_core_log(line);
  }
  return 0;
}
int TRT_GUEST_ABI fake_sc_ResizeTarget(void*, const void* mode) {
  gap_trace_record("DXGI.ResizeTarget");
  const auto* dimensions = static_cast<const u32*>(mode);
  const u32 width = dimensions ? dimensions[0] : 0;
  const u32 height = dimensions ? dimensions[1] : 0;
  constexpr int invalid_call = static_cast<int>(0x887A0001u);
  constexpr int unavailable = static_cast<int>(0x887A0022u);
  int result = invalid_call;
  if (dimensions && width <= INT32_MAX && height <= INT32_MAX) {
    result = unavailable;
    if (!g_sc_is_d3d12 && g_d3d11_hwnd) {
      // Desktop windowed ResizeTarget changes the client rectangle. A later
      // ResizeBuffers (including one from WM_SIZE) re-creates the buffers.
      constexpr u32 flags = 0x0002u | 0x0004u | 0x0010u;  // NOMOVE | NOZORDER | NOACTIVATE
      result = trt_SetWindowPos(g_d3d11_hwnd, nullptr, 0, 0,
                               static_cast<i32>(width), static_cast<i32>(height), flags)
                   ? 0 : invalid_call;
    }
  }
  std::lock_guard<std::mutex> lock(g_resize_stats_mutex);
  ++g_resize_stats.target_calls;
  g_resize_stats.target_width = width;
  g_resize_stats.target_height = height;
  g_resize_stats.target_result = result;
  return result;
}
static void* out_get_offline_output(u32* addref_done);

int TRT_GUEST_ABI fake_sc_GetContainingOutput(void* self, void** out) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(15);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, void**);
    const int hr = ((fn_t)slot)(sc, out);
    if (hr >= 0 && out && *out) return hr;
  }
  if (!out) return TRT_E_INVALIDARG;
  u32 dummy = 0;
  *out = out_get_offline_output(&dummy);
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetFrameStatistics(void* self, void* stats) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(16);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, void*);
    return ((fn_t)slot)(sc, stats);
  }
  return TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_GetLastPresentCount(void* self, u32* count) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(17);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32*);
    return ((fn_t)slot)(sc, count);
  }
  if (count) *count = 0;
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetDesc1(void* self, void* desc1) {
  (void)self;
  if (!desc1) return TRT_E_INVALIDARG;
  u32* d = static_cast<u32*>(desc1);
  void* sc = active_sc();
  void* slot = real_sc_slot(18);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, void*);
    const int hr = ((fn_t)slot)(sc, desc1);
    if (hr >= 0) describe_guest_backbuffer(d, &d[2], &d[7], &d[9]);
    return hr;
  }
  memset(d, 0, 48);
  d[0] = g_sc_width ? g_sc_width : (g_hooks.width ? g_hooks.width : 1280);
  d[1] = g_sc_height ? g_sc_height : (g_hooks.height ? g_hooks.height : 720);
  d[2] = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM
  d[4] = 1;    // SampleDesc.Count
  d[6] = 0x20; // DXGI_USAGE_RENDER_TARGET_OUTPUT
  d[7] = g_sc_buf_count ? g_sc_buf_count : 2u;  // BufferCount
  d[9] = 3;    // DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
  describe_guest_backbuffer(d, &d[2], &d[7], &d[9]);
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetFullscreenDesc(void* self, void* fdesc) {
  (void)self;
  if (!fdesc) return (int)0x80070057;
  // DXGI_SWAP_CHAIN_FULLSCREEN_DESC has five DWORD fields.
  auto* desc = static_cast<u32*>(fdesc);
  memset(desc, 0, 5 * sizeof(*desc));
  desc[4] = 1;  // Windowed = TRUE
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetHwnd(void* self, void** hwnd) {
  (void)self;
  if (!hwnd) return TRT_E_INVALIDARG;
  *hwnd = !g_sc_is_d3d12 ? g_d3d11_hwnd : nullptr;
  return *hwnd ? 0 : (int)0x887A0001u;  // DXGI_ERROR_INVALID_CALL on CoreWindow
}
int TRT_GUEST_ABI fake_sc_GetCoreWindow(void* self, const void* riid, void** unk) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(21);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, void**);
    return ((fn_t)slot)(sc, riid, unk);
  }
  if (unk) *unk = nullptr;
  return TRT_E_NOTIMPL;
}
int TRT_GUEST_ABI fake_sc_Present1(void* self, u32 sync, u32 flags, const void* params) {
  (void)self;
  void* sc = active_sc();
  void* slot = real_sc_slot(22);
  if (slot && sc) {
    if (!g_sc_is_d3d12 && !(flags & 1u /*DXGI_PRESENT_TEST*/) &&
        g_hooks.prepare_d3d11_present)
      g_hooks.prepare_d3d11_present();
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32, const void*);
    const int hr = ((fn_t)slot)(sc, sync, flags & ~0x200u, params);
    record_present(flags, hr);
    return hr;
  }
  return fake_sc_Present(self, sync, flags);
}
int TRT_GUEST_ABI fake_sc_IsTemporaryMonoSupported(void*) { return 0; }
int TRT_GUEST_ABI fake_sc_GetRestrictToOutput(void*, void** out) {
  if (out) *out = nullptr;
  return 0;
}
int TRT_GUEST_ABI fake_sc_SetBackgroundColor(void*, const void*) { return 0; }
int TRT_GUEST_ABI fake_sc_GetBackgroundColor(void*, void* color) {
  if (color) memset(color, 0, 16);
  return 0;
}
int TRT_GUEST_ABI fake_sc_SetRotation(void*, u32) { return 0; }
int TRT_GUEST_ABI fake_sc_GetRotation(void*, u32* rot) {
  if (rot) *rot = 1;  // DXGI_MODE_ROTATION_IDENTITY
  return 0;
}
int TRT_GUEST_ABI fake_sc_SetSourceSize(void*, u32 w, u32 h) {
  if (w) g_sc_width = w;
  if (h) g_sc_height = h;
  return 0;
}
int TRT_GUEST_ABI fake_sc_GetSourceSize(void*, u32* w, u32* h) {
  if (w) *w = g_sc_width ? g_sc_width : (g_hooks.width ? g_hooks.width : 1280);
  if (h) *h = g_sc_height ? g_sc_height : (g_hooks.height ? g_hooks.height : 720);
  return 0;
}
int TRT_GUEST_ABI fake_sc_SetMaxFrameLatency(void*, u32) { return 0; }
int TRT_GUEST_ABI fake_sc_GetMaxFrameLatency(void*, u32* max_lat) {
  if (max_lat) *max_lat = 2;
  return 0;
}
void* TRT_GUEST_ABI fake_sc_GetWaitableObject(void*) {
  void* sc = active_sc3();
  void* slot = real_sc_slot(33);
  if (slot && sc) {
    using fn_t = void* (TRT_GUEST_ABI *)(void*);
    return ((fn_t)slot)(sc);
  }
  return nullptr;
}
int TRT_GUEST_ABI fake_sc_SetMatrixTransform(void*, const void*) { return 0; }
int TRT_GUEST_ABI fake_sc_GetMatrixTransform(void*, void* m) {
  if (m) {
    float* f = (float*)m;
    memset(f, 0, 24);
    f[0] = 1.0f; f[3] = 1.0f;
  }
  return 0;
}
u32 TRT_GUEST_ABI fake_sc_GetCurrentBackBufferIndex(void*) {
  if (!g_sc_is_d3d12 && g_d3d11_stable_backbuffer.load()) return 0;
  void* sc = active_sc3();
  void* slot = real_sc_slot(36);
  if (slot && sc) {
    using fn_t = u32 (TRT_GUEST_ABI *)(void*);
    return ((fn_t)slot)(sc);
  }
  return g_sc_backbuf_idx % (g_sc_buf_count ? g_sc_buf_count : 2u);
}
int TRT_GUEST_ABI fake_sc_CheckColorSpaceSupport(void*, u32 cs, u32* flags) {
  void* sc = active_sc3();
  void* slot = real_sc_slot(37);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32*);
    return ((fn_t)slot)(sc, cs, flags);
  }
  if (flags) *flags = (cs == 0 || cs == 12) ? 1u : 0u;
  return 0;
}
int TRT_GUEST_ABI fake_sc_SetColorSpace1(void*, u32 cs) {
  void* sc = active_sc3();
  void* slot = real_sc_slot(38);
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32);
    return ((fn_t)slot)(sc, cs);
  }
  g_sc_colorspace = cs;
  return 0;
}
int TRT_GUEST_ABI fake_sc_ResizeBuffers1(void* self, u32 count, u32 w, u32 h,
                                         u32 fmt, u32 flags, const u32* mask,
                                         void* const* queues) {
  if (!g_sc_is_d3d12 && g_hooks.configure_d3d11_swapchain)
    return fake_sc_ResizeBuffers(self, count, w, h, fmt, flags);
  void* sc = active_sc3();
  void* slot = real_sc_slot(39);
  if (g_sc_is_d3d12) {
    if (count >= 2) g_sc_buf_count = count;
    if (w) g_sc_width = w;
    if (h) g_sc_height = h;
    g_sc_backbuf_idx = 0;
  }
  if (slot && sc) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32, const u32*, void* const*);
    const int hr = ((fn_t)slot)(sc, count, w, h, fmt, flags & ~0x802u, mask, queues);
    if (hr >= 0) return hr;
  }
  return fake_sc_ResizeBuffers(self, count, w, h, fmt, flags);
}
int TRT_GUEST_ABI fake_sc_SetHDRMetaData(void*, u32 type, u32 size, const void* data) {
  (void)type; (void)size; (void)data;
  return 0;
}

const void* g_swapchain_proxy_vtbl[41] = {
    (const void*)&fake_sc_QueryInterface,           // 0
    (const void*)&fake_sc_AddRef,                   // 1
    (const void*)&fake_sc_Release,                  // 2
    (const void*)&fake_sc_SetPrivateData,           // 3
    (const void*)&fake_sc_SetPrivateDataInterface,  // 4
    (const void*)&fake_sc_GetPrivateData,           // 5
    (const void*)&fake_sc_GetParent,                // 6
    (const void*)&fake_sc_GetDevice,                // 7
    (const void*)&fake_sc_Present,                  // 8
    (const void*)&fake_sc_GetBuffer,                // 9
    (const void*)&fake_sc_SetFullscreenState,       // 10
    (const void*)&fake_sc_GetFullscreenState,       // 11
    (const void*)&fake_sc_GetDesc,                  // 12
    (const void*)&fake_sc_ResizeBuffers,            // 13
    (const void*)&fake_sc_ResizeTarget,             // 14
    (const void*)&fake_sc_GetContainingOutput,      // 15
    (const void*)&fake_sc_GetFrameStatistics,       // 16
    (const void*)&fake_sc_GetLastPresentCount,      // 17
    (const void*)&fake_sc_GetDesc1,                 // 18
    (const void*)&fake_sc_GetFullscreenDesc,        // 19
    (const void*)&fake_sc_GetHwnd,                  // 20
    (const void*)&fake_sc_GetCoreWindow,            // 21
    (const void*)&fake_sc_Present1,                 // 22
    (const void*)&fake_sc_IsTemporaryMonoSupported, // 23
    (const void*)&fake_sc_GetRestrictToOutput,      // 24
    (const void*)&fake_sc_SetBackgroundColor,       // 25
    (const void*)&fake_sc_GetBackgroundColor,       // 26
    (const void*)&fake_sc_SetRotation,              // 27
    (const void*)&fake_sc_GetRotation,              // 28
    (const void*)&fake_sc_SetSourceSize,            // 29
    (const void*)&fake_sc_GetSourceSize,            // 30
    (const void*)&fake_sc_SetMaxFrameLatency,       // 31
    (const void*)&fake_sc_GetMaxFrameLatency,       // 32
    (const void*)&fake_sc_GetWaitableObject,        // 33
    (const void*)&fake_sc_SetMatrixTransform,       // 34
    (const void*)&fake_sc_GetMatrixTransform,       // 35
    (const void*)&fake_sc_GetCurrentBackBufferIndex,// 36
    (const void*)&fake_sc_CheckColorSpaceSupport,   // 37
    (const void*)&fake_sc_SetColorSpace1,           // 38
    (const void*)&fake_sc_ResizeBuffers1,           // 39
    (const void*)&fake_sc_SetHDRMetaData,           // 40
};
fake_swapchain g_swapchain_proxy_obj = {g_swapchain_proxy_vtbl, 1};

// ---------------------------------------------------------------------------
// Offline IDXGIAdapter4 (used when no hardware factory hook is present)
// ---------------------------------------------------------------------------
struct offline_adapter {
  const void** vtbl;
  u32 refcount;
};

// Version-aware QI (the d3d12 BUG-6 rule applied to DXGI): the 19-slot
// vtbl below serves exactly the IDXGIAdapter chain whose slot count it
// covers — IDXGIAdapter(9) .. IDXGIAdapter4(19, GetDesc3). IIDs verified
// verbatim against the xwin SDK dxgi.h/dxgi1_2.h/dxgi1_4.h/dxgi1_6.h
// DEFINE_GUID block. Anything else fails VISIBLY with E_NOINTERFACE
// instead of handing back an object a game would call off the end of.
bool is_adapter_iid(const void* riid) {
  if (!riid) return false;
  if (guid_eq(riid, 0x00000000u, 0x0000, 0x0000, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46)) return true; // IUnknown
  if (guid_eq(riid, 0xAEC22FB8u, 0x76F3, 0x4639, 0x9B,0xE0,0x28,0xEB,0x43,0xA6,0x7A,0x2E)) return true; // IDXGIObject
  if (guid_eq(riid, 0x2411E7E1u, 0x12AC, 0x4CCF, 0xBD,0x14,0x97,0x98,0xE8,0x53,0x4D,0xC0)) return true; // IDXGIAdapter
  if (guid_eq(riid, 0x29038F61u, 0x3839, 0x4626, 0x91,0xFD,0x08,0x68,0x79,0x01,0x1A,0x05)) return true; // IDXGIAdapter1
  if (guid_eq(riid, 0x0AA1AE0Au, 0xFA0E, 0x4B84, 0x86,0x44,0xE0,0x5F,0xF8,0xE5,0xAC,0xB5)) return true; // IDXGIAdapter2
  if (guid_eq(riid, 0x645967A4u, 0x1392, 0x4310, 0xA7,0x98,0x80,0x53,0xCE,0x3E,0x93,0xFD)) return true; // IDXGIAdapter3
  if (guid_eq(riid, 0x3C8D99D1u, 0x4FBF, 0x4181, 0xA8,0x2C,0xAF,0x66,0xBF,0x7B,0xD2,0x4E)) return true; // IDXGIAdapter4
  return false;
}

int TRT_GUEST_ABI adp_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return TRT_E_NOINTERFACE;
  *ppv = nullptr;
  if (!is_adapter_iid(riid)) return TRT_E_NOINTERFACE;
  *ppv = self;
  ++((offline_adapter*)self)->refcount;
  return 0;
}
u32 TRT_GUEST_ABI adp_addref(void* self) { return ++((offline_adapter*)self)->refcount; }
u32 TRT_GUEST_ABI adp_release(void* self) {
  auto* a = (offline_adapter*)self;
  return a->refcount > 1 ? --a->refcount : 1;
}
int TRT_GUEST_ABI adp_priv(void*, const void*, u32, const void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI adp_priv_iface(void*, const void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI adp_get_priv(void*, const void*, u32*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI adp_get_parent(void*, const void*, void** p) {
  if (p) *p = nullptr;
  return TRT_E_NOTIMPL;
}
struct offline_output {
  const void** vtbl;
  u32 refcount;
};

static bool out_guid_eq(const void* a, u32 d1, u16 d2, u16 d3, u8 b0, u8 b1,
                        u8 b2, u8 b3, u8 b4, u8 b5, u8 b6, u8 b7) {
  if (!a) return false;
  const u8* p = (const u8*)a;
  const u8 target[16] = {
      (u8)(d1 & 0xFF), (u8)((d1 >> 8) & 0xFF), (u8)((d1 >> 16) & 0xFF),
      (u8)((d1 >> 24) & 0xFF), (u8)(d2 & 0xFF), (u8)((d2 >> 8) & 0xFF),
      (u8)(d3 & 0xFF), (u8)((d3 >> 8) & 0xFF),
      b0, b1, b2, b3, b4, b5, b6, b7,
  };
  return memcmp(p, target, 16) == 0;
}

u32 TRT_GUEST_ABI out_addref(void* self) {
  return ++((offline_output*)self)->refcount;
}
u32 TRT_GUEST_ABI out_release(void* self) {
  auto* o = (offline_output*)self;
  return o->refcount > 1 ? --o->refcount : 1;
}
int TRT_GUEST_ABI out_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return TRT_E_INVALIDARG;
  *ppv = nullptr;
  // Version-aware (BUG-6 rule): the vtbl below serves the full IDXGIOutput
  // chain through IDXGIOutput6 (29 slots). IIDs verified verbatim against
  // the xwin SDK dxgi.h..dxgi1_6.h DEFINE_GUID block. Anything else fails
  // VISIBLY with E_NOINTERFACE.
  if (out_guid_eq(riid, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x46) ||                      // IUnknown
      out_guid_eq(riid, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb,
                  0x43, 0xa6, 0x7a, 0x2e) ||                      // IDXGIObject
      out_guid_eq(riid, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d,
                  0xc2, 0x02, 0x13, 0xaa) ||                      // IDXGIOutput
      out_guid_eq(riid, 0x00cddea8, 0x939b, 0x4b83, 0xa3, 0x40, 0xa6, 0x85,
                  0x22, 0x66, 0x66, 0xcc) ||                      // IDXGIOutput1
      out_guid_eq(riid, 0x595e39d1, 0x2724, 0x4663, 0x99, 0xb1, 0xda, 0x96,
                  0x9d, 0xe2, 0x83, 0x64) ||                      // IDXGIOutput2
      out_guid_eq(riid, 0x8a6bb301, 0x7e7e, 0x41f4, 0xa8, 0xe0, 0x5b, 0x32,
                  0xf7, 0xf9, 0x9b, 0x18) ||                      // IDXGIOutput3
      out_guid_eq(riid, 0xdc7dca35, 0x2196, 0x414d, 0x9f, 0x53, 0x61, 0x78,
                  0x84, 0x03, 0x2a, 0x60) ||                      // IDXGIOutput4
      out_guid_eq(riid, 0x80a07424, 0xab52, 0x42eb, 0x83, 0x3c, 0x0c, 0x42,
                  0xfd, 0x28, 0x2d, 0x98) ||                      // IDXGIOutput5
      out_guid_eq(riid, 0x068346e8, 0xaaec, 0x4b84, 0xad, 0xd7, 0x13, 0x7f,
                  0x51, 0x3f, 0x77, 0xa1)) {                      // IDXGIOutput6
    *ppv = self;
    out_addref(self);
    return 0;
  }
  return TRT_E_NOINTERFACE;
}

extern offline_output g_offline_output_obj;
extern offline_adapter g_offline_adapter_obj;

int TRT_GUEST_ABI out_get_parent(void*, const void*, void** pp) {
  if (!pp) return TRT_E_INVALIDARG;
  *pp = &g_offline_adapter_obj;
  adp_addref(&g_offline_adapter_obj);
  return 0;
}

static void out_active_dims(u32* w, u32* h) {
  int dw = 0, dh = 0;
  trt_display_size(&dw, &dh);
  *w = dw > 0 ? (u32)dw : 1920u;
  *h = dh > 0 ? (u32)dh : 1080u;
}

int TRT_GUEST_ABI out_get_desc(void*, void* pDesc) {
  if (!pDesc) return TRT_E_INVALIDARG;
  u8* b = (u8*)pDesc;
  memset(b, 0, 96);  // DXGI_OUTPUT_DESC x64
  static const char kName[] = "\\\\.\\DISPLAY1";
  u16* wname = (u16*)b;
  for (size_t i = 0; i < sizeof(kName); ++i) wname[i] = (u16)(u8)kName[i];
  u32 w = 1920, h = 1080;
  out_active_dims(&w, &h);
  *(i32*)(b + 64) = 0;
  *(i32*)(b + 68) = 0;
  *(i32*)(b + 72) = (i32)w;
  *(i32*)(b + 76) = (i32)h;
  *(u32*)(b + 80) = 1;                // AttachedToDesktop = TRUE
  *(u32*)(b + 84) = 1;                // DXGI_MODE_ROTATION_IDENTITY
  *(u64*)(b + 88) = 0x00010001ULL;    // HMONITOR
  return 0;
}

static u32 out_build_modes(u32 fmt, u32 widths[2], u32 heights[2]) {
  u32 dw = 1920, dh = 1080;
  out_active_dims(&dw, &dh);
  widths[0] = dw;
  heights[0] = dh;
  if (dw != 1280 || dh != 720) {
    widths[1] = 1280;
    heights[1] = 720;
    (void)fmt;
    return 2;
  }
  widths[1] = 1920;
  heights[1] = 1080;
  return 2;
}

int TRT_GUEST_ABI out_get_modes(void*, u32 fmt, u32 flags, u32* pNum,
                                void* pDesc) {
  (void)flags;
  if (!pNum) return TRT_E_INVALIDARG;
  u32 ws[2], hs[2];
  const u32 total = out_build_modes(fmt, ws, hs);
  if (!pDesc) {
    *pNum = total;
    return 0;
  }
  const u32 cap = *pNum;
  const u32 n = cap < total ? cap : total;
  const u32 use_fmt = fmt ? fmt : 28u;  // R8G8B8A8_UNORM
  for (u32 i = 0; i < n; ++i) {
    u32* m = (u32*)((u8*)pDesc + (size_t)i * 28);
    m[0] = ws[i];
    m[1] = hs[i];
    m[2] = 60;
    m[3] = 1;
    m[4] = use_fmt;
    m[5] = 0;
    m[6] = 0;
  }
  *pNum = total;
  return (cap < total) ? (int)0x887A0003u /*DXGI_ERROR_MORE_DATA*/ : 0;
}

int TRT_GUEST_ABI out_find_closest(void*, const void* match, void* closest,
                                   void*) {
  if (!match || !closest) return TRT_E_INVALIDARG;
  memcpy(closest, match, 28);
  u32* m = (u32*)closest;
  u32 dw = 1920, dh = 1080;
  out_active_dims(&dw, &dh);
  if (m[0] == 0) m[0] = dw;
  if (m[1] == 0) m[1] = dh;
  if (m[2] == 0 || m[3] == 0) {
    m[2] = 60;
    m[3] = 1;
  }
  if (m[4] == 0) m[4] = 28u;
  return 0;
}

int TRT_GUEST_ABI out_get_modes1(void*, u32 fmt, u32 flags, u32* pNum,
                                 void* pDesc1) {
  (void)flags;
  if (!pNum) return TRT_E_INVALIDARG;
  u32 ws[2], hs[2];
  const u32 total = out_build_modes(fmt, ws, hs);
  if (!pDesc1) {
    *pNum = total;
    return 0;
  }
  const u32 cap = *pNum;
  const u32 n = cap < total ? cap : total;
  const u32 use_fmt = fmt ? fmt : 28u;
  for (u32 i = 0; i < n; ++i) {
    u32* m = (u32*)((u8*)pDesc1 + (size_t)i * 32);
    m[0] = ws[i];
    m[1] = hs[i];
    m[2] = 60;
    m[3] = 1;
    m[4] = use_fmt;
    m[5] = 0;
    m[6] = 0;
    m[7] = 0;  // Stereo = FALSE
  }
  *pNum = total;
  return (cap < total) ? (int)0x887A0003u : 0;
}

int TRT_GUEST_ABI out_find_closest1(void* self, const void* match,
                                    void* closest, void* dev) {
  const int hr = out_find_closest(self, match, closest, dev);
  if (hr == 0 && match && closest) {
    ((u32*)closest)[7] = ((const u32*)match)[7];
  }
  return hr;
}

int TRT_GUEST_ABI out_ok0(void*) { return 0; }
int TRT_GUEST_ABI out_take_own(void*, void*, int) { return 0; }
void TRT_GUEST_ABI out_rel_own(void*) {}
int TRT_GUEST_ABI out_notimpl_p1(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI out_ok_const(void*, const void*) { return 0; }

int TRT_GUEST_ABI out_get_frame_stats(void*, void* pStats) {
  if (!pStats) return TRT_E_INVALIDARG;
  memset(pStats, 0, 32);  // DXGI_FRAME_STATISTICS
  static u32 s_frame = 0;
  const u32 cnt = ++s_frame;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  const u64 qpc = (u64)(ns / 100);  // 100ns ticks
  ((u32*)pStats)[0] = cnt;
  ((u32*)pStats)[1] = cnt;
  ((u32*)pStats)[2] = cnt;
  ((u64*)pStats)[2] = qpc;
  ((u64*)pStats)[3] = qpc;
  return 0;
}

int TRT_GUEST_ABI out_dup_output(void*, void*, void** ppDup) {
  if (ppDup) *ppDup = nullptr;
  return TRT_E_NOTIMPL;
}

// ---- IDXGIOutput2..IDXGIOutput6 (SDK dxgi1_3.h..dxgi1_6.h ABI) ----------
// SupportsOverlays: the DXGI_OUTPUT_DESC1-era probes; honest FALSE / named
// E_NOTIMPL — CoreWindow composition owns overlays on console.
int TRT_GUEST_ABI out_supports_overlays(void*) { return 0; }  // FALSE
int TRT_GUEST_ABI out_check_overlay_support(void*, const void*, void*, u32* flags) {
  if (flags) *flags = 0;
  return (int)0x887A0004u;  // DXGI_ERROR_UNSUPPORTED: no overlay support
}
int TRT_GUEST_ABI out_check_overlay_colorspace(void*, const void*, void*, u32* flags) {
  if (flags) *flags = 0;
  return (int)0x887A0004u;  // DXGI_ERROR_UNSUPPORTED
}
int TRT_GUEST_ABI out_dup_output1(void*, u32, u32, const void*, void** ppDup) {
  if (ppDup) *ppDup = nullptr;
  return TRT_E_NOTIMPL;
}
// GetDesc1 (IDXGIOutput6): DXGI_OUTPUT_DESC1 — the HDR/colorimetry probe
// modern engines (UE5/DECIMA/REDengine) run at boot. BitsPerColor 10 +
// sRGB color space, scRGB luminance window: honest RDNA2-class values.
int TRT_GUEST_ABI out_get_desc1(void*, void* desc1) {
  if (!desc1) return TRT_E_INVALIDARG;
  // DXGI_OUTPUT_DESC1 x64: DESC prefix is 96 bytes (DeviceName[32] WCHAR +
  // RECT + Rotation + HMONITOR), then BitsPerColor(4) ColorSpace(4)
  // RedPrimary/GreenPrimary/BluePrimary/WhitePoint (8 floats)
  // MinLuminance MaxLuminance MaxFullFrameLuminance (3 floats) = 148.
  out_get_desc(nullptr, desc1);  // fill the DESC prefix (DeviceName/RECT/...)
  u32* bpc = (u32*)((u8*)desc1 + 96);
  *bpc = 10;                     // BitsPerColor
  *(u32*)((u8*)desc1 + 100) = 0; // DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 (0)
  float* f = (float*)((u8*)desc1 + 104);
  f[0] = 0.640f; f[1] = 0.330f;  // RedPrimary
  f[2] = 0.300f; f[3] = 0.600f;  // GreenPrimary
  f[4] = 0.150f; f[5] = 0.060f;  // BluePrimary
  f[6] = 0.3127f; f[7] = 0.3290f;// WhitePoint
  f[8] = 0.0f;                   // MinLuminance
  f[9] = 1499.0f;                // MaxLuminance
  f[10] = 799.0f;                // MaxFullFrameLuminance
  return 0;
}
int TRT_GUEST_ABI out_check_hw_composition(void*, u32* flags) {
  if (!flags) return TRT_E_INVALIDARG;
  *flags = 2u;  // DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED
  return 0;
}

// IDXGIOutput6 vtable: 29 slots (SDK dxgi1_6.h IDXGIOutput6Vtbl order)
const void* g_offline_output_vtbl[29] = {
    (const void*)&out_qi,               // 0
    (const void*)&out_addref,           // 1
    (const void*)&out_release,          // 2
    (const void*)&adp_priv,             // 3 SetPrivateData
    (const void*)&adp_priv_iface,       // 4 SetPrivateDataInterface
    (const void*)&adp_get_priv,         // 5 GetPrivateData
    (const void*)&out_get_parent,       // 6 GetParent
    (const void*)&out_get_desc,         // 7 GetDesc
    (const void*)&out_get_modes,        // 8 GetDisplayModeList
    (const void*)&out_find_closest,     // 9 FindClosestMatchingMode
    (const void*)&out_ok0,              // 10 WaitForVBlank
    (const void*)&out_take_own,         // 11 TakeOwnership
    (const void*)&out_rel_own,          // 12 ReleaseOwnership
    (const void*)&out_notimpl_p1,       // 13 GetGammaControlCapabilities
    (const void*)&out_ok_const,         // 14 SetGammaControl
    (const void*)&out_notimpl_p1,       // 15 GetGammaControl
    (const void*)&out_notimpl_p1,       // 16 SetDisplaySurface
    (const void*)&out_notimpl_p1,       // 17 GetDisplaySurfaceData
    (const void*)&out_get_frame_stats,  // 18 GetFrameStatistics
    (const void*)&out_get_modes1,       // 19 GetDisplayModeList1
    (const void*)&out_find_closest1,    // 20 FindClosestMatchingMode1
    (const void*)&out_notimpl_p1,       // 21 GetDisplaySurfaceData1
    (const void*)&out_dup_output,       // 22 DuplicateOutput
    (const void*)&out_supports_overlays,        // 23 IDXGIOutput2
    (const void*)&out_check_overlay_support,    // 24 IDXGIOutput3
    (const void*)&out_check_overlay_colorspace, // 25 IDXGIOutput4
    (const void*)&out_dup_output1,              // 26 IDXGIOutput5
    (const void*)&out_get_desc1,                // 27 IDXGIOutput6::GetDesc1
    (const void*)&out_check_hw_composition,     // 28 IDXGIOutput6::CheckHardwareCompositionSupport
};
offline_output g_offline_output_obj = {g_offline_output_vtbl, 1};

static void* out_get_offline_output(u32* addref_done) {
  if (addref_done) *addref_done = out_addref(&g_offline_output_obj);
  return &g_offline_output_obj;
}

int TRT_GUEST_ABI adp_enum_outputs(void*, u32 idx, void** out) {
  if (!out) return TRT_E_INVALIDARG;
  if (idx == 0) {
    u32 dummy = 0;
    *out = out_get_offline_output(&dummy);
    return 0;
  }
  *out = nullptr;
  return (int)0x887A0002u;  // DXGI_ERROR_NOT_FOUND
}
static void fill_adapter_desc(void* out, size_t total_sz) {
  memset(out, 0, total_sz);
  u16* name = (u16*)out;
  const char* label = "Tiramisu Xbox GPU";
  for (int i = 0; label[i]; ++i) name[i] = (u16)(unsigned char)label[i];
  u8* b = (u8*)out;
  *(u32*)(b + 256) = 0x1002u;  // VendorId (AMD RDNA2 Xbox SoC class)
  *(u32*)(b + 260) = 0x13FFu;  // DeviceId
  *(u64*)(b + 272) = 8ULL * 1024 * 1024 * 1024;  // DedicatedVideoMemory
  *(u64*)(b + 288) = 4ULL * 1024 * 1024 * 1024;  // SharedSystemMemory
  *(u32*)(b + 296) = 0x1337u;                    // AdapterLuid.LowPart
}
int TRT_GUEST_ABI adp_get_desc(void*, void* desc) {
  if (!desc) return TRT_E_INVALIDARG;
  fill_adapter_desc(desc, 304);
  return 0;
}
int TRT_GUEST_ABI adp_check_iface(void*, const void*, i64* ver) {
  if (ver) *ver = 0x001E000000000000LL;
  return 0;
}
int TRT_GUEST_ABI adp_get_desc1(void*, void* desc1) {
  if (!desc1) return TRT_E_INVALIDARG;
  fill_adapter_desc(desc1, 312);
  return 0;
}
int TRT_GUEST_ABI adp_get_desc23(void*, void* desc2) {
  if (!desc2) return TRT_E_INVALIDARG;
  fill_adapter_desc(desc2, 320);
  return 0;
}
int TRT_GUEST_ABI adp_reg_noop(void*, void*, u32*) { return 0; }
void TRT_GUEST_ABI adp_unreg_noop(void*, u32) {}
int TRT_GUEST_ABI adp_query_vram(void*, u32, u32, void* info) {
  if (!info) return TRT_E_INVALIDARG;
  u64* q = (u64*)info;
  q[0] = 8ULL * 1024 * 1024 * 1024;  // Budget
  q[1] = 0;
  q[2] = 4ULL * 1024 * 1024 * 1024;  // AvailableForReservation
  q[3] = 0;
  return 0;
}
int TRT_GUEST_ABI adp_set_res(void*, u32, u32, u64) { return 0; }

const void* g_offline_adapter_vtbl[19] = {
    (const void*)&adp_qi,          // 0
    (const void*)&adp_addref,      // 1
    (const void*)&adp_release,     // 2
    (const void*)&adp_priv,        // 3
    (const void*)&adp_priv_iface,  // 4
    (const void*)&adp_get_priv,    // 5
    (const void*)&adp_get_parent,  // 6
    (const void*)&adp_enum_outputs,// 7
    (const void*)&adp_get_desc,    // 8
    (const void*)&adp_check_iface, // 9
    (const void*)&adp_get_desc1,   // 10
    (const void*)&adp_get_desc23,  // 11 GetDesc2
    (const void*)&adp_reg_noop,    // 12
    (const void*)&adp_unreg_noop,  // 13
    (const void*)&adp_query_vram,  // 14
    (const void*)&adp_set_res,     // 15
    (const void*)&adp_reg_noop,    // 16
    (const void*)&adp_unreg_noop,  // 17
    (const void*)&adp_get_desc23,  // 18 GetDesc3
};
offline_adapter g_offline_adapter_obj = {g_offline_adapter_vtbl, 1};

// ---------------------------------------------------------------------------
// the fake factory: a COM object implementing IDXGIFactory6 (30 slots)
// ---------------------------------------------------------------------------
struct fake_factory {
  const void** vtbl;
  u32 refcount;
};

const void** g_real_factory_vtbl = nullptr;  // the console's real factory

// forward a call to the console's real factory (same slot, same args)
void* g_real_vtbl_slot(int index) {
  if (index >= 25) {
    if (!g_hooks.factory6) return nullptr;
    const void** vt6 = *(const void***)g_hooks.factory6;
    return vt6 ? (void*)vt6[index] : nullptr;
  }
  if (!g_real_factory_vtbl) return nullptr;
  return (void*)g_real_factory_vtbl[index];
}

bool is_factory_iid(const void* riid) {
  if (guid_eq(riid, 0x00000000u, 0x0000, 0x0000, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46)) return true;
  if (guid_eq(riid, 0xAEC22FB8u, 0x76F3, 0x4639, 0x9B,0xE0,0x28,0xEB,0x43,0xA6,0x7A,0x2E)) return true;
  if (guid_eq(riid, 0x7B7166ECu, 0x21C7, 0x44AE, 0xB2,0x1A,0xC9,0xAE,0x32,0x1A,0xE3,0x69)) return true;
  if (guid_eq(riid, 0x770AAE78u, 0xF26F, 0x4DBA, 0xA8,0x29,0x25,0x3C,0x83,0xD1,0xB3,0x87)) return true;
  if (guid_eq(riid, 0x50C83A1Cu, 0xE072, 0x4C48, 0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0)) return true;
  if (guid_eq(riid, 0x25483823u, 0xCD46, 0x4C7D, 0x86,0xCA,0x47,0xAA,0x95,0xB8,0x37,0xBD)) return true;
  if (guid_eq(riid, 0x1BC6EA02u, 0xEF36, 0x464F, 0xBF,0x0C,0x21,0xCA,0x39,0xE5,0x16,0x8A)) return true;
  if (guid_eq(riid, 0x7632E1F5u, 0xEE65, 0x4DCA, 0x87,0xFD,0x84,0xCD,0x75,0xF8,0x83,0x8D)) return true;
  if (guid_eq(riid, 0xC1B6694Fu, 0xFF09, 0x44A9, 0xB0,0x3C,0x77,0x90,0x0A,0x0A,0x1D,0x17)) return true;
  if (guid_eq(riid, 0xA4966EEDu, 0x76DB, 0x44DA, 0x84,0xC1,0xEE,0x9A,0x7A,0xFB,0x20,0xA8)) return true;
  return false;
}

// --- IUnknown ---
int TRT_GUEST_ABI fake_QueryInterface(void* self, const void* riid, void** ppv) {
  if (!ppv) return TRT_E_NOINTERFACE;
  if (riid && !is_factory_iid(riid)) {
    *ppv = nullptr;
    return TRT_E_NOINTERFACE;
  }
  *ppv = self;
  fake_factory* f = (fake_factory*)self;
  ++f->refcount;
  return 0;
}
u32 TRT_GUEST_ABI fake_AddRef(void* self) {
  fake_factory* f = (fake_factory*)self;
  return ++f->refcount;
}
u32 TRT_GUEST_ABI fake_Release(void* self) {
  fake_factory* f = (fake_factory*)self;
  return f->refcount ? --f->refcount : 0;
}

// --- IDXGIObject: nothing a game needs from us ---
int TRT_GUEST_ABI fake_SetPrivateData(void*, const void*, u32, const void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_SetPrivateDataInterface(void*, const void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_GetPrivateData(void*, const void*, void*, u32*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_GetParent(void*, void**) { return TRT_E_NOTIMPL; }

int create_swapchain_impl(void* device, u32 w, u32 h, u32 fmt, u32 count,
                          u32 swap_effect, void** swapchain, void* hwnd = nullptr) {
  if (!swapchain) return TRT_E_NOINTERFACE;
  *swapchain = nullptr;
  g_sc_presented.store(false);
  if (trt_d3d12_is_queue(device)) {
    if (g_hooks.acquire_d3d12_swapchain) {
      void* sc12 = nullptr;
      const int hr = g_hooks.acquire_d3d12_swapchain(
          device, w, h, fmt, count < 2 ? 2 : count, &sc12);
      if (hr < 0) {
        *swapchain = nullptr;
        return hr;
      }
      g_hooks.d3d12_swapchain = sc12;
    }
    g_sc_is_d3d12 = true;
    g_sc_backbuf_idx = 0;
    g_sc_buf_count = count >= 2 ? count : 2;
    g_sc_width = w ? w : (g_hooks.width ? g_hooks.width : 1280);
    g_sc_height = h ? h : (g_hooks.height ? g_hooks.height : 720);
    *swapchain = &g_swapchain_proxy_obj;
    ++g_swapchain_proxy_obj.refcount;
    return 0;
  }
  if (!g_hooks.swapchain) return TRT_E_NOTIMPL;
  const int hr = configure_guest_swapchain(w, h, fmt, count, swap_effect, hwnd, true);
  if (hr < 0) return hr;
  g_sc_is_d3d12 = false;
  *swapchain = &g_swapchain_proxy_obj;
  ++g_swapchain_proxy_obj.refcount;
  g_d3d11_swapchains.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

// --- IDXGIFactory ---
int TRT_GUEST_ABI fake_EnumAdapters(void* self, u32 index, void** adapter) {
  (void)self;
  void* slot = g_real_vtbl_slot(7);
  if (slot && g_hooks.factory) {
    if (!adapter) return TRT_E_INVALIDARG;
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, void**);
    return ((fn_t)slot)(g_hooks.factory, index, adapter);
  }
  if (!adapter) return TRT_E_INVALIDARG;
  if (index == 0) {
    *adapter = &g_offline_adapter_obj;
    adp_addref(&g_offline_adapter_obj);
    return 0;
  }
  *adapter = nullptr;
  return (int)0x887A0002u;  // DXGI_ERROR_NOT_FOUND
}
int TRT_GUEST_ABI fake_MakeWindowAssociation(void*, void*, u32) { return 0; }
int TRT_GUEST_ABI fake_GetWindowAssociation(void*, void**) { return TRT_E_NOTIMPL; }
// IDXGIFactory::CreateSwapChain(pDevice, pDesc, ppSwapChain)
int TRT_GUEST_ABI fake_CreateSwapChain(void* self, void* device, void* desc,
                                       void** swapchain) {
  (void)self;
  const u32* d = (const u32*)desc;
  const u32 w = d ? d[0] : 0;
  const u32 h = d ? d[1] : 0;
  const u32 fmt = d ? d[4] : 87;
  const u32 cnt = d ? d[10] : 2;
  char line[160];
  snprintf(line, sizeof(line),
           "trt: IDXGIFactory::CreateSwapChain(%ux%u fmt=%u cnt=%u) -> out=%p",
           w, h, fmt, cnt, (void*)swapchain);
  trt_core_log(line);
  return create_swapchain_impl(device, w, h, fmt, cnt, d ? d[15] : 3,
                               swapchain, desktop_swapchain_window(d));
}
int TRT_GUEST_ABI fake_CreateSoftwareAdapter(void*, void*, void**) { return TRT_E_NOTIMPL; }

// --- IDXGIFactory1 ---
int TRT_GUEST_ABI fake_EnumAdapters1(void* self, u32 index, void** adapter) {
  (void)self;
  void* slot = g_real_vtbl_slot(12);
  if (slot && g_hooks.factory) {
    if (!adapter) return TRT_E_INVALIDARG;
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, void**);
    return ((fn_t)slot)(g_hooks.factory, index, adapter);
  }
  if (!adapter) return TRT_E_INVALIDARG;
  if (index == 0) {
    *adapter = &g_offline_adapter_obj;
    adp_addref(&g_offline_adapter_obj);
    return 0;
  }
  *adapter = nullptr;
  return (int)0x887A0002u;
}
int TRT_GUEST_ABI fake_IsCurrent(void*) { return 1; }

// --- IDXGIFactory2 ---
int TRT_GUEST_ABI fake_IsWindowedStereoEnabled(void*) { return 0; }
int TRT_GUEST_ABI fake_CreateSwapChainForHwnd(void* self, void* device, void* hwnd,
                                void* desc, void* fullscreen_desc, void* restrict_info,
                                void** swapchain) {
  (void)self; (void)fullscreen_desc; (void)restrict_info;
  const u32* d = (const u32*)desc;
  const u32 w = d ? d[0] : 0;
  const u32 h = d ? d[1] : 0;
  const u32 fmt = d ? d[2] : 87;
  const u32 cnt = d ? d[7] : 2;
  char line[192];
  snprintf(line, sizeof(line),
           "trt: CreateSwapChainForHwnd(%ux%u fmt=%u cnt=%u) -> out=%p",
           w, h, fmt, cnt, (void*)swapchain);
  trt_core_log(line);
  return create_swapchain_impl(device, w, h, fmt, cnt, d ? d[9] : 3, swapchain, hwnd);
}
int TRT_GUEST_ABI fake_CreateSwapChainForCoreWindow(void* self, void* device,
                                                   void* core_window, const void* desc,
                                                   void* restrict_info, void** swapchain) {
  // IDXGIFactory2 declares fullscreen_desc only for CreateSwapChainForHwnd.
  (void)self; (void)core_window; (void)restrict_info;
  const u32* d = (const u32*)desc;
  return create_swapchain_impl(device, d ? d[0] : 0, d ? d[1] : 0,
                               d ? d[2] : 87, d ? d[7] : 2, d ? d[9] : 3, swapchain);
}
int TRT_GUEST_ABI fake_GetSharedResourceAdapterLuid(void*, void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterStereoStatusWindow(void*, u32, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterStereoStatusEvent(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_UnregisterStereoStatus(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterOcclusionStatusWindow(void*, u32, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterOcclusionStatusEvent(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_UnregisterOcclusionStatus(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_CreateSwapChainForComposition(void* self, void* device, void* desc,
                                        void* restrict_info, void** swapchain) {
  (void)self; (void)restrict_info;
  const u32* d = (const u32*)desc;
  return create_swapchain_impl(device, d ? d[0] : 0, d ? d[1] : 0,
                               d ? d[2] : 87, d ? d[7] : 2, d ? d[9] : 3, swapchain);
}

// --- IDXGIFactory3 / IDXGIFactory4 / IDXGIFactory5 / IDXGIFactory6 ---------
u32 TRT_GUEST_ABI fake_GetCreationFlags(void*) { return 0; }
int TRT_GUEST_ABI fake_EnumAdapterByLuid(void* self, u64 luid, const void* riid, void** out) {
  (void)luid; (void)riid;
  void* slot = g_real_vtbl_slot(26);
  if (slot && g_hooks.factory6) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u64, const void*, void**);
    return ((fn_t)slot)(g_hooks.factory6, luid, riid, out);
  }
  return fake_EnumAdapters1(self, 0, out);
}
int TRT_GUEST_ABI fake_EnumWarpAdapter(void* self, const void* riid, void** out) {
  (void)riid;
  void* slot = g_real_vtbl_slot(27);
  if (slot && g_hooks.factory6) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, const void*, void**);
    return ((fn_t)slot)(g_hooks.factory6, riid, out);
  }
  return fake_EnumAdapters1(self, 0, out);
}
int TRT_GUEST_ABI fake_CheckFeatureSupport(void*, u32 feature, void* data, u32 sz) {
  if (!data || sz < 4) return TRT_E_INVALIDARG;
  if (feature == 0) {
    *(u32*)data = 0;  // DXGI_FEATURE_PRESENT_ALLOW_TEARING: unsupported on CoreWindow
    return 0;
  }
  return TRT_E_INVALIDARG;
}
int TRT_GUEST_ABI fake_EnumAdapterByGpuPreference(void* self, u32 idx, u32 pref,
                                                 const void* riid, void** out) {
  (void)pref; (void)riid;
  void* slot = g_real_vtbl_slot(29);
  if (slot && g_hooks.factory6) {
    using fn_t = int (TRT_GUEST_ABI *)(void*, u32, u32, const void*, void**);
    return ((fn_t)slot)(g_hooks.factory6, idx, pref, riid, out);
  }
  return fake_EnumAdapters1(self, idx, out);
}

// ---- IDXGIFactory7 (SDK dxgi1_6.h; IID a4966eed-76db-44da-84c1-ee9a7afb20a8)
// The QI accepts the Factory7 IID (is_factory_iid), so the vtbl MUST cover
// its slots — a 30-slot table handed to a Factory7 QI is the exact BUG-1
// class (call off the end of the vtbl). Adapters never change under the
// runtime (one RDNA2 SoC), so register=success+cookie, unregister=S_OK.
int TRT_GUEST_ABI fake_RegisterAdaptersChangedEvent(void*, void* hEvent, u32* cookie) {
  static std::atomic<u32> s_cookie{1};
  if (cookie) *cookie = s_cookie.fetch_add(1);
  (void)hEvent;
  return 0;  // S_OK
}
int TRT_GUEST_ABI fake_UnregisterAdaptersChangedEvent(void*, u32) { return 0; }

// IDXGIFactory7 vtable: 32 slots
const void* g_factory_vtbl[32] = {
    (const void*)&fake_QueryInterface,                  // 0
    (const void*)&fake_AddRef,                          // 1
    (const void*)&fake_Release,                         // 2
    (const void*)&fake_SetPrivateData,                  // 3
    (const void*)&fake_SetPrivateDataInterface,         // 4
    (const void*)&fake_GetPrivateData,                  // 5
    (const void*)&fake_GetParent,                       // 6
    (const void*)&fake_EnumAdapters,                    // 7
    (const void*)&fake_MakeWindowAssociation,           // 8
    (const void*)&fake_GetWindowAssociation,            // 9
    (const void*)&fake_CreateSwapChain,                 // 10
    (const void*)&fake_CreateSoftwareAdapter,           // 11
    (const void*)&fake_EnumAdapters1,                   // 12
    (const void*)&fake_IsCurrent,                       // 13
    (const void*)&fake_IsWindowedStereoEnabled,         // 14
    (const void*)&fake_CreateSwapChainForHwnd,          // 15
    (const void*)&fake_CreateSwapChainForCoreWindow,    // 16
    (const void*)&fake_GetSharedResourceAdapterLuid,    // 17
    (const void*)&fake_RegisterStereoStatusWindow,      // 18
    (const void*)&fake_RegisterStereoStatusEvent,       // 19
    (const void*)&fake_UnregisterStereoStatus,          // 20
    (const void*)&fake_RegisterOcclusionStatusWindow,   // 21
    (const void*)&fake_RegisterOcclusionStatusEvent,    // 22
    (const void*)&fake_UnregisterOcclusionStatus,       // 23
    (const void*)&fake_CreateSwapChainForComposition,   // 24
    (const void*)&fake_GetCreationFlags,                // 25
    (const void*)&fake_EnumAdapterByLuid,               // 26
    (const void*)&fake_EnumWarpAdapter,                 // 27
    (const void*)&fake_CheckFeatureSupport,             // 28
    (const void*)&fake_EnumAdapterByGpuPreference,      // 29
    (const void*)&fake_RegisterAdaptersChangedEvent,    // 30 IDXGIFactory7
    (const void*)&fake_UnregisterAdaptersChangedEvent,  // 31 IDXGIFactory7
};
// ABI pins (the TRIANGLE-GATE rule): a vtbl serves exactly the interface
// versions whose slot count it covers. SDK: IDXGIFactory2=25, Factory3=26,
// Factory4=27, Factory5=28 (+CheckFeatureSupport), Factory6=30, Factory7=32.
static_assert(sizeof(g_factory_vtbl) / sizeof(g_factory_vtbl[0]) == 32,
              "factory vtbl must cover IDXGIFactory7 (32 slots)");

// SDK: IDXGIOutput=11, Output1=23, Output2=24, Output3=25, Output4=26,
// Output5=27, Output6=29.
static_assert(sizeof(g_offline_output_vtbl) / sizeof(g_offline_output_vtbl[0]) == 29,
              "output vtbl must cover IDXGIOutput6 (29 slots)");
static_assert(sizeof(g_offline_adapter_vtbl) / sizeof(g_offline_adapter_vtbl[0]) == 19,
              "adapter vtbl must cover IDXGIAdapter4 (19 slots)");

fake_factory g_factory_obj = {g_factory_vtbl, 1};

} // namespace

extern "C" {


// D3D11CreateDevice(adapter, driver, sw, flags, levels, count, sdk, device,
//                    level, context)
extern "C" int TRT_GUEST_ABI trt_D3D11CreateDevice(void* adapter, u32 driver_type, void* software,
                          u32 flags, const void* feature_levels,
                          u32 feature_level_count, u32 sdk_version,
                          void** out_device, u32* out_level, void** out_context) {
  gap_trace_record("D3D11CreateDevice");
  g_d3d11_devices.fetch_add(1, std::memory_order_relaxed);
  // .215 DIAG: Sekiro silent death -- does the game reach D3D11 device creation?
  {
    char line[160];
    snprintf(line, sizeof(line),
             "d3d: D3D11CreateDevice ENTRY driver_type=%u flags=0x%x",
             driver_type, flags);
    trt_core_log(line);
  }
  (void)adapter; (void)driver_type; (void)software; (void)flags;
  (void)sdk_version;
  if (!g_hooks.device || !g_hooks.context) {
    char line[160];
    snprintf(line, sizeof(line),
             "trt: D3D11CreateDevice - no host device (host not ready?)");
    trt_core_log(line);
    return 0x887A0004;  // DXGI_ERROR_UNSUPPORTED
  }
  u32 negotiated = 0;
  const int hr = trt_apply_fl_contract(feature_levels, feature_level_count,
                                       &negotiated);
  if (hr != 0) return hr;
  if (out_device) *out_device = retain_com_object(g_hooks.device);
  if (out_context) *out_context = retain_com_object(g_hooks.context);
  if (out_level) *out_level = negotiated;
  return 0;  // S_OK
}

// D3D11CreateDeviceAndSwapChain(... , out_swapchain, out_device, out_level,
//                                 out_context)
extern "C" int TRT_GUEST_ABI trt_D3D11CreateDeviceAndSwapChain(void* adapter, u32 driver_type,
                                       void* software, u32 flags,
                                       const void* feature_levels,
                                       u32 feature_level_count, u32 sdk_version,
                                       void* swapchain_desc, void** out_swapchain,
                                       void** out_device, u32* out_level,
                                       void** out_context) {
  if (out_swapchain) *out_swapchain = nullptr;
  if (out_device) *out_device = nullptr;
  if (out_context) *out_context = nullptr;
  u32 level = 0;
  const int hr = trt_D3D11CreateDevice(adapter, driver_type, software, flags,
                                       feature_levels, feature_level_count,
                                       sdk_version, nullptr, &level, nullptr);
  if (hr != 0) return hr;

  if (out_swapchain) {
    // DXGI_SWAP_CHAIN_DESC, using the same layout as IDXGIFactory::CreateSwapChain.
    const auto* desc = static_cast<const u32*>(swapchain_desc);
    const int result = create_swapchain_impl(g_hooks.device, desc ? desc[0] : 0,
        desc ? desc[1] : 0, desc ? desc[4] : 0, desc ? desc[10] : 0,
        desc ? desc[15] : 3, out_swapchain, desktop_swapchain_window(desc));
    if (result < 0) return result;
  }

  // Publish references only after both parts have succeeded.
  if (out_device) *out_device = retain_com_object(g_hooks.device);
  if (out_context) *out_context = retain_com_object(g_hooks.context);
  if (out_level) *out_level = level;
  return 0;
}

// CreateDXGIFactory* return an HRESULT (S_OK) and hand the factory out
// through the out parameter - returning the pointer itself is a classic
// mistake and the guest reads it as a failing HRESULT.
extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory(void* riid, void** out_factory) {
  (void)riid;
  if (!out_factory) return TRT_E_NOINTERFACE;
  // the real factory's vtable is needed to forward adapter enumeration
  if (g_hooks.factory) {
    void** vt = (void**)g_hooks.factory;
    g_real_factory_vtbl = (const void**)vt[0];
  }
  *out_factory = (void*)&g_factory_obj;
  return 0;  // S_OK
}

extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory1(void* riid, void** out_factory) {
  return trt_CreateDXGIFactory(riid, out_factory);
}

// CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) — 3 args!
extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory2(u32 flags, void* riid,
                                                   void** out_factory) {
  (void)flags;
  return trt_CreateDXGIFactory(riid, out_factory);
}

extern "C" int TRT_GUEST_ABI trt_DXGIGetDebugInterface1(u32 flags, void* riid,
                                                       void** out_debug) {
  (void)riid;
  if (out_debug) *out_debug = nullptr;
  char line[128];
  snprintf(line, sizeof(line),
           "trt: DXGIGetDebugInterface1(flags=%u) -> E_NOINTERFACE "
           "(debug layer unavailable)",
           flags);
  trt_core_log(line);
  return TRT_E_NOINTERFACE;
}

extern "C" int TRT_GUEST_ABI trt_DXGIDeclareAdapterRemovalSupport(void) {
  return 0;  // S_OK (AppContainer runtime adapter pinning handled by CoreWindow)
}

extern "C" int TRT_GUEST_ABI trt_D3D11On12CreateDevice(
    void* device, u32 flags, const void* levels, u32 level_count,
    void* const* queues, u32 num_queues, u32 node_mask,
    void** out_device, void** out_ctx, u32* out_level) {
  if (g_hooks.d3d11on12_create) {
    return g_hooks.d3d11on12_create(device, flags, levels, level_count, queues,
                                    num_queues, node_mask, out_device, out_ctx,
                                    out_level);
  }
  if (out_device) *out_device = nullptr;
  if (out_ctx) *out_ctx = nullptr;
  trt_core_log("trt: D3D11On12CreateDevice -> E_NOTIMPL "
               "(host D3D11On12 hook not bound)");
  return TRT_E_NOTIMPL;
}

const trt_named_fn k_d3d11_fns[] = {
    {"D3D11CreateDevice", (void*)&trt_D3D11CreateDevice},
    {"D3D11CreateDeviceAndSwapChain",
     (void*)&trt_D3D11CreateDeviceAndSwapChain},
    {"D3D11On12CreateDevice", (void*)&trt_D3D11On12CreateDevice},
};

const trt_named_fn k_dxgi_fns[] = {
    {"CreateDXGIFactory", (void*)&trt_CreateDXGIFactory},
    {"CreateDXGIFactory1", (void*)&trt_CreateDXGIFactory1},
    {"CreateDXGIFactory2", (void*)&trt_CreateDXGIFactory2},
    {"DXGIGetDebugInterface1", (void*)&trt_DXGIGetDebugInterface1},
    {"DXGIDeclareAdapterRemovalSupport",
     (void*)&trt_DXGIDeclareAdapterRemovalSupport},
};


void trt_gpu_set_hooks(const trt_gpu_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}

const trt_gpu_hooks* trt_gpu_hooks_get(void) { return &g_hooks; }

// Preflight answers: what the host can actually present or create right now.
int trt_gpu_host_device_present(void) {
  return (g_hooks.device && g_hooks.context) ? 1 : 0;
}
int trt_gpu_host_d3d12_present(void) { return g_hooks.d3d12_device ? 1 : 0; }
int trt_gpu_host_swapchain_present(void) {
  return (g_hooks.swapchain || g_hooks.d3d12_swapchain) ? 1 : 0;
}

int trt_gpu_d3d11_uses_stable_backbuffer(u32 buffer_count, u32 swap_effect) {
  return buffer_count == 1 || swap_effect == 0 /* DXGI_SWAP_EFFECT_DISCARD */;
}

u32 trt_gpu_count_lit_rgb32(const void* pixels, u32 width, u32 height, u32 row_pitch) {
  if (!pixels || width > row_pitch / 4u) return 0;
  const auto* row = static_cast<const u8*>(pixels);
  u32 lit = 0;
  for (u32 y = 0; y < height; ++y, row += row_pitch) {
    for (u32 x = 0; x < width; ++x) {
      const auto* rgb = row + x * 4u;
      if (rgb[0] > 12 || rgb[1] > 12 || rgb[2] > 12) ++lit;
    }
  }
  return lit;
}

void trt_gpu_reset_render_stats(void) {
  g_d3d11_devices.store(0, std::memory_order_relaxed);
  g_d3d11_swapchains.store(0, std::memory_order_relaxed);
  g_d3d11_buffers.store(0, std::memory_order_relaxed);
  g_d3d11_presents.store(0, std::memory_order_relaxed);
  g_d3d11_visible_presents.store(0, std::memory_order_relaxed);
  g_d3d11_last_present.store(0, std::memory_order_relaxed);
  g_d3d11_swap_effect.store(3, std::memory_order_relaxed);
  g_d3d11_buffer_count.store(2, std::memory_order_relaxed);
  g_d3d11_stable_backbuffer.store(0, std::memory_order_relaxed);
  g_d3d11_guest_format = 0;
  g_d3d11_width.store(0);
  g_d3d11_height.store(0);
  g_d3d11_hwnd = nullptr;
  std::lock_guard<std::mutex> lock(g_resize_stats_mutex);
  g_resize_stats = {};
}

void trt_gpu_get_resize_stats(trt_gpu_resize_stats* stats) {
  if (!stats) return;
  std::lock_guard<std::mutex> lock(g_resize_stats_mutex);
  *stats = g_resize_stats;
}

void trt_gpu_get_render_stats(trt_gpu_render_stats* stats) {
  if (!stats) return;
  stats->d3d11_device_calls = g_d3d11_devices.load(std::memory_order_relaxed);
  stats->d3d11_swapchains = g_d3d11_swapchains.load(std::memory_order_relaxed);
  stats->d3d11_get_buffer_calls = g_d3d11_buffers.load(std::memory_order_relaxed);
  stats->d3d11_present_calls = g_d3d11_presents.load(std::memory_order_relaxed);
  stats->d3d11_successful_presents = g_d3d11_visible_presents.load(std::memory_order_relaxed);
  stats->d3d11_last_present_result = g_d3d11_last_present.load(std::memory_order_relaxed);
  stats->d3d11_swap_effect = g_d3d11_swap_effect.load(std::memory_order_relaxed);
  stats->d3d11_buffer_count = g_d3d11_buffer_count.load(std::memory_order_relaxed);
  stats->d3d11_stable_backbuffer = g_d3d11_stable_backbuffer.load(std::memory_order_relaxed);
  stats->d3d11_width = g_d3d11_width.load(std::memory_order_relaxed);
  stats->d3d11_height = g_d3d11_height.load(std::memory_order_relaxed);
}

int trt_bridge_register_d3d(void) {
  // ---- dwmapi: the DWM compositor surface UnityPlayer asks for.
  // On Xbox UWP DWM is the system compositor; DwmGetWindowAttribute is
  // desktop-only there too, but UnityPlayer boots with the query. v0:
  // a named no-op answer (attribute unknown -> E_INVALIDARG-shaped), the
  // API's own failure value, never silent.
  //
  // ABI WARNING (measured live, Ultra Pool): every fn here is CALLED BY THE
  // GUEST with Win64 convention. A plain SysV lambda reads its args from
  // rdi/rsi (garbage — Win64 passed them in rcx/rdx; the log showed
  // attr=4306832 instead of 0x21) AND clobbers rdi/rsi/r10/r11 on return —
  // nonvolatile registers in Win64. The caller's object pointer became
  // stack garbage and the next frame dereferenced a non-canonical address
  // (SIGSEGV si_code=SI_KERNEL). All bridge-side guest-called fns must be
  // TRT_GUEST_ABI (ms_abi) — the macro exists for exactly this.
  static const trt_named_fn k_dwm_fns[] = {
      {"DwmGetWindowAttribute",
       (void*)+[](u64 hwnd, u32 attr, void* out, u32 cap) TRT_GUEST_ABI -> i32 {
         (void)hwnd; (void)out; (void)cap;
         char line[128];
         snprintf(line, sizeof(line),
                  "dwm: DwmGetWindowAttribute(attr=%u) -> E_INVALIDARG "
                  "(no DWM attribute surface: fail-visible)", attr);
         trt_core_log(line);
         return (int)0x80070057;
       }},
      {"DwmSetWindowAttribute",
       (void*)+[](u64 hwnd, u32 attr, const void* val, u32 cap) TRT_GUEST_ABI -> i32 {
         (void)hwnd; (void)val; (void)cap;
         char line[128];
         snprintf(line, sizeof(line),
                  "dwm: DwmSetWindowAttribute(attr=%u) -> E_INVALIDARG "
                  "(no DWM attribute surface: fail-visible)", attr);
         trt_core_log(line);
         return (int)0x80070057;
       }},
      {"DwmEnableBlurBehindWindow",
       (void*)+[](u64 hwnd, const void* bb) TRT_GUEST_ABI -> i32 {
         (void)hwnd; (void)bb;
         trt_core_log("dwm: DwmEnableBlurBehindWindow -> E_NOTIMPL "
                      "(no DWM blur surface on console UWP; fail-visible)");
         return (int)0x80004001;  // E_NOTIMPL
       }},
      {"DwmFlush",
       (void*)+[](void) TRT_GUEST_ABI -> i32 {
         return 0;  // S_OK (composition frame paced via DXGI Present)
       }},
      {"DwmIsCompositionEnabled",
       (void*)+[](int* enabled) TRT_GUEST_ABI -> i32 {
         if (!enabled) return (int)0x80070057;
         *enabled = 1;
         return 0;
       }},
      {"DwmExtendFrameIntoClientArea",
       (void*)+[](u64 hwnd, const void* margins) TRT_GUEST_ABI -> i32 {
         (void)hwnd; (void)margins;
         return 0;
       }},
      {"DwmDefWindowProc",
       (void*)+[](u64 hwnd, u32 msg, u64 wparam, i64 lparam, i64* res) TRT_GUEST_ABI -> int {
         (void)hwnd; (void)msg; (void)wparam; (void)lparam;
         if (res) *res = 0;
         return 0;
       }},
  };
  trt_module_register("dwmapi.dll", TRT_ROUTE_BUILTIN, k_dwm_fns,
                      (int)(sizeof(k_dwm_fns) / sizeof(k_dwm_fns[0])));
  trt_module_register("d3d11.dll", TRT_ROUTE_BUILTIN, k_d3d11_fns,
                       (int)(sizeof(k_d3d11_fns) / sizeof(k_d3d11_fns[0])));
  trt_module_register("dxgi.dll", TRT_ROUTE_BUILTIN, k_dxgi_fns,
                       (int)(sizeof(k_dxgi_fns) / sizeof(k_dxgi_fns[0])));
  return 0;
}

} // extern "C"
