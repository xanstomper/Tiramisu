// tiramisu/tests/test_console_pack.cpp
// End-to-end verification of the console-missing desktop DLL bridges:
//   1. opengl32.dll (WGL + GL 1.1 fail-visible bridge)
//   2. hid.dll + setupapi.dll (empty HID device list -> ERROR_NO_MORE_ITEMS)
//   3. xinput1_4.dll / xinput1_3.dll (pad state + ordinals #2 / #100)
//   4. WASAPI COM audio stack (CLSID_MMDeviceEnumerator -> IMMDevice ->
//      IAudioClient -> IAudioRenderClient -> PCM submitted to host sink)
//   5. combase.dll (WinRT HSTRING lifecycle) + version.dll + shcore.dll +
//      dwrite.dll + uiautomationcore.dll + winhttp.dll
//   6. user32.dll WM_CREATE CREATESTRUCTW* lpCreateParams + GWL slot separation
//      + MonitorFromWindow / GetMonitorInfoW / EnumDisplaySettingsW
//   7. gdi32.dll CreateDIBSection + GetObjectW(BITMAP) + DeleteObject
#include "../src/trt/trt.h"
#include "../src/trt/pe.h"
#include "../src/trt/input.h"
#include "../src/trt/images.h"
#include "../src/trt/bridge_audio.h"
#include "../src/trt/bridge_console_pack.h"
#include "../src/trt/bridge_ole.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
u32 TRT_STDCALL trt_GetLastError(void);
void* TRT_STDCALL trt_GetProcAddress(void*, const char*);
int trt_bridge_register_user32(void);
int trt_bridge_register_gdi(void);
}

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                               \
  do {                                                 \
    if (cond) {                                        \
      printf("  PASS %s\n", msg);                      \
    } else {                                           \
      printf("  FAIL %s\n", msg);                      \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

int g_pcm_submits = 0;
u32 g_pcm_bytes = 0;
i16 g_first_sample = 0;
int on_pcm_submit(const void* pcm, u32 bytes, u32 rate, u32 channels, u32 bps,
                  void* user) {
  (void)rate; (void)channels; (void)bps; (void)user;
  ++g_pcm_submits;
  g_pcm_bytes += bytes;
  if (pcm && bytes >= 2) g_first_sample = *(const i16*)pcm;
  return 0;
}

void* resolve_builtin(const char* mod, const char* sym) {
  return trt_resolve_module(nullptr, mod, sym, 0);
}

// ---- ThreadPool.RunAsync test helpers (WorkItemHandler duck-type) ----------
// IWorkItemHandler delegate: vtbl {QI, AddRef, Release, Invoke(asyncAction)}.
static u32* g_tp_flag = nullptr;
static const void* g_wh_vtbl[4];
static i32 TRT_GUEST_ABI wh_qi(void*, const void*, void** out) {
  if (out) *out = nullptr;
  return (int)0x80004002u;  // E_NOINTERFACE
}
static u32 TRT_GUEST_ABI wh_addref(void*) { return 2; }
static u32 TRT_GUEST_ABI wh_release(void*) { return 1; }
static i32 TRT_GUEST_ABI wh_invoke(void*, void*) {
  if (g_tp_flag) ++*g_tp_flag;
  return 0;
}
struct wh_init {
  wh_init() {
    g_wh_vtbl[0] = (const void*)&wh_qi;
    g_wh_vtbl[1] = (const void*)&wh_addref;
    g_wh_vtbl[2] = (const void*)&wh_release;
    g_wh_vtbl[3] = (const void*)&wh_invoke;
  }
} g_wh_init;
struct wh_obj {
  const void** vtbl;
};

}  // namespace

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tiramisu console-pack bridge suite\n");

  trt_bridge_boot(nullptr);
  trt_bridge_register_ole();
  trt_bridge_register_user32();
  trt_bridge_register_gdi();
  trt_bridge_register_console_pack();

  // ---- 1. OpenGL32 ----
  {
    CHECK(trt_module_route("opengl32.dll") == TRT_ROUTE_BUILTIN,
          "opengl32.dll routes BUILTIN");
    using wglCreateContext_t = void*(TRT_GUEST_ABI*)(void*);
    using wglMakeCurrent_t = int(TRT_GUEST_ABI*)(void*, void*);
    using wglDeleteContext_t = int(TRT_GUEST_ABI*)(void*);
    using wglGetProcAddress_t = void*(TRT_GUEST_ABI*)(const char*);
    auto fn_ctx = (wglCreateContext_t)resolve_builtin("opengl32.dll", "wglCreateContext");
    auto fn_cur = (wglMakeCurrent_t)resolve_builtin("opengl32.dll", "wglMakeCurrent");
    auto fn_del = (wglDeleteContext_t)resolve_builtin("opengl32.dll", "wglDeleteContext");
    auto fn_gpa = (wglGetProcAddress_t)resolve_builtin("opengl32.dll", "wglGetProcAddress");
    void* ctx = fn_ctx ? fn_ctx((void*)1) : nullptr;
    CHECK(ctx != nullptr, "wglCreateContext returns live D3D11-backed GL context");
    CHECK(fn_cur && fn_cur((void*)1, ctx) == 1, "wglMakeCurrent succeeds");
    CHECK(fn_gpa && fn_gpa("wglCreateContextAttribsARB") != nullptr,
          "wglGetProcAddress resolves WGL_ARB_create_context");
    CHECK(fn_gpa && fn_gpa("glCreateShader") != nullptr,
          "wglGetProcAddress resolves core GL shader procs");
    CHECK(fn_cur((void*)1, nullptr) == 1 && fn_del && fn_del(ctx) == 1,
          "wglDeleteContext cleans up");
  }

  // ---- 2. HID + SetupAPI ----
  {
    CHECK(trt_module_route("hid.dll") == TRT_ROUTE_BUILTIN,
          "hid.dll routes BUILTIN");
    CHECK(trt_module_route("setupapi.dll") == TRT_ROUTE_BUILTIN,
          "setupapi.dll routes BUILTIN");
    using HidD_GetHidGuid_t = void(TRT_GUEST_ABI*)(void*);
    using SetupDiGetClassDevsW_t = void*(TRT_GUEST_ABI*)(const void*, const u16*, void*, u32);
    using SetupDiEnumDeviceInterfaces_t = int(TRT_GUEST_ABI*)(void*, void*, const void*, u32, void*);
    using SetupDiDestroyDeviceInfoList_t = int(TRT_GUEST_ABI*)(void*);

    auto fn_guid = (HidD_GetHidGuid_t)resolve_builtin("hid.dll", "HidD_GetHidGuid");
    auto fn_devs = (SetupDiGetClassDevsW_t)resolve_builtin("setupapi.dll", "SetupDiGetClassDevsW");
    auto fn_enum = (SetupDiEnumDeviceInterfaces_t)resolve_builtin("setupapi.dll", "SetupDiEnumDeviceInterfaces");
    auto fn_free = (SetupDiDestroyDeviceInfoList_t)resolve_builtin("setupapi.dll", "SetupDiDestroyDeviceInfoList");

    u8 guid[16] = {};
    fn_guid(guid);
    CHECK(guid[0] == 0xB2 && guid[1] == 0x55, "HidD_GetHidGuid writes GUID_DEVINTERFACE_HID");
    void* hdev = fn_devs(guid, nullptr, nullptr, 0x12);
    CHECK(hdev != nullptr && hdev != (void*)(intptr_t)-1,
          "SetupDiGetClassDevsW returns valid HDEVINFO record");
    u8 ifdata[32] = {};
    int ok = fn_enum(hdev, nullptr, guid, 0, ifdata);
    CHECK(ok == 0 && trt_GetLastError() == 259u,
          "SetupDiEnumDeviceInterfaces(0) -> FALSE + ERROR_NO_MORE_ITEMS (259)");
    CHECK(fn_free(hdev) == 1, "SetupDiDestroyDeviceInfoList frees HDEVINFO record");
  }

  // ---- 3. XInput ----
  {
    CHECK(trt_module_route("xinput1_4.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("xinput1_3.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("xinput9_1_0.dll") == TRT_ROUTE_BUILTIN,
          "xinput1_4 / xinput1_3 / xinput9_1_0 route BUILTIN");
    struct xinput_state {
      u32 packet;
      u16 buttons;
      u8 lt, rt;
      i16 lx, ly, rx, ry;
    };
    using XInputGetState_t = u32(TRT_GUEST_ABI*)(u32, xinput_state*);
    auto fn_name = (XInputGetState_t)resolve_builtin("xinput1_4.dll", "XInputGetState");
    auto fn_ord2 = (XInputGetState_t)resolve_builtin("xinput1_4.dll", "#2");
    auto fn_ord100 = (XInputGetState_t)resolve_builtin("xinput1_3.dll", "#100");
    trt_input_pad_state(12345, -23456, 5432, 6543, 0x1000 /*A*/, 128, 250);
    xinput_state st{};
    CHECK(fn_name && fn_name(0, &st) == 0 && st.buttons == 0x1000 &&
              st.lx == 12345 && st.rt == 250,
          "XInputGetState(0) reads live pad state");
    CHECK(fn_ord2 == fn_name && fn_ord100 != nullptr,
          "XInput ordinals #2 and #100 resolve");
    void* module = trt_images_synthetic_handle("xinput1_3.dll");
    auto dynamic_state = reinterpret_cast<XInputGetState_t>(
        trt_GetProcAddress(module, reinterpret_cast<const char*>(uintptr_t{100})));
    xinput_state dynamic_sample{};
    CHECK(dynamic_state == fn_ord100 && dynamic_state &&
              dynamic_state(0, &dynamic_sample) == 0 &&
              dynamic_sample.buttons == 0x1000 && dynamic_sample.lx == 12345,
          "GetProcAddress ordinal #100 reaches live controller input");
    CHECK(fn_name(1, &st) == 1167u,
          "XInputGetState(1) returns ERROR_DEVICE_NOT_CONNECTED (1167)");
  }

  // ---- 4. WASAPI COM audio stack -> host XAudio2 PCM sink ----
  {
    trt_audio_set_hooks(&on_pcm_submit, nullptr);

    static const u8 CLSID_MMDeviceEnumerator[16] = {
        0x95, 0x03, 0xDE, 0xBC, 0x2F, 0xE5, 0x7C, 0x46,
        0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E};
    static const u8 IID_IMMDeviceEnumerator[16] = {
        0xD2, 0x64, 0x56, 0xA9, 0x14, 0x96, 0x35, 0x4F,
        0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6};
    static const u8 IID_IAudioClient[16] = {
        0x4C, 0xAD, 0xB9, 0x1C, 0xFA, 0xDB, 0x32, 0x4C,
        0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2};
    static const u8 IID_IAudioRenderClient[16] = {
        0xFC, 0xAC, 0x94, 0xF2, 0x46, 0x31, 0x83, 0x44,
        0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2};

    void* enumerator = nullptr;
    int hr = ole_CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, 1,
                                  IID_IMMDeviceEnumerator, &enumerator);
    CHECK(hr == 0 && enumerator != nullptr,
          "CoCreateInstance(CLSID_MMDeviceEnumerator) succeeds");

    // IMMDeviceEnumerator::GetDefaultAudioEndpoint is slot 4
    using GetDefaultEndpoint_t = i32(TRT_GUEST_ABI*)(void*, i32, i32, void**);
    void** enum_vtbl = *(void***)enumerator;
    void* device = nullptr;
    hr = ((GetDefaultEndpoint_t)enum_vtbl[4])(enumerator, 0 /*eRender*/, 0, &device);
    CHECK(hr == 0 && device != nullptr,
          "IMMDeviceEnumerator::GetDefaultAudioEndpoint returns IMMDevice");

    // IMMDevice::Activate is slot 3
    using Activate_t = i32(TRT_GUEST_ABI*)(void*, const void*, u32, void*, void**);
    void** dev_vtbl = *(void***)device;
    void* client = nullptr;
    hr = ((Activate_t)dev_vtbl[3])(device, IID_IAudioClient, 1, nullptr, &client);
    CHECK(hr == 0 && client != nullptr,
          "IMMDevice::Activate(IID_IAudioClient) returns IAudioClient");

    // IAudioClient::GetMixFormat (slot 8), Initialize (slot 3), Start (slot 10), GetService (slot 14)
    void** cli_vtbl = *(void***)client;
    using GetMixFormat_t = i32(TRT_GUEST_ABI*)(void*, void**);
    using Initialize_t = i32(TRT_GUEST_ABI*)(void*, i32, u32, i64, i64, const void*, const void*);
    using Start_t = i32(TRT_GUEST_ABI*)(void*);
    using GetService_t = i32(TRT_GUEST_ABI*)(void*, const void*, void**);

    void* mix_fmt = nullptr;
    hr = ((GetMixFormat_t)cli_vtbl[8])(client, &mix_fmt);
    CHECK(hr == 0 && mix_fmt != nullptr && *(u16*)mix_fmt == 0xFFFEu,
          "IAudioClient::GetMixFormat returns WAVEFORMATEXTENSIBLE IEEE_FLOAT");
    hr = ((Initialize_t)cli_vtbl[3])(client, 0, 0, 1000000, 0, mix_fmt, nullptr);
    CHECK(hr == 0, "IAudioClient::Initialize succeeds");
    ole_CoTaskMemFree(mix_fmt);

    void* render = nullptr;
    hr = ((GetService_t)cli_vtbl[14])(client, IID_IAudioRenderClient, &render);
    CHECK(hr == 0 && render != nullptr,
          "IAudioClient::GetService(IID_IAudioRenderClient) succeeds");

    // IAudioRenderClient::GetBuffer (slot 3), ReleaseBuffer (slot 4)
    void** ren_vtbl = *(void***)render;
    using GetBuffer_t = i32(TRT_GUEST_ABI*)(void*, u32, u8**);
    using ReleaseBuffer_t = i32(TRT_GUEST_ABI*)(void*, u32, u32);
    u8* audio_buf = nullptr;
    hr = ((GetBuffer_t)ren_vtbl[3])(render, 64, &audio_buf);
    CHECK(hr == 0 && audio_buf != nullptr,
          "IAudioRenderClient::GetBuffer(64) returns float buffer");
    float* flt = (float*)audio_buf;
    flt[0] = 0.5f;   // +16383 in s16
    flt[1] = -0.5f;
    ((Start_t)cli_vtbl[10])(client);
    hr = ((ReleaseBuffer_t)ren_vtbl[4])(render, 64, 0);
    CHECK(hr == 0 && g_pcm_submits == 1 && g_pcm_bytes == 64 * 4 &&
              g_first_sample > 16000 && g_first_sample < 16500,
          "IAudioRenderClient::ReleaseBuffer converts float32 -> s16 PCM and submits to host");
  }

  // ---- 5. WinRT HSTRING + version.dll + shcore.dll ----
  {
    CHECK(trt_module_route("combase.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("version.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("shcore.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("dwrite.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("uiautomationcore.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_route("winhttp.dll") == TRT_ROUTE_BUILTIN,
          "combase / version / shcore / dwrite / uiautomationcore / winhttp route BUILTIN");

    using WindowsCreateString_t = i32(TRT_GUEST_ABI*)(const u16*, u32, void**);
    using WindowsGetStringRawBuffer_t = const u16*(TRT_GUEST_ABI*)(void*, u32*);
    using WindowsDeleteString_t = i32(TRT_GUEST_ABI*)(void*);
    auto fn_create = (WindowsCreateString_t)resolve_builtin("combase.dll", "WindowsCreateString");
    auto fn_raw = (WindowsGetStringRawBuffer_t)resolve_builtin("combase.dll", "WindowsGetStringRawBuffer");
    auto fn_del = (WindowsDeleteString_t)resolve_builtin("combase.dll", "WindowsDeleteString");

    const u16 k_hello[] = {'X', 'b', 'o', 'x', 0};
    void* hstr = nullptr;
    u32 hlen = 0;
    CHECK(fn_create(k_hello, 4, &hstr) == 0 && hstr != nullptr,
          "WindowsCreateString allocates HSTRING");
    const u16* buf = fn_raw(hstr, &hlen);
    CHECK(hlen == 4 && buf[0] == 'X' && buf[3] == 'x',
          "WindowsGetStringRawBuffer round-trips length and UTF-16 buffer");
    CHECK(fn_del(hstr) == 0, "WindowsDeleteString frees HSTRING");

    using GetFileVersionInfoSizeW_t = u32(TRT_GUEST_ABI*)(const u16*, u32*);
    using GetFileVersionInfoW_t = int(TRT_GUEST_ABI*)(const u16*, u32, u32, void*);
    using VerQueryValueW_t = int(TRT_GUEST_ABI*)(const void*, const u16*, void**, u32*);
    auto fn_vsz = (GetFileVersionInfoSizeW_t)resolve_builtin("version.dll", "GetFileVersionInfoSizeW");
    auto fn_vget = (GetFileVersionInfoW_t)resolve_builtin("version.dll", "GetFileVersionInfoW");
    auto fn_vq = (VerQueryValueW_t)resolve_builtin("version.dll", "VerQueryValueW");
    u32 dummy = 0;
    u32 vsz = fn_vsz(k_hello, &dummy);
    u8 vbuf[256] = {};
    void* ffi = nullptr;
    u32 ffilen = 0;
    const u16 k_root[] = {'\\', 0};
    CHECK(vsz > 0 && fn_vget(k_hello, 0, vsz, vbuf) == 1 &&
              fn_vq(vbuf, k_root, &ffi, &ffilen) == 1 &&
              *(u32*)ffi == 0xFEEF04BDu,
          "version.dll GetFileVersionInfoW + VerQueryValueW returns VS_FIXEDFILEINFO (0xFEEF04BD)");
  }

  // ---- 6. user32 WM_CREATE CREATESTRUCTW* & GWL slot separation ----
  {
    using CreateWindowExW_t = void*(TRT_GUEST_ABI*)(u32, const u16*, const u16*,
                                                    u32, int, int, int, int,
                                                    void*, void*, void*, void*);
    using SetWindowLongPtrW_t = i64(TRT_GUEST_ABI*)(u64, int, i64);
    using GetWindowLongPtrW_t = i64(TRT_GUEST_ABI*)(u64, int);
    using GetMonitorInfoW_t = int(TRT_GUEST_ABI*)(void*, void*);

    auto fn_cw = (CreateWindowExW_t)resolve_builtin("user32.dll", "CreateWindowExW");
    auto fn_swl = (SetWindowLongPtrW_t)resolve_builtin("user32.dll", "SetWindowLongPtrW");
    auto fn_gwl = (GetWindowLongPtrW_t)resolve_builtin("user32.dll", "GetWindowLongPtrW");
    auto fn_mi = (GetMonitorInfoW_t)resolve_builtin("user32.dll", "GetMonitorInfoW");

    { trt_msg m{}; while (trt_msg_peek(&m, true)) {} }
    u64 magic_param = 0xCAFEBABE12345678ULL;
    static const u16 k_cls[] = {'T', 'e', 's', 't', 'C', 'l', 's', 0};
    static const u16 k_title[] = {'T', 'e', 's', 't', 'T', 'i', 't', 'l', 'e', 0};
    void* hwnd = fn_cw(0, k_cls, k_title, 0x10CF0000u, 0, 0, 1280, 720,
                       nullptr, nullptr, nullptr, &magic_param);
    trt_msg wm_create{};
    bool got_msg = trt_msg_peek(&wm_create, true);
    CHECK(got_msg && wm_create.message == 0x0001 && wm_create.lparam != 0 &&
              *(void**)(uintptr_t)wm_create.lparam == &magic_param,
          "CreateWindowExW posts WM_CREATE with CREATESTRUCTW* whose lpCreateParams == param");
    { trt_msg m{}; while (trt_msg_peek(&m, true)) {} }

    fn_swl((u64)(uintptr_t)hwnd, -21 /*GWLP_USERDATA*/, (i64)0x1111222233334444LL);
    fn_swl((u64)(uintptr_t)hwnd, -16 /*GWL_STYLE*/, (i64)0x99998888LL);
    fn_swl((u64)(uintptr_t)hwnd, -20 /*GWL_EXSTYLE*/, (i64)0x77776666LL);
    CHECK(fn_gwl((u64)(uintptr_t)hwnd, -21) == (i64)0x1111222233334444LL &&
              fn_gwl((u64)(uintptr_t)hwnd, -16) == (i64)0x99998888LL &&
              fn_gwl((u64)(uintptr_t)hwnd, -20) == (i64)0x77776666LL,
          "GWLP_USERDATA (-21), GWL_STYLE (-16), and GWL_EXSTYLE (-20) occupy distinct slots");

    u32 mi[10] = {40};
    CHECK(fn_mi((void*)0x50001, mi) == 1 && mi[3] == 1920 && mi[4] == 1080,
          "GetMonitorInfoW reports 1920x1080 primary monitor");
  }

  // ---- 7. gdi32 CreateDIBSection + GetObjectW(BITMAP) ----
  {
    using CreateDIBSection_t = void*(TRT_GUEST_ABI*)(void*, const void*, u32, void**, void*, u32);
    using GetObjectW_t = int(TRT_GUEST_ABI*)(void*, int, void*);
    using DeleteObject_t = int(TRT_GUEST_ABI*)(void*);
    auto fn_dib = (CreateDIBSection_t)resolve_builtin("gdi32.dll", "CreateDIBSection");
    auto fn_obj = (GetObjectW_t)resolve_builtin("gdi32.dll", "GetObjectW");
    auto fn_del = (DeleteObject_t)resolve_builtin("gdi32.dll", "DeleteObject");

    u8 bmi[40] = {};
    *(u32*)(bmi + 0) = 40;
    *(i32*)(bmi + 4) = 32;
    *(i32*)(bmi + 8) = 16;
    *(u16*)(bmi + 12) = 1;
    *(u16*)(bmi + 14) = 32;
    void* bits = nullptr;
    void* hbm = fn_dib(nullptr, bmi, 0, &bits, nullptr, 0);
    u8 bm_struct[32] = {};
    int got = fn_obj(hbm, 32, bm_struct);
    CHECK(hbm != nullptr && bits != nullptr && got == 32 &&
              *(i32*)(bm_struct + 4) == 32 && *(i32*)(bm_struct + 8) == 16 &&
              *(void**)(bm_struct + 24) == bits,
          "CreateDIBSection allocates pixel buffer and GetObjectW returns 32-byte BITMAP");
    CHECK(fn_del(hbm) == 1, "DeleteObject frees DIB section");
  }

  // ---- 8. gameinput.dll IGameInput -> XInput + KBM ----
  {
    CHECK(trt_module_route("gameinput.dll") == TRT_ROUTE_BUILTIN &&
              trt_module_query("gameinput.dll", "GameInputCreate") == 1,
          "gameinput.dll registered BUILTIN with GameInputCreate");
    using GICreate_t = i32(TRT_GUEST_ABI*)(void**);
    auto fn_gi = (GICreate_t)resolve_builtin("gameinput.dll", "GameInputCreate");
    void* gi = nullptr;
    void* reading = nullptr;
    struct {
      u32 buttons;
      float lt, rt, lx, ly, rx, ry;
    } gst{};
    trt_input_pad_state_n(0, true, 16384, -8192, 0, 0, 0x1000u /*A*/, 255, 0);
    const i32 chr = fn_gi ? fn_gi(&gi) : -1;
    bool got_gp = false;
    if (chr == 0 && gi) {
      void** gvt = *(void***)gi;
      const i32 rhr = ((i32(TRT_GUEST_ABI*)(void*, u32, void*, void**))gvt[4])(
          gi, 0x00080000u /*GameInputKindGamepad*/, nullptr, &reading);
      if (rhr == 0 && reading) {
        void** rvt = *(void***)reading;
        got_gp = ((u8(TRT_GUEST_ABI*)(void*, void*))rvt[22])(reading, &gst) != 0;
        ((u32(TRT_GUEST_ABI*)(void*))rvt[2])(reading);
      }
      ((u32(TRT_GUEST_ABI*)(void*))gvt[2])(gi);
    }
    CHECK(chr == 0 && got_gp && gst.lt > 0.95f && gst.lx > 0.4f && gst.buttons != 0,
          "gameinput.dll GetCurrentReading + GetGamepadState returns live pad input");
  }

  // ---- 9. Offline WinRT classes (Gamepad, DispatcherQueueController, DisplayInfo, AppData) ----
  {
    using WinCrtStr_t = i32(TRT_GUEST_ABI*)(const u16*, u32, void**);
    using WinRawStr_t = const u16*(TRT_GUEST_ABI*)(void*, u32*);
    using WinDelStr_t = i32(TRT_GUEST_ABI*)(void*);
    using RoGetFac_t = i32(TRT_GUEST_ABI*)(void*, const void*, void**);

    auto pCrtS = (WinCrtStr_t)resolve_builtin("combase.dll", "WindowsCreateString");
    auto pRawS = (WinRawStr_t)resolve_builtin("combase.dll", "WindowsGetStringRawBuffer");
    auto pDelS = (WinDelStr_t)resolve_builtin("combase.dll", "WindowsDeleteString");
    auto pRoFac = (RoGetFac_t)resolve_builtin("combase.dll", "RoGetActivationFactory");

    auto make_hs = [&](const char* ascii) -> void* {
      u16 buf[128] = {};
      u32 n = 0;
      while (ascii[n] && n < 127) { buf[n] = (u16)(u8)ascii[n]; ++n; }
      void* hs = nullptr;
      if (pCrtS) pCrtS(buf, n, &hs);
      return hs;
    };

    // DispatcherQueueController
    void* hs_dqc = make_hs("Windows.System.DispatcherQueueController");
    void* dqc_stat = nullptr;
    void* dqc = nullptr;
    void* dq = nullptr;
    u8 has_access = 0;
    if (pRoFac && hs_dqc && pRoFac(hs_dqc, nullptr, &dqc_stat) == 0 && dqc_stat) {
      void** svt = *(void***)dqc_stat;
      if (((i32(TRT_GUEST_ABI*)(void*, void**))svt[6])(dqc_stat, &dqc) == 0 && dqc) {
        void** cvt = *(void***)dqc;
        if (((i32(TRT_GUEST_ABI*)(void*, void**))cvt[6])(dqc, &dq) == 0 && dq) {
          struct { u32 d1; u16 d2, d3; u8 d4[8]; } static constexpr iid_dq2 = {
              0xc822c647, 0x30ef, 0x506e, {0xbd,0x1e,0xa6,0x47,0xae,0x66,0x75,0xff}};
          void** qvt = *(void***)dq;
          void* dq2 = nullptr;
          if (((i32(TRT_GUEST_ABI*)(void*, const void*, void**))qvt[0])(dq, &iid_dq2, &dq2) == 0 && dq2) {
            void** q2vt = *(void***)dq2;
            ((i32(TRT_GUEST_ABI*)(void*, u8*))q2vt[6])(dq2, &has_access);
          }
        }
      }
    }
    if (hs_dqc && pDelS) pDelS(hs_dqc);
    CHECK(dqc != nullptr && dq != nullptr && has_access == 1,
          "WinRT DispatcherQueueController creates DispatcherQueue with HasThreadAccess=1");

    // Windows.System.Threading.ThreadPool (IThreadPoolStatics)
    void* hs_tp = make_hs("Windows.System.Threading.ThreadPool");
    void* tp_stat = nullptr;
    void* tp_action = nullptr;
    u32 tp_status = 0xDEADu;
    u32 tp_ran = 0;
    if (pRoFac && hs_tp && pRoFac(hs_tp, nullptr, &tp_stat) == 0 && tp_stat) {
      void** tvt = *(void***)tp_stat;
      g_tp_flag = &tp_ran;
      wh_obj handler{g_wh_vtbl};
      ((i32(TRT_GUEST_ABI*)(void*, void*, void**))tvt[6])(tp_stat, &handler,
                                                          &tp_action);
      if (tp_action) {
        void** avt = *(void***)tp_action;
        ((i32(TRT_GUEST_ABI*)(void*, u32*))avt[7])(tp_action, &tp_status);
      }
      // The work item runs on a detached host thread — bounded wait.
      for (int i = 0; i < 2000 && !tp_ran; ++i) {
        struct timespec ts{0, 1000000};  // 1ms
        nanosleep(&ts, nullptr);
      }
    }
    if (hs_tp && pDelS) pDelS(hs_tp);
    CHECK(tp_stat != nullptr && tp_action != nullptr && tp_status == 1u &&
              tp_ran == 1u,
          "WinRT ThreadPool.RunAsync -> completed IAsyncAction + handler invoked off-thread");

    // Windows.Gaming.Input.Gamepad
    trt_input_pad_state_n(0, true, 32767, 0, 0, 0, 0x1000u /*A*/, 0, 255);
    void* hs_gp = make_hs("Windows.Gaming.Input.Gamepad");
    void* gp_stat = nullptr;
    void* gp_vec = nullptr;
    void* gp0 = nullptr;
    u32 gp_cnt = 0;
    struct {
      u64 ts;
      u32 btns;
      u32 pad_;
      double lt, rt, lx, ly, rx, ry;
    } gr{};
    struct { double lm, rm, lt, rt; } gv{0.5, 0.25, 0.0, 0.0};
    if (pRoFac && hs_gp && pRoFac(hs_gp, nullptr, &gp_stat) == 0 && gp_stat) {
      void** svt = *(void***)gp_stat;
      if (((i32(TRT_GUEST_ABI*)(void*, void**))svt[10])(gp_stat, &gp_vec) == 0 && gp_vec) {
        void** vvt = *(void***)gp_vec;
        ((i32(TRT_GUEST_ABI*)(void*, u32*))vvt[7])(gp_vec, &gp_cnt);
        ((i32(TRT_GUEST_ABI*)(void*, u32, void**))vvt[6])(gp_vec, 0, &gp0);
      }
      if (gp0) {
        void** gvt = *(void***)gp0;
        ((i32(TRT_GUEST_ABI*)(void*, void*))gvt[8])(gp0, &gr);
        ((i32(TRT_GUEST_ABI*)(void*, decltype(gv)))gvt[7])(gp0, gv);
      }
    }
    u16 vl = 0, vr = 0;
    trt_pad_get_vibration(0, &vl, &vr);
    if (hs_gp && pDelS) pDelS(hs_gp);
    CHECK(gp_cnt >= 1 && gp0 != nullptr && (gr.btns & 0x4u) != 0 &&
              gr.rt > 0.95 && gr.lx > 0.9 && vl > 30000 && vr > 15000,
          "WinRT Windows.Gaming.Input.Gamepad reads state and drives vibration");

    // Windows.Graphics.Display.DisplayInformation + ApplicationData
    void* hs_disp = make_hs("Windows.Graphics.Display.DisplayInformation");
    void* disp_stat = nullptr;
    void* disp = nullptr;
    i32 orient = 0, scale = 0;
    float dpi = 0.0f;
    if (pRoFac && hs_disp && pRoFac(hs_disp, nullptr, &disp_stat) == 0 && disp_stat) {
      void** svt = *(void***)disp_stat;
      if (((i32(TRT_GUEST_ABI*)(void*, void**))svt[6])(disp_stat, &disp) == 0 && disp) {
        void** dvt = *(void***)disp;
        ((i32(TRT_GUEST_ABI*)(void*, i32*))dvt[6])(disp, &orient);
        ((i32(TRT_GUEST_ABI*)(void*, i32*))dvt[10])(disp, &scale);
        ((i32(TRT_GUEST_ABI*)(void*, float*))dvt[11])(disp, &dpi);
      }
    }
    if (hs_disp && pDelS) pDelS(hs_disp);

    void* hs_ad = make_hs("Windows.Storage.ApplicationData");
    void* ad_stat = nullptr;
    void* ad = nullptr;
    void* lf = nullptr;
    void* si = nullptr;
    void* path_hs = nullptr;
    u32 path_len = 0;
    const u16* path_w = nullptr;
    if (pRoFac && hs_ad && pRoFac(hs_ad, nullptr, &ad_stat) == 0 && ad_stat) {
      void** svt = *(void***)ad_stat;
      if (((i32(TRT_GUEST_ABI*)(void*, void**))svt[6])(ad_stat, &ad) == 0 && ad) {
        void** avt = *(void***)ad;
        if (((i32(TRT_GUEST_ABI*)(void*, void**))avt[12])(ad, &lf) == 0 && lf) {
          struct { u32 d1; u16 d2, d3; u8 d4[8]; } static constexpr iid_si = {
              0x4207a996, 0xca2f, 0x42f7, {0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30}};
          void** fvt = *(void***)lf;
          if (((i32(TRT_GUEST_ABI*)(void*, const void*, void**))fvt[0])(lf, &iid_si, &si) == 0 && si) {
            void** ivt = *(void***)si;
            ((i32(TRT_GUEST_ABI*)(void*, void**))ivt[12])(si, &path_hs);
          }
        }
      }
    }
    if (path_hs && pRawS) path_w = pRawS(path_hs, &path_len);
    const bool path_ok = path_w != nullptr && path_len > 3 && path_w[0] == 'C' && path_w[1] == ':';
    if (path_hs && pDelS) pDelS(path_hs);
    if (hs_ad && pDelS) pDelS(hs_ad);
    CHECK(orient == 1 && scale == 100 && dpi > 90.0f && path_ok,
          "WinRT DisplayInformation (DPI=96) and ApplicationData LocalFolder Path succeed");
  }

  printf(g_failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         g_failures);
  return g_failures ? 1 : 0;
}
