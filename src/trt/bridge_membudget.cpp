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
//   HeapAlloc/HeapFree/HeapReAlloc         (bridge_kernel32.cpp — host backend)
//   MapViewOfFile / UnmapViewOfFile / LRU eviction (bridge_mem.cpp)
//
// Budget: env TRT_MEM_BUDGET_MB, default 5120 (5 GB), clamped 1..16384.
// Read once per process (like the TRT_MAX_MAPS ceiling) so a test that needs
// a small budget re-execs itself with the env set.
//
// Fail shape: over-budget commit/map refuses with ERROR_COMMITMENT_LIMIT
// (1455) + a named, rate-limited log line. Never a silent lie, never a
// platform kill the guest could have avoided.
#include "trt.h"
#include "thunk.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void TRT_STDCALL trt_SetLastError(u32 err);

namespace {

std::atomic<u64> g_committed{0};   // VirtualAlloc commit + heap + mapped views
std::atomic<u64> g_mapped{0};      // the mapped-view subclass of g_committed
std::atomic<u64> g_peak{0};
std::atomic<u64> g_refusals{0};

std::atomic<bool> g_soft80{false};
std::atomic<bool> g_soft90{false};

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
           (unsigned long long)(budget_bytes_once() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_committed.load() / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

void log_soft_level(u64 committed, const char* pct) {
  char line[192];
  snprintf(line, sizeof(line),
           "[trt] mem: %s of the %llu MB budget committed (%llu MB) — "
           "approaching the console Game-package ceiling",
           pct,
           (unsigned long long)(budget_bytes_once() / (1024ULL * 1024ULL)),
           (unsigned long long)(committed / (1024ULL * 1024ULL)));
  trt_core_log(line);
}

// Returns 0 when the charge fits, -1 when it would exceed the budget (the
// caller must then fail the guest allocation with 1455).
int charge_common(const char* what, u64 bytes) {
  const u64 budget = budget_bytes_once();
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
  const u64 budget = budget_bytes_once();
  const u64 committed = g_committed.load();
  if (committed < (budget / 10) * 8) {
    g_soft80.store(false);
    g_soft90.store(false);
  } else if (committed < (budget / 10) * 9) {
    g_soft90.store(false);
  }
}

int trt_mem_budget_charge_map(u64 bytes) {
  if (!bytes) return 0;
  if (charge_common("MapViewOfFile", bytes) != 0) return -1;
  g_mapped.fetch_add(bytes);
  return 0;
}

void trt_mem_budget_discharge_map(u64 bytes) {
  if (!bytes) return;
  discharge(g_mapped, bytes, "mapped view");
  trt_mem_budget_discharge_commit(bytes);
}

u64 trt_mem_budget_bytes(void) { return budget_bytes_once(); }
u64 trt_mem_budget_committed(void) { return g_committed.load(); }
u64 trt_mem_budget_mapped(void) { return g_mapped.load(); }
u64 trt_mem_budget_peak(void) { return g_peak.load(); }
u64 trt_mem_budget_refusals(void) { return g_refusals.load(); }

void trt_mem_budget_report(char* line, unsigned cap) {
  if (!line || !cap) return;
  const u64 budget = budget_bytes_once();
  const u64 committed = g_committed.load();
  snprintf(line, cap,
           "memory budget: %llu/%llu MB committed (%llu MB mapped views, "
           "peak %llu MB, %llu refusals)",
           (unsigned long long)(committed / (1024ULL * 1024ULL)),
           (unsigned long long)(budget / (1024ULL * 1024ULL)),
           (unsigned long long)(g_mapped.load() / (1024ULL * 1024ULL)),
           (unsigned long long)(g_peak.load() / (1024ULL * 1024ULL)),
           (unsigned long long)g_refusals.load());
}

}  // extern "C"
