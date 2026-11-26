// tiramisu/src/trt/bridge_sys_extras.cpp
// Honest BUILTIN bridges for auxiliary Windows system DLLs commonly imported
// (statically or via delay-load) by Unity, Unreal, .NET, and PC engines:
//   - powrprof.dll : PowerCreateRequest / CallNtPowerInformation / PowerDeterminePlatformRole
//   - pdh.dll      : Performance Data Helper queries/counters
//   - wer.dll      : WerRegisterRuntimeExceptionModule / WerRegisterMemoryBlock (+ kernel32)
//   - propsys.dll  : In-memory COM IPropertyStore + PropVariant helpers
//   - userenv.dll  : User profile directories & environment blocks
//   - wtsapi32.dll : Session notifications & WTSQuerySessionInformation
#include "bridge_sys_extras.h"
#include "gdi_raster.h"
#include "bridge_ole.h"
#include "trt.h"
#include "thunk.h"

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>

extern "C" {
void TRT_STDCALL trt_SetLastError(u32 e);
void* TRT_STDCALL trt_LocalAlloc(u32 flags, u64 bytes);
void* TRT_STDCALL trt_LocalFree(void* p);
}

namespace {

constexpr i32 S_OK_ = 0;
constexpr i32 E_NOINTERFACE_ = (i32)0x80004002u;
constexpr i32 E_POINTER_ = (i32)0x80004003u;
constexpr i32 E_INVALIDARG_ = (i32)0x80070057u;
constexpr i32 TYPE_E_ELEMENTNOTFOUND_ = (i32)0x8002802Bu;

void write_u16_str(const char* src, u16* dst, u32 cap) {
  if (!dst || cap == 0) return;
  u32 i = 0;
  for (; src && src[i] && i + 1 < cap; ++i) dst[i] = (u16)(u8)src[i];
  dst[i] = 0;
}

bool ascii_contains_ci(const char* hay, const char* needle) {
  if (!hay || !needle || !needle[0]) return false;
  const size_t nlen = strlen(needle);
  for (size_t i = 0; hay[i]; ++i) {
    size_t j = 0;
    while (j < nlen && hay[i + j]) {
      char a = hay[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
      ++j;
    }
    if (j == nlen) return true;
  }
  return false;
}

// ============================================================================
// 1. powrprof.dll + Power*Request on kernel32
// ============================================================================
struct power_req_slot {
  bool used = false;
  u32 active_mask = 0;
};
std::mutex g_pwr_mu;
power_req_slot g_pwr_slots[64];

bool valid_pwr_handle(void* h, int* slot_out) {
  const uintptr_t v = (uintptr_t)h;
  if (v < 0x50570001ULL || v >= 0x50570041ULL) return false;
  int idx = (int)(v - 0x50570001ULL);
  if (idx < 0 || idx >= 64) return false;
  if (slot_out) *slot_out = idx;
  return true;
}

void* TRT_GUEST_ABI pwr_PowerCreateRequest(void* context) {
  (void)context;
  std::lock_guard<std::mutex> lk(g_pwr_mu);
  for (int i = 0; i < 64; ++i) {
    if (!g_pwr_slots[i].used) {
      g_pwr_slots[i].used = true;
      g_pwr_slots[i].active_mask = 0;
      return (void*)(uintptr_t)(0x50570001ULL + (u64)i);
    }
  }
  trt_SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
  return (void*)(intptr_t)-1;
}

int TRT_GUEST_ABI pwr_PowerSetRequest(void* h, u32 req_type) {
  int idx = -1;
  std::lock_guard<std::mutex> lk(g_pwr_mu);
  if (!valid_pwr_handle(h, &idx) || !g_pwr_slots[idx].used || req_type > 3) {
    trt_SetLastError(87);
    return 0;
  }
  g_pwr_slots[idx].active_mask |= (1u << req_type);
  return 1;
}

int TRT_GUEST_ABI pwr_PowerClearRequest(void* h, u32 req_type) {
  int idx = -1;
  std::lock_guard<std::mutex> lk(g_pwr_mu);
  if (!valid_pwr_handle(h, &idx) || !g_pwr_slots[idx].used || req_type > 3) {
    trt_SetLastError(87);
    return 0;
  }
  g_pwr_slots[idx].active_mask &= ~(1u << req_type);
  return 1;
}

int TRT_GUEST_ABI pwr_PowerCloseRequest(void* h) {
  int idx = -1;
  std::lock_guard<std::mutex> lk(g_pwr_mu);
  if (!valid_pwr_handle(h, &idx) || !g_pwr_slots[idx].used) return 0;
  g_pwr_slots[idx].used = false;
  g_pwr_slots[idx].active_mask = 0;
  return 1;
}

i32 TRT_GUEST_ABI pwr_CallNtPowerInformation(int info_level, void* in_buf,
                                             u32 in_sz, void* out_buf,
                                             u32 out_sz) {
  (void)in_buf;
  (void)in_sz;
  if (!out_buf && out_sz > 0) return (i32)0xC000000Du;  // STATUS_INVALID_PARAMETER
  if (info_level == 11 /*ProcessorInformation*/) {
    // PROCESSOR_POWER_INFORMATION is 6 * u32 = 24 bytes per logical CPU
    if (out_sz < 24) return (i32)0xC0000023u;  // STATUS_BUFFER_TOO_SMALL
    const u32 count = out_sz / 24u;
    memset(out_buf, 0, out_sz);
    for (u32 i = 0; i < count && i < 8u; ++i) {
      u32* rec = (u32*)((u8*)out_buf + i * 24u);
      rec[0] = i;      // Number
      rec[1] = 3600u;  // MaxMhz
      rec[2] = 3600u;  // CurrentMhz
      rec[3] = 3600u;  // MhzLimit
      rec[4] = 0;      // MaxIdleState
      rec[5] = 0;      // CurrentIdleState
    }
    return 0;  // STATUS_SUCCESS
  }
  if (info_level == 5 /*SystemBatteryState*/) {
    if (out_sz < 32) return (i32)0xC0000023u;
    memset(out_buf, 0, out_sz);
    u8* b = (u8*)out_buf;
    b[0] = 1;  // AcOnLine = TRUE
    b[1] = 0;  // BatteryPresent = FALSE
    return 0;
  }
  if (out_buf && out_sz) memset(out_buf, 0, out_sz);
  return 0;  // STATUS_SUCCESS
}

u32 TRT_GUEST_ABI pwr_PowerDeterminePlatformRole(void) {
  return 1;  // PlatformRoleDesktop
}
u32 TRT_GUEST_ABI pwr_PowerDeterminePlatformRoleEx(u32 ver) {
  (void)ver;
  return 1;  // PlatformRoleDesktop
}

u32 TRT_GUEST_ABI pwr_PowerRegisterSuspendResumeNotification(u32 flags,
                                                             void* recipient,
                                                             void** reg_out) {
  (void)flags;
  (void)recipient;
  if (!reg_out) return 87;
  *reg_out = (void*)(uintptr_t)0x5057F001ULL;
  return 0;  // ERROR_SUCCESS
}

u32 TRT_GUEST_ABI pwr_PowerUnregisterSuspendResumeNotification(void* reg) {
  (void)reg;
  return 0;
}

u32 TRT_GUEST_ABI pwr_PowerGetActiveScheme(void* key, void** out_guid) {
  (void)key;
  if (!out_guid) return 87;
  void* g = trt_LocalAlloc(0x40 /*LPTR*/, 16);
  if (!g) return 8;
  // GUID_MAX_POWER_SAVINGS high-performance-like scheme ID
  static const u8 k_perf[16] = {
      0xA7, 0x04, 0x5E, 0x8C, 0x53, 0xC1, 0x94, 0x43,
      0x91, 0xF1, 0x60, 0xE5, 0x89, 0x22, 0xEA, 0x21};
  memcpy(g, k_perf, 16);
  *out_guid = g;
  return 0;
}

// ============================================================================
// 2. pdh.dll — Performance Data Helper
// ============================================================================
struct pdh_query {
  bool used = false;
  u64 collect_count = 0;
};
struct pdh_counter {
  bool used = false;
  uintptr_t query_tag = 0;
  std::string path;
};
std::mutex g_pdh_mu;
pdh_query g_pdh_queries[64];
pdh_counter g_pdh_counters[256];

i32 TRT_GUEST_ABI pdh_OpenQueryW(const u16* data_src, u64 user_data,
                                 void** out_query) {
  (void)data_src;
  (void)user_data;
  if (!out_query) return (i32)0xC0000BBDu;  // PDH_INVALID_ARGUMENT
  std::lock_guard<std::mutex> lk(g_pdh_mu);
  for (int i = 0; i < 64; ++i) {
    if (!g_pdh_queries[i].used) {
      g_pdh_queries[i].used = true;
      g_pdh_queries[i].collect_count = 0;
      *out_query = (void*)(uintptr_t)(0x50440001ULL + (u64)i);
      return 0;
    }
  }
  return (i32)0x800007D0u;  // PDH_MEMORY_ALLOCATION_FAILURE
}

i32 TRT_GUEST_ABI pdh_OpenQueryA(const char* data_src, u64 user_data,
                                 void** out_query) {
  (void)data_src;
  return pdh_OpenQueryW(nullptr, user_data, out_query);
}

i32 TRT_GUEST_ABI pdh_CloseQuery(void* q) {
  const uintptr_t v = (uintptr_t)q;
  if (v < 0x50440001ULL || v >= 0x50440041ULL) return (i32)0xC0000BBCu;
  const int qidx = (int)(v - 0x50440001ULL);
  std::lock_guard<std::mutex> lk(g_pdh_mu);
  if (!g_pdh_queries[qidx].used) return (i32)0xC0000BBCu;
  g_pdh_queries[qidx].used = false;
  for (int i = 0; i < 256; ++i) {
    if (g_pdh_counters[i].used && g_pdh_counters[i].query_tag == v) {
      g_pdh_counters[i].used = false;
      g_pdh_counters[i].path.clear();
    }
  }
  return 0;
}

static i32 pdh_add_counter_utf8(void* q, const char* utf8, u64 user_data,
                                void** out_counter) {
  (void)user_data;
  const uintptr_t v = (uintptr_t)q;
  if (!out_counter || v < 0x50440001ULL || v >= 0x50440041ULL)
    return (i32)0xC0000BBDu;
  std::lock_guard<std::mutex> lk(g_pdh_mu);
  const int qidx = (int)(v - 0x50440001ULL);
  if (!g_pdh_queries[qidx].used) return (i32)0xC0000BBCu;
  for (int i = 0; i < 256; ++i) {
    if (!g_pdh_counters[i].used) {
      g_pdh_counters[i].used = true;
      g_pdh_counters[i].query_tag = v;
      g_pdh_counters[i].path = utf8 ? utf8 : "";
      *out_counter = (void*)(uintptr_t)(0x50450001ULL + (u64)i);
      return 0;
    }
  }
  return (i32)0x800007D0u;
}

i32 TRT_GUEST_ABI pdh_AddCounterW(void* q, const u16* path, u64 user_data,
                                  void** out_counter) {
  char buf[512] = {};
  if (path && trt_utf16_to_utf8(path, buf, sizeof(buf)) != 0) buf[0] = 0;
  return pdh_add_counter_utf8(q, buf, user_data, out_counter);
}

i32 TRT_GUEST_ABI pdh_AddCounterA(void* q, const char* path, u64 user_data,
                                  void** out_counter) {
  return pdh_add_counter_utf8(q, path, user_data, out_counter);
}

i32 TRT_GUEST_ABI pdh_RemoveCounter(void* c) {
  const uintptr_t v = (uintptr_t)c;
  if (v < 0x50450001ULL || v >= 0x50450101ULL) return (i32)0xC0000BBCu;
  std::lock_guard<std::mutex> lk(g_pdh_mu);
  const int idx = (int)(v - 0x50450001ULL);
  if (!g_pdh_counters[idx].used) return (i32)0xC0000BBCu;
  g_pdh_counters[idx].used = false;
  g_pdh_counters[idx].path.clear();
  return 0;
}

i32 TRT_GUEST_ABI pdh_CollectQueryData(void* q) {
  const uintptr_t v = (uintptr_t)q;
  if (v < 0x50440001ULL || v >= 0x50440041ULL) return (i32)0xC0000BBCu;
  std::lock_guard<std::mutex> lk(g_pdh_mu);
  const int idx = (int)(v - 0x50440001ULL);
  if (!g_pdh_queries[idx].used) return (i32)0xC0000BBCu;
  ++g_pdh_queries[idx].collect_count;
  return 0;
}

i32 TRT_GUEST_ABI pdh_GetFormattedCounterValue(void* c, u32 format, u32* type,
                                               void* value_out) {
  // PDH_FMT_COUNTERVALUE on x64: u32 CStatus (0) + u32 pad + union(i32 / double / i64 / ptr) = 16B
  const uintptr_t v = (uintptr_t)c;
  if (!value_out || v < 0x50450001ULL || v >= 0x50450101ULL)
    return (i32)0xC0000BBDu;
  std::string path;
  {
    std::lock_guard<std::mutex> lk(g_pdh_mu);
    const int idx = (int)(v - 0x50450001ULL);
    if (!g_pdh_counters[idx].used) return (i32)0xC0000BBCu;
    path = g_pdh_counters[idx].path;
  }
  if (type) *type = 0;
  memset(value_out, 0, 16);
  double val = 10.0;
  if (ascii_contains_ci(path.c_str(), "available mbytes")) val = 4096.0;
  else if (ascii_contains_ci(path.c_str(), "available bytes")) val = 4096.0 * 1024.0 * 1024.0;
  else if (ascii_contains_ci(path.c_str(), "processor time")) val = 12.5;
  else if (ascii_contains_ci(path.c_str(), "committed bytes")) val = 512.0 * 1024.0 * 1024.0;
  // PDH_FMT_LONG=0x100, PDH_FMT_DOUBLE=0x200, PDH_FMT_LARGE=0x400
  if (format & 0x200u) {
    memcpy((u8*)value_out + 8, &val, sizeof(double));
  } else if (format & 0x400u) {
    const i64 iv = (i64)val;
    memcpy((u8*)value_out + 8, &iv, sizeof(i64));
  } else {
    const i32 iv = (i32)val;
    memcpy((u8*)value_out + 8, &iv, sizeof(i32));
  }
  return 0;
}

i32 TRT_GUEST_ABI pdh_ValidatePathW(const u16* path) {
  return (path && path[0]) ? 0 : (i32)0xC0000BBDu;
}
i32 TRT_GUEST_ABI pdh_ValidatePathA(const char* path) {
  return (path && path[0]) ? 0 : (i32)0xC0000BBDu;
}

// ============================================================================
// 3. wer.dll — Windows Error Reporting runtime module registration
// ============================================================================
struct wer_rt_mod {
  std::string path;
  void* ctx = nullptr;
};
std::mutex g_wer_mu;
std::vector<wer_rt_mod> g_wer_mods;
std::atomic<u32> g_wer_flags{0};

i32 TRT_GUEST_ABI wer_RegisterRuntimeExceptionModule(const u16* path,
                                                     void* context) {
  if (!path || !path[0]) return E_INVALIDARG_;
  char utf8[512] = {};
  if (trt_utf16_to_utf8(path, utf8, sizeof(utf8)) != 0) utf8[0] = 0;
  {
    std::lock_guard<std::mutex> lk(g_wer_mu);
    g_wer_mods.push_back({std::string(utf8), context});
  }
  char line[320];
  snprintf(line, sizeof(line),
           "wer: WerRegisterRuntimeExceptionModule(%s) -> S_OK", utf8);
  trt_core_log(line);
  return S_OK_;
}

i32 TRT_GUEST_ABI wer_UnregisterRuntimeExceptionModule(const u16* path,
                                                       void* context) {
  char utf8[512] = {};
  if (path && trt_utf16_to_utf8(path, utf8, sizeof(utf8)) != 0) utf8[0] = 0;
  std::lock_guard<std::mutex> lk(g_wer_mu);
  for (size_t i = 0; i < g_wer_mods.size(); ++i) {
    if (g_wer_mods[i].ctx == context &&
        (!utf8[0] || g_wer_mods[i].path == utf8)) {
      g_wer_mods.erase(g_wer_mods.begin() + (ptrdiff_t)i);
      return S_OK_;
    }
  }
  return S_OK_;
}

i32 TRT_GUEST_ABI wer_RegisterMemoryBlock(const void* addr, u32 size) {
  (void)addr;
  (void)size;
  return S_OK_;
}
i32 TRT_GUEST_ABI wer_UnregisterMemoryBlock(const void* addr) {
  (void)addr;
  return S_OK_;
}
i32 TRT_GUEST_ABI wer_SetFlags(u32 flags) {
  g_wer_flags.store(flags, std::memory_order_release);
  return S_OK_;
}
i32 TRT_GUEST_ABI wer_GetFlags(void* proc, u32* flags) {
  (void)proc;
  if (!flags) return E_POINTER_;
  *flags = g_wer_flags.load(std::memory_order_acquire);
  return S_OK_;
}
i32 TRT_GUEST_ABI wer_RegisterFile(const u16* file, int reg_type, u32 flags) {
  (void)file;
  (void)reg_type;
  (void)flags;
  return S_OK_;
}

// ============================================================================
// 4. propsys.dll — In-memory IPropertyStore + PropVariant helpers
// ============================================================================
struct prop_key_raw {
  u8 bytes[20];  // GUID (16) + pid (4)
};
struct prop_var_raw {
  u8 bytes[24];  // PROPVARIANT on x64 is 24 bytes
};
struct prop_entry {
  prop_key_raw key;
  prop_var_raw val;
};
struct mem_prop_store {
  const void** vtbl;
  std::atomic<u32> refcount;
  std::mutex mu;
  std::vector<prop_entry> items;
};

i32 TRT_GUEST_ABI ps_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (!out) return E_POINTER_;
  *out = self;
  ((mem_prop_store*)self)->refcount.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI ps_AddRef(void* self) {
  return ((mem_prop_store*)self)->refcount.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI ps_Release(void* self) {
  auto* s = (mem_prop_store*)self;
  const u32 r = s->refcount.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (r == 0) delete s;
  return r;
}
i32 TRT_GUEST_ABI ps_GetCount(void* self, u32* count) {
  if (!count) return E_POINTER_;
  auto* s = (mem_prop_store*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  *count = (u32)s->items.size();
  return S_OK_;
}
i32 TRT_GUEST_ABI ps_GetAt(void* self, u32 idx, void* pkey_out) {
  if (!pkey_out) return E_POINTER_;
  auto* s = (mem_prop_store*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  if (idx >= s->items.size()) return E_INVALIDARG_;
  memcpy(pkey_out, s->items[idx].key.bytes, 20);
  return S_OK_;
}
i32 TRT_GUEST_ABI ps_GetValue(void* self, const void* key, void* pv_out) {
  if (!key || !pv_out) return E_POINTER_;
  memset(pv_out, 0, 24);
  auto* s = (mem_prop_store*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  for (const auto& e : s->items) {
    if (memcmp(e.key.bytes, key, 20) == 0) {
      memcpy(pv_out, e.val.bytes, 24);
      return S_OK_;
    }
  }
  return S_OK_;  // VT_EMPTY per IPropertyStore::GetValue spec
}
i32 TRT_GUEST_ABI ps_SetValue(void* self, const void* key, const void* pv) {
  if (!key || !pv) return E_POINTER_;
  auto* s = (mem_prop_store*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  for (auto& e : s->items) {
    if (memcmp(e.key.bytes, key, 20) == 0) {
      memcpy(e.val.bytes, pv, 24);
      return S_OK_;
    }
  }
  prop_entry pe;
  memcpy(pe.key.bytes, key, 20);
  memcpy(pe.val.bytes, pv, 24);
  s->items.push_back(pe);
  return S_OK_;
}
i32 TRT_GUEST_ABI ps_Commit(void* self) {
  (void)self;
  return S_OK_;
}

const void* g_propstore_vtbl[8] = {
    (const void*)&ps_QueryInterface,
    (const void*)&ps_AddRef,
    (const void*)&ps_Release,
    (const void*)&ps_GetCount,
    (const void*)&ps_GetAt,
    (const void*)&ps_GetValue,
    (const void*)&ps_SetValue,
    (const void*)&ps_Commit,
};

i32 TRT_GUEST_ABI ps_PSCreateMemoryPropertyStore(const void* riid, void** out) {
  (void)riid;
  if (!out) return E_POINTER_;
  auto* s = new mem_prop_store();
  s->vtbl = g_propstore_vtbl;
  s->refcount.store(1, std::memory_order_relaxed);
  *out = s;
  return S_OK_;
}

i32 TRT_GUEST_ABI ps_PropVariantToString(const void* pv, u16* out, u32 cap) {
  if (!out || cap == 0) return E_INVALIDARG_;
  out[0] = 0;
  if (!pv) return S_OK_;
  const u16 vt = *(const u16*)pv;
  if (vt == 31 /*VT_LPWSTR*/ || vt == 8 /*VT_BSTR*/) {
    const u16* s = *(const u16* const*)((const u8*)pv + 8);
    if (s) {
      u32 i = 0;
      for (; s[i] && i + 1 < cap; ++i) out[i] = s[i];
      out[i] = 0;
      return S_OK_;
    }
  }
  return TYPE_E_ELEMENTNOTFOUND_;
}

i32 TRT_GUEST_ABI ps_PSGetPropertyKeyFromName(const u16* name, void* pkey_out) {
  (void)name;
  if (!pkey_out) return E_POINTER_;
  memset(pkey_out, 0, 20);
  return TYPE_E_ELEMENTNOTFOUND_;
}

// ============================================================================
// 5. userenv.dll — Profile paths & environment block
// ============================================================================
static int copy_dir_u16(const char* val, u16* buf, u32* len_io) {
  if (!len_io) { trt_SetLastError(87); return 0; }
  const u32 need = (u32)strlen(val) + 1u;
  const u32 cap = *len_io;
  *len_io = need;
  if (!buf || cap < need) {
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  write_u16_str(val, buf, cap);
  return 1;
}

static int copy_dir_a(const char* val, char* buf, u32* len_io) {
  if (!len_io) { trt_SetLastError(87); return 0; }
  const u32 need = (u32)strlen(val) + 1u;
  const u32 cap = *len_io;
  *len_io = need;
  if (!buf || cap < need) {
    trt_SetLastError(122);
    return 0;
  }
  memcpy(buf, val, need);
  return 1;
}

int TRT_GUEST_ABI uenv_GetUserProfileDirectoryW(void* tok, u16* buf, u32* len) {
  (void)tok;
  return copy_dir_u16("C:\\users\\player", buf, len);
}
int TRT_GUEST_ABI uenv_GetUserProfileDirectoryA(void* tok, char* buf, u32* len) {
  (void)tok;
  return copy_dir_a("C:\\users\\player", buf, len);
}
int TRT_GUEST_ABI uenv_GetProfilesDirectoryW(u16* buf, u32* len) {
  return copy_dir_u16("C:\\users", buf, len);
}
int TRT_GUEST_ABI uenv_GetProfilesDirectoryA(char* buf, u32* len) {
  return copy_dir_a("C:\\users", buf, len);
}
int TRT_GUEST_ABI uenv_GetAllUsersProfileDirectoryW(u16* buf, u32* len) {
  return copy_dir_u16("C:\\ProgramData", buf, len);
}
int TRT_GUEST_ABI uenv_CreateEnvironmentBlock(void** env, void* tok, int inherit) {
  (void)tok;
  (void)inherit;
  if (!env) { trt_SetLastError(87); return 0; }
  // Double-NUL terminated empty Unicode env block
  u16* block = (u16*)calloc(4, sizeof(u16));
  if (!block) return 0;
  *env = block;
  return 1;
}
int TRT_GUEST_ABI uenv_DestroyEnvironmentBlock(void* env) {
  free(env);
  return 1;
}

// ============================================================================
// 6. wtsapi32.dll — Terminal Services session state
// ============================================================================
int TRT_GUEST_ABI wts_WTSRegisterSessionNotification(void* hwnd, u32 flags) {
  (void)hwnd; (void)flags;
  return 1;
}
int TRT_GUEST_ABI wts_WTSUnRegisterSessionNotification(void* hwnd) {
  (void)hwnd;
  return 1;
}
int TRT_GUEST_ABI wts_WTSRegisterSessionNotificationEx(void* srv, void* hwnd, u32 flags) {
  (void)srv; (void)hwnd; (void)flags;
  return 1;
}
int TRT_GUEST_ABI wts_WTSUnRegisterSessionNotificationEx(void* srv, void* hwnd) {
  (void)srv; (void)hwnd;
  return 1;
}

int TRT_GUEST_ABI wts_WTSQuerySessionInformationW(void* srv, u32 sid,
                                                  u32 info_class, void** out_buf,
                                                  u32* out_bytes) {
  (void)srv; (void)sid;
  if (!out_buf || !out_bytes) { trt_SetLastError(87); return 0; }
  if (info_class == 4 /*WTSSessionId*/ || info_class == 8 /*WTSConnectState*/) {
    u32* p = (u32*)trt_LocalAlloc(0x40, sizeof(u32));
    if (!p) return 0;
    *p = (info_class == 4) ? 1u : 0u /*WTSActive*/;
    *out_buf = p;
    *out_bytes = sizeof(u32);
    return 1;
  }
  if (info_class == 5 /*WTSUserName*/ || info_class == 10 /*WTSClientName*/ ||
      info_class == 11 /*WTSWinStationName*/) {
    const char* s = (info_class == 5) ? "player" : "Console";
    const u32 bytes = (u32)(strlen(s) + 1) * 2u;
    u16* w = (u16*)trt_LocalAlloc(0x40, bytes);
    if (!w) return 0;
    write_u16_str(s, w, bytes / 2u);
    *out_buf = w;
    *out_bytes = bytes;
    return 1;
  }
  u16* empty = (u16*)trt_LocalAlloc(0x40, 4);
  if (!empty) return 0;
  *out_buf = empty;
  *out_bytes = 2;
  return 1;
}

int TRT_GUEST_ABI wts_WTSQuerySessionInformationA(void* srv, u32 sid,
                                                  u32 info_class, void** out_buf,
                                                  u32* out_bytes) {
  (void)srv; (void)sid;
  if (!out_buf || !out_bytes) { trt_SetLastError(87); return 0; }
  if (info_class == 4 || info_class == 8) {
    u32* p = (u32*)trt_LocalAlloc(0x40, sizeof(u32));
    if (!p) return 0;
    *p = (info_class == 4) ? 1u : 0u;
    *out_buf = p;
    *out_bytes = sizeof(u32);
    return 1;
  }
  const char* s = (info_class == 5) ? "player" : "Console";
  const u32 bytes = (u32)strlen(s) + 1u;
  char* a = (char*)trt_LocalAlloc(0x40, bytes);
  if (!a) return 0;
  memcpy(a, s, bytes);
  *out_buf = a;
  *out_bytes = bytes;
  return 1;
}

void TRT_GUEST_ABI wts_WTSFreeMemory(void* p) {
  if (p) trt_LocalFree(p);
}

int TRT_GUEST_ABI k32_ProcessIdToSessionId(u32 pid, u32* sid) {
  (void)pid;
  if (!sid) { trt_SetLastError(87); return 0; }
  *sid = 1u;
  return 1;
}

// ---- 7. vulkan-1.dll now implemented in bridge_vulkan.cpp ---------------------
std::atomic<bool> g_comdlg_logged{false};
std::atomic<bool> g_wintrust_logged{false};

// ---- 8. comdlg32.dll — honest user-cancel / no desktop common dialogs --------

int TRT_GUEST_ABI cdlg_fail_bool(void*) {
  if (!g_comdlg_logged.exchange(true, std::memory_order_relaxed)) {
    trt_core_log("[comdlg32] desktop common file/color/font dialog unavailable on Xbox");
  }
  trt_SetLastError(0);
  return 0;
}

void* TRT_GUEST_ABI cdlg_fail_ptr(void*) {
  if (!g_comdlg_logged.exchange(true, std::memory_order_relaxed)) {
    trt_core_log("[comdlg32] desktop common find/replace dialog unavailable on Xbox");
  }
  return nullptr;
}

u32 TRT_GUEST_ABI cdlg_CommDlgExtendedError(void) { return 0; }

// ---- 9. uxtheme.dll — classic non-themed UWP window chrome ------------------

constexpr i32 E_NOTIMPL_ = (i32)0x80004001u;

void* TRT_GUEST_ABI uxt_OpenThemeData(void*, const u16*) { return nullptr; }
void* TRT_GUEST_ABI uxt_OpenThemeDataEx(void*, const u16*, u32) { return nullptr; }
void* TRT_GUEST_ABI uxt_OpenThemeDataForDpi(void*, const u16*, u32) {
  return nullptr;
}
i32 TRT_GUEST_ABI uxt_CloseThemeData(void*) { return S_OK_; }
int TRT_GUEST_ABI uxt_IsThemeActive(void) { return 0; }
int TRT_GUEST_ABI uxt_IsAppThemed(void) { return 0; }
i32 TRT_GUEST_ABI uxt_SetWindowTheme(void*, const u16*, const u16*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_DrawThemeBackground(void*, void*, int, int, const void*,
                                          const void*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_DrawThemeBackgroundEx(void*, void*, int, int, const void*,
                                            const void*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_DrawThemeText(void*, void*, int, int, const u16*, int, u32,
                                    u32, const void*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_GetThemeColor(void*, int, int, int, u32*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_GetThemeInt(void*, int, int, int, int*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_GetThemeBool(void*, int, int, int, int*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_GetThemeMargins(void*, void*, int, int, int, const void*,
                                      void*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI uxt_GetThemePartSize(void*, void*, int, int, const void*, int,
                                       void*) {
  return E_NOTIMPL_;
}
u32 TRT_GUEST_ABI uxt_GetThemeSysColor(void*, int) { return 0; }
int TRT_GUEST_ABI uxt_IsThemePartDefined(void*, int, int) { return 0; }
int TRT_GUEST_ABI uxt_IsThemeBackgroundPartiallyTransparent(void*, int, int) {
  return 0;
}
i32 TRT_GUEST_ABI uxt_EnableThemeDialogTexture(void*, u32) { return E_NOTIMPL_; }
i32 TRT_GUEST_ABI uxt_BufferedPaintInit(void) { return S_OK_; }
i32 TRT_GUEST_ABI uxt_BufferedPaintUnInit(void) { return S_OK_; }
void* TRT_GUEST_ABI uxt_BeginBufferedPaint(void*, const void*, int, void*,
                                           void** hdc_out) {
  if (hdc_out) *hdc_out = nullptr;
  return nullptr;
}
i32 TRT_GUEST_ABI uxt_EndBufferedPaint(void*, int) { return E_INVALIDARG_; }
i32 TRT_GUEST_ABI uxt_BufferedPaintSetAlpha(void*, const void*, u8) {
  return E_INVALIDARG_;
}
i32 TRT_GUEST_ABI uxt_GetCurrentThemeName(u16* name, int name_chars,
                                          u16* color, int color_chars,
                                          u16* size, int size_chars) {
  if (name && name_chars > 0) name[0] = 0;
  if (color && color_chars > 0) color[0] = 0;
  if (size && size_chars > 0) size[0] = 0;
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI uxt_ShouldAppsUseDarkMode(void) { return 1; }
int TRT_GUEST_ABI uxt_AllowDarkModeForWindow(void*, int) { return 1; }
int TRT_GUEST_ABI uxt_SetPreferredAppMode(int) { return 1; }

// ---- 10. msimg32.dll — real blended blits on the GDI raster surface -------
//
// AlphaBlend / TransparentBlt / GradientFill used to be named no-ops ("headless
// GDI surface"). The surface now holds real pixels, so they composite:
//  - AlphaBlend: AC_SRC_OVER with SourceConstantAlpha (per-pixel AC_SRC_ALPHA
//    needs an alpha channel the XRGB surface does not carry; named below).
//  - TransparentBlt: color-keyed stretch blit.
//  - GradientFill: GRADIENT_FILL_RECT_H/V between two TRIVERTEX stops;
//    GRADIENT_FILL_TRIANGLE stays a named FALSE (no triangle rasterizer).
namespace {

struct trivertex_ {
  u32 x, y;
  u16 red, green, blue, alpha;
};

struct blendfunction_ {
  u8 blend_op, blend_flags, source_constant_alpha, alpha_format;
};

inline u32 argb_of_trivertex(const trivertex_* v) {
  return ((u32)(v->alpha >> 8) << 24) | ((u32)(v->red >> 8) << 16) |
         ((u32)(v->green >> 8) << 8) | (u32)(v->blue >> 8);
}

}  // namespace

int TRT_GUEST_ABI msimg_AlphaBlend(void* hdc_dst, int x, int y, int w, int h,
                                   void* hdc_src, int sx, int sy, int sw,
                                   int sh, u32 blendfunc) {
  blendfunction_ bf;
  memcpy(&bf, &blendfunc, sizeof(bf));
  if (bf.blend_op != 0) {  // AC_SRC_OVER is the only defined op
    trt_core_log("[msimg32] AlphaBlend -> FALSE (BlendOp != AC_SRC_OVER)");
    trt_SetLastError(87);
    return 0;
  }
  if (bf.alpha_format == 1) {
    // Per-pixel alpha: the raster surface is XRGB, so there is nothing to read.
    trt_core_log(
        "[msimg32] AlphaBlend AC_SRC_ALPHA: surface has no alpha channel, "
        "blending with SourceConstantAlpha instead");
  }
  const u8 alpha = bf.source_constant_alpha;
  if (trt_raster::alpha_blend(hdc_dst, x, y, w, h, hdc_src, sx, sy, sw, sh,
                              alpha)) {
    return 1;
  }
  trt_core_log("[msimg32] AlphaBlend -> FALSE (no raster surface for a DC)");
  trt_SetLastError(87);
  return 0;
}

int TRT_GUEST_ABI msimg_TransparentBlt(void* hdc_dst, int x, int y, int w,
                                       int h, void* hdc_src, int sx, int sy,
                                       int sw, int sh, u32 transparent) {
  if (trt_raster::key_blit(hdc_dst, x, y, w, h, hdc_src, sx, sy, sw, sh,
                           transparent)) {
    return 1;
  }
  trt_core_log(
      "[msimg32] TransparentBlt -> FALSE (no raster surface for a DC)");
  trt_SetLastError(87);
  return 0;
}

int TRT_GUEST_ABI msimg_GradientFill(void* hdc, void* vertex, u32 n_vertex,
                                     void* mesh, u32 n_mesh, u32 mode) {
  constexpr u32 kRectV = 1, kTriangle = 2;
  if (!hdc || !vertex || n_vertex < 2 || !mesh || n_mesh < 1) {
    trt_core_log("[msimg32] GradientFill -> FALSE (bad arguments)");
    trt_SetLastError(87);
    return 0;
  }
  if (mode == kTriangle) {
    trt_core_log("[msimg32] GradientFill GRADIENT_FILL_TRIANGLE named (no triangle mesh rasterizer)");
    trt_SetLastError(87);
    return 0;
  }
  const trivertex_* v = (const trivertex_*)vertex;
  const u32* rects = (const u32*)mesh;  // UpperLeft, LowerRight vertex indices
  for (u32 i = 0; i < n_mesh; ++i) {
    const u32 ul = rects[i * 2], lr = rects[i * 2 + 1];
    if (ul >= n_vertex || lr >= n_vertex) continue;
    const u32 c0 = argb_of_trivertex(&v[ul]);
    const u32 c1 = argb_of_trivertex(&v[lr]);
    const i32 x0 = (i32)v[ul].x, y0 = (i32)v[ul].y;
    const i32 x1 = (i32)v[lr].x, y1 = (i32)v[lr].y;
    // TRIVERTEX.Alpha is often left 0 by callers that mean "opaque" (the
    // classic two-stop gradient); honour that instead of drawing nothing.
    const u32 a0 = c0 | ((c0 >> 24) == 0 ? 0xFF000000u : 0u);
    const u32 a1 = c1 | ((c1 >> 24) == 0 ? 0xFF000000u : 0u);
    trt_raster::gradient(hdc, x0, y0, x1, y1, a0, a1, mode == kRectV);
  }
  return 1;
}

// ---- 11. secur32.dll / sspicli.dll — honest SSPI/LSA + GetUserNameEx -------

constexpr i32 SEC_E_UNSUPPORTED_ = (i32)0x80090302u;
constexpr i32 STATUS_ACCESS_DENIED_ = (i32)0xC0000022u;

u8 TRT_GUEST_ABI sec_GetUserNameExW(int, u16* buf, u32* len_io) {
  if (!len_io) {
    trt_SetLastError(87);
    return 0;
  }
  const char* name = "tiramisu";
  const u32 chars_no_null = (u32)strlen(name);
  const u32 need = chars_no_null + 1u;
  if (!buf || *len_io < need) {
    *len_io = need;
    trt_SetLastError(234);  // ERROR_MORE_DATA
    return 0;
  }
  write_u16_str(name, buf, *len_io);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetUserNameExW -> buf=%p value=\"%.128s\"", (void*)buf, name); trt_core_log(st_line); }
  *len_io = chars_no_null;
  return 1;
}

u8 TRT_GUEST_ABI sec_GetUserNameExA(int, char* buf, u32* len_io) {
  if (!len_io) {
    trt_SetLastError(87);
    return 0;
  }
  const char* name = "tiramisu";
  const u32 chars_no_null = (u32)strlen(name);
  const u32 need = chars_no_null + 1u;
  if (!buf || *len_io < need) {
    *len_io = need;
    trt_SetLastError(234);  // ERROR_MORE_DATA
    return 0;
  }
  memcpy(buf, name, need);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetUserNameExA -> buf=%p value=\"%.128s\"", (void*)buf, name); trt_core_log(st_line); }
  *len_io = chars_no_null;
  return 1;
}

i32 TRT_GUEST_ABI sec_EnumerateSecurityPackages(u32* count, void** pkgs) {
  if (count) *count = 0;
  if (pkgs) *pkgs = nullptr;
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_FreeContextBuffer(void*) { return 0; }

i32 TRT_GUEST_ABI sec_AcquireCredentialsHandle(void*, void*, u32, void*, void*,
                                               void*, void*, void*, void*) {
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_InitializeSecurityContext(void*, void*, void*, u32, u32,
                                                u32, void*, u32, void*, void*,
                                                u32*, void*) {
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_AcceptSecurityContext(void*, void*, void*, u32, u32,
                                            void*, void*, u32*, void*) {
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_FreeCredOrSecHandle(void*) { return 0; }

i32 TRT_GUEST_ABI sec_QuerySecurityPackageInfo(void*, void** pkg) {
  if (pkg) *pkg = nullptr;
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_QueryContextAttributes(void*, u32, void*) {
  return SEC_E_UNSUPPORTED_;
}

i32 TRT_GUEST_ABI sec_CryptoMsgOp(void*, u32, void*, u64) {
  return SEC_E_UNSUPPORTED_;
}

void* TRT_GUEST_ABI sec_InitSecurityInterface(void) { return nullptr; }

i32 TRT_GUEST_ABI sec_LsaConnectUntrusted(void** handle) {
  if (handle) *handle = nullptr;
  return STATUS_ACCESS_DENIED_;
}

i32 TRT_GUEST_ABI sec_LsaDeregisterLogonProcess(void*) { return 0; }

i32 TRT_GUEST_ABI sec_LsaLookupAuthenticationPackage(void*, void*, u32* pkg) {
  if (pkg) *pkg = 0;
  return STATUS_ACCESS_DENIED_;
}

i32 TRT_GUEST_ABI sec_LsaCallAuthenticationPackage(void*, u32, void*, u32,
                                                   void** ret_buf, u32* ret_len,
                                                   i32* sub_status) {
  if (ret_buf) *ret_buf = nullptr;
  if (ret_len) *ret_len = 0;
  if (sub_status) *sub_status = STATUS_ACCESS_DENIED_;
  return STATUS_ACCESS_DENIED_;
}

i32 TRT_GUEST_ABI sec_LsaFreeReturnBuffer(void*) { return 0; }

// ---- 12. netapi32.dll — honest LAN/domain enumeration unavailable ----------

u32 TRT_GUEST_ABI net_NetApiBufferAllocate(u32 bytes, void** buf) {
  if (!buf) return 87u;
  *buf = malloc(bytes ? bytes : 1u);
  if (*buf && bytes) memset(*buf, 0, bytes);
  return *buf ? 0u : 8u;
}

u32 TRT_GUEST_ABI net_NetApiBufferFree(void* buf) {
  free(buf);
  return 0u;
}

u32 TRT_GUEST_ABI net_NetApiBufferReallocate(void* old_buf, u32 new_bytes,
                                             void** new_buf) {
  if (!new_buf) return 87u;
  void* p = realloc(old_buf, new_bytes ? new_bytes : 1u);
  *new_buf = p;
  return p ? 0u : 8u;
}

u32 TRT_GUEST_ABI net_NetApiBufferSize(void* buf, u32* bytes) {
  if (!buf || !bytes) return 87u;
  *bytes = 0u;
  return 0u;
}

u32 TRT_GUEST_ABI net_NetGetInfoUnsupported4(void*, u32, void** out) {
  if (out) *out = nullptr;
  return 50u;  // ERROR_NOT_SUPPORTED
}

u32 TRT_GUEST_ABI net_NetGetInfoUnsupported3(void*, void*, void** out) {
  if (out) *out = nullptr;
  return 50u;
}

u32 TRT_GUEST_ABI net_NetShareEnum(void*, u32, void** buf, u32, u32* read,
                                   u32* total, u32*) {
  if (buf) *buf = nullptr;
  if (read) *read = 0;
  if (total) *total = 0;
  return 50u;
}

u8 TRT_GUEST_ABI net_Netbios(void*) { return 0x05u; }

// ---- 13. dnsapi.dll — honest DNS_ERROR_RCODE_NAME_ERROR without raw DNS ----

i32 TRT_GUEST_ABI dns_DnsQuery(const void*, u16, u32, void*, void** res,
                               void*) {
  if (res) *res = nullptr;
  return 9003;  // DNS_ERROR_RCODE_NAME_ERROR
}

void TRT_GUEST_ABI dns_DnsRecordListFree(void*, int) {}

i32 TRT_GUEST_ABI dns_DnsValidateName(const void*, int) { return 0; }

// ---- 14. wintrust.dll — honest TRUST_E_NOSIGNATURE -------------------------

constexpr i32 TRUST_E_NOSIGNATURE_ = (i32)0x800B0100u;

i32 TRT_GUEST_ABI wt_WinVerifyTrust(void*, void*, void*) {
  if (!g_wintrust_logged.exchange(true, std::memory_order_relaxed)) {
    trt_core_log("[wintrust] WinVerifyTrust -> TRUST_E_NOSIGNATURE (0x800B0100)");
  }
  return TRUST_E_NOSIGNATURE_;
}

int TRT_GUEST_ABI wt_CryptCATFail(void*) {
  trt_SetLastError(120);
  return 0;
}

int TRT_GUEST_ABI wt_CryptCATRelease(void*) { return 1; }

void* TRT_GUEST_ABI wt_HelperNull(void*) { return nullptr; }

// ---- 15. mpr.dll — honest ERROR_NOT_CONNECTED for network drives -----------

u32 TRT_GUEST_ABI mpr_NotConnected3(void*, void*, u32*) {
  return 2250u;  // ERROR_NOT_CONNECTED
}

u32 TRT_GUEST_ABI mpr_NotConnected4(void*, u32, void*, u32*) {
  return 2250u;
}

u32 TRT_GUEST_ABI mpr_NotConnectedOpen(u32, u32, u32, void*, void** h) {
  if (h) *h = nullptr;
  return 2250u;
}

u32 TRT_GUEST_ABI mpr_NotConnectedEnum(void*, u32* count, void*, u32*) {
  if (count) *count = 0;
  return 2250u;
}

u32 TRT_GUEST_ABI mpr_CloseEnum(void*) { return 0u; }

u32 TRT_GUEST_ABI mpr_NotConnected(void*) { return 2250u; }

// ---- 16. oleacc.dll — honest MSAA unavailable (E_NOINTERFACE / E_NOTIMPL) --

i32 TRT_GUEST_ABI acc_AccessibleObjectFromWindow(void*, u32, const void*,
                                                 void** ppv) {
  if (ppv) *ppv = nullptr;
  return E_NOINTERFACE_;
}

i32 TRT_GUEST_ABI acc_AccessibleObjectFromPoint(i64, void** ppv, void*) {
  if (ppv) *ppv = nullptr;
  return E_NOTIMPL_;
}

i32 TRT_GUEST_ABI acc_AccessibleObjectFromEvent(void*, u32, u32, void** ppv,
                                                void*) {
  if (ppv) *ppv = nullptr;
  return E_NOTIMPL_;
}

i32 TRT_GUEST_ABI acc_AccessibleChildren(void*, i32, i32, void*, i32* obtained) {
  if (obtained) *obtained = 0;
  return E_NOTIMPL_;
}

i32 TRT_GUEST_ABI acc_CreateStdAccessibleObject(void*, i32, const void*,
                                               void** ppv) {
  if (ppv) *ppv = nullptr;
  return E_NOTIMPL_;
}

i64 TRT_GUEST_ABI acc_LresultFromObject(const void*, u64, void*) { return 0; }

i32 TRT_GUEST_ABI acc_ObjectFromLresult(i64, const void*, u64, void** ppv) {
  if (ppv) *ppv = nullptr;
  return E_NOINTERFACE_;
}

u32 TRT_GUEST_ABI acc_GetTextW(u32, u16* buf, u32 cap) {
  if (buf && cap > 0) buf[0] = 0;
  return 0;
}

u32 TRT_GUEST_ABI acc_GetTextA(u32, char* buf, u32 cap) {
  if (buf && cap > 0) buf[0] = 0;
  return 0;
}

// ---- 17. dhcpcsvc.dll — honest DHCP param unsupported ----------------------

u32 TRT_GUEST_ABI dhcp_DhcpCApiInitialize(u32* version) {
  if (version) *version = 2u;
  return 0u;
}

void TRT_GUEST_ABI dhcp_DhcpCApiCleanup(void) {}

u32 TRT_GUEST_ABI dhcp_DhcpUnsupported(void*) { return 50u; }

// ---- 18. normaliz.dll — RFC 3492 IDNA/Punycode + Unicode normalization -----

size_t u16_len_bounded(const u16* s, i32 n) {
  if (!s) return 0;
  if (n >= 0) return (size_t)n;
  size_t k = 0;
  while (s[k]) ++k;
  return k;
}

i32 write_u16_result(const std::vector<u16>& out, bool add_nul, u16* dst,
                     i32 dst_cap) {
  const size_t total = out.size() + (add_nul ? 1u : 0u);
  if (!dst || dst_cap == 0) {
    trt_SetLastError(0);
    return (i32)total;
  }
  if (dst_cap < 0 || (size_t)dst_cap < total) {
    trt_SetLastError(122u /*ERROR_INSUFFICIENT_BUFFER*/);
    return 0;
  }
  for (size_t i = 0; i < out.size(); ++i) dst[i] = out[i];
  if (add_nul) dst[out.size()] = 0;
  trt_SetLastError(0);
  return (i32)total;
}

u32 puny_adapt(u32 delta, u32 numpoints, bool firsttime) {
  delta = firsttime ? (delta / 700u) : (delta >> 1);
  delta += delta / numpoints;
  u32 k = 0;
  while (delta > ((36u - 1u) * 26u) / 2u) {
    delta /= (36u - 1u);
    k += 36u;
  }
  return k + (((36u - 1u + 1u) * delta) / (delta + 38u));
}

u16 puny_encode_digit(u32 d) {
  return (u16)(d < 26u ? ('a' + d) : ('0' + (d - 26u)));
}

int puny_decode_digit(u16 c) {
  if (c >= '0' && c <= '9') return (int)(c - '0' + 26);
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int)(c - 'a');
  return -1;
}

bool puny_encode_label(const u32* cps, size_t n_cp, std::vector<u16>* out) {
  bool all_ascii = true;
  for (size_t i = 0; i < n_cp; ++i) {
    if (cps[i] >= 0x80u) { all_ascii = false; break; }
  }
  if (all_ascii) {
    for (size_t i = 0; i < n_cp; ++i) out->push_back((u16)cps[i]);
    return true;
  }
  out->push_back('x'); out->push_back('n');
  out->push_back('-'); out->push_back('-');
  u32 b = 0;
  for (size_t i = 0; i < n_cp; ++i) {
    if (cps[i] < 0x80u) {
      out->push_back((u16)cps[i]);
      ++b;
    }
  }
  if (b > 0) out->push_back('-');
  u32 h = b;
  u32 n = 128u;
  u32 delta = 0;
  u32 bias = 72u;
  while (h < (u32)n_cp) {
    u32 m = 0xFFFFFFFFu;
    for (size_t i = 0; i < n_cp; ++i) {
      if (cps[i] >= n && cps[i] < m) m = cps[i];
    }
    if ((m - n) > (0x7FFFFFFFu - delta) / (h + 1u)) return false;
    delta += (m - n) * (h + 1u);
    n = m;
    for (size_t i = 0; i < n_cp; ++i) {
      if (cps[i] < n) {
        if (++delta == 0) return false;
      } else if (cps[i] == n) {
        u32 q = delta;
        for (u32 k = 36u;; k += 36u) {
          u32 t = (k <= bias) ? 1u : (k >= bias + 26u ? 26u : k - bias);
          if (q < t) break;
          out->push_back(puny_encode_digit(t + ((q - t) % (36u - t))));
          q = (q - t) / (36u - t);
        }
        out->push_back(puny_encode_digit(q));
        bias = puny_adapt(delta, h + 1u, h == b);
        delta = 0;
        ++h;
      }
    }
    ++delta;
    ++n;
  }
  return true;
}

bool puny_decode_label(const u16* in, size_t n_in, std::vector<u32>* out) {
  if (n_in >= 4 && (in[0] == 'x' || in[0] == 'X') &&
      (in[1] == 'n' || in[1] == 'N') && in[2] == '-' && in[3] == '-') {
    size_t delim = 0;
    for (size_t j = 4; j < n_in; ++j) {
      if (in[j] == '-') delim = j;
    }
    size_t pos = 4;
    if (delim > 4) {
      for (; pos < delim; ++pos) out->push_back((u32)in[pos]);
      ++pos;
    }
    u32 n = 128u;
    u32 i = 0;
    u32 bias = 72u;
    while (pos < n_in) {
      u32 oldi = i;
      u32 w = 1u;
      for (u32 k = 36u;; k += 36u) {
        if (pos >= n_in) return false;
        int d = puny_decode_digit(in[pos++]);
        if (d < 0) return false;
        i += (u32)d * w;
        u32 t = (k <= bias) ? 1u : (k >= bias + 26u ? 26u : k - bias);
        if ((u32)d < t) break;
        w *= (36u - t);
      }
      u32 len = (u32)out->size() + 1u;
      bias = puny_adapt(i - oldi, len, oldi == 0);
      n += i / len;
      i %= len;
      out->insert(out->begin() + (ptrdiff_t)i, n);
      ++i;
    }
    return true;
  }
  for (size_t k = 0; k < n_in; ++k) out->push_back((u32)in[k]);
  return true;
}

i32 TRT_GUEST_ABI norm_IdnToAscii(u32 dwFlags, const u16* lpSrc, i32 cchSrc,
                                  u16* lpDst, i32 cchDst) {
  (void)dwFlags;
  if (!lpSrc || cchSrc == 0 || cchDst < 0) {
    trt_SetLastError(87u);
    return 0;
  }
  const size_t n = u16_len_bounded(lpSrc, cchSrc);
  std::vector<u16> out;
  out.reserve(n + 16);
  size_t i = 0;
  while (i <= n) {
    std::vector<u32> label;
    while (i < n) {
      u16 cu = lpSrc[i++];
      if (cu == '.' || cu == 0x3002u || cu == 0xFF0Eu || cu == 0xFF61u) break;
      u32 cp = cu;
      if (cu >= 0xD800u && cu <= 0xDBFFu && i < n &&
          lpSrc[i] >= 0xDC00u && lpSrc[i] <= 0xDFFFu) {
        cp = 0x10000u + (((u32)(cu - 0xD800u) << 10) | (u32)(lpSrc[i++] - 0xDC00u));
      }
      if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');
      label.push_back(cp);
    }
    if (!puny_encode_label(label.data(), label.size(), &out)) {
      trt_SetLastError(87u);
      return 0;
    }
    if (i <= n && i > 0 &&
        (lpSrc[i - 1] == '.' || lpSrc[i - 1] == 0x3002u ||
         lpSrc[i - 1] == 0xFF0Eu || lpSrc[i - 1] == 0xFF61u)) {
      out.push_back('.');
      if (i == n) break;
    } else {
      break;
    }
  }
  return write_u16_result(out, cchSrc < 0, lpDst, cchDst);
}

i32 TRT_GUEST_ABI norm_IdnToUnicode(u32 dwFlags, const u16* lpSrc, i32 cchSrc,
                                    u16* lpDst, i32 cchDst) {
  (void)dwFlags;
  if (!lpSrc || cchSrc == 0 || cchDst < 0) {
    trt_SetLastError(87u);
    return 0;
  }
  const size_t n = u16_len_bounded(lpSrc, cchSrc);
  std::vector<u16> out;
  out.reserve(n);
  size_t i = 0;
  while (i <= n) {
    size_t start = i;
    while (i < n && lpSrc[i] != '.') ++i;
    std::vector<u32> cps;
    if (!puny_decode_label(lpSrc + start, i - start, &cps)) {
      trt_SetLastError(87u);
      return 0;
    }
    for (u32 cp : cps) {
      if (cp <= 0xFFFFu) {
        out.push_back((u16)cp);
      } else {
        u32 v = cp - 0x10000u;
        out.push_back((u16)(0xD800u + (v >> 10)));
        out.push_back((u16)(0xDC00u + (v & 0x3FFu)));
      }
    }
    if (i < n && lpSrc[i] == '.') {
      out.push_back('.');
      ++i;
      if (i == n) break;
    } else {
      break;
    }
  }
  return write_u16_result(out, cchSrc < 0, lpDst, cchDst);
}

i32 TRT_GUEST_ABI norm_IdnToNameprepUnicode(u32 flags, const u16* src,
                                            i32 cchSrc, u16* dst, i32 cchDst) {
  (void)flags;
  if (!src || cchSrc == 0 || cchDst < 0) {
    trt_SetLastError(87u);
    return 0;
  }
  const size_t n = u16_len_bounded(src, cchSrc);
  std::vector<u16> out(n);
  for (size_t i = 0; i < n; ++i) {
    u16 c = src[i];
    if (c >= 'A' && c <= 'Z') c = (u16)(c + ('a' - 'A'));
    out[i] = c;
  }
  return write_u16_result(out, cchSrc < 0, dst, cchDst);
}

bool valid_norm_form(i32 f) {
  return f == 1 || f == 2 || f == 5 || f == 6;
}

void normalize_run(i32 form, const u16* src, size_t n, std::vector<u16>* out) {
  const bool compose = (form == 1 || form == 5);
  out->reserve(n);
  struct latin_pair { u16 pre; u16 base; u16 comb; };
  static const latin_pair k_pairs[] = {
      {0x00C0, 'A', 0x0300}, {0x00C1, 'A', 0x0301}, {0x00C4, 'A', 0x0308},
      {0x00C8, 'E', 0x0300}, {0x00C9, 'E', 0x0301}, {0x00D1, 'N', 0x0303},
      {0x00D3, 'O', 0x0301}, {0x00D6, 'O', 0x0308}, {0x00DA, 'U', 0x0301},
      {0x00DC, 'U', 0x0308}, {0x00E0, 'a', 0x0300}, {0x00E1, 'a', 0x0301},
      {0x00E4, 'a', 0x0308}, {0x00E7, 'c', 0x0327}, {0x00E8, 'e', 0x0300},
      {0x00E9, 'e', 0x0301}, {0x00EB, 'e', 0x0308}, {0x00ED, 'i', 0x0301},
      {0x00F1, 'n', 0x0303}, {0x00F3, 'o', 0x0301}, {0x00F6, 'o', 0x0308},
      {0x00FA, 'u', 0x0301}, {0x00FC, 'u', 0x0308},
  };
  for (size_t i = 0; i < n; ++i) {
    u16 cu = src[i];
    if (compose && i + 1 < n) {
      bool matched = false;
      for (const latin_pair& p : k_pairs) {
        if (p.base == cu && p.comb == src[i + 1]) {
          out->push_back(p.pre);
          ++i;
          matched = true;
          break;
        }
      }
      if (matched) continue;
    } else if (!compose) {
      bool matched = false;
      for (const latin_pair& p : k_pairs) {
        if (p.pre == cu) {
          out->push_back(p.base);
          out->push_back(p.comb);
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    out->push_back(cu);
  }
}

i32 TRT_GUEST_ABI norm_NormalizeString(i32 norm_form, const u16* src, i32 src_len,
                                       u16* dst, i32 dst_len) {
  if (!valid_norm_form(norm_form) || !src || dst_len < 0) {
    trt_SetLastError(87u);
    return 0;
  }
  const size_t n = u16_len_bounded(src, src_len);
  std::vector<u16> out;
  normalize_run(norm_form, src, n, &out);
  return write_u16_result(out, src_len < 0, dst, dst_len);
}

i32 TRT_GUEST_ABI norm_IsNormalizedString(i32 norm_form, const u16* src,
                                          i32 src_len) {
  if (!valid_norm_form(norm_form) || !src) {
    trt_SetLastError(87u);
    return 0;
  }
  const size_t n = u16_len_bounded(src, src_len);
  std::vector<u16> out;
  normalize_run(norm_form, src, n, &out);
  if (out.size() != n) return 0;
  for (size_t i = 0; i < n; ++i) {
    if (out[i] != src[i]) return 0;
  }
  return 1;
}

// ---- 19. sechost.dll — SCM / ETW / SID / LSA forwarding --------------------

std::atomic<u64> g_se_etw_cookie{0x53450001ULL};
std::atomic<bool> g_sc_logged{false};

void* TRT_GUEST_ABI sc_OpenAccessDenied() {
  if (!g_sc_logged.exchange(true, std::memory_order_relaxed)) {
    trt_core_log("[sechost] OpenSCManager/Service -> NULL (ERROR_ACCESS_DENIED)");
  }
  trt_SetLastError(5u);
  return nullptr;
}

i32 TRT_GUEST_ABI sc_CloseServiceHandle(void*) { return 1; }

i32 TRT_GUEST_ABI sc_FailAccessDenied() {
  trt_SetLastError(5u);
  return 0;
}

u32 TRT_GUEST_ABI sc_NotifyUnsupported() { return 50u; }

void* TRT_GUEST_ABI sc_RegisterServiceCtrlHandler(const void*, void*, void*) {
  return (void*)(uintptr_t)0x53430001ULL;
}

u32 TRT_GUEST_ABI se_EventRegister(const void*, void*, void*, u64* h) {
  if (h) *h = g_se_etw_cookie.fetch_add(1u, std::memory_order_relaxed);
  return 0u;
}

u32 TRT_GUEST_ABI se_EventReturnZero() { return 0u; }
u8 TRT_GUEST_ABI se_EventEnabledFalse() { return 0u; }

i32 TRT_GUEST_ABI se_ConvertSidToStringSidW(const void* sid, u16** out) {
  typedef i32 (TRT_GUEST_ABI* pfn)(const void*, u16**);
  if (auto fn = (pfn)trt_resolve_module(nullptr, "advapi32.dll",
                                        "ConvertSidToStringSidW", 0)) {
    return fn(sid, out);
  }
  if (out) *out = nullptr;
  trt_SetLastError(87u);
  return 0;
}

i32 TRT_GUEST_ABI se_ConvertStringSidToSidW(const u16* s, void** out) {
  if (out) *out = nullptr;
  if (!s || (s[0] != 'S' && s[0] != 's') || s[1] != '-' || s[2] != '1' ||
      s[3] != '-') {
    trt_SetLastError(1337u /*ERROR_INVALID_SID*/);
    return 0;
  }
  size_t pos = 4;
  u64 auth = 0;
  while (s[pos] && s[pos] != '-') {
    if (s[pos] < '0' || s[pos] > '9') {
      trt_SetLastError(1337u);
      return 0;
    }
    auth = auth * 10u + (u64)(s[pos++] - '0');
  }
  std::vector<u32> subs;
  while (s[pos] == '-') {
    ++pos;
    if (!s[pos] || s[pos] < '0' || s[pos] > '9') {
      trt_SetLastError(1337u);
      return 0;
    }
    u32 sub = 0;
    while (s[pos] >= '0' && s[pos] <= '9') {
      sub = sub * 10u + (u32)(s[pos++] - '0');
    }
    subs.push_back(sub);
    if (subs.size() > 15u) { trt_SetLastError(1337u); return 0; }
  }
  if (s[pos] != 0) { trt_SetLastError(1337u); return 0; }
  u8* mem = (u8*)trt_LocalAlloc(0x40u, 8u + (u64)subs.size() * 4u);
  if (!mem) { trt_SetLastError(8u); return 0; }
  mem[0] = 1u;
  mem[1] = (u8)subs.size();
  for (int b = 0; b < 6; ++b) {
    mem[2 + b] = (u8)((auth >> ((5 - b) * 8)) & 0xFFu);
  }
  for (size_t j = 0; j < subs.size(); ++j) {
    memcpy(mem + 8 + j * 4, &subs[j], 4);
  }
  *out = mem;
  trt_SetLastError(0);
  return 1;
}

i32 TRT_GUEST_ABI se_ConvertStringSidToSidA(const char* s, void** out) {
  if (!s) { trt_SetLastError(1337u); return 0; }
  u16 wide[256];
  write_u16_str(s, wide, 256);
  return se_ConvertStringSidToSidW(wide, out);
}

i32 TRT_GUEST_ABI se_LookupNoneMapped() {
  trt_SetLastError(1332u /*ERROR_NONE_MAPPED*/);
  return 0;
}

i32 TRT_GUEST_ABI se_LsaAccessDenied(void*, void*, void*, u32, void**) {
  return (i32)0xC0000022u;  // STATUS_ACCESS_DENIED
}

i32 TRT_GUEST_ABI se_LsaOpenPolicy(void*, void*, u32, void** out) {
  if (out) *out = (void*)(uintptr_t)0x4C530001ULL;
  return 0;
}

i32 TRT_GUEST_ABI se_LsaClose(void*) { return 0; }
i32 TRT_GUEST_ABI se_LsaFreeMemory(void* p) { free(p); return 0; }

i32 TRT_GUEST_ABI se_LsaQueryInfoUnsupported(void*, u32, void** buf) {
  if (buf) *buf = nullptr;
  return (i32)0xC0000022u;
}

// ---- 20. ncrypt.dll — CNG key storage provider & honest no-key responses ---

struct ncrypt_prov_slot { bool used = false; };
std::mutex g_nc_mu;
ncrypt_prov_slot g_nc_provs[32];

bool valid_nc_prov(u64 h, int* idx_out = nullptr) {
  if (h < 0x4E430001ULL || h >= 0x4E430021ULL) return false;
  int idx = (int)(h - 0x4E430001ULL);
  if (!g_nc_provs[idx].used) return false;
  if (idx_out) *idx_out = idx;
  return true;
}

i32 TRT_GUEST_ABI nc_OpenStorageProvider(u64* phProv, const u16* name, u32) {
  (void)name;
  if (!phProv) return (i32)0x80090027u;  // NTE_INVALID_PARAMETER
  std::lock_guard<std::mutex> lk(g_nc_mu);
  for (int i = 0; i < 32; ++i) {
    if (!g_nc_provs[i].used) {
      g_nc_provs[i].used = true;
      *phProv = 0x4E430001ULL + (u64)i;
      return 0;
    }
  }
  return (i32)0x8009000Eu;  // NTE_NO_MEMORY
}

i32 TRT_GUEST_ABI nc_EnumStorageProviders(u32* count, void** list_out, u32) {
  if (!count || !list_out) return (i32)0x80090027u;
  static const char k_name[] = "Microsoft Software Key Storage Provider";
  static const char k_cmt[] = "Software Key Storage Provider";
  const size_t n1 = sizeof(k_name);
  const size_t n2 = sizeof(k_cmt);
  // NCryptProviderName = { LPWSTR pszName; LPWSTR pszComment; } (16 bytes)
  u8* blk = (u8*)malloc(16u + (n1 + n2) * 2u);
  if (!blk) return (i32)0x8009000Eu;
  u16* s1 = (u16*)(blk + 16u);
  u16* s2 = s1 + n1;
  write_u16_str(k_name, s1, (u32)n1);
  write_u16_str(k_cmt, s2, (u32)n2);
  memcpy(blk, &s1, 8);
  memcpy(blk + 8, &s2, 8);
  *count = 1u;
  *list_out = blk;
  return 0;
}

i32 TRT_GUEST_ABI nc_FreeBuffer(void* pv) {
  free(pv);
  return 0;
}

i32 TRT_GUEST_ABI nc_FreeObject(u64 h) {
  std::lock_guard<std::mutex> lk(g_nc_mu);
  int idx = 0;
  if (valid_nc_prov(h, &idx)) {
    g_nc_provs[idx].used = false;
    return 0;
  }
  return (i32)0x80090026u;  // NTE_INVALID_HANDLE
}

i32 TRT_GUEST_ABI nc_IsKeyHandle(u64) { return 0; }

i32 TRT_GUEST_ABI nc_NotFound(u64 h, void** out, const void*, u32, u32) {
  std::lock_guard<std::mutex> lk(g_nc_mu);
  if (out) *out = nullptr;
  return valid_nc_prov(h) ? (i32)0x80090011u /*NTE_NOT_FOUND*/
                          : (i32)0x80090026u /*NTE_INVALID_HANDLE*/;
}

i32 TRT_GUEST_ABI nc_NotSupported() {
  return (i32)0x80090029u;  // NTE_NOT_SUPPORTED
}

// ---- 21. usp10.dll — minimal Unicode shaping / glyph layout pipeline -------

struct usp_script_props {
  u32 word0;
  u32 word1;
};

struct usp_font_props {
  i32 cBytes;
  u16 wgBlank;
  u16 wgDefault;
  u16 wgInvalid;
  u16 wgKashida;
  i32 iKashidaWidth;
};

struct usp_script_item {
  i32 iCharPos;
  u16 analysis_word;
  u16 state_word;
};

struct usp_visattr {
  u16 flags;
};

struct usp_abc {
  i32 abcA;
  u32 abcB;
  i32 abcC;
};

struct usp_goffset {
  i32 du;
  i32 dv;
};

struct usp_string_state {
  i32 cchars;
  i32 width;
  i32 height;
  std::vector<u8> logattr;
  std::vector<u32> order;
  std::vector<i32> adv;
};

const usp_script_props k_usp_props_data[4] = {
    {0x00000009u, 0x00000000u},  // 0: Undefined/Default
    {0x00000409u, 0x00000000u},  // 1: Latin
    {0x00000009u | (1u << 16), 0x00000000u},  // 2: Numeric
    {0x00000009u, 0x00000001u},  // 3: Control
};
const usp_script_props* const k_usp_props_ptrs[4] = {
    &k_usp_props_data[0], &k_usp_props_data[1],
    &k_usp_props_data[2], &k_usp_props_data[3],
};

i32 TRT_GUEST_ABI usp_ScriptFreeCache(void** psc) {
  if (psc) *psc = nullptr;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptItemize(const u16* chars, i32 cInChars, i32 cMaxItems,
                                    const void*, const void*, void* items_v,
                                    i32* pcItems) {
  if (!chars || cInChars < 0 || cMaxItems < 2 || !items_v || !pcItems)
    return E_INVALIDARG_;
  usp_script_item* items = (usp_script_item*)items_v;
  items[0].iCharPos = 0;
  items[0].analysis_word = 1u;  // eScript = 1 (Latin)
  items[0].state_word = 0u;
  items[1].iCharPos = cInChars;
  items[1].analysis_word = 0u;
  items[1].state_word = 0u;
  *pcItems = 1;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptItemizeOpenType(const u16* chars, i32 cInChars,
                                            i32 cMaxItems, const void* pCtrl,
                                            const void* pState, void* items_v,
                                            void*, i32* pcItems) {
  return usp_ScriptItemize(chars, cInChars, cMaxItems, pCtrl, pState, items_v,
                           pcItems);
}

i32 TRT_GUEST_ABI usp_ScriptLayout(i32 cRuns, const u8*, i32* v2l, i32* l2v) {
  if (cRuns < 0) return E_INVALIDARG_;
  for (i32 i = 0; i < cRuns; ++i) {
    if (v2l) v2l[i] = i;
    if (l2v) l2v[i] = i;
  }
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptShape(void*, void**, const u16* chars, i32 cChars,
                                  i32 cMaxGlyphs, void*, u16* glyphs,
                                  u16* logclust, void* sva_v, i32* pcGlyphs) {
  if (!chars || cChars < 0 || !glyphs || !logclust || !sva_v || !pcGlyphs)
    return E_INVALIDARG_;
  if (cMaxGlyphs < cChars) return (i32)0x8007000Eu;  // E_OUTOFMEMORY
  usp_visattr* sva = (usp_visattr*)sva_v;
  for (i32 i = 0; i < cChars; ++i) {
    glyphs[i] = chars[i];
    logclust[i] = (u16)i;
    sva[i].flags = 0x0012u;  // uJustification=2 | fClusterStart=1
  }
  *pcGlyphs = cChars;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptShapeOpenType(void* hdc, void** psc, void* psa, u32,
                                          u32, i32*, void**, i32,
                                          const u16* chars, i32 cChars,
                                          i32 cMaxGlyphs, u16* logclust, void*,
                                          u16* glyphs, void* gprops_v,
                                          i32* pcGlyphs) {
  if (!chars || cChars < 0 || !glyphs || !logclust || !gprops_v || !pcGlyphs)
    return E_INVALIDARG_;
  if (cMaxGlyphs < cChars) return (i32)0x8007000Eu;
  (void)hdc; (void)psc; (void)psa;
  struct gprop { u16 vis; u16 res; };
  gprop* gp = (gprop*)gprops_v;
  for (i32 i = 0; i < cChars; ++i) {
    glyphs[i] = chars[i];
    logclust[i] = (u16)i;
    gp[i].vis = 0x0012u;
    gp[i].res = 0u;
  }
  *pcGlyphs = cChars;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptPlace(void*, void**, const u16*, i32 cGlyphs,
                                  const void*, void*, i32* piAdvance,
                                  void* pGoffset_v, void* pABC_v) {
  if (cGlyphs < 0 || !piAdvance) return E_INVALIDARG_;
  usp_goffset* goff = (usp_goffset*)pGoffset_v;
  for (i32 i = 0; i < cGlyphs; ++i) {
    piAdvance[i] = 8;
    if (goff) { goff[i].du = 0; goff[i].dv = 0; }
  }
  if (pABC_v) {
    usp_abc* abc = (usp_abc*)pABC_v;
    abc->abcA = 0;
    abc->abcB = (u32)(cGlyphs * 8);
    abc->abcC = 0;
  }
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptPlaceOpenType(void* hdc, void** psc, void* psa, u32,
                                          u32, i32*, void**, i32, const u16*,
                                          u16*, void*, i32, const u16* glyphs,
                                          const void*, i32 cGlyphs,
                                          i32* piAdvance, void* pGoffset,
                                          void* pABC) {
  return usp_ScriptPlace(hdc, psc, glyphs, cGlyphs, nullptr, psa, piAdvance,
                         pGoffset, pABC);
}

i32 TRT_GUEST_ABI usp_ScriptBreak(const u16* chars, i32 cChars, const void*,
                                  u8* psla) {
  if (!chars || cChars < 0 || !psla) return E_INVALIDARG_;
  for (i32 i = 0; i < cChars; ++i) {
    u8 attr = 0x04u;  // fCharStop
    if (chars[i] == ' ' || chars[i] == '\t') attr |= 0x03u;  // fSoftBreak|fWhiteSpace
    if (i == 0 || chars[i - 1] == ' ' || chars[i - 1] == '\t') attr |= 0x08u;
    psla[i] = attr;
  }
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptCPtoX(i32 iCP, i32 fTrailing, i32 cChars, i32,
                                  const u16*, const void*, const i32* piAdvance,
                                  const void*, i32* piX) {
  if (!piX || !piAdvance || iCP < 0) return E_INVALIDARG_;
  i32 pos = iCP + (fTrailing ? 1 : 0);
  if (pos > cChars) pos = cChars;
  i32 x = 0;
  for (i32 i = 0; i < pos; ++i) x += piAdvance[i];
  *piX = x;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptXtoCP(i32 iX, i32 cChars, i32, const u16*,
                                  const void*, const i32* piAdvance,
                                  const void*, i32* piCP, i32* piTrailing) {
  if (!piCP || !piTrailing || !piAdvance) return E_INVALIDARG_;
  if (iX <= 0) { *piCP = 0; *piTrailing = 0; return S_OK_; }
  i32 cur = 0;
  for (i32 i = 0; i < cChars; ++i) {
    const i32 adv = piAdvance[i] > 0 ? piAdvance[i] : 8;
    if (iX < cur + adv) {
      *piCP = i;
      *piTrailing = (iX - cur >= adv / 2) ? 1 : 0;
      return S_OK_;
    }
    cur += adv;
  }
  *piCP = (cChars > 0) ? (cChars - 1) : 0;
  *piTrailing = (cChars > 0) ? 1 : 0;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptGetLogicalWidths(const void*, i32 cChars,
                                             i32 cGlyphs, const i32* piGlyphW,
                                             const u16*, const void*, i32* piDx) {
  if (!piGlyphW || !piDx || cChars < 0) return E_INVALIDARG_;
  for (i32 i = 0; i < cChars; ++i) piDx[i] = (i < cGlyphs) ? piGlyphW[i] : 8;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptApplyLogicalWidth(const i32* piDx, i32 cChars,
                                              i32 cGlyphs, const u16*,
                                              const void*, const i32* piAdv,
                                              const void*, void* pABC,
                                              i32* piJustify) {
  if (!piDx || !piJustify || cGlyphs < 0) return E_INVALIDARG_;
  i32 sum = 0;
  for (i32 i = 0; i < cGlyphs; ++i) {
    piJustify[i] = (i < cChars) ? piDx[i] : (piAdv ? piAdv[i] : 8);
    sum += piJustify[i];
  }
  if (pABC) {
    usp_abc* abc = (usp_abc*)pABC;
    abc->abcA = 0;
    abc->abcB = (u32)(sum > 0 ? sum : 0);
    abc->abcC = 0;
  }
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptGetCMap(void*, void**, const u16* in, i32 cChars,
                                    u32, u16* glyphs) {
  if (!in || cChars < 0 || !glyphs) return E_INVALIDARG_;
  for (i32 i = 0; i < cChars; ++i) glyphs[i] = in[i];
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptGetGlyphABCWidth(void*, void**, u16, void* pABC) {
  if (!pABC) return E_INVALIDARG_;
  usp_abc* abc = (usp_abc*)pABC;
  abc->abcA = 0; abc->abcB = 7; abc->abcC = 1;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptGetProperties(const void*** ppSp, i32* piNum) {
  if (!ppSp || !piNum) return E_INVALIDARG_;
  *ppSp = (const void**)k_usp_props_ptrs;
  *piNum = 4;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptGetFontProperties(void*, void**, void* sfp_v) {
  usp_font_props* sfp = (usp_font_props*)sfp_v;
  if (!sfp || sfp->cBytes < (i32)sizeof(usp_font_props)) return E_INVALIDARG_;
  sfp->wgBlank = 0x0020u;
  sfp->wgDefault = 0x003Fu;
  sfp->wgInvalid = 0u;
  sfp->wgKashida = 0u;
  sfp->iKashidaWidth = 0;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptCacheGetHeight(void*, void**, i32* tmHeight) {
  if (!tmHeight) return E_INVALIDARG_;
  *tmHeight = 16;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptIsComplex(const u16*, i32, u32) { return 1; }
i32 TRT_GUEST_ABI usp_ScriptOk() { return S_OK_; }

i32 TRT_GUEST_ABI usp_ScriptStringAnalyse(void*, const void* pString, i32 cString,
                                          i32, i32, u32, i32, void*, void*,
                                          const i32* piDx, void*, const u8*,
                                          void** pssa) {
  if (!pssa || !pString || cString < 0) return E_INVALIDARG_;
  auto* st = new usp_string_state();
  st->cchars = cString;
  st->height = 16;
  st->logattr.resize((size_t)cString, 0x04u);
  st->order.resize((size_t)cString);
  st->adv.resize((size_t)cString);
  i32 w = 0;
  const u16* u = (const u16*)pString;
  for (i32 i = 0; i < cString; ++i) {
    st->order[(size_t)i] = (u32)i;
    st->adv[(size_t)i] = piDx ? piDx[i] : 8;
    if (u[i] == ' ' || u[i] == '\t') st->logattr[(size_t)i] |= 0x03u;
    w += st->adv[(size_t)i];
  }
  st->width = w;
  *pssa = st;
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptStringFree(void** pssa) {
  if (pssa && *pssa) {
    delete (usp_string_state*)(*pssa);
    *pssa = nullptr;
  }
  return S_OK_;
}

const i32* TRT_GUEST_ABI usp_ScriptString_pcOutChars(void* ssa) {
  return ssa ? &((usp_string_state*)ssa)->cchars : nullptr;
}

const u8* TRT_GUEST_ABI usp_ScriptString_pLogAttr(void* ssa) {
  if (!ssa) return nullptr;
  auto* st = (usp_string_state*)ssa;
  return st->logattr.empty() ? nullptr : st->logattr.data();
}

const void* TRT_GUEST_ABI usp_ScriptString_pSize(void* ssa) {
  return ssa ? &((usp_string_state*)ssa)->width : nullptr;
}

i32 TRT_GUEST_ABI usp_ScriptStringGetOrder(void* ssa, u32* puOrder) {
  if (!ssa || !puOrder) return E_INVALIDARG_;
  auto* st = (usp_string_state*)ssa;
  for (size_t i = 0; i < st->order.size(); ++i) puOrder[i] = st->order[i];
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptStringGetLogicalWidths(void* ssa, i32* piDx) {
  if (!ssa || !piDx) return E_INVALIDARG_;
  auto* st = (usp_string_state*)ssa;
  for (size_t i = 0; i < st->adv.size(); ++i) piDx[i] = st->adv[i];
  return S_OK_;
}

i32 TRT_GUEST_ABI usp_ScriptStringCPtoX(void* ssa, i32 icp, i32 fTrailing,
                                        i32* pX) {
  if (!ssa || !pX) return E_INVALIDARG_;
  auto* st = (usp_string_state*)ssa;
  return usp_ScriptCPtoX(icp, fTrailing, st->cchars, st->cchars, nullptr,
                         nullptr, st->adv.data(), nullptr, pX);
}

i32 TRT_GUEST_ABI usp_ScriptStringXtoCP(void* ssa, i32 iX, i32* piCh,
                                        i32* piTrailing) {
  if (!ssa) return E_INVALIDARG_;
  auto* st = (usp_string_state*)ssa;
  return usp_ScriptXtoCP(iX, st->cchars, st->cchars, nullptr, nullptr,
                         st->adv.data(), nullptr, piCh, piTrailing);
}

i32 TRT_GUEST_ABI usp_ScriptStringValidate(void* ssa) {
  return ssa ? S_OK_ : E_INVALIDARG_;
}

// ---- 22. cryptui.dll — honest AppContainer no-UI cancellation --------------

std::atomic<bool> g_cui_logged{false};
i32 TRT_GUEST_ABI cui_DlgCancelled() {
  if (!g_cui_logged.exchange(true, std::memory_order_relaxed)) {
    trt_core_log("[cryptui] UI dialog unavailable in console host -> ERROR_CANCELLED");
  }
  trt_SetLastError(1223u /*ERROR_CANCELLED*/);
  return 0;
}

void* TRT_GUEST_ABI cui_SelectCertNull() {
  trt_SetLastError(1223u);
  return nullptr;
}

i32 TRT_GUEST_ABI cui_WizFree() { return 1; }

// ---- 23. credui.dll — real CredUIParseUserName + honest prompt cancel ------

u32 TRT_GUEST_ABI cred_ParseUserNameW(const u16* name, u16* user, u32 user_cap,
                                      u16* dom, u32 dom_cap) {
  if (!name || !user || !dom) return 87u;
  size_t len = u16_len_bounded(name, -1);
  if (len == 0) return 1315u;  // ERROR_INVALID_ACCOUNT_NAME
  size_t sep = (size_t)-1;
  bool upn = false;
  for (size_t i = 0; i < len; ++i) {
    if (name[i] == '\\') { sep = i; upn = false; break; }
    if (name[i] == '@') { sep = i; upn = true; break; }
  }
  if (sep == (size_t)-1) return 1315u;
  const u16* u_src = upn ? name : (name + sep + 1);
  size_t u_len = upn ? sep : (len - sep - 1);
  const u16* d_src = upn ? (name + sep + 1) : name;
  size_t d_len = upn ? (len - sep - 1) : sep;
  if (u_len == 0) return 1315u;
  if (u_len + 1 > user_cap || d_len + 1 > dom_cap) return 122u;
  memcpy(user, u_src, u_len * 2u);
  user[u_len] = 0;
  memcpy(dom, d_src, d_len * 2u);
  dom[d_len] = 0;
  return 0u;
}

u32 TRT_GUEST_ABI cred_ParseUserNameA(const char* name, char* user, u32 user_cap,
                                      char* dom, u32 dom_cap) {
  if (!name || !user || !dom) return 87u;
  const char* bs = strchr(name, '\\');
  const char* at = bs ? nullptr : strchr(name, '@');
  if (!bs && !at) return 1315u;
  const char* u_src = bs ? (bs + 1) : name;
  size_t u_len = bs ? strlen(bs + 1) : (size_t)(at - name);
  const char* d_src = bs ? name : (at + 1);
  size_t d_len = bs ? (size_t)(bs - name) : strlen(at + 1);
  if (u_len == 0) return 1315u;
  if (u_len + 1 > user_cap || d_len + 1 > dom_cap) return 122u;
  memcpy(user, u_src, u_len);
  user[u_len] = 0;
  memcpy(dom, d_src, d_len);
  dom[d_len] = 0;
  return 0u;
}

u32 TRT_GUEST_ABI cred_ConfirmOk() { return 0u; }
u32 TRT_GUEST_ABI cred_PromptCancelled() { return 1223u; }
i32 TRT_GUEST_ABI cred_PackUnsupported() {
  trt_SetLastError(50u);
  return 0;
}

// ---- 24. dxcore.dll — real single-adapter COM factory/list/adapter ---------

struct dxc_guid { u32 d1; u16 d2; u16 d3; u8 d4[8]; };

bool dxc_guid_eq(const void* p, const dxc_guid& g) {
  return p && memcmp(p, &g, 16) == 0;
}

const dxc_guid k_iid_unknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const dxc_guid k_iid_dxcore_factory =
    {0x78ee5945u, 0xc36eu, 0x4b13u, {0xa6,0x69,0x00,0x5d,0xd1,0x1c,0x0f,0x06}};
const dxc_guid k_iid_dxcore_list =
    {0x526c7776u, 0x40e9u, 0x459bu, {0xb7,0x11,0xf3,0x2a,0xd7,0x6d,0xfc,0x28}};
const dxc_guid k_iid_dxcore_adapter =
    {0xf0db4c7fu, 0xfe5au, 0x42a2u, {0xbd,0x62,0xf2,0xa6,0xcf,0x6f,0xc8,0x3e}};
const dxc_guid k_attr_d3d12_gfx =
    {0x0c9ece4du, 0x2f6eu, 0x4f01u, {0x8c,0x96,0xe8,0x9e,0x33,0x1b,0x47,0xb1}};
const dxc_guid k_attr_d3d12_compute =
    {0x248e2800u, 0xa793u, 0x4724u, {0xab,0xaa,0x23,0xa6,0xde,0x1b,0xe0,0x90}};

struct dxc_factory_obj;
struct dxc_list_obj;
struct dxc_adapter_obj {
  const void* const* vtbl;
  std::atomic<u32> refs{1};
};
struct dxc_list_obj {
  const void* const* vtbl;
  std::atomic<u32> refs{1};
};
struct dxc_factory_obj {
  const void* const* vtbl;
  std::atomic<u32> refs{1};
};

dxc_factory_obj* dxc_make_factory();

u32 TRT_GUEST_ABI dxc_adp_AddRef(dxc_adapter_obj* s) { return ++s->refs; }
u32 TRT_GUEST_ABI dxc_adp_Release(dxc_adapter_obj* s) {
  u32 r = --s->refs;
  if (r == 0) delete s;
  return r;
}
i32 TRT_GUEST_ABI dxc_adp_QI(dxc_adapter_obj* s, const void* riid, void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  if (dxc_guid_eq(riid, k_iid_unknown) || dxc_guid_eq(riid, k_iid_dxcore_adapter)) {
    dxc_adp_AddRef(s);
    *ppv = s;
    return S_OK_;
  }
  return E_NOINTERFACE_;
}
u8 TRT_GUEST_ABI dxc_adp_IsValid(dxc_adapter_obj*) { return 1; }
u8 TRT_GUEST_ABI dxc_adp_IsAttrSupported(dxc_adapter_obj*, const void* g) {
  return (dxc_guid_eq(g, k_attr_d3d12_gfx) || dxc_guid_eq(g, k_attr_d3d12_compute))
             ? 1
             : 0;
}
u8 TRT_GUEST_ABI dxc_adp_IsPropSupported(dxc_adapter_obj*, u32 prop) {
  return prop <= 13u ? 1 : 0;
}
i32 TRT_GUEST_ABI dxc_adp_GetPropertySize(dxc_adapter_obj*, u32 prop, u64* sz) {
  if (!sz) return E_INVALIDARG_;
  switch (prop) {
    case 0: *sz = 8; return S_OK_;   // InstanceLuid
    case 1: *sz = 8; return S_OK_;   // DriverVersion
    case 2: *sz = sizeof("Tiramisu Xbox Series GPU"); return S_OK_;
    case 3: *sz = 16; return S_OK_;  // HardwareID
    case 4: *sz = 4; return S_OK_;   // KmdModelVersion
    case 5: case 6: *sz = 2; return S_OK_;
    case 7: case 8: case 9: *sz = 8; return S_OK_;
    case 10: case 11: case 12: case 13: *sz = 1; return S_OK_;
    default: return E_INVALIDARG_;
  }
}
i32 TRT_GUEST_ABI dxc_adp_GetProperty(dxc_adapter_obj* s, u32 prop, u64 sz,
                                      void* out) {
  if (!out) return E_INVALIDARG_;
  u64 need = 0;
  if (dxc_adp_GetPropertySize(s, prop, &need) != S_OK_) return E_INVALIDARG_;
  if (sz < need) return (i32)0x8007007Au;  // E_NOT_SUFFICIENT_BUFFER
  switch (prop) {
    case 0: { u32 luid[2] = {0x4321u, 0u}; memcpy(out, luid, 8); break; }
    case 1: { u64 v = 0x001E000000010000ULL; memcpy(out, &v, 8); break; }
    case 2: { memcpy(out, "Tiramisu Xbox Series GPU", need); break; }
    case 3: { u32 hw[4] = {0x1002u, 0x13FEu, 0u, 0u}; memcpy(out, hw, 16); break; }
    case 4: { u32 wddm = 2700u; memcpy(out, &wddm, 4); break; }
    case 5: case 6: { u16 g = 0u; memcpy(out, &g, 2); break; }
    case 7: { u64 m = 4ULL << 30; memcpy(out, &m, 8); break; }
    case 8: { u64 m = 0ULL; memcpy(out, &m, 8); break; }
    case 9: { u64 m = 4ULL << 30; memcpy(out, &m, 8); break; }
    case 10: case 11: case 12: { *(u8*)out = 1u; break; }
    case 13: { *(u8*)out = 0u; break; }
    default: return E_INVALIDARG_;
  }
  return S_OK_;
}
u8 TRT_GUEST_ABI dxc_adp_IsQuerySupported(dxc_adapter_obj*, u32 st) {
  return st == 0u ? 1 : 0;  // AdapterMemoryBudget
}
i32 TRT_GUEST_ABI dxc_adp_QueryState(dxc_adapter_obj*, u32 st, u64, const void*,
                                     u64 out_sz, void* out) {
  if (st != 0u || !out || out_sz < 16u) return E_INVALIDARG_;
  // DXCoreAdapterMemoryBudget = { uint64_t budget; uint64_t currentUsage; }
  u64 info[4] = {4ULL << 30, 0ULL, 0ULL, 0ULL};
  memcpy(out, info, out_sz < sizeof(info) ? out_sz : sizeof(info));
  return S_OK_;
}
u8 TRT_GUEST_ABI dxc_adp_IsSetSupported(dxc_adapter_obj*, u32) { return 0; }
i32 TRT_GUEST_ABI dxc_adp_SetState(dxc_adapter_obj*, u32, u64, const void*, u64,
                                   const void*) {
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI dxc_adp_GetFactory(dxc_adapter_obj*, const void* riid, void** ppv) {
  dxc_factory_obj* f = dxc_make_factory();
  if (!f) return (i32)0x8007000Eu;
  i32 hr = ((i32 (TRT_GUEST_ABI*)(dxc_factory_obj*, const void*, void**))f->vtbl[0])(f, riid, ppv);
  ((u32 (TRT_GUEST_ABI*)(dxc_factory_obj*))f->vtbl[2])(f);
  return hr;
}

const void* const k_dxc_adapter_vtbl[] = {
    (const void*)&dxc_adp_QI,
    (const void*)&dxc_adp_AddRef,
    (const void*)&dxc_adp_Release,
    (const void*)&dxc_adp_IsValid,
    (const void*)&dxc_adp_IsAttrSupported,
    (const void*)&dxc_adp_IsPropSupported,
    (const void*)&dxc_adp_GetProperty,
    (const void*)&dxc_adp_GetPropertySize,
    (const void*)&dxc_adp_IsQuerySupported,
    (const void*)&dxc_adp_QueryState,
    (const void*)&dxc_adp_IsSetSupported,
    (const void*)&dxc_adp_SetState,
    (const void*)&dxc_adp_GetFactory,
};

u32 TRT_GUEST_ABI dxc_list_AddRef(dxc_list_obj* s) { return ++s->refs; }
u32 TRT_GUEST_ABI dxc_list_Release(dxc_list_obj* s) {
  u32 r = --s->refs;
  if (r == 0) delete s;
  return r;
}
i32 TRT_GUEST_ABI dxc_list_QI(dxc_list_obj* s, const void* riid, void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  if (dxc_guid_eq(riid, k_iid_unknown) || dxc_guid_eq(riid, k_iid_dxcore_list)) {
    dxc_list_AddRef(s);
    *ppv = s;
    return S_OK_;
  }
  return E_NOINTERFACE_;
}
i32 TRT_GUEST_ABI dxc_list_GetAdapter(dxc_list_obj*, u32 idx, const void* riid,
                                      void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  if (idx != 0u) return E_INVALIDARG_;
  auto* adp = new dxc_adapter_obj{k_dxc_adapter_vtbl};
  i32 hr = dxc_adp_QI(adp, riid, ppv);
  dxc_adp_Release(adp);
  return hr;
}
u32 TRT_GUEST_ABI dxc_list_GetAdapterCount(dxc_list_obj*) { return 1u; }
u8 TRT_GUEST_ABI dxc_list_IsStale(dxc_list_obj*) { return 0; }
i32 TRT_GUEST_ABI dxc_list_GetFactory(dxc_list_obj*, const void* riid, void** ppv) {
  return dxc_adp_GetFactory(nullptr, riid, ppv);
}
i32 TRT_GUEST_ABI dxc_list_Sort(dxc_list_obj*, u32, const void*) { return S_OK_; }
u8 TRT_GUEST_ABI dxc_list_IsPrefSupported(dxc_list_obj*, u32) { return 1; }

const void* const k_dxc_list_vtbl[] = {
    (const void*)&dxc_list_QI,
    (const void*)&dxc_list_AddRef,
    (const void*)&dxc_list_Release,
    (const void*)&dxc_list_GetAdapter,
    (const void*)&dxc_list_GetAdapterCount,
    (const void*)&dxc_list_IsStale,
    (const void*)&dxc_list_GetFactory,
    (const void*)&dxc_list_Sort,
    (const void*)&dxc_list_IsPrefSupported,
};

u32 TRT_GUEST_ABI dxc_fac_AddRef(dxc_factory_obj* s) { return ++s->refs; }
u32 TRT_GUEST_ABI dxc_fac_Release(dxc_factory_obj* s) {
  u32 r = --s->refs;
  if (r == 0) delete s;
  return r;
}
i32 TRT_GUEST_ABI dxc_fac_QI(dxc_factory_obj* s, const void* riid, void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  if (dxc_guid_eq(riid, k_iid_unknown) || dxc_guid_eq(riid, k_iid_dxcore_factory)) {
    dxc_fac_AddRef(s);
    *ppv = s;
    return S_OK_;
  }
  return E_NOINTERFACE_;
}
i32 TRT_GUEST_ABI dxc_fac_CreateList(dxc_factory_obj*, u32, const void*,
                                     const void* riid, void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  auto* list = new dxc_list_obj{k_dxc_list_vtbl};
  i32 hr = dxc_list_QI(list, riid, ppv);
  dxc_list_Release(list);
  return hr;
}
i32 TRT_GUEST_ABI dxc_fac_GetByLuid(dxc_factory_obj*, i64, const void* riid,
                                    void** ppv) {
  return dxc_list_GetAdapter(nullptr, 0u, riid, ppv);
}
u8 TRT_GUEST_ABI dxc_fac_IsNotifSupported(dxc_factory_obj*, u32) { return 0; }
i32 TRT_GUEST_ABI dxc_fac_RegNotif(dxc_factory_obj*, void*, u32, void*, void*,
                                   u32* cookie) {
  if (cookie) *cookie = 1u;
  return S_OK_;
}
i32 TRT_GUEST_ABI dxc_fac_UnregNotif(dxc_factory_obj*, u32) { return S_OK_; }

const void* const k_dxc_factory_vtbl[] = {
    (const void*)&dxc_fac_QI,
    (const void*)&dxc_fac_AddRef,
    (const void*)&dxc_fac_Release,
    (const void*)&dxc_fac_CreateList,
    (const void*)&dxc_fac_GetByLuid,
    (const void*)&dxc_fac_IsNotifSupported,
    (const void*)&dxc_fac_RegNotif,
    (const void*)&dxc_fac_UnregNotif,
};

dxc_factory_obj* dxc_make_factory() {
  return new dxc_factory_obj{k_dxc_factory_vtbl};
}

i32 TRT_GUEST_ABI dxc_CreateAdapterFactory(const void* riid, void** ppv) {
  if (!ppv) return E_POINTER_;
  *ppv = nullptr;
  auto* f = dxc_make_factory();
  i32 hr = dxc_fac_QI(f, riid, ppv);
  dxc_fac_Release(f);
  return hr;
}

// ---- 25. directxmath.dll — CPU feature verification ------------------------

u8 TRT_GUEST_ABI dxm_XMVerifyCPUSupport(void) { return 1u; }

}  // namespace

extern "C" int trt_bridge_register_sys_extras(void) {
  static const trt_named_fn k_pwr[] = {
      {"PowerCreateRequest", (void*)&pwr_PowerCreateRequest},
      {"PowerSetRequest", (void*)&pwr_PowerSetRequest},
      {"PowerClearRequest", (void*)&pwr_PowerClearRequest},
      {"PowerCloseRequest", (void*)&pwr_PowerCloseRequest},
      {"CallNtPowerInformation", (void*)&pwr_CallNtPowerInformation},
      {"PowerDeterminePlatformRole", (void*)&pwr_PowerDeterminePlatformRole},
      {"PowerDeterminePlatformRoleEx", (void*)&pwr_PowerDeterminePlatformRoleEx},
      {"PowerRegisterSuspendResumeNotification",
       (void*)&pwr_PowerRegisterSuspendResumeNotification},
      {"PowerUnregisterSuspendResumeNotification",
       (void*)&pwr_PowerUnregisterSuspendResumeNotification},
      {"PowerGetActiveScheme", (void*)&pwr_PowerGetActiveScheme},
  };
  trt_module_register("powrprof.dll", TRT_ROUTE_BUILTIN, k_pwr,
                      (int)(sizeof(k_pwr) / sizeof(k_pwr[0])));

  static const trt_named_fn k_pdh[] = {
      {"PdhOpenQueryW", (void*)&pdh_OpenQueryW},
      {"PdhOpenQueryA", (void*)&pdh_OpenQueryA},
      {"PdhCloseQuery", (void*)&pdh_CloseQuery},
      {"PdhAddCounterW", (void*)&pdh_AddCounterW},
      {"PdhAddCounterA", (void*)&pdh_AddCounterA},
      {"PdhAddEnglishCounterW", (void*)&pdh_AddCounterW},
      {"PdhAddEnglishCounterA", (void*)&pdh_AddCounterA},
      {"PdhRemoveCounter", (void*)&pdh_RemoveCounter},
      {"PdhCollectQueryData", (void*)&pdh_CollectQueryData},
      {"PdhGetFormattedCounterValue", (void*)&pdh_GetFormattedCounterValue},
      {"PdhValidatePathW", (void*)&pdh_ValidatePathW},
      {"PdhValidatePathA", (void*)&pdh_ValidatePathA},
  };
  trt_module_register("pdh.dll", TRT_ROUTE_BUILTIN, k_pdh,
                      (int)(sizeof(k_pdh) / sizeof(k_pdh[0])));

  static const trt_named_fn k_wer[] = {
      {"WerRegisterRuntimeExceptionModule",
       (void*)&wer_RegisterRuntimeExceptionModule},
      {"WerUnregisterRuntimeExceptionModule",
       (void*)&wer_UnregisterRuntimeExceptionModule},
      {"WerRegisterMemoryBlock", (void*)&wer_RegisterMemoryBlock},
      {"WerUnregisterMemoryBlock", (void*)&wer_UnregisterMemoryBlock},
      {"WerSetFlags", (void*)&wer_SetFlags},
      {"WerGetFlags", (void*)&wer_GetFlags},
      {"WerRegisterFile", (void*)&wer_RegisterFile},
  };
  trt_module_register("wer.dll", TRT_ROUTE_BUILTIN, k_wer,
                      (int)(sizeof(k_wer) / sizeof(k_wer[0])));

  // Also merge Power*Request, WerRegister*, ProcessIdToSessionId into kernel32/kernelbase
  static const trt_named_fn k_k32_sys[] = {
      {"PowerCreateRequest", (void*)&pwr_PowerCreateRequest},
      {"PowerSetRequest", (void*)&pwr_PowerSetRequest},
      {"PowerClearRequest", (void*)&pwr_PowerClearRequest},
      {"WerRegisterRuntimeExceptionModule",
       (void*)&wer_RegisterRuntimeExceptionModule},
      {"WerUnregisterRuntimeExceptionModule",
       (void*)&wer_UnregisterRuntimeExceptionModule},
      {"WerRegisterMemoryBlock", (void*)&wer_RegisterMemoryBlock},
      {"WerUnregisterMemoryBlock", (void*)&wer_UnregisterMemoryBlock},
      {"WerSetFlags", (void*)&wer_SetFlags},
      {"WerGetFlags", (void*)&wer_GetFlags},
      {"ProcessIdToSessionId", (void*)&k32_ProcessIdToSessionId},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_k32_sys,
                      (int)(sizeof(k_k32_sys) / sizeof(k_k32_sys[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_k32_sys,
                      (int)(sizeof(k_k32_sys) / sizeof(k_k32_sys[0])));

  static const trt_named_fn k_propsys[] = {
      {"PSCreateMemoryPropertyStore", (void*)&ps_PSCreateMemoryPropertyStore},
      {"PropVariantToString", (void*)&ps_PropVariantToString},
      {"PSGetPropertyKeyFromName", (void*)&ps_PSGetPropertyKeyFromName},
  };
  trt_module_register("propsys.dll", TRT_ROUTE_BUILTIN, k_propsys,
                      (int)(sizeof(k_propsys) / sizeof(k_propsys[0])));

  static const trt_named_fn k_userenv[] = {
      {"GetUserProfileDirectoryW", (void*)&uenv_GetUserProfileDirectoryW},
      {"GetUserProfileDirectoryA", (void*)&uenv_GetUserProfileDirectoryA},
      {"GetProfilesDirectoryW", (void*)&uenv_GetProfilesDirectoryW},
      {"GetProfilesDirectoryA", (void*)&uenv_GetProfilesDirectoryA},
      {"GetAllUsersProfileDirectoryW",
       (void*)&uenv_GetAllUsersProfileDirectoryW},
      {"CreateEnvironmentBlock", (void*)&uenv_CreateEnvironmentBlock},
      {"DestroyEnvironmentBlock", (void*)&uenv_DestroyEnvironmentBlock},
  };
  trt_module_register("userenv.dll", TRT_ROUTE_BUILTIN, k_userenv,
                      (int)(sizeof(k_userenv) / sizeof(k_userenv[0])));

  static const trt_named_fn k_wts[] = {
      {"WTSRegisterSessionNotification",
       (void*)&wts_WTSRegisterSessionNotification},
      {"WTSUnRegisterSessionNotification",
       (void*)&wts_WTSUnRegisterSessionNotification},
      {"WTSRegisterSessionNotificationEx",
       (void*)&wts_WTSRegisterSessionNotificationEx},
      {"WTSUnRegisterSessionNotificationEx",
       (void*)&wts_WTSUnRegisterSessionNotificationEx},
      {"WTSQuerySessionInformationW", (void*)&wts_WTSQuerySessionInformationW},
      {"WTSQuerySessionInformationA", (void*)&wts_WTSQuerySessionInformationA},
      {"WTSFreeMemory", (void*)&wts_WTSFreeMemory},
  };
  trt_module_register("wtsapi32.dll", TRT_ROUTE_BUILTIN, k_wts,
                      (int)(sizeof(k_wts) / sizeof(k_wts[0])));

  static const trt_named_fn k_comdlg32[] = {
      {"GetOpenFileNameW", (void*)&cdlg_fail_bool},
      {"GetOpenFileNameA", (void*)&cdlg_fail_bool},
      {"GetSaveFileNameW", (void*)&cdlg_fail_bool},
      {"GetSaveFileNameA", (void*)&cdlg_fail_bool},
      {"ChooseColorW", (void*)&cdlg_fail_bool},
      {"ChooseColorA", (void*)&cdlg_fail_bool},
      {"ChooseFontW", (void*)&cdlg_fail_bool},
      {"ChooseFontA", (void*)&cdlg_fail_bool},
      {"PrintDlgW", (void*)&cdlg_fail_bool},
      {"PrintDlgA", (void*)&cdlg_fail_bool},
      {"PageSetupDlgW", (void*)&cdlg_fail_bool},
      {"PageSetupDlgA", (void*)&cdlg_fail_bool},
      {"FindTextW", (void*)&cdlg_fail_ptr},
      {"FindTextA", (void*)&cdlg_fail_ptr},
      {"ReplaceTextW", (void*)&cdlg_fail_ptr},
      {"ReplaceTextA", (void*)&cdlg_fail_ptr},
      {"CommDlgExtendedError", (void*)&cdlg_CommDlgExtendedError},
  };
  trt_module_register("comdlg32.dll", TRT_ROUTE_BUILTIN, k_comdlg32,
                      (int)(sizeof(k_comdlg32) / sizeof(k_comdlg32[0])));

  static const trt_named_fn k_uxtheme[] = {
      {"OpenThemeData", (void*)&uxt_OpenThemeData},
      {"OpenThemeDataEx", (void*)&uxt_OpenThemeDataEx},
      {"OpenThemeDataForDpi", (void*)&uxt_OpenThemeDataForDpi},
      {"CloseThemeData", (void*)&uxt_CloseThemeData},
      {"IsThemeActive", (void*)&uxt_IsThemeActive},
      {"IsAppThemed", (void*)&uxt_IsAppThemed},
      {"SetWindowTheme", (void*)&uxt_SetWindowTheme},
      {"DrawThemeBackground", (void*)&uxt_DrawThemeBackground},
      {"DrawThemeBackgroundEx", (void*)&uxt_DrawThemeBackgroundEx},
      {"DrawThemeText", (void*)&uxt_DrawThemeText},
      {"GetThemeColor", (void*)&uxt_GetThemeColor},
      {"GetThemeInt", (void*)&uxt_GetThemeInt},
      {"GetThemeBool", (void*)&uxt_GetThemeBool},
      {"GetThemeMargins", (void*)&uxt_GetThemeMargins},
      {"GetThemePartSize", (void*)&uxt_GetThemePartSize},
      {"GetThemeSysColor", (void*)&uxt_GetThemeSysColor},
      {"IsThemePartDefined", (void*)&uxt_IsThemePartDefined},
      {"IsThemeBackgroundPartiallyTransparent",
       (void*)&uxt_IsThemeBackgroundPartiallyTransparent},
      {"EnableThemeDialogTexture", (void*)&uxt_EnableThemeDialogTexture},
      {"BufferedPaintInit", (void*)&uxt_BufferedPaintInit},
      {"BufferedPaintUnInit", (void*)&uxt_BufferedPaintUnInit},
      {"BeginBufferedPaint", (void*)&uxt_BeginBufferedPaint},
      {"EndBufferedPaint", (void*)&uxt_EndBufferedPaint},
      {"BufferedPaintSetAlpha", (void*)&uxt_BufferedPaintSetAlpha},
      {"GetCurrentThemeName", (void*)&uxt_GetCurrentThemeName},
      {"ShouldAppsUseDarkMode", (void*)&uxt_ShouldAppsUseDarkMode},
      {"AllowDarkModeForWindow", (void*)&uxt_AllowDarkModeForWindow},
      {"SetPreferredAppMode", (void*)&uxt_SetPreferredAppMode},
  };
  trt_module_register("uxtheme.dll", TRT_ROUTE_BUILTIN, k_uxtheme,
                      (int)(sizeof(k_uxtheme) / sizeof(k_uxtheme[0])));

  static const trt_named_fn k_msimg32[] = {
      {"AlphaBlend", (void*)&msimg_AlphaBlend},
      {"TransparentBlt", (void*)&msimg_TransparentBlt},
      {"GradientFill", (void*)&msimg_GradientFill},
  };
  trt_module_register("msimg32.dll", TRT_ROUTE_BUILTIN, k_msimg32,
                      (int)(sizeof(k_msimg32) / sizeof(k_msimg32[0])));

  static const trt_named_fn k_secur32[] = {
      {"GetUserNameExW", (void*)&sec_GetUserNameExW},
      {"GetUserNameExA", (void*)&sec_GetUserNameExA},
      {"EnumerateSecurityPackagesW", (void*)&sec_EnumerateSecurityPackages},
      {"EnumerateSecurityPackagesA", (void*)&sec_EnumerateSecurityPackages},
      {"FreeContextBuffer", (void*)&sec_FreeContextBuffer},
      {"AcquireCredentialsHandleW", (void*)&sec_AcquireCredentialsHandle},
      {"AcquireCredentialsHandleA", (void*)&sec_AcquireCredentialsHandle},
      {"InitializeSecurityContextW", (void*)&sec_InitializeSecurityContext},
      {"InitializeSecurityContextA", (void*)&sec_InitializeSecurityContext},
      {"AcceptSecurityContext", (void*)&sec_AcceptSecurityContext},
      {"DeleteSecurityContext", (void*)&sec_FreeCredOrSecHandle},
      {"FreeCredentialsHandle", (void*)&sec_FreeCredOrSecHandle},
      {"QuerySecurityPackageInfoW", (void*)&sec_QuerySecurityPackageInfo},
      {"QuerySecurityPackageInfoA", (void*)&sec_QuerySecurityPackageInfo},
      {"QueryContextAttributesW", (void*)&sec_QueryContextAttributes},
      {"QueryContextAttributesA", (void*)&sec_QueryContextAttributes},
      {"EncryptMessage", (void*)&sec_CryptoMsgOp},
      {"DecryptMessage", (void*)&sec_CryptoMsgOp},
      {"MakeSignature", (void*)&sec_CryptoMsgOp},
      {"VerifySignature", (void*)&sec_CryptoMsgOp},
      {"InitSecurityInterfaceW", (void*)&sec_InitSecurityInterface},
      {"InitSecurityInterfaceA", (void*)&sec_InitSecurityInterface},
      {"LsaConnectUntrusted", (void*)&sec_LsaConnectUntrusted},
      {"LsaDeregisterLogonProcess", (void*)&sec_LsaDeregisterLogonProcess},
      {"LsaLookupAuthenticationPackage",
       (void*)&sec_LsaLookupAuthenticationPackage},
      {"LsaCallAuthenticationPackage",
       (void*)&sec_LsaCallAuthenticationPackage},
      {"LsaFreeReturnBuffer", (void*)&sec_LsaFreeReturnBuffer},
  };
  trt_module_register("secur32.dll", TRT_ROUTE_BUILTIN, k_secur32,
                      (int)(sizeof(k_secur32) / sizeof(k_secur32[0])));
  trt_module_register("sspicli.dll", TRT_ROUTE_BUILTIN, k_secur32,
                      (int)(sizeof(k_secur32) / sizeof(k_secur32[0])));

  static const trt_named_fn k_netapi32[] = {
      {"NetApiBufferAllocate", (void*)&net_NetApiBufferAllocate},
      {"NetApiBufferFree", (void*)&net_NetApiBufferFree},
      {"NetApiBufferReallocate", (void*)&net_NetApiBufferReallocate},
      {"NetApiBufferSize", (void*)&net_NetApiBufferSize},
      {"NetWkstaGetInfo", (void*)&net_NetGetInfoUnsupported4},
      {"NetUserGetInfo", (void*)&net_NetGetInfoUnsupported4},
      {"NetServerGetInfo", (void*)&net_NetGetInfoUnsupported4},
      {"NetGetJoinInformation", (void*)&net_NetGetInfoUnsupported3},
      {"NetGetAnyDCName", (void*)&net_NetGetInfoUnsupported3},
      {"NetGetDCName", (void*)&net_NetGetInfoUnsupported3},
      {"NetShareEnum", (void*)&net_NetShareEnum},
      {"Netbios", (void*)&net_Netbios},
  };
  trt_module_register("netapi32.dll", TRT_ROUTE_BUILTIN, k_netapi32,
                      (int)(sizeof(k_netapi32) / sizeof(k_netapi32[0])));

  static const trt_named_fn k_dnsapi[] = {
      {"DnsQuery_W", (void*)&dns_DnsQuery},
      {"DnsQuery_A", (void*)&dns_DnsQuery},
      {"DnsQuery_UTF8", (void*)&dns_DnsQuery},
      {"DnsRecordListFree", (void*)&dns_DnsRecordListFree},
      {"DnsValidateName_W", (void*)&dns_DnsValidateName},
      {"DnsValidateName_A", (void*)&dns_DnsValidateName},
  };
  trt_module_register("dnsapi.dll", TRT_ROUTE_BUILTIN, k_dnsapi,
                      (int)(sizeof(k_dnsapi) / sizeof(k_dnsapi[0])));

  static const trt_named_fn k_wintrust[] = {
      {"WinVerifyTrust", (void*)&wt_WinVerifyTrust},
      {"WinVerifyTrustEx", (void*)&wt_WinVerifyTrust},
      {"CryptCATAdminAcquireContext", (void*)&wt_CryptCATFail},
      {"CryptCATAdminAcquireContext2", (void*)&wt_CryptCATFail},
      {"CryptCATAdminCalcHashFromFileHandle", (void*)&wt_CryptCATFail},
      {"CryptCATAdminCalcHashFromFileHandle2", (void*)&wt_CryptCATFail},
      {"CryptCATAdminReleaseContext", (void*)&wt_CryptCATRelease},
      {"CryptCATAdminReleaseCatalogContext", (void*)&wt_CryptCATRelease},
      {"WTHelperGetProvSignerFromChain", (void*)&wt_HelperNull},
      {"WTHelperProvDataFromStateData", (void*)&wt_HelperNull},
  };
  trt_module_register("wintrust.dll", TRT_ROUTE_BUILTIN, k_wintrust,
                      (int)(sizeof(k_wintrust) / sizeof(k_wintrust[0])));

  static const trt_named_fn k_mpr[] = {
      {"WNetGetConnectionW", (void*)&mpr_NotConnected3},
      {"WNetGetConnectionA", (void*)&mpr_NotConnected3},
      {"WNetGetUniversalNameW", (void*)&mpr_NotConnected4},
      {"WNetGetUniversalNameA", (void*)&mpr_NotConnected4},
      {"WNetGetUserW", (void*)&mpr_NotConnected3},
      {"WNetGetUserA", (void*)&mpr_NotConnected3},
      {"WNetOpenEnumW", (void*)&mpr_NotConnectedOpen},
      {"WNetOpenEnumA", (void*)&mpr_NotConnectedOpen},
      {"WNetEnumResourceW", (void*)&mpr_NotConnectedEnum},
      {"WNetEnumResourceA", (void*)&mpr_NotConnectedEnum},
      {"WNetCloseEnum", (void*)&mpr_CloseEnum},
      {"WNetAddConnection2W", (void*)&mpr_NotConnected},
      {"WNetCancelConnection2W", (void*)&mpr_NotConnected},
  };
  trt_module_register("mpr.dll", TRT_ROUTE_BUILTIN, k_mpr,
                      (int)(sizeof(k_mpr) / sizeof(k_mpr[0])));

  static const trt_named_fn k_oleacc[] = {
      {"AccessibleObjectFromWindow", (void*)&acc_AccessibleObjectFromWindow},
      {"AccessibleObjectFromPoint", (void*)&acc_AccessibleObjectFromPoint},
      {"AccessibleObjectFromEvent", (void*)&acc_AccessibleObjectFromEvent},
      {"AccessibleChildren", (void*)&acc_AccessibleChildren},
      {"CreateStdAccessibleObject", (void*)&acc_CreateStdAccessibleObject},
      {"LresultFromObject", (void*)&acc_LresultFromObject},
      {"ObjectFromLresult", (void*)&acc_ObjectFromLresult},
      {"GetRoleTextW", (void*)&acc_GetTextW},
      {"GetRoleTextA", (void*)&acc_GetTextA},
      {"GetStateTextW", (void*)&acc_GetTextW},
      {"GetStateTextA", (void*)&acc_GetTextA},
  };
  trt_module_register("oleacc.dll", TRT_ROUTE_BUILTIN, k_oleacc,
                      (int)(sizeof(k_oleacc) / sizeof(k_oleacc[0])));

  static const trt_named_fn k_dhcpcsvc[] = {
      {"DhcpCApiInitialize", (void*)&dhcp_DhcpCApiInitialize},
      {"DhcpCApiCleanup", (void*)&dhcp_DhcpCApiCleanup},
      {"DhcpRequestParams", (void*)&dhcp_DhcpUnsupported},
      {"DhcpUndoRequestParams", (void*)&dhcp_DhcpUnsupported},
      {"DhcpRegisterParamChange", (void*)&dhcp_DhcpUnsupported},
      {"DhcpDeRegisterParamChange", (void*)&dhcp_DhcpUnsupported},
  };
  trt_module_register("dhcpcsvc.dll", TRT_ROUTE_BUILTIN, k_dhcpcsvc,
                      (int)(sizeof(k_dhcpcsvc) / sizeof(k_dhcpcsvc[0])));

  static const trt_named_fn k_normaliz[] = {
      {"IdnToAscii", (void*)&norm_IdnToAscii},
      {"IdnToUnicode", (void*)&norm_IdnToUnicode},
      {"IdnToNameprepUnicode", (void*)&norm_IdnToNameprepUnicode},
      {"NormalizeString", (void*)&norm_NormalizeString},
      {"IsNormalizedString", (void*)&norm_IsNormalizedString},
  };
  trt_module_register("normaliz.dll", TRT_ROUTE_BUILTIN, k_normaliz,
                      (int)(sizeof(k_normaliz) / sizeof(k_normaliz[0])));

  static const trt_named_fn k_sechost[] = {
      {"OpenSCManagerW", (void*)&sc_OpenAccessDenied},
      {"OpenSCManagerA", (void*)&sc_OpenAccessDenied},
      {"OpenServiceW", (void*)&sc_OpenAccessDenied},
      {"OpenServiceA", (void*)&sc_OpenAccessDenied},
      {"CreateServiceW", (void*)&sc_OpenAccessDenied},
      {"CreateServiceA", (void*)&sc_OpenAccessDenied},
      {"CloseServiceHandle", (void*)&sc_CloseServiceHandle},
      {"DeleteService", (void*)&sc_FailAccessDenied},
      {"StartServiceW", (void*)&sc_FailAccessDenied},
      {"StartServiceA", (void*)&sc_FailAccessDenied},
      {"ControlService", (void*)&sc_FailAccessDenied},
      {"QueryServiceStatus", (void*)&sc_FailAccessDenied},
      {"QueryServiceStatusEx", (void*)&sc_FailAccessDenied},
      {"QueryServiceConfigW", (void*)&sc_FailAccessDenied},
      {"QueryServiceConfigA", (void*)&sc_FailAccessDenied},
      {"EnumServicesStatusExW", (void*)&sc_FailAccessDenied},
      {"EnumServicesStatusExA", (void*)&sc_FailAccessDenied},
      {"StartServiceCtrlDispatcherW", (void*)&sc_FailAccessDenied},
      {"StartServiceCtrlDispatcherA", (void*)&sc_FailAccessDenied},
      {"SetServiceStatus", (void*)&sc_FailAccessDenied},
      {"RegisterServiceCtrlHandlerW", (void*)&sc_RegisterServiceCtrlHandler},
      {"RegisterServiceCtrlHandlerA", (void*)&sc_RegisterServiceCtrlHandler},
      {"RegisterServiceCtrlHandlerExW", (void*)&sc_RegisterServiceCtrlHandler},
      {"RegisterServiceCtrlHandlerExA", (void*)&sc_RegisterServiceCtrlHandler},
      {"NotifyServiceStatusChangeW", (void*)&sc_NotifyUnsupported},
      {"NotifyServiceStatusChangeA", (void*)&sc_NotifyUnsupported},
      {"EventRegister", (void*)&se_EventRegister},
      {"EventUnregister", (void*)&se_EventReturnZero},
      {"EventWrite", (void*)&se_EventReturnZero},
      {"EventWriteEx", (void*)&se_EventReturnZero},
      {"EventWriteTransfer", (void*)&se_EventReturnZero},
      {"EventWriteString", (void*)&se_EventReturnZero},
      {"EventSetInformation", (void*)&se_EventReturnZero},
      {"EventEnabled", (void*)&se_EventEnabledFalse},
      {"EventProviderEnabled", (void*)&se_EventEnabledFalse},
      {"ConvertSidToStringSidW", (void*)&se_ConvertSidToStringSidW},
      {"ConvertStringSidToSidW", (void*)&se_ConvertStringSidToSidW},
      {"ConvertStringSidToSidA", (void*)&se_ConvertStringSidToSidA},
      {"LookupAccountNameW", (void*)&se_LookupNoneMapped},
      {"LookupAccountNameA", (void*)&se_LookupNoneMapped},
      {"LookupAccountSidW", (void*)&se_LookupNoneMapped},
      {"LookupAccountSidA", (void*)&se_LookupNoneMapped},
      {"LookupPrivilegeValueW", (void*)&se_LookupNoneMapped},
      {"LookupPrivilegeValueA", (void*)&se_LookupNoneMapped},
      {"LsaOpenPolicy", (void*)&se_LsaOpenPolicy},
      {"LsaClose", (void*)&se_LsaClose},
      {"LsaFreeMemory", (void*)&se_LsaFreeMemory},
      {"LsaQueryInformationPolicy", (void*)&se_LsaQueryInfoUnsupported},
      {"LsaLookupNames2", (void*)&se_LsaAccessDenied},
      {"LsaLookupSids", (void*)&se_LsaAccessDenied},
      {"LsaLookupSids2", (void*)&se_LsaAccessDenied},
  };
  trt_module_register("sechost.dll", TRT_ROUTE_BUILTIN, k_sechost,
                      (int)(sizeof(k_sechost) / sizeof(k_sechost[0])));

  static const trt_named_fn k_ncrypt[] = {
      {"NCryptOpenStorageProvider", (void*)&nc_OpenStorageProvider},
      {"NCryptEnumStorageProviders", (void*)&nc_EnumStorageProviders},
      {"NCryptFreeBuffer", (void*)&nc_FreeBuffer},
      {"NCryptFreeObject", (void*)&nc_FreeObject},
      {"NCryptIsKeyHandle", (void*)&nc_IsKeyHandle},
      {"NCryptOpenKey", (void*)&nc_NotFound},
      {"NCryptEnumKeys", (void*)&nc_NotFound},
      {"NCryptCreatePersistedKey", (void*)&nc_NotSupported},
      {"NCryptFinalizeKey", (void*)&nc_NotSupported},
      {"NCryptImportKey", (void*)&nc_NotSupported},
      {"NCryptExportKey", (void*)&nc_NotSupported},
      {"NCryptEncrypt", (void*)&nc_NotSupported},
      {"NCryptDecrypt", (void*)&nc_NotSupported},
      {"NCryptSignHash", (void*)&nc_NotSupported},
      {"NCryptVerifySignature", (void*)&nc_NotSupported},
      {"NCryptGetProperty", (void*)&nc_NotSupported},
      {"NCryptSetProperty", (void*)&nc_NotSupported},
      {"NCryptDeleteKey", (void*)&nc_NotSupported},
      {"NCryptEnumAlgorithms", (void*)&nc_NotSupported},
      {"NCryptIsAlgSupported", (void*)&nc_NotSupported},
      {"NCryptSecretAgreement", (void*)&nc_NotSupported},
      {"NCryptDeriveKey", (void*)&nc_NotSupported},
      {"NCryptKeyDerivation", (void*)&nc_NotSupported},
  };
  trt_module_register("ncrypt.dll", TRT_ROUTE_BUILTIN, k_ncrypt,
                      (int)(sizeof(k_ncrypt) / sizeof(k_ncrypt[0])));

  static const trt_named_fn k_usp10[] = {
      {"ScriptFreeCache", (void*)&usp_ScriptFreeCache},
      {"ScriptItemize", (void*)&usp_ScriptItemize},
      {"ScriptItemizeOpenType", (void*)&usp_ScriptItemizeOpenType},
      {"ScriptLayout", (void*)&usp_ScriptLayout},
      {"ScriptShape", (void*)&usp_ScriptShape},
      {"ScriptShapeOpenType", (void*)&usp_ScriptShapeOpenType},
      {"ScriptPlace", (void*)&usp_ScriptPlace},
      {"ScriptPlaceOpenType", (void*)&usp_ScriptPlaceOpenType},
      {"ScriptTextOut", (void*)&usp_ScriptOk},
      {"ScriptJustify", (void*)&usp_ScriptOk},
      {"ScriptBreak", (void*)&usp_ScriptBreak},
      {"ScriptCPtoX", (void*)&usp_ScriptCPtoX},
      {"ScriptXtoCP", (void*)&usp_ScriptXtoCP},
      {"ScriptGetLogicalWidths", (void*)&usp_ScriptGetLogicalWidths},
      {"ScriptApplyLogicalWidth", (void*)&usp_ScriptApplyLogicalWidth},
      {"ScriptGetCMap", (void*)&usp_ScriptGetCMap},
      {"ScriptGetGlyphABCWidth", (void*)&usp_ScriptGetGlyphABCWidth},
      {"ScriptGetProperties", (void*)&usp_ScriptGetProperties},
      {"ScriptGetFontProperties", (void*)&usp_ScriptGetFontProperties},
      {"ScriptCacheGetHeight", (void*)&usp_ScriptCacheGetHeight},
      {"ScriptIsComplex", (void*)&usp_ScriptIsComplex},
      {"ScriptRecordDigitSubstitution", (void*)&usp_ScriptOk},
      {"ScriptApplyDigitSubstitution", (void*)&usp_ScriptOk},
      {"ScriptStringAnalyse", (void*)&usp_ScriptStringAnalyse},
      {"ScriptStringFree", (void*)&usp_ScriptStringFree},
      {"ScriptString_pcOutChars", (void*)&usp_ScriptString_pcOutChars},
      {"ScriptString_pLogAttr", (void*)&usp_ScriptString_pLogAttr},
      {"ScriptString_pSize", (void*)&usp_ScriptString_pSize},
      {"ScriptStringGetOrder", (void*)&usp_ScriptStringGetOrder},
      {"ScriptStringGetLogicalWidths", (void*)&usp_ScriptStringGetLogicalWidths},
      {"ScriptStringCPtoX", (void*)&usp_ScriptStringCPtoX},
      {"ScriptStringXtoCP", (void*)&usp_ScriptStringXtoCP},
      {"ScriptStringValidate", (void*)&usp_ScriptStringValidate},
      {"ScriptStringOut", (void*)&usp_ScriptOk},
  };
  trt_module_register("usp10.dll", TRT_ROUTE_BUILTIN, k_usp10,
                      (int)(sizeof(k_usp10) / sizeof(k_usp10[0])));

  static const trt_named_fn k_cryptui[] = {
      {"CryptUIDlgViewContext", (void*)&cui_DlgCancelled},
      {"CryptUIDlgViewCertificateW", (void*)&cui_DlgCancelled},
      {"CryptUIDlgViewCertificateA", (void*)&cui_DlgCancelled},
      {"CryptUIDlgSelectCertificateFromStore", (void*)&cui_SelectCertNull},
      {"CryptUIDlgCertMgr", (void*)&cui_DlgCancelled},
      {"CryptUIWizDigitalSign", (void*)&cui_DlgCancelled},
      {"CryptUIWizFreeDigitalSignContext", (void*)&cui_WizFree},
      {"CryptUIWizExport", (void*)&cui_DlgCancelled},
      {"CryptUIWizImport", (void*)&cui_DlgCancelled},
      {"CryptUIDlgViewSignerInfoW", (void*)&cui_DlgCancelled},
      {"CryptUIDlgViewSignerInfoA", (void*)&cui_DlgCancelled},
  };
  trt_module_register("cryptui.dll", TRT_ROUTE_BUILTIN, k_cryptui,
                      (int)(sizeof(k_cryptui) / sizeof(k_cryptui[0])));

  static const trt_named_fn k_credui[] = {
      {"CredUIParseUserNameW", (void*)&cred_ParseUserNameW},
      {"CredUIParseUserNameA", (void*)&cred_ParseUserNameA},
      {"CredUIConfirmCredentialsW", (void*)&cred_ConfirmOk},
      {"CredUIConfirmCredentialsA", (void*)&cred_ConfirmOk},
      {"CredUIPromptForCredentialsW", (void*)&cred_PromptCancelled},
      {"CredUIPromptForCredentialsA", (void*)&cred_PromptCancelled},
      {"CredUIPromptForWindowsCredentialsW", (void*)&cred_PromptCancelled},
      {"CredUIPromptForWindowsCredentialsA", (void*)&cred_PromptCancelled},
      {"CredUICmdLinePromptForCredentialsW", (void*)&cred_PromptCancelled},
      {"CredUICmdLinePromptForCredentialsA", (void*)&cred_PromptCancelled},
      {"CredUIStoreSSOCredW", (void*)&cred_PromptCancelled},
      {"CredUIReadSSOCredW", (void*)&cred_PromptCancelled},
      {"CredPackAuthenticationBufferW", (void*)&cred_PackUnsupported},
      {"CredPackAuthenticationBufferA", (void*)&cred_PackUnsupported},
      {"CredUnPackAuthenticationBufferW", (void*)&cred_PackUnsupported},
      {"CredUnPackAuthenticationBufferA", (void*)&cred_PackUnsupported},
  };
  trt_module_register("credui.dll", TRT_ROUTE_BUILTIN, k_credui,
                      (int)(sizeof(k_credui) / sizeof(k_credui[0])));

  static const trt_named_fn k_dxcore[] = {
      {"DXCoreCreateAdapterFactory", (void*)&dxc_CreateAdapterFactory},
  };
  trt_module_register("dxcore.dll", TRT_ROUTE_BUILTIN, k_dxcore,
                      (int)(sizeof(k_dxcore) / sizeof(k_dxcore[0])));

  static const trt_named_fn k_directxmath[] = {
      {"XMVerifyCPUSupport", (void*)&dxm_XMVerifyCPUSupport},
  };
  trt_module_register("directxmath.dll", TRT_ROUTE_BUILTIN, k_directxmath,
                      (int)(sizeof(k_directxmath) / sizeof(k_directxmath[0])));
  return 0;
}
