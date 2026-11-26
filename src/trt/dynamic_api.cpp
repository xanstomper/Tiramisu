// tiramisu/src/trt/dynamic_api.cpp
// Universal Dynamic API Synthesizer & Intelligent Blanket Thunking.
#include "dynamic_api.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

std::mutex g_synth_mu;
std::unordered_set<std::string> g_synthesized_symbols;

// Universal COM Object Implementation
// Vtable containing 256 slots following Microsoft x64 calling convention (TRT_GUEST_ABI / ms_abi).
// QueryInterface returns the universal COM object itself with S_OK.
// AddRef returns 2, Release returns 1.
// All interface methods return S_OK (0).

static int TRT_GUEST_ABI universal_com_QueryInterface(void* self, const void* riid, void** ppv) {
  (void)riid;
  if (ppv) {
    *ppv = self;
    return 0; // S_OK
  }
  return (int)0x80004003; // E_POINTER
}

static uint32_t TRT_GUEST_ABI universal_com_AddRef(void* self) {
  (void)self;
  return 2;
}

static uint32_t TRT_GUEST_ABI universal_com_Release(void* self) {
  (void)self;
  return 1;
}

static int TRT_GUEST_ABI universal_com_stub_method(void* self, void* a1, void* a2, void* a3) {
  (void)self; (void)a1; (void)a2; (void)a3;
  return 0; // S_OK
}

struct UniversalComObject {
  const void* const* lpVtbl;
};

static const void* g_universal_com_vtable[256] = { nullptr };
static UniversalComObject g_universal_com_instance = { nullptr };
static std::once_flag g_com_init_flag;

void init_universal_com() {
  g_universal_com_vtable[0] = (const void*)&universal_com_QueryInterface;
  g_universal_com_vtable[1] = (const void*)&universal_com_AddRef;
  g_universal_com_vtable[2] = (const void*)&universal_com_Release;
  for (int i = 3; i < 256; ++i) {
    g_universal_com_vtable[i] = (const void*)&universal_com_stub_method;
  }
  g_universal_com_instance.lpVtbl = g_universal_com_vtable;
}

} // namespace

extern "C" {

void* trt_dynamic_universal_com_object(void) {
  std::call_once(g_com_init_flag, init_universal_com);
  return &g_universal_com_instance;
}

uint64_t trt_dynamic_deduce_return_val(const char* module, const char* symbol) {
  (void)module;
  if (!symbol) return 0;

  // Ordinal fallback
  if (symbol[0] == '#') return 0;

  // Hash symbol name for stable, non-zero pseudo-handles
  uint32_t h = 0x55555555;
  for (const char* p = symbol; *p; ++p) {
    h = (h * 33) ^ (unsigned char)(*p);
  }
  uint64_t pseudo_handle = 0x77000000ULL | (uint64_t)((h & 0x00FFFFFE) + 2);

  // 1. Steamworks API
  if (strncmp(symbol, "SteamAPI_Init", 13) == 0 ||
      strncmp(symbol, "SteamAPI_IsSteamRunning", 23) == 0) {
    return 1; // true
  }
  if (strncmp(symbol, "SteamAPI_RestartAppIfNecessary", 30) == 0) {
    return 0; // false: no need to restart through Steam client
  }
  if (strncmp(symbol, "SteamInternal_CreateInterface", 29) == 0 ||
      strncmp(symbol, "SteamInternal_FindOrCreateUserInterface", 39) == 0 ||
      strncmp(symbol, "SteamInternal_FindOrCreateGameServerInterface", 45) == 0 ||
      strcmp(symbol, "SteamAppList") == 0 ||
      strcmp(symbol, "SteamApps") == 0 ||
      strcmp(symbol, "SteamFriends") == 0 ||
      strcmp(symbol, "SteamUtils") == 0 ||
      strcmp(symbol, "SteamUser") == 0 ||
      strcmp(symbol, "SteamUserStats") == 0 ||
      strcmp(symbol, "SteamRemoteStorage") == 0 ||
      strcmp(symbol, "SteamScreenshots") == 0 ||
      strcmp(symbol, "SteamMatchmaking") == 0 ||
      strcmp(symbol, "SteamMatchmakingServers") == 0 ||
      strcmp(symbol, "SteamNetworking") == 0 ||
      strcmp(symbol, "SteamHTTP") == 0 ||
      strcmp(symbol, "SteamController") == 0 ||
      strcmp(symbol, "SteamUGC") == 0 ||
      strcmp(symbol, "SteamInput") == 0) {
    return (uint64_t)(uintptr_t)trt_dynamic_universal_com_object();
  }
  if (strncmp(symbol, "SteamAPI_GetHSteamUser", 22) == 0 ||
      strncmp(symbol, "SteamAPI_GetHSteamPipe", 22) == 0) {
    return pseudo_handle;
  }

  // 2. Audio Middleware (FMOD, Wwise, Bink, OpenAL, Miles)
  if (strncmp(symbol, "FMOD_", 5) == 0) {
    return 0; // FMOD_OK (0)
  }
  if (strncmp(symbol, "Ak", 2) == 0 || strncmp(symbol, "AK::", 4) == 0) {
    return 1; // AK_Success (1) in Audiokinetic Wwise SDK
  }
  if (strncmp(symbol, "BinkOpen", 8) == 0) {
    return pseudo_handle;
  }
  if (strncmp(symbol, "Bink", 4) == 0) {
    return 0;
  }

  // 3. Epic Online Services (EOS) & Discord & Galaxy & NVAPI & AMD AGS & NVIDIA GFSDK
  if (strncmp(symbol, "EOS_Initialize", 14) == 0 ||
      strncmp(symbol, "EOS_Shutdown", 12) == 0) {
    return 0; // EOS_Success (0)
  }
  if (strncmp(symbol, "EOS_Platform_Create", 19) == 0 ||
      strncmp(symbol, "EOS_Platform_Get", 16) == 0) {
    return pseudo_handle;
  }
  if (strncmp(symbol, "DiscordCreate", 13) == 0) {
    return 0; // DiscordResult_Ok (0)
  }
  if (strncmp(symbol, "NvAPI_Initialize", 16) == 0 ||
      strncmp(symbol, "NvAPI_", 6) == 0) {
    return 0; // NVAPI_OK (0)
  }
  if (strncmp(symbol, "ags", 3) == 0) {
    return 0; // AGS_SUCCESS (0)
  }
  if (strncmp(symbol, "GFSDK_", 6) == 0) {
    return 0; // GFSDK_OK (0)
  }
  if (strncmp(symbol, "Crashpad_", 9) == 0 ||
      strncmp(symbol, "Breakpad_", 9) == 0 ||
      strncmp(symbol, "sentry_", 7) == 0) {
    return 0;
  }

  // 4. Handle & Object Creators / Openers / Loaders
  if (strncmp(symbol, "Create", 6) == 0 ||
      strncmp(symbol, "Open", 4) == 0 ||
      strncmp(symbol, "FindFirst", 9) == 0 ||
      strncmp(symbol, "LoadLibrary", 11) == 0 ||
      strncmp(symbol, "GetModuleHandle", 15) == 0 ||
      strncmp(symbol, "GetStdHandle", 12) == 0 ||
      strncmp(symbol, "BeginPaint", 10) == 0 ||
      strncmp(symbol, "VirtualAlloc", 12) == 0 ||
      strncmp(symbol, "HeapAlloc", 9) == 0 ||
      strncmp(symbol, "GlobalAlloc", 11) == 0 ||
      strncmp(symbol, "LocalAlloc", 10) == 0) {
    return pseudo_handle;
  }

  // 5. Booleans & Success-by-default modifiers
  if (strncmp(symbol, "QueryPerformance", 16) == 0 ||
      strncmp(symbol, "GetClientRect", 13) == 0 ||
      strncmp(symbol, "GetWindowRect", 13) == 0 ||
      strncmp(symbol, "AdjustWindowRect", 16) == 0 ||
      strncmp(symbol, "Is", 2) == 0 ||
      strncmp(symbol, "Set", 3) == 0 ||
      strncmp(symbol, "Close", 5) == 0 ||
      strncmp(symbol, "Delete", 6) == 0 ||
      strncmp(symbol, "Post", 4) == 0 ||
      strncmp(symbol, "Peek", 4) == 0 ||
      strncmp(symbol, "Update", 6) == 0 ||
      strncmp(symbol, "Enable", 6) == 0 ||
      strncmp(symbol, "Initialize", 10) == 0 ||
      strncmp(symbol, "Destroy", 7) == 0 ||
      strncmp(symbol, "Show", 4) == 0 ||
      strncmp(symbol, "Hide", 4) == 0 ||
      strncmp(symbol, "Reset", 5) == 0 ||
      strncmp(symbol, "Play", 4) == 0 ||
      strncmp(symbol, "Stop", 4) == 0 ||
      strncmp(symbol, "Pause", 5) == 0 ||
      strncmp(symbol, "VirtualFree", 11) == 0 ||
      strncmp(symbol, "HeapFree", 8) == 0 ||
      strncmp(symbol, "Release", 7) == 0) {
    return 1;
  }

  if (strcmp(symbol, "GetSystemMetrics") == 0) {
    return 1080;
  }

  // 6. COM / HRESULT / Win32 Status
  if (strncmp(symbol, "Co", 2) == 0 ||
      strncmp(symbol, "Reg", 3) == 0 ||
      strncmp(symbol, "D3D", 3) == 0 ||
      strncmp(symbol, "DXGI", 4) == 0 ||
      strncmp(symbol, "XAudio", 6) == 0 ||
      strncmp(symbol, "MF", 2) == 0 ||
      strncmp(symbol, "Direct", 6) == 0 ||
      strncmp(symbol, "Wsa", 3) == 0 ||
      strncmp(symbol, "WSA", 3) == 0) {
    return 0; // S_OK / ERROR_SUCCESS
  }

  // 7. Versioning & Dimensions
  if (strncmp(symbol, "GetVersion", 10) == 0) {
    return 0x0A00; // Windows 10
  }
  if (strstr(symbol, "Count") || strstr(symbol, "Length") || strstr(symbol, "Size")) {
    return 1;
  }

  // 8. Memory queries
  if (strncmp(symbol, "VirtualQuery", 12) == 0) {
    return 48; // sizeof(MEMORY_BASIC_INFORMATION64)
  }

  return 0;
}

int trt_dynamic_deduce_arg_count(const char* module, const char* symbol) {
  (void)module;
  if (!symbol) return 0;
  if (symbol[0] == '#') return 4;

  if (strncmp(symbol, "Close", 5) == 0 ||
      strncmp(symbol, "Free", 4) == 0 ||
      strncmp(symbol, "GetModuleHandle", 15) == 0 ||
      strncmp(symbol, "LoadLibraryA", 12) == 0 ||
      strncmp(symbol, "LoadLibraryW", 12) == 0 ||
      strncmp(symbol, "SetEvent", 8) == 0 ||
      strncmp(symbol, "ResetEvent", 10) == 0) {
    return 1;
  }

  if (strncmp(symbol, "GetProcAddress", 14) == 0 ||
      strncmp(symbol, "WaitForSingleObject", 19) == 0 ||
      strncmp(symbol, "SetWindowText", 13) == 0 ||
      strncmp(symbol, "ShowWindow", 10) == 0 ||
      strncmp(symbol, "FindClose", 9) == 0) {
    return 2;
  }

  if (strncmp(symbol, "LoadLibraryEx", 13) == 0) {
    return 3;
  }

  if (strncmp(symbol, "PostMessage", 11) == 0 ||
      strncmp(symbol, "SendMessage", 11) == 0) {
    return 4;
  }

  if (strncmp(symbol, "PeekMessage", 11) == 0 ||
      strncmp(symbol, "ReadFile", 8) == 0 ||
      strncmp(symbol, "WriteFile", 9) == 0) {
    return 5;
  }

  if (strncmp(symbol, "CreateFile", 10) == 0) {
    return 7;
  }

  if (strncmp(symbol, "CreateWindowEx", 14) == 0) {
    return 12;
  }
  if (strncmp(symbol, "CreateWindow", 12) == 0) {
    return 11;
  }

  return 4;
}

int trt_dynamic_is_synthesized(const char* module, const char* symbol) {
  if (!module || !symbol) return 0;
  std::string key = std::string(module) + "!" + symbol;
  std::lock_guard<std::mutex> lk(g_synth_mu);
  return g_synthesized_symbols.count(key) ? 1 : 0;
}

void* trt_dynamic_synthesize_api(const char* module, const char* symbol, uint16_t ordinal) {
  const char* mod_name = module ? module : "unknown.dll";
  char ord_buf[32];
  const char* sym_name = symbol;
  if (!sym_name) {
    snprintf(ord_buf, sizeof(ord_buf), "#%u", (unsigned)ordinal);
    sym_name = ord_buf;
  }

  std::string full_key = std::string(mod_name) + "!" + sym_name;
  uint64_t ret_val = trt_dynamic_deduce_return_val(mod_name, sym_name);

  bool first_time = false;
  {
    std::lock_guard<std::mutex> lk(g_synth_mu);
    if (g_synthesized_symbols.find(full_key) == g_synthesized_symbols.end()) {
      g_synthesized_symbols.insert(full_key);
      first_time = true;
    }
  }

  if (first_time) {
    char log_buf[384];
    snprintf(log_buf, sizeof(log_buf),
             "trt: [AUTONOMOUS SYNTHESIS] synthesized dynamic API %s!%s (inferred ret=0x%llx)",
             mod_name, sym_name, (unsigned long long)ret_val);
    trt_core_log(log_buf);
  }

  return trt_thunk_make_ext(trt_core_backend(), "SYNTH", mod_name, sym_name, ret_val);
}

} // extern "C"
