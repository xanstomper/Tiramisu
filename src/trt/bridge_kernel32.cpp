// tiramisu/src/trt/bridge_kernel32.cpp
// Builtin kernel32 — the subset a game boots on, implemented on top of the
// backend (memory) and CRT. Wine semantics, Tiramisu implementation.
// v0 scope: heap/memory/sync-primitive/file-io/clock/error/TLS basics.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(_WIN32)
#  include <intrin.h>
#else
#  include <pthread.h>
#endif
#include "iso.h"
#include <algorithm>
#include "trt.h"
#include "pe.h"
#include "thunk.h"
#include "images.h"
#include "apiset.h"
#include "pe_exports.h"
#include "bridge_d3d.h"
#include "bridge_d3d12.h"
#include "bridge_reg.h"
#include "bridge_mem.h"
#include "bridge_thread.h"
#include "bridge_sys.h"
// Guest object width (24-byte PE32 vs 40-byte Win64 CRITICAL_SECTION); defined
// in bridge_thread.cpp.
extern "C" int trt_cs_size_for_guest(void);
#include "bridge_winmm.h"
#include "bridge_d3d9.h"


#include "bridge_dsound.h"
#include "bridge_ddraw.h"
#include "bridge_d3dx9.h"
#include "bridge_d3d8.h"

// Unwinder readability probe (page_readable): process_vm_readv EFAULT
// mapped-check (see comment at the probe for why not write()/dev/null).
// Linux-only syscalls: guarded so the UWP host build uses VirtualQuery.
#ifdef __linux__
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#include "bridge_openal.h"
#include "bridge_ole.h"
#include "bridge_shell.h"
#include "bridge_shlwapi.h"
#include "bridge_actctx.h"
#include "bridge_version.h"
#include "bridge_mmedia.h"
#include "bridge_d3dx.h"
#include "bridge_dinput.h"
#include "bridge_audio.h"
#include "bridge_steam.h"
#include "bridge_comctl.h"
#include "bridge_gdi.h"
#include "bridge_gdiplus.h"
#include "bridge_fmod.h"
#include "bridge_wwise.h"
#include "bridge_bink.h"
#include "bridge_criware.h"
#include "bridge_miles.h"
#include "bridge_wininet.h"
#include "bridge_psapi.h"
#include "bridge_iphlpapi.h"
#include "bridge_imm32.h"
#include "bridge_crypt32.h"
#include "bridge_dbghelp.h"
#include "bridge_ntdll.h"
#include "bridge_console_pack.h"
#include "bridge_sys_extras.h"
#include "bridge_vulkan.h"
#include "bridge_mf.h"
#include "bridge_gameinput.h"
#include "bridge_wldap32.h"
#include "bridge_ktmw32.h"

// ---- 5 GB memory-budget governor (bridge_membudget.cpp): every host-side
// byte that stands in for guest commit charges one ledger. Over-budget
// commits refuse with ERROR_COMMITMENT_LIMIT so the guest's own low-memory
// paths run instead of a platform kill. ----
extern "C" int trt_mem_budget_charge_commit(u64 bytes);
extern "C" void trt_mem_budget_discharge_commit(u64 bytes);
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);
extern "C" void trt_mem_budget_report(char* line, unsigned cap);
extern "C" void trt_sync_parked_log(void);
extern "C" int trt_sync_parked_count(void);

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#  include <pthread.h>
#  include <signal.h>
#  include <ucontext.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/mman.h>
#endif
#  include <time.h>
#  include <sys/stat.h>
#  include <errno.h>

#include "bridge_heap.h"

// Host-settings hook (weak: absent when trt runs without the host layer —
// offline tests). File-scope near the top: clang rejects attributed
// block-scope extern declarations.
extern "C" bool trt_settings_write_watch() __attribute__((weak));

namespace {

// ---- FLS (fiber-local storage; uncapped metadata + per-thread values) ----
constexpr size_t k_max_fls_slots = 128;
struct fls_meta { void* cb; bool used; };
std::mutex g_fls_mu;
fls_meta g_fls_meta[k_max_fls_slots]{};
std::atomic<u32> g_fls_epoch{1};
thread_local void* t_fls_slots[k_max_fls_slots]{};
thread_local u32 t_fls_epoch = 0;

size_t wcslen16(const u16* s) { size_t n = 0; while (s && s[n]) ++n; return n; }

// ---- synthetic handle space (uncapped, thread-safe) ----
struct trt_handle_table {
  void* ptr;
  u32   tag;
};
std::mutex g_handles_mu;
std::deque<trt_handle_table> g_handles;

std::atomic<u32> g_err_epoch{1};
std::atomic<u32> g_next_guest_tid{1};
thread_local u32 t_last_error = 0;
thread_local u32 t_last_error_epoch = 0;
thread_local u32 t_guest_tid = 0;

inline u32& err_ref_for_current_thread() {
  const u32 ep = g_err_epoch.load(std::memory_order_acquire);
  if (t_last_error_epoch != ep) {
    t_last_error = 0;
    t_last_error_epoch = ep;
  }
  return t_last_error;
}

inline u32 err_for_current_thread() {
  return err_ref_for_current_thread();
}

inline void set_err_for_current_thread(u32 err) {
  err_ref_for_current_thread() = err;
}

void* handle_alloc(u32 tag, void* ptr) {
  std::lock_guard<std::mutex> lk(g_handles_mu);
  const size_t idx = g_handles.size();
  g_handles.push_back({ptr, tag});
  return (void*)(uintptr_t)(((u64)tag << 32) | (u64)(idx + 1));
}
// ---- backend singletons (set at boot by host/test) ----
trt_backend g_be;
bool g_be_ready = false;

// ---- QPC ----
i64 now_qpc_ticks() {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  timespec_get(&ts, TIME_UTC);
#endif
  return (i64)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

// A continuation can repair memory without changing RIP. Permit retries while
// bounding a stuck handler that resumes the identical fault in the same frame.
struct fault_retry {
  u64 pc, stack, address;
  u32 code, access;
  i64 last_tick;
  unsigned count;
};
thread_local fault_retry t_fault_retry{};

bool guest_fault_retry_exhausted(u64 pc, u64 stack, u64 address, u32 code, u32 access) {
  constexpr i64 retry_interval_ns = 1000000000;
  constexpr unsigned max_same_fault_retries = 8;
  const i64 tick = now_qpc_ticks();
  auto& prior = t_fault_retry;
  if (prior.pc != pc || prior.stack != stack || prior.address != address ||
      prior.code != code || prior.access != access || tick < prior.last_tick ||
      tick - prior.last_tick > retry_interval_ns) {
    prior = {pc, stack, address, code, access, tick, 1};
    return false;
  }
  prior.last_tick = tick;
  return ++prior.count > max_same_fault_retries;
}

} // namespace

extern "C" {

// Defined below; the registration tables are earlier in this unit.
void TRT_STDCALL trt_GetCurrentThreadStackLimits(u64* low, u64* high);

// ---- guest-visible kernel32 surface (v0) ----

void* TRT_STDCALL trt_GetProcessHeap(void) { return (void*)(uintptr_t)0x1; }

void* TRT_STDCALL trt_HeapCreate(u32 flags, u64 init, u64 max) {
  (void)flags; (void)init; (void)max;
  return handle_alloc(0xCAFE, nullptr);  // heap handle: a record, not memory
}

void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes) {
  (void)heap;
  // backend alloc = RW memory; flags HEAP_ZERO_MEMORY = 0x8
  const size_t n = (size_t)(bytes ? bytes : 1);
  void* p = nullptr;
  if (n <= TRT_HEAP_POOL_MAX) {
    // small block: carve from a slab and record it (the slab reservation is
    // already charged; carving charges nothing). The pool owns its mutex.
    p = trt_heap_pool_alloc(n);
  } else {
    // large block: bypass the pool — one backend alloc, charged exactly
    // (commit + VA), as before
    if (trt_mem_budget_charge_va((u64)n) != 0) return nullptr;  // 8
    if (trt_mem_budget_charge_commit((u64)n) != 0) {
      trt_mem_budget_discharge_va((u64)n);
      return nullptr;  // 1455
    }
    p = g_be.alloc(n, 0);
    if (!p) {
      trt_mem_budget_discharge_commit((u64)n);
      trt_mem_budget_discharge_va((u64)n);
      return nullptr;
    }
    trt_heap_rec_add(p, n, 1);
  }
  if (!p) return nullptr;  // slab charge refused: 1455/8 already set
  if (flags & 0x8) memset(p, 0, n);
  return p;
}

int TRT_STDCALL trt_HeapSetInformation(void* heap, u32 info_class,
                                       const void* info, u64 info_size) {
  // Guest heaps use backend allocations. Never forward their synthetic
  // handles to the OS heap manager. In particular, the static MSVC CRT
  // requests low fragmentation (class 0) on its private heap at startup.
  (void)heap;
  (void)info_class;
  (void)info;
  (void)info_size;
  set_err_for_current_thread(50);  // ERROR_NOT_SUPPORTED
  return 0;
}

int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  if (!p) return 1;
  size_t n = 0;
  int direct = 0;
  const int r = trt_heap_release(p, &n, &direct);
  if (r < 0) {
    // foreign pointer HeapAlloc never recorded: the backend no-ops unknown
    // addresses, exactly as the pre-split code relied on
    g_be.release(p, 0);  // backend tracks size internally (mmap/UWP)
    return 1;
  }
  if (r == 0) return 0;  // pool corruption: logged, block deliberately leaked
  if (direct) {
    // a pool block rode on an already-charged slab; only a direct block
    // moved the ledger
    trt_mem_budget_discharge_commit((u64)n);
    trt_mem_budget_discharge_va((u64)n);
  }
  return 1;
}

// ---- VirtualAlloc / VirtualFree / VirtualQuery extent tracker (uncapped) ----
enum class va_backing : u8 { eager, deferred, materialized };
struct va_extent {
  uintptr_t start;
  size_t size;
  u32 state;  // 0x1000 MEM_COMMIT, 0x2000 MEM_RESERVE
  u32 prot;   // Win32 PAGE_*
  bool pending_rx;
  va_backing backing = va_backing::eager;
};
struct va_region {
  uintptr_t base;
  size_t size;
  u32 alloc_prot;
  std::vector<va_extent> extents;
};
std::mutex g_va_mu;
std::vector<va_region> g_va;
std::vector<va_extent> g_extra_pending_rx;
// Scratch for va_split_and_set_locked: reusing one vector avoids a heap
// allocation per VA call (every VirtualAlloc/Free/Query ran one). Every
// caller holds g_va_mu, so a single shared scratch is safe.
std::vector<va_extent> g_va_split_scratch;
trt_vmem_reserve_fn g_vmem_reserve = nullptr;
trt_vmem_commit_fn g_vmem_commit = nullptr;
trt_vmem_decommit_fn g_vmem_decommit = nullptr;
std::atomic<bool> g_va_defer_data{false};
std::atomic<bool> g_va_has_deferred_data{false};

unsigned va_prot_bits(u32 wp) {
  switch (wp & 0xFFu) {
    case 0x01: return 0;                                         // NOACCESS
    case 0x02: return TRT_PROT_R;                                // READONLY
    case 0x04: return TRT_PROT_R | TRT_PROT_W;                   // READWRITE
    case 0x08: return TRT_PROT_R | TRT_PROT_W;                   // WRITECOPY
    case 0x10: return TRT_PROT_X;                                // EXECUTE
    case 0x20: return TRT_PROT_R | TRT_PROT_X;                   // EXECUTE_READ
    case 0x40: return TRT_PROT_R | TRT_PROT_W | TRT_PROT_X;      // EXECUTE_READWRITE
    case 0x80: return TRT_PROT_R | TRT_PROT_W | TRT_PROT_X;      // EXECUTE_WRITECOPY
    default:   return TRT_PROT_R | TRT_PROT_W;
  }
}

// .252 FIX: forget the VA-extent registry on budget reset
// (mirror of trt_crt_forget_all, .251). After a guest
// force-kill, trt_mem_budget_reset() zeroes the ledgers, but
// the dead guest's VA regions stay in g_va; when teardown
// frees them, VirtualFree finds the extent and discharges
// against the zeroed ledger, tripping the over-discharge
// tripwire (the .251 1 MB VA discharge). Clearing the
// registry makes those frees land as foreign (no discharge,
// memory still released), matching the reset's fresh-start
// fiction. Called only from trt_mem_budget_reset(), after
// the guest thread is joined.
extern "C" void trt_va_forget_all(void) {
  std::lock_guard<std::mutex> lk(g_va_mu);
  g_va.clear();
  g_extra_pending_rx.clear();
  g_va_has_deferred_data.store(false);
}

void va_split_and_set_locked(va_region& r, uintptr_t s, uintptr_t e,
                             int new_state, u32 new_prot, int pending_rx,
                             int backing = -1) {
  const uintptr_t rend = r.base + r.size;
  if (s < r.base) s = r.base;
  if (e > rend) e = rend;
  if (s >= e) return;
  // reused scratch (guarded by g_va_mu, like every caller): no heap alloc
  // per split
  std::vector<va_extent>& next = g_va_split_scratch;
  next.clear();
  next.reserve(r.extents.size() + 2);
  for (size_t i = 0; i < r.extents.size(); ++i) {
    const va_extent& ex = r.extents[i];
    const uintptr_t ex_end = ex.start + ex.size;
    if (ex_end <= s || ex.start >= e) {
      next.push_back(ex);
      continue;
    }
    if (ex.start < s) {
      next.push_back({ex.start, s - ex.start, ex.state, ex.prot, ex.pending_rx, ex.backing});
    }
    const uintptr_t ms = (ex.start > s) ? ex.start : s;
    const uintptr_t me = (ex_end < e) ? ex_end : e;
    if (ms < me) {
      next.push_back({
          ms,
          me - ms,
          (new_state >= 0) ? (u32)new_state : ex.state,
          (new_prot != 0xFFFFFFFFu) ? (new_prot & 0xFFu) : ex.prot,
          (pending_rx >= 0) ? (pending_rx != 0) : ex.pending_rx,
          backing >= 0 ? static_cast<va_backing>(backing) : ex.backing,
      });
    }
    if (ex_end > e) {
      next.push_back({e, ex_end - e, ex.state, ex.prot, ex.pending_rx, ex.backing});
    }
  }
  r.extents.clear();
  for (size_t i = 0; i < next.size(); ++i) {
    const va_extent& x = next[i];
    if (!r.extents.empty()) {
      va_extent& back = r.extents.back();
      if (back.start + back.size == x.start && back.state == x.state &&
          back.prot == x.prot && back.pending_rx == x.pending_rx && back.backing == x.backing) {
        back.size += x.size;
        continue;
      }
    }
    r.extents.push_back(x);
  }
}

void va_record_add(uintptr_t base, size_t size, u32 alloc_prot, u32 cur_prot,
                   u32 state, bool pending_rx, va_backing backing = va_backing::eager) {
  std::lock_guard<std::mutex> lk(g_va_mu);
  va_extent ex{base, size, state, cur_prot & 0xFFu, pending_rx, backing};
  size_t free_idx = (size_t)-1;
  for (size_t i = 0; i < g_va.size(); ++i) {
    if (g_va[i].base == base) {
      g_va[i].size = size;
      g_va[i].alloc_prot = alloc_prot & 0xFFu;
      g_va[i].extents.assign(1, ex);
      return;
    }
    if (g_va[i].base == 0 && free_idx == (size_t)-1) free_idx = i;
  }
  va_region rec;
  rec.base = base;
  rec.size = size;
  rec.alloc_prot = alloc_prot & 0xFFu;
  rec.extents.push_back(ex);
  if (free_idx != (size_t)-1) g_va[free_idx] = std::move(rec);
  else g_va.push_back(std::move(rec));
}

int va_find_idx_locked(uintptr_t addr) {
  for (size_t i = 0; i < g_va.size(); ++i) {
    if (g_va[i].base && addr >= g_va[i].base &&
        addr < g_va[i].base + g_va[i].size)
      return (int)i;
  }
  return -1;
}

const va_extent* va_find_extent_locked(const va_region& r, uintptr_t addr) {
  for (size_t i = 0; i < r.extents.size(); ++i) {
    if (addr >= r.extents[i].start &&
        addr < r.extents[i].start + r.extents[i].size)
      return &r.extents[i];
  }
  return nullptr;
}

// Bytes in [s, e) currently in MEM_COMMIT state, from the region's
// extents. Caller must hold g_va_mu. This is what makes commit charging
// exact: a re-commit charges only the not-yet-committed bytes, and a
// decommit discharges only the actually-committed bytes (the old code
// charged/discharged the whole span either way, drifting the ledger in
// both directions).
size_t va_committed_in_locked(const va_region& r, uintptr_t s, uintptr_t e) {
  size_t n = 0;
  for (size_t i = 0; i < r.extents.size(); ++i) {
    const va_extent& ex = r.extents[i];
    if (ex.state != 0x1000u || ex.backing == va_backing::deferred) continue;
    const uintptr_t os = ex.start > s ? ex.start : s;
    const uintptr_t oe =
        ex.start + ex.size < e ? ex.start + ex.size : e;
    if (os < oe) n += (size_t)(oe - os);
  }
  return n;
}

// Caller holds g_va_mu. The entire span must already be logically committed.
bool va_has_deferred_readwrite_locked(const va_region& region, uintptr_t start,
                                       uintptr_t end) {
  if (start >= end || end > region.base + region.size) return false;
  bool deferred = false;
  for (const auto& extent : region.extents) {
    const uintptr_t extent_end = extent.start + extent.size;
    if (extent_end <= start) continue;
    if (extent.start > start || extent.state != 0x1000u ||
        extent.prot != 0x04u || extent.pending_rx) return false;
    deferred |= extent.backing == va_backing::deferred;
    if (extent_end >= end) return deferred;
    start = extent_end;
  }
  return false;
}

extern "C" int trt_va_is_deferred_readwrite(const void* buffer, size_t size) {
  if (!g_va_has_deferred_data.load() || !buffer || !size) return 0;
  const uintptr_t raw = reinterpret_cast<uintptr_t>(buffer);
  if (size > UINTPTR_MAX - raw || raw + size > UINTPTR_MAX - 4095) return 0;
  const uintptr_t start = raw & ~uintptr_t(4095);
  const uintptr_t end = (raw + size + 4095) & ~uintptr_t(4095);
  std::lock_guard<std::mutex> lock(g_va_mu);
  const int index = va_find_idx_locked(start);
  return index >= 0 &&
         va_has_deferred_readwrite_locked(g_va[static_cast<size_t>(index)], start, end);
}

extern "C" void trt_set_vmem_hooks(trt_vmem_reserve_fn reserve_fn,
                                   trt_vmem_commit_fn commit_fn,
                                   trt_vmem_decommit_fn decommit_fn) {
  std::lock_guard<std::mutex> lk(g_va_mu);
  g_vmem_reserve = reserve_fn;
  g_vmem_commit = commit_fn;
  g_vmem_decommit = decommit_fn;
}

extern "C" void trt_va_enable_deferred_data(int enabled) {
  g_va_defer_data.store(enabled != 0);
}

// The logical MEM_COMMIT state is already visible through VirtualQuery.
// Charge RAM at the moment a physical data range is committed.
bool va_materialize_locked(va_region& region, uintptr_t start, uintptr_t end) {
  if (!g_vmem_commit || start >= end) return false;
  try {
    // Leave no allocation or reallocation after a successful host commit.
    g_va_split_scratch.reserve(region.extents.size() + 2);
    region.extents.reserve(region.extents.size() + 2);
  } catch (...) {
    set_err_for_current_thread(8);
    return false;
  }
  const size_t size = end - start;
  if (trt_mem_budget_charge_commit(size) != 0) return false;
  if (g_vmem_commit(reinterpret_cast<void*>(start), size, TRT_PROT_R | TRT_PROT_W) != 0) {
    trt_mem_budget_discharge_commit(size);
    set_err_for_current_thread(8);
    return false;
  }
  va_split_and_set_locked(region, start, end, -1, 0xFFFFFFFFu, -1,
                          static_cast<int>(va_backing::materialized));
  return true;
}

extern "C" int trt_va_handle_deferred_data_fault(u64 address, u32 access) {
  if (!g_va_defer_data.load() || !g_va_has_deferred_data.load() || access > 1)
    return 0;  // Never enable execute.
  thread_local uintptr_t last_racing_page = 0;
  const uintptr_t page = static_cast<uintptr_t>(address) & ~uintptr_t(4095);
  std::lock_guard<std::mutex> lock(g_va_mu);
  const int index = va_find_idx_locked(page);
  if (index < 0) return 0;
  auto& region = g_va[static_cast<size_t>(index)];
  const auto* extent = va_find_extent_locked(region, page);
  if (!extent || extent->state != 0x1000u || extent->prot != 0x04u) return 0;
  if (extent->backing == va_backing::materialized) {
    // A second thread can fault before the first thread finishes committing.
    // Retry that page once, then let a real fault reach the guest handler.
    if (last_racing_page == page) return 0;
    last_racing_page = page;
    return 1;
  }
  if (extent->backing != va_backing::deferred) return 0;

  const u32 prior_error = err_for_current_thread();
  constexpr uintptr_t chunk = 65536;
  const uintptr_t chunk_start = page & ~(chunk - 1);
  const uintptr_t start = std::max(chunk_start, extent->start);
  const uintptr_t end = std::min(chunk_start + chunk, extent->start + extent->size);
  if (!va_materialize_locked(region, start, end) &&
      ((start == page && end == page + 4096) ||
       !va_materialize_locked(region, page, page + 4096))) return 0;
  set_err_for_current_thread(prior_error);
  last_racing_page = page;
  return 1;
}

// A host operation may pass the buffer to the kernel, which cannot deliver
// a first-touch exception back to the application. Materialize its span.
extern "C" int trt_va_prepare_data_range(const void* buffer, size_t size) {
  if (!g_va_defer_data.load() || !g_va_has_deferred_data.load() || !buffer || !size)
    return 1;
  const uintptr_t raw = reinterpret_cast<uintptr_t>(buffer);
  if (size > UINTPTR_MAX - raw || raw + size > UINTPTR_MAX - 4095) return 0;
  uintptr_t start = raw & ~uintptr_t(4095);
  const uintptr_t end = (raw + size + 4095) & ~uintptr_t(4095);
  std::lock_guard<std::mutex> lock(g_va_mu);
  const int index = va_find_idx_locked(start);
  if (index < 0) return 1;
  auto& region = g_va[static_cast<size_t>(index)];
  while (start < end && start < region.base + region.size) {
    const auto* extent = va_find_extent_locked(region, start);
    if (!extent) break;
    const uintptr_t next = std::min(end, extent->start + extent->size);
    if (extent->backing == va_backing::deferred &&
        !va_materialize_locked(region, start, next)) return 0;
    start = next;
  }
  return 1;
}

extern "C" void trt_va_notify_protect(void* base, size_t size, u32 win32_prot,
                                      u32* prev_prot_out) {
  const uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095ULL;
  const uintptr_t e = s + (size ? size : 4096ULL);
  std::lock_guard<std::mutex> lk(g_va_mu);
  const int idx = va_find_idx_locked(s);
  if (idx >= 0) {
    va_region& r = g_va[(size_t)idx];
    if (prev_prot_out) {
      if (const va_extent* ex = va_find_extent_locked(r, s))
        *prev_prot_out = ex->prot;
    }
    va_split_and_set_locked(r, s, e, -1, win32_prot & 0xFFu, 0);
  }
}

extern "C" int trt_va_mark_pending_rx(void* base, size_t size,
                                      u32* prev_prot_out) {
  const uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095ULL;
  const uintptr_t e = s + (size ? size : 4096ULL);
  std::lock_guard<std::mutex> lk(g_va_mu);
  const int idx = va_find_idx_locked(s);
  if (idx >= 0) {
    va_region& r = g_va[(size_t)idx];
    if (prev_prot_out) {
      if (const va_extent* ex = va_find_extent_locked(r, s))
        *prev_prot_out = ex->prot;
    }
    va_split_and_set_locked(r, s, e, -1, 0x04u, 1);
    return 1;
  }
  g_extra_pending_rx.push_back({s, e - s, 0x1000u, 0x04u, true});
  return 1;
}

extern "C" int trt_va_try_promote_rx(u64 fault_addr) {
  const uintptr_t a = (uintptr_t)fault_addr;
  uintptr_t prom_s = 0;
  size_t prom_sz = 0;
  {
    std::lock_guard<std::mutex> lk(g_va_mu);
    const int idx = va_find_idx_locked(a);
    if (idx >= 0) {
      if (const va_extent* ex = va_find_extent_locked(g_va[(size_t)idx], a)) {
        if (ex->pending_rx) {
          prom_s = ex->start;
          prom_sz = ex->size;
        }
      }
    } else {
      for (size_t i = 0; i < g_extra_pending_rx.size(); ++i) {
        const va_extent& ex = g_extra_pending_rx[i];
        if (ex.pending_rx && a >= ex.start && a < ex.start + ex.size) {
          prom_s = ex.start;
          prom_sz = ex.size;
          break;
        }
      }
    }
  }
  if (!prom_s || !prom_sz) return 0;
  if (g_be.protect &&
      g_be.protect((void*)prom_s, prom_sz, TRT_PROT_R | TRT_PROT_X) != 0) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_va_mu);
    const int idx = va_find_idx_locked(prom_s);
    if (idx >= 0) {
      va_split_and_set_locked(g_va[(size_t)idx], prom_s, prom_s + prom_sz, -1,
                              0x20u, 0);
    }
    for (size_t i = 0; i < g_extra_pending_rx.size(); ++i) {
      if (g_extra_pending_rx[i].start == prom_s &&
          g_extra_pending_rx[i].size == prom_sz) {
        g_extra_pending_rx[i].pending_rx = false;
        g_extra_pending_rx[i].prot = 0x20u;
      }
    }
  }
#if defined(_WIN32)
  ::FlushInstructionCache(::GetCurrentProcess(), (const void*)prom_s,
                          (SIZE_T)prom_sz);
#elif !defined(_MSC_VER)
  __builtin___clear_cache((char*)prom_s, (char*)prom_s + prom_sz);
#endif
  return 1;
}

// Write-watch (MEM_WRITE_WATCH / GetWriteWatch) — the bdwgc (Unity IL2CPP)
// incremental-GC probe. Both surfaces MUST answer honestly:
//   * host _WIN32 (console): passthrough to the real kernel32 APIs — they
//     exist and work inside a UWP AppContainer; a fake failure would disable
//     GWW_VDB and cost frame hitches for nothing. Prototypes come from the
//     SDK's memoryapi.h (WINAPI_PARTITION_APP) — do NOT redeclare them here
//     (conflicting types = compile error); resolve via GetProcAddress.
//   * Linux gate: /proc/self/pagemap soft-dirty emulation (bit 55; survives
//     the unprivileged PFN masking — the bit is what GWW needs). Reset via
//     /proc/self/clear_refs "4".
// Anything else fails named (never a lying success = GC mis-mark = corruption).
static u32 ww_host_GetWriteWatch(u32 flags, void* addr, u64 bytes, void** addrs,
                                 u64* count, u32* gran) {
#if defined(_WIN32)
  using fn_t = u32(__stdcall*)(u32, void*, u64, void**, u64*, u32*);
  static fn_t fn = (fn_t)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                               "GetWriteWatch");
  if (fn) return fn(flags, addr, bytes, addrs, count, gran);
  return (u32)-1;
#else
  (void)flags; (void)addr; (void)bytes; (void)addrs; (void)count; (void)gran;
  return (u32)-1;  // no host fallback; caller uses the emulator below
#endif
}
static bool eh_trace_enabled();
static bool ww_pagemap_entry_dirty(u64 pagemap_off) {
  // returns 1 dirty, 0 clean, -1 error. Requires PRESENT: this kernel marks
  // fresh never-faulted anonymous pages soft-dirty=1 while absent (measured:
  // entry bit 55 set, bit 63 clear, zero writes) — absent pages cannot have
  // been written, so reporting them would be a lying over-report.
  if (!((pagemap_off >> 63) & 1)) return 0;
  return (int)((pagemap_off >> 55) & 1);
}
// One-time probe: does soft-dirty actually WORK on this kernel? Several
// 6.x kernels strip the bit for unprivileged pagemap readers (all entries
// read 0 even after fresh first-touch writes). Trusting a dead bit would
// answer "0 dirty pages" to a GC = heap corruption. Probe before trust.
static bool ww_softdirty_works() {
  static const bool works = [] {
#if defined(_WIN32)
    return false;  // console uses real GetWriteWatch passthrough
#else
    // NO clear_refs here: "4" clears soft-dirty bits PROCESS-WIDE and would
    // wipe the dirty bits of memory the caller already wrote (measured:
    // first GWW call returned n=0 for pages written before it). A fresh
    // first-touch write sets the bit without touching anything else.
    const size_t sz = 4096;
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    ((volatile u8*)p)[0] = 1;  // first-touch fault should set soft-dirty
    const int pm = open("/proc/self/pagemap", O_RDONLY);
    if (pm < 0) { munmap(p, sz); return false; }
    u64 ent = 0;
    const bool ok =
        pread(pm, &ent, 8, ((uintptr_t)p / 4096) * 8) == 8 &&
        ww_pagemap_entry_dirty(ent) == 1;
    close(pm);
    munmap(p, sz);
    return ok;
#endif
  }();
  return works;
}
u32 TRT_STDCALL trt_GetWriteWatch(u32 flags, void* addr, u64 bytes, void** addrs,
                                  u64* count, u32* gran) {
  if (!addr || !count || !gran || (flags & ~1u)) {
    if (count) *count = 0;
    if (gran) *gran = 4096;
    set_err_for_current_thread(87);
    return (u32)-1;
  }
  if (gran) *gran = 4096;
  // user kill-switch (dashboard setting): force the honest fail so a GC
  // deterministically picks MPROTECT_VDB.
  if (trt_settings_write_watch && !trt_settings_write_watch()) {
    trt_core_log(
        "kernel32: GetWriteWatch -> ERROR_INVALID_PARAMETER (disabled in "
        "settings; GC falls back to MPROTECT_VDB)");
    if (count) *count = 0;
    set_err_for_current_thread(87);
    return (u32)-1;
  }
  // host passthrough first (console truth)
  if (ww_host_GetWriteWatch(flags, addr, bytes, addrs, count, gran) != (u32)-1) {
    return 0;
  }
#if !defined(_WIN32)
  // soft-dirty emulation: /proc/self/pagemap, one u64 per 4K page.
  // ONLY if the kernel actually honors the bit (probed once — several 6.x
  // kernels strip it unprivileged; a dead bit silently answering "clean"
  // would corrupt a trusting GC). Otherwise fail named: bdwgc falls back
  // to MPROTECT_VDB, which needs only VirtualProtect + fault routing.
  if (!ww_softdirty_works()) {
    trt_core_log(
        "kernel32: GetWriteWatch -> ERROR_INVALID_PARAMETER (soft-dirty dead on "
        "this kernel; GC falls back to MPROTECT_VDB)");
    if (count) *count = 0;
    set_err_for_current_thread(87);
    return (u32)-1;
  }
  // soft-dirty emulation: /proc/self/pagemap, one u64 per 4K page
  const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)4095ULL;
  const uintptr_t end =
      ((uintptr_t)addr + bytes + 4095ULL) & ~(uintptr_t)4095ULL;
  const int pm = open("/proc/self/pagemap", O_RDONLY);
  if (pm < 0) {
    trt_core_log(
        "kernel32: GetWriteWatch -> ERROR_INVALID_PARAMETER (pagemap unreadable; "
        "GC falls back to MPROTECT_VDB)");
    if (count) *count = 0;
    set_err_for_current_thread(87);
    return (u32)-1;
  }
  u64 cap = count ? *count : 0;
  u64 n = 0;
  bool overflow = false;
  for (uintptr_t p = start; p < end; p += 4096) {
    u64 ent = 0;
    const off_t off = (off_t)((p / 4096) * sizeof(u64));
    if (pread(pm, &ent, sizeof(ent), off) != sizeof(ent)) continue;
    if (!ww_pagemap_entry_dirty(ent)) continue;
    if (n < cap && addrs) {
      addrs[n] = (void*)p;
    } else if (n >= cap) {
      overflow = true;
    }
    ++n;
  }
  close(pm);
  if (count) *count = n;
  if ((flags & 1u /*WRITE_WATCH_FLAG_RESET*/) && n > 0) {
    FILE* cr = fopen("/proc/self/clear_refs", "w");
    if (cr) {
      fputs("4\n", cr);
      fclose(cr);
    }
  }
  if (overflow) {
    set_err_for_current_thread(1455);  // ERROR_OUT_OF_MEMORY: buffer too small
    return (u32)-1;
  }
  if (eh_trace_enabled()) {
    char t[128];
    snprintf(t, sizeof(t),
             "kernel32: GetWriteWatch -> %llu dirty pages in [0x%llx..0x%llx)",
             (unsigned long long)n, (unsigned long long)start,
             (unsigned long long)end);
    trt_core_log(t);
  }
  return 0;
#else
  if (count) *count = 0;
  set_err_for_current_thread(87);
  return (u32)-1;
#endif
}
u32 TRT_STDCALL trt_ResetWriteWatch(void* addr, u64 bytes) {
#if defined(_WIN32)
  using fn_t = u32(__stdcall*)(void*, u64);
  static fn_t fn = (fn_t)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                               "ResetWriteWatch");
  if (fn) return fn(addr, bytes);
#endif
#if !defined(_WIN32)
  // LIMITATION (named): Linux clear_refs "4" clears soft-dirty PROCESS-WIDE,
  // not just [addr, addr+bytes) — broader than Win32 semantics. Harmless for
  // bdwgc (it resets once per GC cycle after scanning everything) and the
  // console uses the real per-range kernel32 API.
  if (addr && bytes) {
    FILE* cr = fopen("/proc/self/clear_refs", "w");
    if (cr) {
      fputs("4\n", cr);
      fclose(cr);
      return 0;
    }
  }
#endif
  set_err_for_current_thread(87);
  return 87;
}

void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 bytes, u32 type, u32 prot) {
  gap_trace_record("VirtualAlloc");
  // MEM_WRITE_WATCH is a REQUEST modifier, not an allocation type: real
  // Windows accepts it silently alongside MEM_RESERVE|COMMIT (bdwgc probes
  // with exactly this combination). Rejecting it fails the whole GC probe.
  const bool write_watch = (type & 0x00200000u) != 0;
  type &= ~0x00200000u;
  if (!(type & 0x3000u)) {
    set_err_for_current_thread(87);  // INVALID_PARAMETER
    return nullptr;
  }
  // .199 diagnostic: log large VirtualAlloc requests (the Sekiro arena
  // allocs are 1537/240/1536/64/16/100/116/800 MB; if the worker never
  // runs, these never appear). First 32.
  if (bytes >= (16ull << 20)) {
    static unsigned s_valloc_logged = 0;
    if (s_valloc_logged < 32) {
      ++s_valloc_logged;
      char vl[192];
      snprintf(vl, sizeof(vl), "thread: VirtualAlloc %lluMB type=0x%x prot=0x%x addr=%p",
               (unsigned long long)(bytes >> 20), type, prot, addr);
      trt_core_log(vl);
    }
  }
  const u32 pclean = (prot & 0xFFu) ? (prot & 0xFFu) : 0x04u;
  const unsigned tprot = va_prot_bits(pclean);

  // .210 FIX (Sekiro arena allocs): real Windows treats MEM_COMMIT with a NULL address as reserve+commit. Sekiro allocates its arenas as MEM_TOP_DOWN|MEM_COMMIT (type 0x101000, addr NULL); the bridge rejected MEM_COMMIT-alone with NULL, so every arena returned NULL and the game faulted popping the empty arena list at sekiro.exe+0x115c66b. Add MEM_RESERVE so the call takes the reserve+commit path below.
  if ((type & 0x1000u) && !(type & 0x2000u) && !addr) {
    type |= 0x2000u;
    // .220 DIAG: log the SIZE of every MEM_COMMIT-alone call -- the
    // 13 arena allocs in .219 were 1537/240/1536/64/16/100/116/27/
    // 800/116MB for the >=16MB ones, but 3 were <16MB and never
    // logged by the .199 diagnostic. With the size on this line,
    // all 13 sizes are directly countable, and the commit total
    // at death (gap final dump) tests the 5GB-cap kill.
    {
      char mcl[192];
      snprintf(mcl, sizeof(mcl),
               "kernel32: VirtualAlloc MEM_COMMIT-alone NULL %lluMB (%llu bytes) -> reserve+commit (Sekiro arena fix)",
               (unsigned long long)(bytes >> 20), (unsigned long long)bytes);
      trt_core_log(mcl);
    }
  }

  // MEM_COMMIT alone on a previously-reserved region (Mono/BoehmGC/Unity/CoreCLR)
  if (!(type & 0x2000u)) {
    if (!addr) {
      set_err_for_current_thread(87);
      return nullptr;
    }
    const uintptr_t raw = (uintptr_t)addr;
    const uintptr_t page = raw & ~(uintptr_t)4095ULL;
    const size_t span = (size_t)((raw - page) + (bytes ? bytes : 4096ULL));
    const size_t sz = (span + 4095ULL) & ~(size_t)4095ULL;
    bool ok = false;
    // bytes in the range not yet committed — the only bytes this call may
    // charge. Re-committing already-committed pages (Mono/Unity do this
    // routinely) must not double-charge the ledger.
    size_t fresh = 0;
    {
      std::lock_guard<std::mutex> lk(g_va_mu);
      const int idx = va_find_idx_locked(page);
      if (idx >= 0) {
        const va_region& r = g_va[(size_t)idx];
        ok = (sz <= r.size && (page - r.base) <= r.size - sz);
        if (ok) {
          if (prot == 0x04u && va_has_deferred_readwrite_locked(r, page, page + sz))
            return reinterpret_cast<void*>(page);
          fresh = sz - va_committed_in_locked(r, page, page + sz);
        }
      }
    }
    if (!ok) {
      set_err_for_current_thread(487);  // INVALID_ADDRESS
      return nullptr;
    }
    trt_vmem_commit_fn cfn = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_va_mu);
      cfn = g_vmem_commit;
    }
    auto do_commit = [&](unsigned want_prot) -> int {
      if (cfn) return cfn((void*)page, sz, want_prot);
      return g_be.protect ? g_be.protect((void*)page, sz, want_prot) : 0;
    };
    u32 eff_prot = pclean;
    bool pending_rx = false;
    if (trt_mem_budget_charge_commit((u64)fresh) != 0) return nullptr;  // 1455 set
    if (do_commit(tprot) != 0) {
      if (pclean == 0x40u || pclean == 0x80u) {
        if (do_commit(TRT_PROT_R | TRT_PROT_W) == 0) {
          eff_prot = 0x04u;
          pending_rx = true;
          trt_core_log("kernel32: VirtualAlloc(COMMIT RWX) -> W^X two-phase JIT (armed RW, flips RX on FlushInstructionCache)");
        } else {
          trt_mem_budget_discharge_commit((u64)fresh);
          set_err_for_current_thread(87);
          return nullptr;
        }
      } else {
        trt_mem_budget_discharge_commit((u64)fresh);
        set_err_for_current_thread(87);
        return nullptr;
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_va_mu);
      const int idx = va_find_idx_locked(page);
      if (idx >= 0) {
        // another thread may have committed part of the range between the
        // charge and this record: only the bytes still RESERVE here are
        // newly committed by THIS call — discharge the overlap so each
        // byte is charged exactly once, by whichever thread records first
        const size_t still_fresh =
            sz - va_committed_in_locked(g_va[(size_t)idx], page, page + sz);
        if (still_fresh < fresh)
          trt_mem_budget_discharge_commit((u64)(fresh - still_fresh));
        va_split_and_set_locked(g_va[(size_t)idx], page, page + sz, 0x1000,
                                eff_prot, pending_rx ? 1 : 0,
                                static_cast<int>(va_backing::eager));
      } else {
        // region vanished mid-commit (released by another thread): the
        // host commit is orphaned — discharge the whole charge
        trt_mem_budget_discharge_commit((u64)fresh);
      }
    }
    return (void*)page;
  }

  if (bytes > SIZE_MAX - 4095ULL) {
    set_err_for_current_thread(8);
    return nullptr;
  }
  const size_t sz = (size_t)(bytes ? ((bytes + 4095ULL) & ~4095ULL) : 4096ULL);
  const bool want_commit = (type & 0x1000u) != 0;
  bool defer_data = g_va_defer_data.load() && want_commit && !addr && !write_watch &&
      prot == 0x04u && (type & ~0x00103000u) == 0 && sz >= 128ULL * 1024 * 1024;
  trt_vmem_reserve_fn rfn = nullptr;
  if (!want_commit || defer_data) {
    std::lock_guard<std::mutex> lk(g_va_mu);
    rfn = g_vmem_reserve;
    // A backend without split reserve/commit still uses eager commit.
    defer_data = defer_data && rfn && g_vmem_commit && g_vmem_decommit;
  }
  if (!want_commit || defer_data) {
    // reserve-only: no commit, but the VA is consumed — charge the VA
    // ledger (a reservation that never commits still exhausts address
    // space, and the old code charged nothing at all here)
    if (trt_mem_budget_charge_va((u64)sz) != 0) return nullptr;  // 8 set
    void* p = rfn ? rfn(sz, (u64)(uintptr_t)addr)
                  : g_be.alloc(sz, (u64)(uintptr_t)addr);
    if (!p && addr) {
      p = rfn ? rfn(sz, 0) : g_be.alloc(sz, 0);
    }
    if (!p) {
      trt_mem_budget_discharge_va((u64)sz);
      set_err_for_current_thread(8);  // NOT_ENOUGH_MEMORY
      return nullptr;
    }
    // Keep reserved pages inaccessible on POSIX fallback without exhausting W^X or commit
    if (!rfn && g_be.protect && (pclean == 0x01u || (pclean & 0xF0u))) {
      g_be.protect(p, sz, 0);
    }
    try {
      va_record_add((uintptr_t)p, sz, pclean, pclean, defer_data ? 0x1000u : 0x2000u,
                    false, defer_data ? va_backing::deferred : va_backing::eager);
      if (defer_data) g_va_has_deferred_data.store(true);
    } catch (...) {
      g_be.release(p, sz);
      trt_mem_budget_discharge_va(sz);
      set_err_for_current_thread(8);
      return nullptr;
    }
    return p;
  }

  // reserve+commit: the bytes are both reserved VA and committed memory
  if (trt_mem_budget_charge_va((u64)sz) != 0) return nullptr;  // 8 set
  if (trt_mem_budget_charge_commit(sz) != 0) {
    trt_mem_budget_discharge_va((u64)sz);
    return nullptr;  // 1455 set
  }
  void* p = g_be.alloc(sz, (u64)(uintptr_t)addr);
  if (!p && addr) {
    // hint address unavailable: retry at any free base
    p = g_be.alloc(sz, 0);
  }
  if (!p) {
    trt_mem_budget_discharge_commit(sz);
    trt_mem_budget_discharge_va((u64)sz);
    set_err_for_current_thread(8);  // NOT_ENOUGH_MEMORY
    return nullptr;
  }
  u32 eff_prot = pclean;
  bool pending_rx = false;
  if (pclean != 0x04u && g_be.protect) {
    if (g_be.protect(p, sz, tprot) != 0) {
      if (pclean == 0x40u || pclean == 0x80u) {
        if (g_be.protect(p, sz, TRT_PROT_R | TRT_PROT_W) == 0) {
          eff_prot = 0x04u;
          pending_rx = true;
          trt_core_log("kernel32: VirtualAlloc(RWX) -> W^X two-phase JIT (armed RW, flips RX on FlushInstructionCache)");
        } else {
          trt_mem_budget_discharge_commit(sz);
          trt_mem_budget_discharge_va((u64)sz);
          g_be.release(p, sz);
          set_err_for_current_thread(87);
          return nullptr;
        }
      } else {
        if (pclean & 0xF0u) {
          trt_core_log("kernel32: VirtualAlloc(EXEC) rejected by host backend");
        }
        trt_mem_budget_discharge_commit(sz);
        trt_mem_budget_discharge_va((u64)sz);
        g_be.release(p, sz);
        set_err_for_current_thread(87);
        return nullptr;
      }
    }
  }
  va_record_add((uintptr_t)p, sz, pclean, eff_prot, 0x1000u, pending_rx);
  return p;
}

int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type) {
  if (!addr) {
    set_err_for_current_thread(87);
    return 0;
  }
  if (type == 0x4000u /*MEM_DECOMMIT*/) {
    const uintptr_t raw = (uintptr_t)addr;
    uintptr_t s = 0, e = 0;
    // bytes in [s, e) actually committed — the only bytes this call may
    // discharge. The old code discharged the whole span even when part of
    // it was already decommitted (discharge-without-charge, drifting the
    // ledger toward under-count and a platform kill).
    size_t committed_here = 0;
    trt_vmem_decommit_fn dfn = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_va_mu);
      dfn = g_vmem_decommit;
      const int idx = va_find_idx_locked(raw);
      if (idx >= 0) {
        va_region& r = g_va[(size_t)idx];
        if (bytes == 0 && raw == r.base) {
          s = r.base;
          e = r.base + r.size;
        } else if (bytes > 0) {
          s = raw & ~(uintptr_t)4095ULL;
          const size_t span = (size_t)((raw - s) + bytes);
          const size_t sz = (span + 4095ULL) & ~(size_t)4095ULL;
          e = s + sz;
          if (e > r.base + r.size) e = r.base + r.size;
        }
        if (s < e) {
          committed_here = va_committed_in_locked(r, s, e);
          va_split_and_set_locked(r, s, e, 0x2000, 0x01u, 0,
                                  static_cast<int>(va_backing::eager));
        }
      }
    }
    if (s < e) {
      if (dfn) dfn((void*)s, e - s);
      else if (g_be.protect) g_be.protect((void*)s, e - s, 0);
      // VA is unchanged: the reservation survives the decommit
      if (committed_here)
        trt_mem_budget_discharge_commit((u64)committed_here);
      return 1;
    }
    return 0;
  }
  if (type != 0x8000u /*MEM_RELEASE*/) return 0;
  size_t r_size = (size_t)bytes;
  u64 release_committed = 0;
  u64 release_va = 0;
  {
    std::lock_guard<std::mutex> lk(g_va_mu);
    const int idx = va_find_idx_locked((uintptr_t)addr);
    if (idx >= 0 && g_va[(size_t)idx].base == (uintptr_t)addr) {
      if (!r_size) r_size = g_va[(size_t)idx].size;
      for (const va_extent& ex : g_va[(size_t)idx].extents) {
        if (ex.state == 0x1000u && ex.backing != va_backing::deferred)
          release_committed += (u64)ex.size;
      }
      release_va = (u64)g_va[(size_t)idx].size;  // the whole reservation goes
      g_va[(size_t)idx].base = 0;
      g_va[(size_t)idx].extents.clear();
    }
  }
  if (release_committed) trt_mem_budget_discharge_commit(release_committed);
  if (release_va) trt_mem_budget_discharge_va(release_va);
  g_be.release(addr, r_size);
  return 1;
}

u64 TRT_STDCALL trt_VirtualQuery(const void* addr, void* mbi_out, u64 len) {
  struct mbi64 {
    u64 BaseAddress;
    u64 AllocationBase;
    u32 AllocationProtect;
    u16 PartitionId;
    u16 _pad;
    u64 RegionSize;
    u32 State;
    u32 Protect;
    u32 Type;
    u32 _pad2;
  };
  static_assert(sizeof(mbi64) == 48, "x64 MEMORY_BASIC_INFORMATION");
  if (!mbi_out || len < sizeof(mbi64)) {
    set_err_for_current_thread(87);
    return 0;
  }
  const uintptr_t a = (uintptr_t)addr;
  if (a > 0x7FFFFFFFFFFFULL) {
    set_err_for_current_thread(87);
    return 0;
  }
  const uintptr_t page = a & ~(uintptr_t)4095ULL;
  mbi64* m = (mbi64*)mbi_out;
  memset(m, 0, sizeof(mbi64));

  bool found = false;
  uintptr_t r_base = 0;
  u32 r_alloc_prot = 0;
  va_extent hit{};
  {
    std::lock_guard<std::mutex> lk(g_va_mu);
    const int idx = va_find_idx_locked(a);
    if (idx >= 0) {
      const va_region& r = g_va[(size_t)idx];
      if (const va_extent* ex = va_find_extent_locked(r, page)) {
        r_base = r.base;
        r_alloc_prot = r.alloc_prot;
        hit = *ex;
        const size_t index = static_cast<size_t>(ex - r.extents.data());
        // Hide host materialization splits when guest state/protection agree.
        for (size_t i = index + 1; i < r.extents.size(); ++i) {
          const auto& next = r.extents[i];
          if (next.start != hit.start + hit.size || next.state != hit.state ||
              next.prot != hit.prot) break;
          hit.size += next.size;
        }
        found = true;
      }
    }
  }
  if (found) {
    m->BaseAddress = (u64)page;
    m->AllocationBase = (u64)r_base;
    m->AllocationProtect = r_alloc_prot;
    m->RegionSize = (u64)((hit.start + hit.size) - page);
    m->State = hit.state;
    m->Protect = (hit.state == 0x1000u) ? hit.prot : 0u;
    m->Type = 0x20000u;  // MEM_PRIVATE
    return sizeof(mbi64);
  }

  const int nimg = trt_images_count();
  for (int i = 0; i < nimg; ++i) {
    const trt_image* img = trt_images_nth(i);
    if (!img || !img->base) continue;
    const uintptr_t b = (uintptr_t)img->base;
    const uintptr_t e = b + (uintptr_t)img->size_of_image;
    if (a >= b && a < e) {
      m->BaseAddress = (u64)page;
      m->AllocationBase = (u64)b;
      m->AllocationProtect = 0x20u;  // PAGE_EXECUTE_READ
      m->Type = 0x1000000u;          // MEM_IMAGE
      m->State = 0x1000u;            // MEM_COMMIT
      u32 prot = (page == b) ? 0x02u : 0x20u;
      u64 reg_end = (u64)e;
      const trt_dos_header* dos = (const trt_dos_header*)img->base;
      if (dos->e_magic == 0x5A4D && dos->e_lfanew > 0 && (u64)dos->e_lfanew < img->size_of_image) {
        const trt_nt_headers64* nt = (const trt_nt_headers64*)(img->base + dos->e_lfanew);
        if (nt->signature == 0x00004550) {
          const u32 rva = (u32)(a - b);
          const u16 nsec = nt->file_header.number_of_sections;
          const trt_section_header* secs =
              (const trt_section_header*)((const u8*)&nt->optional_header +
                                         nt->file_header.size_of_optional_header);
          for (u16 s = 0; s < nsec; ++s) {
            const u32 sva = secs[s].virtual_address;
            const u32 ssz = secs[s].virtual_size ? secs[s].virtual_size : secs[s].size_of_raw_data;
            const u32 salign = (ssz + 4095) & ~4095;
            if (rva >= sva && rva < sva + salign) {
              const u32 ch = secs[s].characteristics;
              if ((ch & 0x20000000) && (ch & 0x80000000)) prot = 0x40;  // PAGE_EXECUTE_READWRITE
              else if (ch & 0x20000000) prot = 0x20;                   // PAGE_EXECUTE_READ
              else if (ch & 0x80000000) prot = 0x04;                   // PAGE_READWRITE
              else prot = 0x02;                                        // PAGE_READONLY
              reg_end = (u64)(b + sva + salign);
              break;
            }
          }
        }
      }
      m->RegionSize = (reg_end > page) ? (reg_end - page) : 4096;
      m->Protect = prot;
      return sizeof(mbi64);
    }
  }

#if defined(_WIN32)
  const SIZE_T nr =
      ::VirtualQuery(addr, (PMEMORY_BASIC_INFORMATION)mbi_out, (SIZE_T)len);
  if (nr) return (u64)nr;
#else
  void* sb = nullptr;
  size_t ss = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    if (pthread_attr_getstack(&attr, &sb, &ss) == 0 && sb && ss) {
      const uintptr_t stack_low = (uintptr_t)sb;
      const uintptr_t stack_high = (uintptr_t)sb + (uintptr_t)ss;
      if (a >= stack_low && a < stack_high) {
        m->BaseAddress = (u64)page;
        m->AllocationBase = (u64)stack_low;
        m->AllocationProtect = 0x04u;  // PAGE_READWRITE
        m->RegionSize = (u64)(stack_high - page);
        m->State = 0x1000u;            // MEM_COMMIT
        m->Protect = 0x04u;            // PAGE_READWRITE
        m->Type = 0x20000u;            // MEM_PRIVATE
        pthread_attr_destroy(&attr);
        return sizeof(mbi64);
      }
    }
    pthread_attr_destroy(&attr);
  }

  unsigned char mincore_vec = 0;
  if (mincore((void*)page, 4096, &mincore_vec) == 0) {
    m->BaseAddress = (u64)page;
    m->AllocationBase = (u64)page;
    m->AllocationProtect = 0x04u;
    m->RegionSize = 4096;
    m->State = 0x1000u;      // MEM_COMMIT
    m->Protect = 0x04u;      // PAGE_READWRITE
    m->Type = 0x20000u;      // MEM_PRIVATE
    return sizeof(mbi64);
  }
#endif

  m->BaseAddress = (u64)page;
  m->RegionSize = 0x10000u;
  m->State = 0x10000u;  // MEM_FREE
  m->Protect = 0x01u;   // PAGE_NOACCESS
  return sizeof(mbi64);
}

int TRT_STDCALL trt_FlushInstructionCache(void* proc, const void* addr,
                                          u64 size) {
  (void)proc;
  if (addr) {
    const uintptr_t raw = (uintptr_t)addr;
    const uintptr_t rs = raw & ~(uintptr_t)4095ULL;
    const size_t span = (size_t)((raw - rs) + (size ? size : 1ULL));
    const uintptr_t re = rs + ((span + 4095ULL) & ~(size_t)4095ULL);
    struct span_pair { uintptr_t s, e; };
    std::vector<span_pair> to_flip;
    {
      std::lock_guard<std::mutex> lk(g_va_mu);
      for (size_t i = 0; i < g_va.size(); ++i) {
        const va_region& r = g_va[i];
        if (!r.base || r.base + r.size <= rs || r.base >= re) continue;
        for (size_t j = 0; j < r.extents.size(); ++j) {
          const va_extent& ex = r.extents[j];
          if (!ex.pending_rx) continue;
          const uintptr_t os = (ex.start > rs) ? ex.start : rs;
          const uintptr_t oe = ((ex.start + ex.size) < re) ? (ex.start + ex.size) : re;
          if (os < oe) to_flip.push_back({os, oe});
        }
      }
      for (size_t i = 0; i < g_extra_pending_rx.size(); ++i) {
        const va_extent& ex = g_extra_pending_rx[i];
        if (!ex.pending_rx) continue;
        const uintptr_t os = (ex.start > rs) ? ex.start : rs;
        const uintptr_t oe = ((ex.start + ex.size) < re) ? (ex.start + ex.size) : re;
        if (os < oe) to_flip.push_back({os, oe});
      }
    }
    for (size_t i = 0; i < to_flip.size(); ++i) {
      const uintptr_t s = to_flip[i].s;
      const uintptr_t e = to_flip[i].e;
      if (!g_be.protect ||
          g_be.protect((void*)s, e - s, TRT_PROT_R | TRT_PROT_X) == 0) {
        std::lock_guard<std::mutex> lk(g_va_mu);
        const int idx = va_find_idx_locked(s);
        if (idx >= 0) {
          va_split_and_set_locked(g_va[(size_t)idx], s, e, -1, 0x20u, 0);
        }
        for (size_t k = 0; k < g_extra_pending_rx.size(); ++k) {
          va_extent& ex = g_extra_pending_rx[k];
          if (ex.pending_rx && s <= ex.start && e >= ex.start + ex.size) {
            ex.pending_rx = false;
            ex.prot = 0x20u;
          }
        }
      }
    }
  }
#if defined(_WIN32)
  if (addr && size) {
    ::FlushInstructionCache(::GetCurrentProcess(), addr, (SIZE_T)size);
  }
#endif
#if defined(_MSC_VER)
  _ReadWriteBarrier();
  MemoryBarrier();
#else
  if (addr && size)
    __builtin___clear_cache((char*)(uintptr_t)addr,
                            (char*)(uintptr_t)addr + size);
  __sync_synchronize();
#endif
  return 1;
}

u32 TRT_STDCALL trt_GetLastError(void) { return err_for_current_thread(); }
void TRT_STDCALL trt_SetLastError(u32 err) {
  set_err_for_current_thread(err);
}

u32 trt_alloc_guest_thread_id(void) {
  u32 id = g_next_guest_tid.fetch_add(1, std::memory_order_relaxed);
  if (id == 0) id = g_next_guest_tid.fetch_add(1, std::memory_order_relaxed);
  return id;
}

void trt_bind_guest_thread_id(u32 tid) {
  if (tid != 0) t_guest_tid = tid;
}

u32 TRT_STDCALL trt_GetCurrentThreadId(void) {
#if defined(_WIN32)
  return (u32)::GetCurrentThreadId();
#else
  if (t_guest_tid == 0) t_guest_tid = trt_alloc_guest_thread_id();
  return t_guest_tid;
#endif
}

// the monotonic clock games use for timing (GetTickCount64 since Win7)
u64 TRT_STDCALL trt_GetTickCount64(void) {
  gap_trace_record("GetTickCount64");
  const i64 t = now_qpc_ticks();
  // our QPC source is nanoseconds; Windows' GetTickCount64 is milliseconds
  return (u64)(t / 1000000ll);
}

u32 TRT_STDCALL trt_GetTickCount(void) { return (u32)trt_GetTickCount64(); }
void* TRT_STDCALL trt_GetCurrentProcess(void) { return (void*)(uintptr_t)-1; }

int TRT_STDCALL trt_QueryPerformanceCounter(i64* out) {
  gap_trace_record("QueryPerformanceCounter");
  if (!out) return 0;
  *out = now_qpc_ticks();
  return 1;
}
int TRT_STDCALL trt_QueryPerformanceFrequency(i64* out) {
  if (!out) return 0;
  *out = 1000000000ll;  // nanosecond tick (matches QPC source above)
  return 1;
}

#if defined(_WIN32)
#include <intrin.h>
#endif

static bool host_supports_avx2() {
  static int cached = -1;
  if (cached != -1) return cached == 1;
#if defined(__x86_64__) || defined(_M_X64)
  int info[4] = {0};
#if defined(_WIN32)
  __cpuidex(info, 7, 0);
#elif defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("cpuid"
                       : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
                       : "a"(7), "c"(0));
#endif
  cached = (info[1] & (1 << 5)) ? 1 : 0;
#else
  cached = 0;
#endif
  return cached == 1;
}

static bool host_supports_bmi2() {
  static int cached = -1;
  if (cached != -1) return cached == 1;
#if defined(__x86_64__) || defined(_M_X64)
  int info[4] = {0};
#if defined(_WIN32)
  __cpuidex(info, 7, 0);
#elif defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("cpuid"
                       : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
                       : "a"(7), "c"(0));
#endif
  cached = (info[1] & (1 << 8)) ? 1 : 0;
#else
  cached = 0;
#endif
  return cached == 1;
}

u32 TRT_STDCALL trt_IsProcessorFeaturePresent(u32 feature) {
  // x64 baseline: SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2/CMPXCHG16B/POPCNT/AVX present.
  // STAGE 7 (OrTMM): 0x17 = PF_FASTFAIL_AVAILABLE — MSVC 2019+ CRT startup
  // queries it before __security_init; returning 0 sends _set_Fflag to
  // __fastfail(FAST_FAIL_FATAL_APP_EXIT) and the EXE dies at CRT-STARTUP
  // with int 0x29 (measured: OrTMM.exe SIGSEGV/UD at image+0xd1d4, the
  // `int $0x29` guarding call IsProcessorFeaturePresent(0x17)).
  // AMD Jaguar (Xbox One, One S, One X) has SSE3, CMPXCHG16B, SSSE3, SSE4.1,
  // SSE4.2, and split-128 AVX, but lacks AVX2 and BMI2. Zen 2 (Series S/X) has AVX2 & BMI2.
  switch (feature) {
    case 0:   // PF_FLOATING_POINT_PRECISION_ERRATA
      return 1;
    case 1:   // PF_FLOATING_POINT_EMULATED
      return 0;
    case 2:   // PF_COMPARE_EXCHANGE_DOUBLE (CMPXCHG8B)
    case 3:   // PF_MMX_INSTRUCTIONS_AVAILABLE
    case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 7:   // PF_3DNOW_INSTRUCTIONS_AVAILABLE
    case 8:   // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 9:   // PF_PAE_ENABLED
    case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    case 12:  // PF_NX_ENABLED
    case 13:  // PF_SSE3_INSTRUCTIONS_AVAILABLE
    case 14:  // PF_COMPARE_EXCHANGE128 (CMPXCHG16B)
    case 15:  // PF_COMPARE64_EXCHANGE128
    case 17:  // PF_XSAVE_ENABLED
    case 20:  // PF_SECOND_LEVEL_ADDRESS_TRANSLATION
    case 22:  // PF_RDWRFSGSBASE_AVAILABLE
    case 23:  // PF_FASTFAIL_AVAILABLE (0x17)
    case 24:  // PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE
    case 32:  // PF_RDTSCP_INSTRUCTION_AVAILABLE
    case 36:  // PF_SSSE3_INSTRUCTIONS_AVAILABLE
    case 37:  // PF_SSE4_1_INSTRUCTIONS_AVAILABLE
    case 38:  // PF_SSE4_2_INSTRUCTIONS_AVAILABLE
    case 39:  // PF_AVX_INSTRUCTIONS_AVAILABLE (AMD Jaguar supports 128-bit AVX)
      return 1;
    case 40:  // PF_AVX2_INSTRUCTIONS_AVAILABLE
      // Jaguar (Xbox One / One S / One X) lacks AVX2; Zen 2 (Series S/X) has AVX2.
      return host_supports_avx2() ? 1 : 0;
    case 41:  // PF_AVX512F_INSTRUCTIONS_AVAILABLE
      return 0;
    case 60:  // PF_BMI2_INSTRUCTIONS_AVAILABLE
      // Jaguar lacks BMI2; Zen 2 has BMI2.
      return host_supports_bmi2() ? 1 : 0;
    default:
      return 0;
  }
}

// Debugger sampling hook: no-op; gdb breaks here to stop the process at the
// moment the CS-stall probe names a wedged owner (see bridge_thread.cpp).
extern "C" __attribute__((noinline, used)) void trt_cs_stall_probe_pause(void) {
  __asm__ volatile("" ::: "memory");
}

void TRT_STDCALL trt_Sleep(u32 ms) {
  // .219 DIAG: record Sleep(0) too -- a Sleep(0) yield spin was
  // invisible in .218 (ms>0 only). Consecutive duplicates collapse
  // in the ring, so a yield spin is one entry with a huge repeat
  // count, not 32 evicted slots.
  gap_trace_record("Sleep");
#ifdef _WIN32
  ::Sleep(ms);  // we are inside a Windows process already
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000l;
  nanosleep(&ts, nullptr);
#endif
}

// ---- address-based waiting (api-ms-win-core-synch-l1-2-0; UnityPlayer's
// bucket allocator waits on the bucket lock word through WaitOnAddress).
// STAGE 7 EXE boot: without these the import binds to a fail-visible thunk
// that returns 0 and the allocator's retry loop unwinds with a NULL block
// that the caller dereferences (measured live: Go Next demo.exe SIGSEGV at
// UnityPlayer+0x16da6a6, vtable call returned 0).
static bool wait_addr_same(volatile void* addr, void* cmp, u32 size) {
  // the loads are ATOMIC (acquire): the signaler's store is Interlocked
  // (guest code) — a plain read here is a mixed-atomicity data race
  // (measured by the TSan gate) and real Win32 gives the wait an acquire
  // fence anyway.
  switch (size) {
    case 1: return __atomic_load_n((volatile u8*)addr, __ATOMIC_ACQUIRE) ==
                    *(u8*)cmp;
    case 2: return __atomic_load_n((volatile u16*)addr, __ATOMIC_ACQUIRE) ==
                    *(u16*)cmp;
    case 4: return __atomic_load_n((volatile u32*)addr, __ATOMIC_ACQUIRE) ==
                    *(u32*)cmp;
    case 8: return __atomic_load_n((volatile u64*)addr, __ATOMIC_ACQUIRE) ==
                    *(u64*)cmp;
    default: return false;
  }
}

struct wait_addr_waiter {
  volatile void* addr = nullptr;
  std::condition_variable cv;
  bool signalled = false;
  wait_addr_waiter* next = nullptr;
};
struct wait_addr_state {
  std::mutex mu;
  wait_addr_waiter* head = nullptr;
};
static wait_addr_state g_wait_addr[64];
// Fail-visible wait/wake accounting: a lost or misdirected wake is invisible
// from a post-mortem (the waiter just never resumes). Counters answer
// "did the wake ever fire" without a debugger attached to a wedged thread.
// Defined in bridge_kernel32.cpp (WaitOnAddress/WakeByAddress impls); read
// from the CS-stall probe here — that probe bypasses the log ring, and a
// lost wake is exactly the kind of wedge the ring can't report.
std::atomic<u64> g_wa_waits{0}, g_wa_wake_single{0}, g_wa_wake_all{0},
    g_wa_wake_single_miss{0}, g_wa_wake_all_miss{0}, g_wa_timed_out{0};
// .250 DIAG: address the RtlWaitOnAddress trap caught a guest thread
// parking on (set by the VEH fire log in the snapshot trap
// carryforward). The .249 park address is per-run (heap/ASLR), so it
// is recorded dynamically, never hardcoded. The bridged
// WakeByAddress* log TARGET lines for this address, and
// trt_WaitOnAddress logs its polled value over time.
static std::atomic<u64> g_wa_trap_addr{0};
static wait_addr_state* wait_addr_slot(volatile void* addr) {
  const uintptr_t v = (uintptr_t)addr;
  return &g_wait_addr[((v >> 4) ^ (v >> 12)) & 63];
}

#if defined(_WIN32)
using pfn_WaitOnAddress = BOOL (WINAPI*)(volatile void*, PVOID, SIZE_T, DWORD);
using pfn_WakeByAddress = void (WINAPI*)(PVOID);
struct win_wait_addr_api {
  std::once_flag once;
  pfn_WaitOnAddress wait = nullptr;
  pfn_WakeByAddress wake_single = nullptr;
  pfn_WakeByAddress wake_all = nullptr;
};
static win_wait_addr_api g_win_wait_api;
static void init_win_wait_api() {
  std::call_once(g_win_wait_api.once, []() {
    HMODULE mod = ::GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll");
    if (!mod) mod = ::GetModuleHandleW(L"kernelbase.dll");
    if (!mod) mod = ::GetModuleHandleW(L"kernel32.dll");
    if (!mod) return;
    auto w = (pfn_WaitOnAddress)::GetProcAddress(mod, "WaitOnAddress");
    auto ws = (pfn_WakeByAddress)::GetProcAddress(mod, "WakeByAddressSingle");
    auto wa = (pfn_WakeByAddress)::GetProcAddress(mod, "WakeByAddressAll");
    if (w && ws && wa) {
      g_win_wait_api.wait = w;
      g_win_wait_api.wake_single = ws;
      g_win_wait_api.wake_all = wa;
    }
  });
}
#endif

int TRT_STDCALL trt_WaitOnAddress(volatile void* addr, void* cmp,
                                  u32 size, u32 ms) {
  if (!addr || !cmp || (size != 1 && size != 2 && size != 4 && size != 8)) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  const u64 wa_n = g_wa_waits.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // .249 DIAG: log the first 16 bridged WaitOnAddress calls (addr/size/ms
  // + the value currently at addr) and a counter summary every 1000
  // waits. The .248 Ultrapool park is NtWaitForAlertByThreadId -- if the
  // bridged WaitOnAddress is never called by the guest main thread, the
  // park is via the real ntdll RtlWaitOnAddress (bypassing the bridge
  // IAT bind) or another primitive entirely.
  {
    static std::atomic<u64> s_wa_logged{0};
    if (s_wa_logged.load(std::memory_order_relaxed) < 16) {
      s_wa_logged.fetch_add(1, std::memory_order_relaxed);
      u64 cur = 0;
      switch (size) {
        case 1:
          cur = __atomic_load_n((volatile u8*)addr, __ATOMIC_ACQUIRE);
          break;
        case 2:
          cur = __atomic_load_n((volatile u16*)addr, __ATOMIC_ACQUIRE);
          break;
        case 4:
          cur = __atomic_load_n((volatile u32*)addr, __ATOMIC_ACQUIRE);
          break;
        case 8:
          cur = __atomic_load_n((volatile u64*)addr, __ATOMIC_ACQUIRE);
          break;
        default:
          break;
      }
      u64 cmp_v = 0;
      switch (size) {
        case 1: cmp_v = *(u8*)cmp; break;
        case 2: cmp_v = *(u16*)cmp; break;
        case 4: cmp_v = *(u32*)cmp; break;
        case 8: cmp_v = *(u64*)cmp; break;
        default: break;
      }
      char line[224];
      snprintf(line, sizeof(line),
               "kernel32: WaitOnAddress #%llu tid=%u addr=0x%llx size=%u "
               "ms=%u cur=0x%llx cmp=0x%llx caller=%p",
               (unsigned long long)wa_n + 1, trt_GetCurrentThreadId(),
               (unsigned long long)(uintptr_t)addr, size, ms,
               (unsigned long long)cur, (unsigned long long)cmp_v,
               __builtin_return_address(0));
      trt_core_log(line);
    }
    // A retry loop can call this millions of times per second; a summary
    // every 1000 waits turns the ring itself into the bottleneck (and the
    // log into gigabytes). One line per million keeps the wedge visible.
    if ((wa_n + 1) % 1000000 == 0) {
      char line[256];
      snprintf(line, sizeof(line),
               "kernel32: wa: waits=%llu wake_single=%llu wake_all=%llu "
               "miss_single=%llu miss_all=%llu timed_out=%llu",
               (unsigned long long)g_wa_waits.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_single.load(
                   std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_all.load(
                   std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_single_miss.load(
                   std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_all_miss.load(
                   std::memory_order_relaxed),
               (unsigned long long)g_wa_timed_out.load(
                   std::memory_order_relaxed));
      trt_core_log(line);
    }
  }
  // .250 DIAG: value-over-time for the address the RtlWaitOnAddress
  // trap caught a guest thread parking on. The worker polls it every
  // 100ms via this bridge call; log the first 8 polls after the trap
  // fires, then every 50th (~1 per 5s) -- does anything ever write
  // to it? The .249 park value was stuck at 0x1.
  {
    const u64 trap_addr = g_wa_trap_addr.load(std::memory_order_relaxed);
    if (trap_addr != 0 && (u64)(uintptr_t)addr == trap_addr) {
      static std::atomic<u64> s_trap_poll_n{0};
      const u64 n = s_trap_poll_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || n % 50 == 0) {
        u64 cur = 0;
        switch (size) {
          case 1:
            cur = __atomic_load_n((volatile u8*)addr, __ATOMIC_ACQUIRE);
            break;
          case 2:
            cur = __atomic_load_n((volatile u16*)addr, __ATOMIC_ACQUIRE);
            break;
          case 4:
            cur = __atomic_load_n((volatile u32*)addr, __ATOMIC_ACQUIRE);
            break;
          case 8:
            cur = __atomic_load_n((volatile u64*)addr, __ATOMIC_ACQUIRE);
            break;
          default:
            break;
        }
        char line[192];
        snprintf(line, sizeof(line),
                 "kernel32: WaitOnAddress TRAP-ADDR #%llu tid=%u cur=0x%llx",
                 (unsigned long long)n, trt_GetCurrentThreadId(),
                 (unsigned long long)cur);
        trt_core_log(line);
      }
    }
  }
  if (!wait_addr_same(addr, cmp, size)) return 1;
  if (ms == 0) {
    err_ref_for_current_thread() = 1460;
    return 0;
  }
#if defined(_WIN32)
  init_win_wait_api();
  if (g_win_wait_api.wait) {
    const BOOL r = g_win_wait_api.wait(addr, cmp, (SIZE_T)size, (DWORD)ms);
    if (!r) err_ref_for_current_thread() = (u32)::GetLastError();
    return r ? 1 : 0;
  }
#endif
  wait_addr_state* st = wait_addr_slot(addr);
  wait_addr_waiter node;
  node.addr = addr;
  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  std::unique_lock<std::mutex> lk(st->mu);
  node.next = st->head;
  st->head = &node;
  int ok = 1;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  while (wait_addr_same(addr, cmp, size)) {
    node.signalled = false;
    if (ms == 0xFFFFFFFFu) {
      node.cv.wait(lk, [&] {
        return node.signalled || !wait_addr_same(addr, cmp, size);
      });
    } else {
      if (!node.cv.wait_until(lk, deadline, [&] {
            return node.signalled || !wait_addr_same(addr, cmp, size);
          })) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ok = wait_addr_same(addr, cmp, size) ? 0 : 1;
        break;
      }
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  wait_addr_waiter** pp = &st->head;
  while (*pp) {
    if (*pp == &node) {
      *pp = node.next;
      break;
    }
    pp = &(*pp)->next;
  }
  lk.unlock();
  if (!ok) {
    err_ref_for_current_thread() = 1460;
    g_wa_timed_out.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

void TRT_STDCALL trt_WakeByAddressSingle(volatile void* addr) {
  if (!addr) return;
  // .250 DIAG: log EVERY wake targeting the address the RtlWaitOnAddress
  // trap caught a guest thread parking on. The .249 first-8 throttle
  // was consumed by Sekiro, so Ultrapool's wakes to the park address
  // were invisible. Includes the value at the address (size 4, the
  // .249 park size).
  {
    const u64 trap_addr = g_wa_trap_addr.load(std::memory_order_relaxed);
    if (trap_addr != 0 && (u64)(uintptr_t)addr == trap_addr) {
      const u32 cur =
          __atomic_load_n((volatile u32*)addr, __ATOMIC_ACQUIRE);
      char line[160];
      snprintf(line, sizeof(line),
               "kernel32: WakeByAddressSingle TARGET addr=0x%llx cur=0x%x",
               (unsigned long long)trap_addr, cur);
      trt_core_log(line);
    }
  }
  // .249 DIAG: log the first 8 wake calls (the .248 Ultrapool park is a
  // wait that never resolves -- these lines say whether any wake fired).
  {
    static std::atomic<u64> s_wake_single_logged{0};
    const u64 n =
        s_wake_single_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
      char line[128];
      snprintf(line, sizeof(line),
               "kernel32: WakeByAddressSingle #%llu addr=0x%llx",
               (unsigned long long)n,
               (unsigned long long)(uintptr_t)addr);
      trt_core_log(line);
    }
  }
#if defined(_WIN32)
  init_win_wait_api();
  if (g_win_wait_api.wake_single) {
    g_win_wait_api.wake_single((PVOID)addr);
    return;
  }
#endif
  std::atomic_thread_fence(std::memory_order_seq_cst);
  wait_addr_state* st = wait_addr_slot(addr);
  std::lock_guard<std::mutex> lk(st->mu);
  bool hit = false;
  for (wait_addr_waiter* w = st->head; w; w = w->next) {
    if (w->addr == addr && !w->signalled) {
      w->signalled = true;
      w->cv.notify_one();
      hit = true;
      break;
    }
  }
  g_wa_wake_single.fetch_add(1, std::memory_order_relaxed);
  if (!hit) g_wa_wake_single_miss.fetch_add(1, std::memory_order_relaxed);
}

void TRT_STDCALL trt_WakeByAddressAll(volatile void* addr) {
  if (!addr) return;
  // .250 DIAG: log EVERY wake targeting the address the RtlWaitOnAddress
  // trap caught a guest thread parking on (see WakeByAddressSingle).
  {
    const u64 trap_addr = g_wa_trap_addr.load(std::memory_order_relaxed);
    if (trap_addr != 0 && (u64)(uintptr_t)addr == trap_addr) {
      const u32 cur =
          __atomic_load_n((volatile u32*)addr, __ATOMIC_ACQUIRE);
      char line[160];
      snprintf(line, sizeof(line),
               "kernel32: WakeByAddressAll TARGET addr=0x%llx cur=0x%x",
               (unsigned long long)trap_addr, cur);
      trt_core_log(line);
    }
  }
  // .249 DIAG: log the first 8 wake calls (see WakeByAddressSingle).
  {
    static std::atomic<u64> s_wake_all_logged{0};
    const u64 n =
        s_wake_all_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
      char line[128];
      snprintf(line, sizeof(line),
               "kernel32: WakeByAddressAll #%llu addr=0x%llx",
               (unsigned long long)n,
               (unsigned long long)(uintptr_t)addr);
      trt_core_log(line);
    }
  }
#if defined(_WIN32)
  init_win_wait_api();
  if (g_win_wait_api.wake_all) {
    g_win_wait_api.wake_all((PVOID)addr);
    return;
  }
#endif
  std::atomic_thread_fence(std::memory_order_seq_cst);
  wait_addr_state* st = wait_addr_slot(addr);
  std::lock_guard<std::mutex> lk(st->mu);
  bool hit = false;
  for (wait_addr_waiter* w = st->head; w; w = w->next) {
    if (w->addr == addr && !w->signalled) {
      w->signalled = true;
      w->cv.notify_one();
      hit = true;
    }
  }
  g_wa_wake_all.fetch_add(1, std::memory_order_relaxed);
  if (!hit) g_wa_wake_all_miss.fetch_add(1, std::memory_order_relaxed);
}

// ---- boot ----
void trt_bridge_boot(const trt_backend* be) {
  if (be) { g_be = *be; g_be_ready = true; }
  // the core needs the backend too: fail-visible thunks allocate through it
  trt_core_set_backend(be);
  // module coverage for real PC games (idempotent; bridges override)
  trt_modules_register_defaults();
  // the graphics bridge takes over d3d11/dxgi (its own functions first,
  // everything else falls through to the console's real DLLs)
  trt_bridge_register_d3d();
  trt_bridge_register_d3d12();
  // the registry: UWP has none, games expect one
  trt_bridge_register_registry();
  // the engine-API batch: mapped files, threads, system information
  trt_bridge_register_memory();
  trt_bridge_register_threads();
  trt_bridge_register_system();
  // DirectInput 8: how most PC games read the mouse and keyboard
  trt_bridge_register_dinput();
  // XAudio2: how games play sound
  trt_bridge_register_audio();
  // winmm: the multimedia clock (timeGetTime) and legacy-device probes
  trt_bridge_register_winmm();
  // d3d9: the D3D9 -> D3D11 bridge (M5)
  trt_bridge_register_d3d9();
  // dsound: DirectSound -> the same console audio sink (M6 coverage)
  trt_bridge_register_dsound();
  // ddraw: DirectDraw -> backend present (M6 coverage)
  trt_bridge_register_ddraw();
  // d3dx9: D3DX math/textures/sprites (M6 coverage) — most-versions
  trt_bridge_register_d3dx9();
  // d3d8: the 2001-2004 generation on the same backend hooks (M7)
  trt_bridge_register_d3d8();
  // openal32: the Minecraft-generation audio API onto the same sink
  trt_bridge_register_openal();
  // ole32/oleaut32: COM boot + BSTR/variant/safearray (games call
  // CoInitialize at boot; console UWP ole32 lacks the full PC surface)
  trt_bridge_register_ole();
  // shell32: SHGetFolderPath/KnownFolderPath for save-game dirs
  trt_bridge_register_shell();
  // shlwapi: Path* string/file helpers
  trt_bridge_register_shlwapi();
  trt_bridge_register_actctx();
  trt_bridge_register_version();
  // msacm32/msvfw32/winspool: codec + printer boot probes (M6 coverage)
  trt_bridge_register_mmedia();
  // d3dx10/11: texture/shader helpers for the D3D10/11 generation (M7)
  trt_bridge_register_d3dx();
  // Steamworks: AUTH ONLY (no DRM bypass, ever)
  trt_bridge_register_steam();
  // comctl32: common-controls acceptance (OrTMM imports ordinal #380)
  trt_bridge_register_comctl();
  // gdi32: the GDI object/state surface (fonts, DC-coupled state)
  trt_bridge_register_gdi();
  // gdiplus: GDI+ 2D flat API (BotW-Native launcher/setup UI, recorded)
  trt_bridge_register_gdiplus();
  // fmod: FMOD Ex/Studio audio (wav/pcm real through the host mixer)
  trt_bridge_register_fmod();
  // wwise: Audiokinetic Wwise audio middleware (stubs, AK_Success)
  trt_bridge_register_wwise();
  // bink: RAD Game Tools Bink video (stubs, skip)
  trt_bridge_register_bink();
  // criware: CRI Middleware (ADX2/CriAtom/CriMana) (stubs)
  trt_bridge_register_criware();
  // miles: Miles Sound System (stubs)
  trt_bridge_register_miles();
  // STAGE 5 boot determinism: the 7 UNCERTAIN console DLLs converted to
  // BUILTIN honest-runtime — the boot no longer depends on unproven
  // console presence (wininet/psapi/iphlpapi/imm32/crypt32/dbghelp/dbgcore)
  trt_bridge_register_wininet();
  trt_bridge_register_psapi();
  trt_bridge_register_iphlpapi();
  trt_bridge_register_imm32();
  trt_bridge_register_crypt32();
  trt_bridge_register_bcrypt();
  trt_bridge_register_dbghelp();
  trt_bridge_register_ntdll();
  trt_bridge_register_console_pack();
  trt_bridge_register_sys_extras();
  trt_bridge_register_vulkan();
  trt_bridge_register_mf();
  trt_bridge_register_gameinput();
  trt_bridge_register_wldap32();
  // ktmw32: transaction handles (discord_game_sdk imports 2 at LOAD time)
  trt_bridge_register_ktmw32();
  trt_bridge_set_cmd_args("");
  g_err_epoch.fetch_add(1, std::memory_order_release);
  t_last_error = 0;
  t_last_error_epoch = g_err_epoch.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(g_fls_mu);
    memset(g_fls_meta, 0, sizeof(g_fls_meta));
  }
  g_fls_epoch.fetch_add(1, std::memory_order_release);
  memset(t_fls_slots, 0, sizeof(t_fls_slots));
}

// ================= files on the VFS (the game-transfer path) ================
namespace {

struct file_rec { void* os; u8 mode; u32 refs; char vpath[512]; u32 io_modes; };
std::deque<file_rec> g_files;
std::mutex g_files_mu;

bool os_seek64(FILE* os, i64 off, int whence) {
#ifdef _WIN32
  return _fseeki64(os, off, whence) == 0;
#else
  return fseeko(os, (off_t)off, whence) == 0;
#endif
}

i64 os_tell64(FILE* os) {
#ifdef _WIN32
  return (i64)_ftelli64(os);
#else
  return (i64)ftello(os);
#endif
}

file_rec* file_at_locked(void* h, u32* out_idx = nullptr) {
  const u64 hv = (u64)(uintptr_t)h;
  if (hv == (u64)-1 || (hv >> 20) != 0x777u) return nullptr;
  const u32 idx = (u32)(hv & 0xFFFFFu);
  if (idx >= g_files.size() || !g_files[idx].os) return nullptr;
  if (out_idx) *out_idx = idx;
  return &g_files[idx];
}

int file_release_locked(u32 idx) {
  if (idx >= g_files.size() || !g_files[idx].os) return 0;
  if (g_files[idx].refs > 1) {
    --g_files[idx].refs;
    return 1;
  }
  // .252 FIX: directory handles hold a real host HANDLE
  // (mode 'd'), not a FILE* -- CloseHandle it instead of fclose.
#ifdef _WIN32
  if (g_files[idx].mode == 'd') ::CloseHandle((HANDLE)g_files[idx].os);
  else fclose((FILE*)g_files[idx].os);
#else
  fclose((FILE*)g_files[idx].os);
#endif
  g_files[idx].os = nullptr;
  g_files[idx].refs = 0;
  g_files[idx].vpath[0] = 0;
  return 1;
}

FILE* file_os(void* h) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* r = file_at_locked(h);
  // .252 FIX: directory handles (mode 'd', host HANDLE in os)
  // are not FILE* -- return null so ReadFile/WriteFile/
  // GetFileSizeEx/GetFileInformationByHandle fail gracefully
  // (ERROR_INVALID_HANDLE) instead of passing a HANDLE to stdio.
  if (!r || r->mode == 'd') return nullptr;
  return (FILE*)r->os;
}

// extern-C accessors for gap / ntdll bridges
extern "C" FILE* trt_file_os(void* h) { return file_os(h); }
extern "C" const char* trt_file_vpath(void* h) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* r = file_at_locked(h);
  return r ? r->vpath : nullptr;
}

extern "C" int trt_file_copy_vpath(void* handle, char* path, size_t capacity) {
  std::lock_guard<std::mutex> lock(g_files_mu);
  const file_rec* record = file_at_locked(handle);
  if (!record || !record->vpath[0]) return 0;
  const size_t length = strlen(record->vpath);
  if (length >= capacity) return 0;
  memcpy(path, record->vpath, length + 1);
  return 1;
}

char g_cwd[512] = "C:\\";   // guest cwd
char g_exepath[512] = "Z:\\game.exe";
char g_cmd_args[1024] = "";

void build_guest_vpath(const u16* name, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  if (!name) return;
  if (trt_vfs_normalize_guest_utf16(name, out, cap) == 0) return;
  char raw[512];
  if (trt_utf16_to_utf8(name, raw, sizeof(raw)) != 0) return;
  for (char* p = raw; *p; ++p) if (*p == '/') *p = '\\';
  if (raw[0] && raw[1] == ':') {
    if (raw[0] >= 'a' && raw[0] <= 'z') raw[0] = (char)(raw[0] - 'a' + 'A');
    snprintf(out, cap, "%s", raw);
  } else if (raw[0] == '\\') {
    const char drv = (g_cwd[0] >= 'a' && g_cwd[0] <= 'z')
                         ? (char)(g_cwd[0] - 'a' + 'A')
                         : (g_cwd[0] ? g_cwd[0] : 'C');
    snprintf(out, cap, "%c:%s", drv, raw);
  } else {
    const size_t cl = strlen(g_cwd);
    const char* sep = (cl > 0 && g_cwd[cl - 1] == '\\') ? "" : "\\";
    snprintf(out, cap, "%s%s%s", g_cwd, sep, raw);
  }
}

void* file_alloc(FILE* os, u8 mode, const char* vpath = nullptr) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  for (size_t i = 0; i < g_files.size(); ++i) {
    if (!g_files[i].os) {
      g_files[i].os = os;
      g_files[i].mode = mode;
      g_files[i].refs = 1;
      g_files[i].io_modes = 0;  // a reused slot resets its notification modes
      snprintf(g_files[i].vpath, sizeof(g_files[i].vpath), "%s",
               vpath ? vpath : "");
      return (void*)(uintptr_t)(0x77700000u | (u32)i);
    }
  }
  if (g_files.size() >= 0xFFFFFu) return (void*)(uintptr_t)-1;
  file_rec rec{};
  rec.os = os;
  rec.mode = mode;
  rec.refs = 1;
  snprintf(rec.vpath, sizeof(rec.vpath), "%s", vpath ? vpath : "");
  const u32 idx = (u32)g_files.size();
  g_files.push_back(rec);
  return (void*)(uintptr_t)(0x77700000u | idx);
}

// extern-C accessor for the gap bridge (bridge_kernel32_gap.cpp)
extern "C" char* trt_gap_cwd(void) { return g_cwd; }
// extern-C accessor for the gap bridge: the guest exe path (toolhelp
// MODULEENTRY32W.szExePath for the main module reads this)
extern "C" const char* trt_gap_exepath(void) { return g_exepath; }

} // namespace

void trt_bridge_set_exe_path(const char* zpath) {
  if (!zpath) return;
  strncpy(g_exepath, zpath, sizeof(g_exepath) - 1);
  g_exepath[sizeof(g_exepath) - 1] = 0;
}

void trt_bridge_set_cmd_args(const char* args) {
  if (!args) {
    g_cmd_args[0] = 0;
    return;
  }
  while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n') {
    ++args;
  }
  size_t i = 0;
  for (; args[i] && i < sizeof(g_cmd_args) - 1; ++i) {
    const char c = args[i];
    g_cmd_args[i] = (c == '\r' || c == '\n') ? ' ' : c;
  }
  while (i > 0 && (g_cmd_args[i - 1] == ' ' || g_cmd_args[i - 1] == '\t')) {
    --i;
  }
  g_cmd_args[i] = 0;
}

void trt_bridge_set_cwd(const char* guest_dir) {
  if (!guest_dir || !guest_dir[0]) return;
  trt_vfs_set_cwd(guest_dir);
  if (trt_vfs_get_cwd(g_cwd, sizeof(g_cwd)) != 0) {
    strncpy(g_cwd, guest_dir, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    for (char* p = g_cwd; *p; ++p) {
      if (*p == '/') *p = '\\';
    }
  }
}

void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access, u32 share,
                                  void* sa, u32 disposition, u32 flags,
                                  void* tmpl) {
  (void)share; (void)sa; (void)tmpl;
  if (!name) return (void*)(uintptr_t)-1;
  // All write access masks need a writable VFS target, including
  // FILE_APPEND_DATA, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES and GENERIC_ALL.
  const u32 write_access = 0x40000000u | 0x10000000u | 0x00000002u |
                           0x00000004u | 0x00000010u | 0x00000100u;
  const bool write = (access & write_access) != 0;
  // disposition: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING,
  // 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING
  const u32 disp = disposition ? disposition : 3;
  const int for_write = (write || disp == 1 || disp == 2 || disp == 4 || disp == 5) ? 1 : 0;
  char host[1024];
  // A disc image is read-only, exactly like the disc it came from: a write
  // open answers ERROR_WRITE_PROTECT instead of silently diverging from it.
  if (trt_vfs_is_image_utf16(name)) {
    if (for_write) {
      err_ref_for_current_thread() = 19;  // ERROR_WRITE_PROTECT
      trt_core_log("kernel32: CreateFileW(write) on a mounted disc image -> "
                   "ERROR_WRITE_PROTECT");
      return (void*)(uintptr_t)-1;
    }
    int img_dir = 0;
    if (trt_vfs_image_materialize(name, host, sizeof(host), &img_dir) != 0 ||
        img_dir) {
      err_ref_for_current_thread() = img_dir ? 5 : 2;
      char plog[1100];
      snprintf(plog, sizeof(plog), "kernel32: CreateFileW(disc image) -> %s",
               img_dir ? "ERROR_ACCESS_DENIED (directory)"
                       : "ERROR_FILE_NOT_FOUND");
      trt_core_log(plog);
      return (void*)(uintptr_t)-1;
    }
    FILE* ios = fopen(host, "rb");
    if (!ios) {
      err_ref_for_current_thread() = 2;
      return (void*)(uintptr_t)-1;
    }
    char vpath[512];
    build_guest_vpath(name, vpath, sizeof(vpath));
    return file_alloc(ios, 'r', vpath);
  }
  if (trt_vfs_resolve_utf16_rw(name, for_write, host, sizeof(host)) != 0) {
    err_ref_for_current_thread() = 3; // PATH_NOT_FOUND
    return (void*)(uintptr_t)-1;
  }
  int is_directory = 0;
  if (disp == 3 && trt_vfs_stat(host, &is_directory, nullptr) == 0 && is_directory) {
    if (!(flags & 0x02000000u /*FILE_FLAG_BACKUP_SEMANTICS*/)) {
      err_ref_for_current_thread() = 5;  // ERROR_ACCESS_DENIED
      return (void*)(uintptr_t)-1;
    }
    FILE* directory = nullptr;
#ifdef _WIN32
    wchar_t wide[1024];
    if (::MultiByteToWideChar(CP_UTF8, 0, host, -1, wide, 1024) <= 0) {
      err_ref_for_current_thread() = 3;  // ERROR_PATH_NOT_FOUND
      return (void*)(uintptr_t)-1;
    }
    CREATEFILE2_EXTENDED_PARAMETERS parameters = {};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFileFlags = 0x02000000u;  // FILE_FLAG_BACKUP_SEMANTICS
    const HANDLE native = ::CreateFile2(wide, access, share, 3, &parameters);
    if (native == INVALID_HANDLE_VALUE) {
      err_ref_for_current_thread() = ::GetLastError();
      return (void*)(uintptr_t)-1;
    }
    directory = reinterpret_cast<FILE*>(native);
#else
    directory = fopen(host, "rb");
    if (!directory) {
      err_ref_for_current_thread() = errno == EACCES ? 5 : 3;
      return (void*)(uintptr_t)-1;
    }
#endif
    char vpath[512];
    build_guest_vpath(name, vpath, sizeof(vpath));
    void* handle = file_alloc(directory, 'd', vpath);
    if (handle == (void*)(uintptr_t)-1) {
#ifdef _WIN32
      ::CloseHandle(reinterpret_cast<HANDLE>(directory));
#else
      fclose(directory);
#endif
      err_ref_for_current_thread() = 8;  // ERROR_NOT_ENOUGH_MEMORY
      return handle;
    }
    err_ref_for_current_thread() = 0;
    return handle;
  }
  if (disp == 1 /*CREATE_NEW*/) {
    char check[1024];
    if (trt_vfs_resolve_utf16_rw(name, 0, check, sizeof(check)) == 0 &&
        trt_vfs_stat(check, nullptr, nullptr) == 0) {
      err_ref_for_current_thread() = 80; // ERROR_FILE_EXISTS
      return (void*)(uintptr_t)-1;
    }
  }
  if (for_write) {
    trt_vfs_ensure_parents(host);
    // Copy-on-write when opening an existing Z: file for modification
    if ((disp == 3 || disp == 4 || disp == 5) &&
        trt_vfs_stat(host, nullptr, nullptr) != 0) {
      char ro[1024];
      int is_dir = 0;
      if (trt_vfs_resolve_utf16_rw(name, 0, ro, sizeof(ro)) == 0 &&
          strcmp(ro, host) != 0 && trt_vfs_stat(ro, &is_dir, nullptr) == 0 &&
          !is_dir) {
        FILE* in = fopen(ro, "rb");
        if (in) {
          FILE* cp = fopen(host, "wb");
          if (cp) {
            char cbuf[16384];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), in)) > 0)
              fwrite(cbuf, 1, n, cp);
            fclose(cp);
          }
          fclose(in);
        }
      }
    }
  }
  FILE* os = nullptr;
  if (disp == 1) {
    os = fopen(host, "w+bx");
    if (!os && errno == EEXIST) {
      err_ref_for_current_thread() = 80;  // ERROR_FILE_EXISTS
      return (void*)(uintptr_t)-1;
    }
  } else if (disp == 2) {
    os = fopen(host, "w+b");
  } else if (disp == 4) {
    os = fopen(host, "r+b");
    if (!os) os = fopen(host, "w+b");
  } else if (disp == 5) {
    os = fopen(host, "w+b");
  } else {
    os = fopen(host, write ? "r+b" : "rb");
  }
  if (!os && write && disp != 1) {
    // Parent directories may not exist yet. Real Windows CreateFileW with
    // CREATE_ALWAYS also fails when the parent dir is absent — but Unity's
    // CrashReporter writes insights-crash-report.json without ever calling
    // CreateDirectoryW (it expects the app's own dir tree to already exist,
    // which the desktop install created and our VFS did not). Create parents
    // once and retry, exactly like a fresh Windows install dir.
    trt_vfs_ensure_parents(host);
    os = fopen(host, "r+b");
    if (!os && (disp == 2 || disp == 5)) os = fopen(host, "w+b");
  }
  if (!os) {
    const int open_errno = errno;
    int cf_is_dir = 0;
    (void)trt_vfs_stat(host, &cf_is_dir, nullptr);
    err_ref_for_current_thread() = 2; // FILE_NOT_FOUND
    trt_core_log("kernel32: CreateFileW open failed on host (see path)");
    char plog[1100];
    snprintf(plog, sizeof(plog),
             "  path was: %s (access=0x%08x disposition=%u write=%d "
             "flags=0x%08x is_dir=%d errno=%d)",
             host, access, disp, write, flags, cf_is_dir, open_errno);
    trt_core_log(plog);
    return (void*)(uintptr_t)-1;
  }
  if (for_write) trt_vfs_mark_z_deleted_utf16(name, 0, 0);
  char vpath[512];
  build_guest_vpath(name, vpath, sizeof(vpath));
  char succ_log[1100];
  snprintf(succ_log, sizeof(succ_log), "kernel32: CreateFileW opened '%s' (host: '%s')", vpath, host);
  trt_core_log(succ_log);
  return file_alloc(os, write ? 'w' : 'r', vpath);
}

static int file_xfer_impl(void* h, void* buf, u32 bytes, int is_write,
                          void* overlapped, size_t* n_out, u32* io_err_out);
// mmap fast path for large positional reads (defined below; shared with
// trt_file_pread — the streaming engines' window population).
static bool file_pread_mmap_locked(FILE* os, u64 offset, void* dst,
                                   size_t bytes, size_t* got_out);

static int file_xfer_impl(void* h, void* buf, u32 bytes, int is_write,
                          void* overlapped, size_t* n_out, u32* io_err_out) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(h);
  // .252 FIX: directory handles (mode 'd') are not FILE* --
  // ReadFile/WriteFile on a directory fails with
  // ERROR_ACCESS_DENIED on real Windows, so fail here instead
  // of passing a HANDLE to stdio.
  if (rec && rec->mode == 'd') {
    trt_SetLastError(5);
    if (n_out) *n_out = 0;
    return 0;
  }
  FILE* os = rec ? (FILE*)rec->os : nullptr;
  if (!os) {
    trt_SetLastError(6);
    if (n_out) *n_out = 0;
    return 0;
  }
  if (!buf && bytes > 0) {
    trt_SetLastError(87);
    if (n_out) *n_out = 0;
    return 0;
  }
  if (overlapped && !is_write) {
    const u64 off = ((const u64*)overlapped)[2];
    size_t got = 0;
    if (file_pread_mmap_locked(os, off, buf, bytes, &got)) {
      if (n_out) *n_out = got;
      // at/past EOF is a clean zero-byte read: ERROR_HANDLE_EOF, exactly the
      // fread path's contract (the guest's ReadFileEx maps it to STATUS_END_OF_FILE)
      if (io_err_out) *io_err_out = (bytes > 0 && got == 0) ? 38u : 0u;
      return 1;
    }
  }
  if (overlapped) {
    const u64 off = ((const u64*)overlapped)[2];
    os_seek64(os, (i64)off, SEEK_SET);
  }
  const size_t n =
      bytes ? (is_write ? fwrite(buf, 1, bytes, os) : fread(buf, 1, bytes, os))
            : 0;
  if (n_out) *n_out = n;
  u32 io_err = 0;
  if (bytes > 0 && n == 0) {
    if (ferror(os)) {
      clearerr(os);
      trt_SetLastError(5);
      return 0;
    }
    if (!is_write && feof(os)) io_err = 38u;  // ERROR_HANDLE_EOF
  }
  if (io_err_out) *io_err_out = io_err;
  return 1;
}

int TRT_STDCALL trt_ReadFile(void* h, void* buf, u32 bytes, u32* read,
                             void* overlapped) {
  extern int TRT_STDCALL trt_SetEvent(u64 handle);
  extern void trt_iocp_notify_io(u64 file_handle, u32 bytes, void* overlapped);
  size_t n = 0;
  if (!buf || !trt_va_prepare_data_range(buf, bytes) ||
      !file_xfer_impl(h, buf, bytes, 0, overlapped, &n, nullptr)) {
    if (read) *read = 0;
    return 0;
  }
  if (read) *read = (u32)n;
  if (overlapped) {
    u64* ov = (u64*)overlapped;
    ov[0] = 0;
    ov[1] = (u64)n;
    const u64 hev = ov[3];
    if (hev) trt_SetEvent(hev);
    trt_iocp_notify_io((u64)(uintptr_t)h, (u32)n, overlapped);
  }
  return 1;
}

// Raw guest stdout/stderr bytes -> the log ring (defined with the console
// surface below); WriteFile routes the Win32 stdio pseudo-handles here.
void console_log_bytes(const char* buf, u32 bytes);

int TRT_STDCALL trt_WriteFile(void* h, const void* buf, u32 bytes, u32* written,
                              void* overlapped) {
  extern int TRT_STDCALL trt_SetEvent(u64 handle);
  extern void trt_iocp_notify_io(u64 file_handle, u32 bytes, void* overlapped);
  // Synthetic stdio: the Win32 standard pseudo-handles route to the log ring
  // so the guest's own stdout/stderr (Godot's error log) is a discovery
  // surface. Raw bytes only — no console-mode surface exists behind these.
  const intptr_t hv = (intptr_t)h;
  if (hv == -10 || hv == -11 || hv == -12) {
    if (buf && bytes) {
      console_log_bytes((const char*)buf, bytes);
    }
    if (written) *written = bytes;
    return 1;
  }
  size_t n = 0;
  if (!buf || !trt_va_prepare_data_range(buf, bytes) ||
      !file_xfer_impl(h, (void*)buf, bytes, 1, overlapped, &n, nullptr)) {
    if (written) *written = 0;
    return 0;
  }
  if (written) *written = (u32)n;
  if (overlapped) {
    u64* ov = (u64*)overlapped;
    ov[0] = 0;
    ov[1] = (u64)n;
    const u64 hev = ov[3];
    if (hev) trt_SetEvent(hev);
    trt_iocp_notify_io((u64)(uintptr_t)h, (u32)n, overlapped);
  }
  return 1;
}

int TRT_STDCALL trt_ReadFileEx(void* h, void* buf, u32 bytes, void* overlapped,
                               void* completion_routine) {
  if (!overlapped) {
    trt_SetLastError(87);
    return 0;
  }
  size_t n = 0;
  u32 io_err = 0;
  if (!file_xfer_impl(h, buf, bytes, 0, overlapped, &n, &io_err)) return 0;
  u64* ov = (u64*)overlapped;
  ov[0] = (io_err == 38u) ? 0xC0000011ull /*STATUS_END_OF_FILE*/ : 0ull;
  ov[1] = (u64)n;
  // MSDN: ReadFileEx ignores OVERLAPPED.hEvent; callers store cookies/this there.
  if (completion_routine) {
    trt_queue_io_apc(completion_routine, io_err, (u64)n, overlapped);
  }
  return 1;
}

int TRT_STDCALL trt_WriteFileEx(void* h, const void* buf, u32 bytes,
                                void* overlapped, void* completion_routine) {
  if (!overlapped) {
    trt_SetLastError(87);
    return 0;
  }
  size_t n = 0;
  if (!file_xfer_impl(h, (void*)buf, bytes, 1, overlapped, &n, nullptr)) return 0;
  u64* ov = (u64*)overlapped;
  ov[0] = 0;
  ov[1] = (u64)n;
  if (completion_routine) {
    trt_queue_io_apc(completion_routine, 0, (u64)n, overlapped);
  }
  return 1;
}

// Retain/release and 64-bit windowed I/O helpers for streaming memory mappings.
int trt_file_retain(void* handle) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(handle);
  if (!rec) return 0;
  ++rec->refs;
  return 1;
}

int trt_file_release(void* handle) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  u32 idx = 0;
  if (!file_at_locked(handle, &idx)) return 0;
  return file_release_locked(idx);
}

int trt_file_stat_size(void* handle, u64* size_out) {
  if (size_out) *size_out = 0;
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(handle);
  if (!rec) return 0;
  // .252 FIX: directory handles are not FILE* -- GetFileSize
  // on a directory returns failure here (real Windows:
  // INVALID_FILE_SIZE).
  if (rec->mode == 'd') return 0;
  FILE* os = (FILE*)rec->os;
  fflush(os);
  const i64 cur = os_tell64(os);
  if (cur < 0 || !os_seek64(os, 0, SEEK_END)) return 0;
  const i64 end = os_tell64(os);
  os_seek64(os, cur, SEEK_SET);
  if (end < 0) return 0;
  if (size_out) *size_out = (u64)end;
  return 1;
}

// Host descriptor + real size behind a guest file handle. bridge_mem.cpp uses
// this to FILE-BACK read-only views: the view's pages become the file's clean
// page cache (mapped, not copied), so an asset archive costs address space
// instead of committed RAM and evicted pages re-fault from the SSD. -1 for
// anything that is not a regular host file (pipes, devices, non-Linux hosts).
int trt_file_host_fd(void* handle, u64* size_out) {
  if (size_out) *size_out = 0;
#ifdef __linux__
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(handle);
  if (!rec || !rec->os) return -1;
  FILE* os = (FILE*)rec->os;
  fflush(os);  // pending guest writes must be visible to the mapped pages
  const int fd = fileno(os);
  if (fd < 0) return -1;
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  if (size_out) *size_out = (u64)st.st_size;
  return fd;
#else
  (void)handle;
  return -1;
#endif
}

// mmap fast path for LARGE positional reads (streaming engines' window
// population): map the page range, copy from the page cache, unmap — no
// stdio buffering, no seek dance. Falls back to the fread path for small
// reads, non-regular files, on mmap failure, and on non-Linux hosts (the
// console keeps its native path). TRT_IO_MMAP_MIN tunes/disables (bytes).
static bool file_pread_mmap_locked(FILE* os, u64 offset, void* dst,
                                   size_t bytes, size_t* got_out) {
#ifdef __linux__
  static const size_t k_mmap_min = [] {
    const char* env = getenv("TRT_IO_MMAP_MIN");
    const long long v = env && *env ? atoll(env) : 1048576LL;  // 1 MiB default
    return v > 0 ? (size_t)v : 0;  // 0 disables the fast path
  }();
  if (!k_mmap_min || bytes < k_mmap_min) return false;
  struct stat st{};
  const int fd = fileno(os);
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return false;
  const u64 fsize = (u64)st.st_size;
  if (offset >= fsize) {  // at/past EOF: a clean zero-byte read
    if (got_out) *got_out = 0;
    return true;
  }
  const size_t want =
      (u64)bytes <= fsize - offset ? bytes : (size_t)(fsize - offset);
  const u64 pa = offset & ~4095ULL;
  const size_t span = (size_t)((offset - pa) + want);
  void* m = mmap(nullptr, span, PROT_READ, MAP_PRIVATE, fd, (off_t)pa);
  if (m == MAP_FAILED) return false;  // fall back to the fread path
  memcpy(dst, (const u8*)m + (offset - pa), want);
  munmap(m, span);
  if (got_out) *got_out = want;
  return true;
#else
  (void)os; (void)offset; (void)dst; (void)bytes; (void)got_out;
  return false;
#endif
}

int trt_file_pread(void* handle, u64 offset, void* dst, size_t bytes,
                   size_t* read_out) {
  if (read_out) *read_out = 0;
  if ((!dst && bytes > 0) || !trt_va_prepare_data_range(dst, bytes)) return 0;
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(handle);
  if (!rec) return 0;
  // .252 FIX: directory handles are not FILE* -- pread on a
  // directory fails (real Windows: ERROR_ACCESS_DENIED).
  if (rec->mode == 'd') return 0;
  FILE* os = (FILE*)rec->os;
  size_t got = 0;
  if (file_pread_mmap_locked(os, offset, dst, bytes, &got)) {
    if (read_out) *read_out = got;
    return 1;
  }
  const i64 cur = os_tell64(os);
  if (!os_seek64(os, (i64)offset, SEEK_SET)) {
    if (cur >= 0) os_seek64(os, cur, SEEK_SET);
    return 0;
  }
  got = bytes ? fread(dst, 1, bytes, os) : 0;
  if (cur >= 0) os_seek64(os, cur, SEEK_SET);
  if (read_out) *read_out = got;
  return 1;
}

int trt_file_pwrite(void* handle, u64 offset, const void* src, size_t bytes,
                    size_t* written_out) {
  if (written_out) *written_out = 0;
  if ((!src && bytes > 0) || !trt_va_prepare_data_range(src, bytes)) return 0;
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(handle);
  if (!rec || rec->mode == 'd') return 0;
  FILE* os = (FILE*)rec->os;
  const i64 cur = os_tell64(os);
  if (!os_seek64(os, (i64)offset, SEEK_SET)) {
    if (cur >= 0) os_seek64(os, cur, SEEK_SET);
    return 0;
  }
  const size_t put = bytes ? fwrite(src, 1, bytes, os) : 0;
  fflush(os);
  if (cur >= 0) os_seek64(os, cur, SEEK_SET);
  if (written_out) *written_out = put;
  return put == bytes;
}

// Legacy full-read helper kept for compatibility (bridge_mem uses pread now).
void* trt_file_bytes(void* handle, size_t* size) {
  u64 sz = 0;
  if (!trt_file_stat_size(handle, &sz)) return nullptr;
  u8* buf = (u8*)malloc((size_t)(sz > 0 ? sz : 1));
  if (!buf) return nullptr;
  size_t got = 0;
  if (sz > 0 && !trt_file_pread(handle, 0, buf, (size_t)sz, &got)) {
    free(buf);
    return nullptr;
  }
  if (size) *size = got;
  return buf;
}

int TRT_STDCALL trt_sync_close(u64 handle);

int TRT_STDCALL trt_CloseHandle(void* h) {
  // Dispatch order: sync objects -> memory mappings -> gap registries
  // (toolhelp snapshots + job records, tag-checked) -> open files.
  if (trt_sync_close((u64)(uintptr_t)h)) return 1;
  if (trt_mem_close_handle(h)) return 1;
  {
    extern int trt_gap_handle_close(void* h);
    if (trt_gap_handle_close(h)) return 1;
  }
  std::lock_guard<std::mutex> lk(g_files_mu);
  u32 idx = 0;
  if (!file_at_locked(h, &idx)) return 1;  // non-file handles: pretend closed
  return file_release_locked(idx);
}

u32 TRT_STDCALL trt_GetFileSize(void* h, u32* hi);

u32 TRT_STDCALL trt_SetFilePointer(void* h, i64 dist, i64* dist_hi, u32 method) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(h);
  if (!rec) {
    err_ref_for_current_thread() = 6;
    return 0xFFFFFFFFu;
  }
  // .252 FIX: directory handles are not FILE* -- SetFilePointer
  // on a directory fails on real Windows (ERROR_INVALID_HANDLE).
  if (rec->mode == 'd') {
    err_ref_for_current_thread() = 6;
    return 0xFFFFFFFFu;
  }
  FILE* os = (FILE*)rec->os;
  i64 off = (i64)(i32)dist;
  if (dist_hi) {
    i32* hi32 = (i32*)dist_hi;
    off = (i64)(((u64)(u32)*hi32 << 32) | (u32)dist);
  }
  const int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
  if (!os_seek64(os, off, whence)) {
    err_ref_for_current_thread() = 131;  // ERROR_NEGATIVE_SEEK
    return 0xFFFFFFFFu;
  }
  const i64 pos = os_tell64(os);
  if (pos < 0) return 0xFFFFFFFFu;
  if (dist_hi) {
    i32* hi32 = (i32*)dist_hi;
    *hi32 = (i32)((u64)pos >> 32);
  }
  err_ref_for_current_thread() = 0;
  return (u32)((u64)pos & 0xFFFFFFFFu);
}

u32 TRT_STDCALL trt_GetFileSize(void* h, u32* hi) {
  if (hi) *hi = 0;
  u64 sz = 0;
  if (!trt_file_stat_size(h, &sz)) return 0xFFFFFFFFu;
  if (hi) *hi = (u32)(sz >> 32);
  return (u32)(sz & 0xFFFFFFFFu);
}

int TRT_STDCALL trt_SetCurrentDirectoryW(const u16* name) {
  if (!name || !name[0]) {
    err_ref_for_current_thread() = 87;  // ERROR_INVALID_PARAMETER
    return 0;
  }
  char guest_norm[512];
  if (trt_vfs_normalize_guest_utf16(name, guest_norm, sizeof(guest_norm)) != 0) {
    err_ref_for_current_thread() = 3;  // ERROR_PATH_NOT_FOUND
    return 0;
  }
  char ov[1024] = "", ro[1024] = "", rel[512] = "";
  if (trt_vfs_resolve_z_pair_utf16(name, ov, sizeof(ov), ro, sizeof(ro),
                                   rel, sizeof(rel)) == 0) {
    if (rel[0] && trt_vfs_is_z_deleted_rel(rel, 0)) {
      err_ref_for_current_thread() = 3;
      return 0;
    }
    int ov_dir = 0, ro_dir = 0;
    const bool ov_ok = ov[0] && trt_vfs_stat(ov, &ov_dir, nullptr) == 0;
    const bool opq = rel[0] && trt_vfs_is_z_opaque_rel(rel) != 0;
    const bool ro_ok = ro[0] && !opq && trt_vfs_stat(ro, &ro_dir, nullptr) == 0;
    if ((ov_ok && !ov_dir) || (!ov_ok && ro_ok && !ro_dir)) {
      err_ref_for_current_thread() = 267;  // ERROR_DIRECTORY
      return 0;
    }
    if ((!ov_ok || !ov_dir) && (!ro_ok || !ro_dir)) {
      err_ref_for_current_thread() = 3;
      return 0;
    }
  } else {
    char host[1024];
    int is_dir = 0;
    if (trt_vfs_resolve_utf16(name, host, sizeof(host)) != 0 ||
        trt_vfs_stat(host, &is_dir, nullptr) != 0) {
      err_ref_for_current_thread() = 3;
      return 0;
    }
    if (!is_dir) {
      err_ref_for_current_thread() = 267;
      return 0;
    }
  }
  trt_vfs_set_cwd(guest_norm);
  trt_vfs_get_cwd(g_cwd, sizeof(g_cwd));
  return 1;
}

int TRT_STDCALL trt_SetCurrentDirectoryA(const char* name) {
  if (!name || !name[0]) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  u16 w[512];
  u32 i = 0;
  for (; name[i] && i < 511; ++i) w[i] = (u16)(u8)name[i];
  w[i] = 0;
  return trt_SetCurrentDirectoryW(w);
}

int TRT_STDCALL trt_GetCurrentDirectoryW(u32 len, u16* buf) {
  trt_vfs_get_cwd(g_cwd, sizeof(g_cwd));
  const u32 clen = (u32)strlen(g_cwd);
  static u32 s_gcd_log = 0;
  if (++s_gcd_log <= 32) {
    char st_line[256];
    snprintf(st_line, sizeof(st_line), "strtrace: GetCurrentDirectoryW -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, g_cwd);
    trt_core_log(st_line);
  }
  if (!buf || len <= clen) return (int)(clen + 1);
  for (u32 i = 0; i <= clen; ++i) buf[i] = (u16)(u8)g_cwd[i];
  return (int)clen;
}

extern const u16* trt_gap_env_build_wblock(const char* const* defaults,
                                           size_t default_count);
extern const char* const* trt_gap_env_defaults(size_t* count);
extern int trt_gap_env_free_wblock(const u16* block);

const u16* TRT_STDCALL trt_GetEnvironmentStringsW() {
  // 0.2.2.95: the full standard Windows env set, shared with the store seed
  // (bridge_kernel32_gap.cpp kTrtDefaultEnv) so the advertised block and
  // GetEnvironmentVariableW/_wgetenv agree.
  size_t default_count = 0;
  const char* const* pairs = trt_gap_env_defaults(&default_count);
  const u16* st_blk = trt_gap_env_build_wblock(pairs, default_count);
  { char st_tmp[136]; char st_line[256]; if (st_blk) { size_t st_o = 0; for (size_t st_i = 0; (st_blk[st_i] || st_blk[st_i + 1]) && st_o < 130; ++st_i) st_tmp[st_o++] = st_blk[st_i] ? (char)(st_blk[st_i] & 0x7f) : ';'; st_tmp[st_o] = 0; snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentStringsW -> ptr=%p \"%.128s\"", (const void*)st_blk, st_tmp); } else { snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentStringsW -> NULL"); } trt_core_log(st_line); }
  return st_blk;
}

int TRT_STDCALL trt_FreeEnvironmentStringsW(const u16* b) {
  return trt_gap_env_free_wblock(b);
}

u32 TRT_STDCALL trt_GetModuleFileNameW(void* mod, u16* buf, u32 len) {
  // Honor the handle: NULL (or the exe's own handle) -> the exe path; a
  // guest DLL handle -> the game's directory + the DLL's file name; a
  // synthetic system-module handle -> System32 + the module name. The old
  // code ignored the handle and always returned the exe path.
  char path[512];
  const char* value = g_exepath;
  if (mod) {
    trt_image* img = trt_images_by_handle(mod);
    if (img && img != trt_images_first()) {
      const char* nm = trt_images_name(mod);
      if (nm && nm[0]) {
        const char* slash = strrchr(g_exepath, '\\');
        const size_t dirlen = slash ? (size_t)(slash - g_exepath) : 0;
        if (dirlen > 0 && dirlen + 1 + strlen(nm) < sizeof(path)) {
          snprintf(path, sizeof(path), "%.*s\\%s", (int)dirlen, g_exepath, nm);
          value = path;
        }
      }
    } else if (!img && trt_images_is_synthetic(mod)) {
      const char* nm = trt_images_synth_name(mod);
      if (nm && nm[0]) {
        snprintf(path, sizeof(path), "C:\\Windows\\System32\\%s", nm);
        value = path;
      }
    }
  }
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetModuleFileNameW -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, value); trt_core_log(st_line); }
  if (!buf || !len) return 0;
  u32 i = 0;
  for (; value[i] && i < len - 1; ++i) buf[i] = (u16)(u8)value[i];
  buf[i] = 0;
  return i;
}

int TRT_STDCALL trt_GetSystemDirectoryW(u16* buf, u32 len) {
  if (!buf || len < 18) return 18;
  const char* s = "C:\\Windows\\System32";
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetSystemDirectoryW -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, s); trt_core_log(st_line); }
  u32 i = 0;
  for (; s[i]; ++i) buf[i] = (u16)(u8)s[i];
  buf[i] = 0;
  return i;
}

void trt_bridge_new_thread_id(u32 id) {
  t_guest_tid = id ? id : g_next_guest_tid.fetch_add(1);
}

// ================= the loader surface (guest DLLs) ==========================
// A shipped game is not one file: it brings engine DLLs and helper DLLs,
// and often its own version.dll/winmm.dll shims. LoadLibrary is how the
// guest reaches them, so it is as load-critical as kernel32 itself.
namespace {

bool k32_file_exists(const char* p) {
  int is_dir = 0;
  return p && p[0] && trt_vfs_stat(p, &is_dir, nullptr) == 0 && !is_dir;
}

bool k32_try_exact_or_dll(const char* base_path, bool allow_dll_ext,
                          char* out, size_t cap) {
  if (!base_path || !base_path[0]) return false;
  if (k32_file_exists(base_path)) {
    snprintf(out, cap, "%s", base_path);
    return true;
  }
  if (allow_dll_ext) {
    const size_t n = strlen(base_path);
    const char* bn = base_path;
    for (const char* p = base_path; *p; ++p)
      if (*p == '/' || *p == '\\') bn = p + 1;
    if (n > 0 && base_path[n - 1] != '.' && !strchr(bn, '.')) {
      char with_ext[1030];
      if (snprintf(with_ext, sizeof(with_ext), "%s.dll", base_path) <
              (int)sizeof(with_ext) &&
          k32_file_exists(with_ext)) {
        snprintf(out, cap, "%s", with_ext);
        return true;
      }
    }
  }
  return false;
}

// "C:\\dir\\engine.dll" / "engine.dll" / "engine" -> an existing host path
bool guest_path_to_host(const u16* guest_path, char* out, size_t cap) {
  char utf8[1024];
  if (trt_utf16_to_utf8(guest_path, utf8, sizeof(utf8)) != 0) return false;
  const char* p = utf8;
  if (((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') &&
       (p[2] == '?' || p[2] == '.') && (p[3] == '\\' || p[3] == '/')) ||
      (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')) {
    p += 4;
  }
  if (!p[0]) return false;
  const bool rooted = (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
  if (rooted) {
    char cand[1024];
    if (trt_vfs_resolve_utf16(guest_path, cand, sizeof(cand)) == 0) {
      return k32_try_exact_or_dll(cand, true, out, cap);
    }
    return false;
  }
  // 1. Loaded-image parent dirs, game dir, SetDllDirectoryW, AddDllDirectory
  if (trt_runtime_find_dll(p, out, cap) == 0) return true;
  // 2. CWD-relative VFS path
  {
    char cand[1024];
    if (trt_vfs_resolve_utf16(guest_path, cand, sizeof(cand)) == 0 &&
        k32_try_exact_or_dll(cand, true, out, cap)) {
      return true;
    }
  }
  // 3. Guest PATH environment variable directories
  const char* trt_gap_env_get(const char*);
  const char* path_env = trt_gap_env_get("PATH");
  if (!path_env) path_env = trt_gap_env_get("Path");
  if (path_env && path_env[0]) {
    const char* cur = path_env;
    while (*cur) {
      while (*cur == ';' || *cur == ' ') ++cur;
      const char* end = cur;
      while (*end && *end != ';') ++end;
      if (end > cur && (size_t)(end - cur) < 500) {
        u16 dir_w[512];
        size_t dn = (size_t)(end - cur);
        for (size_t k = 0; k < dn; ++k) dir_w[k] = (u16)(u8)cur[k];
        dir_w[dn] = 0;
        char dir_h[1024];
        if (trt_vfs_resolve_utf16(dir_w, dir_h, sizeof(dir_h)) == 0 && dir_h[0]) {
          size_t dl = strlen(dir_h);
          while (dl > 0 && (dir_h[dl - 1] == '/' || dir_h[dl - 1] == '\\')) --dl;
          char clean_rel[512];
          size_t rl = 0;
          for (; p[rl] && rl + 1 < sizeof(clean_rel); ++rl)
            clean_rel[rl] = (p[rl] == '\\') ? '/' : p[rl];
          clean_rel[rl] = 0;
          char cand[1024];
          if (dl > 0 &&
              snprintf(cand, sizeof(cand), "%.*s/%s", (int)dl, dir_h, clean_rel) <
                  (int)sizeof(cand) &&
              k32_try_exact_or_dll(cand, true, out, cap)) {
            return true;
          }
        }
      }
      cur = end;
    }
  }
  return false;
}

} // namespace

// ---- synthetic system-module handles (STAGE 7, OrTMM) ---------------------
// The MSVC CRT startup (__crt_load_kernel32 / ucrt's own boot) calls
// LoadLibraryW("kernel32.dll") and probes api-set contract names
// (api-ms-win-core-synch-l1-2-0, api-ms-win-core-fibers-l1-1-1). On real
// Windows those SUCCEED — every one is loaded in-process. Here they must
// succeed too: a NULL return makes the CRT failfast (int 0x29) and the
// game dies before WinMain (measured: OrTMM.exe SIGSEGV at image+0xd1d4,
// the `int $0x29` guarding IsProcessorFeaturePresent(0x17)).
// The handle for a system module is a synthetic tag the image registry
// already understands (trt_images_by_handle); GetProcAddress on it resolves
// through the module table — the SAME surface the static import binder
// used, so the answers are identical.
namespace {
const char* path_basename(const char* s) {
  if (!s) return "";
  const char* b1 = strrchr(s, '\\');
  const char* b2 = strrchr(s, '/');
  const char* b = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
  return b ? b + 1 : s;
}

bool eq_ci(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

void* system_module_handle(const char* utf8_name) {
  if (!utf8_name || !utf8_name[0]) return nullptr;
  const char* base = path_basename(utf8_name);
  // a guest image with this name wins (Windows: the loaded module list)
  if (trt_images_find(base)) return nullptr;  // caller falls through to images
  // LoadLibrary appends ".dll" when the name has no extension; the module
  // table registers names WITH the extension.
  char with_ext[96];
  if (!strchr(base, '.')) {
    snprintf(with_ext, sizeof(with_ext), "%s.dll", base);
    base = with_ext;
  }
  // api-set contract names resolve to their implementation module first
  if (const char* target = trt_apiset_target(base)) base = target;
  if (trt_module_route(base) == TRT_ROUTE_MISSING) return nullptr;
  void* h = trt_images_synthetic_handle(base);
  if (h) {
    static u32 s_syn_log = 0;
    if (++s_syn_log <= 64) {
      char line[224];
      snprintf(line, sizeof(line),
               "kernel32: LoadLibrary/GetModuleHandle(%s) -> synthetic handle "
               "(system module, offline bridge surface)", base);
      trt_core_log(line);
    }
  }
  if (!h) {
    char st_line[224];
    snprintf(st_line, sizeof(st_line),
             "modtrace: LoadLibrary/GetModuleHandle(%s) -> NULL (synthetic handle failed)",
             base);
    trt_core_log(st_line);
  }
  return h;
}
} // namespace

static void* load_library_impl(const u16* path, u32 flags) {
  if (!path || !path[0]) {
    err_ref_for_current_thread() = 87;  // ERROR_INVALID_PARAMETER
    return nullptr;
  }
  // Never load a guest-shipped steam_api64.dll / steam_api.dll; always route
  // Steamworks calls through Tiramisu's BUILTIN bridge (auth-only, zero DRM bypass).
  char utf8_in[1024];
  if (trt_utf16_to_utf8(path, utf8_in, sizeof(utf8_in)) == 0) {
    const char* b = path_basename(utf8_in);
    if (eq_ci(b, "steam_api64.dll") || eq_ci(b, "steam_api64") ||
        eq_ci(b, "steam_api.dll") || eq_ci(b, "steam_api")) {
      if (void* h = system_module_handle(b)) return h;
    }
    // If bare name (no path separators/drive) matches an already-loaded image
    // (including from SetDllDirectory / Plugins subdir), return that handle.
    const bool bare = !strchr(utf8_in, ':') && !strchr(utf8_in, '\\') &&
                      !strchr(utf8_in, '/');
    if (bare) {
      if (trt_image* loaded = trt_images_find(b)) {
        return trt_images_handle(loaded);
      }
    }
  }
  char host[1024];
  if (!guest_path_to_host(path, host, sizeof(host))) {
    char utf8[256];
    if (trt_utf16_to_utf8(path, utf8, sizeof(utf8)) == 0) {
      if (void* h = system_module_handle(utf8)) return h;
    }
    err_ref_for_current_thread() = 126;  // ERROR_MOD_NOT_FOUND
    return nullptr;
  }
  // already loaded? Windows returns the same handle
  const char* base = path_basename(host);
  if (trt_image* img = trt_images_find(base)) return trt_images_handle(img);

  // LOAD_LIBRARY_AS_DATAFILE (0x2), LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE (0x20),
  // DONT_RESOLVE_DLL_REFERENCES (0x1): map sections without executing DllMain.
  const int run_entry = (flags & 0x23u) ? 0 : 1;
  trt_image* img = trt_load_pe(host, base, run_entry);
  if (!img) {
    char utf8[256];
    if (trt_utf16_to_utf8(path, utf8, sizeof(utf8)) == 0) {
      if (void* h = system_module_handle(utf8)) return h;
    }
    err_ref_for_current_thread() = 126;  // MOD_NOT_FOUND
    return nullptr;
  }

  return trt_images_handle(img);
}

void* TRT_STDCALL trt_LoadLibraryW(const u16* path) {
  gap_trace_record("LoadLibraryW");
  void* h = load_library_impl(path, 0);
  static u32 s_llw_log = 0;
  if (++s_llw_log <= 64) { char st_u8[136]; char st_line[320]; const char* st_n = "(null)"; if (path) { if (trt_utf16_to_utf8(path, st_u8, sizeof(st_u8)) == 0) st_n = st_u8; else st_n = "(unconvertible)"; } if (h) snprintf(st_line, sizeof(st_line), "modtrace: LoadLibraryW -> \"%s\" handle=%p", st_n, h); else snprintf(st_line, sizeof(st_line), "modtrace: LoadLibraryW -> \"%s\" handle=NULL", st_n); trt_core_log(st_line); }
  return h;
}

void* TRT_STDCALL trt_LoadLibraryExW(const u16* path, void* file, u32 flags) {
  gap_trace_record("LoadLibraryExW");
  (void)file;
  void* h = load_library_impl(path, flags);
  { char st_u8[136]; char st_line[320]; const char* st_n = "(null)"; if (path) { if (trt_utf16_to_utf8(path, st_u8, sizeof(st_u8)) == 0) st_n = st_u8; else st_n = "(unconvertible)"; } if (h) snprintf(st_line, sizeof(st_line), "modtrace: LoadLibraryExW -> \"%s\" flags=0x%x handle=%p", st_n, flags, h); else snprintf(st_line, sizeof(st_line), "modtrace: LoadLibraryExW -> \"%s\" flags=0x%x handle=NULL", st_n, flags); trt_core_log(st_line); }
  return h;
}

int TRT_STDCALL trt_FreeLibrary(void* handle) {
  (void)handle;
  return 1;  // images live for the process lifetime
}

int TRT_STDCALL trt_DisableThreadLibraryCalls(void* handle) {
  if (!handle) {
    err_ref_for_current_thread() = 6;  // ERROR_INVALID_HANDLE
    return 0;
  }
  if (!trt_images_disable_thread_calls(handle)) {
    err_ref_for_current_thread() = 6;  // ERROR_INVALID_HANDLE
    return 0;
  }
  return 1;
}

void* TRT_STDCALL trt_GetProcAddress(void* handle, const char* name) {
  gap_trace_record("GetProcAddress");
  const uintptr_t name_value = reinterpret_cast<uintptr_t>(name);
  if (!name) {
    err_ref_for_current_thread() = 127;  // ERROR_PROC_NOT_FOUND
    return nullptr;
  }
  const bool by_ordinal = name_value < 0x10000;
  const u16 ordinal = by_ordinal ? static_cast<u16>(name_value) : 0;
  // .213 DIAG: log when the game resolves an exit/terminate entry point
  // (dynamic exit path -- a static CRT or PEB-walked exit bypasses this,
  // but a GetProcAddress-resolved one is visible here).
  if (!by_ordinal) {
    static const char* const k_exit_names[] = {
        "ExitProcess", "RtlExitUserProcess", "TerminateProcess", "FatalExit",
        "_exit", "_Exit", "abort", "quick_exit"};
    for (size_t i = 0; i < sizeof(k_exit_names) / sizeof(k_exit_names[0]); ++i) {
      if (strcmp(name, k_exit_names[i]) == 0) {
        char line[256];
        snprintf(line, sizeof(line),
                 "kernel32: GetProcAddress exit-name %s (handle=%p)", name, handle);
        trt_core_log(line);
        break;
      }
    }
  }
  // STAGE 7 (OrTMM): a synthetic system-module handle holds no PE image —
  // resolve through the module table, the SAME surface the static import
  // binder used (kernel32!RtlCaptureContext etc. live there, not in a file).
  if (handle && trt_images_is_synthetic(handle)) {
    const char* mod = trt_images_synth_name(handle);
    if (!mod) {
      err_ref_for_current_thread() = 6;  // ERROR_INVALID_HANDLE
      return nullptr;
    }
    if (void* fn = trt_resolve_export(mod, by_ordinal ? nullptr : name, ordinal)) return fn;
    char ordinal_name[16];
    if (by_ordinal) snprintf(ordinal_name, sizeof(ordinal_name), "#%u", unsigned(ordinal));
    char line[256];
    snprintf(line, sizeof(line), "kernel32: GetProcAddress(%s!%s) -> NULL",
             mod, by_ordinal ? ordinal_name : name);
    trt_core_log(line);
    err_ref_for_current_thread() = 127;  // ERROR_PROC_NOT_FOUND
    return nullptr;
  }
  trt_image* img = handle ? trt_images_by_handle(handle) : nullptr;
  if (!img) {
    // NULL/unknown handle: invalid handle
    err_ref_for_current_thread() = 6;  // ERROR_INVALID_HANDLE
    return nullptr;
  }
  // Resolve forwarded exports with the same lookup rules as direct probes.
  void* fn = trt_pe_find_export(trt_runtime_backend(), trt_runtime_export_resolver(),
                               img, by_ordinal ? nullptr : name, ordinal);
  if (!fn) err_ref_for_current_thread() = 127;  // ERROR_PROC_NOT_FOUND
  return fn;
}

void* TRT_STDCALL trt_GetModuleHandleW(const u16* name) {
  gap_trace_record("GetModuleHandleW");
  void* h = nullptr;
  const char* st_base = "(null)";
  char st_u8[256];
  if (!name || !name[0]) {
    trt_image* first = trt_images_first();
    h = first ? trt_images_handle(first) : nullptr;
  } else if (trt_utf16_to_utf8(name, st_u8, sizeof(st_u8)) != 0) {
    h = nullptr;
  } else {
    st_base = path_basename(st_u8);
    trt_image* img = trt_images_find(st_base);
    if (img) {
      h = trt_images_handle(img);
    } else {
      // STAGE 7 (OrTMM): the CRT boot asks for "kernel32.dll" (and apiset
      // contract names) by handle before any file exists for them. A known
      // SYSTEM/BUILTIN module name gets the synthetic handle — a NULL here
      // failfasts the CRT before WinMain.
      h = system_module_handle(st_base);
    }
  }
  { char st_line[320]; if (h) snprintf(st_line, sizeof(st_line), "modtrace: GetModuleHandleW -> \"%s\" handle=%p", st_base, h); else snprintf(st_line, sizeof(st_line), "modtrace: GetModuleHandleW -> \"%s\" handle=NULL", st_base); trt_core_log(st_line); }
  return h;
}

// ==================== console + child-process honesty =======================
// OrTMM (and every MSVC-runtime game) imports the console surface. The
// console has NO console for a UWP AppContainer process: everything here
// must say so HONESTLY (NULL/FALSE + a named last error), never a fake
// success the CRT would then trust.
//
// CreateProcessW: the console blocks child processes for game PEs
// (documented in docs/02-bridge-surface.md) — fail named with
// ERROR_ACCESS_DENIED. EXCEPTION: the offline EXE-boot harness installs
// trt_boot_spawn_hook and models the OS — a PyInstaller onefile parent
// MUST re-spawn its own exe as a child process (pyi_utils_create_child);
// the hook forks a real child that re-boots the inherited image.
void (*trt_boot_spawn_hook)(const char* exe_path, const char* cmdline,
                            int* pid_out) = nullptr;
void (*trt_boot_chainload_hook)(const char* exe_path, const char* cmdline,
                                u32* exit_code_out, int* ok_out) = nullptr;

// the spawned child's outcome, shared with GetExitCodeProcess
int trt_child_pid = -1;
int trt_child_running = 0;   // 1 while the child is alive
u32 trt_child_exit_code = 0;

namespace {

void console_log(const char* s) { trt_core_log(s); }

// Raw guest stdout/stderr bytes -> the log ring, split on newlines so each
// guest print becomes one ring line (guest prints are not NUL-terminated).
// Definition sits with the console surface below; WriteFile routes the
// Win32 stdio pseudo-handles here.
void console_log_bytes(const char* buf, u32 bytes) {
  static _Thread_local char t_pending[512];
  static _Thread_local u32 t_pending_n = 0;
  u32 i = 0;
  while (i < bytes) {
    const char ch = buf[i++];
    if (ch == '\n') {
      t_pending[t_pending_n] = '\0';
      if (t_pending_n) console_log(t_pending);
      t_pending_n = 0;
      continue;
    }
    if (t_pending_n + 1 < sizeof(t_pending)) {
      t_pending[t_pending_n++] = ch;
    } else {
      t_pending[t_pending_n] = '\0';
      console_log(t_pending);
      t_pending_n = 0;
      t_pending[t_pending_n++] = ch;
    }
  }
}

// Raw guest stdout/stderr bytes -> the log ring, split on newlines so each
// guest print becomes one ring line (guest prints are not NUL-terminated).
void console_log_bytes(const char* buf, u32 bytes);

void console_no_console(const char* api, u32 err) {
  char line[128];
  snprintf(line, sizeof(line),
           "kernel32: %s -> no console (AppContainer); lasterr=%u", api, err);
  console_log(line);
  set_err_for_current_thread(err);
}

void apply_child_env_block(void* env, u32 flags) {
  if (!env) return;
  extern void trt_gap_env_set(const char* name, const char* value);
  if (flags & 0x0400u /*CREATE_UNICODE_ENVIRONMENT*/) {
    const u16* w = (const u16*)env;
    while (*w) {
      size_t len = 0;
      while (w[len]) ++len;
      std::string kv(len * 3 + 4, '\0');
      if (trt_utf16_to_utf8(w, &kv[0], kv.size()) == 0) {
        kv.resize(strlen(kv.c_str()));
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) {
          trt_gap_env_set(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
        }
      }
      w += len + 1;
    }
  } else {
    const char* p = (const char*)env;
    while (*p) {
      const char* eq = strchr(p, '=');
      if (eq && eq != p) {
        std::string key(p, (size_t)(eq - p));
        trt_gap_env_set(key.c_str(), eq + 1);
      }
      p += strlen(p) + 1;
    }
  }
}

}  // close extern "C" briefly for C++ helpers returning STL types
namespace {

std::string lower_basename(const std::string& path) {
  size_t s = path.find_last_of("/\\");
  std::string base = (s == std::string::npos) ? path : path.substr(s + 1);
  for (char& c : base) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return base;
}

std::string extract_exe_target(const u16* app, const u16* cmdline) {
  auto utf8_of = [](const u16* s) -> std::string {
    if (!s) return {};
    size_t n = 0;
    while (s[n]) ++n;
    std::string out(n * 3 + 4, '\0');
    if (trt_utf16_to_utf8(s, &out[0], out.size()) != 0) return {};
    out.resize(strlen(out.c_str()));
    return out;
  };
  if (app && app[0]) return utf8_of(app);
  std::string cmd = utf8_of(cmdline);
  size_t pos = 0;
  while (pos < cmd.size() && (cmd[pos] == ' ' || cmd[pos] == '\t')) ++pos;
  if (pos >= cmd.size()) return {};
  if (cmd[pos] == '"') {
    const size_t end = cmd.find('"', pos + 1);
    return (end == std::string::npos) ? cmd.substr(pos + 1)
                                      : cmd.substr(pos + 1, end - (pos + 1));
  }
  const size_t end = cmd.find_first_of(" \t", pos);
  return (end == std::string::npos) ? cmd.substr(pos)
                                    : cmd.substr(pos, end - pos);
}

}  // namespace
extern "C" {

bool is_crash_helper_exe(const std::string& base) {
  return base == "unitycrashhandler64.exe" ||
         base == "unitycrashhandler32.exe" ||
         base == "unitycrashhandler.exe" ||
         base == "crashpad_handler.exe" ||
         base == "crashreportclient.exe" ||
         base == "bssndrpt.exe" ||
         base == "bssndrpt64.exe";
}

} // namespace

void* TRT_GUEST_ABI trt_GetStdHandle(u32 handle_id) {
  // Synthetic stdio: real Win32 pseudo-handles (STD_INPUT=-10, OUTPUT=-11,
  // ERROR=-12) routed by WriteFile/ReadFile to the runtime log ring. On the
  // console there is no AppContainer console (measured: a TRUE AttachConsole
  // made Godot flood GetStdHandle forever), but a game's own stdout is a
  // discovery surface — Godot's error log names its internal state. Handled
  // in trt_WriteFile; anything else (GetConsoleMode etc.) still reports
  // no-console honestly.
  if (handle_id == 0xFFFFFFF6u) return (void*)(i64)-10;
  if (handle_id == 0xFFFFFFF5u) return (void*)(i64)-11;
  if (handle_id == 0xFFFFFFF4u) return (void*)(i64)-12;
  console_no_console("GetStdHandle", 6 /*ERROR_INVALID_HANDLE*/);
  return nullptr;
}

int TRT_GUEST_ABI trt_SetStdHandle(u32 handle_id, void* handle) {
  (void)handle_id; (void)handle;
  console_no_console("SetStdHandle", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_GetConsoleMode(void* console, u32* mode_out) {
  (void)console; (void)mode_out;
  console_no_console("GetConsoleMode", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_ReadConsoleW(void* console, void* buffer, u32 to_read,
                                  u32* read_out, void* ctrl) {
  (void)console; (void)buffer; (void)to_read; (void)read_out; (void)ctrl;
  console_no_console("ReadConsoleW", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_WriteConsoleW(void* console, const void* buffer, u32 to_write,
                                   u32* written_out, void* reserved) {
  (void)console; (void)buffer; (void)to_write; (void)written_out; (void)reserved;
  console_no_console("WriteConsoleW", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_SetConsoleCtrlHandler(void* handler, int add) {
  (void)handler; (void)add;
  console_no_console("SetConsoleCtrlHandler", 120 /*ERROR_CALL_NOT_IMPLEMENTED*/);
  return 0;  // FALSE: there is no console to attach a handler to
}

int TRT_GUEST_ABI trt_CreateProcessW(const u16* app, u16* cmdline, void* proc_attr,
                                    void* thread_attr, int inherit,
                                    u32 flags, void* env, const u16* dir,
                                    const void* startup, void* info_out) {
  (void)proc_attr; (void)thread_attr; (void)inherit; (void)dir; (void)startup;
  char utf8[512];
  if (cmdline)
    trt_utf16_to_utf8(cmdline, utf8, sizeof(utf8));
  else if (app)
    trt_utf16_to_utf8(app, utf8, sizeof(utf8));
  else
    snprintf(utf8, sizeof(utf8), "(null)");

  apply_child_env_block(env, flags);

  const std::string target = extract_exe_target(app, cmdline);
  const std::string base = lower_basename(target);
  static std::atomic<u32> g_synth_pid{4100};

  // Out-of-process crash reporters (UnityCrashHandler64 / crashpad_handler):
  // report clean signaled handle so games proceed without spawning children.
  if (is_crash_helper_exe(base)) {
    extern u64 trt_process_object_alloc(void);
    extern void trt_process_object_signal(u64 h);
    const u64 hproc = trt_process_object_alloc();
    if (hproc) trt_process_object_signal(hproc);
    const u32 pid = g_synth_pid.fetch_add(1);
    trt_child_pid = (int)pid;
    trt_child_running = 0;
    trt_child_exit_code = 0;
    if (info_out) {
      u64* pi = (u64*)info_out;
      pi[0] = hproc;
      pi[1] = 0;
      u32* p32 = (u32*)(pi + 2);
      p32[0] = pid;
      p32[1] = 0;
    }
    char line[320];
    snprintf(line, sizeof(line),
             "kernel32: CreateProcessW('%s') -> diagnostic helper skipped "
             "(signaled pid=%u)", utf8, pid);
    console_log(line);
    return 1;
  }

  // harness-modeled spawn: the hook forks a real child; while it runs the
  // guest is blocked HERE (PyInstaller's parent waits inline), so the
  // returned handle is born already-signalled
  if (trt_boot_spawn_hook) {
    char app8[512] = {};
    if (app) trt_utf16_to_utf8(app, app8, sizeof(app8));
    int pid = -1;
    trt_boot_spawn_hook(app8[0] ? app8 : utf8, utf8, &pid);
    if (pid >= 0) {
      trt_child_pid = pid;
      trt_child_running = 1;
      trt_child_exit_code = 0;  // STILL_ACTIVE until GetExitCodeProcess
      extern u64 trt_process_object_alloc(void);
      extern void trt_process_object_signal(u64 h);
      const u64 hproc = trt_process_object_alloc();
      if (hproc) trt_process_object_signal(hproc);
      if (info_out) {
        u64* pi = (u64*)info_out;
        pi[0] = hproc;          // hProcess
        pi[1] = 0;              // hThread
        u32* p32 = (u32*)(pi + 2);
        p32[0] = (u32)pid;      // dwProcessId
        p32[1] = 0;             // dwThreadId
      }
      char line[320];
      snprintf(line, sizeof(line),
               "kernel32: CreateProcessW('%s') -> pid=%d (harness-modeled "
               "child; console would deny)", utf8, pid);
      console_log(line);
      return 1;  // TRUE
    }
    trt_core_log("kernel32: CreateProcessW spawn hook returned -1 (fork "
                 "failed) -> ERROR_ACCESS_DENIED");
    if (info_out) memset(info_out, 0, 24);
    set_err_for_current_thread(5);
    return 0;
  }

  // Optional in-process host chainload hook for resolved VFS child PEs.
  if (trt_boot_chainload_hook && !target.empty()) {
    std::vector<u16> wtarget(target.size() + 1);
    for (size_t i = 0; i < target.size(); ++i) wtarget[i] = (u16)(unsigned char)target[i];
    wtarget[target.size()] = 0;
    char host_path[1024] = {};
    if (trt_vfs_resolve_utf16(wtarget.data(), host_path, sizeof(host_path)) == 0) {
      int is_dir = 0;
      u64 fsz = 0;
      if (trt_vfs_stat(host_path, &is_dir, &fsz) == 0 && !is_dir && fsz > 64) {
        u32 child_rc = 0;
        int ok = 0;
        trt_boot_chainload_hook(host_path, utf8, &child_rc, &ok);
        if (ok) {
          extern u64 trt_process_object_alloc(void);
          extern void trt_process_object_signal(u64 h);
          const u64 hproc = trt_process_object_alloc();
          if (hproc) trt_process_object_signal(hproc);
          const u32 pid = g_synth_pid.fetch_add(1);
          trt_child_pid = (int)pid;
          trt_child_running = 0;
          trt_child_exit_code = child_rc;
          if (info_out) {
            u64* pi = (u64*)info_out;
            pi[0] = hproc;
            pi[1] = 0;
            u32* p32 = (u32*)(pi + 2);
            p32[0] = pid;
            p32[1] = 0;
          }
          return 1;
        }
      }
    }
  }

  if (info_out) memset(info_out, 0, 24);
  char line[320];
  snprintf(line, sizeof(line),
           "kernel32: CreateProcessW('%s') -> ERROR_ACCESS_DENIED "
           "(console blocks child processes; documented)", utf8);
  console_log(line);
  char diag[384];
  snprintf(diag, sizeof(diag),
           "kernel32: CreateProcessW deny diag: hook=%s exepath=%s",
           trt_boot_spawn_hook ? "SET" : "NULL", g_exepath);
  console_log(diag);
  set_err_for_current_thread(5); // ACCESS_DENIED
  return 0;  // FALSE
}

// ==================== STAGE 7: EXE boot / MSVC CRT surface ==================
// A game EXE is not a DLL: before WinMain its CRT startup runs and calls a
// kernel32 surface the DLL corpus never touched (process/heap/FLS/SEH).
// Every answer here is honest-minimal and NAMED, never silent.

// process-exit trap hooks: NULL on the console (real exit), the offline EXE
// boot harness sets them to longjmp back into the harness. C-linkage globals
// (declared in trt.h) so the harness can install them.
void (*trt_boot_exit_trap)(int code) = nullptr;
void (*trt_boot_raise_trap)(u32 code) = nullptr;

namespace {

// STAGE 7 named depth marker: the boot harness greps the ring for these.
void boot_log(const char* s) { trt_core_log(s); }

// GetStartupInfoW must FILL the STARTUPINFOW (the CRT reads dwY/dwXCountChars
// etc. to decide console dimensions; a zeroed struct is a valid "no
// console" answer, cb must be sizeof(STARTUPINFOW) = 104 on x64).
void fill_startup_info_w(void* si) {
  if (!si) return;
  memset(si, 0, 104);
  ((u32*)si)[0] = 104;  // cb
  // Real Windows fills lpDesktop ("WinSta0\Default") and lpTitle (full exe path
  // GoW faults passing a NULL string to a CRT string function right
  // after Steam init; a NULL lpTitle/lpDesktop is the live candidate.
  // Static storage: the struct holds pointers, so stack buffers would dangle.
  // u16 arrays (not L"..." literals): wchar_t is 32-bit on the Linux gate.
  static u16 desktop_buf[] = {'W', 'i', 'n', 'S', 't', 'a', '0', '\\',
                              'D', 'e', 'f', 'a', 'u', 'l', 't', 0};
  static u16 title_buf[260];
  // Full path, not basename: GoW strrchrs lpTitle for a backslash;
  // a bare basename returns NULL and its pointer math faults at -1.
  size_t ti = 0;
  for (; g_exepath[ti] && ti < 258; ++ti) title_buf[ti] = (u16)(u8)g_exepath[ti];
  title_buf[ti] = 0;
  ((u16**)si)[2] = desktop_buf;  // lpDesktop @16
  ((u16**)si)[3] = title_buf;    // lpTitle @24
  { char st_dt[64]; char st_tt[136]; char st_line[320]; size_t st_i = 0; for (; desktop_buf[st_i] && st_i < 60; ++st_i) st_dt[st_i] = (char)(desktop_buf[st_i] & 0x7f); st_dt[st_i] = 0; st_i = 0; for (; title_buf[st_i] && st_i < 132; ++st_i) st_tt[st_i] = (char)(title_buf[st_i] & 0x7f); st_tt[st_i] = 0; snprintf(st_line, sizeof(st_line), "strtrace: GetStartupInfoW -> lpDesktop=%p \"%s\" lpTitle=%p \"%.128s\"", (void*)desktop_buf, st_dt, (void*)title_buf, st_tt); trt_core_log(st_line); }
  boot_log("kernel32: GetStartupInfoW -> zeroed STARTUPINFOW (no console)");
  boot_log("kernel32: GetStartupInfoW lpDesktop/lpTitle filled");
}

void fill_startup_info_a(void* si) {
  if (!si) return;
  memset(si, 0, 104);
  ((u32*)si)[0] = 104;  // cb (STARTUPINFOA is 104 bytes on x64, like W)
  // ANSI strings: the old code reused the W fill, leaving UTF-16 pointers
  // in an ANSI struct (an ANSI reader sees "W\0i\0n\0..." = "W").
  // Static storage: the struct holds pointers, so stack buffers would dangle.
  static char desktop_buf_a[] = "WinSta0\\Default";
  static char title_buf_a[260];
  // Full path, not basename: GoW strrchrs lpTitle for a backslash;
  // a bare basename returns NULL and its pointer math faults at -1.
  size_t ti = 0;
  for (; g_exepath[ti] && ti < 258; ++ti) title_buf_a[ti] = g_exepath[ti];
  title_buf_a[ti] = 0;
  ((char**)si)[2] = desktop_buf_a;  // lpDesktop @16
  ((char**)si)[3] = title_buf_a;    // lpTitle @24
  { char st_line[320]; snprintf(st_line, sizeof(st_line), "strtrace: GetStartupInfoA -> lpDesktop=%p \"%s\" lpTitle=%p \"%.128s\"", (void*)desktop_buf_a, desktop_buf_a, (void*)title_buf_a, title_buf_a); trt_core_log(st_line); }
  boot_log("kernel32: GetStartupInfoA -> zeroed STARTUPINFOA (no console)");
}

// FindFirstFileExW / FindNextFileW / FindClose: REAL directory enumeration
// over the VFS. This was an INVALID_HANDLE-stub — the measured blocker where
// PyInstaller-packed games (OrTMM) and every engine scanning its own asset /
// mod / save directories died. The guest passes a Win32 path with an optional
// wildcard; we resolve through the VFS, enumerate the host directory, and
// hand back WIN32_FIND_DATAW records the slot registry tracks.
//
// Both platforms are REAL:
//   Linux (offline gate)  — POSIX readdir over the resolved host dir
//   Windows (the console) — passthrough to the native FindFirstFileExW family
//                           on the VFS-resolved host path
struct find_ctx {
  void* os_dir;
  void* os_dir2;
  char pattern[260];
  bool have_pattern;
  char host_path[1024];
  char host_path2[1024];
  char z_rel[512];
  bool is_z;
  bool opaque_ro;
  int phase;
  std::vector<std::string> seen;
  // disc-image enumeration: the ISO records are the source of truth, so a
  // FindFirstFile over D:\ lists the disc's real names and sizes.
  bool is_iso;
  size_t iso_pos;
  std::vector<std::string> iso_names;
  std::vector<u64> iso_sizes;
  std::vector<u8> iso_dirs;
};
find_ctx g_find_ctx[32];
bool g_find_used[32];

char find_lower_c(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

std::string find_lower_str(const char* s) {
  std::string out;
  if (!s) return out;
  for (; *s; ++s) out.push_back(find_lower_c(*s));
  return out;
}

bool find_seen_has(const find_ctx& c, const char* name) {
  std::string low = find_lower_str(name);
  for (size_t i = 0; i < c.seen.size(); ++i)
    if (c.seen[i] == low) return true;
  return false;
}

void find_seen_push(find_ctx& c, const char* name) {
  std::string low = find_lower_str(name);
  for (size_t i = 0; i < c.seen.size(); ++i)
    if (c.seen[i] == low) return;
  c.seen.push_back(low);
}

void find_free(int slot) {
  if (slot < 0 || slot >= 32) return;
#if defined(_WIN32)
  if (g_find_ctx[slot].os_dir) {
    FindClose(g_find_ctx[slot].os_dir);
    g_find_ctx[slot].os_dir = nullptr;
  }
  if (g_find_ctx[slot].os_dir2) {
    FindClose(g_find_ctx[slot].os_dir2);
    g_find_ctx[slot].os_dir2 = nullptr;
  }
#else
  if (g_find_ctx[slot].os_dir) {
    closedir((DIR*)g_find_ctx[slot].os_dir);
    g_find_ctx[slot].os_dir = nullptr;
  }
  if (g_find_ctx[slot].os_dir2) {
    closedir((DIR*)g_find_ctx[slot].os_dir2);
    g_find_ctx[slot].os_dir2 = nullptr;
  }
#endif
  g_find_ctx[slot].seen.clear();
  g_find_ctx[slot].is_z = false;
  g_find_ctx[slot].opaque_ro = false;
  g_find_ctx[slot].phase = 0;
  g_find_ctx[slot].is_iso = false;
  g_find_ctx[slot].iso_pos = 0;
  g_find_ctx[slot].iso_names.clear();
  g_find_ctx[slot].iso_sizes.clear();
  g_find_ctx[slot].iso_dirs.clear();
  g_find_used[slot] = false;
}

// wildcard match: '*' and '?' the Win32 way (case-insensitive, "*.*" == "*")
bool find_match_ci(const char* pat, const char* s) {
  if (!*pat) return !*s;
  if (*pat == '*') {
    for (const char* q = s; ; ++q) {
      if (find_match_ci(pat + 1, q)) return true;
      if (!*q) break;
    }
    return false;
  }
  if (!*s) return false;
  if (*pat == '?' || find_lower_c(*pat) == find_lower_c(*s))
    return find_match_ci(pat + 1, s + 1);
  return false;
}

bool find_match(const char* pat, const char* s) {
  if (strcmp(pat, "*.*") == 0) return true;
  return find_match_ci(pat, s);
}

// fill a WIN32_FIND_DATAW (x64, real Win32 layout, 592 bytes):
// attrs +0, FILETIMEs +4/+12/+20, size hi +28 / lo +32, reserved +36/+40,
// cFileName +44 (u16[260]), cAlternateFileName +564 (u16[14])
void find_fill_data(void* data, const char* name, bool is_dir, off_t size,
                    i64 ctime_s = 0, i64 atime_s = 0, i64 mtime_s = 0) {
  u8* p = (u8*)data;
  memset(p, 0, 592);
  *(u32*)(p + 0) = is_dir ? 0x10 /*FILE_ATTRIBUTE_DIRECTORY*/ : 0x80 /*NORMAL*/;
  auto write_ft = [&](size_t off, i64 sec) {
    if (sec <= 0) return;
    const u64 ft = (u64)sec * 10000000ULL + 116444736000000000ULL;
    *(u32*)(p + off) = (u32)(ft & 0xFFFFFFFFu);
    *(u32*)(p + off + 4) = (u32)(ft >> 32);
  };
  write_ft(4, ctime_s);
  write_ft(12, atime_s);
  write_ft(20, mtime_s);
  const u64 usz = size > 0 ? (u64)size : 0ULL;
  *(u32*)(p + 28) = (u32)(usz >> 32);
  *(u32*)(p + 32) = (u32)(usz & 0xFFFFFFFFu);
  u16* wname = (u16*)(p + 44);
  u32 i = 0;
  for (; name[i] && i < 259; ++i) wname[i] = (u16)(u8)name[i];
  wname[i] = 0;
}

// "." and ".." are excluded the Win32 way (FindFirstFile never returns them)
bool find_skip_dot(const char* n) {
  return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

// Disc-image enumeration: one record per ISO directory entry, filtered by the
// guest's wildcard. Works on both platforms because the source is the image.
bool find_iso_next(int slot, void* info) {
  find_ctx& c = g_find_ctx[slot];
  while (c.iso_pos < c.iso_names.size()) {
    const size_t i = c.iso_pos++;
    const char* nm = c.iso_names[i].c_str();
    if (c.have_pattern && !find_match(c.pattern, nm)) continue;
    find_fill_data(info, nm, c.iso_dirs[i] != 0, (off_t)c.iso_sizes[i], 0, 0, 0);
    return true;
  }
  return false;
}

#if defined(_WIN32)
void find_win_build_pat(const char* dir, const char* pat, u16* out, size_t cap) {
  size_t wn = 0;
  for (size_t i = 0; dir && dir[i] && wn + 4 < cap; ++i)
    out[wn++] = (u16)(u8)dir[i];
  if (wn && out[wn - 1] != '\\' && out[wn - 1] != '/') out[wn++] = '\\';
  for (size_t i = 0; pat && pat[i] && wn + 1 < cap; ++i)
    out[wn++] = (u16)(u8)pat[i];
  out[wn] = 0;
}

void find_win_name_utf8(const WIN32_FIND_DATAW& fd, char* out, size_t cap) {
  size_t i = 0;
  for (; fd.cFileName[i] && i + 1 < cap; ++i) out[i] = (char)fd.cFileName[i];
  out[i] = 0;
}

bool find_win_ro_accept(find_ctx& c, const WIN32_FIND_DATAW& fd) {
  char nm[260];
  find_win_name_utf8(fd, nm, sizeof(nm));
  if (find_seen_has(c, nm)) return false;
  if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
    return true;
  if (!c.is_z) return true;
  char child[1024];
  if (c.z_rel[0]) snprintf(child, sizeof(child), "%s/%s", c.z_rel, nm);
  else snprintf(child, sizeof(child), "%s", nm);
  return trt_vfs_is_z_deleted_rel(child, 1) == 0;
}
#endif

#if !defined(_WIN32)
bool find_advance_posix(int slot, void* info, char* matched_name,
                        size_t matched_cap) {
  find_ctx& c = g_find_ctx[slot];
  for (;;) {
    if (c.phase == 0) {
      if (!c.os_dir) { c.phase = 1; continue; }
      errno = 0;
      dirent* e = readdir((DIR*)c.os_dir);
      if (!e) {
        closedir((DIR*)c.os_dir);
        c.os_dir = nullptr;
        c.phase = 1;
        continue;
      }
      if (find_skip_dot(e->d_name)) continue;
      if (!find_match(c.pattern, e->d_name)) continue;
      if (c.is_z) find_seen_push(c, e->d_name);
      struct stat st;
      char fullp[1100];
      snprintf(fullp, sizeof(fullp), "%s/%s", c.host_path, e->d_name);
      off_t sz = 0;
      bool isdir = (e->d_type == DT_DIR);
      i64 ct = 0, at = 0, mt = 0;
      if (stat(fullp, &st) == 0) {
        sz = st.st_size;
        isdir = S_ISDIR(st.st_mode);
        ct = (i64)st.st_ctime;
        at = (i64)st.st_atime;
        mt = (i64)st.st_mtime;
      }
      find_fill_data(info, e->d_name, isdir, sz, ct, at, mt);
      if (matched_name && matched_cap)
        snprintf(matched_name, matched_cap, "%s", e->d_name);
      return true;
    } else {
      if (!c.os_dir2) return false;
      errno = 0;
      dirent* e = readdir((DIR*)c.os_dir2);
      if (!e) {
        closedir((DIR*)c.os_dir2);
        c.os_dir2 = nullptr;
        return false;
      }
      if (find_skip_dot(e->d_name)) continue;
      if (!find_match(c.pattern, e->d_name)) continue;
      if (c.is_z) {
        if (find_seen_has(c, e->d_name)) continue;
        char child[1024];
        if (c.z_rel[0]) snprintf(child, sizeof(child), "%s/%s", c.z_rel, e->d_name);
        else snprintf(child, sizeof(child), "%s", e->d_name);
        if (trt_vfs_is_z_deleted_rel(child, 1)) continue;
      }
      struct stat st;
      char fullp[1100];
      snprintf(fullp, sizeof(fullp), "%s/%s", c.host_path2, e->d_name);
      off_t sz = 0;
      bool isdir = (e->d_type == DT_DIR);
      i64 ct = 0, at = 0, mt = 0;
      if (stat(fullp, &st) == 0) {
        sz = st.st_size;
        isdir = S_ISDIR(st.st_mode);
        ct = (i64)st.st_ctime;
        at = (i64)st.st_atime;
        mt = (i64)st.st_mtime;
      }
      find_fill_data(info, e->d_name, isdir, sz, ct, at, mt);
      if (matched_name && matched_cap)
        snprintf(matched_name, matched_cap, "%s", e->d_name);
      return true;
    }
  }
}
#endif  // !_WIN32

void* find_first_file(const u16* name, void* found) {
  char utf8[512];
  if (name) trt_utf16_to_utf8(name, utf8, sizeof(utf8));
  else snprintf(utf8, sizeof(utf8), "(null)");

  // split "dir\pattern" (last separator); handle "Z:pat" and bare "pat" in cwd
  char dir[512] = ".", pat[260] = {};
  char* last_sep = nullptr;
  for (char* q = utf8; *q; ++q)
    if (*q == '\\' || *q == '/') last_sep = q;
  if (last_sep) {
    const size_t dl = (size_t)(last_sep - utf8);
    if (dl == 0) snprintf(dir, sizeof(dir), "/");
    else if (dl == 2 && utf8[1] == ':') snprintf(dir, sizeof(dir), "%c:\\", utf8[0]);
    else { snprintf(dir, sizeof(dir), "%.*s", (int)dl, utf8); }
    snprintf(pat, sizeof(pat), "%s", last_sep + 1);
  } else if (find_lower_c(utf8[0]) >= 'a' && find_lower_c(utf8[0]) <= 'z' &&
             utf8[1] == ':') {
    snprintf(dir, sizeof(dir), "%c:", utf8[0]);
    snprintf(pat, sizeof(pat), "%s", utf8 + 2);
  } else {
    snprintf(pat, sizeof(pat), "%s", utf8);
  }

  // pattern "" or missing find-data falls through to the honest no-match
  if (!pat[0] || !found) {
    char line[640];
    snprintf(line, sizeof(line),
             "kernel32: FindFirstFileExW('%s') -> INVALID_HANDLE_VALUE "
             "(no pattern/find-data; lasterr=2)", utf8);
    boot_log(line);
    err_ref_for_current_thread() = 2;  // FILE_NOT_FOUND
    return (void*)(uintptr_t)-1;
  }

  u16 wdir[512];
  u32 wi = 0;
  for (; dir[wi] && wi < 511; ++wi) wdir[wi] = (u16)(u8)dir[wi];
  wdir[wi] = 0;

  // A mounted disc image answers its own directory listing: real names, real
  // sizes, no extraction. (D:\... -> the ISO's records.)
  {
    char iso_rel[512] = "";
    trt_iso* iso = trt_vfs_image_for_guest_utf16(wdir, iso_rel, sizeof(iso_rel));
    if (iso) {
      int is_dir = 0;
      if (trt_iso_stat(iso, iso_rel, &is_dir, nullptr) != 0 || !is_dir) {
        char line[640];
        snprintf(line, sizeof(line),
                 "kernel32: FindFirstFileExW('%s') -> INVALID_HANDLE_VALUE "
                 "(disc image has no such directory '%s'; lasterr=3)", utf8,
                 iso_rel);
        boot_log(line);
        err_ref_for_current_thread() = 3;
        return (void*)(uintptr_t)-1;
      }
      int slot = -1;
      for (int i = 0; i < 32; ++i)
        if (!g_find_used[i]) { slot = i; break; }
      if (slot < 0) {
        err_ref_for_current_thread() = 4;
        return (void*)(uintptr_t)-1;
      }
      find_ctx& c = g_find_ctx[slot];
      find_free(slot);
      c.is_iso = true;
      c.iso_pos = 0;
      snprintf(c.pattern, sizeof(c.pattern), "%s", pat);
      c.have_pattern = !(strcmp(pat, "*") == 0 || strcmp(pat, "*.*") == 0);
      c.iso_names.clear();
      c.iso_sizes.clear();
      c.iso_dirs.clear();
      trt_iso_enum(iso, iso_rel, [](void* user, const char* name, int is_dir,
                                    uint64_t size) {
        find_ctx* cc = (find_ctx*)user;
        cc->iso_names.push_back(name);
        cc->iso_sizes.push_back(size);
        cc->iso_dirs.push_back((u8)(is_dir ? 1 : 0));
      }, &c);
      g_find_used[slot] = true;
      if (!find_iso_next(slot, found)) {
        find_free(slot);
        char line[640];
        snprintf(line, sizeof(line),
                 "kernel32: FindFirstFileExW('%s') -> INVALID_HANDLE_VALUE "
                 "(no match in disc image dir '%s'; lasterr=2)", utf8, iso_rel);
        boot_log(line);
        err_ref_for_current_thread() = 2;
        return (void*)(uintptr_t)-1;
      }
      char line[700];
      snprintf(line, sizeof(line),
               "kernel32: FindFirstFileExW('%s') -> handle slot=%d (disc image "
               "%zu entries)", utf8, slot, c.iso_names.size());
      boot_log(line);
      return (void*)(uintptr_t)(0x88800000u | (u32)slot);
    }
  }

  char ov[1024] = "", ro[1024] = "", zrel[512] = "", host[1024] = "";
  const bool is_z =
      trt_vfs_resolve_z_pair_utf16(wdir, ov, sizeof(ov), ro, sizeof(ro),
                                   zrel, sizeof(zrel)) == 0;
  if (is_z) {
    if (zrel[0] && trt_vfs_is_z_deleted_rel(zrel, 0)) {
      err_ref_for_current_thread() = 3;
      return (void*)(uintptr_t)-1;
    }
  } else {
    if (trt_vfs_resolve_utf16(wdir, host, sizeof(host)) != 0)
      snprintf(host, sizeof(host), "%s", dir);
  }

  int slot = -1;
  for (int i = 0; i < 32; ++i)
    if (!g_find_used[i]) { slot = i; break; }
  if (slot < 0) {
    boot_log("kernel32: FindFirstFileExW -> handle table full (32)");
    err_ref_for_current_thread() = 4;
    return (void*)(uintptr_t)-1;
  }

  find_ctx& c = g_find_ctx[slot];
  c.seen.clear();
  c.os_dir = nullptr;
  c.os_dir2 = nullptr;
  c.is_z = is_z;
  c.opaque_ro = is_z && zrel[0] && trt_vfs_is_z_opaque_rel(zrel) != 0;
  snprintf(c.pattern, sizeof(c.pattern), "%s", pat);
  c.have_pattern = !strchr(pat, '*') && !strchr(pat, '?');
  snprintf(c.z_rel, sizeof(c.z_rel), "%s", zrel);

#if defined(_WIN32)
  if (!is_z) {
    u16 wfull[1100];
    find_win_build_pat(host, pat, wfull, 1100);
    WIN32_FIND_DATAW native_fd;
    HANDLE h = FindFirstFileExW((LPCWSTR)wfull, FindExInfoStandard, &native_fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
      const u32 le = GetLastError();
      err_ref_for_current_thread() = (le == ERROR_PATH_NOT_FOUND) ? 3 : 2;
      return (void*)(uintptr_t)-1;
    }
    c.os_dir = (void*)h;
    c.phase = 0;
    snprintf(c.host_path, sizeof(c.host_path), "%s", host);
    g_find_used[slot] = true;
    memcpy(found, &native_fd, sizeof(WIN32_FIND_DATAW));
    return (void*)(uintptr_t)(0x88800000u | (u32)slot);
  }

  snprintf(c.host_path, sizeof(c.host_path), "%s", ov);
  snprintf(c.host_path2, sizeof(c.host_path2), "%s", ro);
  WIN32_FIND_DATAW native_fd;
  if (ov[0]) {
    u16 wov[1100];
    find_win_build_pat(ov, pat, wov, 1100);
    HANDLE h_ov = FindFirstFileExW((LPCWSTR)wov, FindExInfoStandard, &native_fd,
                                   FindExSearchNameMatch, nullptr, 0);
    if (h_ov != INVALID_HANDLE_VALUE) {
      c.os_dir = (void*)h_ov;
      c.phase = 0;
      g_find_used[slot] = true;
      char fnm[260];
      find_win_name_utf8(native_fd, fnm, sizeof(fnm));
      find_seen_push(c, fnm);
      memcpy(found, &native_fd, sizeof(WIN32_FIND_DATAW));
      return (void*)(uintptr_t)(0x88800000u | (u32)slot);
    }
  }
  if (ro[0] && !c.opaque_ro) {
    u16 wro[1100];
    find_win_build_pat(ro, pat, wro, 1100);
    HANDLE h_ro = FindFirstFileExW((LPCWSTR)wro, FindExInfoStandard, &native_fd,
                                   FindExSearchNameMatch, nullptr, 0);
    if (h_ro != INVALID_HANDLE_VALUE) {
      bool ok = find_win_ro_accept(c, native_fd);
      while (!ok && FindNextFileW(h_ro, &native_fd))
        ok = find_win_ro_accept(c, native_fd);
      if (ok) {
        c.os_dir2 = (void*)h_ro;
        c.phase = 1;
        g_find_used[slot] = true;
        memcpy(found, &native_fd, sizeof(WIN32_FIND_DATAW));
        return (void*)(uintptr_t)(0x88800000u | (u32)slot);
      }
      FindClose(h_ro);
    }
  }
  err_ref_for_current_thread() = 2;
  return (void*)(uintptr_t)-1;
#else
  if (!is_z) {
    DIR* d = opendir(host);
    if (!d) {
      char line[640];
      snprintf(line, sizeof(line), "kernel32: FindFirstFileExW('%s') -> "
               "INVALID_HANDLE_VALUE (opendir '%s' failed; lasterr=3)", utf8, host);
      boot_log(line);
      err_ref_for_current_thread() = 3;
      return (void*)(uintptr_t)-1;
    }
    c.os_dir = d;
    c.phase = 0;
    snprintf(c.host_path, sizeof(c.host_path), "%s", host);
  } else {
    DIR* d_ov = ov[0] ? opendir(ov) : nullptr;
    DIR* d_ro = (ro[0] && !c.opaque_ro) ? opendir(ro) : nullptr;
    if (!d_ov && !d_ro) {
      char line[640];
      snprintf(line, sizeof(line), "kernel32: FindFirstFileExW('%s') -> "
               "INVALID_HANDLE_VALUE (opendir ov='%s' ro='%s' failed; lasterr=3)",
               utf8, ov, ro);
      boot_log(line);
      err_ref_for_current_thread() = 3;
      return (void*)(uintptr_t)-1;
    }
    c.os_dir = d_ov;
    c.os_dir2 = d_ro;
    c.phase = d_ov ? 0 : 1;
    snprintf(c.host_path, sizeof(c.host_path), "%s", ov);
    snprintf(c.host_path2, sizeof(c.host_path2), "%s", ro);
  }

  g_find_used[slot] = true;
  char matched[260] = {};
  if (!find_advance_posix(slot, found, matched, sizeof(matched))) {
    find_free(slot);
    char line[640];
    snprintf(line, sizeof(line), "kernel32: FindFirstFileExW('%s') -> "
             "INVALID_HANDLE_VALUE (no match in %s; lasterr=2)",
             utf8, is_z ? (ro[0] ? ro : ov) : host);
    boot_log(line);
    err_ref_for_current_thread() = 2;
    return (void*)(uintptr_t)-1;
  }
  char line[700];
  snprintf(line, sizeof(line), "kernel32: FindFirstFileExW('%s') -> handle "
           "slot=%d first='%s'", utf8, slot, matched);
  boot_log(line);
  return (void*)(uintptr_t)(0x88800000u | (u32)slot);
#endif
}

} // namespace

// ExitProcess: REAL process-termination semantics. On the console (host
// build) there is genuinely nothing after this call. In the offline boot
// harness the trap hook longjmps back to the harness so the run continues.
// NAMED either way — "BOOT EXITED" is the harness's report.
void TRT_STDCALL trt_ExitProcess(u32 code) {
  char line[128];
  snprintf(line, sizeof(line), "kernel32: ExitProcess(%u) called", code);
  boot_log(line);
  if (trt_boot_exit_trap) { trt_boot_exit_trap((int)code); }
  // console path: a UWP process really does end here
#ifdef _WIN32
  ::ExitProcess(code);
#else
  _exit((int)(code & 0xFF));
#endif
}

// .213 DIAG: FatalExit is a real kernel32 export the game could resolve
// (statically or via GetProcAddress). It was not bridged anywhere; treat it
// as ExitProcess so the exit is logged instead of silent.
void TRT_STDCALL trt_FatalExit(u32 code) {
  char line[128];
  snprintf(line, sizeof(line), "kernel32: FatalExit(%u) called -- treated as ExitProcess", code);
  boot_log(line);
  trt_ExitProcess(code);
}

int TRT_STDCALL trt_TerminateProcess(void* proc, u32 code) {
  (void)proc;
  char line[128];
  snprintf(line, sizeof(line), "kernel32: TerminateProcess(%u) called", code);
  boot_log(line);
  if (trt_boot_exit_trap) { trt_boot_exit_trap((int)code); }
#ifdef _WIN32
  ::TerminateProcess(::GetCurrentProcess(), code);
#else
  _exit((int)(code & 0xFF));
#endif
  return 1;
}

void* TRT_STDCALL trt_GetStartupInfoW(void* si) {
  fill_startup_info_w(si);
  return si;  // void WINAPI GetStartupInfoW: returns nothing (0 in rax)
}

void* TRT_STDCALL trt_GetStartupInfoA(void* si) {
  fill_startup_info_a(si);
  return si;
}

const char* TRT_STDCALL trt_GetCommandLineA(void) {
  // argv[0], quoted like Windows does. Built from g_exepath + optional
  // tiramisu_args.txt arguments.
  static char cmd_a[1600];
  if (g_cmd_args[0]) {
    snprintf(cmd_a, sizeof(cmd_a), "\"%s\" %s", g_exepath, g_cmd_args);
  } else {
    snprintf(cmd_a, sizeof(cmd_a), "\"%s\"", g_exepath);
  }
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetCommandLineA -> ptr=%p \"%.128s\"", (const void*)cmd_a, cmd_a); trt_core_log(st_line); }
  return cmd_a;
}

const u16* TRT_STDCALL trt_GetCommandLineW(void) {
  static u16 cmd[1600];
  u32 i = 0;
  cmd[i++] = '"';
  for (const char* p = g_exepath; *p && i < 1580; ++p) cmd[i++] = (u16)(u8)*p;
  cmd[i++] = '"';
  if (g_cmd_args[0] && i + 2 < 1590) {
    cmd[i++] = ' ';
    for (const char* p = g_cmd_args; *p && i < 1595; ++p) {
      cmd[i++] = (u16)(u8)*p;
    }
  }
  cmd[i] = 0;
  { char st_tmp[136]; char st_line[256]; size_t st_i = 0; for (; cmd[st_i] && st_i < 132; ++st_i) st_tmp[st_i] = (char)(cmd[st_i] & 0x7f); st_tmp[st_i] = 0; snprintf(st_line, sizeof(st_line), "strtrace: GetCommandLineW -> ptr=%p \"%.128s\"", (const void*)cmd, st_tmp); trt_core_log(st_line); }
  return cmd;
}

// ---- heap registry integration (HeapSize / HeapReAlloc) ----
u64 TRT_STDCALL trt_HeapSize(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  size_t sz = 0;
  if (trt_heap_rec_peek(p, &sz, nullptr)) return (u64)sz;
  // not ours: Windows would consult the heap; honest answer is 0 + named log
  boot_log("kernel32: HeapSize on a block HeapAlloc did not record -> 0");
  return 0;
}

void* TRT_STDCALL trt_HeapReAlloc(void* heap, u32 flags, void* p, u64 bytes) {
  (void)heap;
  if (!p) return trt_HeapAlloc(heap, flags, bytes);
  size_t old = 0;
  int old_direct = 0;
  const bool known = trt_heap_rec_peek(p, &old, &old_direct) != 0;
  const size_t want = (size_t)(bytes ? bytes : 1);
  const bool new_direct = want > TRT_HEAP_POOL_MAX;
  // Ledger delta for the transition (pool blocks ride on already-charged
  // slabs, so only direct blocks move the ledger):
  //   direct->direct: charge max(0, want-old) now, discharge on shrink later
  //   pool->direct:   charge the full new size (new backend memory)
  //   direct->pool:   no charge now; discharge the old size after the move
  //   pool->pool:     no ledger change at all
  // An unrecorded pointer (foreign to HeapAlloc) is treated as a direct
  // block of size 0, as before: charge the full new size, release the old
  // pointer through the backend, discharge nothing.
  size_t charge_now = 0;
  if (new_direct) {
    if (!known) charge_now = want;
    else if (old_direct) charge_now = want > old ? want - old : 0;
    else charge_now = want;  // pool -> direct: the whole block is new memory
  }
  if (charge_now) {
    if (trt_mem_budget_charge_va((u64)charge_now) != 0) return nullptr;  // 8
    if (trt_mem_budget_charge_commit((u64)charge_now) != 0) {
      trt_mem_budget_discharge_va((u64)charge_now);
      return nullptr;  // 1455
    }
  }
  void* np = nullptr;
  if (!new_direct) {
    np = trt_heap_pool_alloc(want);  // carves and records the pool block
  } else {
    np = g_be.alloc(want, 0);
    if (np) trt_heap_rec_add(np, want, 1);
  }
  if (!np) {
    if (charge_now) {
      trt_mem_budget_discharge_commit((u64)charge_now);
      trt_mem_budget_discharge_va((u64)charge_now);
    }
    // a pool failure already carries 1455/8 from the slab charge; a
    // backend failure needs the error set here
    if (new_direct) set_err_for_current_thread(8);
    return nullptr;
  }
  const size_t copied = old ? (old < (size_t)bytes ? old : (size_t)bytes) : 0;
  if (copied) memcpy(np, p, copied);
  if ((flags & 0x8) && (size_t)bytes > copied) {
    memset((u8*)np + copied, 0, (size_t)bytes - copied);
  }
  // Free the old block (the discharge side of the transition).
  // trt_heap_release reports what it released but leaves the ledger to us:
  // realloc's discharge rules differ from plain free's.
  size_t released = 0;
  int released_direct = 0;
  const int fr = trt_heap_release(p, &released, &released_direct);
  if (fr < 0) {
    g_be.release(p, 0);  // foreign pointer: same backend release as before
  } else if (fr > 0 && released_direct) {
    if (!new_direct) {
      // direct -> pool: the old backend block is gone
      trt_mem_budget_discharge_commit((u64)released);
      trt_mem_budget_discharge_va((u64)released);
    } else if (released > want) {
      // direct -> direct shrink
      trt_mem_budget_discharge_commit((u64)(released - want));
      trt_mem_budget_discharge_va((u64)(released - want));
    }
  }
  return np;
}

u64 TRT_STDCALL trt_HeapQueryInformation(void*, u32, void*, void*, u64, u64*) {
  return 0;  // HeapInformation classes are not implemented: honest 0
}

// ---- FLS (fiber-local storage; uncapped per-thread values) ----
void* TRT_STDCALL trt_FlsGetValue(u32 idx);

void* TRT_STDCALL trt_FlsGetValue2(u32 idx) {
  return trt_FlsGetValue(idx);
}

static void fls_sync_thread_epoch() {
  const u32 ep = g_fls_epoch.load(std::memory_order_acquire);
  if (t_fls_epoch != ep) {
    memset(t_fls_slots, 0, sizeof(t_fls_slots));
    t_fls_epoch = ep;
  }
}

u32 TRT_STDCALL trt_FlsAlloc(void* cb) {
  std::lock_guard<std::mutex> lk(g_fls_mu);
  for (size_t i = 0; i < k_max_fls_slots; ++i) {
    if (!g_fls_meta[i].used) {
      g_fls_meta[i] = {cb, true};
      return (u32)(i + 1);
    }
  }
  return 0xFFFFFFFFu;
}

void* TRT_STDCALL trt_FlsGetValue(u32 idx) {
  fls_sync_thread_epoch();
  if (idx == 0 || idx > k_max_fls_slots) return nullptr;
  return t_fls_slots[idx - 1];
}

int TRT_STDCALL trt_FlsSetValue(u32 idx, void* val) {
  fls_sync_thread_epoch();
  if (idx == 0 || idx > k_max_fls_slots) return 0;
  t_fls_slots[idx - 1] = val;
  return 1;
}

int TRT_STDCALL trt_FlsFree(u32 idx) {
  fls_sync_thread_epoch();
  if (idx == 0 || idx > k_max_fls_slots) return 0;
  void* cb = nullptr;
  void* val = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_fls_mu);
    if (!g_fls_meta[idx - 1].used) return 0;
    cb = g_fls_meta[idx - 1].cb;
    g_fls_meta[idx - 1] = {nullptr, false};
    val = t_fls_slots[idx - 1];
    t_fls_slots[idx - 1] = nullptr;
  }
  if (cb && val) trt_call_guest(cb, (u64)(uintptr_t)val, 0, 0, 0);
  return 1;
}

void trt_fls_on_thread_exit(void) {
  fls_sync_thread_epoch();
  for (int pass = 0; pass < 4; ++pass) {
    bool fired_any = false;
    for (size_t i = 0; i < k_max_fls_slots; ++i) {
      void* val = t_fls_slots[i];
      if (!val) continue;
      t_fls_slots[i] = nullptr;
      void* cb = nullptr;
      {
        std::lock_guard<std::mutex> lk(g_fls_mu);
        if (g_fls_meta[i].used) cb = g_fls_meta[i].cb;
      }
      if (cb) {
        fired_any = true;
        trt_call_guest(cb, (u64)(uintptr_t)val, 0, 0, 0);
      }
    }
    if (!fired_any) break;
  }
}

// ---- code pages / string type / locale-ish strings (CRT init probes) ----
int TRT_STDCALL trt_GetCPInfo(u32 codepage, void* cpinfo) {
  if (!cpinfo) return 0;
  // Win64 CPINFO is 4+2+12=18 bytes (+2 pad => sizeof 20; winnls.h:
  // MAX_DEFAULTCHAR=2, MAX_LEADBYTES=12 — NOT 126; 126 is a misread of the
  // MAX_LEADBYTES=12 — NOT 126; 126 is a misread of the lead-RANGE byte
  // pairs). Writing more here corrupts the caller's stack: the MSVC CRT
  // passes a frame-local CPINFO with its /GS cookie 0x18 bytes past the
  // buffer start, and a 132-byte memset zeroed that cookie — every corpus
  // DLL + OrTMM died in __raise_securityfailure (int 0x29) at CRT locale
  // init from exactly this overwrite (pinned live under gdb: watchpoint on
  // the cookie slot fired inside this bridge at the 0x44-wide movups run).
  struct cpinfo_x { u32 max_size; u8 default_char[2]; u8 lead[12]; };
  static_assert(sizeof(cpinfo_x) == 20, "CPINFO is 18 bytes + 2 pad on x64");
  cpinfo_x* ci = (cpinfo_x*)cpinfo;
  memset(ci, 0, sizeof(cpinfo_x));
  ci->max_size = codepage == 65001 ? 4 : (codepage == 0 || codepage == 1252 ? 1 : 2);
  boot_log("kernel32: GetCPInfo answered (single/double-byte by codepage)");
  return 1;
}

u32 TRT_STDCALL trt_GetConsoleCP(void) { return 437; }
u32 TRT_STDCALL trt_GetConsoleOutputCP(void) { return 65001; }  // UTF-8

int TRT_STDCALL trt_GetStringTypeW(u32 type, const u16* src, int count,
                                   u16* types) {
  (void)type;
  if (!src || count <= 0 || !types) return 0;
  for (int i = 0; i < count; ++i) {
    const u16 c = src[i];
    u16 t = 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) t = 0x0001;  // C1_UPPER|LOWER approx
    else if (c >= '0' && c <= '9') t = 0x0004;                          // C1_DIGIT
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') t = 0x0008;  // C1_SPACE
    else t = 0x0100;                                                     // C1_PUNCT-ish
    types[i] = t;
  }
  return 1;
}

int TRT_STDCALL trt_LCMapStringW(u32 lcid, u32 flags, const u16* src, int srclen,
                                 u16* dst, int dstlen) {
  (void)lcid;
  if (!src || (srclen < 0 && srclen != -1)) return 0;
  const int n = srclen == -1 ? (int)wcslen16(src) : srclen;
  const int needed = n + (srclen == -1 ? 1 : 0);
  if (!dst || dstlen <= 0) return needed > 0 ? needed : 1;
  int i = 0;
  const int limit = (srclen == -1) ? (dstlen - 1) : dstlen;
  for (; i < n && i < limit; ++i) {
    u16 c = src[i];
    if (flags & 0x200 /*LCMAP_LOWERCASE*/ && c >= 'A' && c <= 'Z') c += 32;
    if (flags & 0x100 /*LCMAP_UPPERCASE*/ && c >= 'a' && c <= 'z') c -= 32;
    dst[i] = c;
  }
  if (srclen == -1) {
    dst[i] = 0;
    return i + 1;
  }
  return i;
}

int TRT_STDCALL trt_CompareStringW(u32 lcid, u32 flags, const u16* a, int alen,
                                   const u16* b, int blen) {
  (void)lcid;
  if (!a || !b) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  const int na = alen == -1 ? (int)wcslen16(a) : alen;
  const int nb = blen == -1 ? (int)wcslen16(b) : blen;
  const bool nocase = (flags & 0x1u /*NORM_IGNORECASE*/) != 0;
  for (int i = 0; i < na && i < nb; ++i) {
    u16 ca = a[i];
    u16 cb = b[i];
    if (nocase) {
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
    }
    if (ca != cb) return ca < cb ? 1 : 3;  // CSTR_LESS_THAN / GREATER
  }
  if (na == nb) return 2;                  // CSTR_EQUAL
  return na < nb ? 1 : 3;
}

int TRT_STDCALL trt_CompareStringEx(const u16* locale, u32 flags, const u16* a,
                                    int alen, const u16* b, int blen, void*,
                                    void*, void*) {
  (void)locale;
  return trt_CompareStringW(0, flags, a, alen, b, blen);
}

int TRT_STDCALL trt_LCMapStringEx(const u16* locale, u32 flags, const u16* src,
                                  int srclen, u16* dst, int dstlen, void*,
                                  void*, void*) {
  (void)locale;
  return trt_LCMapStringW(0, flags, src, srclen, dst, dstlen);
}

// ---- SEH / unwind family: Win64 .pdata + UNWIND_INFO walker for guest
// exceptions (e.g. MSVC __CxxFrameHandler4 / C++ try/catch on Linux offline
// and native Windows OS unwinding on UWP).

struct trt_runtime_fn {
  u32 begin, end, unwind;
};

struct veh_rec { void* fn; u64 cookie; bool active; };
std::mutex g_veh_mu;
std::deque<veh_rec> g_vehs;
std::vector<size_t> g_veh_order;
std::atomic<u64> g_veh_seq{1};

static void veh_snapshot_ordered(std::vector<void*>* out) {
  std::lock_guard<std::mutex> lk(g_veh_mu);
  out->clear();
  out->reserve(g_veh_order.size());
  for (size_t idx : g_veh_order) {
    if (idx < g_vehs.size() && g_vehs[idx].active && g_vehs[idx].fn) {
      out->push_back(g_vehs[idx].fn);
    }
  }
}

namespace {

trt_image* find_guest_image_for_pc(u64 pc) {
  if (!pc) return nullptr;
  for (int i = 0, n = trt_images_count(); i < n; ++i) {
    trt_image* img = trt_images_nth(i);
    if (!img || !img->base) continue;
    const u64 base = (u64)(uintptr_t)img->base;
    if (pc >= base && pc < base + img->size_of_image) return img;
  }
  return nullptr;
}

bool is_guest_exec_pc(u64 pc) {
  if (!pc) return false;
  if (find_guest_image_for_pc(pc) != nullptr) return true;
  std::lock_guard<std::mutex> lk(g_va_mu);
  return va_find_idx_locked((uintptr_t)pc) >= 0;
}

struct trt_ex_record {
  u32 code;
  u32 flags;
  u64 record;
  u64 address;
  u32 num_params;
  u32 pad;
  u64 info[15];
};
static_assert(sizeof(trt_ex_record) == 152, "Win64 EXCEPTION_RECORD size");

struct trt_ex_pointers {
  u64 exception_record;
  u64 context_record;
};

struct trt_dispatcher_ctx {
  u64 control_pc;
  u64 image_base;
  u64 function_entry;
  u64 establisher_frame;
  u64 target_pc;
  u64 context_record;
  u64 language_handler;
  u64 handler_data;
  u64 history_table;
  u32 scope_index;
  u8  control_pc_is_unwound;
  u8  pad[3];
  u64 non_volatile_regs;
};
static_assert(sizeof(trt_dispatcher_ctx) == 88, "Win64 DISPATCHER_CONTEXT size");


// Win64 x64 CONTEXT register offsets (verified against mingw winnt.h; 0x4d0 B)
constexpr size_t k_ctx_flags  = 0x030;
constexpr size_t k_ctx_eflags = 0x044;
constexpr size_t k_ctx_rax    = 0x078;
constexpr size_t k_ctx_rip    = 0x0F8;
constexpr size_t k_ctx_xmm0   = 0x1A0;
constexpr size_t k_ctx_size   = 0x4D0;

inline u64 ctx_gpr(const u8* c, u8 reg) {
  u64 v = 0;
  if (reg < 16) memcpy(&v, c + k_ctx_rax + (size_t)reg * 8, 8);
  return v;
}
inline void ctx_set_gpr(u8* c, u8 reg, u64 v) {
  if (reg < 16) memcpy(c + k_ctx_rax + (size_t)reg * 8, &v, 8);
}
inline u64 ctx_rip(const u8* c) {
  u64 v = 0; memcpy(&v, c + k_ctx_rip, 8); return v;
}
inline void ctx_set_rip(u8* c, u64 v) { memcpy(c + k_ctx_rip, &v, 8); }
inline u32 ctx_eflags(const u8* c) {
  u32 v = 0; memcpy(&v, c + k_ctx_eflags, 4); return v;
}
inline void ctx_set_eflags(u8* c, u32 v) { memcpy(c + k_ctx_eflags, &v, 4); }
inline u8* ctx_xmm(u8* c, u8 reg) {
  return c + k_ctx_xmm0 + (size_t)(reg & 15u) * 16;
}


#if !defined(_WIN32) && defined(__x86_64__)

struct dispatch_node {
  u64 return_pc;
  u8 throw_ctx[k_ctx_size];
  dispatch_node* prev;
};
__thread dispatch_node* t_dispatch_head = nullptr;


// Funclet resume records. When RtlUnwindEx stops at TargetFrame and resumes
// the catch/__finally funclet via trt_restore_win64_context, the funclet runs
// on the unwound stack: its caller chain upward is spliced scratch, not real
// frames. Windows never sees this because its dispatcher frames are real and
// stay intact above the funclet. A `throw;` rethrow inside the funclet walks
// funclet -> (garbage) and would exit the guest images. Record the outward
// context (the frame ABOVE the resume target) so the search/unwind walks can
// splice back onto the real caller chain when they leave guest images inside
// the resumed region.
struct resume_node {
  u64 rsp_lo;                    // funclet entry RSP (= TargetFrame RSP)
  u8 outward_ctx[k_ctx_size];    // caller context above the resume target
  u32 call_depth;                // trt guest-call nesting at creation
};
constexpr int k_max_resume_nodes = 32;
__thread resume_node t_resume_nodes[k_max_resume_nodes];
__thread int t_resume_count = 0;
// Set when an EH walk crossed a funclet splice boundary (rescue fired) —
// the UnityPlayer native wrapper-catch signature for boundary escapes.
static thread_local bool t_walk_hit_splice = false;

// Called at the restore point: (funclet_rsp, ctx_above_target).
static void resume_record_push(u64 funclet_rsp, const u8* ctx_above) {
  if (t_resume_count < k_max_resume_nodes) {
    ++t_resume_count;
  } else {
    memmove(t_resume_nodes, t_resume_nodes + 1,
            sizeof(resume_node) * (k_max_resume_nodes - 1));
  }
  resume_node& n = t_resume_nodes[t_resume_count - 1];
  n.rsp_lo = funclet_rsp;
  memcpy(n.outward_ctx, ctx_above, k_ctx_size);
  extern thread_local unsigned g_guest_call_depth;
  n.call_depth = g_guest_call_depth;
}

// Retire records created at call depths >= depth (the guest call that created
// them has returned — its stack, and any splice boundary inside it, is gone).
void trt_eh_retire_resume_records(unsigned depth) {
  while (t_resume_count > 0 && t_resume_nodes[t_resume_count - 1].call_depth >= depth) {
    --t_resume_count;
  }
}

// If the walk (now outside guest images) is inside a resumed funclet's stack
// region, splice its context onto the real caller chain. Returns true when
// the walk context was replaced. Bounded to 16 KiB above the funclet entry
// RSP: the walk must be in the funclet's own frame region, not merely at a
// higher address than it (the thread-root boundary is always higher).
// Consumes the matched record AND every older one: the restored outward
// context is outside the guest images (its rip sits on the host boundary),
// so the very next walk iteration would otherwise re-match an older record
// whose window still contains the same rsp and re-restore the same boundary.
static bool resume_record_rescue(u64 walk_rsp, u8* walk_ctx) {
  for (int i = t_resume_count - 1; i >= 0; --i) {
    const u64 lo = t_resume_nodes[i].rsp_lo;
    if (walk_rsp >= lo && walk_rsp < lo + 0x4000) {
      memcpy(walk_ctx, t_resume_nodes[i].outward_ctx, k_ctx_size);
      t_resume_count = i;
      // The walk crossed a funclet splice boundary: the throw escaped the
      // guest through the host→guest call boundary (the UnityPlayer native
      // wrapper-catch signature on Windows).
      t_walk_hit_splice = true;
      return true;
    }
  }
  return false;
}
// Invoke a Win64 4-arg PE handler (EHANDLER/UHANDLER/CONSOLIDATE) while
// capturing the exact return PC pushed by `call *fn` so RtlUnwindEx can walk
// seamlessly across our ELF host RaiseException dispatcher back to throw_ctx.
__attribute__((noinline)) u64 call_pe_eh_handler(void* fn, u64 a0, u64 a1,
                                                   u64 a2, u64 a3,
                                                   u64* ret_pc_out) {
  if (!fn) return 1;  // ExceptionContinueSearch
  const u64 teb = (u64)(uintptr_t)trt_guest_teb_acquire(nullptr);
  u64 args[6] = {a0, a1, a2, a3, (u64)(uintptr_t)ret_pc_out, teb};
  u64 ret = 0;
  asm volatile(
      // Read ALL memory operands before adjusting %rsp (stack-relative!).
      "movq %[a0], %%rcx\n"
      "movq %[a1], %%rdx\n"
      "movq %[a2], %%r8\n"
      "movq %[a3], %%r9\n"
      "movq %[rp], %%r11\n"
      "movq %[teb], %%rax\n"
      "movq %%rsp, %%r10\n"
      "andq $-16, %%rsp\n"
      "subq $8, %%rsp\n"
      "pushq %%r10\n"
      "subq $48, %%rsp\n"
      "movq %%rax, 32(%%rsp)\n"
      "leaq 9f(%%rip), %%r10\n"
      "testq %%r11, %%r11\n"
      "jz 0f\n"
      "movq %%r10, (%%r11)\n"
      "0:\n"
      "cmpq $0, 32(%%rsp)\n"
      "jz 1f\n"
      "rdgsbase %%r11\n"
      "movq %%r11, 40(%%rsp)\n"
      "movq 32(%%rsp), %%r11\n"
      "wrgsbase %%r11\n"
      "1:\n"
      "call *%[fn]\n"
      "9:\n"
      "cmpq $0, 32(%%rsp)\n"
      "jz 2f\n"
      "movq 40(%%rsp), %%r11\n"
      "wrgsbase %%r11\n"
      "2:\n"
      "addq $48, %%rsp\n"
      "popq %%rsp\n"
      : "=&a"(ret)
      : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
        [a3] "m"(args[3]), [rp] "m"(args[4]), [teb] "m"(args[5]),
        [fn] "r"(fn)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  return ret;
}
#endif

} // namespace

#if !defined(_WIN32) && defined(__x86_64__)
static thread_local int t_hw_fault_depth = 0;

static void unblock_fault_signals(void) {
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, SIGSEGV);
  sigaddset(&unblock, SIGBUS);
  sigaddset(&unblock, SIGILL);
  sigaddset(&unblock, SIGFPE);
  sigaddset(&unblock, SIGTRAP);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
}

__attribute__((naked, used)) void trt_restore_win64_context(const u8* ctx) {
  asm volatile(
      // SysV %rdi = ctx (const u8*); place target Rip right below target Rsp
      "movq 0x98(%rdi), %rsp\n"
      "subq $8, %rsp\n"
      "movq 0xF8(%rdi), %rax\n"
      "movq %rax, (%rsp)\n"
      "movl 0x44(%rdi), %eax\n"
      "pushq %rax\n"
      "popfq\n"
      "movups 0x1A0(%rdi), %xmm0\n"
      "movups 0x1B0(%rdi), %xmm1\n"
      "movups 0x1C0(%rdi), %xmm2\n"
      "movups 0x1D0(%rdi), %xmm3\n"
      "movups 0x1E0(%rdi), %xmm4\n"
      "movups 0x1F0(%rdi), %xmm5\n"
      "movups 0x200(%rdi), %xmm6\n"
      "movups 0x210(%rdi), %xmm7\n"
      "movups 0x220(%rdi), %xmm8\n"
      "movups 0x230(%rdi), %xmm9\n"
      "movups 0x240(%rdi), %xmm10\n"
      "movups 0x250(%rdi), %xmm11\n"
      "movups 0x260(%rdi), %xmm12\n"
      "movups 0x270(%rdi), %xmm13\n"
      "movups 0x280(%rdi), %xmm14\n"
      "movups 0x290(%rdi), %xmm15\n"
      "movq 0x78(%rdi), %rax\n"
      "movq 0x80(%rdi), %rcx\n"
      "movq 0x88(%rdi), %rdx\n"
      "movq 0x90(%rdi), %rbx\n"
      "movq 0xA0(%rdi), %rbp\n"
      "movq 0xA8(%rdi), %rsi\n"
      "movq 0xB8(%rdi), %r8\n"
      "movq 0xC0(%rdi), %r9\n"
      "movq 0xC8(%rdi), %r10\n"
      "movq 0xD0(%rdi), %r11\n"
      "movq 0xD8(%rdi), %r12\n"
      "movq 0xE0(%rdi), %r13\n"
      "movq 0xE8(%rdi), %r14\n"
      "movq 0xF0(%rdi), %r15\n"
      "movq 0xB0(%rdi), %rdi\n"
      "ret\n");
}
#endif

struct dyn_fn_table {
  const trt_runtime_fn* fns;
  u32 count;
  u64 base_addr;
};
static std::mutex g_dyn_fn_mu;
static std::vector<dyn_fn_table> g_dyn_fn_tables;

static const trt_runtime_fn* lookup_dyn_fn(u64 pc, u64* base_out) {
  std::lock_guard<std::mutex> lk(g_dyn_fn_mu);
  for (size_t t = 0; t < g_dyn_fn_tables.size(); ++t) {
    const dyn_fn_table& tab = g_dyn_fn_tables[t];
    for (u32 i = 0; i < tab.count; ++i) {
      const u64 fn_start = tab.base_addr + tab.fns[i].begin;
      const u64 fn_end = tab.base_addr + tab.fns[i].end;
      if (pc >= fn_start && pc < fn_end) {
        if (base_out) *base_out = tab.base_addr;
        return &tab.fns[i];
      }
    }
  }
  return nullptr;
}

static const trt_runtime_fn* lookup_guest_pdata(u64 ctrlpc, u64* base_out,
                                                bool* in_guest_out) {
  if (base_out) *base_out = 0;
  if (in_guest_out) *in_guest_out = false;
  if (trt_image* img = find_guest_image_for_pc(ctrlpc)) {
    if (in_guest_out) *in_guest_out = true;
    const u64 base = (u64)(uintptr_t)img->base;
    if (base_out) *base_out = base;
    if (img->pdata && img->pdata_count) {
      u32 lo = 0, hi = img->pdata_count;
      const trt_runtime_fn* fns = (const trt_runtime_fn*)img->pdata;
      while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        const u64 fn_start = base + fns[mid].begin;
        const u64 fn_end = base + fns[mid].end;
        if (ctrlpc < fn_start) { hi = mid; }
        else if (ctrlpc >= fn_end) { lo = mid + 1; }
        else { return &fns[mid]; }
      }
    }
    return nullptr;
  }
  if (const trt_runtime_fn* dfn = lookup_dyn_fn(ctrlpc, base_out)) {
    if (in_guest_out) *in_guest_out = true;
    return dfn;
  }
  return nullptr;
}

u8 TRT_STDCALL trt_RtlAddFunctionTable(void* fn_table, u32 entry_count,
                                       u64 base_addr) {
  if (!fn_table || entry_count == 0) return 0;
  {
    std::lock_guard<std::mutex> lk(g_dyn_fn_mu);
    bool updated = false;
    for (size_t i = 0; i < g_dyn_fn_tables.size(); ++i) {
      if (g_dyn_fn_tables[i].fns == (const trt_runtime_fn*)fn_table) {
        g_dyn_fn_tables[i].count = entry_count;
        g_dyn_fn_tables[i].base_addr = base_addr;
        updated = true;
        break;
      }
    }
    if (!updated) {
      g_dyn_fn_tables.push_back(
          {(const trt_runtime_fn*)fn_table, entry_count, base_addr});
    }
  }
#ifdef _WIN32
  ::RtlAddFunctionTable((PRUNTIME_FUNCTION)fn_table, entry_count, base_addr);
#endif
  return 1;
}

u8 TRT_STDCALL trt_RtlDeleteFunctionTable(void* fn_table) {
  if (!fn_table) return 0;
  bool removed = false;
  {
    std::lock_guard<std::mutex> lk(g_dyn_fn_mu);
    for (size_t i = 0; i < g_dyn_fn_tables.size(); ++i) {
      if (g_dyn_fn_tables[i].fns == (const trt_runtime_fn*)fn_table) {
        g_dyn_fn_tables[i] = g_dyn_fn_tables.back();
        g_dyn_fn_tables.pop_back();
        removed = true;
        break;
      }
    }
  }
#ifdef _WIN32
  if (::RtlDeleteFunctionTable((PRUNTIME_FUNCTION)fn_table)) removed = true;
#endif
  return removed ? 1 : 0;
}

// Look up the loaded guest image (or JIT function table) owning `pc`.
void* TRT_STDCALL trt_RtlPcToFileHeader(u64 pc, u64* base_out) {
  if (base_out) *base_out = 0;
  if (trt_image* img = find_guest_image_for_pc(pc)) {
    if (base_out) *base_out = (u64)(uintptr_t)img->base;
    return img->base;
  }
  u64 dyn_base = 0;
  if (lookup_dyn_fn(pc, &dyn_base)) {
    if (base_out) *base_out = dyn_base;
    return (void*)(uintptr_t)dyn_base;
  }
#ifdef _WIN32
  PVOID base = nullptr;
  PVOID hdr = ::RtlPcToFileHeader((PVOID)(uintptr_t)pc, &base);
  if (base_out) *base_out = (u64)(uintptr_t)base;
  return hdr;
#else
  return nullptr;
#endif
}

// Walk loaded guest images' .pdata + dynamic JIT tables and return the
// RUNTIME_FUNCTION covering ctrlpc (base_out = image/table base).
void* TRT_STDCALL trt_RtlLookupFunctionEntry(u64 ctrlpc, u64* base_out,
                                             void* history) {
  bool in_guest = false;
  const trt_runtime_fn* fn = lookup_guest_pdata(ctrlpc, base_out, &in_guest);
  if (in_guest) {
    (void)history;
    return (void*)fn;
  }
#ifdef _WIN32
  return (void*)::RtlLookupFunctionEntry((ULONG64)ctrlpc, (PULONG64)base_out,
                                         (PUNWIND_HISTORY_TABLE)history);
#else
  (void)history;
  return nullptr;
#endif
}

#if !defined(_WIN32) && defined(__x86_64__)
__attribute__((naked, used)) void* TRT_STDCALL trt_RtlCaptureContext(void* ctx) {
  asm volatile(
      // Win64 ABI: %rcx = ctx, [%rsp] = caller Rip, %rsp+8 = caller Rsp
      "testq %rcx, %rcx\n"
      "jz 1f\n"
      "movq %rax, 0x78(%rcx)\n"
      "movq %rcx, 0x80(%rcx)\n"
      "movq %rdx, 0x88(%rcx)\n"
      "movq %rbx, 0x90(%rcx)\n"
      "leaq 8(%rsp), %rax\n"
      "movq %rax, 0x98(%rcx)\n"
      "movq %rbp, 0xA0(%rcx)\n"
      "movq %rsi, 0xA8(%rcx)\n"
      "movq %rdi, 0xB0(%rcx)\n"
      "movq %r8,  0xB8(%rcx)\n"
      "movq %r9,  0xC0(%rcx)\n"
      "movq %r10, 0xC8(%rcx)\n"
      "movq %r11, 0xD0(%rcx)\n"
      "movq %r12, 0xD8(%rcx)\n"
      "movq %r13, 0xE0(%rcx)\n"
      "movq %r14, 0xE8(%rcx)\n"
      "movq %r15, 0xF0(%rcx)\n"
      "movq (%rsp), %rax\n"
      "movq %rax, 0xF8(%rcx)\n"
      "pushfq\n"
      "popq %rax\n"
      "movl %eax, 0x44(%rcx)\n"
      "movl $0x10000F, 0x30(%rcx)\n"
      "movl $0x1F80, 0x34(%rcx)\n"
      "movl $0x1F80, 0x118(%rcx)\n"
      "movw $0x33, 0x38(%rcx)\n"
      "movw $0x2B, 0x42(%rcx)\n"
      "movups %xmm0,  0x1A0(%rcx)\n"
      "movups %xmm1,  0x1B0(%rcx)\n"
      "movups %xmm2,  0x1C0(%rcx)\n"
      "movups %xmm3,  0x1D0(%rcx)\n"
      "movups %xmm4,  0x1E0(%rcx)\n"
      "movups %xmm5,  0x1F0(%rcx)\n"
      "movups %xmm6,  0x200(%rcx)\n"
      "movups %xmm7,  0x210(%rcx)\n"
      "movups %xmm8,  0x220(%rcx)\n"
      "movups %xmm9,  0x230(%rcx)\n"
      "movups %xmm10, 0x240(%rcx)\n"
      "movups %xmm11, 0x250(%rcx)\n"
      "movups %xmm12, 0x260(%rcx)\n"
      "movups %xmm13, 0x270(%rcx)\n"
      "movups %xmm14, 0x280(%rcx)\n"
      "movups %xmm15, 0x290(%rcx)\n"
      "1:\n"
      "movq %rcx, %rax\n"
      "ret\n");
}
#else
void* TRT_STDCALL trt_RtlCaptureContext(void* ctx) {
  if (!ctx) return ctx;
#ifdef _WIN32
  ::RtlCaptureContext((PCONTEXT)ctx);
  ULONG64 img_base = 0;
  if (PRUNTIME_FUNCTION rf =
          ::RtlLookupFunctionEntry(((PCONTEXT)ctx)->Rip, &img_base, nullptr)) {
    PVOID hdata = nullptr;
    ULONG64 est = 0;
    ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, img_base, ((PCONTEXT)ctx)->Rip, rf,
                       (PCONTEXT)ctx, &hdata, &est, nullptr);
  }
#else
  memset(ctx, 0, 0x4d0);
#endif
  return ctx;
}
#endif

// ---- guest memory safety for the unwinder --------------------------------
// The unwinder walks GUEST-provided unwind metadata and GUEST stack memory.
// A corrupted .pdata or a stale RSP (measured: UnityPlayer SIGSEGV inside
// trt_RtlVirtualUnwind during its MapViewOfFile crash path) must produce a
// clean "unwind failed" verdict, never a host crash. Safe read = range check
// against registered guest images + Tiramisu bridge mappings.
namespace {
// Readability probe: process_vm_readv copies from the target address and
// returns EFAULT for unmapped pages — no signal, no crash. (First attempt
// used write() to /dev/null: WRONG — /dev/null's write handler never reads
// the user buffer, so unmapped pages "passed" the probe and still SIGSEGV'd
// at the first ui[0] dereference; caught by the test_unwind_safety
// regression and confirmed in the disassembly.)
#ifdef __linux__
bool page_readable(const void* p) {
  char scratch;  // probe writes 1 byte into this local
  struct iovec local { &scratch, 1 };
  struct iovec remote { const_cast<void*>(p), 1 };
  const int saved = errno;
  const ssize_t r = ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0);
  // Only an explicit successful read proves the page is mapped. Any error
  // (EFAULT unmapped / EPERM e.g. hardened kernels or gdb attach contexts /
  // ENOSYS) conservatively reads as NOT readable: the caller fails the
  // unwind cleanly, which is always better than a host SIGSEGV.
  errno = saved;
  return r >= 0;
}
#else
// UWP/console host: VirtualQuery answers committed pages without signals —
// same conservative contract (anything not provably committed+readable is
// NOT readable; the caller fails the unwind cleanly).
bool page_readable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
  return (mbi.Protect & bad) == 0;
}
#endif
bool ptr_in_guest_image(const void* p) {
  if (!p) return false;
  const uintptr_t v = (uintptr_t)p;
  for (int i = 0; i < trt_images_count(); ++i) {
    trt_image* img = trt_images_nth(i);
    if (!img || !img->base) continue;
    const uintptr_t b = (uintptr_t)img->base;
    if (v >= b && v < b + img->size_of_image) return true;
  }
  // .pdata and mapped-file views live outside images; accept a wide host
  // range. Anything below 4 KiB or in the last 64 KiB of the address space
  // is a null/wild dereference by construction. Inside the range, a kernel
  // readability probe catches unmapped metadata (fail-visible, no SIGSEGV).
  if (v < 0x10000ull || v > 0x7ffffffe0000ull) return false;
  return page_readable(p);
}
// Header read with validation: `ui` must sit inside a guest image and the
// slot walk stays within the same image.
const u8* safe_unwind_info(const u8* ui, const u8* image_end) {
  if (!ui || !ptr_in_guest_image(ui)) return nullptr;
  if (image_end && ui + 4 > image_end) return nullptr;
  return ui;
}
}  // namespace

u64 TRT_STDCALL trt_RtlVirtualUnwind(u32 handler_type, u64 image_base,
                                     u64 ctrl_pc, void* fn_entry, void* ctx_in,
                                     void** handler_data, u64* establisher,
                                     void* ctx_ptrs) {
#ifdef _WIN32
  return (u64)(uintptr_t)::RtlVirtualUnwind(
      (DWORD)handler_type, (DWORD64)image_base, (DWORD64)ctrl_pc,
      (PRUNTIME_FUNCTION)fn_entry, (PCONTEXT)ctx_in, (PVOID*)handler_data,
      (PDWORD64)establisher, (PKNONVOLATILE_CONTEXT_POINTERS)ctx_ptrs);
#elif defined(__x86_64__)
  (void)ctx_ptrs;
  if (handler_data) *handler_data = nullptr;
  u8* ctx = (u8*)ctx_in;
  if (!ctx) {
    if (establisher) *establisher = 0;
    return 0;
  }
  const trt_runtime_fn* fn = (const trt_runtime_fn*)fn_entry;
  if (!fn || !image_base) {
    // Leaf function with no .pdata entry: pop return address cleanly.
    const u64 sp = ctx_gpr(ctx, 4);
    if (establisher) *establisher = sp;
    ctx_set_rip(ctx, *(const u64*)(uintptr_t)sp);
    ctx_set_gpr(ctx, 4, sp + 8);
    return 0;
  }
  // fn_entry points into the guest image's .pdata; a garbage entry (stale
  // dispatcher context, corrupted frame) must not be dereferenced.
  if (!ptr_in_guest_image(fn)) {
    if (handler_data) *handler_data = nullptr;
    if (establisher) *establisher = 0;
    return 0;
  }

  bool machframe_popped = false;
  bool is_chained = false;
  bool unwind_failed = false;  // corrupted metadata -> clean failure verdict
  u64 last_establisher = ctx_gpr(ctx, 4);
  const u8* final_ui = nullptr;
  u8 final_flags = 0;
  u8 final_count = 0;

  for (int chain_depth = 0; chain_depth < 32; ++chain_depth) {
    // fn->unwind is read ONCE here; the fwd-chase below re-validates every
    // forwarded fn before ITS unwind read (r14=garbage -> +8 unmapped
    // SIGSEGV, found by test_unwind_safety t2).
    u32 fn_unwind = fn->unwind;
    // Chained unwind info: follow the forwarding pointer, but bounded AND
    // validated — a corrupt .pdata can chain to itself (livelock) or into
    // unmapped memory (SIGSEGV on the next `fn->unwind` read; found by the
    // test_unwind_safety regression). Every hop re-validates before any
    // dereference; beyond 16 hops or any bad hop -> corrupt, fail cleanly.
    for (int fwd = 0; (fn_unwind & 1u) != 0u; ++fwd) {
      const u64 next = image_base + (fn_unwind & ~1u);
      if (fwd >= 16 || next == (u64)(uintptr_t)fn) {
        char bl[128];
        snprintf(bl, sizeof(bl),
                 "ehfix: chained unwind self-loop/over-deep (base=%llx) -> "
                 "unwind failed cleanly\n",
                 (unsigned long long)image_base);
        ssize_t wrc = ::write(2, bl, strlen(bl));
        (void)wrc;
        unwind_failed = true;
        break;
      }
      // Validate the forwarded RUNTIME_FUNCTION (full 12-byte struct) before
      // the next loop iteration dereferences fn->unwind there.
      if (!page_readable((const void*)(uintptr_t)next)) {
        char bl[160];
        snprintf(bl, sizeof(bl),
                 "ehfix: chained unwind target unmapped (next=%llx base=%llx)"
                 " -> unwind failed cleanly\n",
                 (unsigned long long)next, (unsigned long long)image_base);
        ssize_t wrc = ::write(2, bl, strlen(bl));
        (void)wrc;
        unwind_failed = true;
        break;
      }
      fn = (const trt_runtime_fn*)(uintptr_t)next;
      fn_unwind = fn->unwind;  // validated above
    }
    if (unwind_failed) break;
    const u8* ui = (const u8*)(uintptr_t)(image_base + fn_unwind);
    // Validate the unwind-info pointer BEFORE any dereference. UnityPlayer's
    // crash path handed us metadata outside any mapped image.
    {
      // image_end from the registered image whose base matches image_base
      const u8* image_end = nullptr;
      for (int i = 0; i < trt_images_count(); ++i) {
        trt_image* img = trt_images_nth(i);
        if (img && img->base && (u64)(uintptr_t)img->base == image_base) {
          image_end = img->base + img->size_of_image;
          break;
        }
      }
      if (!safe_unwind_info(ui, image_end)) {
        char bl[160];
        snprintf(bl, sizeof(bl),
                 "ehfix: unwind metadata rejected (ui=%llx base=%llx) -> "
                 "unwind failed cleanly\n",
                 (unsigned long long)(uintptr_t)ui,
                 (unsigned long long)image_base);
        ssize_t wrc = ::write(2, bl, strlen(bl));
        (void)wrc;
        unwind_failed = true;
        break;
      }
    }
    const u8 flags = ui[0] >> 3;
    const u8 prolog = ui[1];
    const u8 count = ui[2];
    const u8 fp_byte = ui[3];
    const u8 fpreg = fp_byte & 0x0Fu;
    const u8 fpoff = (fp_byte >> 4) & 0x0Fu;
    const u64 prolog_off =
        ctrl_pc >= image_base + fn->begin ? ctrl_pc - (image_base + fn->begin) : 0;

    // Frame base is used by UWOP_SAVE_NONVOL/SAVE_XMM128 and establishes the
    // fixed frame address when fpreg is live.
    u64 frame_base = ctx_gpr(ctx, 4);
    if (fpreg != 0) {
      bool fp_established = is_chained || (prolog_off >= prolog);
      if (!fp_established) {
        const u16* probe = (const u16*)(ui + 4);
        for (u32 k = 0; k < count;) {
          const u8 coff = (u8)(probe[k] & 0xFFu);
          const u8 op = (u8)((probe[k] >> 8) & 0x0Fu);
          const u8 info = (u8)((probe[k] >> 12) & 0x0Fu);
          if (op == 3 && coff <= prolog_off) { fp_established = true; break; }
          u32 st = 1;
          if (op == 1) st = (info == 0) ? 2 : 3;
          else if (op == 4 || op == 8) st = 2;
          else if (op == 5 || op == 9) st = 3;
          k += st;
        }
      }
      if (fp_established) {
        frame_base = ctx_gpr(ctx, fpreg) - (u64)fpoff * 16ULL;
      }
    }
    last_establisher = frame_base;

    const u16* slots = (const u16*)(ui + 4);
    for (u32 i = 0; i < count;) {
      const u8 code_off = (u8)(slots[i] & 0xFFu);
      const u8 op = (u8)((slots[i] >> 8) & 0x0Fu);
      const u8 info = (u8)((slots[i] >> 12) & 0x0Fu);
      u32 step = 1;
      if (op == 1) step = (info == 0) ? 2 : 3;
      else if (op == 4 || op == 8) step = 2;
      else if (op == 5 || op == 9) step = 3;
      if (i + step > count) break;

      if (is_chained || prolog_off >= code_off) {
        switch (op) {
          case 0: {  // UWOP_PUSH_NONVOL
            const u64 sp = ctx_gpr(ctx, 4);
            ctx_set_gpr(ctx, info, *(const u64*)(uintptr_t)sp);
            ctx_set_gpr(ctx, 4, sp + 8);
            break;
          }
          case 1: {  // UWOP_ALLOC_LARGE
            u64 sz = 0;
            if (info == 0) sz = (u64)slots[i + 1] * 8ULL;
            else sz = (u64)slots[i + 1] | ((u64)slots[i + 2] << 16);
            ctx_set_gpr(ctx, 4, ctx_gpr(ctx, 4) + sz);
            break;
          }
          case 2: {  // UWOP_ALLOC_SMALL
            ctx_set_gpr(ctx, 4, ctx_gpr(ctx, 4) + (u64)info * 8ULL + 8ULL);
            break;
          }
          case 3: {  // UWOP_SET_FPREG
            if (fpreg != 0) {
              ctx_set_gpr(ctx, 4, ctx_gpr(ctx, fpreg) - (u64)fpoff * 16ULL);
            }
            break;
          }
          case 4: {  // UWOP_SAVE_NONVOL
            const u64 off = (u64)slots[i + 1] * 8ULL;
            ctx_set_gpr(ctx, info, *(const u64*)(uintptr_t)(frame_base + off));
            break;
          }
          case 5: {  // UWOP_SAVE_NONVOL_FAR
            const u64 off = (u64)slots[i + 1] | ((u64)slots[i + 2] << 16);
            ctx_set_gpr(ctx, info, *(const u64*)(uintptr_t)(frame_base + off));
            break;
          }
          case 6:  // UWOP_EPILOG (v2)
          case 7:  // UWOP_SPARE_CODE
            break;
          case 8: {  // UWOP_SAVE_XMM128
            const u64 off = (u64)slots[i + 1] * 16ULL;
            memcpy(ctx_xmm(ctx, info),
                   (const void*)(uintptr_t)(frame_base + off), 16);
            break;
          }
          case 9: {  // UWOP_SAVE_XMM128_FAR
            const u64 off = (u64)slots[i + 1] | ((u64)slots[i + 2] << 16);
            memcpy(ctx_xmm(ctx, info),
                   (const void*)(uintptr_t)(frame_base + off), 16);
            break;
          }
          case 10: {  // UWOP_PUSH_MACHFRAME
            const u64 sp = ctx_gpr(ctx, 4) + (info ? 8ULL : 0ULL);
            const u64* mf = (const u64*)(uintptr_t)sp;
            ctx_set_rip(ctx, mf[0]);
            ctx_set_eflags(ctx, (u32)mf[2]);
            ctx_set_gpr(ctx, 4, mf[3]);
            machframe_popped = true;
            break;
          }
          default:
            break;
        }
      }
      i += step;
    }

    final_ui = ui;
    final_flags = flags;
    final_count = count;
    if ((flags & 0x04u) == 0) break;  // !UNW_FLAG_CHAININFO
    // UNW_FLAG_CHAININFO: the slot after the (even-padded) unwind codes holds
    // an image-relative RVA of the chained RUNTIME_FUNCTION (the parent frame
    // — MSVC catch/__finally funclets always chain). Dereference it; treating
    // the slot address as the function entry reads the scope table as unwind
    // metadata and produces garbage caller contexts.
    const u32 even_codes = ((u32)count + 1u) & ~1u;
    u32 chain_rva = 0;
    memcpy(&chain_rva, ui + 4 + even_codes * 2u, 4);
    fn = (const trt_runtime_fn*)(uintptr_t)(image_base + (u64)chain_rva);
    is_chained = true;
  }

  if (unwind_failed) {
    // Corrupt/unmapped unwind metadata: return "no handler, can't unwind".
    // The dispatcher turns this into unhandled-exception handling (which the
    // runtime already routes to the guest's UEF / crash report) instead of
    // faulting the host inside the unwinder.
    if (handler_data) *handler_data = nullptr;
    if (establisher) *establisher = 0;
    return 0;
  }
  if (!machframe_popped) {
    const u64 sp = ctx_gpr(ctx, 4);
    ctx_set_rip(ctx, *(const u64*)(uintptr_t)sp);
    ctx_set_gpr(ctx, 4, sp + 8);
  }
  if (establisher) *establisher = last_establisher;

  if (final_ui && (final_flags & (handler_type & 0x03u)) != 0) {
    const u32 even_codes = ((u32)final_count + 1u) & ~1u;
    const u8* tail = final_ui + 4 + even_codes * 2u;
    u32 handler_rva = 0;
    memcpy(&handler_rva, tail, 4);
    if (handler_data) *handler_data = (void*)(tail + 4);
    return image_base + handler_rva;
  }
  return 0;
#else
  (void)handler_type; (void)image_base; (void)ctrl_pc; (void)fn_entry;
  (void)ctx_in; (void)handler_data; (void)establisher; (void)ctx_ptrs;
  return 0;
#endif
}

namespace {
std::atomic<void*> g_unhandled_filter{nullptr};

const char* eh_filter_verdict_name(i32 v) {
  if (v == 1 /*EXCEPTION_EXECUTE_HANDLER*/) return "handled";
  if (v == -1 /*EXCEPTION_CONTINUE_EXECUTION*/)
    return "unhandled(continue-exec)";  // undefined per MSDN; treated as death
  return "unhandled";
}
}  // namespace

#if !defined(_WIN32) && defined(__x86_64__)
static bool eh_trace_enabled() {
  static const bool on = getenv("TRT_EH_TRACE") != nullptr;
  return on;
}

extern "C" __attribute__((used)) void trt_rtl_unwind_ex_sysv(
    u64 target_frame, u64 target_ip, void* exrec_in, u64 retval, void* ctx_out,
    void* history, const u8* caller_ctx_in) {
  (void)history;
  u8 ctx[k_ctx_size];
  // RtlUnwindEx starts at the caller-supplied ContextRecord (the exception
  // context, i.e. the throw site) so inner __finally handlers run on the way
  // to TargetFrame. A null ContextRecord means "use my own caller's context",
  // which is what RtlUnwind() from a __finally funclet does.
  memcpy(ctx, ctx_out ? (const u8*)ctx_out : caller_ctx_in, k_ctx_size);
  // Zero unused CONTEXT areas so handlers inspect a clean Win64 CONTEXT
  memset(ctx, 0, 0x30);
  memset(ctx + 0x100, 0, 0xA0);
  memset(ctx + 0x2A0, 0, k_ctx_size - 0x2A0);
  const u32 init_flags = 0x10000Fu;
  const u32 init_mxcsr = 0x1F80u;
  memcpy(ctx + k_ctx_flags, &init_flags, 4);
  memcpy(ctx + 0x34, &init_mxcsr, 4);
  memcpy(ctx + 0x118, &init_mxcsr, 4);

  if (eh_trace_enabled()) {
    char t[160];
    snprintf(t, sizeof(t),
             "kernel32: UNWIND enter target=0x%llx target_ip=0x%llx ctx_rip=0x%llx ctx_rsp=0x%llx",
             (unsigned long long)target_frame, (unsigned long long)target_ip,
             (unsigned long long)ctx_rip(ctx), (unsigned long long)ctx_gpr(ctx, 4));
    boot_log(t);
  }

  trt_ex_record ex_local = {};
  const trt_ex_record* ex_orig = (const trt_ex_record*)exrec_in;
  if (ex_orig) {
    ex_local = *ex_orig;
  } else {
    ex_local.code = 0xC0000027u;  // STATUS_UNWIND
    ex_local.address = ctx_rip(ctx);
  }
  ex_local.flags |= 0x02u;  // EXCEPTION_UNWINDING
  if (!target_frame) ex_local.flags |= 0x04u;  // EXCEPTION_EXIT_UNWIND

  u64 hist_buf[16] = {};
  u64 prev_rsp = ctx_gpr(ctx, 4);  // last good guest frame's RSP
  for (int depth = 0; depth < 512; ++depth) {
    const u64 rip = ctx_rip(ctx);
    if (eh_trace_enabled()) {
      char t[160];
      snprintf(t, sizeof(t),
               "kernel32: UNWIND depth=%d rip=0x%llx rsp=0x%llx target=0x%llx",
               depth, (unsigned long long)rip,
               (unsigned long long)ctx_gpr(ctx, 4),
               (unsigned long long)target_frame);
      boot_log(t);
    }
    // Bridge seamlessly across any active ELF RaiseException dispatcher frame.
    bool crossed = false;
    for (dispatch_node* d = t_dispatch_head; d; d = d->prev) {
      if (rip == d->return_pc) {
        // Unwinding ascends to older frames. A node whose throw context sits
        // deeper than the current walk position is a stale/replay match (its
        // dispatcher frame was reused by a nested dispatch) — crossing it
        // ping-pongs the loop forever. Skip it.
        if (ctx_gpr(d->throw_ctx, 4) <= ctx_gpr(ctx, 4)) {
          if (eh_trace_enabled()) {
            char t[160];
            snprintf(t, sizeof(t),
                     "kernel32: UNWIND skip-stale depth=%d rip=0x%llx throw_rsp=0x%llx cur_rsp=0x%llx",
                     depth, (unsigned long long)rip,
                     (unsigned long long)ctx_gpr(d->throw_ctx, 4),
                     (unsigned long long)ctx_gpr(ctx, 4));
            boot_log(t);
          }
          continue;
        }
        if (eh_trace_enabled()) {
          char t[160];
          snprintf(t, sizeof(t),
                   "kernel32: UNWIND cross depth=%d rip=0x%llx -> throw_rsp=0x%llx cur_rsp=0x%llx",
                   depth, (unsigned long long)rip,
                   (unsigned long long)ctx_gpr(d->throw_ctx, 4),
                   (unsigned long long)ctx_gpr(ctx, 4));
          boot_log(t);
        }
        memcpy(ctx, d->throw_ctx, k_ctx_size);
        t_dispatch_head = d->prev;
        crossed = true;
        break;
      }
    }
    if (crossed) continue;

    u64 image_base = 0;
    const u64 lookup_pc = (depth > 0 && rip > 0) ? (rip - 1) : rip;
    void* fn = trt_RtlLookupFunctionEntry(lookup_pc, &image_base, hist_buf);
    if (!image_base) {
      // Walk left the guest images. If this is inside a resumed funclet's
      // spliced stack region, continue the unwind on the real caller chain
      // above the resume target (Windows: real dispatcher frames). Test the
      // bogus context's RSP first, then the last good guest frame's RSP.
      const u64 walk_rsp = ctx_gpr(ctx, 4);
      if (resume_record_rescue(walk_rsp, ctx) ||
          resume_record_rescue(prev_rsp, ctx)) {
        if (eh_trace_enabled()) {
          char t[160];
          snprintf(t, sizeof(t),
                   "kernel32: UNWIND funclet-rescue depth=%d rsp=0x%llx prev=0x%llx -> rip=0x%llx",
                   depth, (unsigned long long)walk_rsp,
                   (unsigned long long)prev_rsp,
                   (unsigned long long)ctx_rip(ctx));
          boot_log(t);
        }
        prev_rsp = ctx_gpr(ctx, 4);
        continue;
      }
      boot_log("kernel32: RtlUnwindEx left guest PE images before TargetFrame");
      break;
    }

    u8 next_ctx[k_ctx_size];
    memcpy(next_ctx, ctx, k_ctx_size);
    void* handler_data = nullptr;
    u64 establisher = 0;
    const u64 handler = trt_RtlVirtualUnwind(
        2u /*UNW_FLAG_UHANDLER*/, image_base, lookup_pc, fn, next_ctx,
        &handler_data, &establisher, nullptr);

    if (target_frame && establisher == target_frame) {
      // Stopped at TargetFrame while `ctx` STILL holds TargetFrame's stack
      // pointer and non-volatile registers!
      u64 resume_ip = target_ip;
      if (ex_orig && ex_orig->code == 0x80000029u /*STATUS_UNWIND_CONSOLIDATE*/ &&
          ex_orig->num_params >= 1 && ex_orig->info[0]) {
        trt_ex_record consolidate_rec = *ex_orig;
        dispatch_node cnode = {};
        memcpy(cnode.throw_ctx, ctx, k_ctx_size);
        cnode.prev = t_dispatch_head;
        t_dispatch_head = &cnode;
        resume_ip = call_pe_eh_handler(
            (void*)(uintptr_t)consolidate_rec.info[0],
            (u64)(uintptr_t)&consolidate_rec, 0, 0, 0, &cnode.return_pc);
        t_dispatch_head = cnode.prev;
      }
      ctx_set_gpr(ctx, 0 /*Rax*/, retval);
      ctx_set_rip(ctx, resume_ip);
      if (ctx_out) memcpy(ctx_out, ctx, k_ctx_size);
      unblock_fault_signals();
      t_hw_fault_depth = 0;
      // Restoring the establishing frame abandons every host frame above it,
      // including the RaiseException dispatcher that owns the bridge nodes;
      // leaving them on t_dispatch_head would hand the next throw a pointer
      // into freed stack.
      t_dispatch_head = nullptr;
      // Record the funclet resume so a rethrow inside the catch/__finally can
      // splice back onto the real caller chain (the frame above the target).
      // Record even when the outward rip is not guest code (an outermost
      // target leaves the host trampoline pc in the caller slot): the rescue
      // restores it and the walk resolves through the remaining records —
      // refusing to record here is what killed the worker-thread rethrow.
      resume_record_push(ctx_gpr(ctx, 4), next_ctx);
      if (eh_trace_enabled()) {
        u64 outward_rsp = ctx_gpr(next_ctx, 4);
        u64 outward_slot = 0;
        memcpy(&outward_slot, (const void*)(uintptr_t)outward_rsp, sizeof(outward_slot));
        char t[224];
        snprintf(t, sizeof(t),
                 "kernel32: UNWIND resume-record rsp=0x%llx -> outward rip=0x%llx%s "
                 "outward_rsp=0x%llx outward_slot=0x%llx",
                 (unsigned long long)ctx_gpr(ctx, 4),
                 (unsigned long long)ctx_rip(next_ctx),
                 find_guest_image_for_pc(ctx_rip(next_ctx)) ? "" : " (host boundary)",
                 (unsigned long long)outward_rsp,
                 (unsigned long long)outward_slot);
        boot_log(t);
      }
      trt_restore_win64_context(ctx);
    }

    if (handler) {
      trt_dispatcher_ctx dc = {};
      dc.control_pc = rip;
      dc.image_base = image_base;
      dc.function_entry = (u64)(uintptr_t)fn;
      dc.establisher_frame = establisher;
      dc.target_pc = target_ip;
      dc.context_record = (u64)(uintptr_t)ctx;
      dc.language_handler = handler;
      dc.handler_data = (u64)(uintptr_t)handler_data;
      dc.history_table = (u64)(uintptr_t)hist_buf;
      dc.control_pc_is_unwound = (depth > 0) ? 1u : 0u;
      dispatch_node unode = {};
      memcpy(unode.throw_ctx, next_ctx, k_ctx_size);
      unode.prev = t_dispatch_head;
      t_dispatch_head = &unode;
      const u32 disp = (u32)call_pe_eh_handler(
          (void*)(uintptr_t)handler, (u64)(uintptr_t)&ex_local, establisher,
          (u64)(uintptr_t)ctx, (u64)(uintptr_t)&dc, &unode.return_pc);
      t_dispatch_head = unode.prev;
      if (eh_trace_enabled()) {
        char t[160];
        snprintf(t, sizeof(t),
                 "kernel32: UNWIND handler depth=%d frame=0x%llx -> verdict=%u (target=0x%llx)",
                 depth, (unsigned long long)establisher, disp,
                 (unsigned long long)target_frame);
        boot_log(t);
      }
      if (disp == 3 /*ExceptionCollidedUnwind*/ && dc.control_pc) {
        memcpy(ctx, (const void*)(uintptr_t)dc.context_record, k_ctx_size);
        continue;
      }
    }

    if (target_frame && establisher > target_frame) {
      boot_log("kernel32: RtlUnwindEx passed TargetFrame without match");
      break;
    }
    const u64 cur_sp = ctx_gpr(ctx, 4);
    const u64 nxt_sp = ctx_gpr(next_ctx, 4);
    if (nxt_sp < cur_sp || (nxt_sp == cur_sp && ctx_rip(next_ctx) == rip)) {
      if (eh_trace_enabled()) {
        char t[160];
        snprintf(t, sizeof(t),
                 "kernel32: UNWIND stop depth=%d nxt_sp=0x%llx cur_sp=0x%llx nxt_rip=0x%llx",
                 depth, (unsigned long long)nxt_sp, (unsigned long long)cur_sp,
                 (unsigned long long)ctx_rip(next_ctx));
        boot_log(t);
      }
      break;
    }
    prev_rsp = cur_sp;
    memcpy(ctx, next_ctx, k_ctx_size);
  }
}

void* TRT_STDCALL trt_GetModuleHandleA(const char* name);

static int dispatch_exception_ctx(trt_ex_record* ex, u8* ctx,
                                  bool allow_continue, bool from_signal) {
  {
    char dm[128];
    snprintf(dm, sizeof(dm), "  [deh] start code=0x%x addr=0x%llx\n", ex->code, (unsigned long long)ex->address);
    ssize_t w = ::write(2, dm, strlen(dm)); (void)w;
  }
  char line[192];
  // Per-dispatch splice-escape flag: set if a walk in THIS dispatch crossed a
  // funclet splice boundary (rescue fired) and still ended up outside the
  // guest images — the UnityPlayer native wrapper-catch signature.
  t_walk_hit_splice = false;
  // First-chance Vectored Exception Handlers (ordered: first=1 prepends, first=0 appends).
  trt_ex_pointers ep = {(u64)(uintptr_t)ex, (u64)(uintptr_t)ctx};
  std::vector<void*> vehs;
  veh_snapshot_ordered(&vehs);
  {
    char dm[64];
    snprintf(dm, sizeof(dm), "  [deh] vehs count=%zu\n", vehs.size());
    ssize_t w = ::write(2, dm, strlen(dm)); (void)w;
  }
  for (size_t vi = 0; vi < vehs.size(); ++vi) {
    void* vfn = vehs[vi];
    {
      trt_image* mimg = trt_images_find("mono.dll");
      void* mbase = mimg ? trt_images_handle(mimg) : nullptr;
      char dm[160];
      snprintf(dm, sizeof(dm), "  [deh] calling veh[%zu]=%p (mono rva=0x%lx)\n",
               vi, vfn, mbase ? (unsigned long)((uintptr_t)vfn - (uintptr_t)mbase) : 0UL);
      ssize_t w = ::write(2, dm, strlen(dm)); (void)w;
    }
    const i32 vres = (i32)trt_call_guest(vfn, (u64)(uintptr_t)&ep, 0, 0, 0);
    {
      char dm[128];
      snprintf(dm, sizeof(dm), "  [deh] veh[%zu] returned %d\n", vi, vres);
      ssize_t w = ::write(2, dm, strlen(dm)); (void)w;
    }
    if (vres == -1 /*EXCEPTION_CONTINUE_EXECUTION*/ && allow_continue) {
      return 1;
    }
  }

  // Frame-based SEH / C++ EH pass-1 search via .pdata + UNW_FLAG_EHANDLER.
  u8 walk_ctx[k_ctx_size];
  memcpy(walk_ctx, ctx, k_ctx_size);
  u64 hist_buf[16] = {};
  u64 prev_rsp = ctx_gpr(walk_ctx, 4);  // last good guest frame's RSP
  dispatch_node* pass1_dispatch = t_dispatch_head;
  if (t_resume_count > 0 && ctx_gpr(walk_ctx, 4) > t_resume_nodes[t_resume_count - 1].rsp_lo) {
    // Geometric staleness check. Records are pushed outward during pass 2, so
    // the newest has the HIGHEST funclet-entry rsp. A LIVE rethrow inside a
    // resumed funclet always throws at an rsp BELOW every record's rsp_lo
    // (the catch runs on the unwound stack). A brand-new unrelated throw that
    // starts ABOVE the newest resume target cannot belong to any live saga —
    // measured raccoin-ehfix12: a CRT-startup record at rsp 0x...d360
    // false-matched a CreateSwapChainForHwnd walk at 0x...d950 and spliced
    // garbage. Purge only that impossible case; deeper walks keep the records.
    t_resume_count = 0;
  }
  for (int depth = 0; depth < 512; ++depth) {
    const u64 rip = ctx_rip(walk_ctx);
    // Bridge seamlessly across any active ELF call_pe_eh_handler frame
    // (e.g. rethrow / nested throw inside STATUS_UNWIND_CONSOLIDATE catch
    // funclets) without mutating t_dispatch_head during pass-1 search.
    bool crossed = false;
    for (dispatch_node* d = pass1_dispatch; d; d = d->prev) {
      if (rip == d->return_pc) {
        // A crossing must move the walk outward to older frames (higher RSP).
        // A node whose throw context is deeper than the current position is a
        // stale/replay match — crossing it would rewind the search into
        // frames it already visited (infinite ping-pong).
        if (ctx_gpr(d->throw_ctx, 4) <= ctx_gpr(walk_ctx, 4)) {
          if (eh_trace_enabled()) {
            char t[160];
            snprintf(t, sizeof(t),
                     "kernel32: EH skip-stale depth=%d rip=0x%llx throw_rsp=0x%llx cur_rsp=0x%llx",
                     depth, (unsigned long long)rip,
                     (unsigned long long)ctx_gpr(d->throw_ctx, 4),
                     (unsigned long long)ctx_gpr(walk_ctx, 4));
            boot_log(t);
          }
          continue;
        }
        if (eh_trace_enabled()) {
          char t[160];
          snprintf(t, sizeof(t),
                   "kernel32: EH cross depth=%d rip=0x%llx -> throw_rsp=0x%llx cur_rsp=0x%llx",
                   depth, (unsigned long long)rip,
                   (unsigned long long)ctx_gpr(d->throw_ctx, 4),
                   (unsigned long long)ctx_gpr(walk_ctx, 4));
          boot_log(t);
        }
        memcpy(walk_ctx, d->throw_ctx, k_ctx_size);
        pass1_dispatch = d->prev;
        crossed = true;
        break;
      }
    }
    if (crossed) continue;

    u64 image_base = 0;
    const u64 lookup_pc = (depth > 0 && rip > 0) ? (rip - 1) : rip;
    void* fn = trt_RtlLookupFunctionEntry(lookup_pc, &image_base, hist_buf);
    if (eh_trace_enabled()) {
      char t[160];
      snprintf(t, sizeof(t), "kernel32: EH depth=%d rip=0x%llx base=0x%llx fn=%p",
               depth, (unsigned long long)rip, (unsigned long long)image_base, fn);
      boot_log(t);
    }
    if (!image_base) {
      // Walk left the guest images. Inside a resumed funclet's spliced stack
      // region, continue the search on the real caller chain above the resume
      // target (Windows gets this from its intact dispatcher frames). The
      // bogus caller context may carry a garbage RSP, so also test the last
      // good guest frame's RSP — that one lands in the funclet region.
      const u64 walk_rsp = ctx_gpr(walk_ctx, 4);
      if (resume_record_rescue(walk_rsp, walk_ctx) ||
          resume_record_rescue(prev_rsp, walk_ctx)) {
        if (eh_trace_enabled()) {
          char t[160];
          snprintf(t, sizeof(t),
                   "kernel32: EH pass-1 funclet-rescue depth=%d rsp=0x%llx prev=0x%llx -> rip=0x%llx",
                   depth, (unsigned long long)walk_rsp,
                   (unsigned long long)prev_rsp,
                   (unsigned long long)ctx_rip(walk_ctx));
          boot_log(t);
        }
        prev_rsp = ctx_gpr(walk_ctx, 4);
        continue;
      }
      if (!from_signal) {
        snprintf(line, sizeof(line),
                 "kernel32: EH pass-1 depth=%d rip=0x%llx left guest images (no .pdata owner)",
                 depth, (unsigned long long)rip);
        boot_log(line);
      }
      break;
    }

    u8 next_ctx[k_ctx_size];
    memcpy(next_ctx, walk_ctx, k_ctx_size);
    void* handler_data = nullptr;
    u64 establisher = 0;
    const u64 handler = trt_RtlVirtualUnwind(
        1u /*UNW_FLAG_EHANDLER*/, image_base, lookup_pc, fn, next_ctx,
        &handler_data, &establisher, nullptr);

    if (handler) {
      trt_dispatcher_ctx dc = {};
      dc.control_pc = rip;
      dc.image_base = image_base;
      dc.function_entry = (u64)(uintptr_t)fn;
      dc.establisher_frame = establisher;
      // Win64 contract: ContextRecord is the *exception* context (the fault /
      // throw site), never the per-frame walk copy. __C_specific_handler hands
      // it straight to RtlUnwindEx, which must start unwinding at the throw so
      // inner __finally handlers run on the way to EstablisherFrame.
      dc.context_record = (u64)(uintptr_t)ctx;
      dc.language_handler = handler;
      dc.handler_data = (u64)(uintptr_t)handler_data;
      dc.history_table = (u64)(uintptr_t)hist_buf;
      dc.control_pc_is_unwound = (depth > 0) ? 1u : 0u;

      dispatch_node node = {};
      memcpy(node.throw_ctx, ctx, k_ctx_size);
      node.prev = t_dispatch_head;
      t_dispatch_head = &node;
      const u32 disp = (u32)call_pe_eh_handler(
          (void*)(uintptr_t)handler, (u64)(uintptr_t)ex, establisher,
          (u64)(uintptr_t)ctx, (u64)(uintptr_t)&dc, &node.return_pc);
      t_dispatch_head = node.prev;
      if (eh_trace_enabled()) {
        char t[160];
        snprintf(t, sizeof(t),
                 "kernel32: EH handler depth=%d frame=0x%llx -> verdict=%u",
                 depth, (unsigned long long)establisher, disp);
        boot_log(t);
      }

      if (disp == 1 /*ExceptionContinueExecution*/ && allow_continue) {
        // the personality may have rewritten the exception context in place
        return 1;
      }
      if (disp == 3 /*ExceptionCollidedUnwind*/ && dc.control_pc) {
        memcpy(walk_ctx, (const void*)(uintptr_t)dc.context_record, k_ctx_size);
        continue;
      }
    }

    const u64 cur_sp = ctx_gpr(walk_ctx, 4);
    const u64 nxt_sp = ctx_gpr(next_ctx, 4);
    if (nxt_sp < cur_sp || (nxt_sp == cur_sp && ctx_rip(next_ctx) == rip)) {
      if (eh_trace_enabled()) {
        char t[160];
        snprintf(t, sizeof(t),
                 "kernel32: EH walk stop depth=%d nxt_sp=0x%llx cur_sp=0x%llx nxt_rip=0x%llx",
                 depth, (unsigned long long)nxt_sp, (unsigned long long)cur_sp,
                 (unsigned long long)ctx_rip(next_ctx));
        boot_log(t);
      }
      break;
    }
    prev_rsp = cur_sp;
    memcpy(walk_ctx, next_ctx, k_ctx_size);
  }
  return 0;
}

extern "C" int trt_dispatch_hw_fault(int sig, void* siginfo_v, void* uctx_v) {
  if (!siginfo_v || !uctx_v) return 0;
  if (t_hw_fault_depth >= 4) return 0;
  ucontext_t* uc = (ucontext_t*)uctx_v;
  siginfo_t* si = (siginfo_t*)siginfo_v;
  const u64 rip = (u64)uc->uc_mcontext.gregs[REG_RIP];
  if (sig == SIGSEGV && si->si_code > 0) {
    const u64 error = (u64)uc->uc_mcontext.gregs[REG_ERR];
    const u32 access = (error & 16u) ? 8u : ((error & 2u) ? 1u : 0u);
    if (trt_va_handle_deferred_data_fault((u64)(uintptr_t)si->si_addr, access)) return 1;
  }
  if (!is_guest_exec_pc(rip)) return 0;

  if (sig == SIGSEGV &&
      ((const u8*)(uintptr_t)rip)[0] == 0xCDu &&
      ((const u8*)(uintptr_t)rip)[1] == 0x29u) {
    char ff[128];
    snprintf(ff, sizeof(ff),
             "kernel32: __fastfail(code=%llu) at rip=0x%llx -> non-continuable",
             (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
             (unsigned long long)rip);
    boot_log(ff);
    return 0;
  }

  if (sig == SIGSEGV &&
      ((u64)uc->uc_mcontext.gregs[REG_ERR] & 16ULL) &&
      trt_va_try_promote_rx(rip)) {
    return 1;
  }

  trt_ex_record ex = {};
  ex.address = rip;
  switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
      ex.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
      const u64 err = (u64)uc->uc_mcontext.gregs[REG_ERR];
      ex.num_params = 2;
      ex.info[0] = (err & 16ULL) ? 8ULL : ((err & 2ULL) ? 1ULL : 0ULL);
      ex.info[1] = (u64)(uintptr_t)si->si_addr;
      break;
    }
    case SIGILL:
      ex.code = 0xC000001Du;
      break;
    case SIGFPE:
      ex.code = (si->si_code == FPE_INTDIV) ? 0xC0000094u : 0xC0000090u;
      break;
    case SIGTRAP:
      ex.code = 0x80000003u;
      break;
    default:
      return 0;
  }

  static const int k_uc_regs[16] = {
      REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
      REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
  };
  u8 ctx[k_ctx_size] = {};
  const u32 init_flags = 0x10000Fu;
  u32 mxcsr = 0x1F80u;
  if (uc->uc_mcontext.fpregs) mxcsr = (u32)uc->uc_mcontext.fpregs->mxcsr;
  memcpy(ctx + k_ctx_flags, &init_flags, 4);
  memcpy(ctx + 0x34, &mxcsr, 4);
  memcpy(ctx + 0x118, &mxcsr, 4);
  ctx_set_rip(ctx, rip);
  ctx_set_eflags(ctx, (u32)uc->uc_mcontext.gregs[REG_EFL]);
  for (u8 r = 0; r < 16; ++r) {
    ctx_set_gpr(ctx, r, (u64)uc->uc_mcontext.gregs[k_uc_regs[r]]);
  }
  if (uc->uc_mcontext.fpregs) {
    for (u8 x = 0; x < 16; ++x) {
      memcpy(ctx_xmm(ctx, x), &uc->uc_mcontext.fpregs->_xmm[x], 16);
    }
  }

  ++t_hw_fault_depth;
  const int handled = dispatch_exception_ctx(&ex, ctx, true, true);
  --t_hw_fault_depth;
  if (handled) {
    const u64 stack = (u64)uc->uc_mcontext.gregs[REG_RSP];
    if (ctx_rip(ctx) != rip || ctx_gpr(ctx, 4) != stack) {
      t_fault_retry = {};
    } else if (guest_fault_retry_exhausted(rip, stack, (u64)(uintptr_t)si->si_addr,
                                            ex.code, static_cast<u32>(ex.info[0]))) {
      static constexpr char reason[] = "kernel32: repeated guest fault made no progress\n";
      (void)::write(2, reason, sizeof(reason) - 1);
      return 0;
    }
    for (u8 r = 0; r < 16; ++r) {
      uc->uc_mcontext.gregs[k_uc_regs[r]] = (greg_t)ctx_gpr(ctx, r);
    }
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ctx_rip(ctx);
    uc->uc_mcontext.gregs[REG_EFL] = (greg_t)ctx_eflags(ctx);
    if (uc->uc_mcontext.fpregs) {
      for (u8 x = 0; x < 16; ++x) {
        memcpy(&uc->uc_mcontext.fpregs->_xmm[x], ctx_xmm(ctx, x), 16);
      }
    }
    return 1;
  }
  return 0;
}

extern "C" int TRT_STDCALL trt_dispatch_win32_exception(void* ep) {
  if (!ep) return 0;
  static thread_local int t_win32_exc_depth = 0;
  if (t_win32_exc_depth > 2) return 0;
  ++t_win32_exc_depth;
  std::vector<void*> vehs;
  veh_snapshot_ordered(&vehs);
  int result = 0;
  for (void* vfn : vehs) {
    if (!vfn) continue;
    const i32 rc = (i32)(u32)trt_call_guest(vfn, (u64)(uintptr_t)ep, 0, 0, 0);
    if (rc == -1 /*EXCEPTION_CONTINUE_EXECUTION*/) {
      result = -1;
      break;
    }
  }
  --t_win32_exc_depth;
  return result;
}

// ---- Sprint 3 item 46: the last-chance unhandled-exception filter ---------
// SetUnhandledExceptionFilter stores the guest filter; the terminal fatal path
// (trt_raise_exception_sysv after the SEH walk finds no handler) invokes it
// once with a Win64 EXCEPTION_POINTERS before terminating, and the
// UnhandledExceptionFilter export consults it exactly as real Win32 does
// (return the filter's verdict; EXCEPTION_CONTINUE_SEARCH with none stored).

// Sprint 3 item 46: the terminal unhandled-exception path gives the stored
// SetUnhandledExceptionFilter one look at the record before the runtime
// terminates. Win32 semantics: the filter runs, then the process dies either
// way (EXCEPTION_EXECUTE_HANDLER or CONTINUE_SEARCH both end in termination;
// CONTINUE_EXECUTION from a last-chance filter is undefined per MSDN — logged
// as unhandled and terminated). The named verdict line is the contract.
static void eh_run_unhandled_filter(const trt_ex_record& ex, const void* ctx) {
  char line[192];
  void* filter = g_unhandled_filter.load();
  if (!filter) {
    snprintf(line, sizeof(line),
             "[trt] eh: unhandled exception 0x%08x @ %p -> filter no-filter",
             ex.code, (void*)(uintptr_t)ex.address);
    boot_log(line);
    return;
  }
  trt_ex_pointers ep{};
  ep.exception_record = (u64)(uintptr_t)&ex;
  ep.context_record = (u64)(uintptr_t)ctx;
  const i32 verdict = (i32)trt_call_guest(filter, (u64)(uintptr_t)&ep, 0, 0, 0);
  snprintf(line, sizeof(line),
           "[trt] eh: unhandled exception 0x%08x @ %p -> filter %s",
           ex.code, (void*)(uintptr_t)ex.address,
           eh_filter_verdict_name(verdict));
  boot_log(line);
}

extern "C" __attribute__((used)) void trt_raise_exception_sysv(
    u32 code, u32 flags, u32 count, const u64* args, const u8* caller_ctx_in) {
  char line[192];
  // MS_VC_EXCEPTION (0x406D1388): the legacy SetThreadName trick. Real
  // Windows ignores this exception when no debugger is attached (documented:
  // "without a debugger, the exception is ignored") — Unity raises it
  // unconditionally for every worker thread (measured: RACCOIN exit 42,
  // code=0x406d1388 flags=0 count=3). Swallow it the way the OS does and
  // resume at RaiseException's caller.
  if (code == 0x406d1388u) {
    snprintf(line, sizeof(line),
             "kernel32: RaiseException(0x406d1388) thread-name exception "
             "swallowed (no debugger attached)");
    boot_log(line);
    return;
  }
  snprintf(line, sizeof(line),
           "kernel32: RaiseException(code=0x%08x flags=%u count=%u arg0=0x%llx)",
           code, flags, count,
           (count && args) ? (unsigned long long)args[0] : 0ull);
  boot_log(line);

  u8 ctx[k_ctx_size];
  memcpy(ctx, caller_ctx_in, k_ctx_size);
  memset(ctx, 0, 0x30);
  memset(ctx + 0x100, 0, 0xA0);
  memset(ctx + 0x2A0, 0, k_ctx_size - 0x2A0);
  const u32 init_flags = 0x10000Fu;
  const u32 init_mxcsr = 0x1F80u;
  memcpy(ctx + k_ctx_flags, &init_flags, 4);
  memcpy(ctx + 0x34, &init_mxcsr, 4);
  memcpy(ctx + 0x118, &init_mxcsr, 4);

  trt_ex_record ex = {};
  ex.code = code;
  ex.flags = flags;
  ex.address = ctx_rip(ctx);
  ex.num_params = (count > 15u) ? 15u : count;
  if (args) {
    for (u32 i = 0; i < ex.num_params; ++i) ex.info[i] = args[i];
  }

  if (dispatch_exception_ctx(&ex, ctx, !(flags & 1u), false)) {
    unblock_fault_signals();
    t_hw_fault_depth = 0;
    trt_restore_win64_context(ctx);
    return;
  }

  // Unhandled MSVC C++ exception from an IL2CPP game: ask the game's own
  // runtime what managed exception it was (diagnostic). MSVC x64 C++ EH
  // parameter contract ( RaiseExceptionReference ):
  //   info[0] = 0x19930520 magic, info[1] = ThrowInfo* (static image data),
  //   info[2] = thrown object (Il2CppExceptionWrapper*, whose first field is
  //   the Il2CppException*). Only deref pointers that live inside a mapped
  //   guest image — a foreign or host-side value is never touched.
  static bool s_formatting = false;
  if (code == 0xe06d7363u && ex.num_params >= 3 && ex.info[2] &&
      is_guest_exec_pc(ex.info[2]) && !s_formatting) {
    s_formatting = true;
    void* ga = trt_GetModuleHandleA("GameAssembly.dll");
    void* fmt = ga ? trt_GetProcAddress(ga, "il2cpp_format_exception") : nullptr;
    if (fmt) {
      static char buf[1536];
      memset(buf, 0, sizeof(buf));
      // Il2CppExceptionWrapper { void* ex; ... }: guarded read — the wrapper
      // must itself point into a guest image before we let guest code see it.
      u64 managed = 0;
      const u64 wrapper = ex.info[2];
      trt_image* wimg = find_guest_image_for_pc(wrapper);
      if (wimg && wrapper + sizeof(u64) < (u64)(uintptr_t)wimg->base + wimg->size_of_image) {
        memcpy(&managed, (const void*)(uintptr_t)wrapper, sizeof(u64));
      }
      if (managed && is_guest_exec_pc(managed)) {
        trt_call_guest(fmt, managed, (u64)(uintptr_t)buf, (u64)sizeof(buf) - 1, 0);
        for (char* c = buf; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
        snprintf(line, sizeof(line), "kernel32: unhandled IL2CPP exception: %.150s", buf);
        boot_log(line);
        if (strlen(buf) > 150) {
          snprintf(line, sizeof(line), "kernel32:   ...%.150s", buf + 150);
          boot_log(line);
        }
        void* fst = trt_GetProcAddress(ga, "il2cpp_format_stack_trace");
        if (fst) {
          memset(buf, 0, sizeof(buf));
          trt_call_guest(fst, managed, (u64)(uintptr_t)buf, (u64)sizeof(buf) - 1, 0);
          for (char* c = buf; *c; ++c) if (*c == '\n' || *c == '\r') *c = '|';
          for (size_t off = 0; off < 900 && buf[off]; off += 170) {
            snprintf(line, sizeof(line), "kernel32: IL2CPP stack: %.170s", buf + off);
            boot_log(line);
          }
        }
      }
    }
  }
  // Measured topology (raccoin-ehfix9/10): when the throw happens inside a
  // funclet resumed at the host→guest boundary, the search exits the guest
  // images because above the handler owner sits only host code — the
  // machine-unwind's outward slot is garbage (outward_slot=0x...000001/2),
  // so there is no guest context to splice onto. On Windows this escape never
  // reaches RaiseException's caller: the native script-boundary wrapper
  // (UnityPlayer) sits between the native caller and GameAssembly and catches
  // the C++ exception there. Our bridge invoked the guest directly, so that
  // wrapper is not on the stack. Emulating it by resuming "after the throw
  // call" is WRONG (the helper is noreturn — it executes garbage and faults,
  // measured raccoin-ehfix10): the correct boundary semantic is the
  // UnityPlayer native wrapper catch: report a synthetic error return at the
  // host→guest call boundary via the per-thread escape pad (setjmp in
  // trt_call_guest_img) — no host frames are unwound and no guest frames are
  // faked; the boundary caller receives a 0 error return.
  if (code == 0xe06d7363u && ex.num_params >= 3 && ex.info[0] == 0x19930520u &&
      g_eh_escape.active && t_walk_hit_splice) {
    snprintf(line, sizeof(line),
             "kernel32: guest C++ exception escaped guest images — swallowed at "
             "host->guest call boundary (UnityPlayer wrapper-catch emulation)");
    boot_log(line);
    longjmp(g_eh_escape.jb, 1);
  }
  // Sprint 3 item 46: the guest's last-chance filter gets its one look at the
  // record BEFORE the runtime terminates (SetUnhandledExceptionFilter
  // semantics: the filter runs, then the process dies either way).
  eh_run_unhandled_filter(ex, ctx);
  if (trt_boot_raise_trap) trt_boot_raise_trap(code);
  boot_log("kernel32: RaiseException unhandled -> terminating");
  _exit(42);
}

#define TRT_CAPTURE_WIN64_REGS_ASM \
  "pushfq\n" \
  "subq $0x4E0, %rsp\n" \
  "movq %rax, 0x78(%rsp)\n" \
  "movq %rcx, 0x80(%rsp)\n" \
  "movq %rdx, 0x88(%rsp)\n" \
  "movq %rbx, 0x90(%rsp)\n" \
  "leaq 0x4F0(%rsp), %rax\n" \
  "movq %rax, 0x98(%rsp)\n" \
  "movq %rbp, 0xA0(%rsp)\n" \
  "movq %rsi, 0xA8(%rsp)\n" \
  "movq %rdi, 0xB0(%rsp)\n" \
  "movq %r8,  0xB8(%rsp)\n" \
  "movq %r9,  0xC0(%rsp)\n" \
  "movq %r10, 0xC8(%rsp)\n" \
  "movq %r11, 0xD0(%rsp)\n" \
  "movq %r12, 0xD8(%rsp)\n" \
  "movq %r13, 0xE0(%rsp)\n" \
  "movq %r14, 0xE8(%rsp)\n" \
  "movq %r15, 0xF0(%rsp)\n" \
  "movq 0x4E8(%rsp), %rax\n" \
  "movq %rax, 0xF8(%rsp)\n" \
  "movl 0x4E0(%rsp), %eax\n" \
  "movl %eax, 0x44(%rsp)\n" \
  "movups %xmm0,  0x1A0(%rsp)\n" \
  "movups %xmm1,  0x1B0(%rsp)\n" \
  "movups %xmm2,  0x1C0(%rsp)\n" \
  "movups %xmm3,  0x1D0(%rsp)\n" \
  "movups %xmm4,  0x1E0(%rsp)\n" \
  "movups %xmm5,  0x1F0(%rsp)\n" \
  "movups %xmm6,  0x200(%rsp)\n" \
  "movups %xmm7,  0x210(%rsp)\n" \
  "movups %xmm8,  0x220(%rsp)\n" \
  "movups %xmm9,  0x230(%rsp)\n" \
  "movups %xmm10, 0x240(%rsp)\n" \
  "movups %xmm11, 0x250(%rsp)\n" \
  "movups %xmm12, 0x260(%rsp)\n" \
  "movups %xmm13, 0x270(%rsp)\n" \
  "movups %xmm14, 0x280(%rsp)\n" \
  "movups %xmm15, 0x290(%rsp)\n"

#define TRT_RESTORE_WIN64_REGS_ASM \
  "movq 0x90(%rsp), %rbx\n" \
  "movq 0xA0(%rsp), %rbp\n" \
  "movq 0xA8(%rsp), %rsi\n" \
  "movq 0xB0(%rsp), %rdi\n" \
  "movq 0xD8(%rsp), %r12\n" \
  "movq 0xE0(%rsp), %r13\n" \
  "movq 0xE8(%rsp), %r14\n" \
  "movq 0xF0(%rsp), %r15\n" \
  "movups 0x200(%rsp), %xmm6\n" \
  "movups 0x210(%rsp), %xmm7\n" \
  "movups 0x220(%rsp), %xmm8\n" \
  "movups 0x230(%rsp), %xmm9\n" \
  "movups 0x240(%rsp), %xmm10\n" \
  "movups 0x250(%rsp), %xmm11\n" \
  "movups 0x260(%rsp), %xmm12\n" \
  "movups 0x270(%rsp), %xmm13\n" \
  "movups 0x280(%rsp), %xmm14\n" \
  "movups 0x290(%rsp), %xmm15\n"

__attribute__((naked, used)) void TRT_STDCALL trt_RtlUnwindEx(
    u64 target, u64 target_ip, void* exrec, u64 retval, void* ctx,
    void* history) {
  asm volatile(
      TRT_CAPTURE_WIN64_REGS_ASM
      "movq %rsp, %r10\n"
      "movq 0x80(%r10), %rdi\n"
      "movq 0x88(%r10), %rsi\n"
      "movq 0xB8(%r10), %rdx\n"
      "movq 0xC0(%r10), %rcx\n"
      "movq 0x510(%r10), %r8\n"
      "movq 0x518(%r10), %r9\n"
      "subq $16, %rsp\n"
      "movq %r10, (%rsp)\n"
      "call trt_rtl_unwind_ex_sysv\n"
      "addq $16, %rsp\n"
      TRT_RESTORE_WIN64_REGS_ASM
      "addq $0x4E0, %rsp\n"
      "popfq\n"
      "ret\n");
}

__attribute__((naked, used)) void TRT_STDCALL trt_RtlUnwind(
    u64 target, u64 target_ip, void* exrec, u64 retval) {
  asm volatile(
      TRT_CAPTURE_WIN64_REGS_ASM
      "movq %rsp, %r10\n"
      "movq 0x80(%r10), %rdi\n"
      "movq 0x88(%r10), %rsi\n"
      "movq 0xB8(%r10), %rdx\n"
      "movq 0xC0(%r10), %rcx\n"
      "xorq %r8, %r8\n"
      "xorq %r9, %r9\n"
      "subq $16, %rsp\n"
      "movq %r10, (%rsp)\n"
      "call trt_rtl_unwind_ex_sysv\n"
      "addq $16, %rsp\n"
      TRT_RESTORE_WIN64_REGS_ASM
      "addq $0x4E0, %rsp\n"
      "popfq\n"
      "ret\n");
}

__attribute__((naked, used)) void TRT_STDCALL trt_RaiseException(
    u32 code, u32 flags, u32 count, u64* args) {
  asm volatile(
      TRT_CAPTURE_WIN64_REGS_ASM
      "movq %rsp, %r8\n"
      "movl 0x80(%r8), %edi\n"
      "movl 0x88(%r8), %esi\n"
      "movl 0xB8(%r8), %edx\n"
      "movq 0xC0(%r8), %rcx\n"
      "call trt_raise_exception_sysv\n"
      TRT_RESTORE_WIN64_REGS_ASM
      "addq $0x4E0, %rsp\n"
      "popfq\n"
      "ret\n");
}
#undef TRT_RESTORE_WIN64_REGS_ASM
#undef TRT_CAPTURE_WIN64_REGS_ASM
#else
void TRT_STDCALL trt_RtlUnwindEx(u64 target, u64 target_ip, void* exrec,
                                 u64 retval, void* ctx, void* history) {
#ifdef _WIN32
  ::RtlUnwindEx((PVOID)(uintptr_t)target, (PVOID)(uintptr_t)target_ip,
                (PEXCEPTION_RECORD)exrec, (PVOID)(uintptr_t)retval,
                (PCONTEXT)ctx, (PUNWIND_HISTORY_TABLE)history);
#else
  (void)target; (void)target_ip; (void)exrec; (void)retval; (void)ctx;
  (void)history;
#endif
}

void TRT_STDCALL trt_RtlUnwind(u64 target, u64 target_ip, void* exrec,
                               u64 retval) {
#ifdef _WIN32
  ::RtlUnwind((PVOID)(uintptr_t)target, (PVOID)(uintptr_t)target_ip,
              (PEXCEPTION_RECORD)exrec, (PVOID)(uintptr_t)retval);
#else
  (void)target; (void)target_ip; (void)exrec; (void)retval;
#endif
}

extern "C" int trt_run_guest_veh(void* ep) {
  if (!ep) return 0;
  std::vector<void*> vehs;
  veh_snapshot_ordered(&vehs);
  for (void* vfn : vehs) {
    const i32 vres = (i32)trt_call_guest(vfn, (u64)(uintptr_t)ep, 0, 0, 0);
    if (vres == -1 /*EXCEPTION_CONTINUE_EXECUTION*/) return -1;
  }
  return 0;
}

extern "C" int trt_dispatch_hw_fault(int sig, void* siginfo_v, void* uctx_v) {
  (void)sig; (void)siginfo_v; (void)uctx_v;
  return 0;
}

extern "C" int TRT_STDCALL trt_dispatch_win32_exception(void* ep_v) {
  if (!ep_v) return 0;
#ifdef _WIN32
  auto* ep = static_cast<PEXCEPTION_POINTERS>(ep_v);
  const auto* record = ep->ExceptionRecord;
  const auto* context = ep->ContextRecord;
  if (!record || !is_guest_exec_pc((u64)(uintptr_t)record->ExceptionAddress))
    return 0;
  const bool access_fault =
      context && record->ExceptionCode == 0xC0000005u && record->NumberParameters >= 2;
  const u32 access = access_fault ? static_cast<u32>(record->ExceptionInformation[0]) : 0;
  const u64 address = access_fault ? record->ExceptionInformation[1] : 0;
  if (access_fault && access == 8 && trt_va_try_promote_rx(address)) return -1;

  const u64 pc = context ? context->Rip : 0;
  const u64 stack = context ? context->Rsp : 0;
  const int result = trt_run_guest_veh(ep_v);
  if (result != -1 || !access_fault || context->Rip != pc || context->Rsp != stack) {
    t_fault_retry = {};
  } else if (guest_fault_retry_exhausted(pc, stack, address, record->ExceptionCode, access)) {
    boot_log("kernel32: repeated guest access violation made no progress");
    return 0;
  }
  return result;
#else
  return trt_run_guest_veh(ep_v);
#endif
}

#ifdef _WIN32
// .216 DIAG: real-exit hook state -- INT3 patches on the REAL ExitProcess /
// TerminateProcess / FatalExit in kernel32.dll + kernelbase.dll. If Sekiro
// exits via a PEB-walked or direct real call (bypassing the bridge trt_
// imports, which it binds but never calls), the breakpoint fires here with
// the caller RIP. One shot per target: the byte is restored on first fire.
struct real_exit_target {
  const char* name;
  void* addr;
  u8 orig;
  bool armed;
};
static real_exit_target g_real_exit_targets[8];
static int g_real_exit_armed = 0;

// .219 DIAG: syscall trap -- INT3 patches on the most likely ntdll /
// kernel32 wait functions Sekiro could be spinning on via a PEB-walked /
// direct call (the .218 gap trace shows the guest alive and spinning
// with zero bridged calls after the last SetEvent). Same one-shot
// INT3+VEH mechanism as the .216 real-exit hook. Armed LAZILY by the
// gap-trace final dump (2s of wall-time quiet) -- arming at launch
// would trip on every host Sleep/wait instead. The VEH only logs +
// disarms when the caller is inside a loaded guest image; a host
// caller (e.g. the watchdog's own Sleep) restores the byte and
// re-arms on the next watchdog tick, so the spinning guest -- calling
// at high frequency -- wins the race to fire first.
struct syscall_trap_target {
  const char* name;
  void* addr;
  u8 orig;
  bool armed;
  bool fired;
  // .220 DIAG: guest-fire count (diagnostic only since .222 --
  // the cap is per-caller now, the target always re-arms).
  int fires;
};
static syscall_trap_target g_syscall_trap_targets[12];
static std::atomic<bool> g_syscall_trap_rearm{false};
// .220 DIAG: trap flap counters (printed on every gap dump) + quiet
// re-arm flag. The watchdog's own Sleep trips the trap every tick,
// so each re-install is silent; only the launch arm logs.
static std::atomic<u64> g_syscall_trap_host_trips{0};
static std::atomic<u64> g_syscall_trap_rearms{0};
// .221 DIAG: VEH-race trips -- a second thread tripping a target
// whose VEH already set armed=false. Counted on the gap dump.
static std::atomic<u64> g_syscall_trap_races{0};
static bool g_syscall_trap_quiet_rearm = false;
// .221 DIAG: per-caller fire budgets -- max 3 fires per distinct
// guest return address per target, so one chatty caller (the
// AudioDriverWASAPI poll loop burned the whole cap in .220)
// cannot exhaust the trap. Small fixed table, guarded by a
// spinlock: the VEH runs on arbitrary threads, so no blocking
// lock and no allocation inside the handler. Fail-open: a full
// table or a lock timeout counts as under budget.
struct trap_caller_budget {
  void* target = nullptr;
  u64 caller = 0;
  int fires = 0;
  bool budget_logged = false;
};
static trap_caller_budget g_trap_caller_budgets[64];
static std::atomic_flag g_trap_budget_lock = ATOMIC_FLAG_INIT;
// Returns 0 = under budget (fire counted), 1 = just exhausted
// (caller should log BUDGET-EXHAUSTED once), 2 = already
// exhausted (silent).
static int trap_caller_note_fire(void* target, u64 caller) {
  int spins = 0;
  while (g_trap_budget_lock.test_and_set(std::memory_order_acquire)) {
    if (++spins > 10000) return 0;
  }
  int rc = 0;
  trap_caller_budget* slot = nullptr;
  trap_caller_budget* free_slot = nullptr;
  for (int i = 0; i < 64; ++i) {
    trap_caller_budget* e = &g_trap_caller_budgets[i];
    if (e->target == target && e->caller == caller) { slot = e; break; }
    if (!free_slot && !e->target) free_slot = e;
  }
  if (!slot) {
    if (!free_slot) {
      g_trap_budget_lock.clear(std::memory_order_release);
      return 0;
    }
    slot = free_slot;
    slot->target = target;
    slot->caller = caller;
    slot->fires = 0;
    slot->budget_logged = false;
  }
  ++slot->fires;
  if (slot->fires <= 3) rc = 0;
  else if (slot->fires == 4 && !slot->budget_logged) {
    slot->budget_logged = true;
    rc = 1;
  } else rc = 2;
  g_trap_budget_lock.clear(std::memory_order_release);
  return rc;
}
static bool caller_in_guest_image(u64 caller) {
  const int n = trt_images_count();
  for (int i = 0; i < n; ++i) {
    const trt_image* im = trt_images_nth(i);
    if (!im || !im->base || !im->size_of_image) continue;
    const u64 b = (u64)(uintptr_t)im->base;
    if (caller >= b && caller < b + im->size_of_image) return true;
  }
  return false;
}

// .224 DIAG: validate a level-1 return address as the call site
// of WorkerThreadPool::wait_for_group_task_completion: it must
// sit in a guest image AND the 5 bytes before it must be a
// direct relative call (E8 rel32) whose target is also in a
// guest image. Best effort: an indirect call or a tail jump
// fails validation (valid=0) and is simply not reported,
// never trusted. The addr-5 read is itself range-checked
// first, so it cannot fault on an image edge.
static bool guest_call_site_valid(u64 addr) {
  if (!addr || !caller_in_guest_image(addr)) return false;
  if (!caller_in_guest_image(addr - 5)) return false;
  const u8* p = (const u8*)(uintptr_t)(addr - 5);
  if (*p != 0xE8) return false;
  i32 rel = 0;
  memcpy(&rel, p + 1, sizeof(rel));
  const u64 target = addr + (i64)rel;
  return caller_in_guest_image(target);
}

// .224 DIAG: level-1 return address of the guest caller of a
// bridged CV wait, from the frame-pointer chain. fp is the
// frame pointer captured in the bridged wait wrapper
// (__builtin_frame_address(0)): when the wrapper omits its
// own frame pointer, fp is the guest caller frame and
// [fp+8] is its return address; when it keeps one, [fp] is
// the guest frame and [[fp]+8] is the return address. Every
// dereference is stack-bounds-checked
// (GetCurrentThreadStackLimits) and the result is validated
// by guest_call_site_valid before use; a missing frame chain
// (FPO guest code) just yields 0.
static u64 guest_caller2_from_fp(void* fp) {
  if (!fp) return 0;
  ULONG_PTR low = 0, high = 0;
  ::GetCurrentThreadStackLimits(&low, &high);
  const uintptr_t f = (uintptr_t)fp;
  if (f < (uintptr_t)low || f + 16 > (uintptr_t)high) return 0;
  const u64 cand_a = *(const u64*)(f + 8);
  if (guest_call_site_valid(cand_a)) return cand_a;
  const u64 saved = *(const u64*)f;
  if (saved < (uintptr_t)low || saved + 16 > (uintptr_t)high) return 0;
  const u64 cand_b = *(const u64*)(saved + 8);
  if (guest_call_site_valid(cand_b)) return cand_b;
  return 0;
}

static LONG CALLBACK trt_win32_os_veh(PEXCEPTION_POINTERS ep) {
  if (ep && ep->ExceptionRecord &&
      ep->ExceptionRecord->ExceptionCode == 0x80000003u /*EXCEPTION_BREAKPOINT*/) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    for (int i = 0; i < 8; ++i) {
      if (g_real_exit_targets[i].addr &&
          g_real_exit_targets[i].addr == addr) {
        // .221 DIAG: VEH RACE FIX -- a second thread can trip
        // the same target while the first trip's VEH is still
        // restoring the byte (armed already false). The old
        // code only handled armed targets, so the second trip
        // fell through to the host unhandled filter and
        // killed the app (Ultrapool .220: HOST UNHANDLED
        // 0x80000003 at ntdll!NtDelayExecution). armed gates
        // INSTALLATION only, never VEH handling.
        const bool was_armed = g_real_exit_targets[i].armed;
        u64 caller = 0;
        if (ep->ContextRecord) {
          const u64 rsp = (u64)ep->ContextRecord->Rsp;
          if (rsp) caller = *(const u64*)(uintptr_t)rsp;
        }
        if (!was_armed) {
          char rl[192];
          snprintf(rl, sizeof(rl), "real-exit: RACE %s called from 0x%llx",
                   g_real_exit_targets[i].name, (unsigned long long)caller);
          trt_core_log(rl);
        } else {
          char line[192];
          snprintf(line, sizeof(line), "real-exit: %s called from 0x%llx",
                   g_real_exit_targets[i].name, (unsigned long long)caller);
          trt_core_log(line);
        }
        // Restore the original byte and re-execute the real function.
        // NOTE: at an INT3 the context RIP points past the breakpoint
        // byte, so reset it to the patched address or the first
        // instruction would be entered mid-opcode.
        DWORD old_prot = 0;
        if (::VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
          *(u8*)addr = g_real_exit_targets[i].orig;
          ::VirtualProtect(addr, 1, old_prot, &old_prot);
        }
        g_real_exit_targets[i].armed = false;
        if (ep->ContextRecord)
          ep->ContextRecord->Rip = (u64)(uintptr_t)addr;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
    // .220 DIAG: syscall trap -- same INT3+VEH mechanism on the
    // ntdll/kernel32 wait functions (see trt_syscall_trap_install).
    // Guest caller: log + count the fire. The target always
    // re-arms (via the watchdog tick); the cap is per-caller
    // (3 fires each, .221 budgets, made per-caller-only in
    // .222), so a startup/init burn cannot kill the trap
    // before the death-phase wait. Host caller: restore the
    // byte so the host proceeds, count the trip, re-arm on
    // the next watchdog tick.
    for (int i = 0; i < 12; ++i) {
      if (g_syscall_trap_targets[i].addr &&
          g_syscall_trap_targets[i].addr == addr) {
        // .221 DIAG: VEH RACE FIX -- handle ANY breakpoint at
        // a known trap address regardless of armed (see the
        // real-exit loop above). A second host thread
        // tripping after the first trip's VEH set armed=false
        // used to fall through to the host unhandled filter
        // and kill the app (Ultrapool .220).
        const bool was_armed = g_syscall_trap_targets[i].armed;
        if (!was_armed) {
          g_syscall_trap_races.fetch_add(1, std::memory_order_relaxed);
        }
        u64 caller = 0;
        if (ep->ContextRecord) {
          const u64 rsp = (u64)ep->ContextRecord->Rsp;
          if (rsp) caller = *(const u64*)(uintptr_t)rsp;
        }
        if (caller_in_guest_image(caller)) {
          // .221 DIAG: per-caller fire budget -- one caller
          // (e.g. the audio poll loop) must not burn the
          // target's 3-fire budget. A caller over its 3 fires
          // logs BUDGET-EXHAUSTED once and no longer counts
          // toward the target's fires; the trap still re-arms
          // so other callers keep coverage.
          const int budget = trap_caller_note_fire(
              g_syscall_trap_targets[i].addr, caller);
          if (budget == 1) {
            char bl[192];
            snprintf(bl, sizeof(bl),
                     "syscall-trap: BUDGET-EXHAUSTED %s caller=0x%llx",
                     g_syscall_trap_targets[i].name,
                     (unsigned long long)caller);
            trt_core_log(bl);
            g_syscall_trap_rearm.store(true, std::memory_order_relaxed);
          } else if (budget == 0) {
            char line[192];
            snprintf(line, sizeof(line),
                     "syscall-trap: %s called from 0x%llx%s",
                     g_syscall_trap_targets[i].name,
                     (unsigned long long)caller,
                     was_armed ? "" : " RACE");
            trt_core_log(line);
            // .222 DIAG: the cap is per-caller-only now --
            // budgets are per-caller (trap_caller_note_fire,
            // 3 fires each), so the target-level 3-fire cap
            // that killed the trap in .220 is gone. The
            // target always re-arms; fires is kept as a
            // diagnostic count only.
            ++g_syscall_trap_targets[i].fires;
            g_syscall_trap_rearm.store(true, std::memory_order_relaxed);
          } else {
            g_syscall_trap_rearm.store(true, std::memory_order_relaxed);
          }
        } else {
          g_syscall_trap_host_trips.fetch_add(1, std::memory_order_relaxed);
          g_syscall_trap_rearm.store(true, std::memory_order_relaxed);
        }
        DWORD old_prot = 0;
        if (::VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
          *(u8*)addr = g_syscall_trap_targets[i].orig;
          ::VirtualProtect(addr, 1, old_prot, &old_prot);
        }
        g_syscall_trap_targets[i].armed = false;
        if (ep->ContextRecord)
          ep->ContextRecord->Rip = (u64)(uintptr_t)addr;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }
  return (LONG)trt_dispatch_win32_exception((void*)ep);
}

// .217 DIAG: install the real-exit INT3 hook (8 targets: kernel32 /
// kernelbase / ntdll). Called from the host guest-launch path before guest
// code runs, once per guest launch. Also registers the OS VEH
// above directly, so the hook works even if the guest never calls
// AddVectoredExceptionHandler.
extern "C" void trt_real_exit_hook_install() {
  static const struct {
    const wchar_t* mod;
    const char* fn;
    const char* name;
  } k_targets[8] = {
      {L"kernel32.dll", "ExitProcess", "kernel32!ExitProcess"},
      {L"kernel32.dll", "TerminateProcess", "kernel32!TerminateProcess"},
      {L"kernel32.dll", "FatalExit", "kernel32!FatalExit"},
      {L"kernelbase.dll", "ExitProcess", "kernelbase!ExitProcess"},
      {L"kernelbase.dll", "TerminateProcess", "kernelbase!TerminateProcess"},
      {L"kernelbase.dll", "FatalExit", "kernelbase!FatalExit"},
      {L"ntdll.dll", "RtlExitUserProcess", "ntdll!RtlExitUserProcess"},
      {L"ntdll.dll", "NtTerminateProcess", "ntdll!NtTerminateProcess"},
  };
  static std::once_flag s_real_exit_veh_once;
  std::call_once(s_real_exit_veh_once, []() {
    using pfn_add_veh = PVOID (WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = ::GetModuleHandleW(L"kernel32.dll");
    if (kb) {
      auto fn = (pfn_add_veh)::GetProcAddress(kb, "AddVectoredExceptionHandler");
      if (fn) fn(1u, &trt_win32_os_veh);
    }
  });
  int installed = 0;
  for (int i = 0; i < 8; ++i) {
    HMODULE mod = ::GetModuleHandleW(k_targets[i].mod);
    if (!mod) {
      char line[192];
      snprintf(line, sizeof(line), "real-exit: %s SKIP module not loaded",
               k_targets[i].name);
      trt_core_log(line);
      continue;
    }
    void* addr = (void*)::GetProcAddress(mod, k_targets[i].fn);
    if (!addr) {
      char line[192];
      snprintf(line, sizeof(line), "real-exit: %s SKIP GetProcAddress null",
               k_targets[i].name);
      trt_core_log(line);
      continue;
    }
    // .217 FIX: never re-patch an already-armed target. On a second guest
    // launch in the same process the old code saved orig=0xCC (the INT3
    // byte itself) and re-armed; the VEH would then restore 0xCC on fire
    // and the breakpoint would never clear. Skip armed targets entirely.
    if (g_real_exit_targets[i].armed && g_real_exit_targets[i].addr == addr) {
      char line[192];
      snprintf(line, sizeof(line), "real-exit: %s at 0x%llx SKIP already-armed",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      trt_core_log(line);
      continue;
    }
    if (*(u8*)addr == 0xCC) {
      // A foreign patch already sits here; saving it as orig would corrupt
      // the restore. Do not touch this target.
      char line[192];
      snprintf(line, sizeof(line), "real-exit: %s at 0x%llx SKIP already 0xCC",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      trt_core_log(line);
      continue;
    }
    DWORD old_prot = 0;
    if (!::VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
      char line[192];
      snprintf(line, sizeof(line),
               "real-exit: %s at 0x%llx SKIP VirtualProtect failed err=%u",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr,
               (unsigned)::GetLastError());
      trt_core_log(line);
      continue;
    }
    g_real_exit_targets[i].name = k_targets[i].name;
    g_real_exit_targets[i].addr = addr;
    g_real_exit_targets[i].orig = *(u8*)addr;
    g_real_exit_targets[i].armed = true;
    *(u8*)addr = 0xCC;  // INT3
    ::VirtualProtect(addr, 1, old_prot, &old_prot);
    ++installed;
    {
      char line[192];
      snprintf(line, sizeof(line), "real-exit: %s at 0x%llx installed",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      trt_core_log(line);
    }
  }
  // Recompute the armed total: re-installs mostly SKIP, so the newly
  // installed count alone understates coverage.
  int armed = 0;
  for (int i = 0; i < 8; ++i) if (g_real_exit_targets[i].armed) ++armed;
  g_real_exit_armed = armed;
  {
    char line[128];
    snprintf(line, sizeof(line), "real-exit: hook installed %d/8 (armed %d)",
             installed, armed);
    trt_core_log(line);
  }
}

// .220 DIAG: install the syscall trap (8 targets). Called at LAUNCH
// from run_guest_trapped (main.cpp, snapshot-only) and from the
// first gap-trace dump, and from the watchdog tick when the VEH
// asked for a re-arm (host caller restored a byte). Idempotent:
// already-armed targets are skipped, fired targets stay dead.
// The OS VEH is already registered by trt_real_exit_hook_install
// (called at every guest launch before guest code runs). The 2
// SleepConditionVariable targets are new in .220: the guest's
// terminal wait is most plausibly a CV wait (Ultrapool's stall
// IS one), and the .219 6-target trap could not catch it.
// Quiet re-arms: when g_syscall_trap_quiet_rearm is set (watchdog
// re-arm after a host trip), install silently -- the watchdog's
// own Sleep trips the trap every tick, and 7 log lines per tick
// would drown the log. Only the launch arm logs.
extern "C" void trt_syscall_trap_install() {
#if !defined(TRT_ENABLE_SYSCALL_TRAP_DIAGNOSTICS)
  // This diagnostic patches shared native wait entry points. With host
  // threads entering those functions concurrently, the handler and watchdog
  // can race over page protection and fault while writing the breakpoint.
  // Keep the instrumentation available for dedicated debug builds.
  return;
#endif
  const bool quiet = g_syscall_trap_quiet_rearm;
  g_syscall_trap_quiet_rearm = false;
#define TRAP_LOG(line) do { if (!quiet) trt_core_log(line); } while (0)
  static const struct {
    const wchar_t* mod;
    const char* fn;
    const char* name;
  } k_targets[12] = {
      {L"kernel32.dll", "Sleep", "kernel32!Sleep"},
      {L"kernel32.dll", "WaitForSingleObject", "kernel32!WaitForSingleObject"},
      {L"kernel32.dll", "WaitForMultipleObjects", "kernel32!WaitForMultipleObjects"},
      // .223 DIAG: the message-pump wait -- a game main
      // thread that MsgWaits on its window queue never
      // trips the WaitFor* targets, so Sekiro death-by-
      // message-wait was invisible to the trap.
      // .224 FIX: MsgWaitForMultipleObjects lives in
      // user32.dll, NOT kernel32.dll -- the .223 target
      // resolved GetProcAddress null and never armed.
      // Ex variant added as a second target.
      {L"user32.dll", "MsgWaitForMultipleObjects", "user32!MsgWaitForMultipleObjects"},
      {L"user32.dll", "MsgWaitForMultipleObjectsEx", "user32!MsgWaitForMultipleObjectsEx"},
      {L"kernel32.dll", "SwitchToThread", "kernel32!SwitchToThread"},
      {L"kernel32.dll", "SleepConditionVariableSRW", "kernel32!SleepConditionVariableSRW"},
      {L"kernel32.dll", "SleepConditionVariableCS", "kernel32!SleepConditionVariableCS"},
      {L"ntdll.dll", "NtWaitForSingleObject", "ntdll!NtWaitForSingleObject"},
      {L"ntdll.dll", "NtWaitForMultipleObjects", "ntdll!NtWaitForMultipleObjects"},
      {L"ntdll.dll", "NtDelayExecution", "ntdll!NtDelayExecution"},
      {L"ntdll.dll", "NtYieldExecution", "ntdll!NtYieldExecution"},
  };
  int installed = 0;
  for (int i = 0; i < 12; ++i) {
    if (g_syscall_trap_targets[i].fired) continue;
    HMODULE mod = ::GetModuleHandleW(k_targets[i].mod);
    // .224 FIX: user32.dll may not be loaded yet -- a plain
    // GetModuleHandleW miss used to SKIP the target. Load it
    // (no-op refcount bump when already loaded) before giving up.
    if (!mod) mod = ::LoadLibraryW(k_targets[i].mod);
    if (!mod) {
      char line[192];
      snprintf(line, sizeof(line), "syscall-trap: %s SKIP module not loaded",
               k_targets[i].name);
      TRAP_LOG(line);
      continue;
    }
    void* addr = (void*)::GetProcAddress(mod, k_targets[i].fn);
    if (!addr) {
      char line[192];
      snprintf(line, sizeof(line), "syscall-trap: %s SKIP GetProcAddress null",
               k_targets[i].name);
      TRAP_LOG(line);
      continue;
    }
    if (g_syscall_trap_targets[i].armed &&
        g_syscall_trap_targets[i].addr == addr) {
      char line[192];
      snprintf(line, sizeof(line),
               "syscall-trap: %s at 0x%llx SKIP already-armed",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      TRAP_LOG(line);
      continue;
    }
    if (*(u8*)addr == 0xCC) {
      char line[192];
      snprintf(line, sizeof(line), "syscall-trap: %s at 0x%llx SKIP already 0xCC",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      TRAP_LOG(line);
      continue;
    }
    DWORD old_prot = 0;
    if (!::VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
      char line[192];
      snprintf(line, sizeof(line),
               "syscall-trap: %s at 0x%llx SKIP VirtualProtect failed err=%u",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr,
               (unsigned)::GetLastError());
      TRAP_LOG(line);
      continue;
    }
    g_syscall_trap_targets[i].name = k_targets[i].name;
    g_syscall_trap_targets[i].addr = addr;
    g_syscall_trap_targets[i].orig = *(u8*)addr;
    g_syscall_trap_targets[i].armed = true;
    *(u8*)addr = 0xCC;  // INT3
    ::VirtualProtect(addr, 1, old_prot, &old_prot);
    ++installed;
    {
      char line[192];
      snprintf(line, sizeof(line), "syscall-trap: %s at 0x%llx installed",
               k_targets[i].name, (unsigned long long)(uintptr_t)addr);
      TRAP_LOG(line);
    }
  }
  int armed = 0;
  for (int i = 0; i < 12; ++i) if (g_syscall_trap_targets[i].armed) ++armed;
  {
    char line[128];
    snprintf(line, sizeof(line), "syscall-trap: hook installed %d/12 (armed %d)",
             installed, armed);
    TRAP_LOG(line);
  }
#undef TRAP_LOG
}
#endif

// .249 DIAG: guarded guest reads + MSVC C++ exception type resolution.
// Mirrors spin_read_qword_guarded (bridge_thread.cpp): the deref goes
// through a noinline call inside a top-level __try, so a fault in the
// callee unwinds to this scope instead of faulting the host. A try body
// with no call does not survive this build optimizer (.241 lesson).
static __attribute__((noinline)) u64 k32_deref_qword_249(u64 addr) {
  return *(const u64*)(uintptr_t)addr;
}

static __attribute__((noinline)) int k32_read_qword_guarded_249(u64 addr,
                                                                u64* out) {
#if defined(_WIN32)
  int ok = 0;
  __try {
    *out = k32_deref_qword_249(addr);
    ok = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  return ok;
#else
  (void)addr;
  (void)out;
  return 0;
#endif
}

// Read a NUL-terminated guest string, qword-wise through the guarded
// reader, into buf (truncated at cap-1). Returns bytes copied.
static int k32_read_guest_str_249(u64 addr, char* buf, size_t cap) {
  if (!addr || !buf || cap < 2) return 0;
  size_t n = 0;
  const size_t max_qw = (cap - 1) / 8;
  for (size_t q = 0; q < max_qw; ++q) {
    u64 qw = 0;
    if (!k32_read_qword_guarded_249(addr + q * 8, &qw)) break;
    for (int b = 0; b < 8; ++b) {
      if (n + 1 >= cap) {
        buf[cap - 1] = '\0';
        return (int)(cap - 1);
      }
      char c = (char)((qw >> (b * 8)) & 0xff);
      if (c == '\0') {
        buf[n] = '\0';
        return (int)n;
      }
      buf[n++] = c;
    }
  }
  buf[n] = '\0';
  return (int)n;
}

// Resolve the MSVC C++ exception type name from the RaiseException args
// (0xe06d7363: [0]=magic 0x19930520, [1]=exception object,
// [2]=_ThrowInfo, [3]=image base). The x64 EH data uses image-relative
// RVAs, resolved against [3]; each level also tries the direct-pointer
// reading, and the final name must start with ".?A" (MSVC mangled type
// name) to count as resolved. Returns true and fills name_buf on
// success; ntypes_out (when non-null) gets the catchable-type count.
static bool k32_cpp_eh_type_name_249(const u64* args, u32 count,
                                     char* name_buf, size_t name_cap,
                                     u32* ntypes_out) {
  if (!args || count < 3 || args[0] != 0x19930520ull) return false;
  const u64 ti = args[2];
  const u64 img = (count >= 4) ? args[3] : 0;
  if (!ti) return false;
  u64 cta_qw = 0;
  // .251 DIAG: pCatchableTypeArray is a 32-bit RVA at ti+12 (the x64
  // image _ThrowInfo is 16 bytes: attributes @0, pfnUnwind @4,
  // pfnForwardCompat @8, pCatchableTypeArray @12). The .249 code read
  // ti+24 -- past the struct -- so it always failed (ntypes=0 in both
  // .250 runs). Dump the raw ThrowInfo qwords first (guarded), then
  // take the RVA from ti+12 (high 32 bits of the ti+8 qword); fall
  // back to the .249 ti+24 reading if it is 0.
  {
    u64 qw0 = 0, qw1 = 0, qw2 = 0, qw3 = 0;
    (void)k32_read_qword_guarded_249(ti, &qw0);
    (void)k32_read_qword_guarded_249(ti + 8, &qw1);
    (void)k32_read_qword_guarded_249(ti + 16, &qw2);
    (void)k32_read_qword_guarded_249(ti + 24, &qw3);
    char dl[256];
    snprintf(dl, sizeof(dl),
             "kernel32: C++ ThrowInfo dump ti=0x%llx img=0x%llx "
             "qw0=0x%llx qw1=0x%llx qw2=0x%llx qw3=0x%llx",
             (unsigned long long)ti, (unsigned long long)img,
             (unsigned long long)qw0, (unsigned long long)qw1,
             (unsigned long long)qw2, (unsigned long long)qw3);
    boot_log(dl);
    cta_qw = (u32)(qw1 >> 32);
    if (!cta_qw) {
      if (!k32_read_qword_guarded_249(ti + 24, &cta_qw)) return false;
    }
  }
  u64 cta = 0;
  u64 n_qw = 0;
  if (k32_read_qword_guarded_249(cta_qw, &n_qw) &&
      (u32)n_qw >= 1 && (u32)n_qw <= 64) {
    cta = cta_qw;
  } else if (img &&
             k32_read_qword_guarded_249(img + (u32)cta_qw, &n_qw) &&
             (u32)n_qw >= 1 && (u32)n_qw <= 64) {
    cta = img + (u32)cta_qw;
  } else {
    return false;
  }
  const u32 ntypes = (u32)n_qw;
  if (ntypes_out) *ntypes_out = ntypes;
  u64 e_qw = 0;
  if (!k32_read_qword_guarded_249(cta + 4, &e_qw)) return false;
  u64 ct = 0;
  u64 probe = 0;
  if (img && k32_read_qword_guarded_249(img + (u32)e_qw, &probe)) {
    ct = img + (u32)e_qw;
  } else if (k32_read_qword_guarded_249(e_qw, &probe)) {
    ct = e_qw;
  } else {
    return false;
  }
  u64 t_qw = 0;
  if (!k32_read_qword_guarded_249(ct + 4, &t_qw)) return false;
  // .251 DIAG: the type name lives INLINE in the _TypeDescriptor at
  // pType+16 (pVFTable @0, spare @8, name[] @16). The .249 code read
  // a pointer at +8 (the spare field, usually 0), so it never
  // resolved. Resolve pType as img+RVA (primary) or direct.
  {
    u64 ptype = 0;
    if (img && (u32)t_qw) {
      ptype = img + (u32)t_qw;
    } else if (t_qw) {
      ptype = t_qw;
    }
    if (ptype) {
      const int n = k32_read_guest_str_249(ptype + 16, name_buf, name_cap);
      if (n >= 4 && name_buf[0] == '.' && name_buf[1] == '?' &&
          name_buf[2] == 'A')
        return true;
    }
  }
  if (img) {
    u64 name_qw = 0;
    if (k32_read_qword_guarded_249(img + (u32)t_qw + 8, &name_qw) &&
        name_qw) {
      const int n = k32_read_guest_str_249(name_qw, name_buf, name_cap);
      if (n >= 4 && name_buf[0] == '.' && name_buf[1] == '?' &&
          name_buf[2] == 'A')
        return true;
    }
  }
  {
    u64 name_qw = 0;
    if (k32_read_qword_guarded_249(t_qw + 8, &name_qw) && name_qw) {
      const int n = k32_read_guest_str_249(name_qw, name_buf, name_cap);
      if (n >= 4 && name_buf[0] == '.' && name_buf[1] == '?' &&
          name_buf[2] == 'A')
        return true;
    }
  }
  return false;
}

// RaiseException: run registered guest VEHs first, then native OS RaiseException on UWP.
void TRT_STDCALL trt_RaiseException(u32 code, u32 flags, u32 count, u64* args) {
  // MS_VC_EXCEPTION (0x406D1388): the legacy SetThreadName trick. Real
  // Windows ignores this exception when no debugger is attached; swallow
  // it here the way the SysV path does, instead of dispatching it through
  // the real ::RaiseException (which faults the guest in ntdll EH dispatch).
  // The VEH fix (.195) + Sleep(1) (.196) + Sleep(50) (.198) all failed to
  // fix the +0x115c66b fault, so timing/VEH side effects are ruled out.
  // If Sekiro passes +0x115c66b with the swallow disabled, the swallow
  // itself is the cause (the missing piece is then the guest __except
  // handler, which the VEH fix does not invoke). If it still faults,
  // the swallow is exonerated. Re-enable in .200 with the real fix.
  if (code == 0x406d1388u) {
    boot_log("kernel32: RaiseException(0x406d1388) thread-name exception "
             "swallowed (no debugger attached)");
#ifdef _WIN32
    // Run registered guest VEHs before swallowing: a guest VEH may do
    // per-thread init on the thread-name exception (the .193 regression:
    // skipping dispatch entirely also skipped guest VEH side effects the
    // game depends on before sekiro.exe+0x115c66b). Mirrors the
    // non-swallow path below; the OS dispatch itself is still skipped
    // (it faults in ntdll EH dispatch on this console).
    std::vector<void*> vehs;
    veh_snapshot_ordered(&vehs);
    if (!vehs.empty() && !(flags & 1u)) {
      EXCEPTION_RECORD rec = {};
      rec.ExceptionCode = code;
      rec.ExceptionFlags = flags;
      rec.NumberParameters = (count > EXCEPTION_MAXIMUM_PARAMETERS)
                                 ? EXCEPTION_MAXIMUM_PARAMETERS
                                 : count;
      if (args) {
        for (DWORD i = 0; i < rec.NumberParameters; ++i)
          rec.ExceptionInformation[i] = (ULONG_PTR)args[i];
      }
      CONTEXT ctx = {};
      ctx.ContextFlags = CONTEXT_FULL;
      ::RtlCaptureContext(&ctx);
      EXCEPTION_POINTERS ep = {&rec, &ctx};
      trt_run_guest_veh(&ep);  // verdict ignored: swallow either way
    }
#endif
#ifdef _WIN32
    // H2 diagnostic (timing race): the .172-.192 full OS dispatch took
    // hundreds of us-ms, giving the arena worker time to init before
    // sekiro.exe+0x115c66b. The instant swallow removes that delay; a
    // 50ms yield restores real-Windows timing (1ms was insufficient
    // on .196 -- the fault persisted). If Sekiro passes
    // +0x115c66b with this yield, H2 is confirmed (the .195 GPR dump
    // shows mov r8,[rax] with rax=-24 = CONTAINING_RECORD(NULL) on a
    // list the worker hasn't populated yet).
#endif
    return;
  }
  static unsigned s_cpp_eh_logged = 0;
  if (code != 0xe06d7363u || ++s_cpp_eh_logged <= 4) {
    char line[192];
    snprintf(line, sizeof(line),
             "kernel32: RaiseException(code=0x%08x flags=%u count=%u arg0=0x%llx)",
             code, flags, count, count ? (unsigned long long)args[0] : 0ull);
    boot_log(line);
    // .249 DIAG: name the C++ exception type (the .248 Sekiro death is a
    // 0xe06d7363 whose type was unknown -- the bridge logged only
    // code/flags/count/arg0). Throttled with the line above (first 4).
    if (code == 0xe06d7363u) {
      char tname[128];
      u32 ntypes = 0;
      const bool ok = k32_cpp_eh_type_name_249(
          args, count, tname, sizeof(tname), &ntypes);
      char tline[256];
      snprintf(tline, sizeof(tline),
               "kernel32: C++ exception type=%s ntypes=%u obj=0x%llx "
               "throwinfo=0x%llx img=0x%llx",
               ok ? tname : "?",
               ntypes,
               (args && count > 1) ? (unsigned long long)args[1] : 0ull,
               (args && count > 2) ? (unsigned long long)args[2] : 0ull,
               (args && count > 3) ? (unsigned long long)args[3] : 0ull);
      boot_log(tline);
    }
  }
  if (trt_boot_raise_trap) { trt_boot_raise_trap(code); }
#ifdef _WIN32
  std::vector<void*> vehs;
  veh_snapshot_ordered(&vehs);
  if (!vehs.empty() && !(flags & 1u)) {
    EXCEPTION_RECORD rec = {};
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.NumberParameters = (count > EXCEPTION_MAXIMUM_PARAMETERS)
                               ? EXCEPTION_MAXIMUM_PARAMETERS
                               : count;
    if (args) {
      for (DWORD i = 0; i < rec.NumberParameters; ++i)
        rec.ExceptionInformation[i] = (ULONG_PTR)args[i];
    }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    ::RtlCaptureContext(&ctx);
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    if (trt_run_guest_veh(&ep) == -1) return;
  }
  ::RaiseException(code, flags, count, (const ULONG_PTR*)(uintptr_t)args);
#else
  boot_log("kernel32: RaiseException unhandled -> terminating");
  _exit(42);
#endif
}
#endif  // !defined(_WIN32) && defined(__x86_64__)

// ---- __C_specific_handler (host-side personality, real Win64 contract) ----
// Statically-linked MSVC CRTs import this from kernel32 (measured live:
// Ultra Pool game.exe IAT slot -> KERNEL32.dll!__C_specific_handler; the
// blanket-0 SYSTEM bind turned every hardware fault into "resume at fault"
// = an 8.9M-refault EH livelock). This mirrors the tested guest clone in
// src/guest/guest_eh.c: scope table {Count, Records[]} in HandlerData,
// HandlerAddress==1 = constant filter, verdict 1 = EXECUTE_HANDLER (unwind
// through RtlUnwindEx, never return), verdict -1 = CONTINUE_EXECUTION,
// no match = ExceptionContinueSearch. Dispositions use the REAL Win64
// constants (ContinueSearch=0, ContinueExecution=1, CollidedUnwind=3).
struct csh_scope_rec {
  u32 begin;
  u32 end;
  u32 handler;
  u32 jump;
};
struct csh_scope_table {
  u32 count;
  csh_scope_rec rec[8];
};

u32 TRT_STDCALL trt_C_specific_handler(u64 rec_p, u64 frame,
                                       u64 ctx_p, u64 dc_p) {
  auto* rec = (trt_ex_record*)(uintptr_t)rec_p;
  auto* ctx = (u8*)(uintptr_t)ctx_p;
  auto* dc = (trt_dispatcher_ctx*)(uintptr_t)dc_p;
  if (!dc || !dc->handler_data) return 0;  // ExceptionContinueSearch
  const u64 image = dc->image_base;
  const u64 pc = dc->control_pc - image;  // control PC as an RVA
  auto* st = (const csh_scope_table*)(uintptr_t)dc->handler_data;
  for (u32 i = 0; i < st->count && i < 8; ++i) {
    const csh_scope_rec& s = st->rec[i];
    if (!s.begin || !s.end) continue;
    if (pc < s.begin || pc >= s.end) continue;
    if (!s.jump) continue;  // __finally: phase-2 territory
    int verdict = 0;
    if (s.handler == 1) {
      verdict = 1;  // __except(1): constant filter, never called
    } else if (s.handler) {
      // Call the guest filter (ms_abi function pointers stored in the PE's
      // .xdata) through the shared guest-call boundary.
      trt_ex_pointers ep = {(u64)(uintptr_t)rec, (u64)(uintptr_t)ctx};
      const i32 v = (i32)(u32)trt_call_guest(
          (void*)(uintptr_t)(image + s.handler),
          (u64)(uintptr_t)&ep, frame, 0, 0);
      verdict = (int)v;
    } else {
      continue;
    }
    if (verdict == 1) {  // EXCEPTION_EXECUTE_HANDLER
      // Unwind from the throw-site context to the establishing frame, then
      // resume inside the __except block. RtlUnwindEx owns the resume; the
      // personality never returns in this case.
      u64 history_buf[16] = {};
      trt_RtlUnwindEx(frame, image + s.jump, rec, rec->code, ctx, history_buf);
      return 0;  // not reached on the SysV path: RtlUnwindEx resumes
    }
    if (verdict == -1) return 1;  // EXCEPTION_CONTINUE_EXECUTION
  }
  return 0;  // ExceptionContinueSearch: keep walking outward
}

// (end __C_specific_handler host personality)

void* TRT_STDCALL trt_SetUnhandledExceptionFilter(void* filter) {
  const void* prev = g_unhandled_filter.exchange(filter);
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: SetUnhandledExceptionFilter(filter=%p) -> stored "
           "(previous=%p); the fatal path will consult it",
           filter, prev);
  boot_log(line);
  return (void*)prev;  // real Win32 returns the previous filter
}

void* TRT_STDCALL trt_UnhandledExceptionFilter(void* info) {
  void* filter = g_unhandled_filter.load();
  if (!filter) {
    boot_log("kernel32: UnhandledExceptionFilter -> EXCEPTION_CONTINUE_SEARCH "
             "(no filter registered)");
    return (void*)(uintptr_t)0;  // EXCEPTION_CONTINUE_SEARCH
  }
  const i32 verdict = (i32)trt_call_guest(filter, (u64)(uintptr_t)info, 0, 0, 0);
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: UnhandledExceptionFilter -> filter %s (verdict %d)",
           eh_filter_verdict_name(verdict), (int)verdict);
  boot_log(line);
  return (void*)(uintptr_t)(i64)verdict;
}

int TRT_STDCALL trt_IsDebuggerPresent(void) { return 0; }

void* TRT_STDCALL trt_EncodePointer(void* p) {
  // pointer obfuscation: identity is a legal (if weak) encoder — the value
  // round-trips through DecodePointer, which is all the CRT requires
  return p;
}

void* TRT_STDCALL trt_DecodePointer(void* p) { return p; }

namespace {
std::atomic_flag g_slist_lock = ATOMIC_FLAG_INIT;
void slist_lock() { while (g_slist_lock.test_and_set(std::memory_order_acquire)) {} }
void slist_unlock() { g_slist_lock.clear(std::memory_order_release); }

struct trt_slist_entry {
  trt_slist_entry* Next;
};
struct trt_slist_header {
  u16 depth;
  u16 seq;
  u32 pad;
  trt_slist_entry* first;
};
static_assert(sizeof(trt_slist_header) == 16, "Win64 SLIST_HEADER is 16 bytes");
} // namespace

int TRT_STDCALL trt_InitializeSListHead(void* head) {
  if (!head) return 0;
  memset(head, 0, 16);  // SLIST_HEADER x64
  return 1;
}

void* TRT_STDCALL trt_InterlockedPushEntrySList(void* head, void* entry) {
  if (!head || !entry) return nullptr;
  trt_slist_header* h = (trt_slist_header*)head;
  trt_slist_entry* e = (trt_slist_entry*)entry;
  slist_lock();
  trt_slist_entry* old_first = h->first;
  e->Next = old_first;
  h->first = e;
  ++h->depth;
  ++h->seq;
  slist_unlock();
  return old_first;
}

void* TRT_STDCALL trt_InterlockedPopEntrySList(void* head) {
  if (!head) return nullptr;
  trt_slist_header* h = (trt_slist_header*)head;
  slist_lock();
  trt_slist_entry* e = h->first;
  if (e) {
    h->first = e->Next;
    if (h->depth > 0) --h->depth;
    ++h->seq;
  }
  slist_unlock();
  return e;
}

void* TRT_STDCALL trt_InterlockedFlushSList(void* head) {
  if (!head) return nullptr;
  trt_slist_header* h = (trt_slist_header*)head;
  slist_lock();
  trt_slist_entry* old_first = h->first;
  h->first = nullptr;
  h->depth = 0;
  ++h->seq;
  slist_unlock();
  return old_first;
}

u16 TRT_STDCALL trt_QueryDepthSList(void* head) {
  if (!head) return 0;
  slist_lock();
  const u16 d = ((trt_slist_header*)head)->depth;
  slist_unlock();
  return d;
}

// Shared with bridge_thread.cpp: initialises the documented
// RTL_CRITICAL_SECTION layout (LockCount = -1, SpinCount at its real offset).
extern "C" void trt_cs_init_layout(void* cs, u32 spin);

int TRT_STDCALL trt_InitializeCriticalSectionAndSpinCount(void* cs, u32 spin) {
  if (!cs) return 0;
  trt_cs_init_layout(cs, spin);
  return 1;
}

int TRT_STDCALL trt_InitializeCriticalSectionEx(void* cs, u32 spin, u32 flags) {
  (void)flags;
  return trt_InitializeCriticalSectionAndSpinCount(cs, spin);
}

// ---- SRW locks + condition variables (real atomic + condvar wait/wake) ----
// SRWLOCK layout: 8 bytes holding state word:
//   0 = unlocked, -1 (0xFFFFFFFFu) = exclusively locked, >0 = shared reader count
// Bit 31 (0x80000000) is the WRITER-WAITING flag: set by a writer that
// could not acquire immediately, it makes new readers queue behind the
// writer instead of starving it (the reader count is capped at 2^31-1,
// which no game approaches). The flag clears when a writer acquires
// (FLAG -> EXCLUSIVE).
// Acquire spins briefly (256 tries) then blocks on WaitOnAddress — the old
// code spun on Sleep(0) FOREVER, burning a full core under contention.
// Every state change that can unblock a waiter is followed by a
// WakeByAddressAll (WaitOnAddress does not wake on a bare value change;
// the waker must call Wake). Waiters re-check the value at WaitOnAddress
// entry, so a wake that lands before registration is not lost.
namespace {
constexpr u32 k_srw_exclusive = 0xFFFFFFFFu;
constexpr u32 k_srw_writer_waiting = 0x80000000u;

struct cv_slot {
  void* key;
#if defined(_WIN32)
  CONDITION_VARIABLE cv;
  CRITICAL_SECTION cs;
  bool init;
#else
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool init;
#endif
  // .215 DIAG: per-CV log counters (rate-limit wake/woke lines).
  u64 wake_log_n;
  u64 woke_log_n;
  // .216 DIAG: per-CV wait count (top-5 in the CV summary).
  std::atomic<u64> wait_n;
};
constexpr int k_max_cvs = 512;
cv_slot g_cv_slots[k_max_cvs];
std::atomic_flag g_cv_table_lock = ATOMIC_FLAG_INIT;
// Fail-visible CV accounting: a wake that finds no registered waiter is a
// no-op by Win32 contract, but a wake that lands while the waiter is still
// BETWEEN lock-release and wait-registration is a LOST WAKEUP (measured live:
// Godot's tid=11 parked forever in SleepConditionVariableSRW while holding a
// CS, wedging the whole boot). Counters + the g_cv_wake_mu-serialized wait
// below make the window impossible instead of invisible.
std::atomic<u64> g_cv_waits{0}, g_cv_wake_single{0}, g_cv_wake_all{0},
    g_cv_wake_no_waiter{0}, g_cv_timeouts{0}, g_cv_wokes{0};
// Serializes "release the user lock -> register as waiter" against Wake*,
// closing the lost-wakeup window. Global (not per-slot): wake frequency is
// low and correctness beats contention here.
static std::mutex g_cv_wake_mu;
static std::atomic<bool> g_cv_exhaustion_named{false};

cv_slot* cv_get(void* key) {
  if (!key) return nullptr;
  while (g_cv_table_lock.test_and_set(std::memory_order_acquire)) {}
  cv_slot* found = nullptr;
  bool created = false;
  for (int i = 0; i < k_max_cvs; ++i) {
    if (g_cv_slots[i].init && g_cv_slots[i].key == key) { found = &g_cv_slots[i]; break; }
  }
  if (!found) {
    for (int i = 0; i < k_max_cvs; ++i) {
      if (!g_cv_slots[i].init) {
        g_cv_slots[i].key = key;
#if defined(_WIN32)
        InitializeConditionVariable(&g_cv_slots[i].cv);
        InitializeCriticalSection(&g_cv_slots[i].cs);
#else
        pthread_mutex_init(&g_cv_slots[i].mu, nullptr);
        pthread_cond_init(&g_cv_slots[i].cv, nullptr);
#endif
        g_cv_slots[i].init = true;
        g_cv_slots[i].wake_log_n = 0;
        g_cv_slots[i].woke_log_n = 0;
        g_cv_slots[i].wait_n.store(0, std::memory_order_relaxed);
        created = true;
        found = &g_cv_slots[i];
        break;
      }
    }
    if (!found && !g_cv_exhaustion_named.exchange(true)) {
      char line[128];
      snprintf(line, sizeof(line),
               "sync: condition-variable table exhausted (%d slots); "
               "further CVs fail with err=8 (fail-visible)", k_max_cvs);
      trt_core_log(line);
    }
  }
  g_cv_table_lock.clear(std::memory_order_release);
  if (created && found) {
    // .215 DIAG: log each CV first appearance (once per CV).
    char line[128];
    snprintf(line, sizeof(line), "sync: CV-new cv=%p", key);
    trt_core_log(line);
  }
  return found;
}
} // namespace

void TRT_STDCALL trt_InitializeSRWLock(void* srw) {
  if (srw) memset(srw, 0, sizeof(void*));
}

void* TRT_STDCALL trt_TryAcquireSRWLockExclusive(void* srw) {
  if (!srw) return (void*)(uintptr_t)0;
  u32 expected = 0;
  bool ok = __atomic_compare_exchange_n((u32*)srw, &expected, k_srw_exclusive,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  return (void*)(uintptr_t)(ok ? 1 : 0);
}

void* TRT_STDCALL trt_AcquireSRWLockExclusive(void* srw) {
  if (!srw) return nullptr;
  const u32 saved_err = err_for_current_thread();
  for (int i = 0; i < 256; ++i) {
    if (trt_TryAcquireSRWLockExclusive(srw)) {
      set_err_for_current_thread(saved_err);
      return srw;
    }
    trt_Sleep(0);
  }
  // set the writer-waiting flag so new readers queue behind this writer
  // instead of starving it (a stream of arriving readers used to hold the
  // lock forever against a waiting writer)
  u32 cur = __atomic_load_n((u32*)srw, __ATOMIC_RELAXED);
  while (cur != k_srw_exclusive && !(cur & k_srw_writer_waiting)) {
    if (__atomic_compare_exchange_n((u32*)srw, &cur,
                                    cur | k_srw_writer_waiting, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      break;
    }
  }
  for (;;) {
    cur = __atomic_load_n((u32*)srw, __ATOMIC_RELAXED);
    if (cur == 0 || cur == k_srw_writer_waiting) {
      u32 expect = cur;
      if (__atomic_compare_exchange_n((u32*)srw, &expect, k_srw_exclusive,
                                      false, __ATOMIC_ACQUIRE,
                                      __ATOMIC_RELAXED)) {
        set_err_for_current_thread(saved_err);
        return srw;
      }
      continue;
    }
    // block until the state word changes (a release wakes us) — no more
    // full-core Sleep(0) spin under contention
    {
      // .206 diagnostic: log the first SRW-exclusive waits (first 64).
      // SRW acquire parks INFINITE with no stall probe, so a blocked
      // writer is otherwise invisible. The [tid=] prefix (boot_log ->
      // trt_core_log) identifies the waiter; state shows the lock state.
      static unsigned s_srw_ex_logged = 0;
      if (s_srw_ex_logged < 64) {
        ++s_srw_ex_logged;
        char sl[192];
        snprintf(sl, sizeof(sl),
                 "sync: AcquireSRWLockExclusive srw=%p CONTENDED state=%u",
                 srw, cur);
        boot_log(sl);
      }
    }
    trt_WaitOnAddress(srw, &cur, 4, 0xFFFFFFFFu /*INFINITE*/);
  }
}

void* TRT_STDCALL trt_ReleaseSRWLockExclusive(void* srw) {
  if (srw) {
    __atomic_store_n((u32*)srw, 0u, __ATOMIC_RELEASE);
    trt_WakeByAddressAll(srw);  // wake reader + writer waiters
  }
  return srw;
}

int TRT_STDCALL trt_TryAcquireSRWLockShared(void* srw) {
  if (!srw) return 0;
  u32 cur = __atomic_load_n((u32*)srw, __ATOMIC_RELAXED);
  // a waiting writer (flag) or an exclusive holder refuses new readers:
  // writer preference, so a writer can never be starved by arriving readers
  while (cur != k_srw_exclusive && !(cur & k_srw_writer_waiting)) {
    if (__atomic_compare_exchange_n((u32*)srw, &cur, cur + 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return 1;
    }
  }
  return 0;
}

void* TRT_STDCALL trt_AcquireSRWLockShared(void* srw) {
  if (!srw) return nullptr;
  const u32 saved_err = err_for_current_thread();
  for (int i = 0; i < 256; ++i) {
    if (trt_TryAcquireSRWLockShared(srw)) {
      set_err_for_current_thread(saved_err);
      return srw;
    }
    trt_Sleep(0);
  }
  for (;;) {
    if (trt_TryAcquireSRWLockShared(srw)) {
      set_err_for_current_thread(saved_err);
      return srw;
    }
    u32 cur = __atomic_load_n((u32*)srw, __ATOMIC_RELAXED);
    trt_WaitOnAddress(srw, &cur, 4, 0xFFFFFFFFu /*INFINITE*/);
  }
}

void* TRT_STDCALL trt_ReleaseSRWLockShared(void* srw) {
  if (srw) {
    const u32 after = __atomic_sub_fetch((u32*)srw, 1u, __ATOMIC_RELEASE);
    // wake waiters only when the last reader left: a writer may now take
    // it (state FLAG or 0). Waking on every release would thundering-herd
    // under heavy reader traffic.
    if (after == 0 || after == k_srw_writer_waiting)
      trt_WakeByAddressAll(srw);
  }
  return srw;
}

void TRT_STDCALL trt_InitializeConditionVariable(void* cv) {
  if (cv) memset(cv, 0, sizeof(void*));
}

// .216 DIAG: CV summary -- every 256th CV wait (.217), log global totals plus
// the top 5 CVs by wait count (names the livelock participants instead of
// forcing a full-log grep). Lock-free reads only; safe from the wait path.
static void cv_log_summary() {
  u64 top_n[5] = {0, 0, 0, 0, 0};
  void* top_cv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  for (int i = 0; i < k_max_cvs; ++i) {
    if (!g_cv_slots[i].init) continue;
    const u64 n = g_cv_slots[i].wait_n.load(std::memory_order_relaxed);
    if (n == 0) continue;
    for (int j = 0; j < 5; ++j) {
      if (n > top_n[j]) {
        for (int k = 4; k > j; --k) {
          top_n[k] = top_n[k - 1];
          top_cv[k] = top_cv[k - 1];
        }
        top_n[j] = n;
        top_cv[j] = g_cv_slots[i].key;
        break;
      }
    }
  }
  char line[512];
  snprintf(line, sizeof(line),
           "sync: CV-summary waits=%llu wakes=%llu wokes=%llu timeouts=%llu "
           "top: %p=%llu %p=%llu %p=%llu %p=%llu %p=%llu",
           (unsigned long long)g_cv_waits.load(std::memory_order_relaxed),
           (unsigned long long)(g_cv_wake_single.load(std::memory_order_relaxed) +
                                g_cv_wake_all.load(std::memory_order_relaxed)),
           (unsigned long long)g_cv_wokes.load(std::memory_order_relaxed),
           (unsigned long long)g_cv_timeouts.load(std::memory_order_relaxed),
           top_cv[0], (unsigned long long)top_n[0],
           top_cv[1], (unsigned long long)top_n[1],
           top_cv[2], (unsigned long long)top_n[2],
           top_cv[3], (unsigned long long)top_n[3],
           top_cv[4], (unsigned long long)top_n[4]);
  trt_core_log(line);
}

// ---- .218 DIAG: gap API-call trace (Sekiro silent death) -----------------
// Ring of the last 32 bridged calls (name + tid + seq + repeat count),
// dumped by the watchdog (main.cpp, snapshot-only). The Sekiro death
// leaves no bridged call between the last SetEvent / input-init and the
// silence; if the ring shows the game called NOTHING bridged in the gap,
// the exit is a direct syscall or a platform kill. If it shows a last
// call, that call is the next lead. Consecutive duplicates collapse into
// a repeat count, so a QPC / GetTickCount spin loop is one entry, not 32
// evicted slots. Names are string literals (pointer-compared).
namespace {
struct gap_entry {
  const char* name;
  u32 tid;
  u64 seq;
  u32 repeats;
};
constexpr int k_gap_ring_n = 32;
gap_entry g_gap_ring[k_gap_ring_n];
int g_gap_ring_head = 0;  // next slot to write
u64 g_gap_seq = 0;        // total records (seq of the last entry)
u64 g_gap_launch_start = 0;
u64 g_gap_entries = 0;    // non-duplicate entries ever written
std::mutex g_gap_mu;
// The thread that presented frame=1 (the main-thread proxy for Ultrapool).
std::atomic<u32> g_gap_frame1_tid{0};
// First-sight distinct CVs the frame-1 thread waits on after frame=1.
constexpr int k_gap_main_cvs = 16;
std::atomic<uintptr_t> g_gap_main_cvs[k_gap_main_cvs];
std::atomic<u64> g_gap_main_cv_n[k_gap_main_cvs];
// .219 DIAG: guest-CPU proof (fed by the watchdog in main.cpp) +
// tex-fail correlation counters.
std::atomic<u64> g_gap_guest_cpu_ms{0};
std::atomic<u64> g_gap_proc_cpu_ms{0};
std::atomic<u64> g_gap_tex_fails{0};
std::atomic<u64> g_gap_main_cv_waits{0};
}  // namespace

void gap_trace_begin_launch(void) {
  std::lock_guard<std::mutex> lock(g_gap_mu);
  g_gap_launch_start = g_gap_seq;
}

u32 gap_trace_launch_tail(trt_launch_api_entry* entries, u32 capacity, u64* calls) {
  std::lock_guard<std::mutex> lock(g_gap_mu);
  if (calls) *calls = g_gap_seq - g_gap_launch_start;
  if (!entries) return 0;
  u32 count = 0;
  while (count < capacity && count < g_gap_entries) {
    const auto index = (g_gap_ring_head + k_gap_ring_n - 1 - count) % k_gap_ring_n;
    const auto& item = g_gap_ring[index];
    if (item.seq <= g_gap_launch_start) break;
    entries[count++] = {item.name, item.repeats};
  }
  return count;
}

void gap_trace_record(const char* name) {
  if (!name) return;
  const u32 tid = trt_GetCurrentThreadId();
  std::lock_guard<std::mutex> lk(g_gap_mu);
  if (g_gap_seq > 0) {
    const int last = (g_gap_ring_head + k_gap_ring_n - 1) % k_gap_ring_n;
    if (g_gap_ring[last].seq > g_gap_launch_start &&
        g_gap_ring[last].name == name && g_gap_ring[last].tid == tid) {
      ++g_gap_ring[last].repeats;
      ++g_gap_seq;
      return;
    }
  }
  gap_entry& e = g_gap_ring[g_gap_ring_head];
  e.name = name;
  e.tid = tid;
  e.seq = ++g_gap_seq;
  e.repeats = 1;
  g_gap_ring_head = (g_gap_ring_head + 1) % k_gap_ring_n;
  if (g_gap_entries < (u64)k_gap_ring_n) ++g_gap_entries;
}

extern "C" void trt_sync_census_log(void);  // .224 DIAG: bridge_thread.cpp

void gap_trace_dump_if_changed(void) {
  // Called every watchdog tick (nominally 10ms). .219 DIAG: WALL-CLOCK
  // cadence -- the .218 tick-count cadence (100 ticks periodic, 50 ticks
  // quiet-window) never fired under watchdog starvation: the Sekiro guest
  // spins at near-100% CPU on a non-bridged path, so the watchdog only
  // iterates ~1Hz and 100 ticks take ~100s. Wall clock fires even then:
  // dump at most once per second while calls arrive, plus one final dump
  // once the ring has been quiet for 2s of wall time. Each dump prints the
  // guest main-thread CPU time + process CPU time fed by the watchdog, so
  // advancing CPU with zero new bridged calls PROVES spinning (vs
  // dead/blocked). The final dump also arms the syscall trap (below).
  // Re-arm the trap first if the VEH restored a byte for a host caller.
// .220 DIAG: silent re-arm (see trt_syscall_trap_install) + count it.
#ifdef _WIN32
  if (g_syscall_trap_rearm.exchange(false, std::memory_order_relaxed)) {
    g_syscall_trap_rearms.fetch_add(1, std::memory_order_relaxed);
    g_syscall_trap_quiet_rearm = true;
    trt_syscall_trap_install();
  }
#endif
  using clock = std::chrono::steady_clock;
  static clock::time_point s_last_dump = clock::now();
  static clock::time_point s_last_change = clock::now();
  static u64 s_last_dumped_seq = 0;
  static u64 s_last_seen_seq = 0;
  static bool s_final_dumped = false;
  static bool s_trap_arm_attempted = false;
  gap_entry snap[8];
  u64 seq = 0;
  u64 newcalls = 0;
  u64 guest_cpu = 0, proc_cpu = 0;
  int n = 0;
  bool final_dump = false;
  long long quiet_ms = 0;
  {
    std::lock_guard<std::mutex> lk(g_gap_mu);
    seq = g_gap_seq;
    const auto now = clock::now();
    if (seq != s_last_seen_seq) {
      s_last_seen_seq = seq;
      s_last_change = now;
      s_final_dumped = false;
    }
    const bool changed = (seq != s_last_dumped_seq);
    if (changed && now - s_last_dump >= std::chrono::seconds(1)) {
      // periodic dump while calls arrive
    } else if (!changed && !s_final_dumped &&
               now - s_last_change >= std::chrono::seconds(2)) {
      // final dump after 2s of wall-time quiet
      final_dump = true;
      quiet_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - s_last_change).count();
    } else {
      return;
    }
    newcalls = seq - s_last_dumped_seq;
    s_last_dumped_seq = seq;
    s_last_dump = now;
    if (final_dump) s_final_dumped = true;
    guest_cpu = g_gap_guest_cpu_ms.load(std::memory_order_relaxed);
    proc_cpu = g_gap_proc_cpu_ms.load(std::memory_order_relaxed);
    const u64 total = g_gap_entries < (u64)k_gap_ring_n
                          ? g_gap_entries
                          : (u64)k_gap_ring_n;
    n = total < 8 ? (int)total : 8;
    for (int i = 0; i < n; ++i) {
      const int idx =
          (g_gap_ring_head - n + i + 2 * k_gap_ring_n) % k_gap_ring_n;
      snap[i] = g_gap_ring[idx];
    }
  }
  char line[192];
  snprintf(line, sizeof(line),
           "gap: trace seq=%llu last %d guestcpu=%llums proccpu=%llums newcalls=%llu%s",
           (unsigned long long)seq, n,
           (unsigned long long)guest_cpu, (unsigned long long)proc_cpu,
           (unsigned long long)newcalls, final_dump ? " final" : "");
  trt_core_log(line);
  // .224 DIAG: sync census on the FIRST dump only -- the Sekiro
  // spin handles (event 25b000000 + mutexes 25b000001/2) are
  // created in the first second, so the first periodic dump
  // already names them; the final dump never runs for Sekiro
  // (it exits <2s after the last bridged call).
  {
    static bool s_census_done = false;
    if (!s_census_done) {
      s_census_done = true;
      trt_sync_census_log();
    }
  }
  // .220 DIAG: trap flap stats on every dump -- how much did the
  // trap flap (host trips vs re-arms) and how many targets are
  // still armed? If hosttrips is huge and armed is 0 most of the
  // time, the launch-arm coverage is thin (re-arm is ~1Hz).
#ifdef _WIN32
  // The trap state above (targets + flap counters) only exists on the Win32
  // trap path, so its diagnostic is compiled out elsewhere.
  {
    int armed_n = 0;
    for (int i = 0; i < 12; ++i)
      if (g_syscall_trap_targets[i].armed) ++armed_n;
    char ts[192];
    snprintf(ts, sizeof(ts),
             "syscall-trap: stats hosttrips=%llu rearms=%llu races=%llu armed=%d/12",
             (unsigned long long)g_syscall_trap_host_trips.load(
                 std::memory_order_relaxed),
             (unsigned long long)g_syscall_trap_rearms.load(
                 std::memory_order_relaxed),
             (unsigned long long)g_syscall_trap_races.load(
                 std::memory_order_relaxed),
             armed_n);
    trt_core_log(ts);
  }
#endif
  for (int i = 0; i < n; ++i) {
    snprintf(line, sizeof(line), "gap: trace #%llu tid=%u %s x%u",
             (unsigned long long)snap[i].seq, snap[i].tid,
             snap[i].name ? snap[i].name : "(null)", snap[i].repeats);
    trt_core_log(line);
  }
  // .220 DIAG: arm the syscall trap at the FIRST dump, not the final
  // one -- the .219 trap never armed because the final dump needs 2s
  // of wall-time quiet and the guest dies first. The launch arm in
  // run_guest_trapped (main.cpp) is the primary arm; this is the
  // backup if it was skipped, and logs SKIP already-armed if it ran.
  // A PEB-walked / direct wait call (NtWaitForSingleObject,
  // NtWaitForMultipleObjects, NtDelayExecution, NtYieldExecution,
  // kernel32 Sleep / WaitForSingleObject / WaitForMultipleObjects /
  // user32 MsgWaitForMultipleObjects / MsgWaitForMultipleObjectsEx /
  // kernel32 SwitchToThread /
  // SleepConditionVariableSRW / SleepConditionVariableCS) is
  // then caught with its guest caller.
  // One attempt per process: re-arms after host-caller restores go
  // through g_syscall_trap_rearm above.
#ifdef _WIN32
  if (!s_trap_arm_attempted) {
    s_trap_arm_attempted = true;
    trt_syscall_trap_install();
  }
#endif
  // .220 DIAG: commit total at death -- the final dump is the last
  // log line before a silent death, so print the bridge ledger
  // here. If committed is still ~4668/5120MB with 0 refusals,
  // the 5GB-cap kill is REFUTED (no growth toward the cap).
#ifdef _WIN32
  if (final_dump) {
    char budget[256];
    trt_mem_budget_report(budget, sizeof(budget));
    char bl[512];
    snprintf(bl, sizeof(bl), "gap: commit-at-death %s", budget);
    trt_core_log(bl);
  }
  // .222 DIAG: PARKED report on EVERY dump, not only the final
  // one -- the final dump never fires on Sekiro (the process
  // exits <2s after the last bridged call, before the 2s-quiet
  // branch runs), so the periodic dump is the only one that
  // fires at death. trt_sync_parked_log lives in
  // bridge_thread.cpp.
  trt_sync_parked_log();
  // .223 DIAG: SILENCE dump -- when the guest goes quiet (no
  // new bridged calls for 2s+ of wall time, i.e. this final
  // dump) with NO parked wait the bridge can see, log the
  // last bridged call + the thread state (which tids made
  // the recent calls, what each was last doing). This
  // addresses the .222 Ultrapool anomaly (guest silent
  // ~170s, PARKED none) and Sekiro death (dies with PARKED
  // none): if nothing is parked and nothing calls, the
  // guest is spinning on a non-bridged path or dead.
  // Parked > 0 explains the silence already (PARKED lines
  // above); parked < 0 means the table was busy, so skip
  // rather than lie.
  if (final_dump && n > 0) {
    const int parked_n = trt_sync_parked_count();
    if (parked_n == 0) {
      // Distinct tids in the last-8 ring, oldest first.
      u32 seen_tids[8];
      int seen_n = 0;
      for (int i = 0; i < n; ++i) {
        bool dup = false;
        for (int j = 0; j < seen_n; ++j) {
          if (seen_tids[j] == snap[i].tid) { dup = true; break; }
        }
        if (!dup) seen_tids[seen_n++] = snap[i].tid;
      }
      const gap_entry& last = snap[n - 1];
      char sl[256];
      snprintf(sl, sizeof(sl),
               "gap: SILENCE quiet=%llums last=#%llu tid=%u %s x%u guestcpu=%llums proccpu=%llums parked=0 threads=%d",
               (unsigned long long)quiet_ms,
               (unsigned long long)last.seq, last.tid,
               last.name ? last.name : "(null)", last.repeats,
               (unsigned long long)guest_cpu,
               (unsigned long long)proc_cpu, seen_n);
      trt_core_log(sl);
      // One line per distinct tid, each with its MOST
      // RECENT ring entry (scan from the newest).
      for (int t = 0; t < seen_n; ++t) {
        const gap_entry* e = nullptr;
        for (int i = n - 1; i >= 0; --i) {
          if (snap[i].tid == seen_tids[t]) { e = &snap[i]; break; }
        }
        if (!e) continue;
        char tl[192];
        snprintf(tl, sizeof(tl), "gap: SILENCE thread tid=%u last=#%llu %s x%u",
                 e->tid, (unsigned long long)e->seq,
                 e->name ? e->name : "(null)", e->repeats);
        trt_core_log(tl);
      }
    }
  }
#endif
}

void gap_trace_set_guest_cpu_ms(u64 guest_ms, u64 proc_ms) {
  g_gap_guest_cpu_ms.store(guest_ms, std::memory_order_relaxed);
  g_gap_proc_cpu_ms.store(proc_ms, std::memory_order_relaxed);
}
// .223 DIAG: app working-set feed -- the watchdog (main.cpp,
// snapshot-only) publishes MemoryManager::AppMemoryUsage()
// here once per tick, so the VIDEO-SAMPLE can print appmem
// next to the video usage without paying for a second ~1s
// WinRT call. The .222 re-test showed app memory tracking
// video usage in lockstep (+1847MB app vs +1779MB video,
// +2044MB vs +2001MB), so the ~2GB is real process memory,
// not a phantom reservation. gap_trace_seq: total bridged
// calls recorded, for the VIDEO-SAMPLE per-interval deltas.
static std::atomic<u64> g_video_app_mem_mb{0};
extern "C" void trt_video_set_app_mem_mb(u64 mb) {
  g_video_app_mem_mb.store(mb, std::memory_order_relaxed);
}
extern "C" u64 trt_video_app_mem_mb(void) {
  return g_video_app_mem_mb.load(std::memory_order_relaxed);
}
extern "C" u64 gap_trace_seq(void) {
  std::lock_guard<std::mutex> lk(g_gap_mu);
  return g_gap_seq;
}

u64 gap_trace_note_tex_fail(void) {
  return g_gap_tex_fails.fetch_add(1, std::memory_order_relaxed) + 1;
}

u64 gap_trace_tex_fails(void) {
  return g_gap_tex_fails.load(std::memory_order_relaxed);
}

u64 gap_trace_main_cv_waits(void) {
  return g_gap_main_cv_waits.load(std::memory_order_relaxed);
}

void gap_trace_note_frame1(u32 tid) {
  u32 expect = 0;
  g_gap_frame1_tid.compare_exchange_strong(expect, tid,
                                           std::memory_order_relaxed);
}

u32 gap_trace_frame1_tid(void) {
  return g_gap_frame1_tid.load(std::memory_order_relaxed);
}

// .224 DIAG: ret2 side-channel (see cv_wait_locked). trt.h
// declares gap_trace_note_main_cv_wait 2-arg and trt.h is
// breadth-agent-owned, so the signature stays 2-arg and the
// 3rd datum rides here, set + cleared around each call.
static thread_local void* s_gap_main_cv_ret2 = nullptr;

int gap_trace_note_main_cv_wait(void* cv, void* ret) {
  const u32 ftid = g_gap_frame1_tid.load(std::memory_order_relaxed);
  if (!ftid || trt_GetCurrentThreadId() != ftid) return 0;
  const uintptr_t key = (uintptr_t)cv;
  for (int i = 0; i < k_gap_main_cvs; ++i) {
    const uintptr_t cur = g_gap_main_cvs[i].load(std::memory_order_relaxed);
    if (cur == key) {
      const u64 n =
          g_gap_main_cv_n[i].fetch_add(1, std::memory_order_relaxed) + 1;
      // .219 DIAG: sampled re-waits -- the AFAFC0 predicate-never-flips
      // pattern is wake -> re-wait; pair each sampled re-wait with the
      // texture-failure count so far to test the tex-fail correlation.
      if (n <= 8 || (n % 64) == 0) {
        char line[160];
        snprintf(line, sizeof(line),
                 "gap: main CV-wait cv=%p n=%llu texfails=%llu",
                 cv, (unsigned long long)n,
                 (unsigned long long)g_gap_tex_fails.load(
                     std::memory_order_relaxed));
        trt_core_log(line);
      }
      return 1;
    }
    if (cur == 0) {
      uintptr_t expect = 0;
      if (g_gap_main_cvs[i].compare_exchange_strong(expect, key,
                                                    std::memory_order_relaxed)) {
        g_gap_main_cv_n[i].store(1, std::memory_order_relaxed);
        // .219 DIAG: WAIT-SITE -- ret is the guest return address of
        // the SleepConditionVariable call, i.e. the Godot code waiting
        // on this CV; the main image base turns it into an offset.
        // First sight only (the site does not move between re-waits).
        const trt_image* first = trt_images_first();
        const u64 base =
            (first && first->base) ? (u64)(uintptr_t)first->base : 0;
        // .224 DIAG: CALLER2 -- ret2 is the level-1 return address,
        // i.e. the engine call site of
        // wait_for_group_task_completion (ret 0x2DC5F72). It is
        // pre-validated by guest_caller2_from_fp; the offset is
        // only computed when it lands in the first image, so
        // the offline .pdata map can name the caller.
        void* ret2 = s_gap_main_cv_ret2;
        u64 c2_off = 0;
        unsigned c2_valid = 0;
        if (ret2 && first && first->base && first->size_of_image) {
          const u64 b = (u64)(uintptr_t)first->base;
          const u64 c = (u64)(uintptr_t)ret2;
          if (c >= b && c < b + first->size_of_image) {
            c2_off = c - b;
            c2_valid = 1;
          }
        }
        char line[320];
        snprintf(line, sizeof(line),
                 "gap: main CV-wait after frame=1 cv=%p ret=0x%llx base=0x%llx tid=%u caller2=0x%llx caller2_off=0x%llx caller2_valid=%u (first sight)",
                 cv, (unsigned long long)(uintptr_t)ret,
                 (unsigned long long)base, ftid,
                 (unsigned long long)(uintptr_t)ret2,
                 (unsigned long long)c2_off, c2_valid);
        trt_core_log(line);
        return 1;
      }
    }
  }
  return 0;
}

static int cv_wait_locked(cv_slot* s, u32 ms, void* ret, void* ret2) {
  // .219 DIAG: Ultrapool main-thread trace -- when the frame-1 thread
  // waits on a CV after frame=1, log the first sight of each CV with the
  // guest return address (the wait-site) + sampled re-waits with the
  // tex-fail count. ret is the guest return address of the
  // SleepConditionVariable call, captured by the caller.
  s_gap_main_cv_ret2 = ret2;
  if (gap_trace_note_main_cv_wait(s ? s->key : nullptr, ret))
    g_gap_main_cv_waits.fetch_add(1, std::memory_order_relaxed);
  s_gap_main_cv_ret2 = nullptr;
  const u64 wn = g_cv_waits.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s) s->wait_n.fetch_add(1, std::memory_order_relaxed);
  // .217 DIAG: CV summary every 256th wait (.216 never fired: 971 waits < 1024).
  if ((wn % 256) == 0) cv_log_summary();
#if defined(_WIN32)
  const BOOL ok = ::SleepConditionVariableCS(&s->cv, &s->cs, ms);
  ::LeaveCriticalSection(&s->cs);
  if (!ok) {
    g_cv_timeouts.fetch_add(1, std::memory_order_relaxed);
    err_ref_for_current_thread() = 1460;  // ERROR_TIMEOUT
    return 0;
  }
  return 1;
#else
  int rc = 0;
  if (ms == 0xFFFFFFFFu) {
    rc = pthread_cond_wait(&s->cv, &s->mu);
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
    rc = pthread_cond_timedwait(&s->cv, &s->mu, &ts);
  }
  pthread_mutex_unlock(&s->mu);
  if (rc == ETIMEDOUT) {
    g_cv_timeouts.fetch_add(1, std::memory_order_relaxed);
    err_ref_for_current_thread() = 1460;  // ERROR_TIMEOUT
    return 0;
  }
  return rc == 0 ? 1 : 0;
#endif
}

int TRT_STDCALL trt_SleepConditionVariableSRW(void* cv, void* srw, u32 ms,
                                              u32 flags) {
  const bool shared = (flags & 0x1u) != 0;
  cv_slot* s = cv_get(cv);
  if (!s) {
    err_ref_for_current_thread() = 8;
    return 0;
  }
  // Hold the per-slot lock across releasing the user's SRW lock and entering
  // the OS condition wait so Wake* (which also acquires the per-slot lock)
  // can never fire in the window between user-lock release and sleep.
#if defined(_WIN32)
  ::EnterCriticalSection(&s->cs);
#else
  pthread_mutex_lock(&s->mu);
#endif
  if (srw) {
    if (shared) trt_ReleaseSRWLockShared(srw);
    else trt_ReleaseSRWLockExclusive(srw);
  }
  if (ms == 0xFFFFFFFFu) {
    // Fail-visible infinite-wait trace: an infinite Sleep on a CV while
    // holding another lock is the exact lost-wakeup wedge shape; pairing
    // these lines with the CS-stall probe names both ends of the contract.
    char iw[128];
    snprintf(iw, sizeof(iw),
             "sync: CV-infinite-wait cv=%p srw=%p flags=%u self_tid=%u", cv,
             srw, flags, trt_GetCurrentThreadId());
    trt_core_log(iw);
  }
  // .224 DIAG: capture the level-1 return address (engine call
  // site of the group wait) before entering the wait; the
  // frame walk is bounds-checked + validated, 0 on failure.
  // Host-only diagnostic: guest_caller2_from_fp walks the frame chain with
  // GetCurrentThreadStackLimits, which exists only in the Windows host build.
  // On other hosts the level-1 caller is simply unknown (0), not a build error.
  u64 caller2 = 0;
#ifdef _WIN32
  caller2 = guest_caller2_from_fp(__builtin_frame_address(0));
#endif
  const int r =
      cv_wait_locked(s, ms, __builtin_return_address(0), (void*)caller2);
  g_cv_wokes.fetch_add(1, std::memory_order_relaxed);
  // .215 DIAG: CV-woke -- log when a waiter EXITS the CV wait (wake->work
  // vs wake->immediate re-wait). Fires for ALL waits (incl. timed), so
  // timed waiters on a CV become visible. Rate-limited: first 64 exits
  // per CV + every 256th (.216: 16 -> 64, the first 16 hid the re-wait).
  {
    const u64 n = (u64)__atomic_fetch_add(&s->woke_log_n, 1, __ATOMIC_RELAXED) + 1;
    if (n <= 64 || (n % 256) == 0) {
      char line[128];
      snprintf(line, sizeof(line), "sync: CV-woke cv=%p r=%d n=%llu", cv, r,
               (unsigned long long)n);
      trt_core_log(line);
    }
  }
  if (srw) {
    if (shared) trt_AcquireSRWLockShared(srw);
    else trt_AcquireSRWLockExclusive(srw);
  }
  return r;
}

int TRT_STDCALL trt_SleepConditionVariableCS(void* cv, void* cs, u32 ms) {
  extern int TRT_STDCALL trt_LeaveCriticalSection(void* cs);
  extern int TRT_STDCALL trt_EnterCriticalSection(void* cs);
  cv_slot* s = cv_get(cv);
  if (!s) {
    err_ref_for_current_thread() = 8;
    return 0;
  }
#if defined(_WIN32)
  ::EnterCriticalSection(&s->cs);
#else
  pthread_mutex_lock(&s->mu);
#endif
  if (cs) trt_LeaveCriticalSection(cs);
  if (ms == 0xFFFFFFFFu) {
    char iw[128];
    snprintf(iw, sizeof(iw),
             "sync: CV-infinite-wait-CS cv=%p cs=%p self_tid=%u", cv,
             cs, trt_GetCurrentThreadId());
    trt_core_log(iw);
  }
  // .224 DIAG: capture the level-1 return address (engine call
  // site of the group wait) before entering the wait; the
  // frame walk is bounds-checked + validated, 0 on failure.
  // Host-only diagnostic: guest_caller2_from_fp walks the frame chain with
  // GetCurrentThreadStackLimits, which exists only in the Windows host build.
  // On other hosts the level-1 caller is simply unknown (0), not a build error.
  u64 caller2 = 0;
#ifdef _WIN32
  caller2 = guest_caller2_from_fp(__builtin_frame_address(0));
#endif
  const int r =
      cv_wait_locked(s, ms, __builtin_return_address(0), (void*)caller2);
  g_cv_wokes.fetch_add(1, std::memory_order_relaxed);
  // .215 DIAG: CV-woke (CS variant) -- see SleepConditionVariableSRW.
  // .216: cap 16 -> 64, same reason as the SRW variant.
  {
    const u64 n = (u64)__atomic_fetch_add(&s->woke_log_n, 1, __ATOMIC_RELAXED) + 1;
    if (n <= 64 || (n % 256) == 0) {
      char line[128];
      snprintf(line, sizeof(line), "sync: CV-woke-CS cv=%p r=%d n=%llu", cv, r,
               (unsigned long long)n);
      trt_core_log(line);
    }
  }
  if (cs) trt_EnterCriticalSection(cs);
  return r;
}

void TRT_STDCALL trt_WakeAllConditionVariable(void* cv) {
  // .219 DIAG: capture the guest return address -- the Godot code that
  // wakes this CV (the waker side of the AFAFC0 stall).
  void* ret = __builtin_return_address(0);
  cv_slot* s = cv_get(cv);
  if (!s) return;
  g_cv_wake_all.fetch_add(1, std::memory_order_relaxed);
  // .215 DIAG: rate-limit the wake log (was 91k lines / 14.9MB in .214).
  // First 16 wakes per CV + every 1024th (heartbeat).
  {
    const u64 n = (u64)__atomic_fetch_add(&s->wake_log_n, 1, __ATOMIC_RELAXED) + 1;
    if (n <= 16 || (n % 1024) == 0) {
      char line[128];
      snprintf(line, sizeof(line), "sync: CV-wake-all cv=%p ret=0x%llx n=%llu", cv,
               (unsigned long long)(uintptr_t)ret, (unsigned long long)n);
      trt_core_log(line);
    }
  }
#if defined(_WIN32)
  ::EnterCriticalSection(&s->cs);
  ::WakeAllConditionVariable(&s->cv);
  ::LeaveCriticalSection(&s->cs);
#else
  pthread_mutex_lock(&s->mu);
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
#endif
}

void TRT_STDCALL trt_WakeConditionVariable(void* cv) {
  // .219 DIAG: capture the guest return address -- see WakeAll.
  void* ret = __builtin_return_address(0);
  cv_slot* s = cv_get(cv);
  if (!s) return;
  g_cv_wake_single.fetch_add(1, std::memory_order_relaxed);
  // .215 DIAG: rate-limit (see CV-wake-all).
  {
    const u64 n = (u64)__atomic_fetch_add(&s->wake_log_n, 1, __ATOMIC_RELAXED) + 1;
    if (n <= 16 || (n % 1024) == 0) {
      char line[128];
      snprintf(line, sizeof(line), "sync: CV-wake cv=%p ret=0x%llx n=%llu", cv,
               (unsigned long long)(uintptr_t)ret, (unsigned long long)n);
      trt_core_log(line);
    }
  }
#if defined(_WIN32)
  ::EnterCriticalSection(&s->cs);
  ::WakeConditionVariable(&s->cv);
  ::LeaveCriticalSection(&s->cs);
#else
  pthread_mutex_lock(&s->mu);
  pthread_cond_signal(&s->cv);
  pthread_mutex_unlock(&s->mu);
#endif
}

// ---- file surface extensions the CRT init uses ----
// REAL Win64 signature: FindFirstFileExW(name, infoLevel, findData,
// searchOp, extraInfo, flags) — 6 args. A 3-arg declaration shifted every
// register left: the guest's infoLevel landed in `info` and the bridge
// memset through it (fault address 0x5 = the level value). Full signature
// keeps the register map honest.
void* TRT_STDCALL trt_FindFirstFileExW(const u16* name, u32 level, void* info,
                                       u32 search_op, void* extra, u32 flags) {
  (void)level; (void)search_op; (void)extra; (void)flags;
  return find_first_file(name, info);
}

int TRT_STDCALL trt_FindNextFileW(void* handle, void* info) {
  const u32 v = (u32)(uintptr_t)handle;
  if ((v >> 20) != 0x888 || (v & 0xFFFFF) >= 32 || !g_find_used[v & 0xFFFFF]) {
    err_ref_for_current_thread() = 6;  // INVALID_HANDLE
    return 0;
  }
  const int slot = (int)(v & 0xFFFFF);
  if (g_find_ctx[slot].is_iso) {
    if (!find_iso_next(slot, info)) {
      err_ref_for_current_thread() = 18;  // NO_MORE_FILES
      return 0;
    }
    return 1;
  }
#if defined(_WIN32)
  find_ctx& c = g_find_ctx[slot];
  WIN32_FIND_DATAW native_fd;
  if (c.phase == 0) {
    if (c.os_dir && FindNextFileW((HANDLE)c.os_dir, &native_fd)) {
      if (c.is_z) {
        char fnm[260];
        find_win_name_utf8(native_fd, fnm, sizeof(fnm));
        find_seen_push(c, fnm);
      }
      memcpy(info, &native_fd, sizeof(WIN32_FIND_DATAW));
      return 1;
    }
    if (c.os_dir) { FindClose((HANDLE)c.os_dir); c.os_dir = nullptr; }
    if (!c.is_z || c.opaque_ro || !c.host_path2[0]) {
      err_ref_for_current_thread() = 18;  // NO_MORE_FILES
      return 0;
    }
    c.phase = 1;
    u16 wro[1100];
    find_win_build_pat(c.host_path2, c.pattern, wro, 1100);
    HANDLE h_ro = FindFirstFileExW((LPCWSTR)wro, FindExInfoStandard, &native_fd,
                                   FindExSearchNameMatch, nullptr, 0);
    if (h_ro == INVALID_HANDLE_VALUE) {
      err_ref_for_current_thread() = 18;
      return 0;
    }
    c.os_dir2 = (void*)h_ro;
    bool ok = find_win_ro_accept(c, native_fd);
    while (!ok && FindNextFileW(h_ro, &native_fd))
      ok = find_win_ro_accept(c, native_fd);
    if (!ok) {
      FindClose(h_ro);
      c.os_dir2 = nullptr;
      err_ref_for_current_thread() = 18;
      return 0;
    }
    memcpy(info, &native_fd, sizeof(WIN32_FIND_DATAW));
    return 1;
  }
  if (!c.os_dir2) {
    err_ref_for_current_thread() = 18;
    return 0;
  }
  while (FindNextFileW((HANDLE)c.os_dir2, &native_fd)) {
    if (!find_win_ro_accept(c, native_fd)) continue;
    memcpy(info, &native_fd, sizeof(WIN32_FIND_DATAW));
    return 1;
  }
  err_ref_for_current_thread() = 18;
  return 0;
#else
  if (!find_advance_posix(slot, info, nullptr, 0)) {
    err_ref_for_current_thread() = 18;  // ERROR_NO_MORE_FILES
    return 0;
  }
  return 1;
#endif
}

int TRT_STDCALL trt_FindClose(void* handle) {
  const u32 v = (u32)(uintptr_t)handle;
  if ((v >> 20) != 0x888 || (v & 0xFFFFF) >= 32 ||
      !g_find_used[v & 0xFFFFF])
    return 0;  // FALSE — unknown or already-freed handle
  find_free((int)(v & 0xFFFFF));
  return 1;
}

void* TRT_STDCALL trt_FindFirstFileW(const u16* name, void* info) {
  return find_first_file(name, info);
}

// ANSI find wrappers: the W family above is the single implementation; these
// narrow a guest path and widen the 592-byte WIN32_FIND_DATAW back down to the
// 320-byte WIN32_FIND_DATAA layout. Plenty of Win32 apps (and packers, and
// this repo's agent guest) import FindFirstFileA and used to fall through to
// the real host API, where a guest C:\ path does not exist.
static void find_w_layout_to_a(const u8* w, u8* a) {
  memset(a, 0, 320);
  memcpy(a, w, 44);  // attrs + three FILETIMEs + size hi/lo + two reserved
  const u16* wn = (const u16*)(w + 44);
  char* an = (char*)(a + 44);
  for (int i = 0; i < 259 && wn[i]; ++i) an[i] = (char)wn[i];
  const u16* wa = (const u16*)(w + 564);
  char* aa = (char*)(a + 304);
  for (int i = 0; i < 13 && wa[i]; ++i) aa[i] = (char)wa[i];
}

void* TRT_STDCALL trt_FindFirstFileA(const char* name, void* info) {
  if (!name || !info) {
    err_ref_for_current_thread() = 87;  // ERROR_INVALID_PARAMETER
    return (void*)(uintptr_t)-1;
  }
  u16 wname[520];
  int i = 0;
  for (; name[i] && i < 519; ++i) wname[i] = (u16)(u8)name[i];
  wname[i] = 0;
  u8 wd[592];
  void* h = find_first_file(wname, wd);
  if (h == (void*)(uintptr_t)-1) return h;
  find_w_layout_to_a(wd, (u8*)info);
  return h;
}

int TRT_STDCALL trt_FindNextFileA(void* handle, void* info) {
  if (!info) {
    err_ref_for_current_thread() = 87;  // ERROR_INVALID_PARAMETER
    return 0;
  }
  u8 wd[592];
  int ok = trt_FindNextFileW(handle, wd);
  if (ok) find_w_layout_to_a(wd, (u8*)info);
  return ok;
}

int TRT_STDCALL trt_GetFileAttributesExW(const u16* name, u32 level, void* info) {
  if (!name || !info) return 0;
  (void)level;
  int is_dir = 0;
  u64 sz = 0;
  // Disc images answer from their own records: a size query never extracts
  // the file (a 2 GB pak must not be copied to answer "how big is it?").
  if (trt_vfs_is_image_utf16(name)) {
    char rel[1024] = "";
    struct trt_iso* iso = trt_vfs_image_for_guest_utf16(name, rel, sizeof(rel));
    if (!iso || trt_iso_stat(iso, rel, &is_dir, &sz) != 0) {
      err_ref_for_current_thread() = 2;  // FILE_NOT_FOUND
      return 0;
    }
    u32* d = (u32*)info;
    memset(d, 0, 36);
    d[0] = is_dir ? 0x10u : 0x80u;
    if (!is_dir) {
      d[7] = (u32)(sz >> 32);
      d[8] = (u32)(sz & 0xFFFFFFFFu);
    }
    return 1;
  }
  char host[1024];
  if (trt_vfs_resolve_utf16_rw(name, 0, host, sizeof(host)) != 0) {
    err_ref_for_current_thread() = 2;
    return 0;
  }
  if (trt_vfs_stat(host, &is_dir, &sz) != 0) {
    err_ref_for_current_thread() = 2;  // FILE_NOT_FOUND
    return 0;
  }
  // WIN32_FILE_ATTRIBUTE_DATA (x64, real Win32 layout, 36 bytes):
  // attrs +0, times +4/+12/+20, size hi +28 / lo +32.
  u32* d = (u32*)info;
  memset(d, 0, 36);
  d[0] = is_dir ? 0x10u : 0x80u;
  if (!is_dir) {
    d[7] = (u32)(sz >> 32);          // nFileSizeHigh (+28)
    d[8] = (u32)(sz & 0xFFFFFFFFu);  // nFileSizeLow (+32)
  }
  return 1;
}

int TRT_STDCALL trt_SetFilePointerEx(void* h, i64 dist, i64* new_pos, u32 method) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  file_rec* rec = file_at_locked(h);
  if (!rec) return 0;
  // .252 FIX: directory handles are not FILE* -- fail (real
  // Windows: SetFilePointerEx on a directory fails).
  if (rec->mode == 'd') return 0;
  FILE* os = (FILE*)rec->os;
  const int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
  if (!os_seek64(os, dist, whence)) return 0;
  if (new_pos) *new_pos = os_tell64(os);
  return 1;
}

int TRT_STDCALL trt_FlushFileBuffers(void* h) {
  FILE* os = file_os(h);
  if (!os) return 0;
  return fflush(os) == 0 ? 1 : 0;
}

u32 TRT_STDCALL trt_GetFileType(void* h) {
  // FILE_TYPE_DISK = 1 (every VFS handle is a disk file; no console/pipe).
  // .252 FIX: directory handles (mode 'd') are disk objects too --
  // check the record directly (file_os returns null for them,
  // correctly, since they hold a HANDLE not a FILE*).
  std::lock_guard<std::mutex> lk(g_files_mu);
  return file_at_locked(h) ? 1u : 0u;
}

u32 TRT_STDCALL trt_GetCurrentProcessId(void) { return 4242; }
u32 TRT_STDCALL trt_GetProcessId(void* proc) {
  (void)proc;
  return 4242;
}

int TRT_STDCALL trt_GetModuleHandleExW(u32 flags, const u16* name, void** out) {
  if (!out) {
    err_ref_for_current_thread() = 87;  // INVALID_PARAM
    return 0;
  }
  *out = nullptr;
  if (flags & 0x4u) {  // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
    if (!name) {
      err_ref_for_current_thread() = 87;
      return 0;
    }
    if (trt_image* img = find_guest_image_for_pc((u64)(uintptr_t)name)) {
      *out = img->base;
      return 1;
    }
#ifdef _WIN32
    PVOID base = nullptr;
    if (::RtlPcToFileHeader((PVOID)(uintptr_t)name, &base) && base) {
      *out = base;
      return 1;
    }
#endif
    err_ref_for_current_thread() = 126;  // MOD_NOT_FOUND
    return 0;
  }
  *out = trt_GetModuleHandleW(name);
  if (!*out) {
    err_ref_for_current_thread() = 126;
    return 0;
  }
  return 1;
}

void* TRT_STDCALL trt_GetModuleHandleA(const char* name) {
  if (!name) return trt_GetModuleHandleW(nullptr);
  u16 w[256];
  u32 i = 0;
  for (; name[i] && i < 255; ++i) w[i] = (u16)(u8)name[i];
  w[i] = 0;
  return trt_GetModuleHandleW(w);
}

// GetSystemTime = SYSTEMTIME (8 words); the CRT only compares it, a real
// conversion of the same clock is the honest answer
int TRT_STDCALL trt_GetSystemTimeAsFileTime(u64* out) {
  if (!out) return 0;
  // Unix epoch -> FILETIME (1601) : +11644473600 seconds
#ifdef _WIN32
  FILETIME ftnow;
  ::GetSystemTimeAsFileTime(&ftnow);
  *out = ((u64)ftnow.dwHighDateTime << 32) | ftnow.dwLowDateTime;
  return 1;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  i64 ft = ((i64)ts.tv_sec + 11644473600ll) * 10000000ll + ts.tv_nsec / 100;
  *out = (u64)ft;
  return 1;
#endif
}

void TRT_STDCALL trt_GetSystemTime(u16* st) {
  if (!st) return;
  u64 ft = 0;
  trt_GetSystemTimeAsFileTime(&ft);
  // FILETIME -> SYSTEMTIME (days since 1601 -> y/m/d h:m:s.ms)
  i64 t = (i64)ft / 10000000ll;                    // whole seconds
  u16 ms = (u16)(((i64)ft % 10000000ll) / 10000);
  i64 days = t / 86400; i64 secs = t % 86400;
  // civil-from-days (Howard Hinnant's algorithm)
  days += 719468;
  i64 era = (days >= 0 ? days : days - 146096) / 146097;
  i64 doe = days - era * 146097;
  i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  i64 y = yoe + era * 400;
  i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
  i64 mp = (5*doy + 2)/153;
  i64 d = doy - (153*mp+2)/5 + 1;
  i64 m = mp < 10 ? mp+3 : mp-9;
  y += (m <= 2);
  u16 w[8];
  w[0] = (u16)y; w[1] = (u16)m; w[2] = (u16)(d - 1) % 7 + 0; w[3] = (u16)d;
  w[4] = (u16)(secs/3600); w[5] = (u16)((secs%3600)/60); w[6] = (u16)(secs%60);
  w[7] = ms;
  for (int i = 0; i < 8; ++i) st[i] = w[i];
}

// ---- Cross-bridge declarations for ANSI / Ex / handle-refcounted kernel32 ----
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                  const u16* name);
int TRT_STDCALL trt_SetEvent(u64 handle);
int TRT_STDCALL trt_ResetEvent(u64 handle);
u64 TRT_STDCALL trt_CreateSemaphoreW(void* attrs, u32 initial, u32 maximum,
                                      const u16* name);
u32 TRT_STDCALL trt_ExitThread(u32 code);
int TRT_STDCALL trt_GetComputerNameW(u16* buf, u32* size);
int TRT_STDCALL trt_DeleteFileW(const u16* path);
int TRT_STDCALL trt_CopyFileW(const u16* src, const u16* dst, bool fail);
int TRT_STDCALL trt_MultiByteToWideChar(u32 codepage, u32 flags,
                                          const char* src, int srclen,
                                          u16* dst, int dstlen);
int TRT_STDCALL trt_WideCharToMultiByte(u32 codepage, u32 flags,
                                          const u16* src, int srclen,
                                          char* dst, int dstlen,
                                          const char* def, void* used);
u32 TRT_STDCALL trt_FormatMessageW(u32 flags, const void* src, u32 msgid,
                                   u32 langid, u16* buf, u32 size,
                                   void* args);

namespace {

int k32_narrow_to_u16(const char* src, int srclen, u16* dst, int dstcap) {
  if (!src || !dst || dstcap <= 0) return 0;
  const int in_len = (srclen < 0) ? (int)strlen(src) : srclen;
  int out = 0;
  for (int i = 0; i < in_len && out + 1 < dstcap; ++i) {
    const unsigned char c = (unsigned char)src[i];
    if (c < 0x80) {
      dst[out++] = (u16)c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < in_len &&
               (((unsigned char)src[i + 1]) & 0xC0) == 0x80) {
      const u32 cp = ((u32)(c & 0x1F) << 6) | ((u32)src[i + 1] & 0x3F);
      dst[out++] = (u16)cp;
      ++i;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < in_len &&
               (((unsigned char)src[i + 1]) & 0xC0) == 0x80 &&
               (((unsigned char)src[i + 2]) & 0xC0) == 0x80) {
      const u32 cp = ((u32)(c & 0x0F) << 12) |
                     (((u32)src[i + 1] & 0x3F) << 6) |
                     ((u32)src[i + 2] & 0x3F);
      dst[out++] = (u16)cp;
      i += 2;
    } else {
      dst[out++] = (u16)c;
    }
  }
  dst[out] = 0;
  return out;
}

u16 k32_ascii_lower(u16 c) {
  return (c >= 'A' && c <= 'Z') ? (u16)(c + ('a' - 'A')) : c;
}

struct dll_dir_slot { char path[512]; bool used; };
dll_dir_slot g_dll_dirs[32] = {};

struct wait_timer_slot {
  u64 handle;
  std::atomic<uint32_t> gen;
};
wait_timer_slot g_wait_timers[64] = {};

struct timer_fire_arg {
  wait_timer_slot* slot;
  uint32_t gen;
  u32 delay_ms;
  u32 period_ms;
};

void timer_run(timer_fire_arg* arg) {
  auto wait_interval = [arg](u32 duration_ms) -> bool {
    u32 rem = duration_ms;
    while (rem > 0) {
      if (arg->slot->gen.load() != arg->gen) return false;
      const u32 step = rem > 10 ? 10 : rem;
      trt_Sleep(step);
      rem -= step;
    }
    return arg->slot->gen.load() == arg->gen;
  };

  if (!wait_interval(arg->delay_ms)) {
    free(arg);
    return;
  }

  do {
    if (arg->slot->gen.load() != arg->gen || !arg->slot->handle) break;
    trt_SetEvent(arg->slot->handle);
    if (arg->period_ms == 0 || !wait_interval(arg->period_ms)) break;
  } while (true);
  free(arg);
}

#if defined(_WIN32)
DWORD WINAPI timer_win_entry(LPVOID p) { timer_run((timer_fire_arg*)p); return 0; }
#else
void* timer_posix_entry(void* p) { timer_run((timer_fire_arg*)p); return nullptr; }
#endif

wait_timer_slot* find_or_alloc_timer(u64 h, bool alloc) {
  if (!h) return nullptr;
  wait_timer_slot* free_slot = nullptr;
  for (int i = 0; i < 64; ++i) {
    if (g_wait_timers[i].handle == h) return &g_wait_timers[i];
    if (!free_slot && g_wait_timers[i].handle == 0) free_slot = &g_wait_timers[i];
  }
  if (alloc && free_slot) {
    free_slot->handle = h;
    free_slot->gen.store(1);
    return free_slot;
  }
  return nullptr;
}

char* g_enva_blocks[16] = {};

} // namespace

int TRT_STDCALL trt_DuplicateHandle(void* src_proc, void* src_h,
                                    void* dst_proc, void** out_h,
                                    u32 access, int inherit, u32 options) {
  (void)access; (void)inherit;
  if (!out_h) {
    err_ref_for_current_thread() = 87;  // INVALID_PARAMETER
    return 0;
  }
  *out_h = nullptr;
  const uintptr_t sp = (uintptr_t)src_proc;
  const uintptr_t dp = (uintptr_t)dst_proc;
  const bool valid_proc =
      (sp == (uintptr_t)-1 || sp == 0xFFFFFFFFu || sp == 0) &&
      (dp == (uintptr_t)-1 || dp == 0xFFFFFFFFu || dp == 0);
  if (!valid_proc && src_proc != dst_proc) {
    trt_core_log("kernel32: DuplicateHandle foreign process unsupported -> FALSE");
    err_ref_for_current_thread() = 5;  // ACCESS_DENIED
    return 0;
  }
  if (!src_h) {
    err_ref_for_current_thread() = 6;  // INVALID_HANDLE
    return 0;
  }
  const uintptr_t hv = (uintptr_t)src_h;
  if ((hv >> 20) == 0x777) {
    if (!trt_file_retain(src_h)) {
      err_ref_for_current_thread() = 6;
      return 0;
    }
    if (options & 1u) trt_CloseHandle(src_h);
    *out_h = src_h;
    return 1;
  }
  if (hv == (uintptr_t)(intptr_t)-2) {  // GetCurrentThread() pseudo-handle
    const u64 th = trt_thread_handle_for_current();
    if (th) {
      *out_h = (void*)(uintptr_t)th;
      return 1;
    }
  }
  if ((hv & 0xFFF00000u) == 0x5B000000u) {
    if (!trt_retain_sync_handle((u64)hv)) {
      err_ref_for_current_thread() = 6;
      return 0;
    }
    if (options & 1u) trt_CloseHandle(src_h);
    *out_h = src_h;
    return 1;
  }
  *out_h = src_h;
  return 1;
}

void* TRT_STDCALL trt_AddVectoredExceptionHandler(u32 first, void* handler) {
  if (!handler) {
    set_err_for_current_thread(87);
    return nullptr;
  }
#if defined(_WIN32)
  static std::once_flag s_os_veh_once;
  std::call_once(s_os_veh_once, []() {
    using pfn_add_veh = PVOID (WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = ::GetModuleHandleW(L"kernel32.dll");
    if (kb) {
      auto fn = (pfn_add_veh)::GetProcAddress(kb, "AddVectoredExceptionHandler");
      if (fn) fn(1u, &trt_win32_os_veh);
    }
  });
#endif
  std::lock_guard<std::mutex> lk(g_veh_mu);
  size_t idx = (size_t)-1;
  for (size_t i = 0; i < g_vehs.size(); ++i) {
    if (!g_vehs[i].active) { idx = i; break; }
  }
  if (idx == (size_t)-1) {
    idx = g_vehs.size();
    g_vehs.push_back({nullptr, 0, false});
  }
  const u64 seq = g_veh_seq.fetch_add(1);
  const u64 cookie = 0x7E00000000ULL + seq;
  g_vehs[idx] = {handler, cookie, true};
  if (first) g_veh_order.insert(g_veh_order.begin(), idx);
  else g_veh_order.push_back(idx);
  return (void*)(uintptr_t)cookie;
}

u32 TRT_STDCALL trt_RemoveVectoredExceptionHandler(void* handle) {
  const u64 hv = (u64)(uintptr_t)handle;
  if (hv <= 0x7E00000000ULL) return 0;
  std::lock_guard<std::mutex> lk(g_veh_mu);
  for (size_t i = 0; i < g_vehs.size(); ++i) {
    if (g_vehs[i].active && g_vehs[i].cookie == hv) {
      g_vehs[i].active = false;
      g_vehs[i].fn = nullptr;
      for (size_t j = 0; j < g_veh_order.size(); ++j) {
        if (g_veh_order[j] == i) {
          g_veh_order.erase(g_veh_order.begin() + (ptrdiff_t)j);
          break;
        }
      }
      return 1;
    }
  }
  return 0;
}

u64 TRT_STDCALL trt_CreateEventExW(void* attrs, const u16* name, u32 flags,
                                    u32 access) {
  (void)access;
  return trt_CreateEventW(attrs, (flags & 1u) != 0, (flags & 2u) != 0, name);
}

u64 TRT_STDCALL trt_CreateEventExA(void* attrs, const char* name, u32 flags,
                                    u32 access) {
  u16 wname[256];
  const u16* wp = nullptr;
  if (name) {
    k32_narrow_to_u16(name, -1, wname, 256);
    wp = wname;
  }
  return trt_CreateEventExW(attrs, wp, flags, access);
}

u64 TRT_STDCALL trt_CreateSemaphoreExW(void* attrs, u32 initial, u32 maximum,
                                        const u16* name, u32 flags,
                                        u32 access) {
  (void)flags; (void)access;
  return trt_CreateSemaphoreW(attrs, initial, maximum, name);
}

u64 TRT_STDCALL trt_CreateSemaphoreExA(void* attrs, u32 initial, u32 maximum,
                                        const char* name, u32 flags,
                                        u32 access) {
  u16 wname[256];
  const u16* wp = nullptr;
  if (name) {
    k32_narrow_to_u16(name, -1, wname, 256);
    wp = wname;
  }
  return trt_CreateSemaphoreExW(attrs, initial, maximum, wp, flags, access);
}

void* TRT_STDCALL trt_CreateWaitableTimerExW(void* attrs, const u16* name,
                                             u32 flags, u32 access) {
  (void)access;
  const bool manual = (flags & 1u) != 0;
  const u64 h = trt_CreateEventW(attrs, manual, false, name);
  if (h) find_or_alloc_timer(h, true);
  return (void*)(uintptr_t)h;
}

void* TRT_STDCALL trt_CreateWaitableTimerExA(void* attrs, const char* name,
                                             u32 flags, u32 access) {
  u16 wname[256];
  const u16* wp = nullptr;
  if (name) {
    k32_narrow_to_u16(name, -1, wname, 256);
    wp = wname;
  }
  return trt_CreateWaitableTimerExW(attrs, wp, flags, access);
}

int TRT_STDCALL trt_SetWaitableTimer(void* h, const int64_t* due_time,
                                     int32_t period, void* comp_routine,
                                     void* comp_arg, int resume) {
  (void)comp_arg; (void)resume;
  if (!h) {
    err_ref_for_current_thread() = 6;
    return 0;
  }
  if (!due_time || period < 0) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  if (comp_routine) {
    trt_core_log("kernel32: SetWaitableTimer APC completion callback is "
                 "unsupported; timer event will still be signaled");
  }

  const u64 uh = (u64)(uintptr_t)h;
  wait_timer_slot* slot = find_or_alloc_timer(uh, true);
  if (!slot) {
    err_ref_for_current_thread() = 8;
    return 0;
  }
  const uint32_t gen = slot->gen.fetch_add(1) + 1;
  trt_ResetEvent(uh);

  // Windows uses a negative due time for a relative delay and a positive
  // FILETIME for an absolute UTC deadline. Convert both without the old
  // one-minute cap; retain a u32 millisecond interval for the worker.
  u64 delay_ms64 = 0;
  if (*due_time < 0) {
    const u64 ticks = (u64)(-(*due_time + 1)) + 1;
    delay_ms64 = ticks / 10000ull + ((ticks % 10000ull) != 0 ? 1ull : 0ull);
  } else if (*due_time > 0) {
    u64 now_ft = 0;
    if (trt_GetSystemTimeAsFileTime(&now_ft) &&
        (u64)*due_time > now_ft) {
      const u64 ticks = (u64)*due_time - now_ft;
      delay_ms64 = ticks / 10000ull + ((ticks % 10000ull) != 0 ? 1ull : 0ull);
    }
  }
  const u32 max_delay = 0xFFFFFFFEu;
  u32 delay_ms = delay_ms64 > max_delay ? max_delay : (u32)delay_ms64;
  const u32 period_ms = (u32)period;

  if (period_ms != 0) {
    char line[192];
    snprintf(line, sizeof(line),
             "kernel32: SetWaitableTimer periodic due=%lld initial=%u ms "
             "period=%u ms handle=%p",
             (long long)*due_time, delay_ms, period_ms, h);
    trt_core_log(line);
  }

  if (delay_ms == 0 && period_ms == 0) {
    trt_SetEvent(uh);
    err_ref_for_current_thread() = 0;
    return 1;
  }

  // A periodic timer with an immediate due time fires now, then starts its
  // recurring interval. The timer worker also handles one-shot relative and
  // absolute deadlines without blocking the caller.
  if (delay_ms == 0) {
    trt_SetEvent(uh);
    delay_ms = period_ms;
  }

  timer_fire_arg* arg = (timer_fire_arg*)malloc(sizeof(timer_fire_arg));
  if (!arg) {
    err_ref_for_current_thread() = 8;
    return 0;
  }
  arg->slot = slot;
  arg->gen = gen;
  arg->delay_ms = delay_ms;
  arg->period_ms = period_ms;
#if defined(_WIN32)
  HANDLE th = ::CreateThread(nullptr, 0, timer_win_entry, arg, 0, nullptr);
  if (th) {
    ::CloseHandle(th);
    err_ref_for_current_thread() = 0;
    return 1;
  }
#else
  pthread_t pt;
  if (pthread_create(&pt, nullptr, timer_posix_entry, arg) == 0) {
    pthread_detach(pt);
    err_ref_for_current_thread() = 0;
    return 1;
  }
#endif
  free(arg);
  err_ref_for_current_thread() = 8;
  return 0;
}

int TRT_STDCALL trt_CancelWaitableTimer(void* h) {
  if (!h) {
    err_ref_for_current_thread() = 6;
    return 0;
  }
  const u64 uh = (u64)(uintptr_t)h;
  if (wait_timer_slot* slot = find_or_alloc_timer(uh, false)) {
    slot->gen.fetch_add(1);
  }
  trt_ResetEvent(uh);
  return 1;
}

void* TRT_STDCALL trt_CreateFileA(const char* name, u32 access, u32 share,
                                  void* sec, u32 disp, u32 flags, void* tmpl) {
  if (!name) {
    err_ref_for_current_thread() = 87;
    return (void*)(uintptr_t)-1;
  }
  u16 wpath[1024];
  k32_narrow_to_u16(name, -1, wpath, 1024);
  return trt_CreateFileW(wpath, access, share, sec, disp, flags, tmpl);
}

int TRT_STDCALL trt_DeleteFileA(const char* path) {
  if (!path) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  u16 wpath[1024];
  k32_narrow_to_u16(path, -1, wpath, 1024);
  return trt_DeleteFileW(wpath);
}

int TRT_STDCALL trt_CopyFileExW(const u16* src, const u16* dst, void* progress,
                                  void* data, int* cancel, u32 flags) {
  (void)progress; (void)data;
  if (cancel && *cancel) {
    err_ref_for_current_thread() = 1235;  // REQUEST_ABORTED
    return 0;
  }
  return trt_CopyFileW(src, dst, (flags & 1u) != 0);
}

int TRT_STDCALL trt_CopyFile2(const u16* src, const u16* dst, const void* ext) {
  bool fail_if_exists = false;
  if (ext) {
    const u32* hdr = (const u32*)ext;
    if (hdr[0] >= 8) fail_if_exists = (hdr[1] & 1u) != 0;
  }
  if (trt_CopyFileW(src, dst, fail_if_exists)) return 0;  // S_OK
  const u32 err = err_ref_for_current_thread();
  const u32 code = err ? err : 2;  // FILE_NOT_FOUND fallback
  return (int)(0x80070000u | (code & 0xFFFFu));
}

int TRT_STDCALL trt_AreFileApisANSI(void) { return 1; }

u32 TRT_STDCALL trt_GetCurrentProcessorNumber(void) {
#if defined(_WIN32)
  return ::GetCurrentProcessorNumber();
#else
  return 0;
#endif
}

u32 TRT_STDCALL trt_GetCurrentDirectoryA(u32 len, char* buf) {
  u16 wbuf[1024];
  const int wlen = trt_GetCurrentDirectoryW(1024, wbuf);
  if (wlen <= 0) return 0;
  char utf8[1024];
  int n = trt_WideCharToMultiByte(65001, 0, wbuf, wlen, utf8,
                                  (int)sizeof(utf8) - 1, nullptr, nullptr);
  if (n < 0) n = 0;
  utf8[n] = 0;
  if (!buf || len < (u32)n + 1) return (u32)n + 1;
  memcpy(buf, utf8, (size_t)n + 1);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetCurrentDirectoryA -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, utf8); trt_core_log(st_line); }
  return (u32)n;
}

int TRT_STDCALL trt_GetComputerNameA(char* buf, u32* size) {
  if (!size) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  u16 wbuf[128];
  u32 wlen = 128;
  if (!trt_GetComputerNameW(wbuf, &wlen)) return 0;
  char utf8[256];
  int n = trt_WideCharToMultiByte(65001, 0, wbuf, (int)wlen, utf8,
                                  (int)sizeof(utf8) - 1, nullptr, nullptr);
  if (n < 0) n = 0;
  utf8[n] = 0;
  if (!buf || *size < (u32)n + 1) {
    *size = (u32)n + 1;
    err_ref_for_current_thread() = 111;  // BUFFER_OVERFLOW
    return 0;
  }
  memcpy(buf, utf8, (size_t)n + 1);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetComputerNameA -> buf=%p value=\"%.128s\"", (void*)buf, utf8); trt_core_log(st_line); }
  *size = (u32)n;
  return 1;
}

const char* TRT_STDCALL trt_GetEnvironmentStringsA(void) {
  const u16* w = trt_GetEnvironmentStringsW();
  if (!w) return "\0";
  size_t wlen = 0;
  while (!(w[wlen] == 0 && w[wlen + 1] == 0)) {
    ++wlen;
    if (wlen > 32768) break;
  }
  const size_t total_w = wlen + 2;
  const size_t cap = total_w * 3 + 2;
  char* out = (char*)malloc(cap);
  if (!out) return "\0";
  size_t pos = 0;
  for (size_t i = 0; i < total_w && pos + 2 < cap; ++i) {
    const u16 c = w[i];
    if (c < 0x80) {
      out[pos++] = (char)c;
    } else if (c < 0x800) {
      out[pos++] = (char)(0xC0 | (c >> 6));
      out[pos++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[pos++] = '?';
    }
  }
  out[pos] = 0;
  out[pos + 1] = 0;
  for (int k = 0; k < 16; ++k) {
    if (!g_enva_blocks[k]) { g_enva_blocks[k] = out; break; }
  }
  { char st_tmp[136]; char st_line[256]; if (out) { size_t st_o = 0; for (size_t st_i = 0; (out[st_i] || out[st_i + 1]) && st_o < 130; ++st_i) st_tmp[st_o++] = out[st_i] ? out[st_i] : ';'; st_tmp[st_o] = 0; snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentStringsA -> ptr=%p \"%.128s\"", (const void*)out, st_tmp); } else { snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentStringsA -> NULL"); } trt_core_log(st_line); }
  return out;
}

int TRT_STDCALL trt_FreeEnvironmentStringsA(const char* block) {
  if (!block) return 0;
  for (int k = 0; k < 16; ++k) {
    if (g_enva_blocks[k] == block) {
      free(g_enva_blocks[k]);
      g_enva_blocks[k] = nullptr;
      break;
    }
  }
  return 1;
}

extern void* TRT_STDCALL trt_LocalAlloc(u32 flags, u64 bytes);

u32 TRT_STDCALL trt_FormatMessageA(u32 flags, const void* src, u32 msgid,
                                   u32 langid, char* buf, u32 size,
                                   void* args) {
  if (!buf) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  u16* wbuf_ptr = nullptr;
  u16 wbuf_stack[256];
  u32 wflags = flags;
  u32 wn = 0;
  if (flags & 0x100u) {
    wn = trt_FormatMessageW(wflags, src, msgid, langid, (u16*)&wbuf_ptr, size, args);
  } else {
    wn = trt_FormatMessageW(wflags, src, msgid, langid, wbuf_stack, 256, args);
    wbuf_ptr = wbuf_stack;
  }
  if (wn == 0 || !wbuf_ptr) return 0;

  if (flags & 0x100u) {
    int n = trt_WideCharToMultiByte(65001, 0, wbuf_ptr, (int)wn, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
      extern void* TRT_STDCALL trt_LocalFree(void* p);
      trt_LocalFree(wbuf_ptr);
      return 0;
    }
    char* abuf = (char*)trt_LocalAlloc(0x40, (u64)(n + 1));
    if (!abuf) {
      extern void* TRT_STDCALL trt_LocalFree(void* p);
      trt_LocalFree(wbuf_ptr);
      err_ref_for_current_thread() = 8;
      return 0;
    }
    trt_WideCharToMultiByte(65001, 0, wbuf_ptr, (int)wn, abuf, n, nullptr, nullptr);
    abuf[n] = 0;
    extern void* TRT_STDCALL trt_LocalFree(void* p);
    trt_LocalFree(wbuf_ptr);
    *(char**)buf = abuf;
    return (u32)n;
  } else {
    if (size == 0) {
      err_ref_for_current_thread() = 87;
      return 0;
    }
    int n = trt_WideCharToMultiByte(65001, 0, wbuf_ptr, (int)wn, buf, (int)size - 1, nullptr, nullptr);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (u32)n;
  }
}

int TRT_STDCALL trt_CompareStringOrdinal(const u16* s1, int c1,
                                         const u16* s2, int c2,
                                         int ignore_case) {
  if (!s1 || !s2) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  int n1 = 0;
  if (c1 < 0) { while (s1[n1]) ++n1; } else { n1 = c1; }
  int n2 = 0;
  if (c2 < 0) { while (s2[n2]) ++n2; } else { n2 = c2; }
  const int n = n1 < n2 ? n1 : n2;
  for (int i = 0; i < n; ++i) {
    const u16 a = ignore_case ? k32_ascii_lower(s1[i]) : s1[i];
    const u16 b = ignore_case ? k32_ascii_lower(s2[i]) : s2[i];
    if (a < b) return 1;  // CSTR_LESS_THAN
    if (a > b) return 3;  // CSTR_GREATER_THAN
  }
  if (n1 < n2) return 1;
  if (n1 > n2) return 3;
  return 2;  // CSTR_EQUAL
}

int TRT_STDCALL trt_CompareStringA(u32 lcid, u32 flags, const char* a, int alen,
                                   const char* b, int blen) {
  if (!a || !b) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  const int na = alen < 0 ? (int)strlen(a) : alen;
  const int nb = blen < 0 ? (int)strlen(b) : blen;
  u16 sbuf_a[512], sbuf_b[512];
  u16* wa = (na < 512) ? sbuf_a : (u16*)malloc((size_t)(na + 1) * 2);
  u16* wb = (nb < 512) ? sbuf_b : (u16*)malloc((size_t)(nb + 1) * 2);
  if (!wa || !wb) {
    if (wa && wa != sbuf_a) free(wa);
    if (wb && wb != sbuf_b) free(wb);
    err_ref_for_current_thread() = 8;
    return 0;
  }
  const int walen = k32_narrow_to_u16(a, na, wa, na + 1);
  const int wblen = k32_narrow_to_u16(b, nb, wb, nb + 1);
  const int r = trt_CompareStringW(lcid, flags, wa, walen, wb, wblen);
  if (wa != sbuf_a) free(wa);
  if (wb != sbuf_b) free(wb);
  return r;
}

int TRT_STDCALL trt_CompareFileTime(const void* ft1, const void* ft2) {
  if (!ft1 || !ft2) return 0;
  const u32* a = (const u32*)ft1;
  const u32* b = (const u32*)ft2;
  const u64 va = ((u64)a[1] << 32) | (u64)a[0];
  const u64 vb = ((u64)b[1] << 32) | (u64)b[0];
  if (va < vb) return -1;
  if (va > vb) return 1;
  return 0;
}

void* TRT_STDCALL trt_AddDllDirectory(const u16* new_dir) {
  if (!new_dir || !new_dir[0]) {
    err_ref_for_current_thread() = 87;
    return nullptr;
  }
  char utf8[512];
  if (trt_utf16_to_utf8(new_dir, utf8, sizeof(utf8)) != 0) {
    err_ref_for_current_thread() = 87;
    return nullptr;
  }
  char host_dir[1024] = {};
  if (trt_vfs_resolve_utf16(new_dir, host_dir, sizeof(host_dir)) != 0 ||
      !host_dir[0]) {
    snprintf(host_dir, sizeof(host_dir), "%s", utf8);
  }
  const int rt_slot = trt_runtime_add_dll_dir(host_dir);
  if (rt_slot >= 0 && rt_slot < 32) {
    snprintf(g_dll_dirs[rt_slot].path, sizeof(g_dll_dirs[rt_slot].path), "%s",
             utf8);
    g_dll_dirs[rt_slot].used = true;
    char line[600];
    snprintf(line, sizeof(line),
             "kernel32: AddDllDirectory(%s) -> cookie slot=%d (%s)", utf8,
             rt_slot, host_dir);
    trt_core_log(line);
    return (void*)(uintptr_t)(0xDD000000u | (u32)rt_slot);
  }
  err_ref_for_current_thread() = 8;
  return nullptr;
}

int TRT_STDCALL trt_RemoveDllDirectory(void* cookie) {
  const uintptr_t cv = (uintptr_t)cookie;
  if ((cv & ~(uintptr_t)0x1Fu) == 0xDD000000u) {
    const u32 idx = (u32)(cv & 0x1Fu);
    trt_runtime_remove_dll_dir((int)idx);
    if (g_dll_dirs[idx].used) {
      g_dll_dirs[idx].used = false;
      g_dll_dirs[idx].path[0] = 0;
      return 1;
    }
  }
  err_ref_for_current_thread() = 87;
  return 0;
}

int TRT_STDCALL trt_SetDefaultDllDirectories(u32 flags) {
  (void)flags;
  return 1;
}

i32 TRT_STDCALL trt_AppPolicyGetProcessTerminationMethod(void* token,
                                                         u32* policy) {
  (void)token;
  if (!policy) return 87;  // ERROR_INVALID_PARAMETER
  *policy = 0;  // AppPolicyProcessTerminationMethod_ExitProcess
  return 0;     // ERROR_SUCCESS
}

void TRT_STDCALL trt_FreeLibraryAndExitThread(void* mod, u32 exit_code) {
  trt_FreeLibrary(mod);
  trt_ExitThread(exit_code);
}

void TRT_STDCALL trt_DebugBreak(void) {
  trt_core_log("kernel32: DebugBreak() intercepted (offline bridge; no trap)");
}

void TRT_STDCALL trt_FatalAppExitA(u32 action, const char* msg) {
  (void)action;
  char line[320];
  snprintf(line, sizeof(line), "kernel32: FatalAppExitA: %s",
           msg ? msg : "(null)");
  trt_core_log(line);
  trt_ExitProcess(1);
}

int TRT_STDCALL trt_EnumSystemLocalesA(void* cb, u32 flags) {
  (void)flags;
  if (!cb) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  typedef int (TRT_STDCALL *locale_enum_a_fn)(const char*);
  locale_enum_a_fn fn = (locale_enum_a_fn)cb;
  static const char* const k_lcids[] = {
      "00000409", "00000411", "00000407", "0000040C", "00000C0A", "00000410",
      "00000412", "00000804", "00000404", "00000419", "00000416", "00000415",
  };
  for (const char* lcid : k_lcids) {
    if (!fn(lcid)) return 1;
  }
  return 1;
}

int TRT_STDCALL trt_EnumSystemLocalesW(void* cb, u32 flags) {
  (void)flags;
  if (!cb) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  typedef int (TRT_STDCALL *locale_enum_w_fn)(const u16*);
  locale_enum_w_fn fn = (locale_enum_w_fn)cb;
  static const char* const k_lcids[] = {
      "00000409", "00000411", "00000407", "0000040C", "00000C0A", "00000410",
      "00000412", "00000804", "00000404", "00000419", "00000416", "00000415",
  };
  for (const char* lcid : k_lcids) {
    u16 lcid16[16] = {};
    for (int i = 0; lcid[i] && i < 15; ++i) lcid16[i] = (u16)(unsigned char)lcid[i];
    if (!fn(lcid16)) return 1;
  }
  return 1;
}

int TRT_STDCALL trt_EnumSystemLocalesEx(void* cb, u32 flags, i64 lparam,
                                        void* reserved) {
  (void)reserved;
  if (!cb) {
    err_ref_for_current_thread() = 87;
    return 0;
  }
  typedef int (TRT_STDCALL *locale_enum_ex_fn)(const u16*, u32, i64);
  locale_enum_ex_fn fn = (locale_enum_ex_fn)cb;
  static const char* const k_locale_names[] = {
      "en-US", "ja-JP", "de-DE", "fr-FR", "es-ES", "it-IT",
      "ko-KR", "zh-CN", "zh-TW", "ru-RU", "pt-BR", "pl-PL",
  };
  const u32 f = flags ? flags : 1u;
  for (const char* loc : k_locale_names) {
    u16 loc16[32] = {};
    for (int i = 0; loc[i] && i < 31; ++i) loc16[i] = (u16)(unsigned char)loc[i];
    if (!fn(loc16, f, lparam)) return 1;
  }
  if (flags == 0 || (flags & 0x10u)) {
    const u16 inv[] = {0};
    fn(inv, 0x10u, lparam);
  }
  return 1;
}

int TRT_STDCALL trt_CreatePipe(void** read_pipe, void** write_pipe,
                               void* sec_attrs, u32 size) {
  (void)sec_attrs; (void)size;
  if (read_pipe) *read_pipe = nullptr;
  if (write_pipe) *write_pipe = nullptr;
  trt_core_log("kernel32: CreatePipe -> FALSE (anonymous pipes absent in offline VFS)");
  err_ref_for_current_thread() = 120;  // CALL_NOT_IMPLEMENTED
  return 0;
}

void* TRT_STDCALL trt_CreateNamedPipeW(const u16* name, u32 open_mode,
                                       u32 pipe_mode, u32 max_inst, u32 out_sz,
                                       u32 in_sz, u32 timeout, void* sec) {
  (void)open_mode; (void)pipe_mode; (void)max_inst; (void)out_sz;
  (void)in_sz; (void)timeout; (void)sec;
  char utf8[160] = {};
  if (name) trt_utf16_to_utf8(name, utf8, sizeof(utf8));
  char line[256];
  snprintf(line, sizeof(line),
           "kernel32: CreateNamedPipeW(%s) -> INVALID_HANDLE_VALUE (no IPC pipes)",
           utf8[0] ? utf8 : "(null)");
  trt_core_log(line);
  err_ref_for_current_thread() = 120;
  return (void*)(uintptr_t)-1;
}

int TRT_STDCALL trt_ConnectNamedPipe(void* pipe, void* overlapped) {
  (void)pipe; (void)overlapped;
  trt_core_log("kernel32: ConnectNamedPipe -> FALSE (no IPC pipes)");
  err_ref_for_current_thread() = 6;  // INVALID_HANDLE
  return 0;
}

int TRT_STDCALL trt_CancelIo(void* h) {
  if (!file_os(h)) {
    err_ref_for_current_thread() = 6;
    return 0;
  }
  return 1;  // no pending overlapped requests on synchronous VFS handle
}

int TRT_STDCALL trt_CancelIoEx(void* h, void* overlapped) {
  (void)overlapped;
  if (!file_os(h)) {
    err_ref_for_current_thread() = 6;
    return 0;
  }
  err_ref_for_current_thread() = 1168;  // ERROR_NOT_FOUND
  return 0;
}

int TRT_STDCALL trt_DeviceIoControl(void* dev, u32 ioctl, void* in_buf,
                                    u32 in_sz, void* out_buf, u32 out_sz,
                                    u32* bytes_ret, void* overlapped) {
  (void)dev; (void)in_buf; (void)in_sz; (void)out_buf; (void)out_sz;
  (void)overlapped;
  if (bytes_ret) *bytes_ret = 0;
  char line[128];
  snprintf(line, sizeof(line),
           "kernel32: DeviceIoControl(0x%08x) -> FALSE (not supported)", ioctl);
  trt_core_log(line);
  err_ref_for_current_thread() = 1;  // INVALID_FUNCTION
  return 0;
}

u32 TRT_STDCALL trt_GetConsoleProcessList(u32* list, u32 count) {
  (void)list; (void)count;
  trt_core_log("kernel32: GetConsoleProcessList -> 0 (no console in AppContainer)");
  err_ref_for_current_thread() = 6;
  return 0;
}

int TRT_STDCALL trt_GetConsoleScreenBufferInfo(void* h, void* info) {
  (void)h; (void)info;
  trt_core_log("kernel32: GetConsoleScreenBufferInfo -> FALSE (no console)");
  err_ref_for_current_thread() = 6;
  return 0;
}

void* TRT_STDCALL trt_GetConsoleWindow(void) { return nullptr; }

int TRT_STDCALL trt_AttachConsole(u32 pid) {
  (void)pid;  // ATTACH_PARENT_PROCESS (0xFFFFFFFF) and any pid: same record
  // Honest failure: an AppContainer has no parent console to attach to (real
  // Windows: AttachConsole fails here too). Returning TRUE was a measured
  // livelock — Ultra Pool believed a console existed and flooded
  // GetStdHandle(NULL) forever from its audio/log thread, wedging the main
  // thread on the CS that thread held. FALSE + ERROR_INVALID_HANDLE lets the
  // guest's no-console path proceed exactly as on desktop Windows.
  trt_core_log("kernel32: AttachConsole -> FALSE (no parent console in "
               "AppContainer; guest falls back to no-console path)");
  err_ref_for_current_thread() = 6;  // ERROR_INVALID_HANDLE
  return 0;
}

// AppModel surface (imported via api-ms-win-appmodel-runtime-l1-1-*.dll by
// Python 3.13 / modern C++ CRTs probing Store packaging): return
// APPMODEL_ERROR_NO_PACKAGE (15700) so desktop PEs run in classic Win32 mode.
u32 TRT_STDCALL trt_GetCurrentPackageFullName(u32* len, u16* full_name) {
  (void)full_name;
  if (len) *len = 0;
  return 15700u;
}
u32 TRT_STDCALL trt_GetCurrentPackageFamilyName(u32* len, u16* family_name) {
  (void)family_name;
  if (len) *len = 0;
  return 15700u;
}
u32 TRT_STDCALL trt_GetCurrentApplicationUserModelId(u32* len, u16* aumid) {
  (void)aumid;
  if (len) *len = 0;
  return 15700u;
}
u32 TRT_STDCALL trt_GetCurrentPackageId(u32* len, u8* buf) {
  (void)buf;
  if (len) *len = 0;
  return 15700u;
}
u32 TRT_STDCALL trt_GetCurrentPackagePath(u32* len, u16* path) {
  (void)path;
  if (len) *len = 0;
  return 15700u;
}

// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (0x1) / FILE_SKIP_SET_EVENT_ON_HANDLE
// (0x2), stored per file handle. trt_iocp_notify_io (bridge_thread.cpp)
// consults bit 0 so a SUCCESSFUL overlapped I/O signals the event but does
// NOT post a completion packet — the previous ok-stub handed engines that
// set the flag phantom double-completions (event AND packet).
int TRT_STDCALL trt_SetFileCompletionNotificationModes(void* file_handle, u8 flags) {
  if (!file_handle || file_handle == (void*)(uintptr_t)-1) {
    trt_SetLastError(6u); // ERROR_INVALID_HANDLE
    return 0;
  }
  if (flags & ~0x3u) {
    trt_SetLastError(87u); // ERROR_INVALID_PARAMETER
    char line[160];
    snprintf(line, sizeof(line),
             "kernel32: file completion notification modes flags=0x%x -> "
             "invalid combination (ERROR_INVALID_PARAMETER)", flags);
    trt_core_log(line);
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_files_mu);
    file_rec* rec = file_at_locked(file_handle);
    if (!rec) {
      trt_SetLastError(6u);
      return 0;
    }
    rec->io_modes = flags;
  }
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: file completion notification modes = 0x%x%s%s stored",
           flags,
           (flags & 0x1u) ? " [SKIP_COMPLETION_PORT_ON_SUCCESS]" : "",
           (flags & 0x2u) ? " [SKIP_SET_EVENT_ON_HANDLE]" : "");
  trt_core_log(line);
  return 1;
}

// Consulted by the IOCP notify path (bridge_thread.cpp): bit 0 suppresses
// the success completion packet for this handle.
extern "C" int trt_file_io_modes(void* file_handle) {
  std::lock_guard<std::mutex> lk(g_files_mu);
  const file_rec* rec = file_at_locked(file_handle);
  return rec ? (int)rec->io_modes : 0;
}

// ---- Sprint 1 gap batch: interlocked primitives (master prompt 1B) -------
// Win32 signed semantics: InterlockedIncrement/Decrement return the NEW
// value; InterlockedExchangeAdd/Exchange/CompareExchange return the OLD.
// Windows 'volatile long*' is a 32-bit target and 'volatile __int64*' is
// 64-bit — fixed-width i32/i64 keeps both the ms_abi guest and the host
// honest, and __atomic builtins give the required full-barrier semantics.
i32 TRT_STDCALL trt_InterlockedIncrement(volatile i32* v) {
  return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}
i32 TRT_STDCALL trt_InterlockedDecrement(volatile i32* v) {
  return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST);
}
i32 TRT_STDCALL trt_InterlockedExchangeAdd(volatile i32* v, i32 add) {
  return __atomic_fetch_add(v, add, __ATOMIC_SEQ_CST);
}
i32 TRT_STDCALL trt_InterlockedExchange(volatile i32* v, i32 value) {
  return __atomic_exchange_n(v, value, __ATOMIC_SEQ_CST);
}
i32 TRT_STDCALL trt_InterlockedCompareExchange(volatile i32* dest, i32 exch,
                                               i32 comparand) {
  i32 expected = comparand;
  __atomic_compare_exchange_n(dest, &expected, exch, false,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return expected;  // the OLD value, Windows convention
}
i64 TRT_STDCALL trt_InterlockedIncrement64(volatile i64* v) {
  return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}
i64 TRT_STDCALL trt_InterlockedDecrement64(volatile i64* v) {
  return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST);
}
i64 TRT_STDCALL trt_InterlockedExchangeAdd64(volatile i64* v, i64 add) {
  return __atomic_fetch_add(v, add, __ATOMIC_SEQ_CST);
}
i64 TRT_STDCALL trt_InterlockedCompareExchange64(volatile i64* dest, i64 exch,
                                                 i64 comparand) {
  i64 expected = comparand;
  __atomic_compare_exchange_n(dest, &expected, exch, false,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return expected;
}

// ---- Sprint 1 gap batch: Rtl memory primitives (master prompt 1C) --------
// winnt.h defines CopyMemory/ZeroMemory/FillMemory as macros that expand to
// these Rtl names; ntdll + kernel32 carry the Rtl names as real exports, so
// GetProcAddress(kernel32, "RtlCopyMemory") must bind too. All VOID.
void TRT_STDCALL trt_RtlCopyMemory(void* dst, const void* src, u64 n) {
  memcpy(dst, src, (size_t)n);
}
void TRT_STDCALL trt_RtlMoveMemory(void* dst, const void* src, u64 n) {
  memmove(dst, src, (size_t)n);
}
void TRT_STDCALL trt_RtlZeroMemory(void* dst, u64 n) {
  memset(dst, 0, (size_t)n);
}
void TRT_STDCALL trt_RtlFillMemory(void* dst, u64 n, u8 fill) {
  memset(dst, (int)fill, (size_t)n);
}

extern "C" int TRT_STDCALL trt_ResolveLocaleName(const u16* name, u16* out, int cch);

static const trt_named_fn k_kernel32_fns[] = {
  {"GetProcessHeap", (void*)&trt_GetProcessHeap},
  {"HeapCreate", (void*)&trt_HeapCreate},
  {"HeapSetInformation", (void*)&trt_HeapSetInformation},
  {"HeapAlloc", (void*)&trt_HeapAlloc},
  {"HeapFree", (void*)&trt_HeapFree},
  {"VirtualAlloc", (void*)&trt_VirtualAlloc},
  {"GetWriteWatch", (void*)&trt_GetWriteWatch},
  {"ResetWriteWatch", (void*)&trt_ResetWriteWatch},
  {"VirtualFree", (void*)&trt_VirtualFree},
  {"GetLastError", (void*)&trt_GetLastError},
  {"SetLastError", (void*)&trt_SetLastError},
  {"GetCurrentThreadId", (void*)&trt_GetCurrentThreadId},
  {"GetCurrentProcess", (void*)&trt_GetCurrentProcess},
  {"QueryPerformanceCounter", (void*)&trt_QueryPerformanceCounter},
  {"QueryPerformanceFrequency", (void*)&trt_QueryPerformanceFrequency},
  {"IsProcessorFeaturePresent", (void*)&trt_IsProcessorFeaturePresent},
  {"Sleep", (void*)&trt_Sleep},
  {"GetTickCount", (void*)&trt_GetTickCount},
  {"GetTickCount64", (void*)&trt_GetTickCount64},
  {"GetCurrentThreadStackLimits", (void*)&trt_GetCurrentThreadStackLimits},
  {"CreateFileW", (void*)&trt_CreateFileW},
  {"ReadFile", (void*)&trt_ReadFile},
  {"WriteFile", (void*)&trt_WriteFile},
  {"ReadFileEx", (void*)&trt_ReadFileEx},
  {"WriteFileEx", (void*)&trt_WriteFileEx},
  {"CloseHandle", (void*)&trt_CloseHandle},
  {"SetFilePointer", (void*)&trt_SetFilePointer},
  {"GetFileSize", (void*)&trt_GetFileSize},
  {"SetCurrentDirectoryW", (void*)&trt_SetCurrentDirectoryW},
  {"SetCurrentDirectoryA", (void*)&trt_SetCurrentDirectoryA},
  {"GetCurrentDirectoryW", (void*)&trt_GetCurrentDirectoryW},
  {"GetEnvironmentStringsW", (void*)&trt_GetEnvironmentStringsW},
  {"FreeEnvironmentStringsW", (void*)&trt_FreeEnvironmentStringsW},
  {"GetModuleFileNameW", (void*)&trt_GetModuleFileNameW},
  {"GetSystemDirectoryW", (void*)&trt_GetSystemDirectoryW},
  {"LoadLibraryW", (void*)&trt_LoadLibraryW},
  {"LoadLibraryExW", (void*)&trt_LoadLibraryExW},
  {"FreeLibrary", (void*)&trt_FreeLibrary},
  {"GetProcAddress", (void*)&trt_GetProcAddress},
  {"GetModuleHandleW", (void*)&trt_GetModuleHandleW},
  // the console/child-process honesty surface (OrTMM's CRT imports)
  {"GetStdHandle", (void*)&trt_GetStdHandle},
  {"SetStdHandle", (void*)&trt_SetStdHandle},
  {"GetConsoleMode", (void*)&trt_GetConsoleMode},
  {"ReadConsoleW", (void*)&trt_ReadConsoleW},
  {"WriteConsoleW", (void*)&trt_WriteConsoleW},
  {"SetConsoleCtrlHandler", (void*)&trt_SetConsoleCtrlHandler},
  {"CreateProcessW", (void*)&trt_CreateProcessW},
  // ---- STAGE 7: EXE boot / MSVC CRT surface ----
  {"ExitProcess", (void*)&trt_ExitProcess},
  {"FatalExit", (void*)&trt_FatalExit},
  {"TerminateProcess", (void*)&trt_TerminateProcess},
  {"GetStartupInfoW", (void*)&trt_GetStartupInfoW},
  {"GetStartupInfoA", (void*)&trt_GetStartupInfoA},
  {"GetCommandLineA", (void*)&trt_GetCommandLineA},
  {"GetCommandLineW", (void*)&trt_GetCommandLineW},
  {"HeapSize", (void*)&trt_HeapSize},
  {"HeapReAlloc", (void*)&trt_HeapReAlloc},
  {"HeapQueryInformation", (void*)&trt_HeapQueryInformation},
  {"FlsAlloc", (void*)&trt_FlsAlloc},
  {"FlsGetValue", (void*)&trt_FlsGetValue},
  {"FlsGetValue2", (void*)&trt_FlsGetValue2},
  {"FlsSetValue", (void*)&trt_FlsSetValue},
  {"FlsFree", (void*)&trt_FlsFree},
  {"GetCPInfo", (void*)&trt_GetCPInfo},
  {"GetConsoleCP", (void*)&trt_GetConsoleCP},
  {"GetConsoleOutputCP", (void*)&trt_GetConsoleOutputCP},
  {"GetStringTypeW", (void*)&trt_GetStringTypeW},
  {"LCMapStringW", (void*)&trt_LCMapStringW},
  {"LCMapStringEx", (void*)&trt_LCMapStringEx},
  {"CompareStringW", (void*)&trt_CompareStringW},
  {"CompareStringEx", (void*)&trt_CompareStringEx},
  {"RtlCaptureContext", (void*)&trt_RtlCaptureContext},
  {"RtlLookupFunctionEntry", (void*)&trt_RtlLookupFunctionEntry},
  {"RtlVirtualUnwind", (void*)&trt_RtlVirtualUnwind},
  {"RtlPcToFileHeader", (void*)&trt_RtlPcToFileHeader},
  {"RtlAddFunctionTable", (void*)&trt_RtlAddFunctionTable},
  {"RtlDeleteFunctionTable", (void*)&trt_RtlDeleteFunctionTable},
  {"RtlUnwind", (void*)&trt_RtlUnwind},
  {"RtlUnwindEx", (void*)&trt_RtlUnwindEx},
  {"RaiseException", (void*)&trt_RaiseException},
  {"__C_specific_handler", (void*)&trt_C_specific_handler},
  {"__C_specific_handler_impl", (void*)&trt_C_specific_handler},
  {"SetUnhandledExceptionFilter", (void*)&trt_SetUnhandledExceptionFilter},
  {"UnhandledExceptionFilter", (void*)&trt_UnhandledExceptionFilter},
  {"IsDebuggerPresent", (void*)&trt_IsDebuggerPresent},
  {"EncodePointer", (void*)&trt_EncodePointer},
  {"DecodePointer", (void*)&trt_DecodePointer},
  {"InitializeSListHead", (void*)&trt_InitializeSListHead},
  {"InterlockedPushEntrySList", (void*)&trt_InterlockedPushEntrySList},
  {"InterlockedPopEntrySList", (void*)&trt_InterlockedPopEntrySList},
  {"InterlockedFlushSList", (void*)&trt_InterlockedFlushSList},
  // Sprint 1 gap batch (master prompt 1B + 1C): interlocked primitives and
  // the Rtl memory macros-as-functions games reach through GetProcAddress
  {"InterlockedIncrement", (void*)&trt_InterlockedIncrement},
  {"InterlockedDecrement", (void*)&trt_InterlockedDecrement},
  {"InterlockedExchangeAdd", (void*)&trt_InterlockedExchangeAdd},
  {"InterlockedExchange", (void*)&trt_InterlockedExchange},
  {"InterlockedCompareExchange", (void*)&trt_InterlockedCompareExchange},
  {"InterlockedIncrement64", (void*)&trt_InterlockedIncrement64},
  {"InterlockedDecrement64", (void*)&trt_InterlockedDecrement64},
  {"InterlockedExchangeAdd64", (void*)&trt_InterlockedExchangeAdd64},
  {"InterlockedCompareExchange64", (void*)&trt_InterlockedCompareExchange64},
  {"RtlCopyMemory", (void*)&trt_RtlCopyMemory},
  {"RtlMoveMemory", (void*)&trt_RtlMoveMemory},
  {"RtlZeroMemory", (void*)&trt_RtlZeroMemory},
  {"RtlFillMemory", (void*)&trt_RtlFillMemory},
  {"QueryDepthSList", (void*)&trt_QueryDepthSList},
  {"InitializeCriticalSectionAndSpinCount",
   (void*)&trt_InitializeCriticalSectionAndSpinCount},
  {"InitializeCriticalSectionEx", (void*)&trt_InitializeCriticalSectionEx},
  {"InitializeSRWLock", (void*)&trt_InitializeSRWLock},
  {"AcquireSRWLockExclusive", (void*)&trt_AcquireSRWLockExclusive},
  {"ReleaseSRWLockExclusive", (void*)&trt_ReleaseSRWLockExclusive},
  {"TryAcquireSRWLockExclusive", (void*)&trt_TryAcquireSRWLockExclusive},
  {"AcquireSRWLockShared", (void*)&trt_AcquireSRWLockShared},
  {"ReleaseSRWLockShared", (void*)&trt_ReleaseSRWLockShared},
  {"TryAcquireSRWLockShared", (void*)&trt_TryAcquireSRWLockShared},
  {"InitializeConditionVariable", (void*)&trt_InitializeConditionVariable},
  {"SleepConditionVariableSRW", (void*)&trt_SleepConditionVariableSRW},
  {"SleepConditionVariableCS", (void*)&trt_SleepConditionVariableCS},
  {"WaitOnAddress", (void*)&trt_WaitOnAddress},
  {"WakeByAddressSingle", (void*)&trt_WakeByAddressSingle},
  {"WakeByAddressAll", (void*)&trt_WakeByAddressAll},
  {"WakeAllConditionVariable", (void*)&trt_WakeAllConditionVariable},
  {"WakeConditionVariable", (void*)&trt_WakeConditionVariable},
  {"FindFirstFileExW", (void*)&trt_FindFirstFileExW},
  {"FindFirstFileW", (void*)&trt_FindFirstFileW},
  {"FindNextFileW", (void*)&trt_FindNextFileW},
  {"FindFirstFileA", (void*)&trt_FindFirstFileA},
  {"FindNextFileA", (void*)&trt_FindNextFileA},
  {"FindClose", (void*)&trt_FindClose},
  {"GetFileAttributesExW", (void*)&trt_GetFileAttributesExW},
  {"SetFilePointerEx", (void*)&trt_SetFilePointerEx},
  {"FlushFileBuffers", (void*)&trt_FlushFileBuffers},
  {"GetFileType", (void*)&trt_GetFileType},
  {"GetCurrentProcessId", (void*)&trt_GetCurrentProcessId},
  {"GetProcessId", (void*)&trt_GetProcessId},
  {"GetModuleHandleExW", (void*)&trt_GetModuleHandleExW},
  {"GetModuleHandleA", (void*)&trt_GetModuleHandleA},
  {"GetSystemTimeAsFileTime", (void*)&trt_GetSystemTimeAsFileTime},
  {"GetSystemTime", (void*)&trt_GetSystemTime},
  {"VirtualQuery", (void*)&trt_VirtualQuery},
  {"FlushInstructionCache", (void*)&trt_FlushInstructionCache},
  {"DuplicateHandle", (void*)&trt_DuplicateHandle},
  {"AddVectoredExceptionHandler", (void*)&trt_AddVectoredExceptionHandler},
  {"RemoveVectoredExceptionHandler", (void*)&trt_RemoveVectoredExceptionHandler},
  {"CreateEventExW", (void*)&trt_CreateEventExW},
  {"CreateEventExA", (void*)&trt_CreateEventExA},
  {"CreateSemaphoreExW", (void*)&trt_CreateSemaphoreExW},
  {"CreateSemaphoreExA", (void*)&trt_CreateSemaphoreExA},
  {"CreateWaitableTimerExW", (void*)&trt_CreateWaitableTimerExW},
  {"CreateWaitableTimerExA", (void*)&trt_CreateWaitableTimerExA},
  {"SetWaitableTimer", (void*)&trt_SetWaitableTimer},
  {"CancelWaitableTimer", (void*)&trt_CancelWaitableTimer},
  {"CreateFileA", (void*)&trt_CreateFileA},
  {"DeleteFileA", (void*)&trt_DeleteFileA},
  {"CopyFileExW", (void*)&trt_CopyFileExW},
  {"CopyFile2", (void*)&trt_CopyFile2},
  {"AreFileApisANSI", (void*)&trt_AreFileApisANSI},
  {"GetCurrentProcessorNumber", (void*)&trt_GetCurrentProcessorNumber},
  {"GetCurrentDirectoryA", (void*)&trt_GetCurrentDirectoryA},
  {"GetComputerNameA", (void*)&trt_GetComputerNameA},
  {"GetEnvironmentStringsA", (void*)&trt_GetEnvironmentStringsA},
  {"FreeEnvironmentStringsA", (void*)&trt_FreeEnvironmentStringsA},
  {"FormatMessageA", (void*)&trt_FormatMessageA},
  {"CompareStringOrdinal", (void*)&trt_CompareStringOrdinal},
  {"CompareStringA", (void*)&trt_CompareStringA},
  {"CompareFileTime", (void*)&trt_CompareFileTime},
  {"AddDllDirectory", (void*)&trt_AddDllDirectory},
  {"RemoveDllDirectory", (void*)&trt_RemoveDllDirectory},
  {"SetDefaultDllDirectories", (void*)&trt_SetDefaultDllDirectories},
  {"AppPolicyGetProcessTerminationMethod",
   (void*)&trt_AppPolicyGetProcessTerminationMethod},
  {"FreeLibraryAndExitThread", (void*)&trt_FreeLibraryAndExitThread},
  {"DisableThreadLibraryCalls", (void*)&trt_DisableThreadLibraryCalls},
  {"DebugBreak", (void*)&trt_DebugBreak},
  {"FatalAppExitA", (void*)&trt_FatalAppExitA},
  {"EnumSystemLocalesA", (void*)&trt_EnumSystemLocalesA},
  {"EnumSystemLocalesW", (void*)&trt_EnumSystemLocalesW},
  {"EnumSystemLocalesEx", (void*)&trt_EnumSystemLocalesEx},
  {"ResolveLocaleName", (void*)&trt_ResolveLocaleName},
  {"CreatePipe", (void*)&trt_CreatePipe},
  {"CreateNamedPipeW", (void*)&trt_CreateNamedPipeW},
  {"ConnectNamedPipe", (void*)&trt_ConnectNamedPipe},
  {"CancelIo", (void*)&trt_CancelIo},
  {"CancelIoEx", (void*)&trt_CancelIoEx},
  {"DeviceIoControl", (void*)&trt_DeviceIoControl},
  {"GetConsoleProcessList", (void*)&trt_GetConsoleProcessList},
  {"GetConsoleScreenBufferInfo", (void*)&trt_GetConsoleScreenBufferInfo},
  {"GetConsoleWindow", (void*)&trt_GetConsoleWindow},
  {"AttachConsole", (void*)&trt_AttachConsole},
  {"GetCurrentPackageFullName", (void*)&trt_GetCurrentPackageFullName},
  {"GetCurrentPackageFamilyName", (void*)&trt_GetCurrentPackageFamilyName},
  {"GetCurrentApplicationUserModelId", (void*)&trt_GetCurrentApplicationUserModelId},
  {"GetCurrentPackageId", (void*)&trt_GetCurrentPackageId},
  {"GetCurrentPackagePath", (void*)&trt_GetCurrentPackagePath},
  {"SetFileCompletionNotificationModes", (void*)&trt_SetFileCompletionNotificationModes},
};

// Bun/V8 query the current thread stack before they scan it.  A synthesised
// return-0 stub leaves both bounds at zero and the GC walks address 0.
void TRT_STDCALL trt_GetCurrentThreadStackLimits(u64* low, u64* high) {
  if (!low || !high) return;
#if defined(_WIN32)
  *low = (u64)__readgsqword(0x10);   // NT_TIB.StackLimit
  *high = (u64)__readgsqword(0x08);  // NT_TIB.StackBase
#else
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* base = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &base, &size) == 0) {
      *low = (u64)(uintptr_t)base;
      *high = *low + (u64)size;
    }
    pthread_attr_destroy(&attr);
  }
#endif
}

void TRT_STDCALL trt_GetCurrentThreadStackLimits(u64* low, u64* high);

int trt_bridge_register_kernel32(void) {
  trt_bridge_register_prng();
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  // kernelbase forwards: PE games import a mix; route kernelbase to the same
  // builtin surface (Wine does the same via spec forwarders)
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  // Sprint 1 gap batch (1C): ntdll also exports the four Rtl memory
  // primitives. Merge them into the ntdll table once the ntdll bridge has
  // upgraded it to BUILTIN (boot order guarantees it already has; the route
  // guard keeps a hypothetical late-order host from replacing the module).
  if (trt_module_route("ntdll.dll") == TRT_ROUTE_BUILTIN) {
    static const trt_named_fn k_nt_rtl[] = {
        {"RtlCopyMemory", (void*)&trt_RtlCopyMemory},
        {"RtlMoveMemory", (void*)&trt_RtlMoveMemory},
        {"RtlZeroMemory", (void*)&trt_RtlZeroMemory},
        {"RtlFillMemory", (void*)&trt_RtlFillMemory},
    };
    trt_module_register("ntdll.dll", TRT_ROUTE_BUILTIN, k_nt_rtl,
                        (int)(sizeof(k_nt_rtl) / sizeof(k_nt_rtl[0])));
  }
  // the OrTMM-audit gap batch (file/locale/time/process surface)
  {
    extern void trt_gap_register(void);
    trt_gap_register();
  }
  return 0;
}
} // extern "C"
