// tiramisu/src/trt/bridge_membudget.cpp
// The 5 GB memory-budget governor (console Game-package allocation).
//
// An Xbox Dev Mode "Game" package gets ~5 GB of committed memory; anything
// beyond it is terminated by the platform, not refused. A guest that walks
// off the cliff must therefore hear about it FROM US, named, while it still
// has its own low-memory paths (games handle allocation failure; nothing
// survives a platform kill). Every host-side byte that stands in for guest
// commit charges this one ledger:
//
//   VirtualAlloc commit/decommit/release   (bridge_kernel32.cpp)
//   HeapAlloc/HeapFree/HeapReAlloc         (bridge_kernel32.cpp — slab pool;
//                                           the SLAB reservation is charged,
//                                           not per-block carving)
//   MapViewOfFile / UnmapViewOfFile / LRU eviction (bridge_mem.cpp)
//   MapViewOfFile read-only FILES: VA only, never commit — the pages are the
//   file's clean page cache, reclaimable on demand (the SSD/RAM overlap)
//   PE image SizeOfImage at map / unmap    (pe_loader.cpp)
//   CRT malloc/calloc/realloc/free family  (bridge_msvcrt.cpp)
//   Anonymous mapping backing buffers      (bridge_mem.cpp)
//
// A SECOND ledger tracks virtual-address-space reservations (MEM_RESERVE
// without commit, thread-stack reserves, and every committed byte's VA —
// committed memory is reserved by definition). VA exhaustion is a
// different failure from commit exhaustion (a game reserving a 100 GB
// arena dies of VA, not of RAM), so it gets its own cap and its own
// refusal code (ERROR_NOT_ENOUGH_MEMORY, 8 — not 1455).
//
// Budget: env TRT_MEM_BUDGET_MB, default 5120 (5 GB), clamped 1..16384.
// VA budget: env TRT_MEM_VA_BUDGET_MB, default 65536 (64 GB), clamped
// 1024..1048576. Both read once per process (like the TRT_MAX_MAPS
// ceiling) so a test that needs a small budget re-execs itself with the
// env set.
//
// Fail shape: over-budget commit/map refuses with ERROR_COMMITMENT_LIMIT
// (1455) + a named, rate-limited log line. Over-budget VA reservation
// refuses with ERROR_NOT_ENOUGH_MEMORY (8) + a named log line. Never a
// silent lie, never a platform kill the guest could have avoided.
#include "trt.h"
#include "thunk.h"

#include <atomic>

extern "C" void trt_crt_forget_all(void);  // .251 FIX: bridge_msvcrt.cpp
extern "C" void trt_va_forget_all(void);  // .252 FIX: bridge_kernel32.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void TRT_STDCALL trt_SetLastError(u32 err);

namespace {

std::atomic<u64> g_committed{0};   // VirtualAlloc commit + heap + mapped views
std::atomic<u64> g_mapped{0};      // the mapped-view subclass of g_committed
std::atomic<u64> g_file_view{0};   // file-backed read-only views (VA only)
std::atomic<u64> g_peak{0};
std::atomic<u64> g_refusals{0};

std::atomic<u64> g_va_reserved{0};  // VA reservations (reserve + commit + stacks)
std::atomic<u64> g_va_peak{0};
std::atomic<u64> g_va_refusals{0};

std::atomic<bool> g_soft80{false};
std::atomic<bool> g_soft90{false};
std::atomic<u64> g_dynamic_budget{0};

u64 budget_bytes_once() {
  // read once per process (function-local static: thread-safe init) — the
  // old code ran getenv+atoll on every charge/discharge/report call
  static const u64 budget = [] {
    const char* env = getenv("TRT_MEM_BUDGET_MB");
    u64 mb = 5120;  // the console Game-package allocation
    if (env && *env) {
      const long long v = atoll(env);
      if (v > 0) mb = (u64)v;
    }
    if (mb < 1) mb = 1;
    if (mb > 16384) mb = 16384;
    return mb * 1024ULL * 1024ULL;
  }();
  return budget;
}

u64 get_active_budget_bytes() {
  u64 dyn = g_dynamic_budget.load();
  if (dyn > 0) return dyn;
  return budget_bytes_once();
}

u64 budget_va_bytes_once() {
  // VA is cheap until it isn't: a game that reserves a 100 GB arena (or a
  // runaway reserve loop) must hear about it before the address space is
  // gone. 64 GB is far above any legitimate game's reservation footprint
  // (Unity-class arenas are single-digit GB) — this is a tripwire, not a
  // tight cap.
  static const u64 budget = [] {
    const char* env = getenv("TRT_MEM_VA_BUDGET_MB");
    u64 mb = 65536;  // 64 GB of virtual address space
    if (env && *env) {
      const long long v = atoll(env);
      if (v > 0) mb = (u64)v;
    }
    if (mb < 1024) mb = 1024;
    if (mb > 1048576) mb = 1048576;
    return mb * 1024ULL * 1024ULL;
  }();
  return budget;
}

// Rate-limited refusal log: the first refusal always logs; after that, one
// line per 100 refusals keeps the ring readable during a streaming storm.
void log_refusal(const char* what, u64 bytes) {
  const u64 n = g_refusals.fetch_add(1) + 1;
  if (n != 1 && (n % 100) != 0) return;
  char line[256];
  snprintf(line, sizeof(line),
           "[trt] mem: BUDGET REFUSAL #%llu: %s of %llu bytes over the %llu MB "
           "budget (committed %llu MB) -> ERROR_COMMITMENT_LIMIT (1455); "
           "guest low-memory path expected",
           (unsigned long long)n, what, (unsigned long long)bytes,
           (unsigned long long)(get_active_budget_bytes() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_committed.load() / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

void log_va_refusal(const char* what, u64 bytes) {
  const u64 n = g_va_refusals.fetch_add(1) + 1;
  if (n != 1 && (n % 100) != 0) return;
  char line[256];
  snprintf(line, sizeof(line),
           "[trt] mem: VA BUDGET REFUSAL #%llu: %s of %llu bytes over the "
           "%llu MB VA budget (reserved %llu MB) -> ERROR_NOT_ENOUGH_MEMORY "
           "(8); guest reservation failed",
           (unsigned long long)n, what, (unsigned long long)bytes,
           (unsigned long long)(budget_va_bytes_once() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_va_reserved.load() / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

void log_soft_level(u64 committed, const char* pct) {
  char line[192];
  snprintf(line, sizeof(line),
           "[trt] mem: %s of the %llu MB budget committed (%llu MB) — "
           "approaching the console Game-package ceiling",
           pct,
           (unsigned long long)(get_active_budget_bytes() / (1024ULL * 1024ULL)),
           (unsigned long long)(committed / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

// Returns 0 when the charge fits, -1 when it would exceed the budget (the
// caller must then fail the guest allocation with 1455).
int charge_common(const char* what, u64 bytes) {
  const u64 budget = get_active_budget_bytes();
  u64 before = g_committed.load();
  u64 after;
  do {
    if (before > budget || bytes > budget - before) {
      log_refusal(what, bytes);
      trt_SetLastError(1455);  // ERROR_COMMITMENT_LIMIT
      return -1;
    }
    after = before + bytes;
  } while (!g_committed.compare_exchange_weak(before, after));
  u64 p = g_peak.load();
  while (after > p && !g_peak.compare_exchange_weak(p, after)) {}
  if (after >= (budget / 10) * 9 && !g_soft90.exchange(true)) {
    log_soft_level(after, "90%");
  } else if (after >= (budget / 10) * 8 && !g_soft80.exchange(true)) {
    log_soft_level(after, "80%");
  }
  return 0;
}

// VA twin of charge_common: 0 when the reservation fits, -1 when it would
// exceed the VA budget (the caller fails with ERROR_NOT_ENOUGH_MEMORY, 8).
int va_charge_common(const char* what, u64 bytes) {
  const u64 budget = budget_va_bytes_once();
  u64 before = g_va_reserved.load();
  u64 after;
  do {
    if (before > budget || bytes > budget - before) {
      log_va_refusal(what, bytes);
      trt_SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
      return -1;
    }
    after = before + bytes;
  } while (!g_va_reserved.compare_exchange_weak(before, after));
  u64 p = g_va_peak.load();
  while (after > p && !g_va_peak.compare_exchange_weak(p, after)) {}
  return 0;
}

void discharge(std::atomic<u64>& counter, u64 bytes, const char* what) {
  u64 before = counter.load();
  u64 after = 0;
  do {
    after = before > bytes ? before - bytes : 0;
  } while (!counter.compare_exchange_weak(before, after));
  if (before < bytes) {
    // accounting tripwire: a discharge larger than the ledger means a
    // caller freed bytes it never charged (or freed twice)
    char line[256];
    snprintf(line, sizeof(line),
             "[trt] mem: BUDGET ACCOUNTING: %s discharge of %llu bytes "
             "exceeds committed %llu — clamped to 0 (over-discharge, "
             "caller bug)",
             what, (unsigned long long)bytes, (unsigned long long)before);
    trt_core_log(line);
  }
}

}  // namespace

extern "C" {

int trt_mem_budget_charge_commit(u64 bytes) {
  if (!bytes) return 0;
  return charge_common("VirtualAlloc/heap commit", bytes);
}

void trt_mem_budget_discharge_commit(u64 bytes) {
  if (!bytes) return;
  discharge(g_committed, bytes, "commit");
  // dropping back below a soft level re-arms its warning: a game that
  // streams (alloc/free cycles near the ceiling) must warn again on the
  // next climb, not once per process
  const u64 budget = get_active_budget_bytes();
  const u64 committed = g_committed.load();
  if (committed < (budget / 10) * 8) {
    g_soft80.store(false);
    g_soft90.store(false);
  } else if (committed < (budget / 10) * 9) {
    g_soft90.store(false);
  }
}

// A file-backed read-only view charges VA only: its pages ARE the file's
// clean page cache, the kernel drops them under pressure for free and re-faults
// them from the SSD. Counting them as commit would charge a streaming game for
// RAM the platform can reclaim whenever it wants.
int trt_mem_budget_charge_file_view(u64 bytes) {
  if (!bytes) return 0;
  if (va_charge_common("MapViewOfFile(file) VA", bytes) != 0) return -1;
  g_file_view.fetch_add(bytes);
  return 0;
}

void trt_mem_budget_discharge_file_view(u64 bytes) {
  if (!bytes) return;
  discharge(g_file_view, bytes, "file-backed view");
  discharge(g_va_reserved, bytes, "file view VA");
}

u64 trt_mem_budget_file_mapped(void) { return g_file_view.load(); }

int trt_mem_budget_charge_map(u64 bytes) {
  if (!bytes) return 0;
  // a mapped view reserves VA for exactly its lifetime, so the map charge
  // carries its VA charge with it (VA first: a VA refusal must not leave
  // a commit charge behind, and a commit refusal must roll the VA back)
  if (va_charge_common("MapViewOfFile VA", bytes) != 0) return -1;
  if (charge_common("MapViewOfFile", bytes) != 0) {
    discharge(g_va_reserved, bytes, "VA reservation");
    return -1;
  }
  g_mapped.fetch_add(bytes);
  return 0;
}

void trt_mem_budget_discharge_map(u64 bytes) {
  if (!bytes) return;
  discharge(g_mapped, bytes, "mapped view");
  trt_mem_budget_discharge_commit(bytes);
  discharge(g_va_reserved, bytes, "VA reservation");
}

// ---- VA reservation ledger ----
// VirtualAlloc reserve paths, heap slabs/direct blocks, PE images and
// thread stacks charge VA directly — their VA lifetime is NOT the commit
// lifetime (a decommit keeps the reservation; a stack reserve never
// commits at all).
int trt_mem_budget_charge_va(u64 bytes) {
  if (!bytes) return 0;
  return va_charge_common("VA reservation", bytes);
}

void trt_mem_budget_discharge_va(u64 bytes) {
  if (!bytes) return;
  discharge(g_va_reserved, bytes, "VA reservation");
}

u64 trt_mem_budget_bytes(void) { return get_active_budget_bytes(); }
void trt_mem_budget_set_mb(u32 mb) {
  if (mb < 1) mb = 1;
  if (mb > 16384) mb = 16384;
  g_dynamic_budget.store((u64)mb * 1024ULL * 1024ULL);
}
u32 trt_mem_budget_get_mb(void) {
  return (u32)(get_active_budget_bytes() / (1024ULL * 1024ULL));
}
u64 trt_mem_budget_committed(void) { return g_committed.load(); }
u64 trt_mem_budget_mapped(void) { return g_mapped.load(); }
u64 trt_mem_budget_peak(void) { return g_peak.load(); }
u64 trt_mem_budget_refusals(void) { return g_refusals.load(); }
u64 trt_mem_budget_va_bytes(void) { return budget_va_bytes_once(); }
u64 trt_mem_budget_reserved(void) { return g_va_reserved.load(); }
u64 trt_mem_budget_va_peak(void) { return g_va_peak.load(); }
u64 trt_mem_budget_va_refusals(void) { return g_va_refusals.load(); }

// Force-kill recovery only: zero both ledgers after the host terminated a
// hung guest thread. The orphaned host memory (VA regions, heap slabs,
// images the dead guest never freed) is STILL HELD — the ledger reset is
// a fresh-start fiction the next guest needs, not a true accounting of
// the process. Peaks and refusal counters are history and are kept.
void trt_mem_budget_reset(void) {
  const u64 committed = g_committed.exchange(0);
  const u64 mapped = g_mapped.exchange(0);
  g_file_view.store(0);
  const u64 va = g_va_reserved.exchange(0);
  g_soft80.store(false);
  g_soft90.store(false);
  // .251 FIX: the dead guest's tracked CRT blocks are still in the
  // registry (bridge_msvcrt.cpp); freeing them after this reset
  // discharges against the zeroed ledger and trips the over-discharge
  // tripwire (the .250 9/16-byte warnings). Forget the registry so
  // those frees land as foreign pointers. The heap-record registry
  // is NOT cleared here (its discharges are large / page-rounded
  // and did not trip in .250); if it trips on a later run, it gets
  // the same treatment.
  // .252 FIX: the VA-extent registry DID trip on .251 (1 MB VA
  // over-discharge after /api/stop) -- forget it too, same treatment.
  trt_crt_forget_all();
  trt_va_forget_all();
  char line[256];
  snprintf(line, sizeof(line),
           "[trt] mem: BUDGET RESET after guest force-kill: ledger zeroed "
           "(was %llu MB committed, %llu MB mapped, %llu MB VA reserved) — "
           "orphaned host memory may still be held; next guest starts fresh",
           (unsigned long long)(committed / (1024ULL * 1024ULL)),
           (unsigned long long)(mapped / (1024ULL * 1024ULL)),
           (unsigned long long)(va / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

void trt_mem_budget_report(char* line, unsigned cap) {
  if (!line || !cap) return;
  const u64 budget = get_active_budget_bytes();
  const u64 committed = g_committed.load();
  snprintf(line, cap,
           "memory budget: %llu/%llu MB committed (%llu MB mapped views, "
           "%llu MB file-backed page cache, peak %llu MB, %llu refusals; "
           "VA %llu/%llu MB reserved, peak %llu MB, %llu refusals)",
           (unsigned long long)(committed / (1024ULL * 1024ULL)),
           (unsigned long long)(budget / (1024ULL * 1024ULL)),
           (unsigned long long)(g_mapped.load() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_file_view.load() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_peak.load() / (1024ULL * 1024ULL)),
           (unsigned long long)g_refusals.load(),
           (unsigned long long)(g_va_reserved.load() / (1024ULL * 1024ULL)),
           (unsigned long long)(budget_va_bytes_once() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_va_peak.load() / (1024ULL * 1024ULL)),
           (unsigned long long)g_va_refusals.load());
}

}  // extern "C"
