// tiramisu/src/trt/bridge_ole.cpp
// Minimal COM boot + OLEAUT. See bridge_ole.h.
//
// CoCreateInstance dispatches to registered CLSID personalities; an
// unregistered CLSID fails REGDB_E_CLASSNOTREG with the GUID in the log —
// the fail-visible rule. That log line is how we discover which COM
// classes a game actually needs on console.
#include "bridge_ole.h"
#include "bridge_wasapi.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <vector>

#ifdef _WIN32
// GetCurrentThreadId arrives with the Windows surface trt.h pulls in.
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

// unique name, file scope, OUTSIDE extern "C" (the log_line collision rule)
namespace olelog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) olelog::line(s_)

// sync primitive shared with the kernel32 bridge (bridge_thread.cpp)
extern "C" u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count,
                                                      const u64* handles,
                                                      bool wait_all, u32 ms);

extern "C" {

namespace {

// ---- HRESULTs ----
constexpr int S_OK_ = 0;
constexpr int S_FALSE_ = 1;
constexpr int E_NOINTERFACE_ = (int)0x80004002;
constexpr int E_OUTOFMEMORY_ = (int)0x8007000E;
constexpr int E_INVALIDARG_ = (int)0x80070057;
constexpr int E_HANDLE_ = (int)0x80070006;
constexpr int RPC_S_CALLPENDING_ = (int)0x80010127;
constexpr int REGDB_E_CLASSNOTREG_ = (int)0x80040154;
constexpr int CO_E_CLASSSTRING_ = (int)0x800401F3;

// apartment model: a game passes COINIT_APARTMENTTHREADED (0x2) or
// COINIT_MULTITHREADED (0x0); either is fine single-threaded
constexpr u32 COINIT_APARTMENTTHREADED = 0x2;

// Apartment state is PER THREAD. CoInitializeEx initializes the calling
// thread and CoGetApartmentType reports that thread's model, so one global
// was a lie the moment a game touched COM from a worker: a worker's STA init
// rewrote what the main thread reported (and its nesting count blocked the
// main thread's own S_FALSE). Every guest thread now carries its own.
thread_local int t_init_count = 0;  // CoInitialize nesting on this thread
thread_local int t_init_mta = 0;    // this thread's model (0=STA 1=MTA)

// Thread id for the apartment log lines, so a guest's two apartments are
// distinguishable in the log instead of looking like one thread re-initing.
uintptr_t ole_thread_id() {
#ifdef _WIN32
  return (uintptr_t)GetCurrentThreadId();
#else
  return (uintptr_t)syscall(SYS_gettid);
#endif
}

// ---- CLSID personality registry (uncapped, thread-safe) ----
struct com_class {
  u8 clsid[16];
  trt_com_factory factory;
  void* user;
};
std::mutex g_classes_mu;
std::vector<com_class> g_classes;

void clsid_to_str(const u8* g, char* out) {
  // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
  static const char hex[] = "0123456789ABCDEF";
  int o = 0;
  out[o++] = '{';
  // fields: u32 LE, u16 LE, u16 LE, 8 bytes BE
  const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = hex[g[order[i]] >> 4];
    out[o++] = hex[g[order[i]] & 0xF];
  }
  out[o++] = '}';
  out[o] = 0;
}

int hex_nibble(u16 c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

bool str_to_clsid(const u16* s, u8* out) {
  if (!s || !out) return false;
  int p = 0;
  if (s[p] == '{') ++p;
  const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      if (s[p] != '-') return false;
      ++p;
    }
    int hi = hex_nibble(s[p++]);
    int lo = hex_nibble(s[p++]);
    if (hi < 0 || lo < 0) return false;
    out[order[i]] = (u8)((hi << 4) | lo);
  }
  return true;
}

}  // namespace

void trt_ole_register_class(const void* clsid, trt_com_factory factory,
                            void* user) {
  if (!clsid || !factory) return;
  std::lock_guard<std::mutex> lk(g_classes_mu);
  for (com_class& c : g_classes) {
    if (memcmp(c.clsid, clsid, 16) == 0) {
      c.factory = factory;
      c.user = user;
      return;
    }
  }
  com_class c = {};
  memcpy(c.clsid, clsid, 16);
  c.factory = factory;
  c.user = user;
  g_classes.push_back(c);
}

// ---- CoInitializeEx / CoInitialize / CoUninitialize ----
int TRT_GUEST_ABI ole_CoInitializeEx(void* reserved, u32 model) {
  gap_trace_record("CoInitializeEx");
  (void)reserved;
  ++t_init_count;
  t_init_mta = (model & COINIT_APARTMENTTHREADED) ? 0 : 1;
  char line[128];
  snprintf(line, sizeof(line),
           "ole: CoInitializeEx(model=%s) count=%d tid=%llu",
           (model & COINIT_APARTMENTTHREADED) ? "STA" : "MTA", t_init_count,
           (unsigned long long)ole_thread_id());
  log_line(line);
  // S_FALSE (1) when this thread was already initialized — games accept both
  return t_init_count > 1 ? S_FALSE_ : S_OK_;
}
int TRT_GUEST_ABI ole_CoInitialize(void* reserved) {
  return ole_CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void TRT_GUEST_ABI ole_CoUninitialize(void) {
  if (t_init_count) --t_init_count;
}
void TRT_GUEST_ABI ole_CoFreeUnusedLibraries(void) {}
void TRT_GUEST_ABI ole_CoFreeUnusedLibrariesEx(u32 delay, u32 reserved) {
  (void)delay; (void)reserved;
}

// ---- CoCreateInstance: dispatch to personalities, else fail VISIBLY ----
int TRT_GUEST_ABI ole_CoCreateInstance(const void* clsid, void* outer,
                                       u32 context, const void* iid,
                                       void** out) {
  if (!clsid || !iid || !out) return E_INVALIDARG_;
  *out = nullptr;
  if (outer) return E_NOINTERFACE_;  // no aggregation
  const u8* g = (const u8*)clsid;
  trt_com_factory fn = nullptr;
  void* user = nullptr;
  u32 idx = 0;
  {
    std::lock_guard<std::mutex> lk(g_classes_mu);
    for (u32 i = 0; i < (u32)g_classes.size(); ++i) {
      if (memcmp(g_classes[i].clsid, g, 16) == 0) {
        fn = g_classes[i].factory;
        user = g_classes[i].user;
        idx = i;
        break;
      }
    }
  }
  if (fn) {
    char line[96];
    snprintf(line, sizeof(line), "ole: CoCreateInstance -> personality %u",
             idx);
    log_line(line);
    return fn(iid, out, user);
  }
  char guid[40];
  clsid_to_str(g, guid);
  char line[160];
  snprintf(line, sizeof(line),
           "ole: CoCreateInstance(%s ctx=%u) -> REGDB_E_CLASSNOTREG "
           "(no personality registered)", guid, context);
  log_line(line);
  return REGDB_E_CLASSNOTREG_;
}
int TRT_GUEST_ABI ole_CoGetClassObject(const void* clsid, u32 context,
                                       void* reserved, const void* iid,
                                       void** out) {
  (void)reserved;
  return ole_CoCreateInstance(clsid, nullptr, context, iid, out);
}

// ---- task memory (CoTaskMem*) ----
void* TRT_GUEST_ABI ole_CoTaskMemAlloc(u32 bytes) {
  if (!bytes) return nullptr;
  return malloc(bytes);
}
void* TRT_GUEST_ABI ole_CoTaskMemRealloc(void* p, u32 bytes) {
  return realloc(p, bytes);
}
void TRT_GUEST_ABI ole_CoTaskMemFree(void* p) { free(p); }
u32 TRT_GUEST_ABI ole_CoTaskMemSize(void* p) {
  (void)p;
  return 0;  // Windows answers the alloc size; nothing depends on it here
}

namespace {

// ---- OLEAUT: BSTR ----
// BSTR = [u32 length-in-bytes][string bytes][\0\0], ptr points at the chars
u16* TRT_GUEST_ABI ole_SysAllocString(const u16* s) {
  if (!s) return nullptr;
  u32 chars = 0;
  while (s[chars]) ++chars;
  u8* raw = (u8*)malloc((chars + 1) * 2 + 4);
  if (!raw) return nullptr;
  *(u32*)raw = chars * 2;
  u16* str = (u16*)(raw + 4);
  memcpy(str, s, chars * 2);
  str[chars] = 0;
  return str;
}
u16* TRT_GUEST_ABI ole_SysAllocStringLen(const u16* s, u32 chars) {
  u8* raw = (u8*)malloc((chars + 1) * 2 + 4);
  if (!raw) return nullptr;
  *(u32*)raw = chars * 2;
  u16* str = (u16*)(raw + 4);
  if (s && chars) memcpy(str, s, chars * 2);
  str[chars] = 0;
  return str;
}
void TRT_GUEST_ABI ole_SysFreeString(u16* s) {
  if (!s) return;
  free((u8*)s - 4);
}
u32 TRT_GUEST_ABI ole_SysStringLen(const u16* s) {
  if (!s) return 0;
  const u8* raw = (const u8*)s - 4;
  return *(const u32*)raw / 2;
}
u32 TRT_GUEST_ABI ole_SysStringByteLen(const u16* s) {
  if (!s) return 0;
  return *(const u32*)((const u8*)s - 4);
}
// SysAllocStringByteLen: BSTR from ANSI bytes (no conversion — byte copy;
// the embedded NULs an ANSI string may carry are preserved, matching ole32)
u16* TRT_GUEST_ABI ole_SysAllocStringByteLen(const char* s, u32 len) {
  u8* raw = (u8*)malloc((size_t)len + 2 + 4);
  if (!raw) return nullptr;
  *(u32*)raw = len;
  u8* str = raw + 4;
  if (s && len) memcpy(str, s, len);
  str[len] = 0;
  str[len + 1] = 0;
  return (u16*)str;
}
int TRT_GUEST_ABI ole_SysReAllocString(u16** p, const u16* s) {
  if (!p) return 0;
  u16* n = ole_SysAllocString(s);
  if (!n) return 0;
  ole_SysFreeString(*p);
  *p = n;
  return 1;
}
int TRT_GUEST_ABI ole_SysReAllocStringLen(u16** p, const u16* s, u32 chars) {
  if (!p) return 0;
  u16* n = ole_SysAllocStringLen(s, chars);
  if (!n) return 0;
  ole_SysFreeString(*p);
  *p = n;
  return 1;
}

// ---- OLEAUT: variants ----
void TRT_GUEST_ABI ole_VariantInit(void* v) {
  if (v) memset(v, 0, 16);  // VARIANT is 24 bytes on x64
}
void TRT_GUEST_ABI ole_VariantClear(void* v) {
  if (v) memset((u16*)v, 0, 2);  // vt = VT_EMPTY
}
void TRT_GUEST_ABI ole_VariantCopy(void* dst, const void* src) {
  if (dst && src) memcpy(dst, src, 24);
}

// ---- OLEAUT: VariantChangeType (oleaut32 ordinal 12) ----
// The ordinal table used to point 12 at VariantInit, so a title converting a
// value got its destination zeroed and no conversion. This implements the
// conversions automation actually performs: VT_EMPTY, VT_I2/I4/I8, VT_R4/R8,
// VT_BOOL and VT_BSTR in either direction. An unsupported pair returns
// DISP_E_TYPEMISMATCH instead of quietly producing garbage.
#define OLE_DISP_E_TYPEMISMATCH_ ((i32)0x80020005u)

static void ole_wide_from_chars(const char* s, u16* out, u32 cap) {
  u32 i = 0;
  for (; s && s[i] && i + 1 < cap; ++i) out[i] = (u16)(u8)s[i];
  out[i] = 0;
}

// Narrow text of a VARIANT that holds a BSTR (no conversion, byte-for-byte),
// or the empty string when the variant is VT_EMPTY.
i32 TRT_GUEST_ABI ole_VariantChangeType(void* dst, const void* src, u16 flags,
                                        u16 vt) {
  (void)flags;
  if (!dst || !src) return E_INVALIDARG_;
  const u16 svt = *(const u16*)src;
  const u8* sdata = (const u8*)src + 8;
  u8* ddata = (u8*)dst + 8;
  if (svt == vt) {
    ole_VariantCopy(dst, src);
    return S_OK_;
  }

  // A textual view of the source, used by every numeric target.
  char text[64];
  text[0] = 0;
  if (svt == 8 /* VT_BSTR */) {
    const u16* b = *(const u16* const*)sdata;
    if (!b) return OLE_DISP_E_TYPEMISMATCH_;
    u32 i = 0;
    for (; b[i] && i + 1 < sizeof(text); ++i) text[i] = (char)(b[i] & 0x7f);
    text[i] = 0;
  }

  if (vt == 8 /* VT_BSTR */) {
    if (svt == 8) {
      ole_VariantCopy(dst, src);
      return S_OK_;
    }
    if (svt == 3) snprintf(text, sizeof(text), "%d", *(const i32*)sdata);
    else if (svt == 2) snprintf(text, sizeof(text), "%d", (int)*(const i16*)sdata);
    else if (svt == 5) snprintf(text, sizeof(text), "%g", *(const double*)sdata);
    else if (svt == 4) snprintf(text, sizeof(text), "%g", (double)*(const float*)sdata);
    else if (svt == 20) snprintf(text, sizeof(text), "%lld", (long long)*(const i64*)sdata);
    else if (svt == 11) snprintf(text, sizeof(text), "%d", *(const i16*)sdata ? -1 : 0);
    else if (svt == 0) text[0] = 0;  // VT_EMPTY -> empty BSTR
    else return OLE_DISP_E_TYPEMISMATCH_;
    u16 wide[64];
    ole_wide_from_chars(text, wide, 64);
    u16* b = ole_SysAllocString(wide);
    if (!b) return (i32)0x8007000Eu;  // E_OUTOFMEMORY
    ole_VariantClear(dst);
    *(u16*)dst = 8;
    *(void**)((u8*)dst + 8) = b;
    return S_OK_;
  }

  if (vt == 3 || vt == 2 || vt == 20 || vt == 5 || vt == 4) {
    double num = 0.0;
    if (svt == 8) {
      char* end = nullptr;
      num = strtod(text, &end);
      if (end == text) return OLE_DISP_E_TYPEMISMATCH_;  // not a number
    } else if (svt == 3) {
      num = (double)*(const i32*)sdata;
    } else if (svt == 2) {
      num = (double)*(const i16*)sdata;
    } else if (svt == 20) {
      num = (double)*(const i64*)sdata;
    } else if (svt == 5) {
      num = *(const double*)sdata;
    } else if (svt == 4) {
      num = (double)*(const float*)sdata;
    } else if (svt == 11) {
      num = *(const i16*)sdata ? -1.0 : 0.0;
    } else if (svt != 0) {
      return OLE_DISP_E_TYPEMISMATCH_;
    }
    ole_VariantClear(dst);
    *(u16*)dst = vt;
    if (vt == 3) *(i32*)ddata = (i32)num;
    else if (vt == 2) *(i16*)ddata = (i16)num;
    else if (vt == 20) *(i64*)ddata = (i64)num;
    else if (vt == 5) *(double*)ddata = num;
    else *(float*)ddata = (float)num;
    return S_OK_;
  }
  return OLE_DISP_E_TYPEMISMATCH_;
}

// ---- OLEAUT: safe arrays (minimal: 1D, no lock tracking) ----
struct safearray {
  u32 dims;
  u32 features;
  u32 element_bytes;
  u32 locks;
  void* data;
  u32 elements;
  u32 lower;
};
u16* TRT_GUEST_ABI ole_SafeArrayCreateVector(u32 vt, u32 lower, u32 elements) {
  u32 eb = 0;
  switch (vt) {
    case 2: case 3: case 4: eb = 2; break;
    case 5: case 6: case 7: eb = 8; break;
    case 11: eb = 1; break;
    case 16: eb = 1; break;
    case 17: eb = 2; break;
    case 18: eb = 4; break;
    case 19: eb = 8; break;
    case 20: eb = 8; break;
    case 22: eb = 4; break;
    case 23: eb = 4; break;
    case 8: eb = 4; break;
    default: eb = 4; break;
  }
  safearray* sa = (safearray*)malloc(sizeof(safearray));
  if (!sa) return nullptr;
  sa->dims = 1;
  sa->features = 0;
  sa->element_bytes = eb;
  sa->locks = 0;
  sa->elements = elements;
  sa->lower = lower;
  sa->data = elements ? calloc(elements, eb) : nullptr;
  return (u16*)sa;
}
// SafeArrayCreate (oleaut32 ordinal 15): the descriptor form of the call.
// SAFEARRAYBOUND is { ULONG cElements; LONG lLbound; }. This bridge models one
// dimension, so a multi-dimensional request fails visibly (NULL) exactly as the
// real API does when it cannot satisfy the descriptor.
u16* TRT_GUEST_ABI ole_SafeArrayCreate(u32 vt, u32 dims, const void* bounds) {
  if (dims != 1 || !bounds) {
    if (dims > 1)
      trt_core_log("oleaut32: SafeArrayCreate(dims>1) -> NULL (1-D only)");
    return nullptr;
  }
  const u32 elements = *(const u32*)bounds;
  const i32 lower = *(const i32*)((const u8*)bounds + 4);
  return ole_SafeArrayCreateVector(vt, (u32)lower, elements);
}

void TRT_GUEST_ABI ole_SafeArrayDestroy(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return;
  free(sa->data);
  free(sa);
}
int TRT_GUEST_ABI ole_SafeArrayLock(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return E_INVALIDARG_;
  ++sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayUnlock(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return E_INVALIDARG_;
  if (sa->locks) --sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayAccessData(u16* h, void** data) {
  safearray* sa = (safearray*)h;
  if (!sa || !data) return E_INVALIDARG_;
  *data = sa->data;
  ++sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayUnaccessData(u16* h) {
  return ole_SafeArrayUnlock(h);
}
u32 TRT_GUEST_ABI ole_SafeArrayGetDim(u16* h) {
  return h ? 1 : 0;
}
int TRT_GUEST_ABI ole_SafeArrayGetVartype(u16* h, u16* out) {
  static const u16 VT_VARIANT_ = 12;
  if (!h || !out) return E_INVALIDARG_;
  *out = VT_VARIANT_;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayGetUBound(u16* h, u32 dim, i32* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !out || dim != 1) return E_INVALIDARG_;
  *out = (i32)(sa->lower + sa->elements - 1);
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayGetLBound(u16* h, u32 dim, i32* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !out || dim != 1) return E_INVALIDARG_;
  *out = (i32)sa->lower;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayGetElement(u16* h, const i32* idx,
                                          void* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !idx || !out) return E_INVALIDARG_;
  const u32 i = (u32)(*idx - (i32)sa->lower);
  if (i >= sa->elements) return E_INVALIDARG_;
  memcpy(out, (u8*)sa->data + i * sa->element_bytes, sa->element_bytes);
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayPutElement(u16* h, const i32* idx,
                                          const void* v) {
  safearray* sa = (safearray*)h;
  if (!sa || !idx || !v) return E_INVALIDARG_;
  const u32 i = (u32)(*idx - (i32)sa->lower);
  if (i >= sa->elements) return E_INVALIDARG_;
  memcpy((u8*)sa->data + i * sa->element_bytes, v, sa->element_bytes);
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayPtrOfIndex(u16* h, const i32* idx,
                                         void** ppvData) {
  safearray* sa = (safearray*)h;
  if (!sa || !idx || !ppvData) return E_INVALIDARG_;
  const u32 i = (u32)(*idx - (i32)sa->lower);
  if (i >= sa->elements) return E_INVALIDARG_;
  *ppvData = (u8*)sa->data + i * sa->element_bytes;
  return S_OK_;
}

// ---- PropVariant head (PropVariantInit) ----
void TRT_GUEST_ABI ole_PropVariantInit(void* p) {
  if (p) memset(p, 0, 8);
}
int TRT_GUEST_ABI ole_PropVariantCopy(void* dst, const void* src) {
  if (!dst || !src) return E_INVALIDARG_;
  memcpy(dst, src, 24);
  return S_OK_;
}

}  // namespace

int TRT_GUEST_ABI ole_PropVariantClear(void* p) {
  ole_VariantClear(p);
  return S_OK_;
}

// ---- GUID helpers ----
int TRT_GUEST_ABI ole_CoCreateGuid(void* out) {
  if (!out) return E_INVALIDARG_;
  static u8 counter = 0;
  u8* g = (u8*)out;
  for (int i = 0; i < 16; ++i) g[i] = 0;
  g[0] = 0x54; g[1] = 0x49; g[2] = 0x52; g[3] = 0x41;  // "TIRA"
  g[15] = ++counter;
  char line[96];
  snprintf(line, sizeof(line), "ole: CoCreateGuid -> TIRA-shaped stub %p",
           out);
  log_line(line);
  return S_OK_;
}
i32 TRT_GUEST_ABI ole_StringFromGUID2(const void* guid, u16* out, i32 cap) {
  if (!guid || !out || cap < 39) return 0;
  char ascii[40];
  clsid_to_str((const u8*)guid, ascii);
  for (int i = 0; i < 39; ++i) out[i] = (u16)ascii[i];
  out[39] = 0;
  return 39;
}

namespace {

int TRT_GUEST_ABI ole_StringFromCLSID(const void* rclsid, u16** lplpsz) {
  if (!rclsid || !lplpsz) return E_INVALIDARG_;
  u16* buf = (u16*)ole_CoTaskMemAlloc(40 * sizeof(u16));
  if (!buf) return E_OUTOFMEMORY_;
  ole_StringFromGUID2(rclsid, buf, 40);
  *lplpsz = buf;
  return S_OK_;
}
int TRT_GUEST_ABI ole_CLSIDFromString(const u16* lpsz, void* pclsid) {
  if (!pclsid) return E_INVALIDARG_;
  if (!lpsz) {
    memset(pclsid, 0, 16);
    return S_OK_;
  }
  if (!str_to_clsid(lpsz, (u8*)pclsid)) return CO_E_CLASSSTRING_;
  return S_OK_;
}
int TRT_GUEST_ABI ole_IIDFromString(const u16* lpsz, void* lpiid) {
  if (!lpsz) return E_INVALIDARG_;
  return ole_CLSIDFromString(lpsz, lpiid);
}
int TRT_GUEST_ABI ole_ProgIDFromCLSID(const void* clsid, u16** lplpszProgID) {
  (void)clsid;
  if (lplpszProgID) *lplpszProgID = nullptr;
  return REGDB_E_CLASSNOTREG_;
}
int TRT_GUEST_ABI ole_CLSIDFromProgID(const u16* progId, void* clsid) {
  (void)progId;
  if (clsid) memset(clsid, 0, 16);
  return CO_E_CLASSSTRING_;
}
int TRT_GUEST_ABI ole_CoSetProxyBlanket(void* proxy, u32 a, u32 b,
                                        void* c, u32 d, u32 e, void* f,
                                        u32 g) {
  (void)proxy; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
  log_line("ole: CoSetProxyBlanket -> S_OK (no-op, single-process COM)");
  return S_OK_;
}
int TRT_GUEST_ABI ole_CoInitializeSecurity(void* pSecDesc, i32 cAuthSvc,
                                           void* asAuthSvc, void* pReserved1,
                                           u32 dwAuthnLevel, u32 dwImpLevel,
                                           void* pAuthList, u32 dwCapabilities,
                                           void* pReserved3) {
  (void)pSecDesc; (void)cAuthSvc; (void)asAuthSvc; (void)pReserved1;
  (void)dwAuthnLevel; (void)dwImpLevel; (void)pAuthList;
  (void)dwCapabilities; (void)pReserved3;
  return S_OK_;
}

}  // namespace

int TRT_GUEST_ABI ole_RoGetAgileReference(u32 opts, const void* iid,
                                          void* unk, void** out) {
  (void)opts; (void)iid;
  if (!out) return E_INVALIDARG_;
  *out = unk;
  log_line("ole: RoGetAgileReference -> pass-through (single-threaded)");
  return S_OK_;
}

namespace {

// ---- CoCreateFreeThreadedMarshaler ----
struct ftm_stub {
  void** vtbl;   // IUnknown vtable: QI, AddRef, Release
  i32 refs;
};

i32 TRT_GUEST_ABI ftm_QueryInterface(void* self, const void* iid,
                                     void** out) {
  if (!out) return E_INVALIDARG_;
  *out = nullptr;
  static const u8 k_iid_unknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x46};
  if (iid && memcmp(iid, k_iid_unknown, 16) == 0) {
    ftm_stub* s = (ftm_stub*)self;
    ++s->refs;
    *out = self;
    return S_OK_;
  }
  return E_NOINTERFACE_;
}
u32 TRT_GUEST_ABI ftm_AddRef(void* self) {
  ftm_stub* s = (ftm_stub*)self;
  return (u32)++s->refs;
}
u32 TRT_GUEST_ABI ftm_Release(void* self) {
  ftm_stub* s = (ftm_stub*)self;
  const i32 left = --s->refs;
  if (left == 0) free(s);
  return (u32)(left > 0 ? left : 0);
}

}  // namespace

int TRT_GUEST_ABI ole_CoCreateFreeThreadedMarshaler(void* outer,
                                                    void** out) {
  if (!out) return E_INVALIDARG_;
  *out = nullptr;
  if (outer) return E_NOINTERFACE_;
  static void* k_vtbl[3] = {
      (void*)&ftm_QueryInterface, (void*)&ftm_AddRef, (void*)&ftm_Release};
  ftm_stub* s = (ftm_stub*)malloc(sizeof(ftm_stub));
  if (!s) return E_OUTOFMEMORY_;
  s->vtbl = k_vtbl;
  s->refs = 1;
  *out = s;
  char line[96];
  snprintf(line, sizeof(line),
           "ole: CoCreateFreeThreadedMarshaler -> refcounted IUnknown stub %p",
           (void*)s);
  log_line(line);
  return S_OK_;
}

// ---- CoGetApartmentType ----
int TRT_GUEST_ABI ole_CoGetApartmentType(u32* apt_type, u32* qualifier) {
  if (!apt_type || !qualifier) return E_INVALIDARG_;
  *apt_type = 0;
  *qualifier = 0;
  // The caller's own thread, not whichever thread initialized last.
  if (t_init_count <= 0) {
    log_line("ole: CoGetApartmentType -> CO_E_NOTINITIALIZED "
             "(this thread never called CoInitialize)");
    return (int)0x800401F0;  // CO_E_NOTINITIALIZED
  }
  *apt_type = t_init_mta ? 1u /*APT_MTA*/ : 2u /*APT_STA*/;
  *qualifier = 0u /*APTQUALIFIER_CURRENT*/;
  char line[128];
  snprintf(line, sizeof(line), "ole: CoGetApartmentType -> %s tid=%llu",
           t_init_mta ? "MTA" : "STA", (unsigned long long)ole_thread_id());
  log_line(line);
  return S_OK_;
}

// ---- CoWaitForMultipleHandles: the STA wait modern engines use instead of
// MsgWaitForMultipleObjectsEx. On the console there is no real message pump
// to drain inside the wait — the wait rides on the SAME sync primitives the
// kernel32 bridge hands out (trt_WaitForMultipleObjects), so events created
// via kernel32 or NtCreateEvent are all waitable here. COWAIT_WAITALL (0x1)
// is honored; COWAIT_ALERTABLE needs no special handling (no APC pump). ----
int TRT_GUEST_ABI ole_CoWaitForMultipleHandles(u32 flags, u32 timeout_ms,
                                                u32 count,
                                                const void* handles,
                                                u32* index_out) {
  if (!handles || count == 0 || !index_out) return E_INVALIDARG_;
  *index_out = 0;
  char line[96];
  snprintf(line, sizeof(line),
           "ole: CoWaitForMultipleHandles(n=%u flags=%u timeout=%ums)",
           count, flags, timeout_ms);
  log_line(line);
  const u32 r = trt_WaitForMultipleObjects(count, (const u64*)handles,
                                           (flags & 0x1u) != 0, timeout_ms);
  if (r == 0xFFFFFFFFu) return E_HANDLE_;        // wait failed / bad handle
  if (r == 0x102u) return RPC_S_CALLPENDING_;    // timed out
  *index_out = r;                                // signaled index (0 if WAITALL)
  return S_OK_;
}

namespace {

// ---- OleInitialize / DragDrop / ErrorInfo / Marshaling ----
int TRT_GUEST_ABI ole_OleInitialize(void* reserved) {
  return ole_CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void TRT_GUEST_ABI ole_OleUninitialize(void) { ole_CoUninitialize(); }

int TRT_GUEST_ABI ole_RegisterDragDrop(void* hwnd, void* dropTarget) {
  (void)hwnd; (void)dropTarget;
  log_line("ole: RegisterDragDrop -> S_OK (no desktop drag-drop on console)");
  return S_OK_;
}
int TRT_GUEST_ABI ole_RevokeDragDrop(void* hwnd) {
  (void)hwnd;
  return S_OK_;
}
void TRT_GUEST_ABI ole_ReleaseStgMedium(void* pmedium) {
  (void)pmedium;
}
int TRT_GUEST_ABI ole_OleGetClipboard(void** ppDataObj) {
  if (ppDataObj) *ppDataObj = nullptr;
  return E_NOINTERFACE_;
}
int TRT_GUEST_ABI ole_OleSetClipboard(void* pDataObj) {
  (void)pDataObj;
  return S_OK_;
}
int TRT_GUEST_ABI ole_OleFlushClipboard(void) { return S_OK_; }

static void* g_error_info = nullptr;
int TRT_GUEST_ABI ole_SetErrorInfo(u32 dwReserved, void* perrinfo) {
  (void)dwReserved;
  g_error_info = perrinfo;
  return S_OK_;
}
int TRT_GUEST_ABI ole_GetErrorInfo(u32 dwReserved, void** pperrinfo) {
  (void)dwReserved;
  if (!pperrinfo) return E_INVALIDARG_;
  *pperrinfo = g_error_info;
  g_error_info = nullptr;
  return *pperrinfo ? S_OK_ : S_FALSE_;
}

int TRT_GUEST_ABI ole_CoMarshalInterThreadInterfaceInStream(const void* riid,
                                                            void* pUnk,
                                                            void** ppStm) {
  (void)riid;
  if (!ppStm) return E_INVALIDARG_;
  // Single-process COM: pass the interface pointer as the stream token
  *ppStm = pUnk;
  return S_OK_;
}
int TRT_GUEST_ABI ole_CoGetInterfaceAndReleaseStream(void* pStm,
                                                     const void* riid,
                                                     void** ppv) {
  (void)riid;
  if (!ppv) return E_INVALIDARG_;
  *ppv = pStm;
  return S_OK_;
}
u32 TRT_GUEST_ABI ole_CoGetCurrentProcess(void) { return 1; }
int TRT_GUEST_ABI ole_CoGetObjectContext(const void* riid, void** ppv) {
  (void)riid;
  if (ppv) *ppv = nullptr;
  return E_NOINTERFACE_;
}
int TRT_GUEST_ABI ole_CoGetContextToken(u64* pToken) {
  if (!pToken) return E_INVALIDARG_;
  *pToken = 1;
  return S_OK_;
}
int TRT_GUEST_ABI ole_CoRegisterInitializeSpy(void* pSpy, u64* puliCookie) {
  (void)pSpy;
  if (puliCookie) *puliCookie = 1;
  return S_OK_;
}
int TRT_GUEST_ABI ole_CoRevokeInitializeSpy(u64 uliCookie) {
  (void)uliCookie;
  return S_OK_;
}
int TRT_GUEST_ABI ole_CreateStreamOnHGlobal(void* hGlobal, int fDeleteOnRelease,
                                            void** ppstm) {
  (void)hGlobal; (void)fDeleteOnRelease;
  if (ppstm) *ppstm = nullptr;
  log_line("ole: CreateStreamOnHGlobal -> E_NOINTERFACE (named gap)");
  return E_NOINTERFACE_;
}
int TRT_GUEST_ABI ole_GetHGlobalFromStream(void* pstm, void** phglobal) {
  (void)pstm;
  if (phglobal) *phglobal = nullptr;
  return E_INVALIDARG_;
}

}  // namespace

int trt_bridge_register_ole(void) {
  // Ensure WASAPI CLSID_MMDeviceEnumerator is registered into g_classes.
  trt_bridge_register_wasapi();

  static const trt_named_fn k_ole32[] = {
      {"CoInitialize", (void*)&ole_CoInitialize},
      {"CoInitializeEx", (void*)&ole_CoInitializeEx},
      {"CoUninitialize", (void*)&ole_CoUninitialize},
      {"CoCreateInstance", (void*)&ole_CoCreateInstance},
      {"CoGetClassObject", (void*)&ole_CoGetClassObject},
      {"CoGetApartmentType", (void*)&ole_CoGetApartmentType},
      {"CoTaskMemAlloc", (void*)&ole_CoTaskMemAlloc},
      {"CoTaskMemRealloc", (void*)&ole_CoTaskMemRealloc},
      {"CoTaskMemFree", (void*)&ole_CoTaskMemFree},
      {"CoTaskMemSize", (void*)&ole_CoTaskMemSize},
      {"CoFreeUnusedLibraries", (void*)&ole_CoFreeUnusedLibraries},
      {"CoFreeUnusedLibrariesEx", (void*)&ole_CoFreeUnusedLibrariesEx},
      {"OleInitialize", (void*)&ole_OleInitialize},
      {"OleUninitialize", (void*)&ole_OleUninitialize},
      {"CoCreateFreeThreadedMarshaler",
       (void*)&ole_CoCreateFreeThreadedMarshaler},
      {"PropVariantInit", (void*)&ole_PropVariantInit},
      {"PropVariantClear", (void*)&ole_PropVariantClear},
      {"PropVariantCopy", (void*)&ole_PropVariantCopy},
      {"CoCreateGuid", (void*)&ole_CoCreateGuid},
      {"StringFromGUID2", (void*)&ole_StringFromGUID2},
      {"StringFromCLSID", (void*)&ole_StringFromCLSID},
      {"CLSIDFromString", (void*)&ole_CLSIDFromString},
      {"IIDFromString", (void*)&ole_IIDFromString},
      {"ProgIDFromCLSID", (void*)&ole_ProgIDFromCLSID},
      {"CLSIDFromProgID", (void*)&ole_CLSIDFromProgID},
      {"CoSetProxyBlanket", (void*)&ole_CoSetProxyBlanket},
      {"CoInitializeSecurity", (void*)&ole_CoInitializeSecurity},
      {"RoGetAgileReference", (void*)&ole_RoGetAgileReference},
      {"RegisterDragDrop", (void*)&ole_RegisterDragDrop},
      {"RevokeDragDrop", (void*)&ole_RevokeDragDrop},
      {"ReleaseStgMedium", (void*)&ole_ReleaseStgMedium},
      {"OleGetClipboard", (void*)&ole_OleGetClipboard},
      {"OleSetClipboard", (void*)&ole_OleSetClipboard},
      {"OleFlushClipboard", (void*)&ole_OleFlushClipboard},
      {"SetErrorInfo", (void*)&ole_SetErrorInfo},
      {"GetErrorInfo", (void*)&ole_GetErrorInfo},
      {"CoMarshalInterThreadInterfaceInStream",
       (void*)&ole_CoMarshalInterThreadInterfaceInStream},
      {"CoGetInterfaceAndReleaseStream",
       (void*)&ole_CoGetInterfaceAndReleaseStream},
      {"CoGetCurrentProcess", (void*)&ole_CoGetCurrentProcess},
      {"CoGetObjectContext", (void*)&ole_CoGetObjectContext},
      {"CoGetContextToken", (void*)&ole_CoGetContextToken},
      {"CoRegisterInitializeSpy", (void*)&ole_CoRegisterInitializeSpy},
      {"CoRevokeInitializeSpy", (void*)&ole_CoRevokeInitializeSpy},
      {"CreateStreamOnHGlobal", (void*)&ole_CreateStreamOnHGlobal},
      {"GetHGlobalFromStream", (void*)&ole_GetHGlobalFromStream},
      {"CoWaitForMultipleHandles", (void*)&ole_CoWaitForMultipleHandles},
  };
  static const trt_named_fn k_oleaut[] = {
      {"SysAllocString", (void*)&ole_SysAllocString},
      {"SysAllocStringLen", (void*)&ole_SysAllocStringLen},
      {"SysFreeString", (void*)&ole_SysFreeString},
      {"SysStringLen", (void*)&ole_SysStringLen},
      {"SysStringByteLen", (void*)&ole_SysStringByteLen},
      {"SysAllocStringByteLen", (void*)&ole_SysAllocStringByteLen},
      {"SysReAllocString", (void*)&ole_SysReAllocString},
      {"SysReAllocStringLen", (void*)&ole_SysReAllocStringLen},
      {"#2", (void*)&ole_SysAllocString},
      {"#3", (void*)&ole_SysReAllocString},
      {"#4", (void*)&ole_SysAllocStringLen},
      {"#5", (void*)&ole_SysReAllocStringLen},
      {"#6", (void*)&ole_SysFreeString},
      {"#7", (void*)&ole_SysStringLen},
      {"#8", (void*)&ole_VariantInit},
      {"#9", (void*)&ole_VariantClear},
      {"#10", (void*)&ole_VariantCopy},
      // Ordinal 12 is VariantChangeType and ordinal 15 is SafeArrayCreate (the
      // descriptor form). Both were mapped onto the wrong function: 12 cleared
      // the caller's destination instead of converting, and 15 read three
      // scalars where the caller passed one bounds pointer.
      {"#12", (void*)&ole_VariantChangeType},
      {"#15", (void*)&ole_SafeArrayCreate},
      {"#16", (void*)&ole_SafeArrayDestroy},
      {"#17", (void*)&ole_SafeArrayGetDim},
      {"#19", (void*)&ole_SafeArrayGetUBound},
      {"#20", (void*)&ole_SafeArrayGetLBound},
      {"#21", (void*)&ole_SafeArrayLock},
      {"#22", (void*)&ole_SafeArrayUnlock},
      {"#23", (void*)&ole_SafeArrayAccessData},
      {"#24", (void*)&ole_SafeArrayUnaccessData},
      {"#25", (void*)&ole_SafeArrayGetElement},
      {"#26", (void*)&ole_SafeArrayPutElement},
      {"#77", (void*)&ole_SafeArrayGetVartype},
      {"#148", (void*)&ole_SafeArrayPtrOfIndex},
      {"#149", (void*)&ole_SysStringByteLen},
      {"#150", (void*)&ole_SysAllocStringByteLen},
      {"#411", (void*)&ole_SafeArrayCreateVector},
      {"#200", (void*)&ole_GetErrorInfo},
      {"#201", (void*)&ole_SetErrorInfo},
      {"VariantInit", (void*)&ole_VariantInit},
      {"VariantClear", (void*)&ole_VariantClear},
      {"VariantCopy", (void*)&ole_VariantCopy},
      {"SafeArrayCreateVector", (void*)&ole_SafeArrayCreateVector},
      {"SafeArrayDestroy", (void*)&ole_SafeArrayDestroy},
      {"SafeArrayLock", (void*)&ole_SafeArrayLock},
      {"SafeArrayUnlock", (void*)&ole_SafeArrayUnlock},
      {"SafeArrayAccessData", (void*)&ole_SafeArrayAccessData},
      {"SafeArrayUnaccessData", (void*)&ole_SafeArrayUnaccessData},
      {"SafeArrayGetDim", (void*)&ole_SafeArrayGetDim},
      {"SafeArrayGetVartype", (void*)&ole_SafeArrayGetVartype},
      {"SafeArrayGetUBound", (void*)&ole_SafeArrayGetUBound},
      {"SafeArrayGetLBound", (void*)&ole_SafeArrayGetLBound},
      {"SafeArrayGetElement", (void*)&ole_SafeArrayGetElement},
      {"SafeArrayPutElement", (void*)&ole_SafeArrayPutElement},
      {"SafeArrayPtrOfIndex", (void*)&ole_SafeArrayPtrOfIndex},
      {"SetErrorInfo", (void*)&ole_SetErrorInfo},
      {"GetErrorInfo", (void*)&ole_GetErrorInfo},
  };
  trt_module_register("ole32.dll", TRT_ROUTE_BUILTIN, k_ole32,
                      (int)(sizeof(k_ole32) / sizeof(k_ole32[0])));
  trt_module_register("oleaut32.dll", TRT_ROUTE_BUILTIN, k_oleaut,
                      (int)(sizeof(k_oleaut) / sizeof(k_oleaut[0])));
  unsigned cls_count = 0;
  {
    std::lock_guard<std::mutex> lk(g_classes_mu);
    cls_count = (unsigned)g_classes.size();
  }
  char line[96];
  snprintf(line, sizeof(line),
           "ole: ole32 + oleaut32 BUILTIN (%u CLSID personalities)",
           cls_count);
  log_line(line);
  return 0;
}

}  // extern "C"
