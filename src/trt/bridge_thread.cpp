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
  o.handle_refs = 1;
  return make_handle(idx, next_gen);
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
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    snprintf(line, sizeof(line),
             "guest fault [worker tid=%u]: 0x%08lx at %s, %s address %p",
             current_tid(), (unsigned long)record.ExceptionCode, where,
             kind == 0 ? "read" : kind == 1 ? "write" : "execute",
             (void*)record.ExceptionInformation[1]);
  } else {
    snprintf(line, sizeof(line),
             "guest fault [worker tid=%u]: 0x%08lx at %s",
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

static int win_thread_seh_filter(void* ep) {
  if (trt_dispatch_win32_exception(ep) == -1) return EXCEPTION_CONTINUE_EXECUTION;
  log_worker_fault(ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

static u64 win_call_guest_seh(void* routine, u64 arg) {
  __try {
    return trt_call_guest(routine, arg, 0, 0, 0);
  } __except (win_thread_seh_filter(GetExceptionInformation())) {
    return (u64)(u32)GetExceptionCode();
  }
}

DWORD WINAPI win_thread_trampoline(LPVOID raw) {
  const u64 obj_h = (u64)(uintptr_t)raw;

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

  trt_guest_tls_sync_thread();
  trt_images_notify_thread(2 /*DLL_THREAD_ATTACH*/);
  {
    char tline[96];
    snprintf(tline, sizeof(tline), "thread: win_thread_trampoline started obj=%llu tid=%u routine=%p",
             (unsigned long long)obj_h, my_tid, routine);
    trt_core_log(tline);
  }
  const u64 ret = win_call_guest_seh(routine, arg);
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
  const u64 obj_h = (u64)(uintptr_t)raw;

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
    char hline[96];
    snprintf(hline, sizeof(hline),
             "thread: guest-tid=%u mapped to host_tid=%d", my_tid,
             (int)syscall(SYS_gettid));
    trt_core_log(hline);
  }
#endif

  trt_guest_tls_sync_thread();
  trt_images_notify_thread(2 /*DLL_THREAD_ATTACH*/);
  u64 ret = 0;
  if (routine) ret = trt_call_guest(routine, arg, 0, 0, 0);
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

  const bool suspended = (flags & 0x4u /*CREATE_SUSPENDED*/) != 0;
  const u32 child_tid = trt_alloc_guest_thread_id();
  {
    std::lock_guard<std::mutex> lk(g_obj_mu);
    obj = alloc_object_locked(OBJ_THREAD);
    if (!obj) {
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
    snprintf(tline, sizeof(tline), "thread: CreateThread tid=%u routine=%p stack=%p flags=0x%x",
             child_tid, routine, stack_size, flags);
    trt_core_log(tline);
  }

  // the new thread's stack reserve is VA for the thread's lifetime (the
  // requested stack_size, else the 1 MB Windows default / 8 MB POSIX
  // pthread default): charge BEFORE creating the thread so a VA refusal
  // fails the creation instead of leaking an uncharged thread
#if defined(_WIN32)
  const u64 host_stack = ((uintptr_t)stack_size < 1024ull * 1024ull)
                             ? 0
                             : (SIZE_T)stack_size;
  const u64 stack_va = (stack_size && (uintptr_t)stack_size >= 1024ull * 1024ull)
                           ? (u64)(uintptr_t)stack_size
                           : 2ull * 1024ull * 1024ull;
#else
  const u64 stack_va = 8ull * 1024ull * 1024ull;  // pthread default stack
#endif
  if (trt_mem_budget_charge_va(stack_va) != 0) {
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
  HANDLE t = ::CreateThread(nullptr, (SIZE_T)host_stack, &win_thread_trampoline,
                            (LPVOID)(uintptr_t)obj, suspended ? CREATE_SUSPENDED : 0u, &win_tid);
  if (!t) {
    const DWORD err = GetLastError();
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
      o->tid = win_tid;
    }
  }
  if (tid) *tid = (u32)win_tid;
#else
  (void)stack_size;
  ensure_sigusr2_installed();
  pthread_t th;
  if (pthread_create(&th, nullptr, &thread_trampoline, (void*)(uintptr_t)obj) != 0) {
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
  notify_thread_exit_once();
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
  pthread_exit((void*)(uintptr_t)code);
#endif
  return code;
}

int TRT_STDCALL trt_TerminateThread(u64 thread, u32 code) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(thread);
  if (!o || o->kind != OBJ_THREAD) return 0;
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

static u32 wait_single_impl(u64 handle, u32 ms, bool alertable) {
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
      const DWORD r = ::WaitForSingleObject(win_thread_h, ms);
      if (r == WAIT_OBJECT_0) return 0;
      if (r == WAIT_ABANDONED) return 0x80u;
      return 0x102u;
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
    return 0;
  }
  if (alertable && drain_apcs_locked(lk, my_tid)) return 0xC0u;
  if (ms == 0) return 0x102u;

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
u32 TRT_STDCALL trt_WaitForSingleObject(u64 handle, u32 ms) {
  return wait_single_impl(handle, ms, false);
}

u32 TRT_STDCALL trt_WaitForSingleObjectEx(u64 handle, u32 ms, int alertable) {
  return wait_single_impl(handle, ms, alertable != 0);
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
  const DWORD res = ::ResumeThread(h);
  char rline[96];
  snprintf(rline, sizeof(rline), "kernel32: ResumeThread(obj=%llu) -> prev_count=%lu", (unsigned long long)thread, (unsigned long)res);
  trt_core_log(rline);
  return (u32)res;
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
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) return 0;
  o->signalled = true;
  g_sync_cv.notify_all();
  return 1;
}

int TRT_STDCALL trt_ResetEvent(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) return 0;
  o->signalled = false;
  return 1;
}

int TRT_STDCALL trt_PulseEvent(u64 handle) {
  std::lock_guard<std::mutex> lk(g_obj_mu);
  sync_object* o = object_for_locked(handle);
  if (!o || o->kind != OBJ_EVENT) return 0;
  o->signalled = true;
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
  if (o->lock_count == 0) {
    o->owner_tid = 0;
    g_sync_cv.notify_all();
  }
  return 1;
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
  return 1;
}

// --- critical sections (fits inside 40-byte Win64 RTL_CRITICAL_SECTION) ---
struct crit_section {
  u32 locked;
  u32 recursion;
  u32 owner;
  u32 spin;
};

int TRT_STDCALL trt_InitializeCriticalSection(void* cs) {
  if (!cs) return 0;
  memset(cs, 0, sizeof(crit_section));
  return 1;
}

int TRT_STDCALL trt_InitializeCriticalSectionAndSpinCount2(void* cs, u32 spin) {
  if (!cs) return 0;
  memset(cs, 0, sizeof(crit_section));
  ((crit_section*)cs)->spin = spin;
  return 1;
}

int TRT_STDCALL trt_TryEnterCriticalSection(void* cs) {
  if (!cs) return 0;
  crit_section* c = (crit_section*)cs;
  const u32 me = current_tid();
  if (__atomic_load_n(&c->locked, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&c->owner, __ATOMIC_RELAXED) == me) {
    ++c->recursion;
    return 1;
  }
  u32 expected = 0;
  if (__atomic_compare_exchange_n(&c->locked, &expected, 1, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_store_n(&c->owner, me, __ATOMIC_RELAXED);
    c->recursion = 1;
    return 1;
  }
  return 0;
}

int TRT_STDCALL trt_EnterCriticalSection(void* cs) {
  if (!cs) return 0;
  crit_section* c = (crit_section*)cs;
  const u32 spins = c->spin ? c->spin : 256u;
  for (u32 i = 0; i < spins; ++i) {
    if (trt_TryEnterCriticalSection(cs)) return 1;
    std::this_thread::yield();
  }
  // Contended: block on the lock word instead of Sleep(1) polling (which
  // cost ~1 ms of handoff latency plus a wakeup per millisecond per
  // waiter). The wait runs in 100 ms slices so the >3s stall probe below
  // still fires, and so a lost wake self-heals at the next slice boundary
  // instead of parking the waiter forever.
  const u32 saved_err = trt_GetLastError();
  long waited_ms = 0;
  bool stall_named = false;
  u32 one = 1;  // the locked word's held value (WaitOnAddress comparand)
  while (!trt_TryEnterCriticalSection(cs)) {
    trt_WaitOnAddress(&c->locked, &one, 4, 100);
    waited_ms += 100;
    if (waited_ms >= 3000 && !stall_named) {
      stall_named = true;
      u32 owner_alive = 2;
      void* start = nullptr;
      u64 arg = 0;
      {
        std::lock_guard<std::mutex> lk(g_obj_mu);
        for (const sync_object& o : g_objects) {
          if (o.kind == OBJ_THREAD && o.tid == c->owner) {
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
               cs, c->owner, owner_alive, start, (void*)(uintptr_t)arg,
               c->recursion, current_tid());
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
  crit_section* c = (crit_section*)cs;
  if (c->recursion > 1) {
    --c->recursion;
    return 1;
  }
  c->recursion = 0;
  __atomic_store_n(&c->owner, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&c->locked, 0u, __ATOMIC_RELEASE);
  trt_WakeByAddressSingle(&c->locked);  // wake one blocked waiter
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

// --- Uncapped generation-safe Win32 TLS ---
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
  return 1;
}

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
