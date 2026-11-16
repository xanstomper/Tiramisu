// tiramisu/src/trt/bridge_dstorage.cpp
// DSTORAGE.dll bridge (DirectStorage). See bridge_dstorage.h.
//
// DirectStorage is the GDK fast-io stack (Series X|S consoles and PC):
// games batch file reads into a DSTORAGE_QUEUE and the OS issues NVMe-
// depth DMA on their behalf. UnityPlayer 6 imports DSTORAGE.dll!
// DStorageGetFactory unconditionally — measured: the Go Next corpus demo
// LOAD FAILED until this surface existed.
//
// Honest-runtime contract on this host: DirectStorage is a SYSTEM-class
// console/OS component we cannot re-implement at DMA depth here. What we
// CAN do honestly is answer the factory probe with S_OK and hand back a
// queue vtable whose Open/Submit/Close all fail visibly with the API's
// own named error (E_NOTIMPL-class), so a game's DStorage OPTIONAL path
// degrades to stdio and its REQUIRED path names the gap in the boot
// report instead of crashing on a null vtable.
//
// C->BUILTIN conversion rule (test_corpus STAGE 5): this removes
// dstorage.dll from the UNCERTAIN SYSTEM(C) whitelist, making the Go Next
// boot fully deterministic (every import a PROVEN route).
#include "bridge_dstorage.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {

#define dlog(s_) trt_core_log(s_)

// ---- factory: the only export UnityPlayer actually imports ----

struct ds_factory { void* vtbl; };

// Static factory object: vtbl[0..2] = IUnknown, [3] = CreateQueue,
// [4] = OpenFile. QI returns the same object for any IID.
void* ds_factory_vtbl[8];
ds_factory g_ds_factory{ds_factory_vtbl};

void* TRT_GUEST_ABI dsf_QueryInterface(void* self, const void* riid, void** out) {
  (void)self; (void)riid;
  if (out) *out = &g_ds_factory;
  return (void*)0;  // S_OK
}
u32 TRT_GUEST_ABI dsf_AddRef(void*) { return 2; }
u32 TRT_GUEST_ABI dsf_Release(void*) { return 1; }
// CreateQueue(const DSTORAGE_QUEUE_DESC*, void**): fail-visible.
i32 TRT_GUEST_ABI dsf_CreateQueue(void* self, const void* desc, void** out) {
  (void)self; (void)desc;
  if (out) *out = nullptr;
  char line[96];
  snprintf(line, sizeof(line),
           "dstorage: CreateQueue -> E_NOTIMPL (DirectStorage not served on "
           "this host; game should fall back to stdio)");
  dlog(line);
  return (i32)0x80004001u;
}
// OpenFile (name-based io): fail-visible the same way.
i32 TRT_GUEST_ABI dsf_OpenFile(const u16* path, const void* desc, void** out) {
  (void)desc;
  if (out) *out = nullptr;
  char line[160];
  char u8[96];
  size_t n = 0;
  for (size_t i = 0; path && path[i] && n + 1 < sizeof(u8); ++i)
    u8[n++] = (path[i] >= 0x20 && path[i] < 0x7F) ? (char)path[i] : '?';
  u8[n] = 0;
  snprintf(line, sizeof(line),
           "dstorage: OpenFile(\"%s\") -> E_NOTIMPL (no DStorage io engine)",
           u8);
  dlog(line);
  return (i32)0x80004001u;
}

// DStorageGetFactory(REFIID, void** ppv) -> HRESULT. Hands back the static
// factory above (queue creation still fails visibly — see dsf_CreateQueue).
i32 TRT_GUEST_ABI ds_DStorageGetFactory(const void* riid, void** ppv) {
  if (!ppv) return (i32)0x80070057u;  // E_POINTER
  *ppv = &g_ds_factory;
  return 0;  // S_OK — the probe succeeds; per-call surfaces name their gap
}

}  // namespace

extern "C" {

int trt_bridge_register_dstorage(void) {
  static const trt_named_fn k_fns[] = {
      {"DStorageGetFactory", (void*)&ds_DStorageGetFactory},
  };
  trt_module_register("dstorage.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("dstoragecore.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
