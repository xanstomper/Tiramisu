// tiramisu/src/trt/pe_loader.cpp
// Tiramisu Runtime PE32/PE64 loader core. Wine-style in-process mapping:
//   validate -> alloc -> headers+sections -> relocate -> bind imports -> TLS
// Zero RWX: everything maps RW; trt_seal_image() flips code to RX.
// Bounds-checked throughout: a malformed PE must fail with a named error,
// never crash the host process (the host IS the console app).
// M7 Phase 1: PE32 (32-bit) images map through trt_map_pe32 — same stages,
// 4-byte thunks, HIGHLOW relocations against a GUEST-VA base, trap-address
// import binding. LOAD-ONLY: nothing 32-bit executes (no x86 engine until
// Phase 2); the entry point and TLS callbacks are collected, never called.
#include "trt.h"
#include "thunk.h"
#include "dynamic_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>

// 5 GB memory-budget governor (bridge_membudget.cpp): a mapped PE image is
// SizeOfImage bytes of real committed memory — charge it (commit + VA) at
// map time, discharge at unmap / load failure.
extern "C" int trt_mem_budget_charge_commit(u64 bytes);
extern "C" void trt_mem_budget_discharge_commit(u64 bytes);
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
extern "C" void __asan_unpoison_memory_region(const volatile void*, size_t)
    __attribute__((weak));
static const auto asan_unpoison = __asan_unpoison_memory_region;
#  endif
#endif

namespace {

u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

void logf_be(const trt_backend* be, const char* msg) {
  if (be && be->log) be->log(msg);
}

// ---------------------------------------------------------------------------
// Per-thread guest TEB + dynamic .tls template instantiation:
// Guest code reads:
//   gs:[0x08] StackBase, gs:[0x10] StackLimit (MSVC stack checks),
//   gs:[0x30] Self pointer,
//   gs:[0x58] ThreadLocalStoragePointer -> thread's own array[index] -> block.
// Each mapped PE with a TLS directory registers a template (raw bytes +
// zero-fill size). Every OS thread materializes its own independent copy of
// every module's TLS block via epoch checking before entering guest code.
// ---------------------------------------------------------------------------
#define TRT_TEB_SIZE 0x4000u          // shared PEB/fallback storage
#define TRT_TEB_TLS_ARRAY_OFF 0x600u

#if defined(_WIN32)
// Leave the native loader's static TLS indices below the guest's range.
constexpr u32 k_guest_tls_first_index = 1024;
extern "C" unsigned long _tls_index;
#else
constexpr u32 k_guest_tls_first_index = 0;
#endif

struct tls_template {
  u32 index = 0;
  std::vector<u8> raw;
  size_t total_size = 0;
};

} // namespace

static std::mutex g_teb_mu;
static u8* g_guest_teb = nullptr;
static u32 g_guest_tls_next = k_guest_tls_first_index;
static std::atomic<u32> g_tls_epoch{1};
static std::vector<tls_template> g_tls_templates;

struct ThreadTlsState {
  alignas(16) u8 teb[0x2000]{};
  void** slots = nullptr;
  size_t cap = 0;
  std::vector<void**> retired_arrays;
  u32 epoch = 0;
  bool teb_inited = false;
  ~ThreadTlsState() {
    if (slots) {
      for (size_t i = k_guest_tls_first_index; i < cap; ++i) {
        free(slots[i]);
      }
      free(slots);
      slots = nullptr;
    }
    for (void** old : retired_arrays) free(old);
    void** exp = *(void***)(teb + 0x1780);
    if (exp) free(exp);
  }
};

static thread_local ThreadTlsState t_tls_state;

static void fill_teb_stack(u8* teb) {
  // NT_TIB offsets: gs:[0x08] = StackBase (high), gs:[0x10] = StackLimit (low, 4K-aligned).
  u64 stack_lim = ((u64)(uintptr_t)teb - 0x100000) & ~0xFFFULL;
  u64 stack_base = (u64)(uintptr_t)teb & ~0xFFFULL;
#if !defined(_WIN32)
  pthread_attr_t attr;
  void* sb = nullptr;
  size_t ss = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    if (pthread_attr_getstack(&attr, &sb, &ss) == 0 && sb && ss) {
      stack_lim = ((u64)(uintptr_t)sb + 0xFFFULL) & ~0xFFFULL;
      stack_base = (((u64)(uintptr_t)sb + (u64)ss)) & ~0xFFFULL;
    }
    pthread_attr_destroy(&attr);
  }
#endif
  *(u64*)(teb + 0x08) = stack_base;  // StackBase  (high addr)
  *(u64*)(teb + 0x10) = stack_lim;   // StackLimit (low addr)
}

extern "C" u32 TRT_STDCALL trt_GetCurrentThreadId(void);

static u8* ensure_shared_teb() {
  std::lock_guard<std::mutex> lk(g_teb_mu);
  if (!g_guest_teb) {
    u8* teb = (u8*)calloc(1, TRT_TEB_SIZE);
    if (!teb) return nullptr;
    void** tls_array = (void**)(teb + TRT_TEB_TLS_ARRAY_OFF);
    fill_teb_stack(teb);
    // gs:[0x60] = PEB pointer (guest CRT reads it: gs:0x60 -> PEB+0x20
    // NumberOfProcessors etc.). Build a minimal PEB right after the TEB.
    u8* peb = teb + 0x2000;
    *(u64*)(teb + 0x60) = (u64)(uintptr_t)peb;
    static u8* s_pp;
    if (!s_pp) s_pp = (u8*)calloc(1, 0x400);
    *(u64*)(peb + 0x20) = (u64)(uintptr_t)s_pp;
    *(u64*)(peb + 0x28) = 0;
    *(u32*)(peb + 0xB8) = 8;                    // NumberOfProcessors
    *(u64*)(teb + 0x30) = (u64)(uintptr_t)teb;  // Self
    *(u64*)(teb + 0x40) = 0x1000;               // ClientId.UniqueProcess
    *(u64*)(teb + 0x48) = (u64)trt_GetCurrentThreadId();  // ClientId.UniqueThread
    *(void**)(teb + 0x58) = (void*)tls_array;
    g_guest_teb = teb;
  }
  return g_guest_teb;
}

static void sync_thread_tls_state() {
  ThreadTlsState& ts = t_tls_state;
  const u32 cur_epoch = g_tls_epoch.load(std::memory_order_acquire);
  if (ts.teb_inited && ts.epoch == cur_epoch && ts.slots != nullptr) return;

  u8* shared = ensure_shared_teb();
  if (!shared) return;

  if (!ts.teb_inited) {
    memset(ts.teb, 0, sizeof(ts.teb));
    fill_teb_stack(ts.teb);
    *(u64*)(ts.teb + 0x30) = (u64)(uintptr_t)ts.teb;  // Self
    *(u64*)(ts.teb + 0x40) = 0x1000;                  // ClientId.UniqueProcess
    *(u64*)(ts.teb + 0x48) = (u64)trt_GetCurrentThreadId();  // ClientId.UniqueThread
    *(u64*)(ts.teb + 0x60) = (u64)(uintptr_t)(shared + 0x2000);  // PEB
    ts.teb_inited = true;
  }

  std::lock_guard<std::mutex> lk(g_teb_mu);
  const size_t needed = (size_t)g_guest_tls_next;
  size_t target_cap = ts.cap ? ts.cap : 512;
  while (target_cap < needed) target_cap *= 2;
  if (!ts.slots || target_cap > ts.cap) {
    void** next = (void**)calloc(target_cap, sizeof(void*));
    if (!next) return;
    if (ts.slots) {
      memcpy(next, ts.slots, ts.cap * sizeof(void*));
      ts.retired_arrays.push_back(ts.slots);
    }
    ts.slots = next;
    ts.cap = target_cap;
  }

  for (const tls_template& tpl : g_tls_templates) {
    if (tpl.index < ts.cap && ts.slots[tpl.index] == nullptr) {
      const size_t alloc_sz = tpl.total_size ? tpl.total_size : 1;
      u8* block = (u8*)malloc(alloc_sz);
      if (!block) continue;
      if (!tpl.raw.empty()) memcpy(block, tpl.raw.data(), tpl.raw.size());
      if (alloc_sz > tpl.raw.size()) {
        memset(block + tpl.raw.size(), 0, alloc_sz - tpl.raw.size());
      }
      ts.slots[tpl.index] = block;
    }
  }

  *(void**)(ts.teb + 0x58) = (void*)ts.slots;
  ts.epoch = g_tls_epoch.load(std::memory_order_relaxed);
}

int trt_guest_tls_register(const u8* raw, size_t raw_size, size_t total_size,
                           u32* out_index) {
  if (!out_index) return -1;
  {
    std::lock_guard<std::mutex> lk(g_teb_mu);
    const u32 idx = g_guest_tls_next++;
    tls_template tpl;
    tpl.index = idx;
    tpl.total_size = total_size ? total_size : (raw_size ? raw_size : 1);
    if (raw && raw_size > 0) {
      tpl.raw.assign(raw, raw + raw_size);
    }
    g_tls_templates.push_back(std::move(tpl));
    g_tls_epoch.fetch_add(1, std::memory_order_release);
    *out_index = idx;
  }
  sync_thread_tls_state();
  return 0;
}

void trt_guest_tls_sync_thread(void) {
  sync_thread_tls_state();
}

void** trt_guest_tls_array(void) {
  sync_thread_tls_state();
  return t_tls_state.slots;
}

#if defined(_WIN32)
static void bind_native_module_tls(HMODULE module, void** native, void** guest) {
  if (!module) return;
  // The platform supplies mapped module handles. A module can still unload
  // between enumeration and this read, so guard the borrowed pointers.
  __try {
    const auto* base = reinterpret_cast<const u8*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS) return;
    const auto& entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!entry.VirtualAddress || entry.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + entry.VirtualAddress);
    if (!tls->AddressOfIndex) return;
    const DWORD index = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
    if (index < k_guest_tls_first_index) guest[index] = native[index];
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#endif

void trt_guest_tls_bind_host_slots(void** native, void** guest) {
#if defined(_WIN32)
  if (!native || !guest || native == guest) return;
  const u32 index = (u32)_tls_index;
  if (index < k_guest_tls_first_index) guest[index] = native[index];

  using enum_modules = BOOL (WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
  static const auto modules = reinterpret_cast<enum_modules>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
  HMODULE loaded[256];
  DWORD bytes = 0;
  if (modules && modules(GetCurrentProcess(), loaded, sizeof(loaded), &bytes)) {
    const size_t count = bytes < sizeof(loaded) ? bytes / sizeof(HMODULE)
                                               : sizeof(loaded) / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) bind_native_module_tls(loaded[i], native, guest);
  } else {
    // The Xbox AppContainer can restrict process enumeration. Preserve the
    // loaded system components that the direct graphics path calls into.
    const wchar_t* graphics_modules[] = {
        L"d3d11.dll", L"d3d11on12.dll", L"d3d12.dll", L"D3D12Core.dll",
        L"dxgi.dll", L"d3dcompiler_47.dll", L"winrtbase.dll", L"combase.dll"};
    for (const auto* name : graphics_modules)
      bind_native_module_tls(GetModuleHandleW(name), native, guest);
  }
#else
  (void)native;
  (void)guest;
#endif
}

void* trt_guest_teb_acquire(u32* out_index) {
  u8* shared = ensure_shared_teb();
  if (!shared) return nullptr;
  if (out_index) {
    if (trt_guest_tls_register(nullptr, 0, 0, out_index) != 0) return nullptr;
    return shared;
  }
  sync_thread_tls_state();
  return t_tls_state.teb;
}

// ---------------------------------------------------------------------------
// M7 Phase 1: 32-bit guest address space + import trap registry.
//
// A 32-bit guest lives in its own 4 GB VA space: its preferred ImageBase
// (0x400000, 0x10000000, ...) is a GUEST address, not a host address. The
// host mapping can sit anywhere (be->alloc picks); the guest base is
// tracked separately so relocations and IAT guest-VAs are computed against
// the address the EMULATOR will use (Phase 2), not the host pointer.
// Guest images live below 0x70000000 (well under the 2 GB user-space split
// and far from the 0xF0000000 trap range). The extent table also answers
// host-pointer -> guest-VA translation for sibling-DLL export binding.
// ---------------------------------------------------------------------------
struct pe32_guest_extent {
  u8* host_base;   // host mapping (nullptr = free slot)
  u64 host_size;
  u32 guest_base;  // guest-VA base
};
static pe32_guest_extent g_pe32_extents[1024];
static int g_pe32_extent_count = 0;
static std::mutex g_pe32_mu;

#define TRT_PE32_GUEST_ALIGN 0x10000u   // 64 KiB allocation granularity
#define TRT_PE32_GUEST_FLOOR 0x10000000u  // bump-alloc scan starts here
#define TRT_PE32_GUEST_CEIL  0x70000000u

static bool pe32_guest_overlaps(u32 base, u64 size) {
  const u64 end = (u64)base + size;
  for (int i = 0; i < g_pe32_extent_count; ++i) {
    if (!g_pe32_extents[i].host_base) continue;
    const u64 b = g_pe32_extents[i].guest_base;
    const u64 e = b + g_pe32_extents[i].host_size;
    if ((u64)base < e && b < end) return true;
  }
  return false;
}

// Reserve a guest-VA range for a new PE32 image: preferred base first,
// else the lowest free 64 KiB-aligned slot at/above the floor. Records the
// (host, guest) pair for host->guest translation. Returns 0 on success.
static int pe32_guest_reserve(u8* host_base, u64 size, u32 preferred,
                              u32* out_guest_base) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  const u64 span =
      (size + TRT_PE32_GUEST_ALIGN - 1) & ~(u64)(TRT_PE32_GUEST_ALIGN - 1);
  u32 chosen = 0;
  if (preferred && (u64)preferred + span <= TRT_PE32_GUEST_CEIL &&
      !pe32_guest_overlaps(preferred, span)) {
    chosen = preferred;
  } else {
    for (u64 cand = TRT_PE32_GUEST_FLOOR;
         cand + span <= TRT_PE32_GUEST_CEIL;
         cand += TRT_PE32_GUEST_ALIGN) {
      if (!pe32_guest_overlaps((u32)cand, span)) {
        chosen = (u32)cand;
        break;
      }
    }
    if (!chosen) return -1;
  }
  int slot = -1;
  for (int i = 0; i < g_pe32_extent_count; ++i)
    if (!g_pe32_extents[i].host_base) {
      slot = i;
      break;
    }
  if (slot < 0) {
    if (g_pe32_extent_count >= 1024) return -1;
    slot = g_pe32_extent_count++;
  }
  g_pe32_extents[slot].host_base = host_base;
  g_pe32_extents[slot].host_size = size;
  g_pe32_extents[slot].guest_base = chosen;
  *out_guest_base = chosen;
  return 0;
}

static void pe32_guest_release_by_host(u8* host_base) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  for (int i = 0; i < g_pe32_extent_count; ++i)
    if (g_pe32_extents[i].host_base == host_base) {
      g_pe32_extents[i].host_base = nullptr;
      g_pe32_extents[i].host_size = 0;
      g_pe32_extents[i].guest_base = 0;
    }
}

// Translate a host pointer inside a mapped PE32 image to its guest VA.
// Returns 1 on success, 0 when the pointer is not inside any PE32 image.
static int pe32_guest_va_for_host(const void* p, u32* out_va) {
  const uintptr_t hp = (uintptr_t)p;
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  for (int i = 0; i < g_pe32_extent_count; ++i) {
    if (!g_pe32_extents[i].host_base) continue;
    const uintptr_t b = (uintptr_t)g_pe32_extents[i].host_base;
    const uintptr_t e = b + g_pe32_extents[i].host_size;
    if (hp >= b && hp < e) {
      *out_va = g_pe32_extents[i].guest_base + (u32)(hp - b);
      return 1;
    }
  }
  return 0;
}

extern "C" void trt_pe32_guest_reset(void) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  for (int i = 0; i < g_pe32_extent_count; ++i) {
    g_pe32_extents[i].host_base = nullptr;
    g_pe32_extents[i].host_size = 0;
    g_pe32_extents[i].guest_base = 0;
  }
  g_pe32_extent_count = 0;
}

// ---------------------------------------------------------------------------
// M7 Phase 1: 32-bit import trap registry.
// One entry per unique bound module!symbol (deduped, case-insensitive
// module). Trap N lives at TRT_PE32_TRAP_BASE + 4*N. The registry is
// process-global and never reset (like the TLS templates): trap addresses
// recorded in already-mapped IATs stay valid for the process lifetime.
// A delay-import binding starts with host_fn == nullptr (unresolved at
// load); a later static bind of the same symbol upgrades it.
// ---------------------------------------------------------------------------
struct pe32_trap_binding {
  char module[64];
  char symbol[128];  // "" when by ordinal
  u16 ordinal;       // 0 when by name
  u32 trap_va;
  void* host_fn;     // the 64-bit bridge address (Phase 2 dispatcher input)
  int is_delay;      // 1 = delay-import binding (fail-visible at load)
};
static std::vector<pe32_trap_binding> g_pe32_traps;

static bool pe32_mod_ieq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// Allocate (or reuse) the trap address for one bound import. Returns 0 on
// success, -1 when the trap table is full.
static int pe32_trap_alloc(const char* module, const char* symbol, u16 ordinal,
                           void* host_fn, int is_delay, u32* out_trap) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  for (auto& t : g_pe32_traps) {
    if (!pe32_mod_ieq(t.module, module)) continue;
    if (symbol && t.symbol[0] && strcmp(t.symbol, symbol) == 0) {
      if (!t.host_fn && host_fn) t.host_fn = host_fn;  // upgrade delay stub
      *out_trap = t.trap_va;
      return 0;
    }
    if (!symbol && !t.symbol[0] && t.ordinal == ordinal) {
      if (!t.host_fn && host_fn) t.host_fn = host_fn;
      *out_trap = t.trap_va;
      return 0;
    }
  }
  if (g_pe32_traps.size() >= TRT_PE32_TRAP_MAX) return -1;
  pe32_trap_binding t;
  memset(&t, 0, sizeof(t));
  snprintf(t.module, sizeof(t.module), "%s", module);
  if (symbol) snprintf(t.symbol, sizeof(t.symbol), "%s", symbol);
  t.ordinal = ordinal;
  t.trap_va = TRT_PE32_TRAP_BASE + 4u * (u32)g_pe32_traps.size();
  t.host_fn = host_fn;
  t.is_delay = is_delay;
  *out_trap = t.trap_va;
  g_pe32_traps.push_back(t);
  return 0;
}

// Bind one resolved 32-bit import to its 4-byte IAT value:
//   - a host pointer inside a mapped PE32 image (a sibling DLL export)
//     becomes the export's GUEST VA (emulated code calls emulated code);
//   - anything else (a 64-bit bridge address) becomes a trap address from
//     the reserved range (Phase 2 dispatches it to the bridge).
// Returns 0 on success, -1 when the trap table is full.
static int pe32_bind_value(void* host_fn, const char* module,
                           const char* symbol, u16 ordinal, int is_delay,
                           u32* out_slot, int* out_is_trap) {
  u32 va = 0;
  if (host_fn && pe32_guest_va_for_host(host_fn, &va)) {
    *out_slot = va;
    *out_is_trap = 0;
    return 0;
  }
  u32 trap = 0;
  if (pe32_trap_alloc(module, symbol, ordinal, host_fn, is_delay, &trap) != 0)
    return -1;
  *out_slot = trap;
  *out_is_trap = 1;
  return 0;
}

// Blanket-0 stub for an import the bridge surface does not implement yet.
// Binding it keeps the image mappable so the boot ladder can name the next
// real gap, instead of refusing to load the whole executable. Every use is
// logged by name at load time (never a silent fake success).
extern "C" u32 trt_pe32_missing_import(void) { return 0; }

extern "C" int trt_pe32_trap_count(void) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  return (int)g_pe32_traps.size();
}

extern "C" int trt_pe32_trap_info(int index, char* mod_out, size_t mod_cap,
                                  char* sym_out, size_t sym_cap,
                                  u16* ordinal_out, u32* trap_va_out) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  if (index < 0 || (size_t)index >= g_pe32_traps.size()) return -1;
  const pe32_trap_binding& t = g_pe32_traps[(size_t)index];
  if (mod_out && mod_cap) snprintf(mod_out, mod_cap, "%s", t.module);
  if (sym_out && sym_cap) snprintf(sym_out, sym_cap, "%s", t.symbol);
  if (ordinal_out) *ordinal_out = t.ordinal;
  if (trap_va_out) *trap_va_out = t.trap_va;
  return 0;
}

extern "C" int trt_pe32_is_trap(u32 guest_va) {
  return guest_va >= TRT_PE32_TRAP_BASE && guest_va < TRT_PE32_TRAP_END;
}

// Bind a symbol resolved at runtime (GetProcAddress). The guest gets back a
// trap VA, never a host function address: a 64-bit pointer does not survive a
// 32-bit register, and the caller would jump into unmapped space.
extern "C" u32 trt_pe32_bind_dynamic(const char* module, const char* symbol,
                                     u32 ordinal, void* host_fn) {
  u32 trap = 0;
  if (pe32_trap_alloc(module && module[0] ? module : "?", symbol, (u16)ordinal,
                      host_fn, 0, &trap) != 0)
    return 0;
  return trap;
}

// The 64-bit bridge address bound to trap N (nullptr for a delay import that
// has not been upgraded yet). Phase 2 dispatches through this.
extern "C" void* trt_pe32_trap_host_fn(int index) {
  std::lock_guard<std::mutex> lk(g_pe32_mu);
  if (index < 0 || (size_t)index >= g_pe32_traps.size()) return nullptr;
  return g_pe32_traps[(size_t)index].host_fn;
}

// fail: record stage detail, release mapping, return error code
int fail(trt_image* img, const trt_backend* be, int code, const char* detail) {
  if (detail && img) {
    strncpy(img->last_detail, detail, sizeof(img->last_detail) - 1);
    img->last_detail[sizeof(img->last_detail) - 1] = 0;
  }
  char line[320];
  snprintf(line, sizeof(line), "trt: FAIL %s: %s", trt_errstr(code),
           detail ? detail : "");
  logf_be(be, line);
  if (img && img->base && be) {
    if (img->is_pe32) pe32_guest_release_by_host(img->base);
    be->release(img->base, (size_t)img->size_of_image);
    trt_mem_budget_discharge_commit(img->size_of_image);
    trt_mem_budget_discharge_va(img->size_of_image);
    img->base = nullptr;
  }
  return code;
}

// section target protection per PE characteristics (pre-seal everything RW;
// seal flips exec to RX)
unsigned section_prot(bool exec) { return exec ? (TRT_PROT_R | TRT_PROT_X)
                                               : (TRT_PROT_R | TRT_PROT_W); }

extern "C" const char* trt_errstr(int code) {
  switch (code) {
    case TRT_OK: return "OK";
    case TRT_ERR_BAD_DOS: return "BAD_DOS (not MZ / e_lfanew out of file)";
    case TRT_ERR_BAD_PE: return "BAD_PE (no PE signature)";
    case TRT_ERR_NOT_PE64: return "NOT_PE64 (machine/magic mismatch: neither PE64 (0x8664/0x020b) nor PE32 (0x014c/0x010b))";
    case TRT_ERR_NO_SECTIONS: return "NO_SECTIONS";
    case TRT_ERR_SIZE_OVERFLOW: return "SIZE_OVERFLOW (SizeOfImage missing or over 2 GB cap)";
    case TRT_ERR_SECTION_BOUNDS: return "SECTION_BOUNDS (section points outside file/image)";
    case TRT_ERR_NO_BASE: return "NO_BASE (fixed base required but unavailable)";
    case TRT_ERR_ALLOC: return "ALLOC (backend allocation failed)";
    case TRT_ERR_RELOC_BADTYPE: return "RELOC_BADTYPE";
    case TRT_ERR_RELOC_BOUNDS: return "RELOC_BOUNDS";
    case TRT_ERR_IMPORT: return "IMPORT (unresolvable symbol)";
    case TRT_ERR_TLS_BOUNDS: return "TLS_BOUNDS";
    case TRT_ERR_SEAL: return "SEAL (final protect failed)";
    case TRT_ERR_DELAY_IMPORT: return "DELAY_IMPORT (delay-load directory not processed in v0)";
    case TRT_ERR_MANAGED: return "MANAGED (.NET assembly: CLR header present, no .NET Framework CLR on console)";
    case TRT_ERR_MONO: return "MONO (Mono embedding failure; see trt_mono_last_error)";
    case TRT_ERR_PE32_EXECUTION: return "PE32_EXECUTION (see interpreter diagnostics)";
    case TRT_ERR_FNA3D: return "FNA3D (FNA3D host-device handoff failure; see trt_fna3d_last_error)";
    default: return "UNKNOWN";
  }
}

extern "C" int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                           const u8* file, size_t file_size, trt_image** out) {
  if (!be || !rs || !file || !out || file_size < sizeof(trt_dos_header))
    return TRT_ERR_BAD_ARGS;
  *out = nullptr;

  trt_image img;
  memset(&img, 0, sizeof(img));

  // ---- 1. DOS header ----
  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return fail(&img, be, TRT_ERR_BAD_DOS, "no MZ");
  u64 nt_off = dos->e_lfanew;
  if (nt_off < sizeof(trt_dos_header) || nt_off + sizeof(trt_nt_headers64) > file_size)
    return fail(&img, be, TRT_ERR_BAD_DOS, "e_lfanew out of file");

  // ---- 2. NT headers ----
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550)
    return fail(&img, be, TRT_ERR_BAD_PE, "no PE\\0\\0");
  // M7 Phase 1: 32-bit images route to the PE32 path (same loader stages,
  // 4-byte thunks, guest-VA base, trap-address imports). Machine and magic
  // live at the same offsets in both optional headers, so these reads are
  // width-safe. (The nt_off + sizeof(nt_headers64) bound above is
  // conservative for PE32: a valid PE32 needs nt_off + 248 + 40*nsec bytes
  // anyway, so no valid PE32 is rejected by it.)
  const u16 machine = rd16((const u8*)&nt->file_header.machine);
  const u16 magic = rd16((const u8*)&nt->optional_header.magic);
  if (machine == TRT_PE_MACHINE_I386 && magic == TRT_PE_OPT32_MAGIC)
    return trt_map_pe32(be, rs, file, file_size, out);
  if (machine != TRT_PE_MACHINE_X64 || magic != TRT_PE_OPT64_MAGIC)
    return fail(&img, be, TRT_ERR_NOT_PE64, "neither PE64 (0x8664/0x020b) nor PE32 (0x014c/0x010b)");
  u32 nsec = rd16((const u8*)&nt->file_header.number_of_sections);
  if (nsec == 0 || nsec > 96)
    return fail(&img, be, TRT_ERR_NO_SECTIONS, "section count 0 or >96");
  u64 size_of_image = rd32((const u8*)&nt->optional_header.size_of_image);
  u64 section_alignment = rd32((const u8*)&nt->optional_header.section_alignment);
  if (size_of_image == 0 || size_of_image > TRT_PE_MAX_IMAGE_BYTES ||
      section_alignment < 4096)
    return fail(&img, be, TRT_ERR_SIZE_OVERFLOW, "SizeOfImage/alignment insane");

  const u64 preferred = rd64((const u8*)&nt->optional_header.image_base);
  const u64 entry_rva = rd32((const u8*)&nt->optional_header.address_of_entry_point);
  const u64 size_headers = rd32((const u8*)&nt->optional_header.size_of_headers);
  const trt_data_directory* dirs = nt->optional_header.directories;

  // ---- 2b. managed (.NET) assemblies ----
  // A PE with a COM descriptor (data directory 14) is a .NET assembly: its
  // entry point is a stub that jumps to mscoree.dll!_CorExeMain, and there
  // is no .NET Framework CLR on the console (no mscoree/clr.dll, no GAC,
  // no fusion, no XNA redist). Fail fast with a dedicated code instead of
  // dying later at import bind with an unexplained mscoree.dll error.
  // Deliberately NOT a STUB: binding _CorExeMain to a thunk that returns 0
  // would make the game "exit 0" silently, which is worse than failing.
  if (rd32((const u8*)&nt->optional_header.number_of_rva_and_sizes) >
          TRT_PE_DIR_COM_DESCRIPTOR &&
      dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0)
    return fail(&img, be, TRT_ERR_MANAGED,
                "managed .NET assembly (COM descriptor present); no CLR on console");

  // section headers live right after optional header, in the file
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  if ((size_t)(sec_base - file) + (size_t)nsec * sizeof(trt_section_header) > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section headers out of file");

  // ---- 3. allocate ----
  const u16 file_chars = rd16((const u8*)&nt->file_header.characteristics);
  const u16 dll_chars = rd16((const u8*)&nt->optional_header.dll_characteristics);
  const bool has_relocs = dirs[TRT_PE_DIR_RELOC].virtual_address != 0 &&
                          dirs[TRT_PE_DIR_RELOC].size != 0;
  // A /DYNAMICBASE image without RELOCS_STRIPPED whose .reloc directory is empty
  // contains zero absolute fixups (pure RIP-relative code) and can safely run
  // off preferred base (critical on Xbox UWP where VirtualAllocFromApp cannot
  // guarantee arbitrary ImageBase).
  const bool can_relocate =
      has_relocs ||
      ((dll_chars & 0x0040u /*IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE*/) &&
       !(file_chars & 0x0001u /*IMAGE_FILE_RELOCS_STRIPPED*/));
  void* base = be->alloc((size_t)size_of_image, preferred);
  if (!base && can_relocate) {        // retry anywhere; relocs (if any) will fix it up
    base = be->alloc((size_t)size_of_image, 0);
    if (base) logf_be(be, "trt: preferred base unavailable, relocated");
  }
  if (!base)
    return fail(&img, be, can_relocate ? TRT_ERR_ALLOC : TRT_ERR_NO_BASE,
                "VirtualAlloc/map failed");
  // the image is real committed memory: charge SizeOfImage (commit + VA)
  // to the guest budget; a refusal releases the reservation and fails the
  // load with a named budget error instead of an uncharged image
  if (trt_mem_budget_charge_va(size_of_image) != 0) {
    be->release(base, (size_t)size_of_image);
    return fail(&img, be, TRT_ERR_ALLOC, "VA budget refused image");
  }
  if (trt_mem_budget_charge_commit(size_of_image) != 0) {
    trt_mem_budget_discharge_va(size_of_image);
    be->release(base, (size_t)size_of_image);
    return fail(&img, be, TRT_ERR_ALLOC, "memory budget refused image");
  }
  memset(base, 0, (size_t)size_of_image);

  img.base = (u8*)base;
  img.preferred_base = preferred;
  img.size_of_image = size_of_image;
  img.entry_rva = entry_rva;
  img.section_count = nsec;
  // exception directory (.pdata) — an array of RUNTIME_FUNCTION sorted by
  // BeginRva; the image copies it verbatim so unwind lookups can binary
  // search it after seal.
  {
    const u64 pd_va = dirs[TRT_PE_DIR_EXCEPTION].virtual_address;
    const u64 pd_sz = dirs[TRT_PE_DIR_EXCEPTION].size;
    if (pd_va && pd_sz >= 12 && pd_va + pd_sz <= size_of_image) {
      img.pdata = img.base + pd_va;
      img.pdata_count = (u32)(pd_sz / 12);
    }
  }

  // ---- 4. headers + sections ----
  if (size_headers > size_of_image || size_headers > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "SizeOfHeaders out of bounds");
  memcpy(img.base, file, (size_t)size_headers);

  u64 max_end = size_headers;
  for (u32 i = 0; i < nsec; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const u64 rsize = rd32((const u8*)&sh->size_of_raw_data);
    const u64 roff = rd32((const u8*)&sh->pointer_to_raw_data);
    const u64 mem_sz = vsize ? vsize : rsize;
    if (va + mem_sz > size_of_image)
      return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section VA+VSize > SizeOfImage");
    if (rsize) {
      if (roff + rsize > file_size)
        return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section raw data out of file");
      // SizeOfRawData is FileAlignment-padded on disk and can exceed VirtualSize
      // (and SizeOfImage) on the last section; PE loaders map at most mem_sz bytes.
      u64 copy_sz = (vsize && rsize > vsize) ? vsize : rsize;
      if (copy_sz > size_of_image - va) copy_sz = size_of_image - va;
      memcpy(img.base + va, file + roff, (size_t)copy_sz);
    }
    // BSS tail already zero from memset
    if (va + mem_sz > max_end) max_end = va + mem_sz;
    char line[96];
    snprintf(line, sizeof(line), "trt: sec[%u] %.8s va=%llx sz=%llx%s",
             i, (const char*)sh->name, (unsigned long long)va,
             (unsigned long long)vsize, (rd32((const u8*)&sh->characteristics) & TRT_SCN_MEM_EXEC) ? " exec" : "");
    logf_be(be, line);
  }
  if (max_end > size_of_image)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "sections exceed SizeOfImage");

  const u64 delta = (u64)(uintptr_t)img.base - preferred;

  // ---- 5. relocations (only when loaded off preferred base) ----
  if (delta != 0) {
    if (!can_relocate)
      return fail(&img, be, TRT_ERR_NO_BASE, "delta != 0 and no reloc directory");
    if (!has_relocs) {
      logf_be(be, "trt: dynamic-base image has empty .reloc (0 fixups); relocated");
    } else {
      const u64 dir_va = dirs[TRT_PE_DIR_RELOC].virtual_address;
      const u64 dir_sz = dirs[TRT_PE_DIR_RELOC].size;
    if (dir_va + dir_sz > size_of_image)
      return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc dir out of image");
    u64 off = 0;
    u32 n_fixed = 0;
    while (off + 8 <= dir_sz) {
      const u8* blk = img.base + dir_va + off;
      const u32 page_rva = rd32(blk);
      const u32 block_size = rd32(blk + 4);
      if (block_size < 8 || off + block_size > dir_sz)
        return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc block size corrupt");
      const u32 n_entries = (block_size - 8) / 2;
      for (u32 e = 0; e < n_entries; ++e) {
        const u16 entry = rd16(blk + 8 + (size_t)e * 2);
        const u32 type = entry >> 12;
        const u32 fo = entry & 0x0FFF;
        if (type == TRT_RELOC_ABSOLUTE) continue;
        const u64 tgt_rva = page_rva + fo;
        if (tgt_rva + 8 > size_of_image)
          return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc target out of image");
        u8* p = img.base + tgt_rva;
        if (type == TRT_RELOC_DIR64) {
          u64 v = rd64(p); v += delta; memcpy(p, &v, 8);
        } else if (type == TRT_RELOC_HIGHLOW) {
          if (tgt_rva + 4 > size_of_image)
            return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "HIGHLOW out of image");
          u32 v = rd32(p); v += (u32)delta; memcpy(p, &v, 4);
        } else {
          char d[64]; snprintf(d, sizeof(d), "reloc type %u unsupported", type);
          return fail(&img, be, TRT_ERR_RELOC_BADTYPE, d);
        }
        ++n_fixed;
      }
      off += block_size;
    }
      img.reloc_fixups = n_fixed;
      char line[96];
      snprintf(line, sizeof(line), "trt: reloc delta=%llx fixed=%u",
               (unsigned long long)delta, n_fixed);
      logf_be(be, line);
    }
  } else {
    logf_be(be, "trt: loaded at preferred base, no reloc pass");
  }

  // ---- 6. imports ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  u32 n_modules = 0, n_symbols = 0;
  if (imp_va && imp_sz) {
    if (imp_va + imp_sz > size_of_image)
      return fail(&img, be, TRT_ERR_IMPORT, "import dir out of image");
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      const trt_import_descriptor* desc =
          (const trt_import_descriptor*)(img.base + imp_va + d);
      if (!desc->first_thunk && !desc->name) break;  // terminator
      if (desc->name >= size_of_image || desc->first_thunk >= size_of_image)
        return fail(&img, be, TRT_ERR_IMPORT, "import descriptor out of image");
      const char* mod_name = (const char*)(img.base + desc->name);
      const u64 int_rva = desc->original_first_thunk ? desc->original_first_thunk
                                                     : desc->first_thunk;
      const u64 iat_rva = desc->first_thunk;
      u32 bound = 0;
      for (u64 t = iat_rva;; t += 8) {
        const u64 lt = int_rva + (t - iat_rva);
        if (t + 8 > size_of_image || lt + 8 > size_of_image)
          return fail(&img, be, TRT_ERR_IMPORT, "IAT runs off image");
        const u64 thunk = rd64(img.base + lt);
        if (thunk == 0) break;  // terminator
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image)
            return fail(&img, be, TRT_ERR_IMPORT, "hint/name out of image");
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256)
            return fail(&img, be, TRT_ERR_IMPORT, "symbol name not terminated");
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          // Universal Dynamic API Blanket: synthesize fail-visible adaptive thunk
          fn = trt_dynamic_synthesize_api(mod_name, sym_name, ordinal);
        }
        if (!fn) {
          char det[224];
          snprintf(det, sizeof(det), "%s!%s", mod_name,
                   sym_name ? sym_name : "#ordinal");
          return fail(&img, be, TRT_ERR_IMPORT, det);
        }
        memcpy(img.base + t, &fn, 8);  // bind IAT
        if (n_symbols == 0) {
          char line[256];
          snprintf(line, sizeof(line), "trt: bind %s!%s -> %p", mod_name,
                   sym_name ? sym_name : "#ordinal", fn);
          logf_be(be, line);
        }
        ++bound; ++n_symbols;
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: import %s bound=%u", mod_name, bound);
      logf_be(be, line);
      ++n_modules;
    }
  } else {
    logf_be(be, "trt: no import directory");
  }
  (void)n_modules; (void)n_symbols;

  // ---- 7. TLS callbacks (collected; caller runs them after seal) ----
  const u64 tls_va = dirs[TRT_PE_DIR_TLS].virtual_address;
  if (tls_va) {
    if (tls_va + sizeof(trt_tls_directory64) > size_of_image)
      return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls dir out of image");
    const trt_tls_directory64* tls = (const trt_tls_directory64*)(img.base + tls_va);
    // A TLS directory with AddressOfCallbacks == 0 is legal (TLS data, no
    // callbacks): zero callbacks, NOT a load failure. Without this guard
    // the VA math below underflows to a huge rva and dies TLS_BOUNDS.
    if (tls->address_of_callbacks == 0) {
      img.tls_callback_count = 0;
      logf_be(be, "trt: tls dir: AddressOfCallbacks==0, no callbacks");
    } else {
      // callbacks array is addressed by VA; after relocation it points at the
      // RUNTIME image (new base + rva), so translate through the actual base —
      // NOT the preferred ImageBase. Subtracting `preferred` only worked when
      // the image loaded at its preferred base (delta==0); the first relocated
      // TLS-bearing DLL (baselib.dll inside GameAssembly's run) produced a
      // huge bogus rva and died TLS_BOUNDS. STAGE 6 run-depth found it.
      const u64 arr_rva =
          (u64)tls->address_of_callbacks - (u64)(uintptr_t)img.base;
      if (arr_rva + 8 > size_of_image)
        return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls callback array out of image");
      u64 c = arr_rva;
      for (; c + 8 <= size_of_image &&
             img.tls_callback_count < TRT_MAX_TLS_CALLBACKS; c += 8) {
        const u64 cb = rd64(img.base + c);
        if (cb == 0) break;
        const u64 cb_rva = cb - (u64)(uintptr_t)img.base;
        if (cb_rva >= size_of_image)
          return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls callback out of image");
        img.tls_callback_rvas[img.tls_callback_count++] = cb_rva;
      }
      // the cap is a real limit, not a silent drop: say so when entries remain
      if (img.tls_callback_count >= TRT_MAX_TLS_CALLBACKS &&
          c + 8 <= size_of_image && rd64(img.base + c) != 0) {
        char capline[128];
        snprintf(capline, sizeof(capline),
                 "trt: tls callbacks truncated at cap=%d (more entries present)",
                 TRT_MAX_TLS_CALLBACKS);
        logf_be(be, capline);
      }
    }
    char line[96];
    snprintf(line, sizeof(line), "trt: tls callbacks=%d", img.tls_callback_count);
    logf_be(be, line);

    // ---- 7b. guest TLS array (STAGE 7): Windows stores a per-thread pointer
    // to an array of module TLS blocks at gs:[0x58] (the TEB's
    // ThreadLocalStoragePointer), and the module's TLS *index* at the
    // address_of_index slot. Guest code compiled with __declspec(thread)
    // reads its block as gs:[0x58] -> array -> array[index]. Without gs
    // pointing at a TEB, the first such access faults (measured live under
    // gdb: UnityPlayer.dll DllMain, mov %gs:0x58,%rax). We build the real
    // structure: index slot = the next free TLS slot, block = the raw TLS
    // data (copy + zero-fill) in a never-freed allocation, TEB slot = the
    // array. trt_tls_set_gs_base() installs the TEB as the guest gs base
    // before the entry runs (host gs must be preserved, so the guest gs
    // base is set per-call in trt_call_guest and restored after).
    {
      struct tls_dir_full {
        u64 start, end, index_va, callbacks_va;
        u32 zero_fill, chars;
      };
      const tls_dir_full* tf = (const tls_dir_full*)(img.base + tls_va);
      const u64 raw_start_rva = (u64)tf->start - (u64)(uintptr_t)img.base;
      const u64 raw_end_rva = (u64)tf->end - (u64)(uintptr_t)img.base;
      const u64 raw_size =
          raw_end_rva > raw_start_rva ? raw_end_rva - raw_start_rva : 0;
      const u64 block_size = raw_size + tf->zero_fill;
      const u64 index_rva = (u64)tf->index_va - (u64)(uintptr_t)img.base;
      char probe[160];
      snprintf(probe, sizeof(probe),
               "trt: tls dir probe: start_rva=%llx end_rva=%llx index_rva=%llx zero_fill=%u block=%llu",
               (unsigned long long)raw_start_rva,
               (unsigned long long)raw_end_rva,
               (unsigned long long)index_rva, tf->zero_fill,
               (unsigned long long)block_size);
      logf_be(be, probe);
      if (index_rva + 4 <= size_of_image && block_size > 0) {
        u32 tls_index = 0;
        const u8* raw_ptr = raw_size ? (img.base + raw_start_rva) : nullptr;
        if (trt_guest_tls_register(raw_ptr, (size_t)raw_size,
                                   (size_t)block_size, &tls_index) == 0) {
          img.guest_teb = ensure_shared_teb();
          *(u32*)(img.base + index_rva) = tls_index;
          char tline[160];
          snprintf(tline, sizeof(tline),
                   "trt: tls block size=%llu index=%u (per-thread TEB templates)",
                   (unsigned long long)block_size, tls_index);
          logf_be(be, tline);
        }
      }
    }
  }

  // ---- 8. delay imports (delay-load directory) ----
  // Games delay-load optional DLLs (d3dcompiler_47, xaudio2_9, ...) and
  // expect LoadLibrary to fail gracefully when they are absent. We bind
  // them EAGERLY: resolvable symbols get the real address, unresolvable
  // ones get a fail-visible thunk so the game still boots and the log
  // names the gap instead of the load dying.
  if (dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address) {
    const u64 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
    const u64 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
    u32 n_delay_mods = 0, n_delay_bound = 0, n_delay_stub = 0;
    for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= (dd_sz ? dd_sz : 4096);
         d += sizeof(trt_delayload_descriptor)) {
      if (dd_va + d + sizeof(trt_delayload_descriptor) > size_of_image) break;
      const trt_delayload_descriptor* dd =
          (const trt_delayload_descriptor*)(img.base + dd_va + d);
      if (!dd->name_rva && !dd->iat_rva) break;  // terminator
      const int rva_based = (dd->attributes & 1u) != 0;
      // legacy descriptors store VAs: convert to RVAs
      const u64 name_rva = rva_based ? dd->name_rva
                                     : (u64)dd->name_rva - preferred;
      const u64 iat_rva = rva_based ? dd->iat_rva : (u64)dd->iat_rva - preferred;
      const u64 int_rva = rva_based ? dd->int_rva : (u64)dd->int_rva - preferred;
      const u64 mod_rva = rva_based ? dd->module_handle_rva
                                    : (u64)dd->module_handle_rva - preferred;
      if (name_rva >= size_of_image || iat_rva >= size_of_image ||
          int_rva >= size_of_image)
        return fail(&img, be, TRT_ERR_DELAY_IMPORT, "delay descriptor out of image");
      const char* mod_name = (const char*)(img.base + name_rva);
      if (strnlen(mod_name, 256) == 256)
        return fail(&img, be, TRT_ERR_DELAY_IMPORT, "delay dll name unterminated");
      for (u64 t = 0;; t += 8) {
        if (int_rva + t + 8 > size_of_image || iat_rva + t + 8 > size_of_image)
          break;
        const u64 thunk = rd64(img.base + int_rva + t);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image) break;
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          // Dynamic API Blanket: synthesize intelligent adaptive thunk with deduced return value
          fn = trt_dynamic_synthesize_api(mod_name, sym_name, ordinal);
        }
        if (fn) {
          memcpy(img.base + iat_rva + t, &fn, 8);
          ++n_delay_bound;
        } else {
          char sym[128];
          if (sym_name) snprintf(sym, sizeof(sym), "%s", sym_name);
          else snprintf(sym, sizeof(sym), "#%u", (unsigned)ordinal);
          void* stub = trt_thunk_make(be, "DELAY", mod_name, sym);
          if (!stub) stub = (void*)&trt_stub_thunk;
          memcpy(img.base + iat_rva + t, &stub, 8);
          ++n_delay_stub;
        }
      }
      // the guest may ask for the module handle: give it a non-null sentinel
      if (mod_rva && mod_rva + 8 <= size_of_image) {
        u64 sentinel = 0x1000;
        memcpy(img.base + mod_rva, &sentinel, 8);
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: DELAY import %s bound=%u stub=%u",
               mod_name, n_delay_bound, n_delay_stub);
      logf_be(be, line);
      ++n_delay_mods;
    }
    char line[128];
    snprintf(line, sizeof(line), "trt: delay imports modules=%u bound=%u stub=%u",
             n_delay_mods, n_delay_bound, n_delay_stub);
    logf_be(be, line);
  }

  *out = (trt_image*)malloc(sizeof(trt_image));
  if (!*out) return fail(&img, be, TRT_ERR_ALLOC, "image struct alloc");
  memcpy(*out, &img, sizeof(img));
  char line[160];
  snprintf(line, sizeof(line),
           "trt: MAPPED base=%llx preferred=%llx size=%llx entry=%llx",
           (unsigned long long)(uintptr_t)img.base,
           (unsigned long long)preferred,
           (unsigned long long)size_of_image, (unsigned long long)entry_rva);
  logf_be(be, line);
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
  // ASan cannot see mmap-backed guest memory: every legitimate guest write
  // into its own image reads as a wild "unknown-crash" (measured: the D3D
  // surface memsets at the image base). Unpoison the whole image so ASan
  // supervises guest writes as valid memory — real overflows past the image
  // are still caught.
  if (asan_unpoison) asan_unpoison(img.base, size_of_image);
#  endif
#endif
  return TRT_OK;
}

// ---------------------------------------------------------------------------
// M7 Phase 1: PE32 (32-bit) mapping. Same stages as trt_map_pe, with the
// 32-bit differences called out at each stage:
//   headers:  trt_nt_headers32 (ImageBase/stack/heap are u32; dirs at +96)
//   alloc:    host mapping anywhere; the GUEST base is reserved separately
//             (preferred guest base first, else the lowest free 64 KiB slot
//             at/above 0x10000000) — relocation is a guest-space concept
//   relocs:   HIGHLOW only (DIR64 in a PE32 image is malformed); the delta
//             is guest_base - preferred, NOT host_base - preferred
//   imports:  4-byte thunks, ordinal flag bit 31; a resolved import binds
//             to a sibling export's GUEST VA or to a reserved trap address
//   TLS:      32-bit TLS directory; callbacks collected as RVAs; the TLS
//             template is RECORDED (tls_*_rva fields) and the index slot
//             left unwritten — the Phase 2 emulator assigns FS-based TLS
//   seal:     trt_seal_image handles PE32 (no .pdata unwind registration)
// LOAD-ONLY: the entry point and TLS callbacks are never called here.
// ---------------------------------------------------------------------------
extern "C" int trt_map_pe32(const trt_backend* be, const trt_resolver* rs,
                            const u8* file, size_t file_size, trt_image** out) {
  if (!be || !rs || !file || !out || file_size < sizeof(trt_dos_header))
    return TRT_ERR_BAD_ARGS;
  *out = nullptr;

  trt_image img;
  memset(&img, 0, sizeof(img));
  img.is_pe32 = 1;

  // ---- 1. DOS header ----
  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return fail(&img, be, TRT_ERR_BAD_DOS, "no MZ");
  u64 nt_off = dos->e_lfanew;
  if (nt_off < sizeof(trt_dos_header) || nt_off + sizeof(trt_nt_headers32) > file_size)
    return fail(&img, be, TRT_ERR_BAD_DOS, "e_lfanew out of file");

  // ---- 2. NT headers (PE32) ----
  const trt_nt_headers32* nt = (const trt_nt_headers32*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550)
    return fail(&img, be, TRT_ERR_BAD_PE, "no PE\\0\\0");
  if (rd16((const u8*)&nt->file_header.machine) != TRT_PE_MACHINE_I386 ||
      rd16((const u8*)&nt->optional_header.magic) != TRT_PE_OPT32_MAGIC)
    return fail(&img, be, TRT_ERR_NOT_PE64, "not PE32 (0x014c/0x010b) on this path");
  // The optional header must at least cover its fixed part (96 bytes) plus
  // the data directories it claims; otherwise the directory reads below
  // would silently consume section-header bytes as RVAs.
  const u16 opt_size = rd16((const u8*)&nt->file_header.size_of_optional_header);
  if (opt_size < 96)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "optional header < 96 bytes");
  const u32 num_dirs = rd32((const u8*)&nt->optional_header.number_of_rva_and_sizes);
  if (num_dirs > TRT_PE_NUM_DIRECTORIES ||
      (u32)opt_size < 96 + 8 * num_dirs)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS,
                "optional header too small for data directories");
  u32 nsec = rd16((const u8*)&nt->file_header.number_of_sections);
  if (nsec == 0 || nsec > 96)
    return fail(&img, be, TRT_ERR_NO_SECTIONS, "section count 0 or >96");
  u64 size_of_image = rd32((const u8*)&nt->optional_header.size_of_image);
  u64 section_alignment = rd32((const u8*)&nt->optional_header.section_alignment);
  if (size_of_image == 0 || size_of_image > TRT_PE_MAX_IMAGE_BYTES ||
      section_alignment < 4096)
    return fail(&img, be, TRT_ERR_SIZE_OVERFLOW, "SizeOfImage/alignment insane");

  const u32 preferred32 = rd32((const u8*)&nt->optional_header.image_base);
  const u64 preferred = preferred32;
  const u64 entry_rva = rd32((const u8*)&nt->optional_header.address_of_entry_point);
  const u64 size_headers = rd32((const u8*)&nt->optional_header.size_of_headers);
  // Directories beyond number_of_rva_and_sizes do not exist: copy the
  // claimed entries into a zeroed local array so a short directory table
  // can never be misread as section-header bytes.
  trt_data_directory dirs_buf[TRT_PE_NUM_DIRECTORIES];
  memset(dirs_buf, 0, sizeof(dirs_buf));
  memcpy(dirs_buf, nt->optional_header.directories,
         sizeof(trt_data_directory) * num_dirs);
  const trt_data_directory* dirs = dirs_buf;

  // section headers live right after optional header, in the file
  const u8* sec_base = (const u8*)&nt->optional_header + opt_size;
  if ((size_t)(sec_base - file) + (size_t)nsec * sizeof(trt_section_header) > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section headers out of file");

  // ---- 3. allocate (host) + reserve (guest) ----
  const u16 file_chars = rd16((const u8*)&nt->file_header.characteristics);
  const u16 dll_chars = rd16((const u8*)&nt->optional_header.dll_characteristics);
  const bool has_relocs = dirs[TRT_PE_DIR_RELOC].virtual_address != 0 &&
                          dirs[TRT_PE_DIR_RELOC].size != 0;
  const bool can_relocate =
      has_relocs ||
      ((dll_chars & 0x0040u /*IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE*/) &&
       !(file_chars & 0x0001u /*IMAGE_FILE_RELOCS_STRIPPED*/));
  // PE32 host allocation NEVER requests a fixed address: the host mapping
  // address is arbitrary (all fixups use the guest-VA delta, and 32-bit
  // code never executes on the host). Requesting the preferred base would
  // be actively dangerous — 0x400000 is both the classic PE32 EXE base
  // AND the Linux non-PIE load address, so a MAP_FIXED there overwrites
  // the running process (segfault, found by the Celeste.exe corpus run).
  // The guest base is reserved independently below.
  void* base = be->alloc((size_t)size_of_image, 0);
  if (!base)
    return fail(&img, be, TRT_ERR_ALLOC, "VirtualAlloc/map failed");
  if (trt_mem_budget_charge_va(size_of_image) != 0) {
    be->release(base, (size_t)size_of_image);
    return fail(&img, be, TRT_ERR_ALLOC, "VA budget refused image");
  }
  if (trt_mem_budget_charge_commit(size_of_image) != 0) {
    trt_mem_budget_discharge_va(size_of_image);
    be->release(base, (size_t)size_of_image);
    return fail(&img, be, TRT_ERR_ALLOC, "memory budget refused image");
  }
  memset(base, 0, (size_t)size_of_image);

  img.base = (u8*)base;
  img.preferred_base = preferred;
  img.size_of_image = size_of_image;
  img.entry_rva = entry_rva;
  img.section_count = nsec;

  // Guest-VA reservation: independent of the host mapping. Two DLLs that
  // share a preferred base (every 0x10000000 DLL in the Celeste corpus)
  // get distinct guest bases; the loser relocates (HIGHLOW pass below).
  u32 guest_base = 0;
  if (pe32_guest_reserve((u8*)base, size_of_image, preferred32, &guest_base) != 0)
    return fail(&img, be, TRT_ERR_ALLOC, "guest VA space exhausted (PE32)");
  img.guest_base = guest_base;
  // PE32 has no .pdata unwind tables (SEH is the FS:0 chain); leave pdata
  // null so no caller tries to register unwind ranges for this image.
  img.pdata = nullptr;
  img.pdata_count = 0;

  // ---- 4. headers + sections (section headers are width-agnostic) ----
  if (size_headers > size_of_image || size_headers > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "SizeOfHeaders out of bounds");
  memcpy(img.base, file, (size_t)size_headers);

  u64 max_end = size_headers;
  for (u32 i = 0; i < nsec; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const u64 rsize = rd32((const u8*)&sh->size_of_raw_data);
    const u64 roff = rd32((const u8*)&sh->pointer_to_raw_data);
    const u64 mem_sz = vsize ? vsize : rsize;
    if (va + mem_sz > size_of_image)
      return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section VA+VSize > SizeOfImage");
    if (rsize) {
      if (roff + rsize > file_size)
        return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section raw data out of file");
      u64 copy_sz = (vsize && rsize > vsize) ? vsize : rsize;
      if (copy_sz > size_of_image - va) copy_sz = size_of_image - va;
      memcpy(img.base + va, file + roff, (size_t)copy_sz);
    }
    // BSS tail already zero from memset
    if (va + mem_sz > max_end) max_end = va + mem_sz;
    char line[96];
    snprintf(line, sizeof(line), "trt: PE32 sec[%u] %.8s va=%llx sz=%llx%s",
             i, (const char*)sh->name, (unsigned long long)va,
             (unsigned long long)vsize, (rd32((const u8*)&sh->characteristics) & TRT_SCN_MEM_EXEC) ? " exec" : "");
    logf_be(be, line);
  }
  if (max_end > size_of_image)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "sections exceed SizeOfImage");

  // The relocation delta is a GUEST-space delta: the host mapping address
  // is invisible to guest code, so a host-side relocation needs no fixups.
  const u32 delta32 = guest_base - preferred32;

  // ---- 5. relocations (only when the guest base differs from preferred) ----
  if (delta32 != 0) {
    if (!can_relocate)
      return fail(&img, be, TRT_ERR_NO_BASE, "guest delta != 0 and no reloc directory");
    if (!has_relocs) {
      logf_be(be, "trt: PE32 dynamic-base image has empty .reloc (0 fixups); relocated");
    } else {
      const u64 dir_va = dirs[TRT_PE_DIR_RELOC].virtual_address;
      const u64 dir_sz = dirs[TRT_PE_DIR_RELOC].size;
      if (dir_va + dir_sz > size_of_image)
        return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc dir out of image");
      u64 off = 0;
      u32 n_fixed = 0;
      while (off + 8 <= dir_sz) {
        const u8* blk = img.base + dir_va + off;
        const u32 page_rva = rd32(blk);
        const u32 block_size = rd32(blk + 4);
        if (block_size < 8 || off + block_size > dir_sz)
          return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc block size corrupt");
        const u32 n_entries = (block_size - 8) / 2;
        for (u32 e = 0; e < n_entries; ++e) {
          const u16 entry = rd16(blk + 8 + (size_t)e * 2);
          const u32 type = entry >> 12;
          const u32 fo = entry & 0x0FFF;
          if (type == TRT_RELOC_ABSOLUTE) continue;
          const u64 tgt_rva = page_rva + fo;
          if (tgt_rva + 4 > size_of_image)
            return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc target out of image");
          u8* p = img.base + tgt_rva;
          if (type == TRT_RELOC_HIGHLOW) {
            u32 v = rd32(p); v += delta32; memcpy(p, &v, 4);
          } else if (type == TRT_RELOC_DIR64) {
            return fail(&img, be, TRT_ERR_RELOC_BADTYPE,
                        "DIR64 reloc in a PE32 image (malformed)");
          } else {
            char d[64]; snprintf(d, sizeof(d), "reloc type %u unsupported", type);
            return fail(&img, be, TRT_ERR_RELOC_BADTYPE, d);
          }
          ++n_fixed;
        }
        off += block_size;
      }
      img.reloc_fixups = n_fixed;
      char line[96];
      snprintf(line, sizeof(line), "trt: PE32 reloc guest_delta=%x fixed=%u",
               delta32, n_fixed);
      logf_be(be, line);
    }
  } else {
    logf_be(be, "trt: PE32 loaded at preferred guest base, no reloc pass");
  }

  // Publish the allocated, section-mapped, relocated image to *out BEFORE
  // walking imports so circular DLL dependencies can resolve exports
  // against this in-progress image without re-entering trt_map_pe.
  *out = (trt_image*)malloc(sizeof(trt_image));
  if (!*out) return fail(&img, be, TRT_ERR_ALLOC, "image struct alloc");
  memcpy(*out, &img, sizeof(img));
  auto fail_out = [&](int code, const char* detail) -> int {
    if (*out) {
      free(*out);
      *out = nullptr;
    }
    return fail(&img, be, code, detail);
  };

  // ---- 6. imports (4-byte thunks; ordinal flag is bit 31) ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  u32 n_modules = 0, n_symbols = 0;
  if (imp_va && imp_sz) {
    if (imp_va + imp_sz > size_of_image)
      return fail_out(TRT_ERR_IMPORT, "import dir out of image");
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      const trt_import_descriptor* desc =
          (const trt_import_descriptor*)(img.base + imp_va + d);
      if (!desc->first_thunk && !desc->name) break;  // terminator
      if (desc->name >= size_of_image || desc->first_thunk >= size_of_image)
        return fail_out(TRT_ERR_IMPORT, "import descriptor out of image");
      const char* mod_name = (const char*)(img.base + desc->name);
      const u64 int_rva = desc->original_first_thunk ? desc->original_first_thunk
                                                     : desc->first_thunk;
      const u64 iat_rva = desc->first_thunk;
      u32 bound = 0;
      for (u64 t = iat_rva;; t += 4) {
        const u64 lt = int_rva + (t - iat_rva);
        if (t + 4 > size_of_image || lt + 4 > size_of_image)
          return fail_out(TRT_ERR_IMPORT, "IAT runs off image");
        const u32 thunk = rd32(img.base + lt);
        if (thunk == 0) break;  // terminator
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk & 0x80000000u) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image)
            return fail_out(TRT_ERR_IMPORT, "hint/name out of image");
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256)
            return fail_out(TRT_ERR_IMPORT, "symbol name not terminated");
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          // Named gap, not a load failure: bind the blanket-0 stub so the
          // guest reaches its own code and the ladder names the next blocker.
          char line[256];
          snprintf(line, sizeof(line),
                   "trt: PE32 import %s!%s is a NAMED GAP (bound to a 0-returning stub)",
                   mod_name, sym_name ? sym_name : "#ordinal");
          logf_be(be, line);
          fn = (void*)&trt_pe32_missing_import;
        }
        u32 slot = 0;
        int is_trap = 0;
        if (pe32_bind_value(fn, mod_name, sym_name, ordinal, 0, &slot, &is_trap) != 0)
          return fail_out(TRT_ERR_IMPORT, "PE32 trap table full");
        memcpy(img.base + t, &slot, 4);  // bind IAT (4 bytes)
        if (is_trap) ++img.pe32_trap_count; else ++img.pe32_guestva_count;
        if (n_symbols == 0) {
          char line[256];
          snprintf(line, sizeof(line), "trt: PE32 bind %s!%s -> %08x%s",
                   mod_name, sym_name ? sym_name : "#ordinal", slot,
                   is_trap ? " (trap)" : " (guest VA)");
          logf_be(be, line);
        }
        ++bound; ++n_symbols;
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: PE32 import %s bound=%u", mod_name, bound);
      logf_be(be, line);
      ++n_modules;
    }
  } else {
    logf_be(be, "trt: PE32 no import directory");
  }
  (void)n_modules; (void)n_symbols;

  // ---- 7. TLS (32-bit directory; callbacks collected as RVAs) ----
  const u64 tls_va = dirs[TRT_PE_DIR_TLS].virtual_address;
  if (tls_va) {
    if (tls_va + sizeof(trt_tls_directory32) > size_of_image)
      return fail_out(TRT_ERR_TLS_BOUNDS, "tls dir out of image");
    const trt_tls_directory32* tls = (const trt_tls_directory32*)(img.base + tls_va);
    // The TLS directory's VAs are GUEST VAs: the reloc pass above already
    // adjusted them by delta32 (when fixups cover them), so translate
    // through the guest base — never the host base.
    const u32 cb_va = tls->address_of_callbacks;
    if (cb_va == 0) {
      img.tls_callback_count = 0;
      logf_be(be, "trt: PE32 tls dir: AddressOfCallbacks==0, no callbacks");
    } else {
      const u64 arr_rva = (u64)cb_va - (u64)guest_base;
      if (arr_rva + 4 > size_of_image)
        return fail_out(TRT_ERR_TLS_BOUNDS, "tls callback array out of image");
      u64 c = arr_rva;
      for (; c + 4 <= size_of_image &&
             img.tls_callback_count < TRT_MAX_TLS_CALLBACKS; c += 4) {
        const u32 cb = rd32(img.base + c);
        if (cb == 0) break;
        const u64 cb_rva = (u64)cb - (u64)guest_base;
        if (cb_rva >= size_of_image)
          return fail_out(TRT_ERR_TLS_BOUNDS, "tls callback out of image");
        img.tls_callback_rvas[img.tls_callback_count++] = cb_rva;
      }
      if (img.tls_callback_count >= TRT_MAX_TLS_CALLBACKS &&
          c + 4 <= size_of_image && rd32(img.base + c) != 0) {
        logf_be(be, "trt: PE32 tls callbacks truncated at cap (more entries present)");
      }
    }
    // TLS template: RECORDED for the Phase 2 emulator's FS-based TLS
    // (32-bit code reads fs:[0x2C], not gs:[0x58]). The index slot is
    // LEFT UNWRITTEN — the 64-bit gs-based TEB machinery is the wrong
    // model here, and writing a 64-bit TLS index into a 32-bit image
    // would be a lie the emulator would have to undo.
    {
      const u64 raw_start_rva =
          (u64)tls->start_address_of_raw_data - (u64)guest_base;
      const u64 raw_end_rva =
          (u64)tls->end_address_of_raw_data - (u64)guest_base;
      const u64 index_rva = (u64)tls->address_of_index - (u64)guest_base;
      const u64 raw_size =
          raw_end_rva > raw_start_rva ? raw_end_rva - raw_start_rva : 0;
      if (raw_start_rva <= size_of_image && index_rva + 4 <= size_of_image) {
        img.tls_data_rva = (u32)raw_start_rva;
        img.tls_data_size = (u32)raw_size;
        img.tls_zero_fill = tls->size_of_zero_fill;
        img.tls_index_rva = (u32)index_rva;
      }
    }
    char line[128];
    snprintf(line, sizeof(line),
             "trt: PE32 tls callbacks=%d data_rva=%x size=%u zerofill=%u",
             img.tls_callback_count, img.tls_data_rva, img.tls_data_size,
             img.tls_zero_fill);
    logf_be(be, line);
  }

  // ---- 8. delay imports (4-byte thunks; unresolvable -> trap, not fatal) ----
  // Same eager-bind discipline as the 64-bit path: resolvable symbols get
  // their guest VA / trap, unresolvable ones get a trap address recorded
  // with a NULL host fn — the Phase 2 dispatcher logs the call
  // fail-visible instead of calling. A delay import never fails the load.
  if (dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address) {
    const u64 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
    const u64 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
    u32 n_delay_mods = 0, n_delay_bound = 0, n_delay_stub = 0;
    for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= (dd_sz ? dd_sz : 4096);
         d += sizeof(trt_delayload_descriptor)) {
      if (dd_va + d + sizeof(trt_delayload_descriptor) > size_of_image) break;
      const trt_delayload_descriptor* dd =
          (const trt_delayload_descriptor*)(img.base + dd_va + d);
      if (!dd->name_rva && !dd->iat_rva) break;  // terminator
      const int rva_based = (dd->attributes & 1u) != 0;
      // Legacy descriptors store VAs, not RVAs. The value read from the
      // mapped image is post-reloc: a guest VA when fixups cover the
      // field, a preferred-base VA when they do not. Try the guest base
      // first, fall back to the preferred base; the bounds check below
      // rejects anything that lands outside the image either way.
      auto va_to_rva = [&](u32 field) -> u64 {
        if (rva_based) return field;
        const u64 rg = (u64)field - (u64)guest_base;
        if (rg < size_of_image) return rg;
        return (u64)field - preferred;
      };
      const u64 name_rva = va_to_rva(dd->name_rva);
      const u64 iat_rva = va_to_rva(dd->iat_rva);
      const u64 int_rva = va_to_rva(dd->int_rva);
      const u64 mod_rva = va_to_rva(dd->module_handle_rva);
      if (name_rva >= size_of_image || iat_rva >= size_of_image ||
          int_rva >= size_of_image)
        return fail_out(TRT_ERR_DELAY_IMPORT, "delay descriptor out of image");
      const char* mod_name = (const char*)(img.base + name_rva);
      if (strnlen(mod_name, 256) == 256)
        return fail_out(TRT_ERR_DELAY_IMPORT, "delay dll name unterminated");
      for (u64 t = 0;; t += 4) {
        if (int_rva + t + 4 > size_of_image || iat_rva + t + 4 > size_of_image)
          break;
        const u32 thunk = rd32(img.base + int_rva + t);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk & 0x80000000u) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image) break;
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          // Dynamic API Blanket: synthesize intelligent adaptive thunk with deduced return value
          fn = trt_dynamic_synthesize_api(mod_name, sym_name, ordinal);
        }
        u32 slot = 0;
        int is_trap = 0;
        if (fn) {
          if (pe32_bind_value(fn, mod_name, sym_name, ordinal, 1, &slot,
                              &is_trap) != 0)
            return fail_out(TRT_ERR_DELAY_IMPORT, "PE32 trap table full");
          memcpy(img.base + iat_rva + t, &slot, 4);
          if (is_trap) ++img.pe32_trap_count; else ++img.pe32_guestva_count;
          ++n_delay_bound;
        } else {
          if (pe32_trap_alloc(mod_name, sym_name, ordinal, nullptr, 1,
                              &slot) != 0)
            return fail_out(TRT_ERR_DELAY_IMPORT, "PE32 trap table full");
          memcpy(img.base + iat_rva + t, &slot, 4);
          ++img.pe32_trap_count;
          ++n_delay_stub;
        }
      }
      // the guest may ask for the module handle: give it a non-null sentinel
      if (mod_rva && mod_rva + 4 <= size_of_image) {
        u32 sentinel = 0x1000;
        memcpy(img.base + mod_rva, &sentinel, 4);
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: PE32 DELAY import %s bound=%u stub=%u",
               mod_name, n_delay_bound, n_delay_stub);
      logf_be(be, line);
      ++n_delay_mods;
    }
    char line[128];
    snprintf(line, sizeof(line),
             "trt: PE32 delay imports modules=%u bound=%u stub=%u",
             n_delay_mods, n_delay_bound, n_delay_stub);
    logf_be(be, line);
  }

  memcpy(*out, &img, sizeof(img));
  char line[192];
  snprintf(line, sizeof(line),
           "trt: PE32 MAPPED host=%llx guest=%08x preferred=%08x size=%llx entry=%llx traps=%u guestva=%u",
           (unsigned long long)(uintptr_t)img.base, guest_base, preferred32,
           (unsigned long long)size_of_image, (unsigned long long)entry_rva,
           img.pe32_trap_count, img.pe32_guestva_count);
  logf_be(be, line);
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
  if (asan_unpoison) asan_unpoison(img.base, size_of_image);
#  endif
#endif
  return TRT_OK;
}

// Undo the W^X seal: every section back to RW. Needed by the ONLY code path
// that writes into a sealed image -- the 32-bit legacy runner
// (trt_x86_run_cdecl), which executes on a private copy and copies the
// guest's data writes back afterwards. Writing into RX pages there is a host
// SIGSEGV, so the copy-back requires the pre-seal protection. The seal is
// re-applied immediately after, so the image is never RWX.
extern "C" int trt_unseal_image(const trt_backend* be, trt_image* img) {
  if (!be || !img || !img->base) return TRT_ERR_BAD_ARGS;
  const trt_dos_header* dos = (const trt_dos_header*)img->base;
  const trt_nt_headers64* nt =
      (const trt_nt_headers64*)(img->base + dos->e_lfanew);
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  for (u32 i = 0; i < img->section_count; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u32 va = rd32((const u8*)&sh->virtual_address);
    const u32 vsize = rd32((const u8*)&sh->virtual_size);
    if (be->protect(img->base + va, (size_t)vsize ? (size_t)vsize : 4096,
                    TRT_PROT_R | TRT_PROT_W)) {
      char d[96];
      snprintf(d, sizeof(d), "unseal protect failed on section %u", i);
      return fail(img, be, TRT_ERR_SEAL, d);
    }
  }
  return TRT_OK;
}

extern "C" int trt_seal_image(const trt_backend* be, trt_image* img) {
  if (!be || !img || !img->base) return TRT_ERR_BAD_ARGS;
  // walk section table in the mapped copy of the headers (NT headers sit at
  // e_lfanew, not at offset 0 — the DOS header occupies the page start)
  const trt_dos_header* dos = (const trt_dos_header*)img->base;
  const trt_nt_headers64* nt =
      (const trt_nt_headers64*)(img->base + dos->e_lfanew);
  // The section table is located via size_of_optional_header, which is
  // width-agnostic — but the exception directory is NOT (PE32 dirs start
  // 16 bytes earlier), so read the magic first.
  const u16 magic = rd16((const u8*)&nt->optional_header.magic);
  const int is_pe32 = (magic == TRT_PE_OPT32_MAGIC);
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  for (u32 i = 0; i < img->section_count; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u32 ch = rd32((const u8*)&sh->characteristics);
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const bool exec = (ch & TRT_SCN_MEM_EXEC) != 0;
    const unsigned prot = section_prot(exec);
    if (be->protect(img->base + va, (size_t)vsize ? (size_t)vsize : 4096, prot)) {
      char d[96];
      snprintf(d, sizeof(d), "protect failed on section %u (prot=%u)", i, prot);
      return fail(img, be, TRT_ERR_SEAL, d);
    }
  }
  logf_be(be, "trt: SEALED (W^X: code pages now RX, no RWX ever)");

  // PE32 has no .pdata unwind tables: 32-bit SEH is the runtime FS:0
  // chain, and register_unwind (RtlAddFunctionTable) has no 32-bit
  // meaning. The Phase 2 emulator owns fault mapping. Seal is complete.
  if (is_pe32) {
    logf_be(be, "trt: PE32: no .pdata unwind (SEH is the FS:0 chain; Phase 2 owns it)");
    return TRT_OK;
  }

  // ---- unwind registration: the OS unwinder cannot see VirtualAlloc'd code
  // until its .pdata is registered. Without this, the FIRST guest C++
  // exception kills the whole process. This is boot-critical, not optional.
  const trt_data_directory* ex =
      trt_pe_directories(&nt->optional_header, magic) + TRT_PE_DIR_EXCEPTION;
  if (ex->virtual_address && ex->size && be->register_unwind) {
    const u32 count = ex->size / 12;  // RUNTIME_FUNCTION = 12 bytes (3 x u32)
    const int rc = be->register_unwind(img->base, img->base + ex->virtual_address,
                                       count);
    char line[128];
    snprintf(line, sizeof(line),
             "trt: unwind .pdata registered funcs=%u rc=%d (RtlAddFunctionTable)",
             count, rc);
    logf_be(be, line);
  } else if (ex->virtual_address && ex->size) {
    logf_be(be, "trt: NOTE .pdata present but backend cannot register unwind "
               "(exceptions in guest code will be fatal)");
  }
  return TRT_OK;
}

extern "C" void trt_unmap(const trt_backend* be, trt_image* img) {
  if (!be || !img) return;
  if (img->base) {
    if (img->is_pe32) pe32_guest_release_by_host(img->base);
    be->release(img->base, (size_t)img->size_of_image);
    trt_mem_budget_discharge_commit(img->size_of_image);
    trt_mem_budget_discharge_va(img->size_of_image);
  }
  free(img);
}

// ---------------------------------------------------------------------------
// Boot audit: parse import + delay-import tables IN FILE SPACE and push
// every module!symbol through the REAL resolver. No mapping, no execution,
// the file bytes are never modified. This is the offline gate for real
// games: it converts "should boot" into a measured, named list.
// ---------------------------------------------------------------------------
namespace {
// file-space RVA -> file offset (headers only; no allocation)
struct file_view {
  const u8* file;
  size_t size;
  u64 sec_off;   // file offset of section headers
  u32 nsec;
};

bool fv_rva2off(const file_view& fv, u64 rva, u64* out_off) {
  for (u32 s = 0; s < fv.nsec; ++s) {
    const u8* sh = fv.file + fv.sec_off + (size_t)s * 40;
    const u32 vsize = rd32(sh + 8);
    const u32 vaddr = rd32(sh + 12);
    const u32 rawsz = rd32(sh + 16);
    const u32 raw = rd32(sh + 20);
    if (vaddr <= rva && rva < vaddr + (vsize ? vsize : rawsz)) {
      if (rva - vaddr >= rawsz && rawsz) return false;  // in virtual padding
      *out_off = raw + (rva - vaddr);
      return *out_off < fv.size;
    }
  }
  return false;
}
}  // namespace

using ::trt_resolve_module;

// Width-parameterized audit core: is_pe32 selects 4-byte thunks (ordinal
// flag bit 31) and the PE32 header layout; otherwise 8-byte thunks
// (ordinal flag bit 63) and PE64. The resolver is width-independent:
// bindability of a module!symbol does not depend on the caller's width.
// The audit allocates NO trap addresses (it is side-effect-free file
// inspection; traps are allocated at map time).
static int audit_imports_impl(const u8* file, size_t file_size,
                              std::vector<std::string>* missing, int is_pe32) {
  const trt_dos_header* dos = (const trt_dos_header*)file;
  const u64 nt_off = dos->e_lfanew;
  // Machine/magic were validated by the dispatcher; the optional header
  // starts at nt_off + 24 in both widths. SizeOfImage is at optional + 56
  // in both; ImageBase is at +28 (PE32, u32) vs +24 (PE64, u64).
  const u8* opt = file + nt_off + 24;
  const u16 magic = is_pe32 ? TRT_PE_OPT32_MAGIC : TRT_PE_OPT64_MAGIC;
  const u32 nsec = rd16(file + nt_off + 6);  // number_of_sections
  if (nsec == 0 || nsec > 96) return TRT_ERR_NO_SECTIONS;
  const u64 size_of_image = rd32(opt + 56);
  const u64 preferred = is_pe32 ? rd32(opt + 28) : rd64(opt + 24);
  const trt_data_directory* dirs = trt_pe_directories(opt, magic);
  // Managed (.NET) assemblies never reach import bind: the CLR header is
  // the verdict.
  const u32 num_dirs = is_pe32 ? rd32(opt + 92) : rd32(opt + 108);
  if (num_dirs > TRT_PE_DIR_COM_DESCRIPTOR &&
      dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0)
    return TRT_ERR_MANAGED;
  const u64 sec_off =
      nt_off + 24 + rd16(file + nt_off + 20);  // size_of_optional_header
  file_view fv{file, file_size, sec_off, nsec};

  // the real resolver — identical to trt_map_pe's import bind
  trt_resolver rs;
  trt_resolver_default(&rs);

  u64 total = 0, resolved = 0;
  char line[256];

  // ---- regular imports (these are hard: the loader fails the map) ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  if (imp_va && imp_sz) {
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      u64 doff;
      if (!fv_rva2off(fv, imp_va + d, &doff)) break;
      const trt_import_descriptor desc = [&] {
        trt_import_descriptor v;
        memcpy(&v, file + doff, sizeof(v));
        return v;
      }();
      if (!desc.first_thunk && !desc.name) break;
      u64 name_off;
      if (!fv_rva2off(fv, desc.name, &name_off)) {
        missing->push_back("<bad>DLL-name-unreadable");
        continue;
      }
      if (name_off >= file_size) break;
      const char* mod_name = (const char*)(file + name_off);
      if (strnlen(mod_name, 256) == 256) break;
      const u32 ilt_rva = desc.original_first_thunk ? desc.original_first_thunk
                                                    : desc.first_thunk;
      const u64 stride = is_pe32 ? 4 : 8;
      for (u64 t = 0;; t += stride) {
        u64 toff;
        if (!fv_rva2off(fv, ilt_rva + t, &toff)) break;
        if (toff + stride > file_size) break;
        const u64 thunk = is_pe32 ? rd32(file + toff) : rd64(file + toff);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (is_pe32 ? (thunk & 0x80000000u) : (thunk >> 63)) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          u64 hoff;
          if (!fv_rva2off(fv, thunk & 0x7fffffffULL, &hoff) ||
              hoff + 2 >= file_size)
            break;
          sym_name = (const char*)(file + hoff + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        ++total;
        // resolve WITHOUT a system loader (the console g_system_load is
        // set at runtime; offline we audit bridges + module defaults)
        void* fn = trt_resolve_module(nullptr, mod_name, sym_name, ordinal);
        if (fn) {
          ++resolved;
        } else {
          snprintf(line, sizeof(line), "%s!%s", mod_name,
                   sym_name ? sym_name : "#ordinal");
          missing->push_back(line);
        }
      }
    }
  }

  // ---- delay imports (soft: the loader stubs these fail-visibly) ----
  const u64 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  const u64 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
  if (dd_va && dd_sz) {
    u64 delay_missing = 0;
    for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= (dd_sz ? dd_sz : 4096);
         d += sizeof(trt_delayload_descriptor)) {
      u64 doff;
      if (!fv_rva2off(fv, dd_va + d, &doff)) break;
      if (doff + sizeof(trt_delayload_descriptor) > file_size) break;
      trt_delayload_descriptor dd;
      memcpy(&dd, file + doff, sizeof(dd));
      if (!dd.name_rva && !dd.iat_rva) break;
      const int rva_based = (dd.attributes & 1u) != 0;
      const u64 name_rva = rva_based ? dd.name_rva
                                     : (u64)dd.name_rva - preferred;
      const u64 int_rva = rva_based ? dd.int_rva
                                    : (u64)dd.int_rva - preferred;
      u64 name_off;
      if (name_rva >= size_of_image ||
          !fv_rva2off(fv, name_rva, &name_off))
        break;
      const char* mod_name = (const char*)(file + name_off);
      if (strnlen(mod_name, 256) == 256) break;
      const u64 stride = is_pe32 ? 4 : 8;
      for (u64 t = 0;; t += stride) {
        u64 toff;
        if (!fv_rva2off(fv, int_rva + t, &toff)) break;
        if (toff + stride > file_size) break;
        const u64 thunk = is_pe32 ? rd32(file + toff) : rd64(file + toff);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (is_pe32 ? (thunk & 0x80000000u) : (thunk >> 63)) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          u64 hoff;
          if (!fv_rva2off(fv, thunk & 0x7fffffffULL, &hoff) ||
              hoff + 2 >= file_size)
            break;
          sym_name = (const char*)(file + hoff + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        ++total;
        // delay imports get fail-visible stubs at load: never a boot blocker
        if (trt_resolve_module(nullptr, mod_name, sym_name, ordinal))
          ++resolved;
        else
          ++delay_missing;
      }
    }
    if (delay_missing) {
      snprintf(line, sizeof(line),
               "(delay imports unresolved: %llu — stubbed fail-visible at "
               "load, not boot-blocking)",
               (unsigned long long)delay_missing);
      missing->push_back(line);
    }
  }

  snprintf(line, sizeof(line), "audit%s: %llu/%llu imports resolve",
           is_pe32 ? " (PE32)" : "",
           (unsigned long long)resolved, (unsigned long long)total);
  trt_core_log(line);
  return TRT_OK;
}

extern "C" int trt_audit_imports(const u8* file, size_t file_size,
                                  void* missing_out) {
  if (!file || file_size < sizeof(trt_dos_header) || !missing_out)
    return TRT_ERR_BAD_ARGS;
  std::vector<std::string>* missing = (std::vector<std::string>*)missing_out;
  missing->clear();

  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return TRT_ERR_BAD_DOS;
  const u64 nt_off = dos->e_lfanew;
  if (nt_off + sizeof(trt_nt_headers64) > file_size) return TRT_ERR_BAD_DOS;
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550) return TRT_ERR_BAD_PE;
  const u16 machine = rd16((const u8*)&nt->file_header.machine);
  const u16 magic = rd16((const u8*)&nt->optional_header.magic);
  if (machine == TRT_PE_MACHINE_X64 && magic == TRT_PE_OPT64_MAGIC)
    return audit_imports_impl(file, file_size, missing, 0);
  if (machine == TRT_PE_MACHINE_I386 && magic == TRT_PE_OPT32_MAGIC)
    return audit_imports_impl(file, file_size, missing, 1);
  return TRT_ERR_NOT_PE64;
}

// ---------------------------------------------------------------------------
// Universal engine: import-surface enumeration (Phase 2)
// ---------------------------------------------------------------------------
// Reads a PE file from disk and fills a bounded trt_universal_profile with
// one entry per importable symbol (direct + delay-load). Module/symbol names
// are lower-cased; entries are sorted by module, symbol, ordinal. Read-only:
// no mapping, no execution, the file bytes are never modified. This is the
// "pull APIs" feed for the universal engine / PC-to-Xbox translator.
// ---------------------------------------------------------------------------
extern "C" int trt_pe_enumerate_imports(const char* path,
                                        trt_universal_profile* up,
                                        uint32_t up_cap) {
  if (!path || !path[0] || !up || up_cap == 0) return -1;
  memset(up, 0, sizeof(*up));

  std::ifstream f(path, std::ios::binary);
  if (!f) return -1;
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  if (buf.size() < sizeof(trt_dos_header)) return -1;
  const u8* file = buf.data();
  const size_t file_size = buf.size();

  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return -1;
  const size_t nt = (size_t)dos->e_lfanew;
  if (nt + 4 + sizeof(trt_file_header) > file_size) return -1;
  const trt_nt_headers64* nt64 = (const trt_nt_headers64*)(file + nt);
  if (rd32((const u8*)&nt64->signature) != 0x00004550) return -1;
  const u16 machine = rd16((const u8*)&nt64->file_header.machine);
  const u16 magic = rd16(file + nt + 24);
  if (machine != TRT_PE_MACHINE_X64 || magic != TRT_PE_OPT64_MAGIC)
    return -1;  // PE32 support joins in Phase 3
  const u32 nsec = rd16(file + nt + 6);
  if (nsec == 0 || nsec > 96) return -1;
  const u8* opt = file + nt + 24;
  const u64 preferred = rd64(opt + 24);
  const u32 size_of_image = rd32(opt + 56);
  const trt_data_directory* dirs = trt_pe_directories(opt, magic);
  const u32 num_dirs = rd32(opt + 108);

  const u64 sec_off = nt + 24 + rd16(file + nt + 20);
  file_view fv{file, file_size, sec_off, nsec};

  up->machine = machine;
  up->is_managed =
      (num_dirs > TRT_PE_DIR_COM_DESCRIPTOR &&
       dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0);

  auto ub_lower = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  };
  auto ub_copy = [&](char* dst, uint32_t cap, const char* src) {
    const size_t n = std::min((size_t)(cap - 1), strnlen(src, 255));
    for (size_t i = 0; i < n; ++i) dst[i] = ub_lower(src[i]);
    dst[n] = '\0';
    return n;
  };

  std::string cur_mod;
  auto add_imp = [&](const char* sym, uint16_t ord, size_t sym_off) {
    if (up->import_count >= up_cap) return;
    trt_universal_import* imp = &up->imports[up->import_count];
    memset(imp, 0, sizeof(*imp));
    if (sym) {
      const size_t sl = ub_copy(imp->symbol, sizeof(imp->symbol), sym);
      ub_copy(imp->module, sizeof(imp->module), cur_mod.c_str());
      imp->symbol_offset = (sym_off >= file_size) ? 0 : (uint32_t)sym_off;
      imp->name_len = (uint32_t)sl;
    } else {
      ub_copy(imp->module, sizeof(imp->module), cur_mod.c_str());
      imp->name_len = (uint32_t)strnlen(imp->module, sizeof(imp->module));
      imp->ordinal = ord;
    }
    up->import_count++;
  };

  // Walk an INT thunk stream at RVA `tbase`; one entry per symbol; stops at
  // the terminator (0) or on any read failure.
  auto walk_thunks = [&](u64 tbase) {
    for (u64 t = 0;; t += 8) {
      u64 toff;
      if (!fv_rva2off(fv, tbase + t, &toff)) break;
      if (toff + 8 > file_size) break;
      const u64 thunk = rd64(file + toff);
      if (thunk == 0) break;
      if (thunk & 0x8000000000000000ULL) {
        add_imp(nullptr, (uint16_t)(thunk & 0xFFFFULL), 0);
      } else {
        u64 hoff;
        if (!fv_rva2off(fv, thunk & 0x7fffffffffffULL, &hoff) ||
            hoff + 2 >= file_size)
          break;
        add_imp((const char*)(file + hoff + 2), 0, hoff + 2);
      }
    }
  };

  // ---- direct imports (IMAGE_DIRECTORY_ENTRY_IMPORT = 1) ----
  const u32 imp_rva = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
       d += sizeof(trt_import_descriptor)) {
    u64 doff;
    if (!fv_rva2off(fv, imp_rva + d, &doff)) break;
    const trt_import_descriptor* desc =
        (const trt_import_descriptor*)(file + doff);
    if (!desc->name && !desc->original_first_thunk && !desc->first_thunk) break;
    u64 name_off;
    if (!fv_rva2off(fv, desc->name, &name_off) || name_off >= file_size)
      continue;
    char mod[256] = {0};
    const size_t cap = std::min((size_t)255, file_size - name_off);
    size_t n = 0;
    while (n < cap && file[name_off + n] != '\0') {
      mod[n] = (char)file[name_off + n];
      ++n;
    }
    mod[n] = '\0';
    if (!mod[0]) continue;
    cur_mod = mod;
    const u32 ilt_rva =
        desc->original_first_thunk ? desc->original_first_thunk
                                   : desc->first_thunk;
    if (ilt_rva) walk_thunks(ilt_rva);
  }

  // ---- delay imports (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT = 13) ----
  const u32 dl_rva = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  const u64 dl_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
  for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= dl_sz;
       d += sizeof(trt_delayload_descriptor)) {
    u64 doff;
    if (!fv_rva2off(fv, dl_rva + d, &doff)) break;
    const trt_delayload_descriptor* dd =
        (const trt_delayload_descriptor*)(file + doff);
    if (!dd->name_rva && !dd->iat_rva && !dd->int_rva) break;
    if (!(dd->attributes & 1u) || !dd->name_rva) continue;  // RVA-based only
    const bool rva_based = (dd->attributes & 1u) != 0;
    const u64 name_rva = rva_based ? dd->name_rva : dd->name_rva - preferred;
    if (name_rva >= size_of_image) break;
    u64 name_off;
    if (!fv_rva2off(fv, name_rva, &name_off) || name_off >= file_size) break;
    char mod[256] = {0};
    const size_t cap = std::min((size_t)255, file_size - name_off);
    size_t n = 0;
    while (n < cap && file[name_off + n] != '\0') {
      mod[n] = (char)file[name_off + n];
      ++n;
    }
    mod[n] = '\0';
    if (!mod[0]) continue;
    cur_mod = mod;
    const u64 int_rva = rva_based ? dd->int_rva : dd->int_rva - preferred;
    if (int_rva) walk_thunks(int_rva);
  }

  std::sort(up->imports, up->imports + up->import_count,
            [](const trt_universal_import& a, const trt_universal_import& b) {
              const int c = strcmp(a.module, b.module);
              if (c) return c < 0;
              const int cs = strcmp(a.symbol, b.symbol);
              if (cs) return cs < 0;
              return a.ordinal < b.ordinal;
            });
  return (int)up->import_count;
}
