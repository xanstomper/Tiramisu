// tiramisu/src/trt/bridge_psapi.cpp
// PSAPI bridge. See bridge_psapi.h.
//
// The process/module surface reports the REAL guest image list from the
// loader's registry (images.cpp): K32EnumProcessModules walks the loaded
// trt_images, K32GetModuleFileNameExW reads the name each one was loaded
// from. EnumProcesses reports one "process" — the guest itself. Everything
// else honest-fails with last-error + named log.
#include "bridge_psapi.h"
#include "thunk.h"
#include "images.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr u32 k_err_invalid_parameter = 87;
constexpr u32 k_err_insuff_buffer = 122;  // ERROR_INSUFFICIENT_BUFFER
constexpr u32 k_err_invalid_handle = 6;

namespace psapilog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) psapilog::line(s_)

}  // namespace

extern "C" {

// Enumerate the loaded guest images' module handles (the 0x4D-tagged
// registry handles — the same ones LoadLibraryW/GetModuleHandleW return).
int TRT_GUEST_ABI psapi_K32EnumProcessModules(void* proc, void** lphandle,
                                              u32 cb, u32* needed) {
  char line[160];
  if (!lphandle) {
    snprintf(line, sizeof(line),
             "psapi: K32EnumProcessModules -> FALSE (null lphandle)");
    log_line(line);
    return 0;
  }
  const int n = trt_images_count();
  // cb is in bytes; each entry is a HANDLE (8 bytes on x64).
  // STAGE 7: handles are the real image bases (HMODULE == base).
  const u32 fit = cb / 8;
  const u32 count = (n < 0 || (u32)n < fit) ? (u32)(n < 0 ? 0 : n) : fit;
  for (u32 i = 0; i < count; ++i) {
    trt_image* im = trt_images_nth((int)i);
    lphandle[i] = im ? im->base : nullptr;
  }
  if (needed) *needed = (u32)(n < 0 ? 0 : n) * 8u;
  // real semantics: if the buffer was too small, fail with the needed size
  if ((u32)(n < 0 ? 0 : n) > fit) {
    snprintf(line, sizeof(line),
             "psapi: K32EnumProcessModules -> FALSE (cb=%u < %u bytes for %d "
             "guest modules)", cb, (u32)(n < 0 ? 0 : n) * 8u, n);
    log_line(line);
    return 0;
  }
  snprintf(line, sizeof(line),
           "psapi: K32EnumProcessModules -> %u guest module handles (real "
           "image table)", count);
  log_line(line);
  return 1;
}

static int resolve_image_index(void* mod) {
  const int n = trt_images_count();
  if (n <= 0) return -1;
  if (!mod) return 0;
  trt_image* im = trt_images_by_handle(mod);
  if (!im) {
    for (int i = 0; i < n; ++i) {
      trt_image* cand = trt_images_nth(i);
      if (cand && (cand->base == mod || (void*)((uintptr_t)cand->base) == mod)) {
        im = cand;
        break;
      }
    }
  }
  if (im) {
    for (int i = 0; i < n; ++i) {
      if (trt_images_nth(i) == im) return i;
    }
  }
  const u32 v = (u32)(uintptr_t)mod;
  if ((v >> 20) == 0x4D0 && (v & 0xFFFFF) < (u32)n)
    return (int)(v & 0xFFFFF);
  return -1;
}

int TRT_GUEST_ABI psapi_K32GetModuleFileNameExW(void* proc, void* mod,
                                                u16* out, u32 cap) {
  char line[256];
  char host8[512];
  int found = resolve_image_index(mod);
  if (found < 0 || !out || cap == 0) {
    snprintf(line, sizeof(line),
             "psapi: K32GetModuleFileNameExW -> 0 (unknown module handle)");
    log_line(line);
    return 0;
  }
  const char* path = trt_images_nth_path(found);
  const char* name = trt_images_nth_name(found);
  if (!path) {
    snprintf(line, sizeof(line),
             "psapi: K32GetModuleFileNameExW -> 0 (module %d vanished)", found);
    log_line(line);
    return 0;
  }
  // the guest sees the guest path: the file name is the honest part we have
  u32 o = 0;
  const char* base = strrchr(path, '/');
  const char* leaf = base ? base + 1 : (name ? name : path);
  for (; leaf[o] && o + 1 < cap; ++o) out[o] = (u16)(u8)leaf[o];
  out[o] = 0;
  snprintf(host8, sizeof(host8), "%s", path);
  snprintf(line, sizeof(line),
           "psapi: K32GetModuleFileNameExW -> %s (host: %s)", leaf, host8);
  log_line(line);
  return (int)o;
}

int TRT_GUEST_ABI psapi_K32GetModuleFileNameExA(void* proc, void* mod,
                                                char* out, u32 cap) {
  char line[256];
  int found = resolve_image_index(mod);
  if (found < 0 || !out || cap == 0) {
    snprintf(line, sizeof(line),
             "psapi: K32GetModuleFileNameExA -> 0 (unknown module handle)");
    log_line(line);
    return 0;
  }
  const char* path = trt_images_nth_path(found);
  const char* name = trt_images_nth_name(found);
  const char* base = path ? strrchr(path, '/') : nullptr;
  const char* leaf = base ? base + 1 : (name ? name : "?");
  u32 o = 0;
  for (; leaf[o] && o + 1 < cap; ++o) out[o] = leaf[o];
  out[o] = 0;
  snprintf(line, sizeof(line),
           "psapi: K32GetModuleFileNameExA -> %s", leaf);
  log_line(line);
  return (int)o;
}

// EnumProcesses: one guest process (this runtime), pid 1
int TRT_GUEST_ABI psapi_EnumProcesses(u32* ids, u32 cb, u32* returned) {
  char line[128];
  if (!ids || !returned || cb < 4) {
    snprintf(line, sizeof(line),
             "psapi: EnumProcesses -> FALSE (bad args)");
    log_line(line);
    return 0;
  }
  ids[0] = 1;
  *returned = 4;
  snprintf(line, sizeof(line),
           "psapi: EnumProcesses -> 1 pid (the guest itself)");
  log_line(line);
  return 1;
}

// GetModuleFileNameExW/A: non-K32 legacy aliases (GameAssembly-style binaries
// import the plain names); same honest surface as the K32 pair.
int TRT_GUEST_ABI psapi_GetModuleFileNameExW(void* proc, void* mod,
                                             u16* out, u32 cap) {
  char line[128];
  snprintf(line, sizeof(line),
           "psapi: GetModuleFileNameExW (legacy alias of K32GetModuleFileNameExW)");
  log_line(line);
  return psapi_K32GetModuleFileNameExW(proc, mod, out, cap);
}

int TRT_GUEST_ABI psapi_GetModuleFileNameExA(void* proc, void* mod,
                                             char* out, u32 cap) {
  char line[128];
  snprintf(line, sizeof(line),
           "psapi: GetModuleFileNameExA (legacy alias of K32GetModuleFileNameExA)");
  log_line(line);
  return psapi_K32GetModuleFileNameExA(proc, mod, out, cap);
}

int TRT_GUEST_ABI psapi_K32GetModuleBaseNameW(void* proc, void* mod,
                                              u16* out, u32 cap) {
  char line[192];
  int found = resolve_image_index(mod);
  const char* name = found >= 0 ? trt_images_nth_name(found) : nullptr;
  if (!name || !out || cap == 0) {
    snprintf(line, sizeof(line),
             "psapi: K32GetModuleBaseNameW -> 0 (unknown module handle)");
    log_line(line);
    return 0;
  }
  u32 o = 0;
  for (; name[o] && o + 1 < cap; ++o) out[o] = (u16)(u8)name[o];
  out[o] = 0;
  snprintf(line, sizeof(line), "psapi: K32GetModuleBaseNameW -> %s", name);
  log_line(line);
  return (int)o;
}

int TRT_GUEST_ABI psapi_GetModuleBaseNameW(void* proc, void* mod,
                                          u16* out, u32 cap) {
  return psapi_K32GetModuleBaseNameW(proc, mod, out, cap);
}

int TRT_GUEST_ABI psapi_GetModuleBaseNameA(void* proc, void* mod,
                                          char* out, u32 cap) {
  u16 w[256] = {};
  if (!psapi_K32GetModuleBaseNameW(proc, mod, w, 256)) return 0;
  u32 o = 0;
  for (; w[o] && o + 1 < cap; ++o) out[o] = (char)(u8)w[o];
  out[o] = 0;
  return (int)o;
}

int TRT_GUEST_ABI psapi_GetModuleInformation(void* proc, void* mod,
                                             void* info, u32 cb) {
  (void)proc;
  if (!info || cb < 24) return 0;
  const int idx = resolve_image_index(mod);
  if (idx < 0) return 0;
  trt_image* im = trt_images_nth(idx);
  if (!im) return 0;

  struct module_info_x64 {
    void* lpBaseOfDll;
    u32 SizeOfImage;
    u32 padding;
    void* EntryPoint;
  };
  module_info_x64* mi = (module_info_x64*)info;
  mi->lpBaseOfDll = im->base;
  mi->SizeOfImage = (u32)im->size_of_image;
  mi->padding = 0;
  mi->EntryPoint = (void*)((uintptr_t)im->base + im->entry_rva);
  char line[192];
  snprintf(line, sizeof(line),
           "psapi: GetModuleInformation -> base=%p size=0x%x entry=%p",
           mi->lpBaseOfDll, mi->SizeOfImage, mi->EntryPoint);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI psapi_GetProcessMemoryInfo(void* proc, void* info, u32 cb) {
  char line[128];
  snprintf(line, sizeof(line),
           "psapi: GetProcessMemoryInfo -> FALSE (no memory accounting on "
           "console; honest empty)");
  log_line(line);
  (void)proc; (void)info; (void)cb;
  return 0;
}

int trt_bridge_register_psapi(void) {
  static const trt_named_fn k_fns[] = {
      {"K32EnumProcessModules", (void*)&psapi_K32EnumProcessModules},
      {"EnumProcessModules", (void*)&psapi_K32EnumProcessModules},
      {"K32GetModuleFileNameExW", (void*)&psapi_K32GetModuleFileNameExW},
      {"K32GetModuleFileNameExA", (void*)&psapi_K32GetModuleFileNameExA},
      {"GetModuleFileNameExW", (void*)&psapi_GetModuleFileNameExW},
      {"GetModuleFileNameExA", (void*)&psapi_GetModuleFileNameExA},
      {"K32GetModuleBaseNameW", (void*)&psapi_K32GetModuleBaseNameW},
      {"GetModuleBaseNameW", (void*)&psapi_GetModuleBaseNameW},
      {"GetModuleBaseNameA", (void*)&psapi_GetModuleBaseNameA},
      {"GetModuleInformation", (void*)&psapi_GetModuleInformation},
      {"K32GetModuleInformation", (void*)&psapi_GetModuleInformation},
      {"EnumProcesses", (void*)&psapi_EnumProcesses},
      {"GetProcessMemoryInfo", (void*)&psapi_GetProcessMemoryInfo},
  };
  trt_module_register("psapi.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
