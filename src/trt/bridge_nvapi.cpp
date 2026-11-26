// tiramisu/src/trt/bridge_nvapi.cpp
// Honest GPU-vendor stub modules: nvapi.dll, nvml.dll, atiadlxx.dll (and the
// x64 atiadlxy.dll name). See bridge_nvapi.h.
//
// The rule for this whole file: every export logs ONCE, then returns its
// real not-available code. No invented hardware, no fake success — engines
// that probe these DLLs get the same outcome they would on a machine
// without the vendor driver: a clean, named bail-out.
//
//   nvapi.dll   NvAPI_Initialize -> NVAPI_NVIDIA_NOT_FOUND (-160)
//               NvAPI_QueryInterface(id) -> NULL (engines bail on NULL)
//               NvAPI_Unload -> NVAPI_API_NOT_INITIALIZED (-178)
//   nvml.dll    nvmlInit / nvmlInit_v2 / nvmlShutdown
//               nvmlDeviceGetHandleByIndex[_v2]
//                 -> NVML_ERROR_NOT_SUPPORTED (9)
//   atiadl*.dll ADL2_* / ADL_* -> ADL_ERR_NOT_SUPPORTED (-9)
#include "bridge_nvapi.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

#include <atomic>

namespace {
void nvlog(const char* s) { trt_core_log(s); }
}  // namespace

extern "C" {

// ---- nvapi.dll -------------------------------------------------------------
// Real nvapi.dll exports the NvAPI_* gateway (QueryInterface hands out every
// other entry point). Initialize failing with NVIDIA_NOT_FOUND is the exact
// signal engines check before touching anything else.
i32 TRT_STDCALL trt_NvAPI_Initialize(void) {
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("nvapi: NvAPI_Initialize -> NVAPI_NVIDIA_NOT_FOUND (-160): not "
          "available on Xbox, honest stub");
  }
  return -160;
}

i32 TRT_STDCALL trt_NvAPI_Unload(void) {
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("nvapi: NvAPI_Unload -> NVAPI_API_NOT_INITIALIZED (-178) (never "
          "initialized)");
  }
  return -178;
}

void* TRT_STDCALL trt_NvAPI_QueryInterface(u32 id) {
  char line[128];
  snprintf(line, sizeof(line),
           "nvapi: NvAPI_QueryInterface(id=0x%x) -> NULL (honest stub: no "
           "NVIDIA surface)", id);
  nvlog(line);
  return nullptr;  // engines bail cleanly on NULL
}

// ---- nvml.dll --------------------------------------------------------------
i32 TRT_STDCALL trt_nvmlInit_v2(void) {
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("nvml: nvmlInit_v2 -> NVML_ERROR_NOT_SUPPORTED (9): honest stub");
  }
  return 9;
}

i32 TRT_STDCALL trt_nvmlInit(void) { return trt_nvmlInit_v2(); }

i32 TRT_STDCALL trt_nvmlShutdown(void) {
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("nvml: nvmlShutdown -> NVML_ERROR_NOT_SUPPORTED (9): honest stub");
  }
  return 9;
}

i32 TRT_STDCALL trt_nvmlDeviceGetHandleByIndex_v2(u32 index, void* device) {
  if (device) *(void**)device = nullptr;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    char line[160];
    snprintf(line, sizeof(line),
             "nvml: nvmlDeviceGetHandleByIndex_v2(%u) -> "
             "NVML_ERROR_NOT_SUPPORTED (9): honest stub",
             index);
    nvlog(line);
  }
  return 9;
}

i32 TRT_STDCALL trt_nvmlDeviceGetHandleByIndex(u32 index, void* device) {
  return trt_nvmlDeviceGetHandleByIndex_v2(index, device);
}

// ---- atiadlxx.dll / atiadlxy.dll -------------------------------------------
// ADL codes: ADL_OK = 0, ADL_ERR_NOT_SUPPORTED = -9 (the code engines
// switch on to skip the AMD path).
i32 TRT_STDCALL trt_ADL2_Main_Control_Create(void* callback, i32 use_enum) {
  (void)callback; (void)use_enum;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("atiadl: ADL2_Main_Control_Create -> ADL_ERR_NOT_SUPPORTED (-9): "
          "honest stub");
  }
  return -9;
}

i32 TRT_STDCALL trt_ADL2_Main_Control_Destroy(void* context) {
  (void)context;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("atiadl: ADL2_Main_Control_Destroy -> ADL_ERR_NOT_SUPPORTED (-9): "
          "honest stub");
  }
  return -9;
}

i32 TRT_STDCALL trt_ADL2_Adapter_NumberOfAdapters_Get(i32* out) {
  if (out) *out = 0;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("atiadl: ADL2_Adapter_NumberOfAdapters_Get -> "
          "ADL_ERR_NOT_SUPPORTED (-9): honest stub");
  }
  return -9;
}

i32 TRT_STDCALL trt_ADL_Adapter_NumberOfAdapters_Get(i32* out) {
  return trt_ADL2_Adapter_NumberOfAdapters_Get(out);
}

i32 TRT_STDCALL trt_ADL2_Adapter_AdapterInfo_Get(void* info, u32 size) {
  (void)info; (void)size;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("atiadl: ADL2_Adapter_AdapterInfo_Get -> ADL_ERR_NOT_SUPPORTED "
          "(-9): honest stub");
  }
  return -9;
}

// ---- amd_ags_x64.dll (AMD GPU Services) -----------------------------------
// Sekiro (and other AMD-tuned titles) probe AGS for display-mode / crossfire /
// driver-extension entry points. The console has no AMD driver surface of this
// kind (AGS is a Windows driver-extension SDK), so every entry point returns
// the AGS not-available code and clears output pointers. Honest stub: engines
// bail to their non-AGS path instead of guessing.
i32 TRT_STDCALL trt_agsInit(void* ctx, void* gpuInfo, void* config) {
  if (ctx) *(void**)ctx = nullptr;
  if (gpuInfo) *(void**)gpuInfo = nullptr;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("amd_ags: agsInit -> AGS_FAILURE (honest stub: no AGS surface)");
  }
  return -1;  // AGS_FAILURE
}

i32 TRT_STDCALL trt_agsDeInit(void* ctx) {
  (void)ctx;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("amd_ags: agsDeInit -> AGS_FAILURE (honest stub)");
  }
  return -1;
}

i32 TRT_STDCALL trt_agsSetDisplayMode(void* ctx, i32 displayIndex, i32 width,
                                      i32 height, i32 refreshRate, i32 flags) {
  (void)ctx; (void)displayIndex; (void)width; (void)height;
  (void)refreshRate; (void)flags;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    nvlog("amd_ags: agsSetDisplayMode -> AGS_FAILURE (honest stub)");
  }
  return -1;
}

// ---- registration ----------------------------------------------------------
static const trt_named_fn k_nvapi_fns[] = {
    {"NvAPI_Initialize", (void*)&trt_NvAPI_Initialize},
    {"NvAPI_Unload", (void*)&trt_NvAPI_Unload},
    {"NvAPI_QueryInterface", (void*)&trt_NvAPI_QueryInterface},
};

static const trt_named_fn k_nvml_fns[] = {
    {"nvmlInit", (void*)&trt_nvmlInit},
    {"nvmlInit_v2", (void*)&trt_nvmlInit_v2},
    {"nvmlShutdown", (void*)&trt_nvmlShutdown},
    {"nvmlDeviceGetHandleByIndex", (void*)&trt_nvmlDeviceGetHandleByIndex},
    {"nvmlDeviceGetHandleByIndex_v2",
     (void*)&trt_nvmlDeviceGetHandleByIndex_v2},
};

static const trt_named_fn k_atiadl_fns[] = {
    {"ADL2_Main_Control_Create", (void*)&trt_ADL2_Main_Control_Create},
    {"ADL2_Main_Control_Destroy", (void*)&trt_ADL2_Main_Control_Destroy},
    {"ADL2_Adapter_NumberOfAdapters_Get",
     (void*)&trt_ADL2_Adapter_NumberOfAdapters_Get},
    {"ADL_Adapter_NumberOfAdapters_Get",
     (void*)&trt_ADL_Adapter_NumberOfAdapters_Get},
    {"ADL2_Adapter_AdapterInfo_Get", (void*)&trt_ADL2_Adapter_AdapterInfo_Get},
};

int trt_bridge_register_nvapi(void) {
  // nvapi: the real DLL exports the NvAPI_* gateway (capital N-v-A-P-I);
  // engines import those names, so that is the honest surface. Both nvapi.dll (x86)
  // and nvapi64.dll (x64) receive the honest table so 32-bit and 64-bit titles resolve cleanly.
  trt_module_register("nvapi.dll", TRT_ROUTE_BUILTIN, k_nvapi_fns,
                      (int)(sizeof(k_nvapi_fns) / sizeof(k_nvapi_fns[0])));
  trt_module_register("nvapi64.dll", TRT_ROUTE_BUILTIN, k_nvapi_fns,
                      (int)(sizeof(k_nvapi_fns) / sizeof(k_nvapi_fns[0])));
  trt_module_register("nvml.dll", TRT_ROUTE_BUILTIN, k_nvml_fns,
                      (int)(sizeof(k_nvml_fns) / sizeof(k_nvml_fns[0])));
  // AMD ADL ships two names: atiadlxx.dll (x86) and atiadlxy.dll (x64).
  // Both get the same honest table so either probe resolves.
  trt_module_register("atiadlxx.dll", TRT_ROUTE_BUILTIN, k_atiadl_fns,
                      (int)(sizeof(k_atiadl_fns) / sizeof(k_atiadl_fns[0])));
  trt_module_register("atiadlxy.dll", TRT_ROUTE_BUILTIN, k_atiadl_fns,
                      (int)(sizeof(k_atiadl_fns) / sizeof(k_atiadl_fns[0])));
  // AMD GPU Services (amd_ags_x64.dll / amd_ags_x86.dll) — Sekiro and other AMD-tuned titles.
  static const trt_named_fn k_amdags_fns[] = {
      {"agsInit", (void*)&trt_agsInit},
      {"agsDeInit", (void*)&trt_agsDeInit},
      {"agsInitialize", (void*)&trt_agsInit},
      {"agsDeInitialize", (void*)&trt_agsDeInit},
      {"agsSetDisplayMode", (void*)&trt_agsSetDisplayMode},
  };
  trt_module_register("amd_ags_x64.dll", TRT_ROUTE_BUILTIN, k_amdags_fns,
                      (int)(sizeof(k_amdags_fns) / sizeof(k_amdags_fns[0])));
  trt_module_register("amd_ags_x86.dll", TRT_ROUTE_BUILTIN, k_amdags_fns,
                      (int)(sizeof(k_amdags_fns) / sizeof(k_amdags_fns[0])));
  return 0;
}

}  // extern "C"
