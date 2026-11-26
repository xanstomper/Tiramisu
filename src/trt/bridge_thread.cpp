// tiramisu/src/trt/bridge_thread.cpp
// Threads, synchronization primitives, IOCP, context control, and Win32 TLS.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "trt.h"
#include "thunk.h"
#include "images.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#if defined(__linux__)
#include <sys/syscall.h>  // gettid for the CreateThread provenance log
#include <unistd.h>
#endif

// WaitOnAddress fail-visible accounting, defined in bridge_kernel32.cpp.
extern std::atomic<u64> g_wa_waits;
extern std::atomic<u64> g_wa_wake_single;
extern std::atomic<u64> g_wa_wake_all;
extern std::atomic<u64> g_wa_wake_single_miss;
extern std::atomic<u64> g_wa_wake_all_miss;
extern std::atomic<u64> g_wa_timed_out;

// Debugger sampling hook (definition in bridge_kernel32.cpp): no-op; gdb
// breaks here to stop the process at the moment the CS-stall probe names a
// wedged owner.
extern "C" void trt_cs_stall_probe_pause(void);

// Per-handle file completion-notification modes (definition in
// bridge_kernel32.cpp): bit 0 = FILE_SKIP_COMPLETION_PORT_ON_SUCCESS.
extern "C" int trt_file_io_modes(void* file_handle);
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <intrin.h>  // .243 DIAG: __readgsqword for the PEB LDR module walk
// TerminateThread is desktop-partition in the SDK headers (not declared
// under WINAPI_FAMILY_APP), but callable from the AppContainer at runtime —
// declared directly.
extern "C" int __stdcall TerminateThread(void* hThread,
                                         unsigned long dwExitCode);
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <sched.h>
#  include <signal.h>
#  include <ucontext.h>
#  include <unistd.h>
#endif

extern "C" u32 TRT_STDCALL trt_GetCurrentThreadId(void);
extern "C" u32 trt_alloc_guest_thread_id(void);
extern "C" void trt_bind_guest_thread_id(u32 tid);
// VA ledger (bridge_membudget.cpp): thread stacks reserve VA for the
// thread's lifetime
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);
// WaitOnAddress/WakeByAddress (bridge_kernel32.cpp): the CS blocking wait
extern "C" int TRT_STDCALL trt_WaitOnAddress(volatile void* addr, void* cmp,
                                             u32 size, u32 ms);
extern "C" void TRT_STDCALL trt_WakeByAddressSingle(volatile void* addr);
extern "C" u32 TRT_STDCALL trt_GetLastError(void);

namespace {

enum obj_kind {
  OBJ_NONE = 0,
  OBJ_EVENT,
  OBJ_MUTEX,
  OBJ_SEMAPHORE,
  OBJ_THREAD,
  OBJ_IOCP,
};

#if !defined(_WIN32)
constexpr u32 k_context_amd64 = 0x00100000u;
constexpr u32 k_context_control = k_context_amd64 | 0x00000001u;
constexpr u32 k_context_integer = k_context_amd64 | 0x00000002u;
constexpr u32 k_context_segments = k_context_amd64 | 0x00000004u;
constexpr u32 k_context_floating_point = k_context_amd64 | 0x00000008u;

struct alignas(16) Win64Context {
  u64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
  u32 ContextFlags;
  u32 MxCsr;
  u16 SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
  u32 EFlags;
  u64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  u64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
  u64 R8, R9, R10, R11, R12, R13, R14, R15;
  u64 Rip;
  u8  LegacySave[160];
  struct alignas(16) { u64 Lo, Hi; } Xmm[16];
  u8  Tail[560];
};
static_assert(offsetof(Win64Context, ContextFlags) == 0x30, "ContextFlags");
static_assert(offsetof(Win64Context, Rax) == 0x78, "Rax");
static_assert(offsetof(Win64Context, Rip) == 0xF8, "Rip");
static_assert(offsetof(Win64Context, Xmm) == 0x1A0, "Xmm0");
static_assert(sizeof(Win64Context) == 1232, "CONTEXT size");
#endif

struct iocp_packet {
  u32 bytes;
  u64 key;
  void* overlapped;
};

struct overlapped_entry_w64 {
  u64 key;
  void* overlapped;
  u64 internal;
  u32 bytes;
  u32 pad;
};
static_assert(sizeof(overlapped_entry_w64) == 32, "OVERLAPPED_ENTRY size");

struct sync_object {
  obj_kind kind = OBJ_NONE;
  u32 generation = 1;
  u32 handle_refs = 0;
  bool signalled = false;
  bool manual_reset = false;
  int lock_count = 0;
  int max_count = 0x7FFFFFFF;
  u32 owner_tid = 0;
  // .221 DIAG: last thread to SetEvent this object + total sets --
  // names the ping-pong partner for a thread parked in
  // WaitForSingleObject on this event.
  u32 last_set_tid = 0;
  u64 set_count = 0;
  // .224 DIAG: thread that created this object -- names the
  // creator of the Sekiro spin mutexes (allocated right after
  // the main event, same generation, adjacent indices).
  u32 creator_tid = 0;
  std::atomic<int> suspend_count{0};
  int priority = 0;
  u32 tid = 0;
  std::atomic<bool> alive{false};
  bool thread_ref = false;
  u32 exit_code = 0;
  std::string name;

  // Startup metadata for CREATE_SUSPENDED SetThreadContext overrides
  void* start_routine = nullptr;
  u64 start_arg = 0;
  bool started = false;

  // VA charged for this thread's stack reserve at creation (1 MB Windows
  // default, 8 MB POSIX default, or the requested stack_size). Discharged
  // exactly once when the thread exits — the field is zeroed on discharge
  // so the trampoline, ExitThread, and TerminateThread paths can't
  // double-discharge.
  u64 stack_va_charge = 0;

  // IOCP queue
  std::deque<iocp_packet> iocp_queue;
  bool iocp_closed = false;

#if defined(_WIN32)
  HANDLE win_handle = nullptr;
#else
  pthread_t pthread_handle{};
  bool has_pthread = false;
  int ack_pipe[2] = {-1, -1};
  int wake_pipe[2] = {-1, -1};
  Win64Context captured_ctx{};
  std::atomic<bool> has_captured_ctx{false};
  std::atomic<bool> ctx_dirty{false};
  std::atomic<bool> in_sig_suspend{false};
#endif

  void clear_payload() {
    kind = OBJ_NONE;
    handle_refs = 0;
    signalled = false;
    manual_reset = false;
    lock_count = 0;
    max_count = 0x7FFFFFFF;
    owner_tid = 0;
    // .224 FIX: these were never reset on slot reuse, so a
    // recycled slot carried a dead object set/creator history
    // into the census.
    last_set_tid = 0;
    set_count = 0;
    creator_tid = 0;
    suspend_count.store(0, std::memory_order_relaxed);
    priority = 0;
    tid = 0;
    alive.store(false, std::memory_order_relaxed);
    thread_ref = false;
    exit_code = 0;
    name.clear();
    start_routine = nullptr;
    start_arg = 0;
    started = false;
    stack_va_charge = 0;
    iocp_queue.clear();
    iocp_closed = false;
#if defined(_WIN32)
    win_handle = nullptr;
#else
    has_pthread = false;
    ack_pipe[0] = ack_pipe[1] = -1;
    wake_pipe[0] = wake_pipe[1] = -1;
    memset(&captured_ctx, 0, sizeof(captured_ctx));
    has_captured_ctx.store(false, std::memory_order_relaxed);
    ctx_dirty.store(false, std::memory_order_relaxed);
    in_sig_suspend.store(false, std::memory_order_relaxed);
#endif
  }
};

std::mutex g_obj_mu;
std::condition_variable g_sync_cv;
std::deque<sync_object> g_objects;  // pointer-stable across push_back!
std::vector<u32> g_free_indices;
std::unordered_map<std::string, u64> g_named_objects;

struct iocp_assoc {
  u64 iocp_handle;
  u64 completion_key;
};
std::unordered_map<u64, iocp_assoc> g_iocp_assocs;

struct apc_item {
  void* fn = nullptr;
  u8 arity = 1;  // 1 = QueueUserAPC(dwData), 3 = FileIOCompletionRoutine(err, bytes, ov)
  u64 a0 = 0;
  u64 a1 = 0;
  u64 a2 = 0;
};
std::unordered_map<u32, std::deque<apc_item>> g_apc_queues;

// .221 DIAG: parked-wait table -- every thread currently parked
// inside wait_single_impl / wait_multiple_impl, so the gap final
// dump can name the thread that never wakes and the event it
// waits on (Sekiro's terminal wait). Guarded by g_obj_mu;
// entries are added before the park loop and removed by an RAII
// guard on every exit path. One slot per tid (a nested wait
// overwrites the outer one -- the innermost wait is the live one).
struct parked_wait {
  u32 tid = 0;
  u64 handle = 0;
  u32 kind = 0;
  u32 handle_count = 1;
  u64 since_ms = 0;
};
static parked_wait g_parked_waits[32];
static u64 park_now_ms() {
  return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void park_wait_add(u32 tid, u64 handle, u32 kind, u32 count) {
  for (int i = 0; i < 32; ++i) {
    if (g_parked_waits[i].tid == tid) {
      g_parked_waits[i].handle = handle;
      g_parked_waits[i].kind = kind;
      g_parked_waits[i].handle_count = count;
      g_parked_waits[i].since_ms = park_now_ms();
      return;
    }
  }
  for (int i = 0; i < 32; ++i) {
    if (g_parked_waits[i].tid == 0) {
      g_parked_waits[i].tid = tid;
      g_parked_waits[i].handle = handle;
      g_parked_waits[i].kind = kind;
      g_parked_waits[i].handle_count = count;
      g_parked_waits[i].since_ms = park_now_ms();
      return;
    }
  }
}
static void park_wait_remove(u32 tid) {
  for (int i = 0; i < 32; ++i) {
    if (g_parked_waits[i].tid == tid) {
      g_parked_waits[i].tid = 0;
      g_parked_waits[i].handle = 0;
      return;
    }
  }
}
struct parked_wait_guard {
  u32 tid;
  bool active;
  parked_wait_guard(u32 t, u64 h, u32 k, u32 c) : tid(t), active(true) {
    park_wait_add(t, h, k, c);
  }
  ~parked_wait_guard() {
    if (active) park_wait_remove(tid);
  }
};

// .225 DIAG: guest return-address validation + lost-wakeup pairing +
// gate trace (Sekiro). All state lives in this file and is guarded by
// g_obj_mu; the helpers below REQUIRE the lock held. Three parts:
// (1) SPIN RETADDR: wait_single_impl threads the export wrapper guest
// return address down as guest_ret; an address in a guest image is the
// spin loop call site, an off-image address (the internal
// WaitForMultipleObjects path) is 0. The offset is against the main
// guest image, like the .219/.224 CV wait-site.
// (2) LOST-WAKEUP: the pairing of a thread spinning on self-owned
// objects with the only worker parked on a never-set start event.
// (3) GATE TRACE: the sync-level record of what the start event
// creator does between the event creation and the death -- event
// allocs, sets, and INFINITE parks, in order -- plus a positive
// SET-NEVER line for a watched start event nobody ever sets.
sync_object* object_for_locked(u64 h, u32* out_idx);

static bool ret_in_guest_image(u64 addr) {
  if (!addr) return false;
  const int n = trt_images_count();
  for (int i = 0; i < n; ++i) {
    const trt_image* im = trt_images_nth(i);
    if (!im || !im->base || !im->size_of_image) continue;
    const u64 b = (u64)(uintptr_t)im->base;
    if (addr >= b && addr < b + im->size_of_image) return true;
  }
  return false;
}

struct gate_ring_entry {
  u32 tid = 0;
  u32 op = 0;  // 1=ALLOC-EVENT 2=SET 3=PARK
  u64 handle = 0;
  u32 kind = 0;
};
static gate_ring_entry g_gate_ring[64];
static unsigned g_gate_ring_n = 0;
static void gate_ring_record(u32 tid, u32 op, u64 handle, u32 kind) {
  if (g_gate_ring_n >= 64) return;
  g_gate_ring[g_gate_ring_n].tid = tid;
  g_gate_ring[g_gate_ring_n].op = op;
  g_gate_ring[g_gate_ring_n].handle = handle;
  g_gate_ring[g_gate_ring_n].kind = kind;
  ++g_gate_ring_n;
}

struct watched_start {
  u64 handle = 0;
  u32 creator_tid = 0;
  u32 parked_tid = 0;
  unsigned set_hits = 0;
};
static watched_start g_watched[8];
static int g_watched_n = 0;
static watched_start* watched_find(u64 handle) {
  for (int i = 0; i < g_watched_n; ++i)
    if (g_watched[i].handle == handle) return &g_watched[i];
  return nullptr;
}
static void watched_add(u64 handle, u32 creator_tid, u32 parked_tid) {
  if (watched_find(handle)) return;
  if (g_watched_n >= 8) return;
  g_watched[g_watched_n].handle = handle;
  g_watched[g_watched_n].creator_tid = creator_tid;
  g_watched[g_watched_n].parked_tid = parked_tid;
  g_watched[g_watched_n].set_hits = 0;
  ++g_watched_n;
}

static void dump_gate_trace(const char* why) {
  // Requires g_obj_mu held (all call sites hold it).
  char head[160];
  snprintf(head, sizeof(head),
           "gap: GATE-TRACE why=%s entries=%u watched=%d", why,
           g_gate_ring_n, g_watched_n);
  trt_core_log(head);
  for (unsigned i = 0; i < g_gate_ring_n; ++i) {
    const gate_ring_entry& e = g_gate_ring[i];
    char line[192];
    snprintf(line, sizeof(line),
             "gap: GATE-TRACE #%u tid=%u op=%u handle=%llx kind=%u", i,
             e.tid, e.op, (unsigned long long)e.handle, e.kind);
    trt_core_log(line);
  }
  for (int i = 0; i < g_watched_n; ++i) {
    const sync_object* o = object_for_locked(g_watched[i].handle, nullptr);
    char line[256];
    if (o && o->set_count == 0) {
      snprintf(line, sizeof(line),
               "sync: SET-NEVER handle=%llx parked_tid=%u creator=%u sets=0",
               (unsigned long long)g_watched[i].handle,
               g_watched[i].parked_tid, g_watched[i].creator_tid);
    } else {
      snprintf(line, sizeof(line),
               "sync: SET-WATCHED-SUMMARY handle=%llx parked_tid=%u creator=%u sets=%llu lastset=%u hits=%u",
               (unsigned long long)g_watched[i].handle,
               g_watched[i].parked_tid, g_watched[i].creator_tid,
               o ? (unsigned long long)o->set_count : 0ull,
               o ? o->last_set_tid : 0u, g_watched[i].set_hits);
    }
    trt_core_log(line);
  }
}

static void maybe_log_lost_wakeup(u32 spin_tid) {
  // Requires g_obj_mu held. One shot: the first time a spinning
  // thread (self-owned mutexes) pairs with a worker parked forever
  // on a never-set start event created by that same thread.
  static bool done = false;
  if (done) return;
  for (int i = 0; i < 32; ++i) {
    if (!g_parked_waits[i].tid) continue;
    const sync_object* o = object_for_locked(g_parked_waits[i].handle, nullptr);
    if (!o || o->kind != OBJ_EVENT || o->set_count != 0) continue;
    if (spin_tid != 0 && o->creator_tid != spin_tid) continue;
    const u32 creator = o->creator_tid;
    bool self_owned = false;
    for (u32 idx = 0; idx < g_objects.size(); ++idx) {
      const sync_object& m = g_objects[idx];
      if (m.kind == OBJ_MUTEX && m.creator_tid == creator &&
          m.owner_tid == creator && m.lock_count >= 1) {
        self_owned = true;
        break;
      }
    }
    if (!self_owned) continue;
    done = true;
    char line[256];
    snprintf(line, sizeof(line),
             "gap: LOST-WAKEUP spin_tid=%u parked_tid=%u handle=%llx creator=%u sets=0",
             creator, g_parked_waits[i].tid,
             (unsigned long long)g_parked_waits[i].handle, creator);
    trt_core_log(line);
    dump_gate_trace("spin-trip");
    return;
  }
}

// .226 DIAG: spin-function range + post-wake watch (Sekiro). All state
// lives in this file and is guarded by g_obj_mu; the helpers below
// REQUIRE the lock held (same as the .225 helpers above).
//
// (a) FN-OFF: the WAIT-FAST ret_off names a call site, not its function.
// spin_fn_range binary-searches the main image's own .pdata (the same
// table lookup_guest_pdata uses in bridge_kernel32.cpp) for the
// RUNTIME_FUNCTION whose [begin,end) contains the return address, and
// returns its start offset + length -- so the five spin sites
// (0x1a5e26c..0x1a5e555) resolve to one function span, from runtime
// data only (no game-file inspection, no symbols needed).
// (b) SPIN TABLE: every immediate-return (fast) wait is counted per
// handle, not just the sampled ones, so the census can say exactly how
// many times the loop waited on each object.
// (c) POST-WAKE: when a watched start event wakes a worker, record the
// wake; the census then reports whether that worker ever called again
// and whether its thread is still alive -- the exit-vs-stuck
// discriminator for the silent death (no bridged exit call exists).
struct spin_pdata_fn {
  u32 begin = 0;
  u32 end = 0;
  u32 unwind = 0;
};

static bool spin_fn_range(u64 retaddr, u64* fn_off, u64* fn_len) {
  if (fn_off) *fn_off = 0;
  if (fn_len) *fn_len = 0;
  if (!retaddr) return false;
  const trt_image* img = trt_images_first();
  if (!img || !img->base || !img->size_of_image || !img->pdata ||
      !img->pdata_count)
    return false;
  const u64 b = (u64)(uintptr_t)img->base;
  if (retaddr < b || retaddr >= b + img->size_of_image) return false;
  const u32 rva = (u32)(retaddr - b);
  const spin_pdata_fn* fns = (const spin_pdata_fn*)img->pdata;
  u32 lo = 0, hi = img->pdata_count;
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2;
    if (rva < fns[mid].begin) {
      hi = mid;
    } else if (rva >= fns[mid].end) {
      lo = mid + 1;
    } else {
      if (fn_off) *fn_off = (u64)fns[mid].begin;
      if (fn_len) *fn_len = (u64)(fns[mid].end - fns[mid].begin);
      return true;
    }
  }
  return false;
}

struct spin_tab_entry {
  u64 handle = 0;
  u32 kind = 0;
  unsigned long long fast = 0;
  // .228 DIAG: the FINAL fast wait per handle (SPIN-LAST) --
  // the live WAIT-FAST lines only sample the first 32 + every
  // 100th, so the last iteration (n=120 in .227) was invisible.
  unsigned long long last_n = 0;
  u64 last_ret_off = 0;
  u64 last_fn_off = 0;
};
static spin_tab_entry g_spin_tab[16];
static unsigned g_spin_tab_n = 0;
// .227 DIAG: total fast waits (all handles), for the
// before/after-wake split in the census SPIN-SUMMARY.
static unsigned long long g_spin_fast_total = 0;
static void spin_tab_count(u64 handle, u32 kind, u64 ret_off,
                         u64 fn_off, unsigned long long n) {
  ++g_spin_fast_total;
  for (unsigned i = 0; i < g_spin_tab_n; ++i) {
    if (g_spin_tab[i].handle == handle) {
      ++g_spin_tab[i].fast;
      g_spin_tab[i].last_n = n;
      g_spin_tab[i].last_ret_off = ret_off;
      g_spin_tab[i].last_fn_off = fn_off;
      return;
    }
  }
  if (g_spin_tab_n >= 16) return;
  g_spin_tab[g_spin_tab_n].handle = handle;
  g_spin_tab[g_spin_tab_n].kind = kind;
  g_spin_tab[g_spin_tab_n].fast = 1;
  g_spin_tab[g_spin_tab_n].last_n = n;
  g_spin_tab[g_spin_tab_n].last_ret_off = ret_off;
  g_spin_tab[g_spin_tab_n].last_fn_off = fn_off;
  ++g_spin_tab_n;
}

// .228 DIAG: main-IP sampler arming state (see
// spin_main_ip_sampler below). Set from the WAIT-FAST path
// on every fast wait; read by the sampler thread only.
static std::atomic<u32> g_spin_main_tid{0};
static std::atomic<unsigned long long> g_spin_last_fast_ms{0};
static std::atomic<bool> g_spin_sampler_started{false};
static std::atomic<bool> g_spin_worker_exit_seen{false};
static std::atomic<unsigned> g_spin_samples_done{0};
// .233 DIAG: wall time of the last loop-sampler sample -- the
// 2s spacing gate below reads it (see spin_main_ip_sampler).
static std::atomic<unsigned long long> g_spin_last_sample_ms{0};
// .230 FIX: the WAKE-MAIN SetEvent fires at most once per
// process run (see the sampler).
static std::atomic<bool> g_spin_wake_done{false};
// .231 DIAG: tid of the most recent ExitThread (set in
// trt_ExitThread); the WAITED-THREAD line compares it
// with the waited-on TID.
static std::atomic<u32> g_spin_worker_exit_tid{0};
static void spin_main_ip_sampler();
// .229 DIAG: helpers for the richer MAIN-IP sample (defined
// after the sampler, before thread_alive_locked).
static bool spin_host_nearest_export(u64 rip, char* name_buf,
                                     size_t name_cap, u64* off_out);
static unsigned spin_read_stack_qwords(u64 rsp, u64* out,
                                       unsigned n);
// .241 FIX: the stack reader calls the guarded qword read
// (defined below); the forward declaration keeps the call
// compiling.
static int spin_read_qword_guarded(u64 addr, u64* out);
// .230 FIX-DESIGN: NtQueryObject type (+ name) for the
// waited-on handle (defined after the sampler, next to
// the other .229 helpers).
static bool spin_query_handle_info(void* h, char* type_buf,
                                   size_t type_cap, char* name_buf,
                                   size_t name_cap);
static unsigned long long spin_now_ms() {
  return (unsigned long long)std::chrono::duration_cast<
      std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void spin_note_fast_wait(u32 tid) {
  g_spin_main_tid.store(tid, std::memory_order_relaxed);
  g_spin_last_fast_ms.store(spin_now_ms(),
                            std::memory_order_relaxed);
  bool expected = false;
  if (g_spin_sampler_started.compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    std::thread([] { spin_main_ip_sampler(); }).detach();
  }
}

struct woke_entry {
  u32 tid = 0;
  u64 handle = 0;
  std::chrono::steady_clock::time_point at{};
  bool reported = false;
};
static woke_entry g_woke[4];
static unsigned g_woke_n = 0;
// .227 DIAG: spin split at first wake -- per-handle fast counts
// frozen when the first watched worker wakes, so the census can
// say whether the spin loop stopped at the wake
// (worker-exit-driven) or ran on (bounded count / timeout).
// Snapshot taken once, in woke_record.
static unsigned long long g_spin_tab_at_wake[16];
static bool g_wake_split_done = false;
static void woke_record(u32 tid, u64 handle) {
  // .227 DIAG: freeze the spin table at the first wake (see above).
  if (!g_wake_split_done) {
    g_wake_split_done = true;
    for (unsigned i = 0; i < g_spin_tab_n; ++i)
      g_spin_tab_at_wake[i] = g_spin_tab[i].fast;
  }
  for (unsigned i = 0; i < g_woke_n; ++i) {
    if (g_woke[i].handle == handle && g_woke[i].tid == tid) {
      g_woke[i].at = std::chrono::steady_clock::now();
      g_woke[i].reported = false;
      return;
    }
  }
  if (g_woke_n >= 4) return;
  g_woke[g_woke_n].tid = tid;
  g_woke[g_woke_n].handle = handle;
  g_woke[g_woke_n].at = std::chrono::steady_clock::now();
  g_woke[g_woke_n].reported = false;
  ++g_woke_n;
}

// .228 DIAG: main-IP sampler -- Sekiro's main thread makes no
// bridged calls after the spin loop ends (#506), so the bridge
// cannot see what it does next. This host watchdog samples the
// main thread's RIP directly: after the spinning tid goes 200ms
// without a fast wait AND a worker ExitThread has been seen, it
// OpenThread/SuspendThread/GetThreadContext/ResumeThread's the
// main thread 3x (~100ms apart) and logs the RIP + enclosing
// .pdata function + RSP. The sample reads the context only --
// no bridge lock is held while the target is suspended, and all
// logging + fn lookup happen AFTER ResumeThread, so a main
// thread suspended while holding a lock cannot deadlock the
// sampler. The suspend window is microseconds (one
// GetThreadContext); this is standard debugger behavior, not a
// guest code patch (no VEH, no INT3 -- see the header note on
// why the spin-return trap was not done). POSIX: no sampler
// (OpenThread is Win32-only); the thread exits immediately.
// .229 DIAG: faster + richer -- first sample at +200ms of main
// silence, then every ~100ms (3 samples inside ~500ms, before
// the PLM suspend freezes the sampler); CONTEXT_FULL for the
// argument registers (RCX = the waited-on handle for
// NtWaitForSingleObject); the stack qwords [RSP..RSP+64] read
// while suspended; and the nearest host export for the RIP
// (the .pdata lookup only covers guest images, so it reports
// fn_off 0 for host code). The stack return address [RSP] is
// resolved through the guest .pdata as well (stkfn=) -- it is
// where main returns when the host wait ends.
// .230 FIX: when the sample names ntdll!NtWaitForSingle-
// Object, the sampler names the waited-on handle (NtQuery-
// Object type + name) and the stack return address through
// the host-export enumeration (stkhost= -- .229 left it as
// the guest .pdata lookup, which reports 0 for host code).
// When the handle is an Event, the sampler signals it once
// (WAKE-MAIN) -- the worker that was supposed to signal it
// has already exited (the sampler only runs after a worker
// ExitThread), so without the wake main is parked in the
// wait forever.
static void spin_main_ip_sampler() {
#if defined(_WIN32)
  for (int iter = 0; iter < 6000; ++iter) {  // up to ~10 min
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // .239 DIAG: budget 6 -> 12 (see the gap sampler note).
    if (g_spin_samples_done.load(std::memory_order_relaxed) >= 12)
      return;
    const u32 tid = g_spin_main_tid.load(std::memory_order_relaxed);
    if (!tid) continue;
    if (!g_spin_worker_exit_seen.load(std::memory_order_relaxed))
      continue;
    const unsigned long long last =
        g_spin_last_fast_ms.load(std::memory_order_relaxed);
    if (!last) continue;
    if (spin_now_ms() < last + 200) continue;
    // .233 DIAG: space loop samples >=2s apart -- .232 spent its
    // whole 3-sample budget on ONE silence (audio init), so the
    // second death (after main advanced past the first fix) got
    // no sample. A slot now costs once per 2s, so n=2 can land on
    // the next silence instead of 100ms after n=1.
    if (spin_now_ms() <
        g_spin_last_sample_ms.load(std::memory_order_relaxed) + 2000)
      continue;
    const unsigned n =
        g_spin_samples_done.fetch_add(1, std::memory_order_relaxed) + 1;
    g_spin_last_sample_ms.store(spin_now_ms(),
                                std::memory_order_relaxed);
    HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION,
                            FALSE, tid);
    if (!h) {
      char sl[160];
      snprintf(sl, sizeof(sl),
               "sync: MAIN-IP-SAMPLE n=%u tid=%u open-fail err=%lu",
               n, tid, (unsigned long)::GetLastError());
      trt_core_log(sl);
      return;  // main thread is gone
    }
    const DWORD sus = ::SuspendThread(h);
    if (sus == (DWORD)-1) {
      const DWORD err = ::GetLastError();
      ::CloseHandle(h);
      char sl[160];
      snprintf(sl, sizeof(sl),
               "sync: MAIN-IP-SAMPLE n=%u tid=%u sus-fail err=%lu",
               n, tid, (unsigned long)err);
      trt_core_log(sl);
      return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    const BOOL ok = ::GetThreadContext(h, &ctx);
    const u64 rip = ok ? (u64)ctx.Rip : 0;
    const u64 rsp = ok ? (u64)ctx.Rsp : 0;
    // .229 DIAG: first four argument registers (NtWaitForSingle-
    // Object takes its handle in RCX; RtlExitUserProcess its
    // code in RCX; NtTerminateProcess its process handle in RCX
    // + code in RDX) + the stack qwords at [RSP..RSP+64], read
    // WHILE suspended (the target runs again after Resume-
    // Thread, so a later read would race). Same-process reads
    // via the POD helper below (no C++ objects across __try).
    const u64 rcx = ok ? (u64)ctx.Rcx : 0;
    const u64 rdx = ok ? (u64)ctx.Rdx : 0;
    const u64 r8 = ok ? (u64)ctx.R8 : 0;
    const u64 r9 = ok ? (u64)ctx.R9 : 0;
    u64 stk[9] = {};
    const unsigned stk_n =
        (ok && rsp) ? spin_read_stack_qwords(rsp, stk, 9) : 0;
    ::ResumeThread(h);
    ::CloseHandle(h);
    // Fn lookup AFTER resume (see header comment).
    u64 fn_off = 0, fn_len = 0, rip_off = 0;
    if (rip) {
      spin_fn_range(rip, &fn_off, &fn_len);
      const trt_image* img = trt_images_first();
      if (img && img->base && img->size_of_image) {
        const u64 b = (u64)(uintptr_t)img->base;
        if (rip >= b && rip < b + img->size_of_image) rip_off = rip - b;
      }
    }
    // .229 DIAG: nearest host export for the RIP (the .pdata
    // lookup only covers guest images) + the guest fn of the
    // stack return address [RSP] (where main returns when the
    // host wait ends).
    char ntfn[160];
    snprintf(ntfn, sizeof(ntfn), "none");
    u64 ntoff = 0;
    if (rip) spin_host_nearest_export(rip, ntfn, sizeof(ntfn), &ntoff);
    u64 stk_fn_off = 0, stk_fn_len = 0;
    if (stk_n > 0) spin_fn_range(stk[0], &stk_fn_off, &stk_fn_len);
    char sl[512];
    snprintf(sl, sizeof(sl),
             "sync: MAIN-IP-SAMPLE n=%u tid=%u rip=0x%llx rip_off=0x%llx fn_off=0x%llx fn_len=0x%llx rsp=0x%llx sus=%lu ok=%d ntfn=%s ntoff=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx stkret=0x%llx stkfn=0x%llx",
             n, tid, (unsigned long long)rip,
             (unsigned long long)rip_off,
             (unsigned long long)fn_off,
             (unsigned long long)fn_len,
             (unsigned long long)rsp, (unsigned long)sus,
             ok ? 1 : 0, ntfn, (unsigned long long)ntoff,
             (unsigned long long)rcx, (unsigned long long)rdx,
             (unsigned long long)r8, (unsigned long long)r9,
             (unsigned long long)(stk_n > 0 ? stk[0] : 0),
             (unsigned long long)stk_fn_off);
    trt_core_log(sl);
    // .230 FIX-DESIGN: name the waited-on handle + the
    // wrapper, and wake main when the design allows it.
    // The sampler only runs after a worker ExitThread has
    // been seen (see the gate above), so by the time this
    // fires the thread that was supposed to signal the
    // event is gone -- main is parked forever unless the
    // bridge signals it. The wake is gated on the handle
    // type being exactly "Event" (NtQueryObject) and fires
    // once per process run; any other type is named and
    // left alone.
    if (std::string(ntfn) == "ntdll!NtWaitForSingleObject") {
      char stkhost[160];
      snprintf(stkhost, sizeof(stkhost), "none");
      u64 stkoff = 0;
      if (stk_n > 0)
        spin_host_nearest_export(stk[0], stkhost, sizeof(stkhost),
                                 &stkoff);
      char htype[64];
      char hname[128];
      snprintf(htype, sizeof(htype), "unknown");
      snprintf(hname, sizeof(hname), "none");
      const bool hok = spin_query_handle_info(
          (HANDLE)(uintptr_t)rcx, htype, sizeof(htype), hname,
          sizeof(hname));
      char hl[384];
      snprintf(hl, sizeof(hl),
               "sync: MAIN-HANDLE n=%u handle=0x%llx ok=%d type=%s name=%s stkhost=%s stkoff=0x%llx",
               n, (unsigned long long)rcx, hok ? 1 : 0, htype,
               hname, stkhost, (unsigned long long)stkoff);
      trt_core_log(hl);
      if (hok && std::string(htype) == "Event" &&
          !g_spin_wake_done.exchange(true,
                                     std::memory_order_relaxed)) {
        const BOOL set_ok = ::SetEvent((HANDLE)(uintptr_t)rcx);
        const DWORD set_err = set_ok ? 0 : ::GetLastError();
        char wl[320];
        snprintf(wl, sizeof(wl),
                 "sync: WAKE-MAIN n=%u handle=0x%llx type=Event name=%s setevent=%d err=%lu",
                 n, (unsigned long long)rcx, hname,
                 set_ok ? 1 : 0, (unsigned long)set_err);
        trt_core_log(wl);
      }
      // .231 DIAG: the waited-on handle is a THREAD, not
      // an Event -- name the thread (GetThreadId) + whether
      // it is still alive (GetExitCodeThread: STILL_ACTIVE
      // = 259) + whether it is main itself (self-wait: the
      // wait can never return) or the worker that already
      // exited (the wait should already have returned).
      // Then sample the waited-on thread once (same
      // suspend trick as the main sampler) so .232 knows
      // what THAT thread is blocked on. No wait-faking
      // here: terminating the waited-on thread or faking
      // the syscall return is not safe until the TID is
      // known.
      if (hok && std::string(htype) == "Thread") {
        const DWORD wtid = ::GetThreadId((HANDLE)(uintptr_t)rcx);
        const DWORD wtid_err = wtid ? 0 : ::GetLastError();
        DWORD wexit = 0;
        const BOOL wok = ::GetExitCodeThread((HANDLE)(uintptr_t)rcx,
                                           &wexit);
        const bool walive = wok && wexit == STILL_ACTIVE;
        const u32 wexittid = g_spin_worker_exit_tid.load(
            std::memory_order_relaxed);
        char tl[384];
        snprintf(tl, sizeof(tl),
                 "sync: WAITED-THREAD n=%u handle=0x%llx wtid=%lu wtid_err=%lu exit_ok=%d exitcode=%lu alive=%d workertid=%u ismain=%d isworker=%d",
                 n, (unsigned long long)rcx,
                 (unsigned long)wtid, (unsigned long)wtid_err,
                 wok ? 1 : 0, (unsigned long)wexit,
                 walive ? 1 : 0, wexittid,
                 (wtid && wtid == tid) ? 1 : 0,
                 (wtid && wexittid && wtid == wexittid) ? 1 : 0);
        trt_core_log(tl);
        // Sample the waited-on thread's RIP (skip when it
        // is main itself -- MAIN-IP-SAMPLE above already
        // has it -- or when it already exited / has no TID).
        if (wtid && wtid != tid && walive) {
          HANDLE wh = ::OpenThread(THREAD_SUSPEND_RESUME |
                                   THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                                   FALSE, wtid);
          if (!wh) {
            char cl[192];
            snprintf(cl, sizeof(cl),
                     "sync: WAITED-THREAD-CTX n=%u wtid=%lu open=0 err=%lu",
                     n, (unsigned long)wtid,
                     (unsigned long)::GetLastError());
            trt_core_log(cl);
          } else {
            const DWORD wsus = ::SuspendThread(wh);
            if (wsus == (DWORD)-1) {
              const DWORD werr = ::GetLastError();
              ::CloseHandle(wh);
              char cl[192];
              snprintf(cl, sizeof(cl),
                       "sync: WAITED-THREAD-CTX n=%u wtid=%lu open=1 sus-fail err=%lu",
                       n, (unsigned long)wtid, (unsigned long)werr);
              trt_core_log(cl);
            } else {
              CONTEXT wctx{};
              wctx.ContextFlags = CONTEXT_FULL;
              const BOOL wok2 = ::GetThreadContext(wh, &wctx);
              const u64 wrip = wok2 ? (u64)wctx.Rip : 0;
              const u64 wrcx = wok2 ? (u64)wctx.Rcx : 0;
              const u64 wrdx = wok2 ? (u64)wctx.Rdx : 0;
              const u64 wrsp = wok2 ? (u64)wctx.Rsp : 0;
              u64 wstk[5] = {};
              const unsigned wstk_n =
                  (wok2 && wrsp)
                      ? spin_read_stack_qwords(wrsp, wstk, 5)
                      : 0;
              // .232 DIAG: Win32 start address of the
              // waited-on thread (NtQueryInformationThread,
              // ThreadQuerySetWin32StartAddress = 9). The
              // worker was created by ::CreateThread with
              // win_thread_trampoline, so start should
              // name the trampoline; anything else names
              // a foreign thread.
              u64 wstart = 0;
              {
                typedef long(NTAPI* NtQueryInformationThread_fn)(
                    HANDLE, unsigned long, void*, unsigned long,
                    unsigned long*);
                static NtQueryInformationThread_fn wqit = nullptr;
                static bool wqit_tried = false;
                if (!wqit_tried) {
                  wqit_tried = true;
                  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
                  if (ntdll)
                    wqit = (NtQueryInformationThread_fn)::GetProcAddress(
                        ntdll, "NtQueryInformationThread");
                }
                if (wqit) {
                  void* sa = nullptr;
                  unsigned long rl = 0;
                  if (wqit(wh, 9, &sa, sizeof(sa), &rl) == 0)
                    wstart = (u64)(uintptr_t)sa;
                }
              }
              ::ResumeThread(wh);
              ::CloseHandle(wh);
              char wntfn[160];
              snprintf(wntfn, sizeof(wntfn), "none");
              u64 wntoff = 0;
              if (wrip)
                spin_host_nearest_export(wrip, wntfn, sizeof(wntfn),
                                         &wntoff);
              char cl[640];
              snprintf(cl, sizeof(cl),
                       "sync: WAITED-THREAD-CTX n=%u wtid=%lu ok=%d rip=0x%llx ntfn=%s ntoff=0x%llx rcx=0x%llx rdx=0x%llx rsp=0x%llx start=0x%llx q0=0x%llx q1=0x%llx q2=0x%llx q3=0x%llx q4=0x%llx",
                       n, (unsigned long)wtid, wok2 ? 1 : 0,
                       (unsigned long long)wrip, wntfn,
                       (unsigned long long)wntoff,
                       (unsigned long long)wrcx,
                       (unsigned long long)wrdx,
                       (unsigned long long)wrsp,
                       (unsigned long long)wstart,
                       (unsigned long long)(wstk_n > 0 ? wstk[0] : 0),
                       (unsigned long long)(wstk_n > 1 ? wstk[1] : 0),
                       (unsigned long long)(wstk_n > 2 ? wstk[2] : 0),
                       (unsigned long long)(wstk_n > 3 ? wstk[3] : 0),
                       (unsigned long long)(wstk_n > 4 ? wstk[4] : 0));
              trt_core_log(cl);
            }
          }
        }
      }
    }
    {
      char kl[640];
      snprintf(kl, sizeof(kl),
               "sync: MAIN-IP-STACK n=%u rsp=0x%llx n=%u q0=0x%llx q1=0x%llx q2=0x%llx q3=0x%llx q4=0x%llx q5=0x%llx q6=0x%llx q7=0x%llx q8=0x%llx",
               n, (unsigned long long)rsp, stk_n,
               (unsigned long long)stk[0], (unsigned long long)stk[1],
               (unsigned long long)stk[2], (unsigned long long)stk[3],
               (unsigned long long)stk[4], (unsigned long long)stk[5],
               (unsigned long long)stk[6], (unsigned long long)stk[7],
               (unsigned long long)stk[8]);
      trt_core_log(kl);
    }
    // .229 DIAG: no post-sample sleep -- the loop 100ms sleep
    // at the top sets the ~100ms sample cadence (the .228
    // 200ms sleep put samples 2-3 past the PLM suspend).
  }
#else
  (void)0;
#endif
}

// .229 DIAG: nearest host export for a sampled RIP, by
// enumerating the export directory of the loaded host images
// (exe + ntdll + kernel32 + kernelbase + user32). The sampler
// RIP is in host code; the .pdata lookup only covers guest
// images, so without this the RIP is an unnamed address.
// Read-only PE parsing, no patching; forwarded exports are
// skipped (their EAT entry is a string RVA, not code).
// Returns false when no export sits at or below the RIP.
// .243 DIAG: PEB LDR module walk -- which host module contains addr?
// The .242 GAP-STACK-HOST resolved q20 (0x7ff80f6cad16) to
// user32!wvsprintfA+0x2440656, an impossible offset (~38MB past the
// export): nearest-export is meaningless when no export sits near
// the address. The containing module + module-relative offset names
// the waiter anyway (d3d11.dll+0x12345, or a host thread-pool DLL).
// Reads the current process's PEB loader list (GS:0x60 -> PEB+0x18
// Ldr -> InMemoryOrderModuleList); EVERY pointer is read through
// spin_read_qword_guarded -- the loader may mutate the list while
// a DLL loads, and a torn read must stop the walk, never fault.
// LDR_DATA_TABLE_ENTRY (x64): InMemoryOrderLinks +0x10, DllBase
// +0x30, SizeOfImage +0x40, BaseDllName (UNICODE_STRING) +0x58
// (Length +0x58, Buffer +0x60). Validates MZ + sane SizeOfImage
// per entry. Name is copied qword-wise (guarded) and truncated to
// the buffer. Returns false when no loaded module contains addr.
static bool spin_host_module_of(u64 addr, char* name_buf, size_t name_cap,
                                u64* off_out) {
#if defined(_WIN32)
  if (!addr || !name_buf || name_cap == 0) return false;
  const u64 peb = (u64)(uintptr_t)__readgsqword(0x60);
  if (!peb) return false;
  u64 ldr = 0;
  if (!spin_read_qword_guarded(peb + 0x18, &ldr) || !ldr) return false;
  u64 first = 0;
  if (!spin_read_qword_guarded(ldr + 0x20, &first) || !first) return false;
  const u64 head_addr = ldr + 0x20;
  u64 link = first;
  for (unsigned i = 0; i < 512; ++i) {
    if (!link || link == head_addr) break;
    u64 next = 0;
    if (!spin_read_qword_guarded(link, &next)) return false;
    const u64 entry = link - 0x10;
    u64 base = 0;
    u64 size_qw = 0;
    if (!spin_read_qword_guarded(entry + 0x30, &base) ||
        !spin_read_qword_guarded(entry + 0x40, &size_qw)) {
      if (!next || next == head_addr) break;
      link = next;
      continue;
    }
    const u64 size = size_qw & 0xffffffffULL;
    if (base >= 0x10000ULL && size >= 0x1000ULL && size < 0x80000000ULL &&
        addr >= base && addr < base + size) {
      u64 mz_qw = 0;
      if (!spin_read_qword_guarded(base, &mz_qw) ||
          (mz_qw & 0xffffULL) != 0x5a4dULL)
        return false;
      u64 name_qw = 0;
      u64 name_buf_ptr = 0;
      if (!spin_read_qword_guarded(entry + 0x58, &name_qw) ||
          !spin_read_qword_guarded(entry + 0x60, &name_buf_ptr) ||
          !name_buf_ptr)
        return false;
      const unsigned nchars = (unsigned)((name_qw & 0xffffULL) / 2);
      const unsigned want = nchars < (unsigned)(name_cap - 1)
                                ? nchars
                                : (unsigned)(name_cap - 1);
      unsigned got = 0;
      for (unsigned j = 0; j < (want + 3) / 4 && got < want; ++j) {
        u64 qw = 0;
        if (!spin_read_qword_guarded(name_buf_ptr + (u64)j * 8, &qw)) break;
        for (unsigned k = 0; k < 4 && got < want; ++k) {
          const unsigned wc = (unsigned)((qw >> (k * 16)) & 0xffffU);
          if (wc == 0 || wc >= 0x80U) {
            name_buf[got] = 0;
            if (off_out) *off_out = addr - base;
            return got > 0;
          }
          name_buf[got++] = (char)wc;
        }
      }
      name_buf[got] = 0;
      if (off_out) *off_out = addr - base;
      return got > 0;
    }
    if (!next || next == head_addr) break;
    link = next;
  }
  return false;
#else
  (void)addr;
  (void)name_buf;
  (void)name_cap;
  (void)off_out;
  return false;
#endif
}

static bool spin_host_nearest_export(u64 rip, char* name_buf,
                                     size_t name_cap, u64* off_out) {
#if defined(_WIN32)
  static const struct {
    const wchar_t* mod;
    const char* label;
  } k_mods[] = {
      {nullptr, "exe"}, {L"ntdll.dll", "ntdll"},
      {L"kernel32.dll", "kernel32"}, {L"kernelbase.dll", "kernelbase"},
      {L"user32.dll", "user32"},
      // .236 DIAG: the 0x62c waiter is host code -- let Vulkan / D3D11 /
      // XAudio callers resolve instead of "none".
      {L"vulkan-1.dll", "vulkan"}, {L"d3d11.dll", "d3d11"},
      {L"xaudio2_9.dll", "xaudio2"},
      // .242 DIAG: the 0x644 waiter's second return
      // (q20=0x7ff80f6cad16) sits in a host module the
      // .236 list never covered -- let CRT/COM/winmm
      // callers resolve instead of "none".
      {L"ucrtbase.dll", "ucrtbase"}, {L"msvcrt.dll", "msvcrt"},
      {L"combase.dll", "combase"}, {L"rpcrt4.dll", "rpcrt4"},
      {L"sechost.dll", "sechost"}, {L"winmm.dll", "winmm"},
      {L"bcrypt.dll", "bcrypt"},
  };
  u64 best = 0;
  const char* best_name = nullptr;
  const char* best_mod = nullptr;
  for (unsigned mi = 0; mi < sizeof(k_mods) / sizeof(k_mods[0]); ++mi) {
    HMODULE h = ::GetModuleHandleW(k_mods[mi].mod);
    if (!h) continue;
    const u8* base = (const u8*)h;
    if (*(const u16*)base != 0x5A4D) continue;
    const u32 pe_off = *(const u32*)(base + 0x3C);
    if (pe_off > 4096) continue;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(base + pe_off);
    if (nt->Signature != 0x4550) continue;
    if (nt->OptionalHeader.Magic != 0x20B) continue;
    const IMAGE_DATA_DIRECTORY& dd =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size) continue;
    const IMAGE_EXPORT_DIRECTORY* exp =
        (const IMAGE_EXPORT_DIRECTORY*)(base + dd.VirtualAddress);
    if (exp->NumberOfNames == 0 || exp->NumberOfNames > 100000) continue;
    const u32* names = (const u32*)(base + exp->AddressOfNames);
    const u16* ords = (const u16*)(base + exp->AddressOfNameOrdinals);
    const u32* funcs = (const u32*)(base + exp->AddressOfFunctions);
    for (u32 i = 0; i < exp->NumberOfNames; ++i) {
      const u32 fva = funcs[ords[i]];
      if (fva >= dd.VirtualAddress && fva < dd.VirtualAddress + dd.Size)
        continue;
      const u64 addr = (u64)(uintptr_t)(base + fva);
      if (addr <= rip && addr > best) {
        best = addr;
        best_name = (const char*)(base + names[i]);
        best_mod = k_mods[mi].label;
      }
    }
  }
  if (!best || !best_name) return false;
  // .243 DIAG: offset sanity bound -- the .242 q20 resolved to
  // user32!wvsprintfA+0x2440656 (~38MB past the export), an
  // impossible offset: nearest-export is meaningless when no
  // export sits near the address. An offset >64KB is "unknown"
  // (the GAP-STACK-HOST scan now names the containing module
  // + module-relative offset instead, so nothing is lost).
  if (rip - best > 0x10000ULL) return false;
  snprintf(name_buf, name_cap, "%s!%s", best_mod, best_name);
  if (off_out) *off_out = rip - best;
  return true;
#else
  (void)rip; (void)name_buf; (void)name_cap; (void)off_out;
  return false;
#endif
}

// .229 DIAG: __try-guarded stack-qword reader for the sampler.
// A standalone POD-only helper: __try in the sampler itself
// would risk C2712 (objects with destructors across __try),
// so the guarded reads live here, where every local is POD.
// Returns how many qwords were read before a fault (if any).
static unsigned spin_read_stack_qwords(u64 rsp, u64* out,
                                       unsigned n) {
#if defined(_WIN32)
  // .241 FIX: this function NO LONGER has a __try. The .240
  // build compiled the __try loop below into a bare memcpy
  // (disasm of the .240 exe at RVA 0xf0d20..0xf0db3: unrolled
  // mov loop, no SEH setup; UNWIND_INFO flags=0x0, no
  // exception handler) -- so a fault on a torn-down stack
  // escaped to spin_capture_ip's CAPTURE scope (fn+0xc0..
  // +0x1a0, which covers the call) and blacklisted the tid
  // (the .240 Ultrapool attempt-#3 fault: stages through
  // close, no fnlookup, FAULT 1, TRY stuck at 3). The .238
  // comment's claim that MSVC reliably scopes a top-level
  // __try is WRONG for a try body with no calls: both small
  // guards (this loop and spin_read_qword_guarded, .240 RVA
  // 0xf0a80: 11 bytes, no handler, no .pdata entry) lost
  // their handlers, while spin_fnlookup_guarded (try body
  // WITH calls) kept its scope. So each qword now goes
  // through spin_read_qword_guarded (own frame, call inside
  // its __try -- the surviving shape); a faulting qword
  // stops the loop and the capture returns partial instead
  // of faulting. Verify the guard's scope in the .241 exe's
  // .xdata before deploying.
  unsigned got = 0;
  for (unsigned i = 0; i < n; ++i) {
    u64 qw = 0;
    if (!spin_read_qword_guarded(rsp + (u64)i * 8, &qw)) break;
    out[i] = qw;
    ++got;
  }
  return got;
#else
  (void)rsp; (void)out; (void)n;
  return 0;
#endif
}

// .230 FIX-DESIGN: query a handle's type (+ name) via
// ntdll!NtQueryObject (ObjectTypeInformation = 2,
// ObjectNameInformation = 1). Same-process query: the guest
// runs in-process, so the sampler thread can ask about the
// handle main is waiting on. Returns false when the type
// cannot be read (bad handle, unknown type); the name is
// best-effort ("none" when unnamed or unreadable).
static bool spin_query_handle_info(void* h, char* type_buf,
                                   size_t type_cap, char* name_buf,
                                   size_t name_cap) {
#if defined(_WIN32)
  typedef long(NTAPI* NtQueryObject_fn)(HANDLE, unsigned long,
                                        void*, unsigned long,
                                        unsigned long*);
  static NtQueryObject_fn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
      fn = (NtQueryObject_fn)::GetProcAddress(ntdll, "NtQueryObject");
  }
  if (!fn || !h) return false;
  struct uni_str {
    unsigned short len;
    unsigned short max;
    const wchar_t* ptr;
  };
  unsigned char buf[512];
  unsigned long ret_len = 0;
  if (fn(h, 2, buf, sizeof(buf), &ret_len) != 0) return false;
  const uni_str* tn = (const uni_str*)buf;
  if (!tn->ptr || tn->len == 0 || tn->len > 120) return false;
  unsigned n = 0;
  for (unsigned i = 0; i < tn->len / 2 && n + 1 < type_cap; ++i) {
    const wchar_t c = tn->ptr[i];
    type_buf[n++] = (c < 0x20 || c > 0x7e) ? '?' : (char)c;
  }
  type_buf[n] = 0;
  snprintf(name_buf, name_cap, "none");
  unsigned char nbuf[256];
  if (fn(h, 1, nbuf, sizeof(nbuf), &ret_len) == 0) {
    const uni_str* nm = (const uni_str*)nbuf;
    if (nm->ptr && nm->len > 0 && nm->len <= 240) {
      unsigned m = 0;
      for (unsigned i = 0; i < nm->len / 2 && m + 1 < name_cap; ++i) {
        const wchar_t c = nm->ptr[i];
        name_buf[m++] = (c < 0x20 || c > 0x7e) ? '?' : (char)c;
      }
      name_buf[m] = 0;
    }
  }
  return true;
#else
  (void)h; (void)type_buf; (void)type_cap; (void)name_buf;
  (void)name_cap;
  return false;
#endif
}

// .233 DIAG: gap-dump sampler -- sample the main thread's RIP from
// the gap-dump path (bridge_kernel32.cpp, snapshot-only), NOT from
// the loop sampler thread. The loop thread fires ~10s late under
// watchdog starvation and .232 spent its budget on the first
// silence (audio init), so the second Sekiro death got no sample;
// and it never starts for Ultrapool at all (no fast waits, so
// spin_note_fast_wait never arms it). The gap dump runs on the
// watchdog tick and fires on every 2s-quiet final dump, so this is
// the path that can name the SECOND death (Sekiro) and the
// post-startup idle stall (Ultrapool, frame-1 tid). Diagnostic
// only: names the waited-on handle/thread when main is in
// NtWaitForSingleObject; never wakes, signals, or fakes anything
// (the .230 WAKE-MAIN stays loop-only). One sample per call;
// throttled to 6 per run, >=5s apart. The suspend window is
// microseconds (one GetThreadContext); logging + fn lookup happen
// AFTER ResumeThread, same contract as the loop sampler.
struct spin_ip_sample {
  u64 rip = 0;
  u64 rsp = 0;
  u64 rcx = 0;
  u64 rdx = 0;
  u64 r8 = 0;
  u64 r9 = 0;
  // .239 DIAG: callee-saved regs at capture -- R12 holds the
  // WorkerThreadPool group ID inside
  // wait_for_group_task_completion (2nd arg, saved at +0x1a,
  // still live at the +0x182 wait); RBX = [group+0x18] inner
  // struct (wait predicate counter at +0x28); R15 = the group
  // CV, R13 = its mutex. Logged by GAP-GROUP.
  u64 r12 = 0;
  u64 rbx = 0;
  u64 r13 = 0;
  u64 r15 = 0;
  u64 fn_off = 0;
  u64 fn_len = 0;
  u64 rip_off = 0;
  // .240 DIAG: first-image base captured by the fnlookup
  // step (inside its __try) -- the stack-gated GAP-GROUP
  // below turns stk[] return addresses into offsets with
  // it. 0 when the image table was gone at lookup time.
  u64 img_base = 0;
  u64 stkret = 0;
  u64 stkfn = 0;
  // .234 DIAG: 9 -> 24 qwords -- the guest caller of Sleep
  // (Sekiro) / WaitForSingleObjectEx (Ultrapool) sits deeper
  // than q8 (the .233 window showed only ntdll/kernel32).
  // .241 DIAG: 24 -> 48 qwords -- the .240 samples had
  // stkguest=0x0 both (main deep in ntdll/heap, no guest
  // addresses in 24 qwords); the wider window gives the
  // GAP-GROUP gate + stkguest more slots to find the guest
  // caller.
  // .242 DIAG: 48 -> 96 qwords -- the .241 Ultrapool
  // samples 9-12 (terminal 0x644 wait) show NO guest code
  // address in 48 qwords: the wait goes through host
  // frames and the guest return sits deeper than 384
  // bytes up the stack. The gate + stkguest search scan
  // the full window, so widening costs 48 more guarded
  // derefs per sample. GAP-IP-STACK still prints q0..q23
  // only (hardcoded 24 slots), so the log line is unchanged.
  u64 stk[96] = {};
  // .234 DIAG: first guest-image address in stk[] (the guest
  // caller) + the qword at rdx (Sleep DelayInterval / wait
  // timeout, read while the target is suspended).
  u64 stkguest = 0;
  u64 stkguest_off = 0;
  u64 rdx_qword = 0;
  int rdx_qword_ok = 0;
  unsigned stk_n = 0;
  unsigned long sus = 0;
  int ok = 0;
  // .241 DIAG: capture-step discriminator -- the sub-step
  // inside spin_capture_ip's CAPTURE __try where a fault
  // fired (1 = GetThreadContext, 2 = context copy,
  // 3 = stack read, 4 = rdx read, 5 = done). 0 = the capture
  // never entered the __try. Read by the GAP-CAPTURE-FAULT
  // logger (step=). The .240 fault carried no sub-step.
  int cap_step = 0;
  char ntfn[160] = {};
  u64 ntoff = 0;
};
// Capture one thread's IP + context (suspend / GetThreadContext /
// resume) + the enclosing guest .pdata fn + nearest host export +
// stack return address. No logging inside: callers log after the
// target resumes. Returns 1 on success, 0 when the thread could
// not be opened, -1 when it could not be suspended.
// .237 DIAG: one-line stage marker for spin_capture_ip --
// brackets the fault (see the .236 guard comment inside the
// function). Called after each step of the capture; the last
// marker seen before a HOST UNHANDLED names the step that
// faulted. The .236 fault fires after the 2nd GAP-CAPTURE-TRY
// with no GAP-IP-SAMPLE / GAP-CAPTURE-FAULT / capture-fail
// between TRY and fault, so it is NOT in the __try-guarded
// regions (the guard would return -2 and log FAULT) -- it is
// in an unguarded call (OpenThread / SuspendThread /
// ResumeThread / CloseHandle) or in the sampler's post-capture
// logging. Diagnostic only.
static void spin_capture_stage(u32 tid, const char* stage) {
  char sl[96];
  snprintf(sl, sizeof(sl), "sync: GAP-CAPTURE-STAGE tid=%u stage=%s",
           tid, stage);
  trt_core_log(sl);
}

// .238 FIX: buffer the in-suspension stage markers -- the .237
// markers logged via trt_core_log WHILE the target was
// suspended; if main was suspended holding the log lock
// (g_ring_mu), the watchdog's "suspend" marker blocked on it
// forever, freezing all logging and deadlocking the watchdog
// before ResumeThread (the .237 Sekiro run froze at
// stage=open, 1 heartbeat, launch wedge). Buffer "suspend" /
// "context" here, flush them AFTER ResumeThread returns (see
// spin_capture_ip). "open" / "resume" / "close" / "fnlookup"
// still log live -- the target is running at those points.
// Never trt_core_log while the target is suspended.
static void spin_capture_stage_buf(u32 tid, const char* stage,
                                   const char** buf, int* n) {
  (void)tid;
  if (*n < 2) buf[(*n)++] = stage;
}
static void spin_capture_stage_flush(u32 tid, const char** buf,
                                     int n) {
  for (int i = 0; i < n; ++i) spin_capture_stage(tid, buf[i]);
}

// .238 FIX: the .237 HOST UNHANDLED (0xc0000005,
// rip_rva=0xecbb4, spin_capture_ip+0x184) is THIS deref:
// `mov (%r15),%rax` with r15 = ctx.Rdx. The source wraps it
// in a nested __try, but the .237 binary's .xdata proves the
// guard is NOT real: spin_capture_ip's scope table has only
// 2 entries (outer __try +0xdd..+0x14b, fn-lookup __try
// +0x253..+0x2eb) while the deref sits at +0x184, OUTSIDE
// every scope -- the nested inner __try got no scope entry
// (MSVC moved the post-call code out of the outer try's
// region). So the AV propagated to the unhandled filter and
// terminated the process. This function reads the qword in
// its OWN frame whose entire body is ONE top-level __try --
// the pattern MSVC reliably scopes (both top-level __trys
// in spin_capture_ip got scopes). MSVC does not inline
// functions containing __try, so the guard stays in its own
// frame with its own scope table. ALSO: ctx.Rdx is often not
// a pointer at all (for a thread parked in
// NtWaitForSingleObject it is the alertable flag, 0/1) -- a
// fault here is EXPECTED, and the guarded read reports ok=0
// instead of killing the host. Verify in the built .238 exe's
// .xdata that this function's scope covers its deref (HOST
// SYMS read_qword=<rva> + /tmp/xdata238.py) before deploying.
// .241 FIX: the .238 guard above is NOT real in the .240
// binary. The .240 exe at RVA 0xf0a80 holds 11 straight-line
// bytes (mov rax,[rcx]; mov [rdx],rax; mov eax,1; ret) -- no
// handler, no .pdata entry: the direct deref inside the
// __try was compiled bare, so a fault escaped to the
// CAPTURE scope (same mechanism as spin_read_stack_qwords).
// The deref now goes through a __declspec(noinline) call
// INSIDE the __try -- the surviving spin_fnlookup_guarded
// shape (try body with calls keeps its scope: 1 scope,
// 49.0% in .240). Verify in the .241 exe's .xdata
// (xdata241.py) before deploying; if the scope is absent,
// do NOT deploy -- fall back to a ReadProcessMemory stack
// read instead.
// __declspec is MSVC-only; the Linux host compiles this file too (it builds
// the same diagnostics for the desktop backend), so the attribute is spelled
// per compiler rather than assumed.
#if defined(_WIN32)
#define TRT_THREAD_NOINLINE __declspec(noinline)
#else
#define TRT_THREAD_NOINLINE __attribute__((noinline))
#endif

static TRT_THREAD_NOINLINE u64 spin_deref_qword(u64 addr) {
  return *(const u64*)(uintptr_t)addr;
}

static TRT_THREAD_NOINLINE int spin_read_qword_guarded(u64 addr,
                                                      u64* out) {
#if defined(_WIN32)
  int ok = 0;
  __try {
    // CALL inside the __try (see the note above): a call in
    // the try body forces MSVC to emit the scope, and the
    // callee is noinline so the call survives optimization.
    // A fault in the callee unwinds to this scope.
    *out = spin_deref_qword(addr);
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

// .239 FIX: guard the FNLOOKUP step the same way as the rdx
// read (own function + top-level __try). The .238 Ultrapool
// run lost one capture here (stages open..close, then
// GAP-CAPTURE-FAULT, no fnlookup marker): the fault is in
// this step -- the fn-lookup __try inside spin_capture_ip
// (scope 1, +0x268..+0x300 in the .238 exe) covers the two
// calls but NOT the inlined stack loop (+0x302..+0x350,
// outside the scope), and the callees it covers do
// UNGUARDED reads of external memory: spin_host_nearest_
// export walks 8 modules' export tables with no __try
// (PE-offset read, export-dir reads, EAT reads, export-name
// string reads; ords[i] indexes funcs[] with no bounds
// check against NumberOfFunctions), and spin_fn_range
// binary-searches img->pdata with no __try. The .238 log
// records no fault RIP, so no single instruction can be
// proven from the log alone -- the strongest candidate is
// the nearest-export EAT walk, and the step discriminator
// below names the sub-step on the next fault instead of
// guessing. Moving the whole step into its own frame with
// one top-level __try covers every sub-step (including the
// out-of-scope stack loop) the same way the guarded rdx
// read does. step_out: 1 = fn_range(rip), 2 = nearest_
// export, 3 = fn_range(stk[0]), 4 = stack loop. Returns 1
// on success, 0 on fault (step_out set).
static int spin_fnlookup_guarded(spin_ip_sample* s, int* step_out) {
#if defined(_WIN32)
  if (step_out) *step_out = 0;
  if (!s || !s->rip) return 1;
  int ok = 0;
  __try {
    // .240 FIX: hoist the image base/end into locals HERE
    // (inside the guarded region) and NEVER re-read img
    // below. The .239 fault was the [img]/[img+0x10]
    // re-read in the step-4 loop: the loop sat OUTSIDE
    // the guard's single SEH scope (fn+0x100..+0x161 vs
    // scope fn+0x52..+0xff in the .239 exe), so the
    // dangling re-read faulted unguarded -- teardown race
    // on Sekiro (image unload between the in-scope
    // validation and the out-of-scope re-read),
    // image-table realloc race mid-run on Ultrapool.
    // Step 4 now touches only these locals + s->stk[]
    // (caller stack, always valid), so it cannot fault
    // even outside the scope.
    u64 img_b = 0, img_e = 0;
    if (step_out) *step_out = 1;
    spin_fn_range(s->rip, &s->fn_off, &s->fn_len);
    {
      const trt_image* img = trt_images_first();
      if (img && img->base && img->size_of_image) {
        img_b = (u64)(uintptr_t)img->base;
        img_e = img_b + img->size_of_image;
      }
    }
    s->img_base = img_b;
    if (img_b && s->rip >= img_b && s->rip < img_e)
      s->rip_off = s->rip - img_b;
    snprintf(s->ntfn, sizeof(s->ntfn), "none");
    if (step_out) *step_out = 2;
    spin_host_nearest_export(s->rip, s->ntfn, sizeof(s->ntfn), &s->ntoff);
    if (s->stk_n > 0) {
      if (step_out) *step_out = 3;
      u64 stk_len = 0;
      spin_fn_range(s->stk[0], &s->stkfn, &stk_len);
    }
    // .234 DIAG: first guest-image address in the (widened)
    // stack -- the guest caller of the host function main is
    // parked in (Sleep / WaitForSingleObjectEx). Same main
    // image range rip_off uses above.
    // .240 FIX: NO img re-read here -- see the step-1 note.
    // The loop uses only the step-1 locals (img_b/img_e) +
    // s->stk[] (caller stack), so it cannot fault even
    // though it sits outside the guard's SEH scope.
    if (step_out) *step_out = 4;
    if (img_b && img_e > img_b) {
      for (unsigned i = 0; i < s->stk_n; ++i) {
        if (s->stk[i] >= img_b && s->stk[i] < img_e) {
          s->stkguest = s->stk[i];
          s->stkguest_off = s->stk[i] - img_b;
          break;
        }
      }
    }
    ok = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  return ok;
#else
  (void)s;
  (void)step_out;
  return 1;
#endif
}

static int spin_capture_ip(u32 tid, spin_ip_sample* s) {
#if defined(_WIN32)
  if (!tid || !s) return 0;
  // .238 FIX: stage-marker buffer -- "suspend" / "context"
  // are logged AFTER ResumeThread (see the flush below), never
  // while the target is suspended (.237 Sekiro deadlock).
  const char* cap_stages[2];
  int cap_stages_n = 0;
  HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION,
                          FALSE, tid);
  if (!h) return 0;
  spin_capture_stage(tid, "open");
  const DWORD sus = ::SuspendThread(h);
  if (sus == (DWORD)-1) {
    ::CloseHandle(h);
    return -1;
  }
  spin_capture_stage_buf(tid, "suspend", cap_stages, &cap_stages_n);
  // .236 FIX: guard the capture -- both .235 Ultrapool runs crashed
  // on the watchdog thread immediately after a GAP-HOOK dump with
  // stid!=0, with no GAP-IP-SAMPLE line between the dump and the
  // crash, i.e. inside this capture. A faulting capture returns -2,
  // never kills the host; the target is ALWAYS resumed below,
  // fault or not. (The guard lives here, not in the gap sampler:
  // __try cannot live there -- C2712, destructible locals.)
  int cap_rc = 1;
  // .241 DIAG: capture-step discriminator (see the cap_step
  // field note): set before each sub-step inside the
  // CAPTURE __try, so a fault caught here names the step.
  s->cap_step = 0;
  __try {
  s->cap_step = 1;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  const BOOL ok = ::GetThreadContext(h, &ctx);
  s->ok = ok ? 1 : 0;
  s->sus = (unsigned long)sus;
  if (ok) {
    s->cap_step = 2;
    s->rip = (u64)ctx.Rip;
    s->rsp = (u64)ctx.Rsp;
    s->rcx = (u64)ctx.Rcx;
    s->rdx = (u64)ctx.Rdx;
    s->r8 = (u64)ctx.R8;
    s->r9 = (u64)ctx.R9;
    // .239 DIAG: callee-saved regs for the GAP-GROUP line
    // (see the struct comment): R12 = group ID at the wait
    // point, RBX = group inner struct, R15 = group CV,
    // R13 = its mutex.
    s->r12 = (u64)ctx.R12;
    s->rbx = (u64)ctx.Rbx;
    s->r13 = (u64)ctx.R13;
    s->r15 = (u64)ctx.R15;
    // .234 DIAG: 9 -> 24 qwords (see struct) + read the qword
    // at rdx (Sleep DelayInterval / wait timeout) while the
    // target is still suspended. Direct deref under __try
    // (the spin_read_stack_qwords pattern): ReadProcessMemory
    // is desktop-only, undeclared in the UWP SDK partition.
    // .241 DIAG: 24 -> 48 qwords (see the struct note).
    // .242 DIAG: 48 -> 96 qwords (see the struct note).
    s->cap_step = 3;
    s->stk_n = s->rsp ? spin_read_stack_qwords(s->rsp, s->stk, 96) : 0;
    s->rdx_qword = 0;
    s->rdx_qword_ok = 0;
    s->cap_step = 4;
    // .238 FIX: the inline nested __try around this deref had
    // NO scope entry in the .237 binary (fault at +0x184,
    // scopes end +0x14b) -- the AV killed the host. Read it
    // through spin_read_qword_guarded (own function, top-level
    // __try). ctx.Rdx is often not a pointer (alertable flag
    // 0/1 for a thread in NtWaitForSingleObject), so a fault
    // is expected and reports ok=0.
    if (ctx.Rdx) {
      s->rdx_qword_ok =
          spin_read_qword_guarded((u64)ctx.Rdx, &s->rdx_qword) ? 1 : 0;
      if (!s->rdx_qword_ok) s->rdx_qword = 0;
    }
    s->stkret = (s->stk_n > 0) ? s->stk[0] : 0;
    s->cap_step = 5;
  }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cap_rc = -2;
  }
  spin_capture_stage_buf(tid, "context", cap_stages, &cap_stages_n);
  ::ResumeThread(h);
  // .238 FIX: flush the buffered in-suspension markers NOW --
  // the target is running again and the log lock is free.
  spin_capture_stage_flush(tid, cap_stages, cap_stages_n);
  spin_capture_stage(tid, "resume");
  ::CloseHandle(h);
  spin_capture_stage(tid, "close");
  if (cap_rc == -2) return -2;
  // Fn lookup AFTER resume (see the header comment).
  // .239 FIX: the whole step runs in spin_fnlookup_guarded
  // (own function, top-level __try -- see its comment for
  // the .238 fault analysis). A fault there returns 0 with
  // the sub-step recorded; the caller logs it as
  // GAP-CAPTURE-FNFAULT and returns -2 exactly like the old
  // inline __try did, so the GAP-CAPTURE-FAULT contract is
  // unchanged. The "fnlookup" stage still logs (the step
  // ran, albeit faulted) -- the .238 gap was the missing
  // marker AND the missing sub-step, both fixed here.
  int fl_step = 0;
  const int fl_ok = spin_fnlookup_guarded(s, &fl_step);
  if (!fl_ok) {
    char fl[128];
    snprintf(fl, sizeof(fl),
             "sync: GAP-CAPTURE-FNFAULT tid=%u step=%d", tid, fl_step);
    trt_core_log(fl);
  }
  spin_capture_stage(tid, "fnlookup");
  return fl_ok ? 1 : -2;
#else
  (void)tid;
  (void)s;
  return 0;
#endif
}
static std::atomic<unsigned> g_gap_samples_done{0};
static std::atomic<unsigned long long> g_gap_last_sample_ms{0};
// .236 FIX: tid whose capture faulted (GAP-CAPTURE-FAULT) -- never
// capture it again (GAP-CAPTURE-SKIP). Set in the gap sampler.
static std::atomic<u32> g_gap_capture_fault_tid{0};
// .241 FIX: two-strike blacklist -- strike 1 only records
// (last-fault tid + count in the atomics below); strike 2
// (consecutive fault on the same tid) sets the blacklist
// above. A single transient capture fault no longer kills
// the tid forever (the .240 Ultrapool run: TRY stuck at 3
// vs .238's 171). A capture success resets the count.
static std::atomic<u32> g_gap_last_fault_tid{0};
static std::atomic<unsigned> g_gap_capture_fault_strikes{0};

// Requires g_obj_mu held. Refresh + read a guest thread's liveness:
// alive=true means the thread is still executing somewhere (guest code
// or an unbridged block); alive=false (or the object reclaimed away)
// means it exited. Mirrors the WIN32 handle poll wait_single_impl does.
static bool thread_alive_locked(u32 tid, u32* exit_code) {
  if (exit_code) *exit_code = 0;
  for (u32 i = 0; i < (u32)g_objects.size(); ++i) {
    sync_object& o = g_objects[i];
    if (o.kind != OBJ_THREAD || o.tid != tid) continue;
#if defined(_WIN32)
    if (!o.signalled && o.win_handle &&
        ::WaitForSingleObject(o.win_handle, 0) == WAIT_OBJECT_0) {
      o.signalled = true;
      o.alive.store(false, std::memory_order_release);
    }
#endif
    if (exit_code) *exit_code = o.exit_code;
    return o.alive.load(std::memory_order_acquire);
  }
  if (exit_code) *exit_code = 0xFFFFFFFFu;  // object gone: reclaimed after exit
  return false;
}

static void dump_spin_summary() {
  // Requires g_obj_mu held.
  for (unsigned i = 0; i < g_spin_tab_n; ++i) {
    const sync_object* o = object_for_locked(g_spin_tab[i].handle, nullptr);
    char line[320];
    snprintf(line, sizeof(line),
             "sync: SPIN-SUMMARY handle=%llx kind=%u fast=%llu wake=%llu after=%llu owner=%u lock=%d sets=%llu lastset=%u lastn=%llu lastret=0x%llx lastfn=0x%llx",
             (unsigned long long)g_spin_tab[i].handle,
             (unsigned)g_spin_tab[i].kind,
             (unsigned long long)g_spin_tab[i].fast,
             (unsigned long long)(g_wake_split_done ? g_spin_tab_at_wake[i] : 0ull),
             (unsigned long long)(g_spin_tab[i].fast - (g_wake_split_done ? g_spin_tab_at_wake[i] : 0ull)),
             o ? o->owner_tid : 0u, o ? o->lock_count : 0,
             o ? (unsigned long long)o->set_count : 0ull,
             o ? o->last_set_tid : 0u,
             (unsigned long long)g_spin_tab[i].last_n,
             (unsigned long long)g_spin_tab[i].last_ret_off,
             (unsigned long long)g_spin_tab[i].last_fn_off);
    trt_core_log(line);
    // .229 DIAG: grep-friendly SPIN-LAST line (the last fast
    // wait per handle -- the final loop iteration, previously
    // only visible as fields inside SPIN-SUMMARY, so a grep
    // for the literal SPIN-LAST missed in .228).
    {
      char last[192];
      snprintf(last, sizeof(last),
               "sync: SPIN-LAST handle=%llx kind=%u lastn=%llu lastret=0x%llx lastfn=0x%llx",
               (unsigned long long)g_spin_tab[i].handle,
               (unsigned)g_spin_tab[i].kind,
               (unsigned long long)g_spin_tab[i].last_n,
               (unsigned long long)g_spin_tab[i].last_ret_off,
               (unsigned long long)g_spin_tab[i].last_fn_off);
      trt_core_log(last);
    }
  }
}

static void dump_post_wake_summary(const char* tag) {
  // Requires g_obj_mu held. tag is POST-WAKE-FINAL (census).
  const auto now = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < g_woke_n; ++i) {
    const unsigned long long ms_ago =
        (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_woke[i].at).count();
    u32 exit_code = 0;
    const bool alive = thread_alive_locked(g_woke[i].tid, &exit_code);
    char line[256];
    snprintf(line, sizeof(line),
             "sync: %s tid=%u handle=%llx woke_ms_ago=%llu alive=%d exit=%u",
             tag, g_woke[i].tid, (unsigned long long)g_woke[i].handle,
             ms_ago, alive ? 1 : 0, exit_code);
    trt_core_log(line);
  }
}

static void maybe_log_post_wake() {
  // Requires g_obj_mu held. One live line per wake, from the WAIT-FAST
  // sample block (every 100th sample after the first 32) -- the census
  // carries the final state regardless.
  for (unsigned i = 0; i < g_woke_n; ++i) {
    if (g_woke[i].reported) continue;
    g_woke[i].reported = true;
    const auto now = std::chrono::steady_clock::now();
    const unsigned long long ms_ago =
        (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_woke[i].at).count();
    u32 exit_code = 0;
    const bool alive = thread_alive_locked(g_woke[i].tid, &exit_code);
    char line[256];
    snprintf(line, sizeof(line),
             "sync: POST-WAKE tid=%u handle=%llx woke_ms_ago=%llu alive=%d exit=%u",
             g_woke[i].tid, (unsigned long long)g_woke[i].handle,
             ms_ago, alive ? 1 : 0, exit_code);
    trt_core_log(line);
  }
}

static void dump_threads_summary() {
  // Requires g_obj_mu held. Every live guest thread object: the
  // exit-vs-stuck discriminator at dump time. A thread whose routine
  // returned (or that exited via any bridged path) shows alive=0;
  // a thread stuck in pure guest code (or an unbridged block) shows
  // alive=1 with no further bridged calls.
  unsigned shown = 0;
  for (u32 i = 0; i < (u32)g_objects.size() && shown < 16; ++i) {
    sync_object& o = g_objects[i];
    if (o.kind != OBJ_THREAD) continue;
#if defined(_WIN32)
    if (!o.signalled && o.win_handle &&
        ::WaitForSingleObject(o.win_handle, 0) == WAIT_OBJECT_0) {
      o.signalled = true;
      o.alive.store(false, std::memory_order_release);
    }
#endif
    ++shown;
    char line[192];
    snprintf(line, sizeof(line),
             "sync: THREAD tid=%u alive=%d exit=%u routine=%p",
             o.tid, o.alive.load(std::memory_order_acquire) ? 1 : 0,
             o.exit_code, o.start_routine);
    trt_core_log(line);
  }
}

u32 current_tid() {
  return trt_GetCurrentThreadId();
}

// Precondition: lk owns g_obj_mu. Drops lk while invoking guest APCs so
// callbacks can inspect handles or re-queue work without deadlocking.
bool drain_apcs_locked(std::unique_lock<std::mutex>& lk, u32 tid) {
  auto it = g_apc_queues.find(tid);
  if (it == g_apc_queues.end() || it->second.empty()) return false;
  std::deque<apc_item> batch;
  batch.swap(it->second);
  lk.unlock();
  for (const apc_item& item : batch) {
    if (!item.fn) continue;
    if (item.arity == 1) {
      trt_call_guest(item.fn, item.a0, 0, 0, 0);
    } else {
      trt_call_guest(item.fn, item.a0, item.a1, item.a2, 0);
    }
  }
  lk.lock();
  return true;
}

// Uncapped generation-safe Win32 TlsAlloc/TlsGetValue/TlsSetValue
std::mutex g_tls_mu;
std::vector<bool> g_tls_used;
std::vector<u32> g_tls_gen;
thread_local std::vector<std::pair<u32, void*>> t_tls_vals;

std::string utf16_to_utf8(const u16* w) {
  std::string out;
  if (!w) return out;
  while (*w) {
    u32 cp = *w++;
    if (cp >= 0xD800u && cp <= 0xDBFFu && *w >= 0xDC00u && *w <= 0xDFFFu) {
      cp = 0x10000u + (((cp - 0xD800u) << 10) | (*w++ - 0xDC00u));
    }
    if (cp < 0x80u) {
      out.push_back((char)cp);
    } else if (cp < 0x800u) {
      out.push_back((char)(0xC0u | (cp >> 6)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
      out.push_back((char)(0xE0u | (cp >> 12)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back((char)(0xF0u | (cp >> 18)));
      out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
  }
  return out;
}

std::vector<u16> ansi_to_utf16(const char* s) {
  std::vector<u16> out;
  if (!s) return out;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    out.push_back((u16)*p);
  }
  out.push_back(0);
  return out;
}

// Handle encoding: low 32 bits = 0x5B000000u | (idx & 0xFFFFFu),
// high 32 bits = generation (tolerates 0 if caller truncated to 32 bits).
u64 make_handle(u32 idx, u32 gen) {
  return ((u64)gen << 32) | (0x5B000000u | (idx & 0xFFFFFu));
}

sync_object* object_for_locked(u64 h, u32* out_idx = nullptr) {
  const u32 lo = (u32)h;
  if ((lo & 0xFFF00000u) != 0x5B000000u) return nullptr;
  const u32 idx = lo & 0xFFFFFu;
  if (idx >= g_objects.size()) return nullptr;
  sync_object* o = &g_objects[idx];
  if (o->kind == OBJ_NONE) return nullptr;
  const u32 gen = (u32)(h >> 32);
  if (gen != 0 && gen != o->generation) return nullptr;
  if (out_idx) *out_idx = idx;
  return o;
}

u64 alloc_object_locked(obj_kind kind) {
  u32 idx = 0;
  if (!g_free_indices.empty()) {
    idx = g_free_indices.back();
    g_free_indices.pop_back();
  } else {
    if (g_objects.size() >= 0xFFFFFu) return 0;
    idx = (u32)g_objects.size();
    g_objects.emplace_back();
  }
  sync_object& o = g_objects[idx];
  const u32 next_gen = (o.generation + 1u) ? (o.generation + 1u) : 1u;
  o.clear_payload();
  o.generation = next_gen;
  o.kind = kind;
  // .224 DIAG: creator is the calling thread at allocation.
  o.creator_tid = current_tid();
  o.handle_refs = 1;
  // .225 DIAG: gate-trace record of event creation (the start
  // events are born here, in the kernel32 CreateEventW wrapper).
  // Mutexes are not recorded: about 1070 of them would flood the
  // 64-entry ring; the census already proves the burst.
  const u64 new_handle = make_handle(idx, next_gen);
  if (kind == OBJ_EVENT)
    gate_ring_record(o.creator_tid, 1, new_handle, (u32)kind);
  return new_handle;
}

// Discharge a thread's stack VA charge exactly once. Called wherever the
// thread's alive flag flips to false (trampoline exit, ExitThread,
// TerminateThread) — the zeroed field makes repeat calls no-ops. Must be
// called with g_obj_mu held, like every other exit-path mutation.
void discharge_thread_stack_locked(sync_object* o) {
  if (!o || !o->stack_va_charge) return;
  trt_mem_budget_discharge_va(o->stack_va_charge);
  o->stack_va_charge = 0;
}

void maybe_reclaim_object_locked(u32 idx) {
  sync_object& o = g_objects[idx];
  if (o.kind == OBJ_NONE) return;
  if (o.handle_refs > 0 || o.thread_ref) return;

  if (!o.name.empty()) {
    auto it = g_named_objects.find(o.name);
    if (it != g_named_objects.end() &&
        ((u32)it->second & 0xFFFFFu) == idx) {
      g_named_objects.erase(it);
    }
  }
  if (o.kind == OBJ_IOCP) {
    for (auto it = g_iocp_assocs.begin(); it != g_iocp_assocs.end();) {
      if (((u32)it->second.iocp_handle & 0xFFFFFu) == idx) {
        it = g_iocp_assocs.erase(it);
      } else {
        ++it;
      }
    }
  }
#if defined(_WIN32)
  if (o.win_handle) {
    CloseHandle(o.win_handle);
    o.win_handle = nullptr;
  }
#else
  if (o.ack_pipe[0] >= 0) close(o.ack_pipe[0]);
  if (o.ack_pipe[1] >= 0) close(o.ack_pipe[1]);
  if (o.wake_pipe[0] >= 0) close(o.wake_pipe[0]);
  if (o.wake_pipe[1] >= 0) close(o.wake_pipe[1]);
#endif
  o.clear_payload();
  g_free_indices.push_back(idx);
}

#if !defined(_WIN32)
std::mutex g_suspend_op_mu;
std::atomic<sync_object*> g_active_suspend{nullptr};
std::once_flag g_sigusr2_once;

void write_pipe_retry(int fd, char c) {
  if (fd < 0) return;
  for (;;) {
    const ssize_t r = write(fd, &c, 1);
    if (r == 1) break;
    if (r < 0 && errno == EINTR) continue;
    break;
  }
}

bool read_pipe_retry(int fd) {
  if (fd < 0) return false;
  char b = 0;
  for (;;) {
    const ssize_t r = read(fd, &b, 1);
    if (r == 1) return true;
    if (r < 0 && errno == EINTR) continue;
    return false;
  }
}

void init_thread_pipes(sync_object* o) {
  if (!o || o->ack_pipe[0] >= 0) return;
  if (pipe(o->ack_pipe) != 0) o->ack_pipe[0] = o->ack_pipe[1] = -1;
  if (pipe(o->wake_pipe) != 0) o->wake_pipe[0] = o->wake_pipe[1] = -1;
}

void sigusr2_handler(int, siginfo_t*, void* uctx_raw) {
  sync_object* o = g_active_suspend.load(std::memory_order_acquire);
  if (!o) return;
#if defined(__linux__) && defined(__x86_64__)
  ucontext_t* uc = (ucontext_t*)uctx_raw;
  if (uc) {
    Win64Context& c = o->captured_ctx;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = k_context_control | k_context_integer |
                     k_context_segments | k_context_floating_point;
    c.Rax = (u64)uc->uc_mcontext.gregs[REG_RAX];
    c.Rcx = (u64)uc->uc_mcontext.gregs[REG_RCX];
    c.Rdx = (u64)uc->uc_mcontext.gregs[REG_RDX];
    c.Rbx = (u64)uc->uc_mcontext.gregs[REG_RBX];
    c.Rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
    c.Rbp = (u64)uc->uc_mcontext.gregs[REG_RBP];
    c.Rsi = (u64)uc->uc_mcontext.gregs[REG_RSI];
    c.Rdi = (u64)uc->uc_mcontext.gregs[REG_RDI];
    c.R8  = (u64)uc->uc_mcontext.gregs[REG_R8];
    c.R9  = (u64)uc->uc_mcontext.gregs[REG_R9];
    c.R10 = (u64)uc->uc_mcontext.gregs[REG_R10];
    c.R11 = (u64)uc->uc_mcontext.gregs[REG_R11];
    c.R12 = (u64)uc->uc_mcontext.gregs[REG_R12];
    c.R13 = (u64)uc->uc_mcontext.gregs[REG_R13];
    c.R14 = (u64)uc->uc_mcontext.gregs[REG_R14];
    c.R15 = (u64)uc->uc_mcontext.gregs[REG_R15];
    c.Rip = (u64)uc->uc_mcontext.gregs[REG_RIP];
    c.EFlags = (u32)uc->uc_mcontext.gregs[REG_EFL];
    c.SegCs = 0x33;
    c.SegDs = c.SegEs = c.SegSs = 0x2B;
    c.SegFs = 0x53;
    c.SegGs = 0x2B;
    if (uc->uc_mcontext.fpregs) {
      c.MxCsr = uc->uc_mcontext.fpregs->mxcsr;
      static_assert(sizeof(c.Xmm) == sizeof(uc->uc_mcontext.fpregs->_xmm),
                    "XMM save area size");
      memcpy(c.Xmm, uc->uc_mcontext.fpregs->_xmm, sizeof(c.Xmm));
    }
    o->has_captured_ctx.store(true, std::memory_order_release);
  }
#else
  (void)uctx_raw;
#endif

  o->in_sig_suspend.store(true, std::memory_order_release);
  write_pipe_retry(o->ack_pipe[1], 'S');

  while (o->suspend_count.load(std::memory_order_acquire) > 0 &&
         o->alive.load(std::memory_order_acquire)) {
    if (!read_pipe_retry(o->wake_pipe[0])) break;
  }
  o->in_sig_suspend.store(false, std::memory_order_release);

#if defined(__linux__) && defined(__x86_64__)
  if (uc && o->ctx_dirty.exchange(false, std::memory_order_acq_rel)) {
    const Win64Context& c = o->captured_ctx;
    const u32 cf = c.ContextFlags;
    if (cf & (k_context_integer & ~k_context_amd64)) {
      uc->uc_mcontext.gregs[REG_RAX] = (greg_t)c.Rax;
      uc->uc_mcontext.gregs[REG_RCX] = (greg_t)c.Rcx;
      uc->uc_mcontext.gregs[REG_RDX] = (greg_t)c.Rdx;
      uc->uc_mcontext.gregs[REG_RBX] = (greg_t)c.Rbx;
      uc->uc_mcontext.gregs[REG_RBP] = (greg_t)c.Rbp;
      uc->uc_mcontext.gregs[REG_RSI] = (greg_t)c.Rsi;
      uc->uc_mcontext.gregs[REG_RDI] = (greg_t)c.Rdi;
      uc->uc_mcontext.gregs[REG_R8]  = (greg_t)c.R8;
      uc->uc_mcontext.gregs[REG_R9]  = (greg_t)c.R9;
      uc->uc_mcontext.gregs[REG_R10] = (greg_t)c.R10;
      uc->uc_mcontext.gregs[REG_R11] = (greg_t)c.R11;
      uc->uc_mcontext.gregs[REG_R12] = (greg_t)c.R12;
      uc->uc_mcontext.gregs[REG_R13] = (greg_t)c.R13;
      uc->uc_mcontext.gregs[REG_R14] = (greg_t)c.R14;
      uc->uc_mcontext.gregs[REG_R15] = (greg_t)c.R15;
    }
    if (cf & (k_context_control & ~k_context_amd64)) {
      uc->uc_mcontext.gregs[REG_RSP] = (greg_t)c.Rsp;
      uc->uc_mcontext.gregs[REG_RIP] = (greg_t)c.Rip;
      uc->uc_mcontext.gregs[REG_EFL] = (greg_t)c.EFlags;
    }
    if ((cf & (k_context_floating_point & ~k_context_amd64)) &&
        uc->uc_mcontext.fpregs) {
      uc->uc_mcontext.fpregs->mxcsr = c.MxCsr;
      memcpy(uc->uc_mcontext.fpregs->_xmm, c.Xmm, sizeof(c.Xmm));
    }
  }
#endif
}

void ensure_sigusr2_installed() {
  std::call_once(g_sigusr2_once, []() {
    struct sigaction sa{};
    sa.sa_sigaction = &sigusr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
  });
}

void fill_initial_win64_ctx(sync_object* o, Win64Context* out) {
  memset(out, 0, sizeof(*out));
  out->ContextFlags = k_context_control | k_context_integer |
                      k_context_segments | k_context_floating_point;
  out->MxCsr = 0x1F80u;
  out->SegCs = 0x33;
  out->SegDs = out->SegEs = out->SegSs = 0x2B;
  out->SegFs = 0x53;
  out->SegGs = 0x2B;
  out->EFlags = 0x202u;
  out->Rip = (u64)(uintptr_t)o->start_routine;
  out->Rcx = o->start_arg;
  out->Rax = out->Rip;
}
#endif  // !_WIN32

struct thread_start {
  u64 object;
};

bool is_object_ready_locked(sync_object* o, u32 my_tid) {
  if (!o) return false;
  switch (o->kind) {
    case OBJ_EVENT:
    case OBJ_THREAD:
      return o->signalled;
    case OBJ_MUTEX:
      return o->lock_count == 0 || o->owner_tid == my_tid;
    case OBJ_SEMAPHORE:
      return o->lock_count > 0;
    default:
      return false;
  }
}

void consume_object_wait_locked(sync_object* o, u32 my_tid) {
  if (!o) return;
  if (o->kind == OBJ_EVENT && !o->manual_reset) {
    o->signalled = false;
  } else if (o->kind == OBJ_MUTEX) {
    ++o->lock_count;
    o->owner_tid = my_tid;
  } else if (o->kind == OBJ_SEMAPHORE && o->lock_count > 0) {
    --o->lock_count;
  }
}

void notify_thread_exit_once() {
  thread_local bool t_detached = false;
  if (t_detached) return;
  t_detached = true;
  trt_images_notify_thread(3 /*DLL_THREAD_DETACH*/);
  trt_fls_on_thread_exit();
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    g_apc_queues.erase(current_tid());
  }
}

#if defined(_WIN32)
// Worker-thread faults used to die silent: the __except handler returned
// the exception code as the thread exit code with no log line, while the
// main thread's guest_seh_filter (host/main.cpp) logs code/address/regs/
// stack. Mirror that format here so a worker crash is diagnosable from
// the log. Runs inside the SEH filter, only for faults the VEHs declined.
static void log_worker_fault(void* ep_v) {
  PEXCEPTION_POINTERS ep = (PEXCEPTION_POINTERS)ep_v;
  if (!ep || !ep->ExceptionRecord) return;
  const EXCEPTION_RECORD& record = *ep->ExceptionRecord;
  char where[192];
  const uintptr_t pc = (uintptr_t)record.ExceptionAddress;
  const char* module = "unknown";
  uintptr_t offset = pc;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery((void*)pc, &mbi, sizeof(mbi))) {
    offset = pc - (uintptr_t)mbi.AllocationBase;
    const char* guest = trt_images_name(mbi.AllocationBase);
    if (guest) module = guest;
  }
  snprintf(where, sizeof(where), "%s+0x%llx (pc=%p)", module,
           (unsigned long long)offset, (void*)pc);
  char line[320];
  // Whose fault is it: a guest-image pc is the title's, anything else is a
  // host-side address and must not be reported as a guest crash.
  const char* side = trt_fault_is_guest(record.ExceptionAddress) ? "guest" : "host";
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    snprintf(line, sizeof(line),
             "%s fault [worker tid=%u]: 0x%08lx at %s, %s address %p", side,
             current_tid(), (unsigned long)record.ExceptionCode, where,
             kind == 0 ? "read" : kind == 1 ? "write" : "execute",
             (void*)record.ExceptionInformation[1]);
  } else {
    snprintf(line, sizeof(line),
             "%s fault [worker tid=%u]: 0x%08lx at %s", side,
             current_tid(), (unsigned long)record.ExceptionCode, where);
  }
  trt_core_log(line);
  if (!ep->ContextRecord) return;
  const CONTEXT& c = *ep->ContextRecord;
  snprintf(line, sizeof(line),
           "guest fault [worker] regs: rip=%016llx rsp=%016llx rbp=%016llx rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
           (unsigned long long)c.Rip, (unsigned long long)c.Rsp,
           (unsigned long long)c.Rbp, (unsigned long long)c.Rax,
           (unsigned long long)c.Rbx, (unsigned long long)c.Rcx,
           (unsigned long long)c.Rdx);
  trt_core_log(line);
  snprintf(line, sizeof(line),
           "guest fault [worker] regs: rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx eflags=%08lx",
           (unsigned long long)c.Rsi, (unsigned long long)c.Rdi,
           (unsigned long long)c.R8, (unsigned long long)c.R9,
           (unsigned long long)c.R10, (unsigned long long)c.R11,
           (unsigned long long)c.R12, (unsigned long long)c.R13,
           (unsigned long long)c.R14, (unsigned long long)c.R15,
           (unsigned long)c.EFlags);
  trt_core_log(line);
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record.NumberParameters >= 2) {
    const void* target = reinterpret_cast<void*>(record.ExceptionInformation[1]);
    MEMORY_BASIC_INFORMATION data{};
    VirtualQuery(target, &data, sizeof(data));
    void** const active_tls = reinterpret_cast<void**>(__readgsqword(0x58));
    void** const guest_tls = trt_guest_tls_array();
    snprintf(line, sizeof(line),
             "guest fault [worker] memory: target=%p base=%p allocation=%p "
             "state=0x%lx protect=0x%lx type=0x%lx active_tls=%p guest_tls=%p",
             target, data.BaseAddress, data.AllocationBase,
             (unsigned long)data.State, (unsigned long)data.Protect,
             (unsigned long)data.Type, (void*)active_tls, (void*)guest_tls);
    trt_core_log(line);
  }
  // stack walk via .pdata unwind (same shape as the main-thread filter)
  CONTEXT ctx = c;
  int depth = 0;
  __try {
    while (ctx.Rip && depth < 32) {
      if (depth > 0) {
        const uintptr_t fpc = (uintptr_t)ctx.Rip;
        const char* fmod = "unknown";
        uintptr_t foff = fpc;
        MEMORY_BASIC_INFORMATION fmbi{};
        if (VirtualQuery((void*)fpc, &fmbi, sizeof(fmbi))) {
          foff = fpc - (uintptr_t)fmbi.AllocationBase;
          const char* fg = trt_images_name(fmbi.AllocationBase);
          if (fg) fmod = fg;
        }
        snprintf(line, sizeof(line), "guest fault [worker] caller %d: %s+0x%llx (pc=%p)",
                 depth, fmod, (unsigned long long)foff, (void*)fpc);
        trt_core_log(line);
      }
      const DWORD64 prev_rsp = ctx.Rsp;
      DWORD64 base = 0;
      if (const auto fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr)) {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx,
                         &handler_data, &establisher, nullptr);
      } else {
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += sizeof(DWORD64);
      }
      if (ctx.Rsp <= prev_rsp) break;
      ++depth;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::atomic<trt_guest_worker_fault_fn> g_guest_worker_fault_reporter{nullptr};

static int win_thread_seh_filter(void* ep) {
  if (trt_dispatch_win32_exception(ep) == -1) return EXCEPTION_CONTINUE_EXECUTION;
  const auto* exception = static_cast<const EXCEPTION_POINTERS*>(ep);
  const auto reporter = g_guest_worker_fault_reporter.load(std::memory_order_acquire);
  if (reporter && exception && exception->ExceptionRecord)
    reporter(exception->ExceptionRecord->ExceptionCode);
  log_worker_fault(ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

static u64 win_call_guest_seh(void* routine, u64 arg, bool* seh_out = nullptr,
                              bool notify_attach = false) {
  // .227 DIAG: seh_out reports whether the __except path fired,
  // so the trampoline can tell routine-return from an SEH fault.
  if (seh_out) *seh_out = false;
  __try {
    if (notify_attach) trt_images_notify_thread(2 /*DLL_THREAD_ATTACH*/);
    return trt_call_guest(routine, arg, 0, 0, 0);
  } __except (win_thread_seh_filter(GetExceptionInformation())) {
    if (seh_out) *seh_out = true;
    return (u64)(u32)GetExceptionCode();
  }
}

DWORD WINAPI win_thread_trampoline(LPVOID raw) {
  thread_start* ts = (thread_start*)raw;
  const u64 obj_h = ts->object;
  free(ts);

  const u32 my_tid = current_tid();
  void* routine = nullptr;
  u64 arg = 0;
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(obj_h);
    if (o) {
      if (!o->tid) o->tid = my_tid;
      o->started = true;
      routine = o->start_routine;
      arg = o->start_arg;
    }
  }

  {
    // .199 diagnostic: did the worker thread start? (First 32.)
    static unsigned s_tramp_logged = 0;
    if (s_tramp_logged < 32) {
      ++s_tramp_logged;
      char tl[192];
      snprintf(tl, sizeof(tl), "thread: win_thread_trampoline started tid=%u routine=0x%llx arg=0x%llx",
               my_tid, (unsigned long long)routine, (unsigned long long)arg);
      trt_core_log(tl);
    }
  }
  trt_guest_tls_sync_thread();
  {
    // .203 diagnostic: entry into protected guest startup. (First 32.)
    static unsigned s_enter_logged = 0;
    if (s_enter_logged < 32) {
      ++s_enter_logged;
      char el[192];
      snprintf(el, sizeof(el), "thread: win_thread_trampoline entering guest routine=0x%llx",
               (unsigned long long)routine);
      trt_core_log(el);
    }
  }
  bool seh_took = false;
  const u64 ret = win_call_guest_seh(routine, arg, &seh_took, true);
  // .227 DIAG: thread-exit path -- routine-return vs SEH fault,
  // with the exit code. The worker's fate in .226 was decided
  // at census (routine returned, exit 0); this line proves it
  // live, per thread, instead of at the census only.
  {
    char xl[192];
    snprintf(xl, sizeof(xl),
             "sync: THREAD-EXIT tid=%u routine=0x%llx exit=%u via=%s",
             my_tid, (unsigned long long)(uintptr_t)routine,
             (u32)ret, seh_took ? "seh" : "routine-return");
    trt_core_log(xl);
  }
  notify_thread_exit_once();

  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    u32 idx = 0;
    sync_object* o = object_for_locked(obj_h, &idx);
    if (o) {
      o->exit_code = (u32)ret;
      o->alive.store(false, std::memory_order_release);
      o->signalled = true;
      o->thread_ref = false;
      discharge_thread_stack_locked(o);
      g_sync_cv.notify_all();
      maybe_reclaim_object_locked(idx);
    }
  }
  return (DWORD)ret;
}
#else
void* thread_trampoline(void* raw) {
  thread_start* ts = (thread_start*)raw;
  const u64 obj_h = ts->object;
  free(ts);

  u32 my_tid = 0;
  void* routine = nullptr;
  u64 arg = 0;
  {
    std::unique_lock<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(obj_h);
    if (o) {
      o->pthread_handle = pthread_self();
      o->has_pthread = true;
      if (!o->tid) o->tid = trt_alloc_guest_thread_id();
      my_tid = o->tid;
      trt_bind_guest_thread_id(my_tid);
      while (o->suspend_count.load(std::memory_order_acquire) > 0 &&
             o->alive.load(std::memory_order_acquire)) {
        g_sync_cv.wait(lk);
      }
      o->started = true;
      routine = o->start_routine;
      arg = o->start_arg;
    }
  }
  if (!my_tid) my_tid = current_tid();

  // Host-tid provenance, logged by the CHILD (gettid here is the new
  // thread): maps guest tids to /proc and gdb LWP ids so a live sampler can
  // name which guest thread is burning CPU. Pairs with the CS-stall probe.
#if defined(__linux__)
  {
    // Addresses only: arg is guest memory and may be unmapped (measured: a
    // CREATE_SUSPENDED thread redirected by SetThreadContext carried a stale
    // value, and dereferencing it here killed the host with SIGSEGV).
    char hline[192];
    snprintf(hline, sizeof(hline),
             "thread: guest-tid=%u mapped to host_tid=%d routine=%p arg=%p", my_tid,
             (int)syscall(SYS_gettid), routine, (void*)arg);
    trt_core_log(hline);
  }
#endif

  trt_guest_tls_sync_thread();
  trt_images_notify_thread(2 /*DLL_THREAD_ATTACH*/);
  u64 ret = 0;
  if (routine) ret = trt_call_guest(routine, arg, 0, 0, 0);
  // .227 DIAG: thread-exit path (POSIX trampoline, see the
  // Windows trampoline above).
  {
    char xl[192];
    snprintf(xl, sizeof(xl),
             "sync: THREAD-EXIT tid=%u routine=0x%llx exit=%u via=%s",
             my_tid, (unsigned long long)(uintptr_t)routine,
             (u32)ret, "routine-return");
    trt_core_log(xl);
  }
  notify_thread_exit_once();

  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    u32 idx = 0;
    sync_object* o = object_for_locked(obj_h, &idx);
    if (o) {
      o->exit_code = (u32)ret;
      o->has_pthread = false;
      o->alive.store(false, std::memory_order_release);
      o->signalled = true;
      o->suspend_count.store(0, std::memory_order_release);
      write_pipe_retry(o->wake_pipe[1], 'X');
      o->thread_ref = false;
      discharge_thread_stack_locked(o);
      g_sync_cv.notify_all();
      maybe_reclaim_object_locked(idx);
    }
  }
  return (void*)(uintptr_t)ret;
}
#endif

} // namespace

extern "C" {

// ABI rule (see pe.h): every declaration of a TRT_STDCALL function must
// itself carry TRT_STDCALL.
u64 TRT_STDCALL trt_GetTickCount64(void);
void TRT_STDCALL trt_Sleep(u32 ms);
void TRT_STDCALL trt_SetLastError(u32 err);
u32 TRT_STDCALL trt_GetCurrentThreadId(void);
int TRT_STDCALL trt_SetEvent(u64 handle);

// Called by bridge_kernel32 ReadFile/WriteFile when OVERLAPPED != nullptr
void trt_iocp_notify_io(u64 file_handle, u32 bytes, void* overlapped) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  auto it = g_iocp_assocs.find(file_handle);
  if (it == g_iocp_assocs.end()) return;
  // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (bridge_kernel32.cpp stores the
  // per-handle modes): a successful overlapped I/O signals the OVERLAPPED
  // event but posts NO completion packet — posting one anyway handed
  // engines that set the flag phantom double-completions.
  if (trt_file_io_modes((void*)(uintptr_t)file_handle) & 0x1u) return;
  sync_object* port = object_for_locked(it->second.iocp_handle);
  if (!port || port->kind != OBJ_IOCP || port->iocp_closed) return;
  port->iocp_queue.push_back(
      iocp_packet{bytes, it->second.completion_key, overlapped});
  g_sync_cv.notify_all();
}

void trt_queue_io_apc(void* fn, u64 err, u64 bytes, void* overlapped) {
  if (!fn) return;
  const u32 my_tid = current_tid();
  std::lock_guard<std::mutex> lk(g_obj_mu);
  g_apc_queues[my_tid].push_back(
      apc_item{fn, 3, err, bytes, (u64)(uintptr_t)overlapped});
  g_sync_cv.notify_all();
}

u32 TRT_STDCALL trt_QueueUserAPC(void* fn, u64 thread, u64 data) {
  if (!fn) {
    trt_SetLastError(87);
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_obj_mu);
  u32 target_tid = 0;
  if (thread == (u64)(i64)-2) {
    target_tid = current_tid();
  } else {
    sync_object* o = object_for_locked(thread);
    if (!o || o->kind != OBJ_THREAD) {
      trt_SetLastError(6);
      return 0;
    }
    if (!o->alive.load(std::memory_order_acquire)) {
      trt_SetLastError(5);
      return 0;
    }
    target_tid = o->tid ? o->tid : 1u;
  }
  g_apc_queues[target_tid].push_back(apc_item{fn, 1, data, 0, 0});
  g_sync_cv.notify_all();
  return 1;
}

u32 TRT_STDCALL trt_QueueUserAPC2(void* fn, u64 thread, u64 data, u32 flags) {
  (void)flags;
  return trt_QueueUserAPC(fn, thread, data);
}

// CreateThread(attrs, stack, start, param, flags, tid)
u64 TRT_STDCALL trt_CreateThread(void* attrs, void* stack_size, void* routine,
                                 void* param, u32 flags, u32* tid) {
  (void)attrs;
  if (!routine) {
    trt_SetLastError(87);
    return 0;
  }
  u64 obj = 0;
  thread_start* ts = (thread_start*)malloc(sizeof(thread_start));
  if (!ts) {
    trt_SetLastError(8);
    return 0;
  }

  const bool suspended = (flags & 0x4u /*CREATE_SUSPENDED*/) != 0;
  const u32 child_tid = trt_alloc_guest_thread_id();
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    obj = alloc_object_locked(OBJ_THREAD);
    if (!obj) {
      free(ts);
      trt_SetLastError(8);
      return 0;
    }
    sync_object* o = object_for_locked(obj);
    o->alive.store(true, std::memory_order_release);
    o->thread_ref = true;
    o->tid = child_tid;
    o->suspend_count.store(suspended ? 1 : 0, std::memory_order_release);
    o->start_routine = routine;
    o->start_arg = (u64)(uintptr_t)param;
#if !defined(_WIN32)
    init_thread_pipes(o);
#endif
  }
  {
    // thread provenance: pairs with the CS-stall probe's owner_tid so a
    // wedge can be traced back to the routine that created the owner.
    char tline[128];
    snprintf(tline, sizeof(tline), "thread: CreateThread tid=%u routine=%p param=%p",
             child_tid, routine, param);
    trt_core_log(tline);
  }
  ts->object = obj;

  // the new thread's stack reserve is VA for the thread's lifetime (the
  // requested stack_size, else the 1 MB Windows default / 8 MB POSIX
  // pthread default): charge BEFORE creating the thread so a VA refusal
  // fails the creation instead of leaking an uncharged thread
#if defined(_WIN32)
  const u64 stack_va = stack_size ? (u64)(uintptr_t)stack_size
                                  : 1024ull * 1024ull;
#else
  const u64 stack_va = 8ull * 1024ull * 1024ull;  // pthread default stack
#endif
  if (trt_mem_budget_charge_va(stack_va) != 0) {
    free(ts);
    std::lock_guard<std::mutex> lk(g_obj_mu);
    u32 idx = 0;
    sync_object* o = object_for_locked(obj, &idx);
    if (o) {
      o->thread_ref = false;
      o->handle_refs = 0;
      maybe_reclaim_object_locked(idx);
    }
    return 0;  // 8 already set by the VA refusal
  }
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(obj);
    if (o) o->stack_va_charge = stack_va;
  }

#if defined(_WIN32)
  DWORD win_tid = 0;
  HANDLE t = ::CreateThread(nullptr, (SIZE_T)stack_size, &win_thread_trampoline,
                            ts, suspended ? CREATE_SUSPENDED : 0u, &win_tid);
  if (!t) {
    const DWORD err = GetLastError();
    free(ts);
    trt_mem_budget_discharge_va(stack_va);
    std::lock_guard<std::mutex> lk(g_obj_mu);
    u32 idx = 0;
    sync_object* o = object_for_locked(obj, &idx);
    if (o) {
      o->thread_ref = false;
      o->handle_refs = 0;
      o->stack_va_charge = 0;
      maybe_reclaim_object_locked(idx);
    }
    trt_SetLastError(err);
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(obj);
    if (o) {
      o->win_handle = t;
      o->tid = (u32)win_tid;
    }
  }
  {
    char tline[96];
    snprintf(tline, sizeof(tline), "thread: CreateThread win_tid=%u guest_tid=%u routine=%p",
             (u32)win_tid, child_tid, routine);
    trt_core_log(tline);
  }
  if (tid) *tid = (u32)win_tid;
#else
  (void)stack_size;
  ensure_sigusr2_installed();
  pthread_t th;
  if (pthread_create(&th, nullptr, &thread_trampoline, ts) != 0) {
    free(ts);
    trt_mem_budget_discharge_va(stack_va);
    std::lock_guard<std::mutex> lk(g_obj_mu);
    u32 idx = 0;
    sync_object* o = object_for_locked(obj, &idx);
    if (o) {
      o->thread_ref = false;
      o->handle_refs = 0;
      o->stack_va_charge = 0;
      maybe_reclaim_object_locked(idx);
    }
    trt_SetLastError(8);
    return 0;
  }
  pthread_detach(th);
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(obj);
    if (o) {
      o->pthread_handle = th;
      o->has_pthread = true;
    }
  }
  if (tid) *tid = child_tid;
#endif
  return obj;
}

u32 TRT_STDCALL trt_ExitThread(u32 code) {
  // .227 DIAG: UNCONDITIONAL -- the main thread has no
  // OBJ_THREAD, so a main ExitThread would otherwise be
  // invisible; this line is the main-exit witness if the
  // guest entry never returns. Logged before ExitThread.
  {
    char xl[128];
    snprintf(xl, sizeof(xl),
             "sync: THREAD-EXIT tid=%u code=%u via=ExitThread",
             current_tid(), code);
    trt_core_log(xl);
    // .228 DIAG: a worker ExitThread arms the main-IP
    // sampler's post-exit gate (the sampler fires only after
    // this + 200ms of main silence).
    g_spin_worker_exit_seen.store(true, std::memory_order_relaxed);
    // .231 DIAG: remember WHICH thread exited, for the
    // WAITED-THREAD comparison in the sampler.
    g_spin_worker_exit_tid.store(current_tid(),
                                 std::memory_order_relaxed);
  }
  notify_thread_exit_once();
  // .232 DIAG: exit-path localization -- if THREAD-EXIT
  // logged but this does not, the worker is stuck inside
  // notify_thread_exit_once (DLL_THREAD_DETACH handler,
  // FLS callback, or g_obj_mu). If this logs but
  // THREAD-EXIT-MARKED does not, the marking loop wedged.
  {
    char dl[128];
    snprintf(dl, sizeof(dl),
             "sync: THREAD-EXIT-DETACHED tid=%u code=%u",
             current_tid(), code);
    trt_core_log(dl);
  }
#if defined(_WIN32)
  {
    const u32 self_tid = current_tid();
    std::lock_guard<std::mutex> lk(g_obj_mu);
    for (u32 i = 0; i < (u32)g_objects.size(); ++i) {
      sync_object& o = g_objects[i];
      if (o.kind == OBJ_THREAD && o.tid == self_tid &&
          o.alive.load(std::memory_order_acquire)) {
        o.exit_code = code;
        o.alive.store(false, std::memory_order_release);
        o.signalled = true;
        o.suspend_count.store(0, std::memory_order_release);
        o.thread_ref = false;
        discharge_thread_stack_locked(&o);
        g_sync_cv.notify_all();
        maybe_reclaim_object_locked(i);
        break;
      }
    }
  }
  // .232 DIAG: exit-path localization -- the marking
  // loop ran (bridge object dead + signalled). If this
  // logs and the OS thread is still alive, the worker is
  // stuck in the real ExitThread (host detach teardown).
  {
    char ml[128];
    snprintf(ml, sizeof(ml),
             "sync: THREAD-EXIT-MARKED tid=%u code=%u",
             current_tid(), code);
    trt_core_log(ml);
  }
  ExitThread(code);
#else
  {
    const pthread_t self = pthread_self();
    std::lock_guard<std::mutex> lk(g_obj_mu);
    for (u32 i = 0; i < g_objects.size(); ++i) {
      sync_object& o = g_objects[i];
      if (o.kind == OBJ_THREAD && o.has_pthread &&
          o.alive.load(std::memory_order_acquire) &&
          pthread_equal(o.pthread_handle, self)) {
        o.exit_code = code;
        o.has_pthread = false;
        o.alive.store(false, std::memory_order_release);
        o.signalled = true;
        o.suspend_count.store(0, std::memory_order_release);
        write_pipe_retry(o.wake_pipe[1], 'X');
        o.thread_ref = false;
        discharge_thread_stack_locked(&o);
        g_sync_cv.notify_all();
        maybe_reclaim_object_locked(i);
        break;
      }
    }
  }
  // .232 DIAG: exit-path localization, POSIX branch --
  // see the Win32 MARKED probe above.
  {
    char ml[128];
    snprintf(ml, sizeof(ml),
             "sync: THREAD-EXIT-MARKED tid=%u code=%u",
             current_tid(), code);
    trt_core_log(ml);
  }
  pthread_exit((void*)(uintptr_t)code);
#endif
  return code;
}

int TRT_STDCALL trt_TerminateThread(u64 thread, u32 code) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) return 0;
  // .227 DIAG: forced thread death, with target + caller tids.
  {
    char xl[160];
    snprintf(xl, sizeof(xl),
             "sync: THREAD-EXIT target_tid=%u code=%u via=TerminateThread caller_tid=%u",
             o->tid, code, current_tid());
    trt_core_log(xl);
  }
  o->alive.store(false, std::memory_order_release);
  o->exit_code = code;  // the terminated thread's exit code IS the code
  o->signalled = true;
  o->suspend_count.store(0, std::memory_order_release);
  discharge_thread_stack_locked(o);  // the stack is gone with the thread
#if defined(_WIN32)
  if (o->win_handle) {
    // actually terminate the OS thread (was: mark dead + close the handle,
    // leaving the thread itself running)
    ::TerminateThread(o->win_handle, (DWORD)code);
    CloseHandle(o->win_handle);
    o->win_handle = nullptr;
  }
#else
  // pthread_cancel is the POSIX analogue (deferred: the thread dies at its
  // next cancellation point); the wake-pipe poke covers threads parked in
  // our own waits
  if (o->has_pthread) pthread_cancel(o->pthread_handle);
  write_pipe_retry(o->wake_pipe[1], 'T');
#endif
  g_sync_cv.notify_all();
  return 1;
}

static u32 wait_single_impl(u64 handle, u32 ms, bool alertable, void* guest_ret) {
  if (handle == (u64)(i64)-2 || handle == (u64)(i64)-1) {
    if (alertable) {
      std::unique_lock<std::mutex> lk(g_obj_mu);
      if (drain_apcs_locked(lk, current_tid())) return 0xC0u;
    }
    return 0x102u;
  }

  const u32 my_tid = current_tid();
#if defined(_WIN32)
  if (!alertable) {
    HANDLE win_thread_h = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_obj_mu);
      sync_object* o = object_for_locked(handle);
      if (!o) {
        trt_SetLastError(6);
        return 0xFFFFFFFFu;
      }
      if (o->kind == OBJ_THREAD && o->win_handle) {
        win_thread_h = o->win_handle;
      }
    }
    if (win_thread_h) {
      // .232 FIX: a bridge-dead thread whose OS thread
      // never terminated (.231: Sekiro worker called
      // ExitThread, the bridge marked it dead +
      // signalled, but the OS thread is stuck in exit
      // teardown, so the host handle never signals and
      // a plain host wait hangs forever). The bridge
      // object is the guest source of truth: marking
      // happens only on the exit paths, after which the
      // guest thread never runs guest code again, so
      // the wait is satisfied here. Slice-poll the host
      // handle (50ms) and re-check the bridge object
      // each slice, so a wait entered BEFORE the worker
      // marks dead is released too. Thread waits are
      // rare (not the event spin), so the poll costs
      // nothing measurable. No TerminateThread: the
      // stuck OS thread is left alone; only main's wait
      // is released.
      const bool infinite = (ms == 0xFFFFFFFFu);
      const auto deadline =
          infinite ? std::chrono::steady_clock::time_point::max()
                   : std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(ms);
      for (;;) {
        {
          std::lock_guard<std::mutex> lk(g_obj_mu);
          sync_object* o2 = object_for_locked(handle);
          if (o2 && o2->kind == OBJ_THREAD &&
              !o2->alive.load(std::memory_order_acquire)) {
            char dl[192];
            snprintf(dl, sizeof(dl),
                     "sync: WAIT-THREAD-DEAD handle=%llx tid=%u exit=%u",
                     (unsigned long long)handle, o2->tid,
                     o2->exit_code);
            trt_core_log(dl);
            return 0;
          }
        }
        DWORD slice_ms = 50;
        if (!infinite) {
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()).count();
          slice_ms = static_cast<DWORD>(remaining <= 0 ? 0 : remaining < 50 ? remaining : 50);
        }
        const DWORD result = ::WaitForSingleObject(win_thread_h, slice_ms);
        if (result == WAIT_FAILED) trt_SetLastError(::GetLastError());
        if (result != WAIT_TIMEOUT || slice_ms == 0) return result;
      }
    }
  }
#endif

  std::unique_lock<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o) {
    trt_SetLastError(6);
    return 0xFFFFFFFFu;
  }
#if defined(_WIN32)
  if (o->kind == OBJ_THREAD && !o->signalled && o->win_handle &&
      ::WaitForSingleObject(o->win_handle, 0) == WAIT_OBJECT_0) {
    o->signalled = true;
    o->alive.store(false, std::memory_order_release);
  }
#endif
  if (is_object_ready_locked(o, my_tid)) {
    consume_object_wait_locked(o, my_tid);
    // .222 DIAG: immediate-return wait -- the Sekiro main
    // thread (tid=872) spins on its own auto-reset event
    // here and never reaches the .207 log or the WAIT-PARK
    // log below, so the spin handle was only ever inferred.
    // Sample the handle + return value (first 32 + every
    // 100000th: a tight spin fires millions of waits/s, so
    // the sample must be sparse).
    {
      static unsigned long long s_fast_n = 0;
      ++s_fast_n;
      // .228 DIAG: hoist the ret/fn computation out of the
      // sampling if (below) so the census SPIN-LAST + the
      // main-IP sampler see EVERY fast wait, not just the
      // sampled ones.
      u64 ret_off = 0;
      const u64 retaddr =
          ret_in_guest_image((u64)(uintptr_t)guest_ret)
              ? (u64)(uintptr_t)guest_ret
              : 0;
      if (retaddr) {
        const trt_image* first_img = trt_images_first();
        if (first_img && first_img->base && first_img->size_of_image) {
          const u64 b = (u64)(uintptr_t)first_img->base;
          if (retaddr >= b && retaddr < b + first_img->size_of_image)
            ret_off = retaddr - b;
        }
      }
      u64 fn_off = 0, fn_len = 0;
      spin_fn_range(retaddr, &fn_off, &fn_len);
      // .226 DIAG: count every fast wait per handle (census
      // SPIN-SUMMARY), not just the sampled ones.
      spin_tab_count(handle, (unsigned)o->kind, ret_off, fn_off,
                     s_fast_n);
      // .228 DIAG: arm the main-IP sampler (see
      // spin_main_ip_sampler): record the spinning tid +
      // the last-fast-wait timestamp on EVERY fast wait;
      // the sampler fires only after this tid goes silent
      // post-worker-exit.
      spin_note_fast_wait(my_tid);
      if (s_fast_n <= 32 || (s_fast_n % 100) == 0) {
        // .225 DIAG: spin return address (ret_off/fn_off now
        // hoisted above spin_tab_count by .228, so the census
        // sees every fast wait, not just the sampled ones).
        // .226 DIAG: enclosing .pdata function of the spin call
        // site (runtime data only) + the wait timeout, so an
        // INFINITE always-ready wait reads differently from a
        // 0-timeout poll.
        char fl[384];
        snprintf(fl, sizeof(fl),
                 "sync: WAIT-FAST handle=%llx kind=%u tid=%u ret=0 ms=%u lastset=%u sets=%llu n=%llu owner=%u lock=%d retaddr=%llx ret_off=%llx fn_off=%llx fn_len=%llx",
                 (unsigned long long)handle, (unsigned)o->kind, my_tid, ms,
                 o->last_set_tid, (unsigned long long)o->set_count,
                 s_fast_n, o->owner_tid, o->lock_count,
                 (unsigned long long)retaddr, (unsigned long long)ret_off,
                 (unsigned long long)fn_off, (unsigned long long)fn_len);
        trt_core_log(fl);
        // .225 DIAG: lost-wakeup pairing, first sampled spin point
        // (retries at every 100th sample -- .226: the 100000th
        // never fires inside a 506-call Sekiro session).
        maybe_log_lost_wakeup(my_tid);
        // .226 DIAG: live post-wake line once per woken worker.
        maybe_log_post_wake();
      }
    }
    return 0;
  }
  if (alertable && drain_apcs_locked(lk, my_tid)) return 0xC0u;
  if (ms == 0) {
    // .222 DIAG: zero-timeout poll -- same sparse sample as
    // WAIT-FAST, so a poll-spin (ret 0x102) is
    // distinguishable from a signalled-event spin (ret 0).
    static unsigned long long s_poll_n = 0;
    ++s_poll_n;
    if (s_poll_n <= 32 || (s_poll_n % 100000) == 0) {
      char pl[192];
      snprintf(pl, sizeof(pl),
               "sync: WAIT-POLL handle=%llx kind=%u tid=%u ret=0x102 n=%llu",
               (unsigned long long)handle, (unsigned)o->kind, my_tid,
               s_poll_n);
      trt_core_log(pl);
    }
    return 0x102u;
  }

  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  {
    // .207 diagnostic: log the first WaitForSingleObject waits (first 64)
    // so a worker parked on an Event/Mutex/Semaphore is visible. The wait
    // parks on g_sync_cv, not a lock word, so .206 never saw it. kind:
    // 1=EVENT 2=MUTEX 3=SEMAPHORE 4=THREAD 5=IOCP. ms=4294967295=INFINITE.
    static unsigned s_wait_single_logged = 0;
    if (s_wait_single_logged < 64) {
      ++s_wait_single_logged;
      char wl[192];
      snprintf(wl, sizeof(wl),
               "sync: WaitForSingleObject handle=%llx kind=%u ms=%u",
               (unsigned long long)handle, (unsigned)o->kind, ms);
      trt_core_log(wl);
    }
  }
  // .221 DIAG: ping-pong partner -- record this park (INFINITE
  // waits only; the terminal Sekiro wait is one) and log the
  // park with the event's last setter, so the partner is named
  // even if the thread never wakes. The RAII guard clears the
  // entry on every exit path.
  parked_wait_guard park_guard(my_tid, handle, (u32)o->kind, 1);
  // .225 DIAG: watch an INFINITE park on a never-set event -- the
  // worker start signal. The creator thread is the one that must
  // set it; the gate trace records what that thread does instead.
  if (ms == 0xFFFFFFFFu && o->kind == OBJ_EVENT && o->set_count == 0) {
    watched_add(handle, o->creator_tid, my_tid);
    gate_ring_record(my_tid, 3, handle, (u32)o->kind);
  }
  if (ms == 0xFFFFFFFFu) {
    static unsigned s_park_logged = 0;
    ++s_park_logged;
    if (s_park_logged <= 128 || (s_park_logged % 128) == 0) {
      char pl[256];
      snprintf(pl, sizeof(pl),
               "sync: WAIT-PARK handle=%llx kind=%u tid=%u ms=%u lastset=%u sets=%llu",
               (unsigned long long)handle, (unsigned)o->kind, my_tid, ms,
               o->last_set_tid, (unsigned long long)o->set_count);
      trt_core_log(pl);
    }
  }
  for (;;) {
    if (ms == 0xFFFFFFFFu) {
      g_sync_cv.wait(lk);
    } else {
      g_sync_cv.wait_until(lk, deadline);
    }
    if (alertable && drain_apcs_locked(lk, my_tid)) return 0xC0u;
    o = object_for_locked(handle);
    if (!o) {
      trt_SetLastError(6);
      return 0xFFFFFFFFu;
    }
#if defined(_WIN32)
    if (o->kind == OBJ_THREAD && !o->signalled && o->win_handle &&
        ::WaitForSingleObject(o->win_handle, 0) == WAIT_OBJECT_0) {
      o->signalled = true;
      o->alive.store(false, std::memory_order_release);
    }
#endif
    if (is_object_ready_locked(o, my_tid)) {
      consume_object_wait_locked(o, my_tid);
      // .221 DIAG: the wait woke -- log who set the event, so a
      // park/wake pair names the partner even when the gap ring
      // only shows the wait side. Same cap shape as WAIT-PARK.
      if (ms == 0xFFFFFFFFu) {
        static unsigned s_woke_logged = 0;
        ++s_woke_logged;
        if (s_woke_logged <= 128 || (s_woke_logged % 128) == 0) {
          char wl[256];
          snprintf(wl, sizeof(wl),
                   "sync: WAIT-WOKE handle=%llx kind=%u tid=%u lastset=%u sets=%llu",
                   (unsigned long long)handle, (unsigned)o->kind, my_tid,
                   o->last_set_tid, (unsigned long long)o->set_count);
          trt_core_log(wl);
        }
        // .226 DIAG: post-wake watch -- record every wake of a
        // watched start event (not just the logged ones) so the
        // census can report the worker's fate.
        if (watched_find(handle)) woke_record(my_tid, handle);
      }
      return 0;
    }
    if (ms != 0xFFFFFFFFu && std::chrono::steady_clock::now() >= deadline) {
      return 0x102u;
    }
  }
}

static u32 wait_multiple_impl(u32 count, const u64* handles, bool wait_all, u32 ms,
                              bool alertable) {
  if (count == 0 || count > 64 || !handles) {
    trt_SetLastError(87);
    return 0xFFFFFFFFu;
  }
  std::vector<u64> hs(handles, handles + count);
  const u32 my_tid = current_tid();

  std::unique_lock<std::mutex> lk(g_obj_mu);
  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  {
    // .207 diagnostic: log the first WaitForMultipleObjects waits (first
    // 64). Same g_sync_cv park as the single-object wait.
    static unsigned s_wait_multi_logged = 0;
    if (s_wait_multi_logged < 64) {
      ++s_wait_multi_logged;
      char wl[256];
      sync_object* o0 = object_for_locked(hs[0]);
      snprintf(wl, sizeof(wl),
               "sync: WaitForMultipleObjects count=%u wait_all=%u ms=%u first_handle=%llx first_kind=%u",
               count, wait_all ? 1u : 0u, ms,
               (unsigned long long)hs[0], o0 ? (unsigned)o0->kind : 99u);
      trt_core_log(wl);
    }
  }
  // .221 DIAG: same parked-wait record as wait_single_impl
  // (first handle only; the full set is in the WAIT log above).
  // Function scope on purpose: the guard must live until the
  // wait returns, so it is NOT wrapped in a bare block.
  sync_object* o_first = object_for_locked(hs[0]);
  parked_wait_guard park_guard_multi(my_tid, hs[0],
      o_first ? (u32)o_first->kind : 99u, count);
  for (;;) {
    u32 signalled = 0;
    u32 first_idx = 0xFFFFFFFFu;
    for (u32 i = 0; i < count; ++i) {
      sync_object* o = object_for_locked(hs[i]);
      if (!o) {
        trt_SetLastError(6);
        return 0xFFFFFFFFu;
      }
#if defined(_WIN32)
      if (o->kind == OBJ_THREAD && !o->signalled && o->win_handle) {
        if (::WaitForSingleObject(o->win_handle, 0) == WAIT_OBJECT_0) {
          o->signalled = true;
          o->alive.store(false, std::memory_order_release);
        }
      }
#endif
      if (is_object_ready_locked(o, my_tid)) {
        ++signalled;
        if (first_idx == 0xFFFFFFFFu) first_idx = i;
      }
    }
    if (wait_all ? (signalled == count) : (signalled > 0)) {
      const u32 lo = wait_all ? 0u : first_idx;
      const u32 hi = wait_all ? count : (first_idx + 1u);
      for (u32 i = lo; i < hi; ++i) {
        consume_object_wait_locked(object_for_locked(hs[i]), my_tid);
      }
      return wait_all ? 0u : first_idx;
    }
    if (alertable && drain_apcs_locked(lk, my_tid)) return 0xC0u;
    if (ms == 0) return 0x102u;
    if (ms == 0xFFFFFFFFu) {
      g_sync_cv.wait(lk);
    } else {
      g_sync_cv.wait_until(lk, deadline);
      if (std::chrono::steady_clock::now() >= deadline) {
        if (alertable && drain_apcs_locked(lk, my_tid)) return 0xC0u;
        u32 fin_sig = 0;
        u32 fin_idx = 0xFFFFFFFFu;
        for (u32 i = 0; i < count; ++i) {
          sync_object* o = object_for_locked(hs[i]);
          if (o && is_object_ready_locked(o, my_tid)) {
            ++fin_sig;
            if (fin_idx == 0xFFFFFFFFu) fin_idx = i;
          }
        }
        if (wait_all ? (fin_sig == count) : (fin_sig > 0)) {
          const u32 lo = wait_all ? 0u : fin_idx;
          const u32 hi = wait_all ? count : (fin_idx + 1u);
          for (u32 i = lo; i < hi; ++i) {
            consume_object_wait_locked(object_for_locked(hs[i]), my_tid);
          }
          return wait_all ? 0u : fin_idx;
        }
        return 0x102u;
      }
    }
  }
}

// WaitForSingleObject(handle, ms): 0 signalled, 0x102 timeout, 0xFFFFFFFF failed
static std::atomic<u32> g_launch_wait_completed{0}, g_launch_wait_timed_out{0};
static std::atomic<u32> g_launch_wait_failed{0}, g_launch_event_signaled{0};
static std::atomic<u32> g_launch_event_failed{0};

void trt_sync_reset_launch_stats(void) {
  g_launch_wait_completed.store(0, std::memory_order_relaxed);
  g_launch_wait_timed_out.store(0, std::memory_order_relaxed);
  g_launch_wait_failed.store(0, std::memory_order_relaxed);
  g_launch_event_signaled.store(0, std::memory_order_relaxed);
  g_launch_event_failed.store(0, std::memory_order_relaxed);
}

void trt_sync_get_launch_stats(trt_sync_launch_stats* stats) {
  if (!stats) return;
  *stats = {g_launch_wait_completed.load(std::memory_order_relaxed),
            g_launch_wait_timed_out.load(std::memory_order_relaxed),
            g_launch_wait_failed.load(std::memory_order_relaxed),
            g_launch_event_signaled.load(std::memory_order_relaxed),
            g_launch_event_failed.load(std::memory_order_relaxed)};
}

extern "C" void trt_set_guest_worker_fault_reporter(trt_guest_worker_fault_fn reporter) {
#ifdef _WIN32
  g_guest_worker_fault_reporter.store(reporter, std::memory_order_release);
#else
  (void)reporter;
#endif
}

u32 TRT_STDCALL trt_WaitForSingleObject(u64 handle, u32 ms) {
  gap_trace_record("WaitForSingleObject");
  const u32 result = wait_single_impl(handle, ms, false, __builtin_return_address(0));
  switch (result) {
    case 0x102u: g_launch_wait_timed_out.fetch_add(1, std::memory_order_relaxed); break;
    case 0xFFFFFFFFu: g_launch_wait_failed.fetch_add(1, std::memory_order_relaxed); break;
    default: g_launch_wait_completed.fetch_add(1, std::memory_order_relaxed); break;
  }
  return result;
}

// .221 DIAG: log the parked-wait table (called from the gap final
// dump in bridge_kernel32.cpp). One line per parked thread: the
// waiter tid, the handle + kind it waits on, how long it has
// been parked, and the last thread to SetEvent that handle +
// how many times it was set. If the terminal Sekiro wait is
// here with sets=0, nothing ever sets the event; if sets>0 but
// the last setter is dead, the partner stopped signalling.
// .233 FIX (link): these two entry points MUST live outside the
// anonymous namespace (closes ~line 2052): anything inside it has
// internal linkage, so bridge_kernel32.cpp could not link against
// them (lld: undefined symbol spin_sample_main_ip_gap). Both are
// extern "C", matching the declarations in bridge_kernel32.cpp.
// They still use the namespace-internal helpers (spin_capture_ip,
// spin_ip_sample, g_gap_*, g_spin_main_tid) -- visible here via
// the implicit using-directive, the same pattern
// trt_sync_parked_log uses below.

// .244 DIAG: last bridge-issued D3D11 call (see vk_backend.hpp
// g_d3d11c). The bridge's D3D11 calls are the only D3D11 entry
// points the Vulkan guest reaches (Godot never calls D3D11
// directly -- D3D11CreateDevice count is 0 in the .243 Ultrapool
// log), so the last call + its age ties the terminal d3d11on12
// wait to the bridge's most recent D3D11 work. The name is a
// string literal (atomic pointer store -- no torn reads); the
// tick is GetTickCount64 ms. The sampler reads both without any
// lock. Outside the anonymous namespace + extern "C" (cross-TU,
// .233 lesson: cross-TU functions must live outside the
// anonymous namespace).
static std::atomic<const char*> g_d3d11_last_name{nullptr};
static std::atomic<unsigned long long> g_d3d11_last_tick_ms{0};
extern "C" void d3d11_note_last(const char* name) {
  g_d3d11_last_name.store(name, std::memory_order_relaxed);
  // The bridge's own implementation, declared above; the bare Win32 name
  // does not exist on the Linux host.
  g_d3d11_last_tick_ms.store((unsigned long long)trt_GetTickCount64(),
                             std::memory_order_relaxed);
}
extern "C" void d3d11_last_call(const char** name_out,
                                unsigned long long* tick_ms_out) {
  if (name_out)
    *name_out = g_d3d11_last_name.load(std::memory_order_relaxed);
  if (tick_ms_out)
    *tick_ms_out = g_d3d11_last_tick_ms.load(std::memory_order_relaxed);
}

extern "C" void spin_sample_main_ip_gap(u32 tid, int is_final) {
#if defined(_WIN32)
  if (!tid) return;
  // .239 DIAG: budget 6 -> 12, throttle 5s -> 2s. The .238
  // run got only 4 samples in 180s because the fnlookup
  // fault blacklisted the tid after capture #5; with the
  // guard (a) captures stop faulting, so the budget can
  // actually be spent on the main-cycle picture.
  if (g_gap_samples_done.load(std::memory_order_relaxed) >= 12) return;
  if (spin_now_ms() <
      g_gap_last_sample_ms.load(std::memory_order_relaxed) + 2000)
    return;
  // .236 FIX: log the capture attempt + guard its outcome -- the
  // .235 crash pattern is dump-with-stid!=0 then HOST UNHANDLED
  // with no sample line between, i.e. inside the capture. A
  // faulted tid is never captured again.
  // .239: check the blacklist BEFORE logging TRY -- the .238
  // run logged 166 TRY+SKIP pairs for the faulted tid (noise);
  // the FAULT line already records the blacklist, so a
  // blacklisted tid now logs nothing at all.
  if (tid == g_gap_capture_fault_tid.load(std::memory_order_relaxed)) {
    return;
  }
  {
    char tl[128];
    snprintf(tl, sizeof(tl), "sync: GAP-CAPTURE-TRY tid=%u done=%u",
             tid,
             g_gap_samples_done.load(std::memory_order_relaxed));
    trt_core_log(tl);
  }
  // .237 DIAG: HOST SYMS, once -- the RVAs of the capture-path
  // functions, so a faulting fn RVA from trt_host_fn_range
  // (HOST FN line, snapshot-only main delta) can be matched to
  // a name offline with no symbols and no game-file inspection.
  // Logged before the first capture, so it survives even if
  // the first capture faults. RVAs (addr - exe base), so they
  // match the HOST FN RVA across runs of the same build.
  {
    static std::atomic<unsigned> s_syms_logged{0};
    if (s_syms_logged.exchange(1, std::memory_order_relaxed) == 0) {
      const unsigned long long eb =
          (unsigned long long)(uintptr_t)::GetModuleHandleW(nullptr);
      char yl[512];
      snprintf(yl, sizeof(yl),
               "sync: HOST SYMS capture_ip=0x%llx sample_gap=0x%llx "
               "read_stack=0x%llx fn_range=0x%llx nearest_export=0x%llx "
               "read_qword=0x%llx fnlookup241=0x%llx",
               (unsigned long long)((uintptr_t)&spin_capture_ip - eb),
               (unsigned long long)((uintptr_t)&spin_sample_main_ip_gap - eb),
               (unsigned long long)((uintptr_t)&spin_read_stack_qwords - eb),
               (unsigned long long)((uintptr_t)&spin_fn_range - eb),
               (unsigned long long)((uintptr_t)&spin_host_nearest_export - eb),
               (unsigned long long)((uintptr_t)&spin_read_qword_guarded - eb),
               (unsigned long long)((uintptr_t)&spin_fnlookup_guarded - eb));
      trt_core_log(yl);
    }
  }
  spin_ip_sample s;
  const int cap = spin_capture_ip(tid, &s);
  if (cap == -2) {
    // .241 FIX: two-strike blacklist (see the state note
    // above). The per-qword guard should make stack-read
    // faults return partial instead of faulting at all;
    // the discriminator (s.cap_step) names the sub-step
    // when one still faults.
    const u32 prev_fault =
        g_gap_last_fault_tid.load(std::memory_order_relaxed);
    const unsigned prev_strikes =
        g_gap_capture_fault_strikes.load(std::memory_order_relaxed);
    const unsigned strikes = (prev_fault == tid) ? prev_strikes + 1 : 1;
    g_gap_last_fault_tid.store(tid, std::memory_order_relaxed);
    g_gap_capture_fault_strikes.store(strikes,
                                      std::memory_order_relaxed);
    if (strikes >= 2)
      g_gap_capture_fault_tid.store(tid, std::memory_order_relaxed);
    char fl[160];
    snprintf(fl, sizeof(fl),
             "sync: GAP-CAPTURE-FAULT tid=%u step=%d strike=%u",
             tid, s.cap_step, strikes);
    trt_core_log(fl);
    return;
  }
  if (cap != 1) {
    // Bound failure spam the same way as successes: consume a
    // slot so a thread that can never be opened/suspended logs
    // at most 6 lines, one per 5s.
    (void)g_gap_samples_done.fetch_add(1, std::memory_order_relaxed);
    g_gap_last_sample_ms.store(spin_now_ms(),
                               std::memory_order_relaxed);
    char fl[160];
    snprintf(fl, sizeof(fl),
             "sync: GAP-IP-SAMPLE tid=%u final=%d capture-fail=%d",
             tid, is_final ? 1 : 0, cap);
    trt_core_log(fl);
    return;
  }
  // .241 FIX: a capture success resets the two-strike count.
  g_gap_capture_fault_strikes.store(0, std::memory_order_relaxed);
  const unsigned n =
      g_gap_samples_done.fetch_add(1, std::memory_order_relaxed) + 1;
  g_gap_last_sample_ms.store(spin_now_ms(), std::memory_order_relaxed);
  // .244 DIAG: last bridge-issued D3D11 call + its age -- ties the
  // terminal wait to the bridge's most recent D3D11 work (see the
  // d3d11_note_last note above spin_sample_main_ip_gap).
  const char* last_name = nullptr;
  unsigned long long last_tick = 0;
  d3d11_last_call(&last_name, &last_tick);
  const unsigned long long last_age_ms =
      last_tick ? (unsigned long long)GetTickCount64() - last_tick : 0;
  // .246 DIAG: name the host function that called
  // NtWaitForAlertByThreadId (stkret) -- identifies main's
  // second-wait primitive (WaitOnAddress vs another alertable
  // wait). spin_host_nearest_export lives in this file.
  char stkret_fn[64] = "";
  u64 stkret_off = 0;
  if (s.stkret) spin_host_nearest_export(s.stkret, stkret_fn, sizeof(stkret_fn), &stkret_off);
  char sl[960];  // .244 DIAG: +lastd3d11=
  snprintf(sl, sizeof(sl),
           // .240 DIAG: r12/rbx/r13/r15 on EVERY sample --
           // the true suspended-thread callee-saved regs
           // (R12 = WorkerThreadPool group ID when main is
           // in wait_for_group_task_completion; RBX = the
           // group inner struct, R15 = its CV, R13 = its
           // mutex). The .239 samples captured them but
           // never logged them -- only GAP-GROUP did, and
           // its rip-based gate never fired.
           "sync: GAP-IP-SAMPLE n=%u tid=%u final=%d rip=0x%llx rip_off=0x%llx fn_off=0x%llx fn_len=0x%llx rsp=0x%llx sus=%lu ok=%d ntfn=%s ntoff=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx stkret=0x%llx stkfn=0x%llx stkguest=0x%llx stkguest_off=0x%llx rdxqw_ok=%d rdxqw=0x%llx r12=0x%llx rbx=0x%llx r13=0x%llx r15=0x%llx lastd3d11=%s %llums stkretfn=%s+0x%llx",
           n, tid, is_final ? 1 : 0,
           (unsigned long long)s.rip,
           (unsigned long long)s.rip_off,
           (unsigned long long)s.fn_off,
           (unsigned long long)s.fn_len,
           (unsigned long long)s.rsp, (unsigned long)s.sus,
           s.ok, s.ntfn, (unsigned long long)s.ntoff,
           (unsigned long long)s.rcx, (unsigned long long)s.rdx,
           (unsigned long long)s.r8, (unsigned long long)s.r9,
           (unsigned long long)s.stkret,
           (unsigned long long)s.stkfn,
           (unsigned long long)s.stkguest,
           (unsigned long long)s.stkguest_off,
           s.rdx_qword_ok,
           (unsigned long long)s.rdx_qword,
           (unsigned long long)s.r12, (unsigned long long)s.rbx,
           (unsigned long long)s.r13, (unsigned long long)s.r15,
           last_name ? last_name : "none", last_age_ms,
           stkret_fn[0] ? stkret_fn : "?", (unsigned long long)stkret_off);
  trt_core_log(sl);
  // .240 FIX: the .239 rip-based gate never fired (GAP-GROUP 0)
  // although main IS in wait_for_group_task_completion -- the
  // .239 CV-wait lines at 6836/7366 prove it (retfn=0x2dc5df0).
  // While main sits inside the SleepConditionVariable call,
  // RIP is in ntdll/kernel32, not in the wait fn: the wait fn
  // is only on the STACK (its return address, the address
  // after the +0x182 wait call). Gate on the stack instead:
  // any stk[] entry inside [0x2dc5df0, +0x57d) names the wait
  // (grp_stkidx = the matching slot; -1 when only RIP
  // matched, i.e. main was caught between wait calls).
  // The gap-hook R12 log the .240 brief asked for is NOT
  // done: the .239 GROUPID lines (6831, 7361) prove the
  // bridge-side R12 is 0 -- RtlCaptureContext inside the
  // wrapper reads the wrapper's R12, not the guest's.
  // The sampler's GetThreadContext R12 here is the true
  // guest value (and GAP-IP-SAMPLE now logs it always).
  // wait_for_group_task_completion (template RVA 0x2dc5df0,
  // len 0x57d): the group ID is the 2nd arg (RDX on entry),
  // saved in R12 at +0x1a (callee-saved, still live at the
  // +0x182 wait); the wait predicate is the counter at
  // [rbx+0x28] (rbx = [group+0x18] inner struct -- the waiter
  // loops while it is 0, decrements once it is nonzero);
  // R15 = the group CV, R13 = its mutex. Guarded reads: the
  // target is running again, the group heap is stable, and
  // a fault reports ok=0 instead of killing the host.
  int grp_stkidx = -1;
  if (s.img_base) {
    for (unsigned gi = 0; gi < s.stk_n; ++gi) {
      if (s.stk[gi] < s.img_base) continue;
      const u64 so = s.stk[gi] - s.img_base;
      if (so >= 0x2dc5df0ull && so < (0x2dc5df0ull + 0x57dull)) {
        grp_stkidx = (int)gi;
        break;
      }
    }
  }
  if (grp_stkidx >= 0 ||
      (s.rip_off >= 0x2dc5df0ull &&
       s.rip_off < (0x2dc5df0ull + 0x57dull))) {
    u64 g_cnt28 = 0, g_cnt34 = 0;
    const int g_ok28 =
        (s.rbx && spin_read_qword_guarded(s.rbx + 0x28, &g_cnt28)) ? 1 : 0;
    const int g_ok34 =
        (s.rbx && spin_read_qword_guarded(s.rbx + 0x34, &g_cnt34)) ? 1 : 0;
    char gl[320];
    snprintf(gl, sizeof(gl),
             "sync: GAP-GROUP n=%u group_id=%llu inner=0x%llx cnt28=%u cnt28_ok=%d cnt34=0x%llx cnt34_ok=%d cv=0x%llx mutex=0x%llx fn_off=0x%llx stkidx=%d",
             n, (unsigned long long)s.r12, (unsigned long long)s.rbx,
             (unsigned)(g_cnt28 & 0xffffffffu), g_ok28,
             (unsigned long long)g_cnt34, g_ok34,
             (unsigned long long)s.r15, (unsigned long long)s.r13,
             (unsigned long long)s.fn_off, grp_stkidx);
    trt_core_log(gl);
  }
  {
    // .234 DIAG: 9 -> 24 qwords (see struct).
    // .234 DIAG: 9 -> 24 qwords (see struct).
    char kl[1024];
    snprintf(kl, sizeof(kl),
             "sync: GAP-IP-STACK n=%u rsp=0x%llx n=%u q0=0x%llx q1=0x%llx q2=0x%llx q3=0x%llx q4=0x%llx q5=0x%llx q6=0x%llx q7=0x%llx q8=0x%llx q9=0x%llx q10=0x%llx q11=0x%llx q12=0x%llx q13=0x%llx q14=0x%llx q15=0x%llx q16=0x%llx q17=0x%llx q18=0x%llx q19=0x%llx q20=0x%llx q21=0x%llx q22=0x%llx q23=0x%llx",
             n, (unsigned long long)s.rsp, s.stk_n,
             (unsigned long long)s.stk[0], (unsigned long long)s.stk[1],
             (unsigned long long)s.stk[2], (unsigned long long)s.stk[3],
             (unsigned long long)s.stk[4], (unsigned long long)s.stk[5],
             (unsigned long long)s.stk[6], (unsigned long long)s.stk[7],
             (unsigned long long)s.stk[8], (unsigned long long)s.stk[9],
             (unsigned long long)s.stk[10], (unsigned long long)s.stk[11],
             (unsigned long long)s.stk[12], (unsigned long long)s.stk[13],
             (unsigned long long)s.stk[14], (unsigned long long)s.stk[15],
             (unsigned long long)s.stk[16], (unsigned long long)s.stk[17],
             (unsigned long long)s.stk[18], (unsigned long long)s.stk[19],
             (unsigned long long)s.stk[20], (unsigned long long)s.stk[21],
             (unsigned long long)s.stk[22], (unsigned long long)s.stk[23]);
    trt_core_log(kl);
  }
  // .234 DIAG: name main's Sleep when it is in NtDelayExecution
  // (Sekiro's second death): the DelayInterval is *rdx
  // (relative 100ns units, negative) or NULL (rdx==0) for an
  // infinite sleep. One sample cannot tell a poll loop from
  // one long sleep, but the up-to-6 gap samples (>=5s apart)
  // can: repeated finite Sleep samples = a poll loop; a
  // single INFINITE = main waits on something that never
  // fires. Diagnostic only: no wake, no fix.
  // .235 DIAG: place the Sleep caller (stkguest) in its function -- in
  // .234 stkguest_off (0x23a1c34) numerically EQUALS the worker routine
  // address, so q4 is either a return address INTO the worker proc
  // (main runs the job-system worker proc inline) or a stored routine
  // pointer. spin_fn_range decides: fn_off == stkguest_off means q4
  // IS the function start (stored pointer); fn_off < stkguest_off
  // means q4 is a return address inside the worker proc.
  u64 sg_fn_off = 0, sg_fn_len = 0;
  if (s.stkguest) spin_fn_range(s.stkguest, &sg_fn_off, &sg_fn_len);
  if (std::string(s.ntfn) == "ntdll!NtDelayExecution") {
    char dl[256];
    if (!s.rdx) {
      snprintf(dl, sizeof(dl),
               "sync: GAP-SLEEP n=%u alertable=%llu rdx=0 iv=INFINITE stkguest_fn_off=0x%llx fn_len=0x%llx",
               n, (unsigned long long)s.rcx,
               (unsigned long long)sg_fn_off, (unsigned long long)sg_fn_len);
    } else if (s.rdx_qword_ok) {
      const long long iv = (long long)s.rdx_qword;
      if (iv < 0) {
        snprintf(dl, sizeof(dl),
                 "sync: GAP-SLEEP n=%u alertable=%llu rdx=0x%llx iv_ms=%lld stkguest_fn_off=0x%llx fn_len=0x%llx",
                 n, (unsigned long long)s.rcx,
                 (unsigned long long)s.rdx, (long long)(-iv / 10000),
                 (unsigned long long)sg_fn_off, (unsigned long long)sg_fn_len);
      } else if (iv == 0) {
        snprintf(dl, sizeof(dl),
                 "sync: GAP-SLEEP n=%u alertable=%llu rdx=0x%llx iv_ms=0 stkguest_fn_off=0x%llx fn_len=0x%llx",
                 n, (unsigned long long)s.rcx,
                 (unsigned long long)s.rdx,
                 (unsigned long long)sg_fn_off, (unsigned long long)sg_fn_len);
      } else {
        snprintf(dl, sizeof(dl),
                 "sync: GAP-SLEEP n=%u alertable=%llu rdx=0x%llx iv_abs=0x%llx stkguest_fn_off=0x%llx fn_len=0x%llx",
                 n, (unsigned long long)s.rcx,
                 (unsigned long long)s.rdx,
                 (unsigned long long)s.rdx_qword,
                 (unsigned long long)sg_fn_off, (unsigned long long)sg_fn_len);
      }
    } else {
      snprintf(dl, sizeof(dl),
               "sync: GAP-SLEEP n=%u alertable=%llu rdx=0x%llx iv_ok=0 stkguest_fn_off=0x%llx fn_len=0x%llx",
               n, (unsigned long long)s.rcx,
               (unsigned long long)s.rdx,
               (unsigned long long)sg_fn_off, (unsigned long long)sg_fn_len);
    }
    trt_core_log(dl);
  }
  // Name the waited-on handle when main is in
  // NtWaitForSingleObject (same naming as the loop sampler's
  // MAIN-HANDLE / WAITED-THREAD, GAP-prefixed so the two
  // samplers' lines stay greppable apart). Diagnostic only: no
  // wake, no fix -- if this names another thread-wait like the
  // first death, the WAIT-THREAD-DEAD shape may apply in .234.
  if (std::string(s.ntfn) == "ntdll!NtWaitForSingleObject") {
    char stkhost[160];
    snprintf(stkhost, sizeof(stkhost), "none");
    u64 stkoff = 0;
    if (s.stk_n > 0)
      spin_host_nearest_export(s.stk[0], stkhost, sizeof(stkhost),
                               &stkoff);
    char htype[64];
    char hname[128];
    snprintf(htype, sizeof(htype), "unknown");
    snprintf(hname, sizeof(hname), "none");
    const bool hok = spin_query_handle_info(
        (void*)(uintptr_t)s.rcx, htype, sizeof(htype), hname,
        sizeof(hname));
    char hl[384];
    snprintf(hl, sizeof(hl),
             "sync: GAP-HANDLE n=%u handle=0x%llx ok=%d type=%s name=%s stkhost=%s stkoff=0x%llx",
             n, (unsigned long long)s.rcx, hok ? 1 : 0, htype,
             hname, stkhost, (unsigned long long)stkoff);
    trt_core_log(hl);
    if (hok && std::string(htype) == "Thread") {
      const DWORD wtid = ::GetThreadId((HANDLE)(uintptr_t)s.rcx);
      const DWORD wtid_err = wtid ? 0 : ::GetLastError();
      DWORD wexit = 0;
      const BOOL wok = ::GetExitCodeThread((HANDLE)(uintptr_t)s.rcx,
                                         &wexit);
      const bool walive = wok && wexit == STILL_ACTIVE;
      const u32 wexittid = g_spin_worker_exit_tid.load(
          std::memory_order_relaxed);
      char tl[384];
      snprintf(tl, sizeof(tl),
               "sync: GAP-WAITED-THREAD n=%u handle=0x%llx wtid=%lu wtid_err=%lu exit_ok=%d exitcode=%lu alive=%d workertid=%u ismain=%d isworker=%d",
               n, (unsigned long long)s.rcx,
               (unsigned long)wtid, (unsigned long)wtid_err,
               wok ? 1 : 0, (unsigned long)wexit,
               walive ? 1 : 0, wexittid,
               (wtid && wtid == tid) ? 1 : 0,
               (wtid && wexittid && wtid == wexittid) ? 1 : 0);
      trt_core_log(tl);
    }
    // .234 DIAG: reverse-map the NT-level host handle to the
    // bridge object table. Bridge events are emulated (no host
    // HANDLE), so a host Event is expected to MISS here -- the
    // miss itself is the finding (the guest got the handle
    // outside the bridge, via an unbridged CreateEventW).
    // A hit names the guest handle + creator + set history.
    // Also scans win_handle (only OBJ_THREAD has one) and the
    // parked-wait table. try_lock: the gap dump must never
    // block on g_obj_mu. Diagnostic only: no wake, no fix.
    {
      std::unique_lock<std::mutex> lk(g_obj_mu, std::try_to_lock);
      char rl[512];
      if (!lk.owns_lock()) {
        snprintf(rl, sizeof(rl),
                 "sync: GAP-HANDLE-REV n=%u host=0x%llx bridge=busy",
                 n, (unsigned long long)s.rcx);
        trt_core_log(rl);
      } else {
        auto* ro = object_for_locked(s.rcx);
        u64 rguest = 0;
        u32 rkind = 0;
        u32 rcreator = 0;
        u32 rlastset = 0;
        u64 rsets = 0;
        int rsig = 0;
        int rmanual = 0;
        if (ro) {
          rguest = s.rcx;
          rkind = (u32)ro->kind;
          rcreator = ro->creator_tid;
          rlastset = ro->last_set_tid;
          rsets = ro->set_count;
          rsig = ro->signalled ? 1 : 0;
          rmanual = ro->manual_reset ? 1 : 0;
        }
        u64 rwinguest = 0;
        for (size_t i = 0; i < g_objects.size(); ++i) {
          auto& wo = g_objects[i];
          if (wo.win_handle == (HANDLE)(uintptr_t)s.rcx) {
            rwinguest = ((u64)wo.generation << 32) |
                        (0x5B000000u | (u32)i);
            break;
          }
        }
        int rparked = 0;
        for (int i = 0; i < 32; ++i) {
          if (g_parked_waits[i].tid &&
              g_parked_waits[i].handle == s.rcx) {
            rparked = 1;
            break;
          }
        }
        snprintf(rl, sizeof(rl),
                 "sync: GAP-HANDLE-REV n=%u host=0x%llx bridge=%d guest=%llx kind=%u creator=%u lastset=%u sets=%llu sig=%d manual=%d winhit=%d winguest=%llx parked=%d",
                 n, (unsigned long long)s.rcx, ro ? 1 : 0,
                 (unsigned long long)rguest, rkind, rcreator, rlastset,
                 (unsigned long long)rsets, rsig, rmanual,
                 rwinguest ? 1 : 0, (unsigned long long)rwinguest, rparked);
        trt_core_log(rl);
      }
    }
    // .236 DIAG: name the host caller chain above the wait -- 0x62c
    // is a host Semaphore (GAP-HANDLE type=Semaphore, bridge=0),
    // so the waiter is host code, not the bridge.
    // .242 DIAG: the .241 GAP-STACK-HOST resolved stk[1..4],
    // but the .241 Ultrapool samples prove stk[1..4] are
    // LOCALS (stack addr / 0 / heap ptr / the handle),
    // not return addresses: all four resolved to none+0x0
    // while the real second return sits at q20
    // (0x7ff80f6cad16, a host address). Scan the whole
    // (now 96-qword) window for qwords that look like host
    // code addresses (0x7FF000000000..0x800000000000) and
    // resolve up to 6 of them to nearest host exports --
    // that names the DLL/fn above
    // kernelbase!WaitForSingleObjectEx for the 0x644 wait.
    // .243 DIAG: the .242 scan resolved q20 to
    // user32!wvsprintfA+0x2440656 -- an impossible offset
    // (~38MB past the export), a nearest-export artifact,
    // NOT a naming. Two fixes: (1) spin_host_nearest_export
    // now rejects offsets >64KB (see that function);
    // (2) each found qword is ALSO resolved to its
    // containing host module via the PEB LDR walk
    // (spin_host_module_of) and printed as mod+off -- the
    // module name + module-relative offset names the waiter
    // even when no export sits near it. The export name
    // (when within 64KB) is printed in parens after it.
    // The scan no longer SKIPS a qword whose export fails
    // to resolve: the module name alone is the deliverable.
    // Diagnostic only: no wake, no fix.
    {
      char sh[1536];
      size_t sh_n = 0;
      unsigned sh_found = 0;
      sh[0] = 0;
      for (unsigned gi = 1; gi < s.stk_n && sh_found < 6; ++gi) {
        const u64 hv = s.stk[gi];
        if (hv < 0x7ff000000000ULL || hv >= 0x800000000000ULL) continue;
        char mod[72];
        u64 moff = 0;
        const bool has_mod = spin_host_module_of(hv, mod, sizeof(mod), &moff);
        char hn[160];
        u64 ho = 0;
        const bool has_exp =
            spin_host_nearest_export(hv, hn, sizeof(hn), &ho);
        int w = 0;
        if (has_mod && has_exp) {
          w = snprintf(sh + sh_n, sizeof(sh) - sh_n,
                       "%sq%u=%s+0x%llx(%s+0x%llx)", sh_found ? " " : "",
                       gi, mod, (unsigned long long)moff,
                       hn, (unsigned long long)ho);
        } else if (has_mod) {
          w = snprintf(sh + sh_n, sizeof(sh) - sh_n,
                       "%sq%u=%s+0x%llx", sh_found ? " " : "",
                       gi, mod, (unsigned long long)moff);
        } else if (has_exp) {
          w = snprintf(sh + sh_n, sizeof(sh) - sh_n,
                       "%sq%u=%s+0x%llx", sh_found ? " " : "",
                       gi, hn, (unsigned long long)ho);
        } else {
          continue;
        }
        if (w <= 0 || (size_t)w >= sizeof(sh) - sh_n) break;
        sh_n += (size_t)w;
        ++sh_found;
      }
      char shl[1664];
      snprintf(shl, sizeof(shl), "sync: GAP-STACK-HOST n=%u found=%u %s",
               n, sh_found, sh_found ? sh : "none");
      trt_core_log(shl);
    }
  }
#else
  (void)tid;
  (void)is_final;
#endif
}

// The gap-dump hook needs the spinning tid (Sekiro main).
// Non-static: the caller lives in bridge_kernel32.cpp.
extern "C" u32 trt_spin_main_tid(void) {
  return g_spin_main_tid.load(std::memory_order_relaxed);
}

// .235 DIAG: attribute a host crash address (module + offset) for the
// HOST UNHANDLED logger (main.cpp, snapshot-only caller). The .234
// Ultrapool host crash (0xc0000005 at 00007FF71C1AB112) named neither
// the faulting thread nor the module. Reuses spin_host_nearest_export
// (same GetModuleHandleW + export walk, no new UWP-partition APIs);
// falls back to the exe-base offset when no export precedes the
// address. Diagnostic only. Outside the anonymous namespace +
// extern "C" (cross-TU, .233 link lesson).
extern "C" void trt_host_crash_addr(u64 addr, char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  buf[0] = 0;
  char nm[160];
  u64 off = 0;
  if (spin_host_nearest_export(addr, nm, sizeof(nm), &off)) {
    snprintf(buf, cap, "%s+0x%llx", nm, (unsigned long long)off);
    return;
  }
#if defined(_WIN32)
  // Host-crash attribution only exists on a Windows host; HMODULE and
  // GetModuleHandleW have no Linux meaning.
  HMODULE exe = ::GetModuleHandleW(nullptr);
  if (exe && addr >= (u64)(uintptr_t)exe) {
    snprintf(buf, cap, "exe+0x%llx",
             (unsigned long long)(addr - (u64)(uintptr_t)exe));
    return;
  }
#endif
  snprintf(buf, cap, "none");
}

// .237 DIAG: resolve a HOST address to its containing function
// in the exe's own .pdata (RUNTIME_FUNCTION table) -- the
// faulting function's exact RVA + length, where the
// nearest-export attribution (trt_host_crash_addr) is weak
// (the exe exports few functions, so the nearest export can
// be ~800KB away, as in .236: F3DAudioCheckInitParams+
// 0xc146c). The HOST UNHANDLED filter (snapshot-only main
// delta) logs the RVA+len; the .237 run names the faulting
// function by matching the RVA against the HOST SYMS line
// (logged on the first GAP-CAPTURE-TRY). Same PE-header walk
// as spin_host_nearest_export (no new UWP-partition APIs);
// __try-guarded reads (a torn PE header must not fault the
// filter). Outside the anonymous namespace + extern "C"
// (cross-TU, .233 link lesson). Returns 1 on success
// (rip_rva/fn_rva/fn_len set), 0 otherwise.
extern "C" int trt_host_fn_range(unsigned long long addr,
                                 unsigned long long* rip_rva,
                                 unsigned long long* fn_rva,
                                 unsigned long long* fn_len) {
  if (rip_rva) *rip_rva = 0;
  if (fn_rva) *fn_rva = 0;
  if (fn_len) *fn_len = 0;
  if (!addr) return 0;
#if defined(_WIN32)
  int rc = 0;
  __try {
    HMODULE exe = ::GetModuleHandleW(nullptr);
    if (!exe) return 0;
    const unsigned char* base = (const unsigned char*)exe;
    if (*(const unsigned short*)base != 0x5A4D) return 0;
    const unsigned long pe_off = *(const unsigned long*)(base + 0x3C);
    if (pe_off > 4096) return 0;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(base + pe_off);
    if (nt->Signature != 0x4550) return 0;
    if (nt->OptionalHeader.Magic != 0x20B) return 0;
    const IMAGE_DATA_DIRECTORY& dd =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dd.VirtualAddress || !dd.Size) return 0;
    const unsigned long count = dd.Size / 12;
    if (count == 0 || count > 1000000) return 0;
    const unsigned long long b = (unsigned long long)(uintptr_t)exe;
    if (addr < b) return 0;
    const unsigned long rva = (unsigned long)(addr - b);
    if (rip_rva) *rip_rva = (unsigned long long)rva;
    const unsigned char* pdata = base + dd.VirtualAddress;
    unsigned long lo = 0, hi = count;
    while (lo < hi) {
      const unsigned long mid = lo + (hi - lo) / 2;
      const unsigned long begin =
          *(const unsigned long*)(pdata + mid * 12);
      const unsigned long end =
          *(const unsigned long*)(pdata + mid * 12 + 4);
      if (rva < begin) {
        hi = mid;
      } else if (rva >= end) {
        lo = mid + 1;
      } else {
        if (fn_rva) *fn_rva = (unsigned long long)begin;
        if (fn_len) *fn_len = (unsigned long long)(end - begin);
        rc = 1;
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    rc = 0;
  }
  return rc;
#else
  (void)addr;
  return 0;
#endif
}

// .237 DIAG: query a handle's type (+ name) for the GAP-WAIT-HOST
// line in the snapshot-only kernel32 delta2 (the
// NtWaitForSingleObject trap path). Wraps spin_query_handle_info
// (same NtQueryObject call the .235 GAP-HANDLE line used).
// Outside the anonymous namespace + extern "C" (cross-TU, .233
// link lesson). Returns 1 when the type was read.
extern "C" int trt_host_handle_type(void* h, char* type_buf,
                                    size_t type_cap, char* name_buf,
                                    size_t name_cap) {
  if (!type_buf || type_cap == 0) return 0;
  type_buf[0] = 0;
  if (name_buf && name_cap > 0) name_buf[0] = 0;
  if (!h) return 0;
#if defined(_WIN32)
  return spin_query_handle_info(h, type_buf, type_cap, name_buf,
                                name_cap)
             ? 1
             : 0;
#else
  (void)h;
  (void)name_buf;
  (void)name_cap;
  return 0;
#endif
}

extern "C" void trt_sync_parked_log(void) {
  std::unique_lock<std::mutex> lk(g_obj_mu, std::try_to_lock);
  if (!lk.owns_lock()) {
    trt_core_log("sync: PARKED report-busy");
    return;
  }
  bool any = false;
  for (int i = 0; i < 32; ++i) {
    if (!g_parked_waits[i].tid) continue;
    any = true;
    const sync_object* o = object_for_locked(g_parked_waits[i].handle);
    char pl[256];
    snprintf(pl, sizeof(pl),
             // .235 DIAG: creator_tid -- who created the event the parked
             // thread waits on (Sekiro: who created the workers'
             // never-set events 25b00042f/35b00042f/25b000438).
             "sync: PARKED tid=%u handle=%llx kind=%u handles=%u parked_ms=%llu lastset=%u sets=%llu creator=%u",
             g_parked_waits[i].tid,
             (unsigned long long)g_parked_waits[i].handle,
             g_parked_waits[i].kind, g_parked_waits[i].handle_count,
             (unsigned long long)(park_now_ms() - g_parked_waits[i].since_ms),
             o ? o->last_set_tid : 0u,
             o ? (unsigned long long)o->set_count : 0ull,
             o ? o->creator_tid : 0u);
    trt_core_log(pl);
  }
  if (!any) trt_core_log("sync: PARKED none");
  // .225 DIAG: pairing check on the final dump (fires for a graceful
  // stop / Ultrapool clear; the Sekiro final dump never runs, so the
  // live pairing comes from the WAIT-FAST trip).
  maybe_log_lost_wakeup(0);
}

// .223 DIAG: parked-thread count for the gap SILENCE dump --
// returns the number of threads currently parked, or -1 if
// the table is busy (same try-lock as trt_sync_parked_log).
extern "C" int trt_sync_parked_count(void) {
  std::unique_lock<std::mutex> lk(g_obj_mu, std::try_to_lock);
  if (!lk.owns_lock()) return -1;
  int n = 0;
  for (int i = 0; i < 32; ++i) {
    if (g_parked_waits[i].tid) ++n;
  }
  return n;
}

u32 TRT_STDCALL trt_WaitForSingleObjectEx(u64 handle, u32 ms, int alertable) {
  return wait_single_impl(handle, ms, alertable != 0, __builtin_return_address(0));
}

u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count, const u64* handles,
                                           bool wait_all, u32 ms) {
  return wait_multiple_impl(count, handles, wait_all, ms, false);
}

u32 TRT_STDCALL trt_WaitForMultipleObjectsEx(u32 count, const u64* handles,
                                             int wait_all, u32 ms,
                                             int alertable) {
  return wait_multiple_impl(count, handles, wait_all != 0, ms, alertable != 0);
}

u32 TRT_STDCALL trt_SleepEx(u32 ms, int alertable) {
  if (!alertable) {
    trt_Sleep(ms);
    return 0;
  }
  const u32 my_tid = current_tid();
  std::unique_lock<std::mutex> lk(g_obj_mu);
  if (drain_apcs_locked(lk, my_tid)) return 0xC0u;
  if (ms == 0) return 0;
  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    if (ms == 0xFFFFFFFFu) {
      g_sync_cv.wait(lk);
    } else {
      g_sync_cv.wait_until(lk, deadline);
    }
    if (drain_apcs_locked(lk, my_tid)) return 0xC0u;
    if (ms != 0xFFFFFFFFu && std::chrono::steady_clock::now() >= deadline) {
      return 0;
    }
  }
}

int TRT_STDCALL trt_SwitchToThread(void) {
  // .219 DIAG: a SwitchToThread yield spin was invisible in .218
  // (not in the gap record list); consecutive duplicates collapse,
  // so a yield spin is one entry with a huge repeat count.
  gap_trace_record("SwitchToThread");
#if defined(_WIN32)
  SwitchToThread();
  return 1;
#else
  sched_yield();
  return 1;
#endif
}

int TRT_STDCALL trt_GetExitCodeThread(u64 thread, u32* code) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return 0;
  }
  if (code) {
    *code = o->alive.load(std::memory_order_acquire) ? 259u /*STILL_ACTIVE*/
                                                     : o->exit_code;
  }
  return 1;
}

void* TRT_STDCALL trt_GetCurrentThread(void) {
  return (void*)(uintptr_t)(i64)-2;
}

u32 TRT_STDCALL trt_GetThreadId(u64 thread) {
  if (thread == (u64)(i64)-2) return trt_GetCurrentThreadId();
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return 0;
  }
  return o->tid ? o->tid : 1;
}

// DuplicateHandle(GetCurrentThread()) must yield a REAL, process-wide thread
// handle: bdwgc (Unity/Mono/IL2CPP) stores it per registered thread and later
// calls SuspendThread/GetThreadContext on it from the collecting thread. The
// -2 pseudo-handle would resolve to the *caller* there (or fail) -> "Fatal
// error in GC: SuspendThread loop failed". Threads we created reuse their
// object; foreign/main threads are adopted (pthread_self captured).
u64 trt_thread_handle_for_current(void) {
#if defined(_WIN32)
  // ADOPT the current real host thread: match by real TID first (threads the
  // bridge created already have a win_handle), otherwise duplicate our own
  // real handle via the HOST kernel32 (NOT the guest bridge — recursion).
  // Without this, DuplicateHandle(GetCurrentThread()) on the console handed
  // the GC the -2 pseudo-handle itself; the collector then failed its
  // SuspendThread loop -> "Fatal error in GC" -> 0x80000003 (RACCOIN).
  const DWORD self_tid = ::GetCurrentThreadId();
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    for (u32 i = 0; i < g_objects.size(); ++i) {
      sync_object& o = g_objects[i];
      if (o.kind == OBJ_THREAD && o.tid == self_tid &&
          o.alive.load(std::memory_order_acquire)) {
        o.handle_refs++;
        return make_handle(i, o.generation);
      }
    }
  }
  HANDLE real = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                         ::GetCurrentProcess(), &real, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);  // alloc_object_locked expects
    const u64 h = alloc_object_locked(OBJ_THREAD);  // the caller to hold it
    if (!h) {
      ::CloseHandle(real);
      return 0;
    }
    sync_object* o = object_for_locked(h);
    o->win_handle = real;
    o->tid = self_tid;
    o->started = true;
    o->alive.store(true, std::memory_order_release);
    o->handle_refs++;  // we hand this back to the caller
    return h;
  }
#else
  const pthread_t self = pthread_self();
  std::lock_guard<std::mutex> lk(g_obj_mu);
  for (u32 i = 0; i < g_objects.size(); ++i) {
    sync_object& o = g_objects[i];
    if (o.kind == OBJ_THREAD && o.has_pthread &&
        o.alive.load(std::memory_order_acquire) &&
        pthread_equal(o.pthread_handle, self)) {
      o.handle_refs++;
      return make_handle(i, o.generation);
    }
  }
  const u64 h = alloc_object_locked(OBJ_THREAD);
  if (!h) return 0;
  sync_object* o = object_for_locked(h);
  o->pthread_handle = self;
  o->has_pthread = true;
  o->tid = current_tid();
  o->started = true;
  o->alive.store(true, std::memory_order_release);
  init_thread_pipes(o);
  return h;
#endif
}

int trt_retain_sync_handle(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o) return 0;
  ++o->handle_refs;
  return 1;
}

u32 TRT_STDCALL trt_ResumeThread(u64 thread) {
#if defined(_WIN32)
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(thread);
    if (!o || o->kind != OBJ_THREAD) {
      trt_SetLastError(6);
      return (u32)-1;
    }
    h = o->win_handle;
    if (o->suspend_count.load(std::memory_order_relaxed) > 0) {
      o->suspend_count.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  if (!h) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  u32 prev_count = ::ResumeThread(h);
  {
    // .199 diagnostic: is the arena worker ever resumed? (First 32.)
    static unsigned s_resume_logged = 0;
    if (s_resume_logged < 32) {
      ++s_resume_logged;
      char rl[160];
      snprintf(rl, sizeof(rl), "thread: ResumeThread obj=0x%llx -> prev_count=%u",
               (unsigned long long)thread, prev_count);
      trt_core_log(rl);
    }
  }
  if (prev_count == 1) ::Sleep(500);  // .205: 100ms insufficient (.204) -- worker does zero allocs after entering routine, needs more init time (or is blocked; 500ms distinguishes)
  return prev_count;
#else
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  const int prev = o->suspend_count.load(std::memory_order_acquire);
  if (prev > 0) {
    if (o->suspend_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (o->in_sig_suspend.load(std::memory_order_acquire)) {
        write_pipe_retry(o->wake_pipe[1], 'R');
      }
      g_sync_cv.notify_all();
    }
  }
  return (u32)prev;
#endif
}

u32 TRT_STDCALL trt_SuspendThread(u64 thread) {
#if defined(_WIN32)
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(thread);
    if (!o || o->kind != OBJ_THREAD) {
      trt_SetLastError(6);
      return (u32)-1;
    }
    h = o->win_handle;
  }
  if (!h) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  // SAFE SUSPEND: SuspendThread can park a guest thread while it holds our
  // registry mutex (g_obj_mu) — the collector's NEXT suspend would deadlock
  // on that mutex forever (the flaky-crash class). Boehm-style loop: park,
  // verify the registry is takeable, resume+retry if not. Converges because
  // bridge critical sections are microsecond-scale; a fresh non-suspended
  // holder also releases in microseconds. The returned value IS the real
  // previous suspend count (from the host), so the object counter stays
  // driven by ResumeThread only.
  unsigned iter = 0;
  while (true) {
    const DWORD src = ::SuspendThread(h);
    if (src == (DWORD)-1) {
      trt_SetLastError(::GetLastError() ? (u32)::GetLastError() : 5u);
      return (u32)-1;
    }
    if (g_obj_mu.try_lock()) {
      g_obj_mu.unlock();
      return (u32)src;  // parked cleanly; registry reachable
    }
    ::ResumeThread(h);  // target (or a peer) holds the registry — retry
    if (++iter > 2000u) {
      trt_SetLastError(5);  // named: registry held too long under suspension
      return (u32)-1;
    }
    std::this_thread::yield();
  }
#else
  ensure_sigusr2_installed();
  std::lock_guard<std::mutex> slk(g_suspend_op_mu);
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  while (o->in_sig_suspend.load(std::memory_order_acquire) &&
         o->suspend_count.load(std::memory_order_acquire) == 0 &&
         o->alive.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const int prev = o->suspend_count.load(std::memory_order_acquire);
  if (!o->alive.load(std::memory_order_acquire)) {
    trt_SetLastError(5);  // ERROR_ACCESS_DENIED on terminated thread
    return (u32)-1;
  }
  const bool already_suspended = (prev > 0) || !o->has_pthread || !o->started;
  const bool is_self = pthread_equal(o->pthread_handle, pthread_self()) != 0;
  o->suspend_count.fetch_add(1, std::memory_order_acq_rel);
  if (already_suspended || is_self) {
    return (u32)prev;
  }

  g_active_suspend.store(o, std::memory_order_release);
  const int krc = pthread_kill(o->pthread_handle, SIGUSR2);
  if (krc != 0) {
    g_active_suspend.store(nullptr, std::memory_order_release);
    o->suspend_count.fetch_sub(1, std::memory_order_acq_rel);
    if (krc == ESRCH) {
      o->has_pthread = false;
      o->alive.store(false, std::memory_order_release);
      o->signalled = true;
      o->thread_ref = false;
      g_sync_cv.notify_all();
      trt_SetLastError(5);  // ERROR_ACCESS_DENIED (thread exited)
    } else {
      trt_SetLastError(6);
    }
    return (u32)-1;
  }
  read_pipe_retry(o->ack_pipe[0]);
  g_active_suspend.store(nullptr, std::memory_order_release);
  return (u32)prev;
#endif
}

int TRT_STDCALL trt_GetThreadContext(u64 thread, void* ctx) {
  if (!ctx) {
    trt_SetLastError(87);
    return 0;
  }
#if defined(_WIN32)
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(thread);
    if (!o || o->kind != OBJ_THREAD) {
      trt_SetLastError(6);
      return 0;
    }
    h = o->win_handle;
  }
  if (!h) {
    trt_SetLastError(6);
    return 0;
  }
  return ::GetThreadContext(h, (CONTEXT*)ctx) ? 1 : 0;
#else
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return 0;
  }
  Win64Context* dst = (Win64Context*)ctx;
  const u32 req_flags = dst->ContextFlags;
  if (o->has_captured_ctx.load(std::memory_order_acquire)) {
    *dst = o->captured_ctx;
    if (req_flags != 0) dst->ContextFlags = req_flags;
    return 1;
  }
  if (!o->started && o->start_routine) {
    fill_initial_win64_ctx(o, dst);
    if (req_flags != 0) dst->ContextFlags = req_flags;
    return 1;
  }
  trt_SetLastError(50);
  return 0;
#endif
}

int TRT_STDCALL trt_SetThreadContext(u64 thread, const void* ctx) {
  if (!ctx) {
    trt_SetLastError(87);
    return 0;
  }
#if defined(_WIN32)
  HANDLE h = nullptr;
  bool pre_start = false;
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    sync_object* o = object_for_locked(thread);
    if (!o || o->kind != OBJ_THREAD) {
      trt_SetLastError(6);
      return 0;
    }
    h = o->win_handle;
    pre_start = (!o->started && o->suspend_count.load(std::memory_order_acquire) > 0);
    if (pre_start) {
      const CONTEXT* c = (const CONTEXT*)ctx;
      if (c->ContextFlags & CONTEXT_CONTROL) {
        o->start_routine = (void*)(uintptr_t)c->Rip;
      }
      if (c->ContextFlags & CONTEXT_INTEGER) {
        o->start_arg = c->Rcx;
      }
      return 1;
    }
  }
  if (!h) {
    trt_SetLastError(6);
    return 0;
  }
  using set_ctx_fn = BOOL(WINAPI*)(HANDLE, const CONTEXT*);
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  auto fn = kb ? (set_ctx_fn)GetProcAddress(kb, "SetThreadContext") : nullptr;
  if (fn && fn(h, (const CONTEXT*)ctx)) return 1;
  trt_SetLastError(50);
  return 0;
#else
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) {
    trt_SetLastError(6);
    return 0;
  }
  const Win64Context* src = (const Win64Context*)ctx;
  if (!o->started) {
    if (src->ContextFlags & (k_context_control & ~k_context_amd64)) {
      o->start_routine = (void*)(uintptr_t)src->Rip;
    }
    if (src->ContextFlags & (k_context_integer & ~k_context_amd64)) {
      o->start_arg = src->Rcx;
    }
    return 1;
  }
  o->captured_ctx = *src;
  o->ctx_dirty.store(true, std::memory_order_release);
  o->has_captured_ctx.store(true, std::memory_order_release);
  return 1;
#endif
}

u64 TRT_STDCALL trt_OpenThread(u32 access, int inherit, u32 tid) {
  (void)access; (void)inherit;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  for (u32 i = 0; i < g_objects.size(); ++i) {
    if (g_objects[i].kind == OBJ_THREAD && g_objects[i].tid == tid) {
      ++g_objects[i].handle_refs;
      return make_handle(i, g_objects[i].generation);
    }
  }
  trt_SetLastError(87);
  return 0;
}

int TRT_STDCALL trt_SetThreadPriority(u64 thread, int prio) {
  if (thread == (u64)(i64)-2) return 1;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) { trt_SetLastError(6); return 0; }
  o->priority = prio;
  return 1;
}

int TRT_STDCALL trt_GetThreadPriority(u64 thread) {
  if (thread == (u64)(i64)-2) return 0;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) { trt_SetLastError(6); return 0x7FFFFFFF; }
  return o->priority;
}

// --- events ---
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                 const u16* name) {
  (void)attrs;
  const std::string key = utf16_to_utf8(name);
  std::lock_guard<std::mutex> lk(g_obj_mu);
  if (!key.empty()) {
    auto it = g_named_objects.find("E:" + key);
    if (it != g_named_objects.end()) {
      sync_object* existing = object_for_locked(it->second);
      if (existing && existing->kind == OBJ_EVENT) {
        ++existing->handle_refs;
        trt_SetLastError(183);  // ERROR_ALREADY_EXISTS
        return it->second;
      }
      g_named_objects.erase(it);
    }
  }
  const u64 h = alloc_object_locked(OBJ_EVENT);
  if (!h) {
    trt_SetLastError(8);
    return 0;
  }
  sync_object* o = object_for_locked(h);
  o->manual_reset = manual;
  o->signalled = initial;
  if (!key.empty()) {
    o->name = "E:" + key;
    g_named_objects[o->name] = h;
  }
  {
    // .208 diagnostic: log event creation (first 64) so the lifecycle of
    // the event the Sekiro worker waits on is visible.
    static unsigned s_create_event_logged = 0;
    if (s_create_event_logged < 64) {
      ++s_create_event_logged;
      char el[256];
      snprintf(el, sizeof(el),
               "sync: CreateEvent handle=%llx manual=%u initial=%u name=%s",
               (unsigned long long)h, manual ? 1u : 0u, initial ? 1u : 0u,
               key.empty() ? "(anon)" : key.c_str());
      trt_core_log(el);
    }
  }
  trt_SetLastError(0);
  return h;
}

u64 TRT_STDCALL trt_CreateEventA(void* attrs, bool manual, bool initial,
                                 const char* name) {
  if (!name) return trt_CreateEventW(attrs, manual, initial, nullptr);
  const auto w = ansi_to_utf16(name);
  return trt_CreateEventW(attrs, manual, initial, w.data());
}

u64 TRT_STDCALL trt_OpenEventW(u32 access, int inherit, const u16* name) {
  (void)access; (void)inherit;
  const std::string key = utf16_to_utf8(name);
  if (key.empty()) {
    trt_SetLastError(87);
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_obj_mu);
  auto it = g_named_objects.find("E:" + key);
  if (it == g_named_objects.end()) {
    trt_SetLastError(2);  // ERROR_FILE_NOT_FOUND
    return 0;
  }
  sync_object* o = object_for_locked(it->second);
  if (!o || o->kind != OBJ_EVENT) {
    trt_SetLastError(2);
    return 0;
  }
  ++o->handle_refs;
  return it->second;
}

u64 TRT_STDCALL trt_OpenEventA(u32 access, int inherit, const char* name) {
  if (!name) { trt_SetLastError(87); return 0; }
  const auto w = ansi_to_utf16(name);
  return trt_OpenEventW(access, inherit, w.data());
}

int TRT_STDCALL trt_SetEvent(u64 handle) {
  gap_trace_record("SetEvent");
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) {
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    g_launch_event_failed.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  o->signalled = true;
  g_launch_event_signaled.fetch_add(1, std::memory_order_relaxed);
  // .221 DIAG: record the setter -- the ping-pong partner for a
  // thread parked in WaitForSingleObject on this event.
  o->last_set_tid = current_tid();
  ++o->set_count;
  {
    // .208 diagnostic: log event signals (first 64 + every
    // 256th, so death-phase sets stay visible; .221 adds the
    // setter tid + set count).
    static unsigned s_set_event_logged = 0;
    ++s_set_event_logged;
    if (s_set_event_logged <= 64 || (s_set_event_logged % 256) == 0) {
      char el[192];
      snprintf(el, sizeof(el), "sync: SetEvent handle=%llx tid=%u sets=%llu",
               (unsigned long long)handle, o->last_set_tid,
               (unsigned long long)o->set_count);
      trt_core_log(el);
    }
    // .225 DIAG: a set of a watched start event (expect none for
    // the start handle) + gate-trace record of the creator
    // thread sets. The ring keeps the first 64 sets overall:
    // init sets, not the spin loop repeats.
    watched_start* w = watched_find(handle);
    if (w && w->set_hits < 8) {
      ++w->set_hits;
      char wl[192];
      snprintf(wl, sizeof(wl),
               "sync: SET-WATCHED handle=%llx tid=%u sets=%llu",
               (unsigned long long)handle, o->last_set_tid,
               (unsigned long long)o->set_count);
      trt_core_log(wl);
    }
    if (s_set_event_logged <= 64)
      gate_ring_record(o->last_set_tid, 2, handle, (u32)o->kind);
  }
  g_sync_cv.notify_all();
  return 1;
}

int TRT_STDCALL trt_ResetEvent(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) {
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  o->signalled = false;
  {
    // .208 diagnostic: log event resets (first 64).
    static unsigned s_reset_event_logged = 0;
    if (s_reset_event_logged < 64) {
      ++s_reset_event_logged;
      char el[128];
      snprintf(el, sizeof(el), "sync: ResetEvent handle=%llx",
               (unsigned long long)handle);
      trt_core_log(el);
    }
  }
  return 1;
}

int TRT_STDCALL trt_PulseEvent(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) return 0;
  o->signalled = true;
  // .221 DIAG: a pulse is a set attempt (see trt_SetEvent).
  o->last_set_tid = current_tid();
  ++o->set_count;
  {
    // .208 diagnostic: log event pulses (first 64). A Pulse before any
    // thread waits LOSES the signal (set then immediately cleared).
    static unsigned s_pulse_event_logged = 0;
    if (s_pulse_event_logged < 64) {
      ++s_pulse_event_logged;
      char el[128];
      snprintf(el, sizeof(el), "sync: PulseEvent handle=%llx",
               (unsigned long long)handle);
      trt_core_log(el);
    }
  }
  g_sync_cv.notify_all();
  o->signalled = false;
  return 1;
}

// --- mutex ---
u64 TRT_STDCALL trt_CreateMutexW(void* attrs, bool initial_owner,
                                 const u16* name) {
  (void)attrs;
  const std::string key = utf16_to_utf8(name);
  const u32 my_tid = current_tid();
  std::lock_guard<std::mutex> lk(g_obj_mu);
  if (!key.empty()) {
    auto it = g_named_objects.find("M:" + key);
    if (it != g_named_objects.end()) {
      sync_object* existing = object_for_locked(it->second);
      if (existing && existing->kind == OBJ_MUTEX) {
        ++existing->handle_refs;
        trt_SetLastError(183);
        return it->second;
      }
      g_named_objects.erase(it);
    }
  }
  const u64 h = alloc_object_locked(OBJ_MUTEX);
  if (!h) {
    trt_SetLastError(8);
    return 0;
  }
  sync_object* o = object_for_locked(h);
  o->lock_count = initial_owner ? 1 : 0;
  o->owner_tid = initial_owner ? my_tid : 0;
  if (!key.empty()) {
    o->name = "M:" + key;
    g_named_objects[o->name] = h;
  }
  {
    // .224 DIAG: log mutex creation (first 64) -- CreateEvent
    // has logged since .208 but CreateMutex never did, so the
    // Sekiro spin mutexes 25b000001/2 had no provenance at all.
    static unsigned s_create_mutex_logged = 0;
    if (s_create_mutex_logged < 64) {
      ++s_create_mutex_logged;
      char ml[256];
      snprintf(ml, sizeof(ml),
               "sync: CreateMutex handle=%llx initial_owner=%u name=%s tid=%u",
               (unsigned long long)h, initial_owner ? 1u : 0u,
               key.empty() ? "(anon)" : key.c_str(), my_tid);
      trt_core_log(ml);
    }
  }
  trt_SetLastError(0);
  return h;
}

u64 TRT_STDCALL trt_CreateMutexA(void* attrs, bool initial_owner,
                                 const char* name) {
  if (!name) return trt_CreateMutexW(attrs, initial_owner, nullptr);
  const auto w = ansi_to_utf16(name);
  return trt_CreateMutexW(attrs, initial_owner, w.data());
}

u64 TRT_STDCALL trt_OpenMutexW(u32 access, int inherit, const u16* name) {
  (void)access; (void)inherit;
  const std::string key = utf16_to_utf8(name);
  if (key.empty()) { trt_SetLastError(87); return 0; }
  std::lock_guard<std::mutex> lk(g_obj_mu);
  auto it = g_named_objects.find("M:" + key);
  if (it == g_named_objects.end()) {
    trt_SetLastError(2);
    return 0;
  }
  sync_object* o = object_for_locked(it->second);
  if (!o || o->kind != OBJ_MUTEX) {
    trt_SetLastError(2);
    return 0;
  }
  ++o->handle_refs;
  return it->second;
}

u64 TRT_STDCALL trt_OpenMutexA(u32 access, int inherit, const char* name) {
  if (!name) { trt_SetLastError(87); return 0; }
  const auto w = ansi_to_utf16(name);
  return trt_OpenMutexW(access, inherit, w.data());
}

int TRT_STDCALL trt_ReleaseMutex(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_MUTEX) return 0;
  if (o->lock_count <= 0) {
    trt_SetLastError(288);  // ERROR_NOT_OWNER
    return 0;
  }
  --o->lock_count;
  // .224 DIAG: a release is a signal for mutexes -- record the
  // releaser (mirrors SetEvent). Before .224, set_count stayed
  // 0 for mutexes forever, so WAIT-FAST lastset=0/sets=0 did
  // NOT mean "never released", it meant "never counted".
  o->last_set_tid = current_tid();
  ++o->set_count;
  if (o->lock_count == 0) {
    o->owner_tid = 0;
    g_sync_cv.notify_all();
  }
  {
    // .224 DIAG: log releases (first 64 + every 256th,
    // mirroring SetEvent) -- shows who releases the spin
    // mutexes, if anyone.
    static unsigned s_release_mutex_logged = 0;
    ++s_release_mutex_logged;
    if (s_release_mutex_logged <= 64 ||
        (s_release_mutex_logged % 256) == 0) {
      char rl[192];
      snprintf(rl, sizeof(rl),
               "sync: ReleaseMutex handle=%llx tid=%u releases=%llu",
               (unsigned long long)handle, o->last_set_tid,
               (unsigned long long)o->set_count);
      trt_core_log(rl);
    }
  }
  return 1;
}

// .224 DIAG: sync-object census -- one line per live sync object
// (handle, kind, name, creator tid, owner, lock count, sets,
// last setter, refs) + a total line, capped at 64 shown. Called
// once from the first gap dump, so the Sekiro spin handles are
// identified while the guest still runs. Logging under g_obj_mu
// matches the WAIT-FAST path, which logs under the same lock.
extern "C" void trt_sync_census_log(void) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  int live = 0, shown = 0;
  for (u32 idx = 0; idx < g_objects.size(); ++idx) {
    const sync_object& o = g_objects[idx];
    if (o.kind == OBJ_NONE) continue;
    ++live;
    if (shown >= 64) continue;
    ++shown;
    const u64 h = make_handle(idx, o.generation);
    char line[256];
    snprintf(line, sizeof(line),
             "sync: CENSUS handle=%llx kind=%u name=%s creator=%u owner=%u lock=%d sets=%llu lastset=%u refs=%u",
             (unsigned long long)h, (unsigned)o.kind,
             o.name.empty() ? "(anon)" : o.name.c_str(),
             o.creator_tid, o.owner_tid, o.lock_count,
             (unsigned long long)o.set_count, o.last_set_tid,
             o.handle_refs);
    trt_core_log(line);
  }
  char tl[128];
  snprintf(tl, sizeof(tl), "sync: CENSUS total=%d shown=%d", live, shown);
  trt_core_log(tl);
  // .225 DIAG: lost-wakeup pairing at first-dump time (usually too
  // early -- the spin has not started, owner=0 lock=0 -- so the live
  // pairing fires from the WAIT-FAST spin-trip instead) + the gate
  // trace for the creation-to-first-dump window.
  maybe_log_lost_wakeup(0);
  dump_gate_trace("census");
  // .226 DIAG: spin shape (exact per-handle fast counts),
  // woken-worker fate (exit vs stuck), and thread liveness
  // at dump time.
  dump_spin_summary();
  dump_post_wake_summary("POST-WAKE-FINAL");
  dump_threads_summary();
}

// --- semaphore ---
u64 TRT_STDCALL trt_CreateSemaphoreW(void* attrs, u32 initial, u32 maximum,
                                     const u16* name) {
  (void)attrs;
  const std::string key = utf16_to_utf8(name);
  std::lock_guard<std::mutex> lk(g_obj_mu);
  if (!key.empty()) {
    auto it = g_named_objects.find("S:" + key);
    if (it != g_named_objects.end()) {
      sync_object* existing = object_for_locked(it->second);
      if (existing && existing->kind == OBJ_SEMAPHORE) {
        ++existing->handle_refs;
        trt_SetLastError(183);
        return it->second;
      }
      g_named_objects.erase(it);
    }
  }
  const u64 h = alloc_object_locked(OBJ_SEMAPHORE);
  if (!h) {
    trt_SetLastError(8);
    return 0;
  }
  sync_object* o = object_for_locked(h);
  o->lock_count = (int)initial;
  o->max_count = (int)maximum;
  if (!key.empty()) {
    o->name = "S:" + key;
    g_named_objects[o->name] = h;
  }
  // .238 DIAG: name the creator of every bridged semaphore --
  // the .237 wait (main on h=0x654, type=Semaphore, infinite)
  // needs its creator named. Bridge handles are
  // 0x5B00xxxx-tagged (make_handle), so a bridged create NEVER
  // yields 0x654; if no GAP-SEM-CREATE line matches the waited
  // handle, the semaphore is ntdll-internal
  // (SleepConditionVariableSRW's wait semaphore for the host
  // CONDITION_VARIABLE backing Godot's CommandQueueMT sync CV)
  // and the signal target is the CV wake (already happening --
  // GAP-CV-WAKER), not a semaphore handle. Creator tid + guest
  // ret name the guest code that created it.
  o->creator_tid = trt_GetCurrentThreadId();
  {
    static std::atomic<unsigned> s_sem_create_n{0};
    const unsigned cn =
        s_sem_create_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cn <= 32 || (cn % 128) == 0) {
      char sl[256];
      snprintf(sl, sizeof(sl),
               "sync: GAP-SEM-CREATE tid=%u h=0x%llx init=%u max=%u name=%s ret=0x%llx n=%u",
               o->creator_tid, h, initial, maximum,
               key.empty() ? "none" : key.c_str(),
               (unsigned long long)(uintptr_t)__builtin_return_address(0),
               cn);
      trt_core_log(sl);
    }
  }
  trt_SetLastError(0);
  return h;
}

u64 TRT_STDCALL trt_CreateSemaphoreA(void* attrs, u32 initial, u32 maximum,
                                     const char* name) {
  if (!name) return trt_CreateSemaphoreW(attrs, initial, maximum, nullptr);
  const auto w = ansi_to_utf16(name);
  return trt_CreateSemaphoreW(attrs, initial, maximum, w.data());
}

u64 TRT_STDCALL trt_OpenSemaphoreW(u32 access, int inherit, const u16* name) {
  (void)access; (void)inherit;
  const std::string key = utf16_to_utf8(name);
  if (key.empty()) { trt_SetLastError(87); return 0; }
  std::lock_guard<std::mutex> lk(g_obj_mu);
  auto it = g_named_objects.find("S:" + key);
  if (it == g_named_objects.end()) {
    trt_SetLastError(2);
    return 0;
  }
  sync_object* o = object_for_locked(it->second);
  if (!o || o->kind != OBJ_SEMAPHORE) {
    trt_SetLastError(2);
    return 0;
  }
  ++o->handle_refs;
  return it->second;
}

u64 TRT_STDCALL trt_OpenSemaphoreA(u32 access, int inherit, const char* name) {
  if (!name) { trt_SetLastError(87); return 0; }
  const auto w = ansi_to_utf16(name);
  return trt_OpenSemaphoreW(access, inherit, w.data());
}

int TRT_STDCALL trt_ReleaseSemaphore(u64 handle, u32 count, u32* previous) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_SEMAPHORE) return 0;
  if (previous) *previous = (u32)o->lock_count;
  o->lock_count += (int)count;
  g_sync_cv.notify_all();
  // .238 DIAG: name what signals bridged semaphores -- the
  // releaser side of the .237 wait question. Throttled: first
  // 32 + every 128th.
  {
    static std::atomic<unsigned> s_sem_release_n{0};
    const unsigned rn =
        s_sem_release_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 32 || (rn % 128) == 0) {
      char sl[256];
      snprintf(sl, sizeof(sl),
               "sync: GAP-SEM-RELEASE tid=%u h=0x%llx count=%u prev=%u n=%u",
               trt_GetCurrentThreadId(), handle, count,
               previous ? *previous : 0, rn);
      trt_core_log(sl);
    }
  }
  return 1;
}

// Guest pointer width of the image currently executing. A PE32 guest's
// RTL_CRITICAL_SECTION is 24 bytes (DebugInfo, LockCount, RecursionCount,
// OwningThread, LockSemaphore, SpinCount) while a Win64 one is 40. Initializing
// 40 bytes into a 24-byte guest object overruns into the next guest object;
// seen live as the last static CS in OpenTTD's .data ending exactly at
// SizeOfImage, where the 16-byte overrun landed on uncommitted memory.
static std::atomic<int> g_guest_ptr_bits{64};

extern "C" void trt_set_guest_ptr_bits(int bits) {
  g_guest_ptr_bits.store(bits, std::memory_order_relaxed);
}

extern "C" int trt_cs_size_for_guest(void) {
  return g_guest_ptr_bits.load(std::memory_order_relaxed) == 32 ? 24 : 40;
}

// --- critical sections ---------------------------------------------------
// The guest object is a REAL RTL_CRITICAL_SECTION, not a private layout:
//   Win64: DebugInfo(0) LockCount(8) RecursionCount(12) OwningThread(16)
//          LockSemaphore(24) SpinCount(32)
//   PE32:  DebugInfo(0) LockCount(4) RecursionCount(8)  OwningThread(12)
//          LockSemaphore(16) SpinCount(20)
// A real CRT (ucrtbase/vcruntime140) initialises its own critical sections
// in that layout. Overlaying our fields at offset 0 made such a lock read as
// permanently held: LockCount (Windows' -1 = unowned) was seen as DebugInfo,
// and EnterCriticalSection spun on WaitOnAddress forever (measured: CPython
// 3.13 startup, 348M waits in 45 s). Use the documented offsets, and accept
// both 0 and -1 as "unowned" so bridge- and CRT-initialised objects agree.
struct cs_fields {
  u32* lock_count;
  u32* recursion;
  u8* owner;       // OwningThread, 4 bytes (PE32) or 8 bytes (Win64)
  u8* spin_ptr;    // SpinCount, 4 or 8 bytes
  int owner_bytes;
};

static cs_fields cs_of(void* cs) {
  const bool p32 = g_guest_ptr_bits.load(std::memory_order_relaxed) == 32;
  u8* b = (u8*)cs;
  cs_fields f;
  f.lock_count = (u32*)(b + (p32 ? 4 : 8));
  f.recursion = (u32*)(b + (p32 ? 8 : 12));
  f.owner = b + (p32 ? 12 : 16);
  f.spin_ptr = b + (p32 ? 20 : 32);
  f.owner_bytes = p32 ? 4 : 8;
  return f;
}

static u64 cs_owner_load(const cs_fields& f) {
  if (f.owner_bytes == 4)
    return __atomic_load_n((u32*)f.owner, __ATOMIC_RELAXED);
  return __atomic_load_n((u64*)f.owner, __ATOMIC_RELAXED);
}

static void cs_owner_store(const cs_fields& f, u64 v) {
  if (f.owner_bytes == 4)
    __atomic_store_n((u32*)f.owner, (u32)v, __ATOMIC_RELAXED);
  else
    __atomic_store_n((u64*)f.owner, v, __ATOMIC_RELAXED);
}

static u32 cs_spin_load(const cs_fields& f) {
  if (f.owner_bytes == 4)
    return __atomic_load_n((u32*)f.spin_ptr, __ATOMIC_RELAXED);
  return (u32)__atomic_load_n((u64*)f.spin_ptr, __ATOMIC_RELAXED);
}

// Zero a guest object with exactly-sized stores. Never memset: libc's vector
// routines are allowed to write past the requested length, and a guest object
// that ends on a committed-page boundary (a static CRITICAL_SECTION at the end
// of .data is the common case) then faults inside the bridge.
static void cs_zero_guest(void* cs) {
  volatile u32* w = (volatile u32*)cs;
  const int words = trt_cs_size_for_guest() / 4;  // 6 for PE32, 10 for Win64
  for (int i = 0; i < words; i++) w[i] = 0;
}

// Initialise in the documented Windows layout: LockCount = -1 (unowned),
// SpinCount = spin, everything else zero. Shared with bridge_kernel32's
// InitializeCriticalSectionAndSpinCount so both registrations agree.
extern "C" void trt_cs_init_layout(void* cs, u32 spin) {
  if (!cs) return;
  cs_zero_guest(cs);
  cs_fields f = cs_of(cs);
  __atomic_store_n(f.lock_count, 0xFFFFFFFFu, __ATOMIC_RELEASE);
  if (f.owner_bytes == 4)
    __atomic_store_n((u32*)f.spin_ptr, spin, __ATOMIC_RELAXED);
  else
    __atomic_store_n((u64*)f.spin_ptr, (u64)spin, __ATOMIC_RELAXED);
}

int TRT_STDCALL trt_InitializeCriticalSection(void* cs) {
  if (!cs) return 0;
  trt_cs_init_layout(cs, 0);
  return 1;
}

int TRT_STDCALL trt_InitializeCriticalSectionAndSpinCount2(void* cs, u32 spin) {
  if (!cs) return 0;
  trt_cs_init_layout(cs, spin);
  return 1;
}

int TRT_STDCALL trt_TryEnterCriticalSection(void* cs) {
  if (!cs) return 0;
  cs_fields f = cs_of(cs);
  const u64 me = current_tid();
  const u32 cur = __atomic_load_n(f.lock_count, __ATOMIC_ACQUIRE);
  if (cur != 0 && cur != 0xFFFFFFFFu) {
    if (cs_owner_load(f) == me) {
      __atomic_store_n(f.recursion,
                       __atomic_load_n(f.recursion, __ATOMIC_RELAXED) + 1,
                       __ATOMIC_RELAXED);
      return 1;
    }
    return 0;
  }
  u32 expected = cur;
  if (__atomic_compare_exchange_n(f.lock_count, &expected, 1u, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    cs_owner_store(f, me);
    __atomic_store_n(f.recursion, 1u, __ATOMIC_RELAXED);
    return 1;
  }
  return 0;
}

int TRT_STDCALL trt_EnterCriticalSection(void* cs) {
  if (!cs) return 0;
  cs_fields f = cs_of(cs);
  const u32 spins = cs_spin_load(f) ? cs_spin_load(f) : 256u;
  for (u32 i = 0; i < spins; ++i) {
    if (trt_TryEnterCriticalSection(cs)) return 1;
    std::this_thread::yield();
  }
  {
    // .206 diagnostic: log the first contended CS waits (first 64) so a
    // worker parked on a lock is visible immediately, not just after the
    // 3s stall probe. The [tid=] prefix identifies the waiter; owner_tid
    // names the thread holding the lock.
    static unsigned s_cs_contended_logged = 0;
    if (s_cs_contended_logged < 64) {
      ++s_cs_contended_logged;
      char cl[192];
      snprintf(cl, sizeof(cl),
               "sync: EnterCriticalSection cs=%p CONTENDED owner_tid=%u recursion=%u",
               cs, (u32)cs_owner_load(f),
               __atomic_load_n(f.recursion, __ATOMIC_RELAXED));
      trt_core_log(cl);
    }
  }
  // Contended: block on the lock word instead of Sleep(1) polling (which
  // cost ~1 ms of handoff latency plus a wakeup per millisecond per
  // waiter). The wait runs in 100 ms slices so the >3s stall probe below
  // still fires, and so a lost wake self-heals at the next slice boundary
  // instead of parking the waiter forever.
  const u32 saved_err = trt_GetLastError();
  long waited_ms = 0;
  bool stall_named = false;
  u32 one = 1;  // the held value our TryEnter writes (WaitOnAddress comparand)
  while (!trt_TryEnterCriticalSection(cs)) {
    trt_WaitOnAddress(f.lock_count, &one, 4, 100);
    waited_ms += 100;
    if (waited_ms >= 3000 && !stall_named) {
      stall_named = true;
      const u32 owner_tid = (u32)cs_owner_load(f);
      u32 owner_alive = 2;
      void* start = nullptr;
      u64 arg = 0;
      {
        std::lock_guard<std::mutex> lk(g_obj_mu);
        for (const sync_object& o : g_objects) {
          if (o.kind == OBJ_THREAD && o.tid == owner_tid) {
            owner_alive = o.alive.load(std::memory_order_acquire) ? 1 : 0;
            start = o.start_routine;
            arg = o.start_arg;
            break;
          }
        }
      }
      char line[200];
      snprintf(line, sizeof(line),
               "sync: EnterCriticalSection cs=%p STALL >3s owner_tid=%u "
               "owner_thread_alive=%u owner_routine=%p owner_arg=%p "
               "recursion=%u self_tid=%u\n",
               cs, owner_tid, owner_alive, start, (void*)(uintptr_t)arg,
               __atomic_load_n(f.recursion, __ATOMIC_RELAXED), current_tid());
#if defined(__linux__)
      ssize_t wrc = ::write(2, line, strlen(line));
      (void)wrc;
      char wl[200];
      snprintf(wl, sizeof(wl),
               "sync: wait-addr accounting waits=%llu wake_single=%llu "
               "(miss=%llu) wake_all=%llu (miss=%llu) timed_out=%llu\n",
               (unsigned long long)g_wa_waits.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_single.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_single_miss.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_all.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_wake_all_miss.load(std::memory_order_relaxed),
               (unsigned long long)g_wa_timed_out.load(std::memory_order_relaxed));
      wrc = ::write(2, wl, strlen(wl));
      (void)wrc;
#else
      trt_core_log(line);
#endif
      trt_cs_stall_probe_pause();
    }
  }
  // WaitOnAddress timeouts set the thread's last error (1460): the wait is
  // an internal detail, not the guest's error — restore it
  trt_SetLastError(saved_err);
  return 1;
}

int TRT_STDCALL trt_LeaveCriticalSection(void* cs) {
  if (!cs) return 0;
  cs_fields f = cs_of(cs);
  const u32 rec = __atomic_load_n(f.recursion, __ATOMIC_RELAXED);
  if (rec > 1) {
    __atomic_store_n(f.recursion, rec - 1, __ATOMIC_RELAXED);
    return 1;
  }
  __atomic_store_n(f.recursion, 0u, __ATOMIC_RELAXED);
  cs_owner_store(f, 0u);
  // Windows' unowned marker is -1; waiters compare against the held value 1,
  // so any change wakes them.
  __atomic_store_n(f.lock_count, 0xFFFFFFFFu, __ATOMIC_RELEASE);
  trt_WakeByAddressSingle(f.lock_count);  // wake one blocked waiter
  return 1;
}

int TRT_STDCALL trt_DeleteCriticalSection(void* cs) {
  (void)cs;
  return 1;
}

// --- QueueUserWorkItem ---
int TRT_STDCALL trt_QueueUserWorkItem(void* fn, void* ctx, u32 flags) {
  (void)flags;
  if (!fn) { trt_SetLastError(87); return 0; }
  const u64 th = trt_CreateThread(nullptr, nullptr, fn, ctx, 0, nullptr);
  if (!th) return 0;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  u32 idx = 0;
  sync_object* o = object_for_locked(th, &idx);
  if (o && o->handle_refs > 0) {
    --o->handle_refs;
    maybe_reclaim_object_locked(idx);
  }
  return 1;
}

// --- IOCP + OVERLAPPED ---
u64 TRT_STDCALL trt_CreateIoCompletionPort(void* file_handle,
                                           u64 existing_port,
                                           u64 completion_key,
                                           u32 concurrent_threads) {
  (void)concurrent_threads;
  std::lock_guard<std::mutex> lk(g_obj_mu);
  u64 port = existing_port;
  if (!port) {
    port = alloc_object_locked(OBJ_IOCP);
    if (!port) {
      trt_SetLastError(8);
      return 0;
    }
  } else {
    sync_object* po = object_for_locked(port);
    if (!po || po->kind != OBJ_IOCP) {
      trt_SetLastError(6);
      return 0;
    }
  }
  if (file_handle && file_handle != (void*)(uintptr_t)(i64)-1) {
    g_iocp_assocs[(u64)(uintptr_t)file_handle] =
        iocp_assoc{port, completion_key};
  }
  return port;
}

int TRT_STDCALL trt_PostQueuedCompletionStatus(u64 port, u32 bytes, u64 key,
                                               void* overlapped) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(port);
  if (!o || o->kind != OBJ_IOCP || o->iocp_closed) {
    trt_SetLastError(6);
    return 0;
  }
  o->iocp_queue.push_back(iocp_packet{bytes, key, overlapped});
  g_sync_cv.notify_all();
  return 1;
}

int TRT_STDCALL trt_GetQueuedCompletionStatus(u64 port, u32* bytes, u64* key,
                                              void** overlapped, u32 ms) {
  std::unique_lock<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(port);
  if (!o || o->kind != OBJ_IOCP) {
    trt_SetLastError(6);
    return 0;
  }
  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  for (;;) {
    if (!o->iocp_queue.empty()) {
      const iocp_packet pkt = o->iocp_queue.front();
      o->iocp_queue.pop_front();
      if (bytes) *bytes = pkt.bytes;
      if (key) *key = pkt.key;
      if (overlapped) *overlapped = pkt.overlapped;
      trt_SetLastError(0);
      return 1;
    }
    if (o->iocp_closed) {
      if (overlapped) *overlapped = nullptr;
      trt_SetLastError(735);
      return 0;
    }
    if (ms == 0) {
      if (overlapped) *overlapped = nullptr;
      trt_SetLastError(258);  // WAIT_TIMEOUT
      return 0;
    }
    if (ms == 0xFFFFFFFFu) {
      g_sync_cv.wait(lk);
    } else {
      g_sync_cv.wait_until(lk, deadline);
      if (std::chrono::steady_clock::now() >= deadline) {
        o = object_for_locked(port);
        if (o && !o->iocp_queue.empty()) continue;
        if (overlapped) *overlapped = nullptr;
        trt_SetLastError(258);
        return 0;
      }
    }
    o = object_for_locked(port);
    if (!o) {
      trt_SetLastError(6);
      return 0;
    }
  }
}

int TRT_STDCALL trt_GetQueuedCompletionStatusEx(u64 port, void* entries,
                                                u32 count, u32* removed,
                                                u32 ms, int alertable) {
  if (!entries || count == 0 || !removed) {
    trt_SetLastError(87);
    return 0;
  }
  *removed = 0;
  overlapped_entry_w64* out = (overlapped_entry_w64*)entries;

  std::unique_lock<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(port);
  if (!o || o->kind != OBJ_IOCP) {
    trt_SetLastError(6);
    return 0;
  }
  const u32 my_tid = current_tid();
  const auto deadline =
      (ms == 0xFFFFFFFFu)
          ? std::chrono::steady_clock::time_point::max()
          : std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

  for (;;) {
    if (!o->iocp_queue.empty()) {
      u32 n = 0;
      while (n < count && !o->iocp_queue.empty()) {
        const iocp_packet pkt = o->iocp_queue.front();
        o->iocp_queue.pop_front();
        out[n].key = pkt.key;
        out[n].overlapped = pkt.overlapped;
        out[n].internal = 0;
        out[n].bytes = pkt.bytes;
        out[n].pad = 0;
        ++n;
      }
      *removed = n;
      trt_SetLastError(0);
      return 1;
    }
    if (alertable && drain_apcs_locked(lk, my_tid)) {
      trt_SetLastError(0xC0u);  // WAIT_IO_COMPLETION
      return 0;
    }
    o = object_for_locked(port);
    if (!o) {
      trt_SetLastError(6);
      return 0;
    }
    if (!o->iocp_queue.empty()) continue;
    if (o->iocp_closed) {
      trt_SetLastError(735);
      return 0;
    }
    if (ms == 0) {
      trt_SetLastError(258);
      return 0;
    }
    if (ms == 0xFFFFFFFFu) {
      g_sync_cv.wait(lk);
    } else {
      g_sync_cv.wait_until(lk, deadline);
      if (std::chrono::steady_clock::now() >= deadline) {
        o = object_for_locked(port);
        if (o && !o->iocp_queue.empty()) continue;
        if (alertable && drain_apcs_locked(lk, my_tid)) {
          trt_SetLastError(0xC0u);
          return 0;
        }
        trt_SetLastError(258);
        return 0;
      }
    }
    o = object_for_locked(port);
    if (!o) {
      trt_SetLastError(6);
      return 0;
    }
  }
}

int TRT_STDCALL trt_GetOverlappedResultEx(void* h, void* ovl, u32* bytes,
                                          u32 ms, int alertable) {
  extern void* trt_file_os(void* h);
  if (!trt_file_os(h) || !ovl) {
    trt_SetLastError(6);
    return 0;
  }
  const u64* ov = (const u64*)ovl;
  if (ov[0] == 0x103u /*STATUS_PENDING*/) {
    if (ov[3] != 0) {
      const u32 w = alertable
                        ? trt_WaitForSingleObjectEx(ov[3], ms, alertable)
                        : trt_WaitForSingleObject(ov[3], ms);
      if (w == 0xC0u) {
        trt_SetLastError(0xC0u);  // WAIT_IO_COMPLETION
        return 0;
      }
      if (w == 0x102u) {
        trt_SetLastError(996);  // ERROR_IO_INCOMPLETE
        return 0;
      }
    } else if (alertable) {
      std::unique_lock<std::mutex> lk(g_obj_mu);
      if (drain_apcs_locked(lk, current_tid())) {
        trt_SetLastError(0xC0u);
        return 0;
      }
    } else if (ms == 0) {
      trt_SetLastError(996);
      return 0;
    }
  }
  if (bytes) *bytes = (u32)ov[1];
  return 1;
}

int TRT_STDCALL trt_GetOverlappedResult(void* h, void* ovl, u32* bytes,
                                        int wait) {
  return trt_GetOverlappedResultEx(h, ovl, bytes, wait ? 0xFFFFFFFFu : 0u, 0);
}

// --- Guest Win32 TLS ---
#if defined(_WIN32)
u32 TRT_STDCALL trt_TlsAlloc(void) {
  const DWORD index = ::TlsAlloc();
  if (index == TLS_OUT_OF_INDEXES) {
    trt_SetLastError((u32)::GetLastError());
    return 0xFFFFFFFFu;
  }
  return (u32)index;
}

void* TRT_STDCALL trt_TlsGetValue(u32 index) {
  void* value = ::TlsGetValue((DWORD)index);
  trt_SetLastError((u32)::GetLastError());
  return value;
}

int TRT_STDCALL trt_TlsSetValue(u32 index, void* value) {
  if (!::TlsSetValue((DWORD)index, value)) {
    trt_SetLastError((u32)::GetLastError());
    return 0;
  }
  return 1;
}

int TRT_STDCALL trt_TlsFree(u32 index) {
  if (!::TlsFree((DWORD)index)) {
    trt_SetLastError((u32)::GetLastError());
    return 0;
  }
  return 1;
}
#else
u32 TRT_STDCALL trt_TlsAlloc(void) {
  std::lock_guard<std::mutex> lk(g_tls_mu);
  for (u32 i = 0; i < g_tls_used.size(); ++i) {
    if (!g_tls_used[i]) {
      g_tls_used[i] = true;
      ++g_tls_gen[i];
      if (g_tls_gen[i] == 0) g_tls_gen[i] = 1;
      return i;
    }
  }
  if (g_tls_used.size() >= 65536u) {
    trt_SetLastError(8);
    return 0xFFFFFFFFu;
  }
  const u32 idx = (u32)g_tls_used.size();
  g_tls_used.push_back(true);
  g_tls_gen.push_back(1u);
  return idx;
}

void* TRT_STDCALL trt_TlsGetValue(u32 index) {
  u32 gen = 0;
  {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    if (index >= g_tls_used.size() || !g_tls_used[index]) {
      trt_SetLastError(87);
      return nullptr;
    }
    gen = g_tls_gen[index];
  }
  trt_SetLastError(0);
  if (index < 64) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) {
      void* val = *(void**)(teb + 0x1480 + (size_t)index * 8);
      if (val) return val;
    }
  } else if (index < 1088) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) {
      void** exp = *(void***)(teb + 0x1780);
      if (exp && exp[index - 64]) return exp[index - 64];
    }
  }
  if (index >= t_tls_vals.size()) return nullptr;
  if (t_tls_vals[index].first != gen) return nullptr;
  return t_tls_vals[index].second;
}

int TRT_STDCALL trt_TlsSetValue(u32 index, void* value) {
  u32 gen = 0;
  {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    if (index >= g_tls_used.size() || !g_tls_used[index]) {
      trt_SetLastError(87);
      return 0;
    }
    gen = g_tls_gen[index];
  }
  if (index >= t_tls_vals.size()) {
    t_tls_vals.resize((size_t)index + 1u, std::make_pair(0u, nullptr));
  }
  t_tls_vals[index] = std::make_pair(gen, value);
  if (index < 64) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) {
      *(void**)(teb + 0x1480 + (size_t)index * 8) = value;
    }
  } else if (index < 1088) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) {
      void** exp = *(void***)(teb + 0x1780);
      if (!exp) {
        exp = (void**)calloc(1024, sizeof(void*));
        *(void***)(teb + 0x1780) = exp;
      }
      if (exp) exp[index - 64] = value;
    }
  }
  return 1;
}

int TRT_STDCALL trt_TlsFree(u32 index) {
  std::lock_guard<std::mutex> lk(g_tls_mu);
  if (index >= g_tls_used.size() || !g_tls_used[index]) {
    trt_SetLastError(87);
    return 0;
  }
  g_tls_used[index] = false;
  ++g_tls_gen[index];
  if (g_tls_gen[index] == 0) g_tls_gen[index] = 1;
  if (index < 64) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) *(void**)(teb + 0x1480 + (size_t)index * 8) = nullptr;
  } else if (index < 1088) {
    u8* teb = (u8*)trt_guest_teb_acquire(nullptr);
    if (teb) {
      void** exp = *(void***)(teb + 0x1780);
      if (exp) exp[index - 64] = nullptr;
    }
  }
  return 1;
}

#endif

// Close a synchronisation/thread/IOCP object
int TRT_STDCALL trt_sync_close(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  u32 idx = 0;
  sync_object* o = object_for_locked(handle, &idx);
  if (!o) return 0;
  if (o->kind == OBJ_IOCP) {
    o->iocp_closed = true;
    g_sync_cv.notify_all();
  }
  if (o->handle_refs > 0) --o->handle_refs;
  maybe_reclaim_object_locked(idx);
  return 1;
}

// ============================================================================
// Fibers (ConvertThreadToFiber / CreateFiber / SwitchToFiber / DeleteFiber)
// ============================================================================
namespace {

struct guest_fiber {
  void* start = nullptr;
  void* param = nullptr;
  bool is_thread_fiber = false;
#if defined(_WIN32)
  void* os_fiber = nullptr;
#else
  ucontext_t ctx{};
  u8* stack = nullptr;
  size_t stack_bytes = 0;
#endif
};

thread_local guest_fiber* t_current_fiber = nullptr;

#if defined(_WIN32)
struct host_fiber_procs {
  using ConvertThread_t = LPVOID(WINAPI*)(LPVOID);
  using ConvertThreadEx_t = LPVOID(WINAPI*)(LPVOID, DWORD);
  using ConvertBack_t = BOOL(WINAPI*)(void);
  using CreateFiber_t = LPVOID(WINAPI*)(SIZE_T, void(WINAPI*)(LPVOID), LPVOID);
  using CreateFiberEx_t = LPVOID(WINAPI*)(SIZE_T, SIZE_T, DWORD, void(WINAPI*)(LPVOID), LPVOID);
  using DeleteFiber_t = void(WINAPI*)(LPVOID);
  using SwitchToFiber_t = void(WINAPI*)(LPVOID);
  ConvertThread_t ConvThread = nullptr;
  ConvertThreadEx_t ConvThreadEx = nullptr;
  ConvertBack_t ConvBack = nullptr;
  CreateFiber_t Create = nullptr;
  CreateFiberEx_t CreateEx = nullptr;
  DeleteFiber_t Del = nullptr;
  SwitchToFiber_t Switch = nullptr;
};
host_fiber_procs g_hf_procs;
std::once_flag g_hf_once;

void init_host_fiber_procs() {
  std::call_once(g_hf_once, []() {
    HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = ::GetModuleHandleW(L"kernel32.dll");
    if (!kb) return;
    g_hf_procs.ConvThread = (host_fiber_procs::ConvertThread_t)::GetProcAddress(kb, "ConvertThreadToFiber");
    g_hf_procs.ConvThreadEx = (host_fiber_procs::ConvertThreadEx_t)::GetProcAddress(kb, "ConvertThreadToFiberEx");
    g_hf_procs.ConvBack = (host_fiber_procs::ConvertBack_t)::GetProcAddress(kb, "ConvertFiberToThread");
    g_hf_procs.Create = (host_fiber_procs::CreateFiber_t)::GetProcAddress(kb, "CreateFiber");
    g_hf_procs.CreateEx = (host_fiber_procs::CreateFiberEx_t)::GetProcAddress(kb, "CreateFiberEx");
    g_hf_procs.Del = (host_fiber_procs::DeleteFiber_t)::GetProcAddress(kb, "DeleteFiber");
    g_hf_procs.Switch = (host_fiber_procs::SwitchToFiber_t)::GetProcAddress(kb, "SwitchToFiber");
  });
}

void WINAPI win_fiber_entry(LPVOID raw) {
  guest_fiber* gf = (guest_fiber*)raw;
  t_current_fiber = gf;
  trt_guest_tls_sync_thread();
  if (gf && gf->start) {
    win_call_guest_seh(gf->start, (u64)(uintptr_t)gf->param);
  }
  ::ExitThread(0);
}
#else
void linux_fiber_entry(u32 lo, u32 hi) {
  const uintptr_t p = (uintptr_t)(((u64)hi << 32) | (u64)lo);
  guest_fiber* gf = (guest_fiber*)p;
  t_current_fiber = gf;
  trt_guest_tls_sync_thread();
  if (gf && gf->start) {
    trt_call_guest(gf->start, (u64)(uintptr_t)gf->param, 0, 0, 0);
  }
  trt_ExitThread(0);
}
#endif

}  // namespace

extern "C" {

void* TRT_STDCALL trt_ConvertThreadToFiberEx(void* param, u32 flags) {
  (void)flags;
  if (t_current_fiber) {
    trt_SetLastError(1280);  // ERROR_ALREADY_FIBER
    return nullptr;
  }
  guest_fiber* gf = new guest_fiber();
  gf->param = param;
  gf->is_thread_fiber = true;
#if defined(_WIN32)
  init_host_fiber_procs();
  if (g_hf_procs.ConvThreadEx) gf->os_fiber = g_hf_procs.ConvThreadEx(param, flags);
  else if (g_hf_procs.ConvThread) gf->os_fiber = g_hf_procs.ConvThread(param);
  if (!gf->os_fiber) {
    delete gf;
    trt_SetLastError(50);  // ERROR_NOT_SUPPORTED
    return nullptr;
  }
#endif
  t_current_fiber = gf;
  return gf;
}

void* TRT_STDCALL trt_ConvertThreadToFiber(void* param) {
  return trt_ConvertThreadToFiberEx(param, 0);
}

int TRT_STDCALL trt_ConvertFiberToThread(void) {
  guest_fiber* gf = t_current_fiber;
  if (!gf || !gf->is_thread_fiber) {
    trt_SetLastError(1281);  // ERROR_ALREADY_THREAD
    return 0;
  }
#if defined(_WIN32)
  init_host_fiber_procs();
  if (g_hf_procs.ConvBack && !g_hf_procs.ConvBack()) return 0;
#endif
  t_current_fiber = nullptr;
  delete gf;
  return 1;
}

void* TRT_STDCALL trt_CreateFiberEx(u64 commit, u64 reserve, u32 flags,
                                    void* start, void* param) {
  (void)flags;
  if (!start) {
    trt_SetLastError(87);
    return nullptr;
  }
  guest_fiber* gf = new guest_fiber();
  gf->start = start;
  gf->param = param;
  gf->is_thread_fiber = false;
#if defined(_WIN32)
  init_host_fiber_procs();
  if (g_hf_procs.CreateEx) {
    gf->os_fiber = g_hf_procs.CreateEx((SIZE_T)commit, (SIZE_T)reserve, flags,
                                       &win_fiber_entry, gf);
  } else if (g_hf_procs.Create) {
    gf->os_fiber = g_hf_procs.Create((SIZE_T)reserve, &win_fiber_entry, gf);
  }
  if (!gf->os_fiber) {
    delete gf;
    trt_SetLastError(50);
    return nullptr;
  }
#else
  (void)commit;
  size_t ssz = (reserve >= 262144ULL) ? (size_t)reserve : 1048576ULL;
  if (ssz > 16ULL * 1024ULL * 1024ULL) ssz = 16ULL * 1024ULL * 1024ULL;
  gf->stack = (u8*)malloc(ssz);
  if (!gf->stack || getcontext(&gf->ctx) != 0) {
    free(gf->stack);
    delete gf;
    trt_SetLastError(8);
    return nullptr;
  }
  gf->stack_bytes = ssz;
  gf->ctx.uc_stack.ss_sp = gf->stack;
  gf->ctx.uc_stack.ss_size = ssz;
  gf->ctx.uc_link = nullptr;
  const uintptr_t p = (uintptr_t)gf;
  makecontext(&gf->ctx, (void(*)(void))linux_fiber_entry, 2,
              (u32)(p & 0xFFFFFFFFULL), (u32)(p >> 32));
#endif
  return gf;
}

void* TRT_STDCALL trt_CreateFiber(u64 stack_size, void* start, void* param) {
  return trt_CreateFiberEx(stack_size, stack_size, 0, start, param);
}

void TRT_STDCALL trt_DeleteFiber(void* fiber_ptr) {
  if (!fiber_ptr) return;
  guest_fiber* gf = (guest_fiber*)fiber_ptr;
  if (gf == t_current_fiber) {
    t_current_fiber = nullptr;
#if defined(_WIN32)
    void* osf = gf->os_fiber;
    delete gf;
    init_host_fiber_procs();
    if (g_hf_procs.Del && osf) g_hf_procs.Del(osf);
    ::ExitThread(0);
#else
    free(gf->stack);
    delete gf;
    trt_ExitThread(0);
#endif
  }
#if defined(_WIN32)
  init_host_fiber_procs();
  if (g_hf_procs.Del && gf->os_fiber) g_hf_procs.Del(gf->os_fiber);
#else
  free(gf->stack);
#endif
  delete gf;
}

void TRT_STDCALL trt_SwitchToFiber(void* fiber_ptr) {
  guest_fiber* cur = t_current_fiber;
  guest_fiber* next = (guest_fiber*)fiber_ptr;
  if (!cur || !next || cur == next) return;
  t_current_fiber = next;
#if defined(_WIN32)
  init_host_fiber_procs();
  if (g_hf_procs.Switch && next->os_fiber) g_hf_procs.Switch(next->os_fiber);
#else
  swapcontext(&cur->ctx, &next->ctx);
#endif
}

void* TRT_STDCALL trt_GetCurrentFiber(void) { return t_current_fiber; }
void* TRT_STDCALL trt_GetFiberData(void) {
  return t_current_fiber ? t_current_fiber->param : nullptr;
}
int TRT_STDCALL trt_IsThreadAFiber(void) {
  return t_current_fiber != nullptr ? 1 : 0;
}

}  // extern "C"

// ============================================================================
// Vista Threadpool (Work / Timer / Wait / Pool / Env / CleanupGroup)
// + RegisterWaitForSingleObject
// ============================================================================
namespace {

struct tp_callback_inst {
  void* cleanup_cs = nullptr;
  u64 cleanup_mut = 0;
  u64 cleanup_sem = 0;
  i32 cleanup_sem_cnt = 0;
  u64 cleanup_ev = 0;
};

static void tp_run_guest_cb(void* fn, u64 a, u64 b, u64 c, u64 d,
                            tp_callback_inst* inst) {
  trt_guest_tls_sync_thread();
#if defined(_WIN32)
  __try {
    trt_call_guest(fn, a, b, c, d);
  } __except (win_thread_seh_filter(GetExceptionInformation())) {
  }
#else
  trt_call_guest(fn, a, b, c, d);
#endif
  if (inst) {
    if (inst->cleanup_cs) trt_LeaveCriticalSection(inst->cleanup_cs);
    if (inst->cleanup_mut) trt_ReleaseMutex(inst->cleanup_mut);
    if (inst->cleanup_sem) trt_ReleaseSemaphore(inst->cleanup_sem, inst->cleanup_sem_cnt, nullptr);
    if (inst->cleanup_ev) trt_SetEvent(inst->cleanup_ev);
  }
}

struct tp_pool_obj {
  u32 min_threads = 1;
  u32 max_threads = 8;
};

struct tp_cleanup_group {
  std::mutex mu;
  std::vector<void*> items;
};

struct tp_work_obj {
  void* cb = nullptr;
  void* ctx = nullptr;
  std::mutex mu;
  std::condition_variable cv;
  int queued = 0;
  int active = 0;
  bool closing = false;
};

struct tp_task {
  tp_work_obj* work = nullptr;
  void* simple_cb = nullptr;
  void* simple_ctx = nullptr;
};

struct tp_runtime {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<tp_task> queue;
  int started_workers = 0;
};

tp_runtime* g_tp = nullptr;
std::once_flag g_tp_once;

void tp_worker_loop() {
  for (;;) {
    tp_task task;
    {
      std::unique_lock<std::mutex> lk(g_tp->mu);
      g_tp->cv.wait(lk, []() { return !g_tp->queue.empty(); });
      task = g_tp->queue.front();
      g_tp->queue.pop_front();
    }
    tp_callback_inst inst;
    if (task.work) {
      tp_work_obj* w = task.work;
      bool run = true;
      {
        std::lock_guard<std::mutex> wl(w->mu);
        if (w->queued > 0) --w->queued;
        if (w->closing) run = false;
        else ++w->active;
      }
      if (run && w->cb) {
        tp_run_guest_cb(w->cb, (u64)(uintptr_t)&inst, (u64)(uintptr_t)w->ctx,
                        (u64)(uintptr_t)w, 0, &inst);
      }
      bool can_delete = false;
      {
        std::lock_guard<std::mutex> wl(w->mu);
        if (run && w->active > 0) --w->active;
        if (w->queued == 0 && w->active == 0) {
          w->cv.notify_all();
          can_delete = w->closing;
        }
      }
      if (can_delete) delete w;
    } else if (task.simple_cb) {
      tp_run_guest_cb(task.simple_cb, (u64)(uintptr_t)&inst,
                      (u64)(uintptr_t)task.simple_ctx, 0, 0, &inst);
    }
  }
}

tp_runtime* tp_ensure_runtime() {
  std::call_once(g_tp_once, []() { g_tp = new tp_runtime(); });
  std::lock_guard<std::mutex> lk(g_tp->mu);
  if (g_tp->started_workers == 0) {
    for (int i = 0; i < 4; ++i) {
      std::thread(&tp_worker_loop).detach();
      ++g_tp->started_workers;
    }
  }
  return g_tp;
}

struct tp_timer_obj {
  void* cb = nullptr;
  void* ctx = nullptr;
  std::mutex mu;
  std::condition_variable cv;
  bool armed = false;
  bool cancelled = false;
  bool closing = false;
  bool running = false;
  bool thread_started = false;
  u64 generation = 0;
  i64 delay_ms = 0;
  u32 period_ms = 0;
};

void tp_timer_loop(tp_timer_obj* t) {
  for (;;) {
    std::unique_lock<std::mutex> lk(t->mu);
    t->cv.wait(lk, [&]() { return t->closing || (t->armed && !t->cancelled); });
    if (t->closing) {
      while (t->running) t->cv.wait(lk);
      delete t;
      return;
    }
    const u64 gen = t->generation;
    const i64 dms = t->delay_ms;
    const u32 pms = t->period_ms;
    if (dms > 0) {
      t->cv.wait_for(lk, std::chrono::milliseconds(dms),
                     [&]() { return t->closing || t->cancelled || t->generation != gen; });
      if (t->closing || t->cancelled || t->generation != gen) continue;
    }
    if (pms == 0) t->armed = false;
    t->running = true;
    lk.unlock();

    tp_callback_inst inst;
    if (t->cb) {
      tp_run_guest_cb(t->cb, (u64)(uintptr_t)&inst, (u64)(uintptr_t)t->ctx,
                      (u64)(uintptr_t)t, 0, &inst);
    }

    lk.lock();
    t->running = false;
    if (pms > 0 && !t->closing && !t->cancelled && t->generation == gen) {
      t->delay_ms = (i64)pms;
    } else if (pms == 0) {
      t->armed = false;
    }
    t->cv.notify_all();
  }
}

struct tp_wait_obj {
  void* cb = nullptr;
  void* ctx = nullptr;
  std::mutex mu;
  std::condition_variable cv;
  u64 wait_handle = 0;
  u32 timeout_ms = 0xFFFFFFFFu;
  bool armed = false;
  bool closing = false;
  bool running = false;
  bool thread_started = false;
  u64 generation = 0;
};

void tp_wait_loop(tp_wait_obj* w) {
  for (;;) {
    u64 h = 0;
    u32 to = 0;
    u64 gen = 0;
    {
      std::unique_lock<std::mutex> lk(w->mu);
      w->cv.wait(lk, [&]() { return w->closing || w->armed; });
      if (w->closing) {
        while (w->running) w->cv.wait(lk);
        delete w;
        return;
      }
      h = w->wait_handle;
      to = w->timeout_ms;
      gen = w->generation;
      w->armed = false;
      w->running = true;
    }
    const u32 res = h ? trt_WaitForSingleObject(h, to) : 258u /*WAIT_TIMEOUT*/;
    bool still_valid = false;
    {
      std::lock_guard<std::mutex> lk(w->mu);
      still_valid = (!w->closing && w->generation == gen);
    }
    if (still_valid && w->cb) {
      tp_callback_inst inst;
      tp_run_guest_cb(w->cb, (u64)(uintptr_t)&inst, (u64)(uintptr_t)w->ctx,
                      (u64)(uintptr_t)w, (u64)res, &inst);
    }
    {
      std::lock_guard<std::mutex> lk(w->mu);
      w->running = false;
      w->cv.notify_all();
    }
  }
}

struct legacy_reg_wait {
  u64 h = 0;
  void* cb = nullptr;
  void* ctx = nullptr;
  u32 ms = 0xFFFFFFFFu;
  u32 flags = 0;
  std::atomic<bool> stop{false};
  std::atomic<bool> finished{false};
  std::thread th;
};

}  // namespace

extern "C" {

void* TRT_STDCALL trt_CreateThreadpool(void* reserved) {
  (void)reserved;
  return new tp_pool_obj();
}
void TRT_STDCALL trt_CloseThreadpool(void* pool) { delete (tp_pool_obj*)pool; }
void TRT_STDCALL trt_SetThreadpoolThreadMaximum(void* pool, u32 max_th) {
  if (pool) ((tp_pool_obj*)pool)->max_threads = max_th ? max_th : 1u;
}
int TRT_STDCALL trt_SetThreadpoolThreadMinimum(void* pool, u32 min_th) {
  if (pool) ((tp_pool_obj*)pool)->min_threads = min_th;
  return 1;
}

void* TRT_STDCALL trt_CreateThreadpoolCleanupGroup(void) {
  return new tp_cleanup_group();
}
void TRT_STDCALL trt_CloseThreadpoolCleanupGroupMembers(void* grp, int cancel,
                                                      void* cleanup_ctx) {
  (void)grp; (void)cancel; (void)cleanup_ctx;
}
void TRT_STDCALL trt_CloseThreadpoolCleanupGroup(void* grp) {
  delete (tp_cleanup_group*)grp;
}

void TRT_STDCALL trt_InitializeThreadpoolEnvironment(void* env) {
  if (env) {
    memset(env, 0, 72);
    *(u32*)env = 3u;  // TP_ENV_VERSION
  }
}
void TRT_STDCALL trt_DestroyThreadpoolEnvironment(void* env) {
  if (env) memset(env, 0, 72);
}
void TRT_STDCALL trt_SetThreadpoolCallbackPool(void* env, void* pool) {
  if (env) *(void**)((u8*)env + 8) = pool;
}
void TRT_STDCALL trt_SetThreadpoolCallbackCleanupGroup(void* env, void* grp,
                                                       void* cancel_cb) {
  if (env) {
    *(void**)((u8*)env + 16) = grp;
    *(void**)((u8*)env + 24) = cancel_cb;
  }
}
void TRT_STDCALL trt_SetThreadpoolCallbackRunsLong(void* env) {
  if (env) *((u32*)env + 12) |= 1u;
}

void* TRT_STDCALL trt_CreateThreadpoolWork(void* pfn, void* ctx, void* env) {
  (void)env;
  if (!pfn) { trt_SetLastError(87); return nullptr; }
  tp_work_obj* w = new tp_work_obj();
  w->cb = pfn;
  w->ctx = ctx;
  return w;
}

void TRT_STDCALL trt_SubmitThreadpoolWork(void* work_ptr) {
  tp_work_obj* w = (tp_work_obj*)work_ptr;
  if (!w) return;
  tp_runtime* rt = tp_ensure_runtime();
  {
    std::lock_guard<std::mutex> wl(w->mu);
    if (w->closing) return;
    ++w->queued;
  }
  {
    std::lock_guard<std::mutex> ql(rt->mu);
    rt->queue.push_back({w, nullptr, nullptr});
  }
  rt->cv.notify_one();
}

void TRT_STDCALL trt_WaitForThreadpoolWorkCallbacks(void* work_ptr,
                                                    int cancel_pending) {
  tp_work_obj* w = (tp_work_obj*)work_ptr;
  if (!w) return;
  if (cancel_pending && g_tp) {
    std::lock_guard<std::mutex> ql(g_tp->mu);
    for (auto it = g_tp->queue.begin(); it != g_tp->queue.end();) {
      if (it->work == w) {
        it = g_tp->queue.erase(it);
        std::lock_guard<std::mutex> wl(w->mu);
        if (w->queued > 0) --w->queued;
      } else {
        ++it;
      }
    }
  }
  std::unique_lock<std::mutex> lk(w->mu);
  w->cv.wait(lk, [&]() { return w->queued == 0 && w->active == 0; });
}

void TRT_STDCALL trt_CloseThreadpoolWork(void* work_ptr) {
  tp_work_obj* w = (tp_work_obj*)work_ptr;
  if (!w) return;
  bool del_now = false;
  {
    std::lock_guard<std::mutex> lk(w->mu);
    w->closing = true;
    del_now = (w->queued == 0 && w->active == 0);
  }
  if (del_now) delete w;
}

int TRT_STDCALL trt_TrySubmitThreadpoolCallback(void* pfn, void* ctx, void* env) {
  (void)env;
  if (!pfn) { trt_SetLastError(87); return 0; }
  tp_runtime* rt = tp_ensure_runtime();
  {
    std::lock_guard<std::mutex> ql(rt->mu);
    rt->queue.push_back({nullptr, pfn, ctx});
  }
  rt->cv.notify_one();
  return 1;
}

void* TRT_STDCALL trt_CreateThreadpoolTimer(void* pfn, void* ctx, void* env) {
  (void)env;
  if (!pfn) { trt_SetLastError(87); return nullptr; }
  tp_timer_obj* t = new tp_timer_obj();
  t->cb = pfn;
  t->ctx = ctx;
  return t;
}

void TRT_STDCALL trt_SetThreadpoolTimer(void* timer_ptr, const i64* due_time,
                                        u32 period_ms, u32 window_len) {
  (void)window_len;
  tp_timer_obj* t = (tp_timer_obj*)timer_ptr;
  if (!t) return;
  std::lock_guard<std::mutex> lk(t->mu);
  ++t->generation;
  if (!due_time) {
    t->armed = false;
    t->cancelled = true;
    t->period_ms = 0;
    t->cv.notify_all();
    return;
  }
  i64 dms = 0;
  const i64 ft = *due_time;
  if (ft < 0) {
    dms = (-ft) / 10000LL;
  } else if (ft > 0) {
    u64 now_ft = 0;
    extern int TRT_STDCALL trt_GetSystemTimeAsFileTime(u64*);
    trt_GetSystemTimeAsFileTime(&now_ft);
    dms = (ft > (i64)now_ft) ? ((ft - (i64)now_ft) / 10000LL) : 0;
  }
  t->delay_ms = dms;
  t->period_ms = period_ms;
  t->cancelled = false;
  t->armed = true;
  if (!t->thread_started) {
    t->thread_started = true;
    std::thread(&tp_timer_loop, t).detach();
  }
  t->cv.notify_all();
}

int TRT_STDCALL trt_SetThreadpoolTimerEx(void* timer_ptr, const i64* due_time,
                                         u32 period_ms, u32 window_len) {
  bool was_set = false;
  if (timer_ptr) {
    std::lock_guard<std::mutex> lk(((tp_timer_obj*)timer_ptr)->mu);
    was_set = ((tp_timer_obj*)timer_ptr)->armed;
  }
  trt_SetThreadpoolTimer(timer_ptr, due_time, period_ms, window_len);
  return was_set ? 1 : 0;
}

int TRT_STDCALL trt_IsThreadpoolTimerSet(void* timer_ptr) {
  if (!timer_ptr) return 0;
  std::lock_guard<std::mutex> lk(((tp_timer_obj*)timer_ptr)->mu);
  return ((tp_timer_obj*)timer_ptr)->armed ? 1 : 0;
}

void TRT_STDCALL trt_WaitForThreadpoolTimerCallbacks(void* timer_ptr,
                                                     int cancel_pending) {
  tp_timer_obj* t = (tp_timer_obj*)timer_ptr;
  if (!t) return;
  std::unique_lock<std::mutex> lk(t->mu);
  if (cancel_pending) {
    t->armed = false;
    t->cancelled = true;
    t->cv.notify_all();
  }
  t->cv.wait(lk, [&]() { return !t->running && (!t->armed || cancel_pending); });
}

void TRT_STDCALL trt_CloseThreadpoolTimer(void* timer_ptr) {
  tp_timer_obj* t = (tp_timer_obj*)timer_ptr;
  if (!t) return;
  bool spawned = false;
  {
    std::lock_guard<std::mutex> lk(t->mu);
    t->armed = false;
    t->cancelled = true;
    t->closing = true;
    spawned = t->thread_started;
    t->cv.notify_all();
  }
  if (!spawned) delete t;
}

void* TRT_STDCALL trt_CreateThreadpoolWait(void* pfn, void* ctx, void* env) {
  (void)env;
  if (!pfn) { trt_SetLastError(87); return nullptr; }
  tp_wait_obj* w = new tp_wait_obj();
  w->cb = pfn;
  w->ctx = ctx;
  return w;
}

void TRT_STDCALL trt_SetThreadpoolWait(void* wait_ptr, u64 handle,
                                       const i64* timeout_ft) {
  tp_wait_obj* w = (tp_wait_obj*)wait_ptr;
  if (!w) return;
  std::lock_guard<std::mutex> lk(w->mu);
  ++w->generation;
  if (!handle) {
    w->armed = false;
    w->wait_handle = 0;
    w->cv.notify_all();
    return;
  }
  u32 ms = 0xFFFFFFFFu;
  if (timeout_ft) {
    const i64 ft = *timeout_ft;
    if (ft == 0) ms = 0;
    else if (ft < 0) ms = (u32)((-ft) / 10000LL);
    else ms = 0;
  }
  w->wait_handle = handle;
  w->timeout_ms = ms;
  w->armed = true;
  if (!w->thread_started) {
    w->thread_started = true;
    std::thread(&tp_wait_loop, w).detach();
  }
  w->cv.notify_all();
}

int TRT_STDCALL trt_SetThreadpoolWaitEx(void* wait_ptr, u64 handle,
                                        const i64* timeout_ft, void* res) {
  (void)res;
  trt_SetThreadpoolWait(wait_ptr, handle, timeout_ft);
  return 1;
}

void TRT_STDCALL trt_WaitForThreadpoolWaitCallbacks(void* wait_ptr,
                                                    int cancel_pending) {
  tp_wait_obj* w = (tp_wait_obj*)wait_ptr;
  if (!w) return;
  std::unique_lock<std::mutex> lk(w->mu);
  if (cancel_pending) {
    ++w->generation;
    w->armed = false;
    w->cv.notify_all();
  }
  w->cv.wait(lk, [&]() { return !w->running && (!w->armed || cancel_pending); });
}

void TRT_STDCALL trt_CloseThreadpoolWait(void* wait_ptr) {
  tp_wait_obj* w = (tp_wait_obj*)wait_ptr;
  if (!w) return;
  bool spawned = false;
  {
    std::lock_guard<std::mutex> lk(w->mu);
    ++w->generation;
    w->armed = false;
    w->closing = true;
    spawned = w->thread_started;
    w->cv.notify_all();
  }
  if (!spawned) delete w;
}

int TRT_STDCALL trt_CallbackMayRunLong(void* inst) {
  (void)inst;
  return 1;
}
void TRT_STDCALL trt_DisassociateCurrentThreadFromCallback(void* inst) {
  (void)inst;
}
void TRT_STDCALL trt_LeaveCriticalSectionWhenCallbackReturns(void* inst, void* cs) {
  if (inst) ((tp_callback_inst*)inst)->cleanup_cs = cs;
}
void TRT_STDCALL trt_ReleaseMutexWhenCallbackReturns(void* inst, u64 mut) {
  if (inst) ((tp_callback_inst*)inst)->cleanup_mut = mut;
}
void TRT_STDCALL trt_ReleaseSemaphoreWhenCallbackReturns(void* inst, u64 sem,
                                                         i32 cnt) {
  if (inst) {
    ((tp_callback_inst*)inst)->cleanup_sem = sem;
    ((tp_callback_inst*)inst)->cleanup_sem_cnt = cnt;
  }
}
void TRT_STDCALL trt_SetEventWhenCallbackReturns(void* inst, u64 ev) {
  if (inst) ((tp_callback_inst*)inst)->cleanup_ev = ev;
}

int TRT_STDCALL trt_RegisterWaitForSingleObject(void** out_wait, u64 h,
                                              void* cb, void* ctx, u32 ms,
                                              u32 flags) {
  if (!out_wait || !cb) { trt_SetLastError(87); return 0; }
  auto* rw = new legacy_reg_wait();
  rw->h = h;
  rw->cb = cb;
  rw->ctx = ctx;
  rw->ms = ms;
  rw->flags = flags;
  rw->th = std::thread([rw]() {
    while (!rw->stop.load(std::memory_order_acquire)) {
      const u32 step = (rw->ms == 0xFFFFFFFFu || rw->ms > 20u) ? 20u : rw->ms;
      const u32 res = trt_WaitForSingleObject(rw->h, step);
      if (rw->stop.load(std::memory_order_acquire)) break;
      if (res == 0 || rw->ms <= 20u) {
        const u64 timed_out = (res == 258u) ? 1ULL : 0ULL;
        tp_run_guest_cb(rw->cb, (u64)(uintptr_t)rw->ctx, timed_out, 0, 0,
                        nullptr);
        if (rw->flags & 0x08u /*WT_EXECUTEONLYONCE*/) break;
        if (rw->ms != 0xFFFFFFFFu && res == 258u) continue;
      }
    }
    rw->finished.store(true, std::memory_order_release);
  });
  *out_wait = rw;
  return 1;
}

void* TRT_STDCALL trt_RegisterWaitForSingleObjectEx(u64 h, void* cb, void* ctx,
                                                    u32 ms, u32 flags) {
  void* out = nullptr;
  trt_RegisterWaitForSingleObject(&out, h, cb, ctx, ms, flags);
  return out;
}

int TRT_STDCALL trt_UnregisterWaitEx(void* wait_ptr, u64 comp_event) {
  legacy_reg_wait* rw = (legacy_reg_wait*)wait_ptr;
  if (!rw) return 1;
  rw->stop.store(true, std::memory_order_release);
  if (rw->th.joinable()) {
    if (comp_event == 0) {
      rw->th.detach();
      return 1;
    }
    rw->th.join();
  }
  if (comp_event && comp_event != (u64)(intptr_t)-1) trt_SetEvent(comp_event);
  delete rw;
  return 1;
}

int TRT_STDCALL trt_UnregisterWait(void* wait_ptr) {
  return trt_UnregisterWaitEx(wait_ptr, 0);
}

}  // extern "C"

int trt_bridge_register_threads(void) {
  static const trt_named_fn k_fns[] = {
      {"ConvertThreadToFiber", (void*)&trt_ConvertThreadToFiber},
      {"ConvertThreadToFiberEx", (void*)&trt_ConvertThreadToFiberEx},
      {"ConvertFiberToThread", (void*)&trt_ConvertFiberToThread},
      {"CreateFiber", (void*)&trt_CreateFiber},
      {"CreateFiberEx", (void*)&trt_CreateFiberEx},
      {"DeleteFiber", (void*)&trt_DeleteFiber},
      {"SwitchToFiber", (void*)&trt_SwitchToFiber},
      {"GetCurrentFiber", (void*)&trt_GetCurrentFiber},
      {"GetFiberData", (void*)&trt_GetFiberData},
      {"IsThreadAFiber", (void*)&trt_IsThreadAFiber},
      {"CreateThreadpool", (void*)&trt_CreateThreadpool},
      {"CloseThreadpool", (void*)&trt_CloseThreadpool},
      {"SetThreadpoolThreadMaximum", (void*)&trt_SetThreadpoolThreadMaximum},
      {"SetThreadpoolThreadMinimum", (void*)&trt_SetThreadpoolThreadMinimum},
      {"CreateThreadpoolCleanupGroup", (void*)&trt_CreateThreadpoolCleanupGroup},
      {"CloseThreadpoolCleanupGroupMembers",
       (void*)&trt_CloseThreadpoolCleanupGroupMembers},
      {"CloseThreadpoolCleanupGroup", (void*)&trt_CloseThreadpoolCleanupGroup},
      {"InitializeThreadpoolEnvironment",
       (void*)&trt_InitializeThreadpoolEnvironment},
      {"DestroyThreadpoolEnvironment", (void*)&trt_DestroyThreadpoolEnvironment},
      {"SetThreadpoolCallbackPool", (void*)&trt_SetThreadpoolCallbackPool},
      {"SetThreadpoolCallbackCleanupGroup",
       (void*)&trt_SetThreadpoolCallbackCleanupGroup},
      {"SetThreadpoolCallbackRunsLong",
       (void*)&trt_SetThreadpoolCallbackRunsLong},
      {"CreateThreadpoolWork", (void*)&trt_CreateThreadpoolWork},
      {"SubmitThreadpoolWork", (void*)&trt_SubmitThreadpoolWork},
      {"WaitForThreadpoolWorkCallbacks",
       (void*)&trt_WaitForThreadpoolWorkCallbacks},
      {"CloseThreadpoolWork", (void*)&trt_CloseThreadpoolWork},
      {"TrySubmitThreadpoolCallback", (void*)&trt_TrySubmitThreadpoolCallback},
      {"CreateThreadpoolTimer", (void*)&trt_CreateThreadpoolTimer},
      {"SetThreadpoolTimer", (void*)&trt_SetThreadpoolTimer},
      {"SetThreadpoolTimerEx", (void*)&trt_SetThreadpoolTimerEx},
      {"IsThreadpoolTimerSet", (void*)&trt_IsThreadpoolTimerSet},
      {"WaitForThreadpoolTimerCallbacks",
       (void*)&trt_WaitForThreadpoolTimerCallbacks},
      {"CloseThreadpoolTimer", (void*)&trt_CloseThreadpoolTimer},
      {"CreateThreadpoolWait", (void*)&trt_CreateThreadpoolWait},
      {"SetThreadpoolWait", (void*)&trt_SetThreadpoolWait},
      {"SetThreadpoolWaitEx", (void*)&trt_SetThreadpoolWaitEx},
      {"WaitForThreadpoolWaitCallbacks",
       (void*)&trt_WaitForThreadpoolWaitCallbacks},
      {"CloseThreadpoolWait", (void*)&trt_CloseThreadpoolWait},
      {"CallbackMayRunLong", (void*)&trt_CallbackMayRunLong},
      {"DisassociateCurrentThreadFromCallback",
       (void*)&trt_DisassociateCurrentThreadFromCallback},
      {"LeaveCriticalSectionWhenCallbackReturns",
       (void*)&trt_LeaveCriticalSectionWhenCallbackReturns},
      {"ReleaseMutexWhenCallbackReturns",
       (void*)&trt_ReleaseMutexWhenCallbackReturns},
      {"ReleaseSemaphoreWhenCallbackReturns",
       (void*)&trt_ReleaseSemaphoreWhenCallbackReturns},
      {"SetEventWhenCallbackReturns", (void*)&trt_SetEventWhenCallbackReturns},
      {"RegisterWaitForSingleObject", (void*)&trt_RegisterWaitForSingleObject},
      {"RegisterWaitForSingleObjectEx",
       (void*)&trt_RegisterWaitForSingleObjectEx},
      {"UnregisterWait", (void*)&trt_UnregisterWait},
      {"UnregisterWaitEx", (void*)&trt_UnregisterWaitEx},
      {"CreateThread", (void*)&trt_CreateThread},
      {"ExitThread", (void*)&trt_ExitThread},
      {"TerminateThread", (void*)&trt_TerminateThread},
      {"WaitForSingleObject", (void*)&trt_WaitForSingleObject},
      {"WaitForSingleObjectEx", (void*)&trt_WaitForSingleObjectEx},
      {"WaitForMultipleObjects", (void*)&trt_WaitForMultipleObjects},
      {"WaitForMultipleObjectsEx", (void*)&trt_WaitForMultipleObjectsEx},
      {"SleepEx", (void*)&trt_SleepEx},
      {"SwitchToThread", (void*)&trt_SwitchToThread},
      {"GetExitCodeThread", (void*)&trt_GetExitCodeThread},
      {"GetCurrentThread", (void*)&trt_GetCurrentThread},
      {"GetThreadId", (void*)&trt_GetThreadId},
      {"ResumeThread", (void*)&trt_ResumeThread},
      {"SuspendThread", (void*)&trt_SuspendThread},
      {"GetThreadContext", (void*)&trt_GetThreadContext},
      {"SetThreadContext", (void*)&trt_SetThreadContext},
      {"OpenThread", (void*)&trt_OpenThread},
      {"SetThreadPriority", (void*)&trt_SetThreadPriority},
      {"GetThreadPriority", (void*)&trt_GetThreadPriority},
      {"QueueUserAPC", (void*)&trt_QueueUserAPC},
      {"QueueUserAPC2", (void*)&trt_QueueUserAPC2},
      {"QueueUserWorkItem", (void*)&trt_QueueUserWorkItem},
      {"CreateEventW", (void*)&trt_CreateEventW},
      {"CreateEventA", (void*)&trt_CreateEventA},
      {"OpenEventW", (void*)&trt_OpenEventW},
      {"OpenEventA", (void*)&trt_OpenEventA},
      {"SetEvent", (void*)&trt_SetEvent},
      {"ResetEvent", (void*)&trt_ResetEvent},
      {"PulseEvent", (void*)&trt_PulseEvent},
      {"CreateMutexW", (void*)&trt_CreateMutexW},
      {"CreateMutexA", (void*)&trt_CreateMutexA},
      {"OpenMutexW", (void*)&trt_OpenMutexW},
      {"OpenMutexA", (void*)&trt_OpenMutexA},
      {"ReleaseMutex", (void*)&trt_ReleaseMutex},
      {"CreateSemaphoreW", (void*)&trt_CreateSemaphoreW},
      {"CreateSemaphoreA", (void*)&trt_CreateSemaphoreA},
      {"OpenSemaphoreW", (void*)&trt_OpenSemaphoreW},
      {"OpenSemaphoreA", (void*)&trt_OpenSemaphoreA},
      {"ReleaseSemaphore", (void*)&trt_ReleaseSemaphore},
      {"InitializeCriticalSection", (void*)&trt_InitializeCriticalSection},
      {"InitializeCriticalSectionAndSpinCount",
       (void*)&trt_InitializeCriticalSectionAndSpinCount2},
      {"EnterCriticalSection", (void*)&trt_EnterCriticalSection},
      {"TryEnterCriticalSection", (void*)&trt_TryEnterCriticalSection},
      {"LeaveCriticalSection", (void*)&trt_LeaveCriticalSection},
      {"DeleteCriticalSection", (void*)&trt_DeleteCriticalSection},
      {"CreateIoCompletionPort", (void*)&trt_CreateIoCompletionPort},
      {"PostQueuedCompletionStatus", (void*)&trt_PostQueuedCompletionStatus},
      {"GetQueuedCompletionStatus", (void*)&trt_GetQueuedCompletionStatus},
      {"GetQueuedCompletionStatusEx", (void*)&trt_GetQueuedCompletionStatusEx},
      {"GetOverlappedResult", (void*)&trt_GetOverlappedResult},
      {"GetOverlappedResultEx", (void*)&trt_GetOverlappedResultEx},
      {"TlsAlloc", (void*)&trt_TlsAlloc},
      {"TlsGetValue", (void*)&trt_TlsGetValue},
      {"TlsSetValue", (void*)&trt_TlsSetValue},
      {"TlsFree", (void*)&trt_TlsFree},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

// ---- process-handle surface (used by bridge_kernel32's CreateProcessW) ----
u64 trt_process_object_alloc(void) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  return alloc_object_locked(OBJ_THREAD);
}

void trt_process_object_signal(u64 h) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(h);
  if (o) {
    o->alive.store(false, std::memory_order_release);
    o->signalled = true;
    g_sync_cv.notify_all();
  }
}

} // extern "C"
