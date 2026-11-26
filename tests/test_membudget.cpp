// tiramisu/tests/test_membudget.cpp
// The 5 GB memory-budget governor (bridge_membudget.cpp): ONE ledger for
// VirtualAlloc commit, heap allocations and mapped views; over-budget
// refusals are named (ERROR_COMMITMENT_LIMIT / 1455) so the guest's own
// low-memory paths run instead of a platform kill on the console.
//
// Runs ARGLESS for the default-budget blocks; the small-budget integration
// blocks re-exec this binary with TRT_MEM_BUDGET_MB set (the budget is
// parsed once per process, like the TRT_MAX_MAPS ceiling).
//
// Runs ARGLESS: every surface is exercised host-side through the bridge
// entry points (the same functions the guest IAT resolves to).
#include "../src/trt/trt.h"
#include "../src/trt/bridge_mem.h"
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

extern "C" {
int trt_bridge_register_kernel32(void);

// budget governor surface (bridge_membudget.cpp)
u64 trt_mem_budget_bytes(void);
u64 trt_mem_budget_committed(void);
u64 trt_mem_budget_mapped(void);
u64 trt_mem_budget_peak(void);
u64 trt_mem_budget_refusals(void);
// VA ledger (item 3): reserved VA is tracked separately from commit
u64 trt_mem_budget_va_bytes(void);
u64 trt_mem_budget_reserved(void);
u64 trt_mem_budget_va_peak(void);
u64 trt_mem_budget_va_refusals(void);
int trt_mem_budget_charge_commit(u64 bytes);
void trt_mem_budget_discharge_commit(u64 bytes);
int trt_mem_budget_charge_map(u64 bytes);
void trt_mem_budget_discharge_map(u64 bytes);
int trt_mem_budget_charge_file_view(u64 bytes);
void trt_mem_budget_discharge_file_view(u64 bytes);
u64 trt_mem_budget_file_mapped(void);
void trt_mem_budget_report(char* line, unsigned cap);

// kernel32 surfaces under test
void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 bytes, u32 type, u32 prot);
int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
u64 TRT_STDCALL trt_VirtualQuery(const void*, void*, u64);
int TRT_STDCALL trt_VirtualProtect(void*, u64, u32, u32*);
int trt_dispatch_hw_fault(int, void*, void*);
void* TRT_STDCALL trt_HeapCreate(u32 flags, u64 init, u64 max);
void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes);
int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p);
u32 TRT_STDCALL trt_GetLastError(void);

// mapped views (bridge_mem.cpp)
u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name);
void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes);
int TRT_STDCALL trt_UnmapViewOfFile(void* view);
}

// ---------------------------------------------------------------------------
// harness (same shape as test_mem_crash.cpp)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> g_log;
void log_capture(const char* line) { g_log.emplace_back(line); }
void log_quiet(const char*) {}

struct extent { void* base; size_t len; };
extent g_extents[4096 + 8];
int g_extent_count = 0;

int log_count(const char* substr) {
  int n = 0;
  for (const std::string& l : g_log) {
    if (l.find(substr) != std::string::npos) ++n;
  }
  return n;
}
bool log_contains(const char* substr) { return log_count(substr) > 0; }

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

void* mmap_alloc(size_t bytes, u64 preferred) {
  (void)preferred;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 4096 + 8) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int mmap_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len =
      ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void mmap_free(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
}

void* mmap_reserve(size_t bytes, u64 preferred) {
  void* base = mmap_alloc(bytes, preferred);
  if (base && mmap_protect(base, bytes, 0) != 0) {
    mmap_free(base, bytes);
    return nullptr;
  }
  return base;
}

int mmap_decommit(void* base, size_t bytes) {
  return madvise(base, bytes, MADV_DONTNEED) == 0 && mmap_protect(base, bytes, 0) == 0 ? 0 : 1;
}

void demand_commit_handler(int sig, siginfo_t* info, void* context) {
  if (!trt_dispatch_hw_fault(sig, info, context)) _exit(90);
}

void boot_runtime(bool capture_log) {
  const trt_backend be{mmap_alloc, mmap_protect, mmap_free,
                       capture_log ? log_capture : log_quiet};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
}

constexpr u64 MB = 1024ULL * 1024ULL;

// ---------------------------------------------------------------------------
// small-budget probe (re-exec'd with TRT_MEM_BUDGET_MB=16)
// ---------------------------------------------------------------------------

int run_small_budget_probe() {
  printf("probe: TRT_MEM_BUDGET_MB=16 integration\n");
  boot_runtime(true);

  CHECK(trt_mem_budget_bytes() == 16ULL * MB, "budget honors the env (16 MB)");

  // --- ledger refusal is named and sets ERROR_COMMITMENT_LIMIT ---
  const u64 before = trt_mem_budget_committed();
  CHECK(trt_mem_budget_charge_commit(20ULL * MB) == -1,
        "charge over the budget refuses (-1)");
  CHECK(trt_GetLastError() == 1455,
        "refusal sets ERROR_COMMITMENT_LIMIT (1455)");
  CHECK(log_contains("BUDGET REFUSAL"),
        "refusal logs a named BUDGET REFUSAL line");
  CHECK(trt_mem_budget_refusals() == 1, "refusal counter increments");
  CHECK(trt_mem_budget_committed() == before,
        "a refused charge leaves the ledger untouched");

  // --- VirtualAlloc integration: refuse / commit / decommit / release ---
  void* big = trt_VirtualAlloc(nullptr, 32ULL * MB, 0x3000 /*RESERVE|COMMIT*/,
                               0x04 /*PAGE_READWRITE*/);
  CHECK(big == nullptr, "VirtualAlloc 32 MB refuses under a 16 MB budget");
  CHECK(trt_GetLastError() == 1455, "VirtualAlloc refusal carries 1455");
  CHECK(trt_mem_budget_committed() == before,
        "refused VirtualAlloc charges nothing");

  void* p = trt_VirtualAlloc(nullptr, 8ULL * MB, 0x3000, 0x04);
  CHECK(p != nullptr, "VirtualAlloc 8 MB succeeds under the budget");
  CHECK(trt_mem_budget_committed() == before + 8ULL * MB,
        "committed ledger +8 MB after the reservation+commit");

  CHECK(trt_VirtualFree(p, 4ULL * MB, 0x4000 /*MEM_DECOMMIT*/) == 1,
        "VirtualFree MEM_DECOMMIT of half the region succeeds");
  CHECK(trt_mem_budget_committed() == before + 4ULL * MB,
        "decommit discharges exactly the decommitted bytes");

  CHECK(trt_VirtualFree(p, 0, 0x8000 /*MEM_RELEASE*/) == 1,
        "VirtualFree MEM_RELEASE succeeds");
  CHECK(trt_mem_budget_committed() == before,
        "release discharges the remaining committed bytes");

  // --- heap integration ---
  void* heap = trt_HeapCreate(0, 0, 0);
  CHECK(heap != nullptr, "HeapCreate works");
  void* h1 = trt_HeapAlloc(heap, 0, 10ULL * MB);
  CHECK(h1 != nullptr, "HeapAlloc 10 MB succeeds");
  CHECK(trt_mem_budget_committed() == before + 10ULL * MB,
        "heap bytes join the same ledger (+10 MB)");
  void* h2 = trt_HeapAlloc(heap, 0, 10ULL * MB);
  CHECK(h2 == nullptr && trt_GetLastError() == 1455,
        "HeapAlloc past the budget refuses with 1455");
  CHECK(trt_HeapFree(heap, 0, h1) == 1, "HeapFree succeeds");
  CHECK(trt_mem_budget_committed() == before,
        "HeapFree discharges the recorded size");
  void* h3 = trt_HeapAlloc(heap, 0, 10ULL * MB);
  CHECK(h3 != nullptr, "after the free the same allocation succeeds again");
  trt_HeapFree(heap, 0, h3);

  // --- heap pool (item 1): small allocs share one 1 MB slab reservation ---
  {
    const u64 pool_before = trt_mem_budget_committed();
    void* s1 = trt_HeapAlloc(heap, 0, 64);
    CHECK(s1 != nullptr, "pooled HeapAlloc(64) succeeds");
    CHECK(trt_mem_budget_committed() == pool_before + 1ULL * MB,
          "first small alloc charges exactly one 1 MB slab");
    void* smalls[100];
    int nsmall = 0;
    for (int i = 0; i < 100; ++i) {
      smalls[i] = trt_HeapAlloc(heap, 0, 64);
      if (!smalls[i]) break;
      ++nsmall;
    }
    CHECK(nsmall == 100, "100 more small allocs succeed from the pool");
    CHECK(trt_mem_budget_committed() == pool_before + 1ULL * MB,
          "pooled allocs add no further commit (one slab covers them)");
    for (int i = 0; i < nsmall; ++i) trt_HeapFree(heap, 0, smalls[i]);
    CHECK(trt_HeapFree(heap, 0, s1) == 1, "pooled HeapFree succeeds");
    CHECK(trt_mem_budget_committed() == pool_before,
          "freeing the whole slab discharges it");
  }

  // --- VA accounting (item 3): reservations charge VA, not commit ---
  {
    const u64 va_before = trt_mem_budget_reserved();
    const u64 c_before = trt_mem_budget_committed();
    void* r = trt_VirtualAlloc(nullptr, 8ULL * MB, 0x2000 /*MEM_RESERVE*/,
                               0x04);
    CHECK(r != nullptr, "reserve-only VirtualAlloc succeeds");
    CHECK(trt_mem_budget_committed() == c_before,
          "reserve-only charges no commit");
    CHECK(trt_mem_budget_reserved() == va_before + 8ULL * MB,
          "reserve-only charges VA");
    void* c = trt_VirtualAlloc(r, 4ULL * MB, 0x1000 /*MEM_COMMIT*/, 0x04);
    CHECK(c != nullptr, "commit inside the reservation succeeds");
    CHECK(trt_mem_budget_committed() == c_before + 4ULL * MB,
          "commit charges commit");
    CHECK(trt_mem_budget_reserved() == va_before + 8ULL * MB,
          "commit inside a reservation adds no VA");
    CHECK(trt_VirtualFree(r, 0, 0x8000 /*MEM_RELEASE*/) == 1,
          "release of the reserved region succeeds");
    CHECK(trt_mem_budget_committed() == c_before &&
              trt_mem_budget_reserved() == va_before,
          "release discharges both commit and VA");
  }

  // --- mapped views join the same ledger ---
  u64 mf = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0,
                                  (u32)(16ULL * MB), nullptr);
  CHECK(mf != 0, "anon mapping core created");
  void* v = trt_MapViewOfFile(mf, 0x04 /*READ*/, 0, 0, 4ULL * MB);
  CHECK(v != nullptr, "MapViewOfFile 4 MB succeeds");
  CHECK(trt_mem_budget_mapped() == 4ULL * MB,
        "mapped-view subclass counts the view bytes");
  CHECK(trt_mem_budget_committed() == before + 4ULL * MB,
        "mapped bytes charge the shared ledger");
  void* v2 = trt_MapViewOfFile(mf, 0x04, 0, 0, 14ULL * MB);
  CHECK(v2 == nullptr, "map past the remaining budget refuses");
  CHECK(trt_UnmapViewOfFile(v) == 1, "unmap succeeds");
  CHECK(trt_mem_budget_mapped() == 0 && trt_mem_budget_committed() == before,
        "unmap discharges the view bytes fully");

  // --- report line ---
  char rep[256];
  trt_mem_budget_report(rep, sizeof(rep));
  CHECK(strstr(rep, "memory budget:") == rep,
        "report line renders (crash-report ready)");

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// default-budget blocks (argless)
// ---------------------------------------------------------------------------

int run_probe(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "--probe-small-budget") == 0) {
    return run_small_budget_probe();
  }
  return -1;  // not a probe
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const int probe_rc = run_probe(argc, argv);
  if (probe_rc >= 0) return probe_rc;

  printf("tiramisu 5 GB memory-budget governor suite\n");

  // NO env: the default budget is the console Game-package allocation.
  unsetenv("TRT_MEM_BUDGET_MB");
  boot_runtime(true);

  CHECK(trt_mem_budget_bytes() == 5120ULL * MB,
        "default budget is 5120 MB (the console 5 GB allocation)");

  // --- charge/discharge arithmetic + peak + subclasses ---
  const u64 before = trt_mem_budget_committed();
  CHECK(trt_mem_budget_charge_commit(100ULL * MB) == 0,
        "charge 100 MB succeeds");
  CHECK(trt_mem_budget_committed() == before + 100ULL * MB,
        "ledger reflects the charge");
  CHECK(trt_mem_budget_charge_map(50ULL * MB) == 0, "charge 50 MB as a map");
  CHECK(trt_mem_budget_mapped() == 50ULL * MB,
        "mapped subclass counts map bytes");
  CHECK(trt_mem_budget_committed() == before + 150ULL * MB,
        "maps and commits share one ledger");
  CHECK(trt_mem_budget_peak() >= before + 150ULL * MB,
        "peak tracks the high-water mark");
  CHECK(trt_mem_budget_charge_commit(0) == 0,
        "a zero-byte charge is a no-op");
  trt_mem_budget_discharge_map(50ULL * MB);
  CHECK(trt_mem_budget_mapped() == 0,
        "discharge_map clears the mapped subclass");
  CHECK(trt_mem_budget_committed() == before + 100ULL * MB,
        "discharge_map also discharges the shared ledger");
  trt_mem_budget_discharge_commit(100ULL * MB);
  CHECK(trt_mem_budget_committed() == before, "ledger returns to baseline");

  // --- soft-level logs fire once each (80% then 90%) ---
  CHECK(trt_mem_budget_charge_commit((5120ULL * MB / 10) * 8 + MB) == 0,
        "charge past 80% of the budget succeeds");
  CHECK(log_contains("80% of the"), "named soft log: 80% of the budget");
  trt_mem_budget_discharge_commit(trt_mem_budget_committed() - before);
  CHECK(trt_mem_budget_charge_commit((5120ULL * MB / 10) * 9 + MB) == 0,
        "charge past 90% of the budget succeeds");
  CHECK(log_contains("90% of the"), "named soft log: 90% of the budget");
  trt_mem_budget_discharge_commit(trt_mem_budget_committed() - before);
  CHECK(trt_mem_budget_committed() == before, "ledger back to baseline");

  // --- report line ---
  char rep[256];
  trt_mem_budget_report(rep, sizeof(rep));
  CHECK(strstr(rep, "memory budget:") == rep, "report line renders");
  CHECK(strstr(rep, "5120/5120 MB") != nullptr || strstr(rep, "/5120 MB"),
        "report carries the budget figure");

  // --- over-discharge clamps at zero instead of wrapping ---
  const u64 c0 = trt_mem_budget_committed();
  trt_mem_budget_discharge_commit(c0 + 4096);
  CHECK(trt_mem_budget_committed() == 0, "over-discharge clamps at zero");

  // --- small-budget integration blocks run in a fresh process ---
  printf("re-exec with TRT_MEM_BUDGET_MB=16 for the integration probes\n");
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    setenv("TRT_MEM_BUDGET_MB", "16", 1);
    execl("/proc/self/exe", "test_membudget", "--probe-small-budget",
          (char*)nullptr);
    _exit(127);
  }
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  const int rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
  CHECK(rc == 0, "small-budget integration probe passes (fresh process)");

  // Reserve all the bytes before releasing them on separate threads. Each
  // release owns a distinct reservation, so a balance left over is lost
  // accounting rather than a missing allocation or a guest double-free.
  constexpr unsigned workers = 6;
  constexpr unsigned releases = 16000;
  constexpr u64 page = 4096;
  constexpr u64 total = workers * static_cast<u64>(releases) * page;
  const u64 baseline_va = trt_mem_budget_reserved();
  const bool maps_charged = trt_mem_budget_charge_map(total) == 0;
  const bool files_charged = trt_mem_budget_charge_file_view(total) == 0;
  CHECK(maps_charged && files_charged,
        "reserve mapped commit and file-backed VA for concurrent release");
  if (maps_charged && files_charged) {
    std::atomic<unsigned> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) {
      threads.emplace_back([&] {
        ready.fetch_add(1);
        while (!release.load()) std::this_thread::yield();
        for (unsigned n = 0; n < releases; ++n) {
          trt_mem_budget_discharge_map(page);
          trt_mem_budget_discharge_file_view(page);
        }
      });
    }
    while (ready.load() != workers) std::this_thread::yield();
    release.store(true);
    for (auto& thread : threads) thread.join();
    CHECK(trt_mem_budget_committed() == 0, "concurrent frees release every committed byte");
    CHECK(trt_mem_budget_mapped() == 0, "concurrent unmaps release every mapped byte");
    CHECK(trt_mem_budget_file_mapped() == 0 && trt_mem_budget_reserved() == baseline_va,
          "concurrent unmaps release every file-backed reservation");
  }

  trt_mem_budget_set_mb(1024);
  trt_mem_budget_report(rep, sizeof(rep));
  CHECK(strstr(rep, "/1024 MB") != nullptr, "report uses the active console memory limit");

  trt_set_vmem_hooks(mmap_reserve, mmap_protect, mmap_decommit);
  trt_mem_budget_set_mb(64);
  trt_va_enable_deferred_data(1);
  const u64 before_arena = trt_mem_budget_committed();
  const u64 before_arena_va = trt_mem_budget_reserved();
  constexpr u64 arena_size = 256 * MB;
  auto* arena = static_cast<u8*>(trt_VirtualAlloc(nullptr, arena_size, 0x101000u, 0x04u));
  CHECK(arena && trt_mem_budget_committed() == before_arena &&
            trt_mem_budget_reserved() == before_arena_va + arena_size,
        "large anonymous data arena reserves VA without exhausting the commit budget");
  if (arena) {
    struct mbi {
      u64 address, base;
      u32 allocation_protect, partition;
      u64 region_size;
      u32 state, protection, type, padding;
    } info{};
    static_assert(sizeof(mbi) == 48);
    CHECK(trt_VirtualQuery(arena, &info, sizeof(info)) == sizeof(info) &&
              info.state == 0x1000u && info.protection == 0x04u &&
              info.region_size == arena_size,
          "VirtualQuery reports the logical committed extent");
    CHECK(trt_VirtualAlloc(arena, arena_size, 0x1000u, 0x04u) == arena &&
              trt_mem_budget_committed() == before_arena,
          "recommitting a logically committed arena leaves physical backing deferred");
    u32 unchanged_protect = 0;
    CHECK(trt_VirtualProtect(arena, arena_size, 0x04u, &unchanged_protect) == 1 &&
              unchanged_protect == 0x04u && trt_mem_budget_committed() == before_arena,
          "an unchanged read/write protection leaves physical backing deferred");
    struct sigaction handler{}, previous{};
    handler.sa_sigaction = demand_commit_handler;
    handler.sa_flags = SA_SIGINFO;
    sigemptyset(&handler.sa_mask);
    if (sigaction(SIGSEGV, &handler, &previous) != 0) {
      CHECK(false, "install the first-touch test handler");
    } else {
      auto* data = static_cast<volatile u8*>(arena);
      CHECK(data[128 * MB] == 0, "first-touch read materializes a zero-initialized page");
      data[0] = 73;
      CHECK(data[0] == 73 && trt_mem_budget_committed() == before_arena + 2 * 65536,
            "touches charge only the two materialized chunks");
      CHECK(trt_va_prepare_data_range(arena + 4 * MB, page) &&
                trt_mem_budget_committed() == before_arena + 2 * 65536 + page,
            "backend I/O can materialize its buffer without faulting");
      u32 old_protection = 0;
      CHECK(trt_VirtualProtect(arena + 5 * MB, page, 0x02u, &old_protection) &&
                old_protection == 0x04u &&
                trt_mem_budget_committed() == before_arena + 2 * 65536 + 2 * page,
            "VirtualProtect materializes just its requested span and preserves the old protection");
      CHECK(trt_VirtualFree(arena, 65536, 0x4000u) &&
                trt_mem_budget_committed() == before_arena + 65536 + 2 * page,
            "decommit releases backed bytes while keeping the reservation");
      CHECK(trt_VirtualAlloc(arena, page, 0x1000u, 0x04u) == arena &&
                data[0] == 0,
            "explicit recommit returns zeroed bytes after decommit");
      sigaction(SIGSEGV, &previous, nullptr);
    }
    CHECK(trt_VirtualFree(arena, 0, 0x8000u) &&
              trt_mem_budget_committed() == before_arena &&
              trt_mem_budget_reserved() == before_arena_va,
          "release returns both memory ledgers to baseline");
  }
  trt_va_enable_deferred_data(0);
  trt_set_vmem_hooks(nullptr, nullptr, nullptr);

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}
