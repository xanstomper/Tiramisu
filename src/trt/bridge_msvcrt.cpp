// tiramisu/src/trt/bridge_msvcrt.cpp
// MSVCRT (msvcrt.dll) C-runtime bridge for MinGW / GCC / legacy Win64 PEs
// (such as Godot / SDL / custom C++ games importing msvcrt.dll).
//
// Translates Win64 C-runtime semantics cleanly:
//   - __iob_func() 48-byte Win64 FILE array + VFS-routed FILE*/fd table
//   - Windows paths (Z:\, C:\) through trt_vfs_resolve_utf16_rw
//   - Win64 CRT globals (__initenv, _commode, _fmode, _timezone, _tzname)
//   - _setjmp / longjmp Win64 register+XMM ABI (x86_64)
//   - __builtin_ms_va_list formatting (_vsnprintf, _vsnprintf_s, _vscprintf,
//     fprintf, vfprintf, fwprintf) with %ls / %s Windows width rules
//   - Guest-ABI callbacks for _initterm, qsort, bsearch, atexit, _beginthreadex
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <setjmp.h>
#else
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include "bridge_msvcrt.h"
#include "thunk.h"
#include "trt.h"

// Settings knob (memory_budget_mb); weak symbol keeps offline tests green
// when src/host/settings.cpp is not in the test binary. Missing definition
// resolves to 0 -> crt_malloc keeps the 256 MB default.
extern "C" int trt_settings_memory_budget_mb(void) __attribute__((weak));

// 5 GB memory-budget governor (bridge_membudget.cpp): the CRT malloc family
// used to be a raw pass-through to host malloc — real committed memory the
// ledger never saw. Every guest CRT allocation charges commit + VA now.
extern "C" int trt_mem_budget_charge_commit(u64 bytes);
extern "C" void trt_mem_budget_discharge_commit(u64 bytes);
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mutex>
#include <unordered_map>

namespace {

// ---------- kernel32 resolution helpers -----------------------------------
void* k32_sym(const char* name) {
  return trt_resolve_module(nullptr, "kernel32.dll", name, 0);
}

typedef void  (TRT_GUEST_ABI* pfn_ExitProcess)(u32);
typedef void  (TRT_GUEST_ABI* pfn_ExitThread)(u32);
typedef void* (TRT_GUEST_ABI* pfn_CreateThread)(void*, u64, void*, void*, u32, u32*);
typedef u32   (TRT_GUEST_ABI* pfn_GetCurrentProcessId)(void);
typedef u32   (TRT_GUEST_ABI* pfn_GetEnvA)(const char*, char*, u32);
typedef i32   (TRT_GUEST_ABI* pfn_SetEnvA)(const char*, const char*);
typedef u32   (TRT_GUEST_ABI* pfn_GetCurrentDirW)(u32, u16*);
typedef i32   (TRT_GUEST_ABI* pfn_SetCurrentDirW)(const u16*);

// ---------- TLS errno & CRT global data -----------------------------------
#if defined(_WIN32)
__declspec(thread) i32 t_msvcrt_errno = 0;
#else
__thread i32 t_msvcrt_errno = 0;
#endif

char  g_prog_name[] = "Z:\\game.exe";
char* g_argv_arr[2] = {g_prog_name, nullptr};
char* g_empty_env[1] = {nullptr};
char** g_msvcrt_initenv = g_empty_env;
i32   g_msvcrt_commode = 0;
i32   g_msvcrt_fmode = 0x8000;  // _O_BINARY
i32   g_msvcrt_timezone = 0;
char  g_tz_utc[] = "UTC";
char* g_msvcrt_tzname[2] = {g_tz_utc, g_tz_utc};
i32   g_app_type = 2;  // GUI
i32   g_err_mode = 0;
void* g_user_matherr = nullptr;
void* g_sig_handlers[32];

// ---------- CRT locks (_lock / _unlock) -----------------------------------
constexpr int k_num_crt_locks = 64;
#ifdef _WIN32
CRITICAL_SECTION g_crt_locks[k_num_crt_locks];
volatile LONG g_locks_ready = 0;
CRITICAL_SECTION g_file_lock;
void ensure_locks(void) {
  if (InterlockedCompareExchange(&g_locks_ready, 1, 0) == 0) {
    for (int i = 0; i < k_num_crt_locks; ++i)
      InitializeCriticalSection(&g_crt_locks[i]);
    InitializeCriticalSection(&g_file_lock);
    InterlockedExchange(&g_locks_ready, 2);
  }
  while (g_locks_ready != 2) {}
}
void file_lock(void)   { ensure_locks(); EnterCriticalSection(&g_file_lock); }
void file_unlock(void) { LeaveCriticalSection(&g_file_lock); }
void TRT_GUEST_ABI crt_lock(i32 n)   { ensure_locks(); EnterCriticalSection(&g_crt_locks[n & 63]); }
void TRT_GUEST_ABI crt_unlock(i32 n) { ensure_locks(); LeaveCriticalSection(&g_crt_locks[n & 63]); }
#else
pthread_mutex_t g_crt_locks[k_num_crt_locks];
pthread_once_t  g_locks_once = PTHREAD_ONCE_INIT;
pthread_mutex_t g_file_lock = PTHREAD_MUTEX_INITIALIZER;
void init_crt_locks(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  for (int i = 0; i < k_num_crt_locks; ++i)
    pthread_mutex_init(&g_crt_locks[i], &attr);
  pthread_mutexattr_destroy(&attr);
}
void file_lock(void)   { pthread_mutex_lock(&g_file_lock); }
void file_unlock(void) { pthread_mutex_unlock(&g_file_lock); }
void TRT_GUEST_ABI crt_lock(i32 n) {
  pthread_once(&g_locks_once, init_crt_locks);
  pthread_mutex_lock(&g_crt_locks[n & 63]);
}
void TRT_GUEST_ABI crt_unlock(i32 n) {
  pthread_once(&g_locks_once, init_crt_locks);
  pthread_mutex_unlock(&g_crt_locks[n & 63]);
}
#endif

// ---------- UTF helpers & path resolution ---------------------------------
void ascii_to_u16(const char* s, u16* w, size_t cap) {
  if (!w || cap == 0) return;
  size_t i = 0;
  if (s) {
    for (; s[i] && i + 1 < cap; ++i) w[i] = (u16)(u8)s[i];
  }
  w[i] = 0;
}

void u16_to_ascii(const u16* w, char* out, size_t cap) {
  if (!out || cap == 0) return;
  size_t i = 0;
  if (w) {
    for (; w[i] && i + 1 < cap; ++i) out[i] = (w[i] < 0x80) ? (char)w[i] : '?';
  }
  out[i] = 0;
}

bool mode_writes(const char* m) {
  if (!m) return false;
  for (; *m; ++m)
    if (*m == 'w' || *m == 'a' || *m == '+') return true;
  return false;
}

bool resolve_guest_path(const char* path8, const u16* path16, bool for_write,
                        char* host_out, size_t cap) {
  u16 wbuf[512];
  if (path16) {
    int rc = trt_vfs_resolve_utf16_rw(path16, for_write ? 1 : 0, host_out, cap);
    if (rc == 0) return true;
    u16_to_ascii(path16, host_out, cap);
    return false;
  }
  if (!path8) return false;
  ascii_to_u16(path8, wbuf, 512);
  if (trt_vfs_resolve_utf16_rw(wbuf, for_write ? 1 : 0, host_out, cap) == 0)
    return true;
  snprintf(host_out, cap, "%s", path8);
  return false;
}

// ---------- msvcrt 48-byte FILE* table & fd map ---------------------------
struct msvcrt_FILE {
  char* _ptr;
  i32   _cnt;
  i32   _pad0;
  char* _base;
  i32   _flag;
  i32   _file;
  i32   _charbuf;
  i32   _bufsiz;
  char* _tmpfname;
};
static_assert(sizeof(msvcrt_FILE) == 48, "Win64 msvcrt FILE must be 48 bytes");

constexpr int k_max_files = 256;
msvcrt_FILE g_files[k_max_files];
FILE*       g_os_files[k_max_files];
int         g_os_fds[k_max_files];
bool        g_files_inited = false;
int         g_maxstdio = 512;

void init_files_locked(void) {
  if (g_files_inited) return;
  g_files_inited = true;
  memset(g_files, 0, sizeof(g_files));
  memset(g_os_files, 0, sizeof(g_os_files));
  for (int i = 0; i < k_max_files; ++i) g_os_fds[i] = -1;
  g_files[0]._file = 0; g_os_files[0] = stdin;  g_os_fds[0] = 0;
  g_files[1]._file = 1; g_os_files[1] = stdout; g_os_fds[1] = 1;
  g_files[2]._file = 2; g_os_files[2] = stderr; g_os_fds[2] = 2;
}

int file_slot_of(const void* gf) {
  if (!gf) return -1;
  const uintptr_t p = (uintptr_t)gf;
  const uintptr_t b = (uintptr_t)&g_files[0];
  const uintptr_t e = (uintptr_t)&g_files[k_max_files];
  if (p < b || p >= e) return -1;
  if (((p - b) % sizeof(msvcrt_FILE)) != 0) return -1;
  int idx = (int)((p - b) / sizeof(msvcrt_FILE));
  if (idx < 0 || idx >= k_max_files || !g_os_files[idx]) return -1;
  return idx;
}

msvcrt_FILE* alloc_file_slot(FILE* osf, int fd) {
  if (!osf) return nullptr;
  file_lock();
  init_files_locked();
  for (int i = 3; i < k_max_files; ++i) {
    if (!g_os_files[i]) {
      memset(&g_files[i], 0, sizeof(g_files[i]));
      g_files[i]._file = fd >= 0 ? fd : i;
      g_os_files[i] = osf;
      g_os_fds[i] = fd;
      file_unlock();
      return &g_files[i];
    }
  }
  file_unlock();
  return nullptr;
}

FILE* os_file_of(void* gf, int* slot_out = nullptr) {
  file_lock();
  init_files_locked();
  int idx = file_slot_of(gf);
  FILE* osf = (idx >= 0) ? g_os_files[idx] : nullptr;
  if (slot_out) *slot_out = idx;
  file_unlock();
  return osf;
}

int host_fd_of(i32 gfd) {
  if (gfd >= 0 && gfd <= 2) return gfd;
  file_lock();
  init_files_locked();
  if (gfd >= 3 && gfd < k_max_files && g_os_fds[gfd] >= 0) {
    int fd = g_os_fds[gfd];
    file_unlock();
    return fd;
  }
  file_unlock();
  return (gfd >= 0) ? gfd : -1;
}

int set_errno_from_host(int e) {
  t_msvcrt_errno = e;
  return e;
}

// ---------- Win64 stat32 / stat64 layouts ----------------------------------
struct win64_stat32 {
  u32 st_dev;
  u16 st_ino;
  u16 st_mode;
  i16 st_nlink;
  i16 st_uid;
  i16 st_gid;
  u16 _pad;
  u32 st_rdev;
  i32 st_size;
  i32 w_atime;
  i32 w_mtime;
  i32 w_ctime;
};

struct win64_stat64 {
  u32 st_dev;
  u16 st_ino;
  u16 st_mode;
  i16 st_nlink;
  i16 st_uid;
  i16 st_gid;
  u16 _pad1;
  u32 st_rdev;
  u32 _pad2;
  i64 st_size;
  i64 w_atime;
  i64 w_mtime;
  i64 w_ctime;
};

// ---------- CRT Startup, locale & process ---------------------------------
u32 TRT_GUEST_ABI crt___lc_codepage_func(void) { return 0; }  // _CLOCALECP
i32 TRT_GUEST_ABI crt___mb_cur_max_func(void) { return 1; }
void* TRT_GUEST_ABI crt___iob_func(void) {
  file_lock();
  init_files_locked();
  file_unlock();
  return &g_files[0];
}
void TRT_GUEST_ABI crt___set_app_type(i32 t) { g_app_type = t; }
void TRT_GUEST_ABI crt___setusermatherr(void* f) { g_user_matherr = f; }
i32* TRT_GUEST_ABI crt__errno(void) { return &t_msvcrt_errno; }
i32 TRT_GUEST_ABI crt__getmaxstdio(void) { return g_maxstdio; }
i32 TRT_GUEST_ABI crt__setmaxstdio(i32 n) {
  g_maxstdio = n > 0 ? n : g_maxstdio;
  return g_maxstdio;
}
i32 TRT_GUEST_ABI crt__set_error_mode(i32 m) {
  i32 prev = g_err_mode;
  g_err_mode = m;
  return prev;
}
i32 TRT_GUEST_ABI crt__getpid(void) {
  if (auto fn = (pfn_GetCurrentProcessId)k32_sym("GetCurrentProcessId"))
    return (i32)fn();
  return 1;
}

typedef void (TRT_GUEST_ABI* pfn_initterm_cb)(void);
typedef i32  (TRT_GUEST_ABI* pfn_initterm_e_cb)(void);

void TRT_GUEST_ABI crt__initterm(void** start, void** end) {
  if (!start || !end) return;
  for (void** p = start; p < end; ++p) {
    if (*p) ((pfn_initterm_cb)(*p))();
  }
}

i32 TRT_GUEST_ABI crt__initterm_e(void** start, void** end) {
  if (!start || !end) return 0;
  for (void** p = start; p < end; ++p) {
    if (*p) {
      i32 r = ((pfn_initterm_e_cb)(*p))();
      if (r != 0) return r;
    }
  }
  return 0;
}

i32 TRT_GUEST_ABI crt___getmainargs(i32* argc, char*** argv, char*** env,
                                    i32 do_wildcard, void* start_info) {
  (void)do_wildcard; (void)start_info;
  if (argc) *argc = 1;
  if (argv) *argv = g_argv_arr;
  if (env) *env = g_empty_env;
  return 0;
}

constexpr int k_max_atexit = 256;
pfn_initterm_cb g_atexit[k_max_atexit];
int g_atexit_n = 0;

void run_atexit(void) {
  file_lock();
  int n = g_atexit_n;
  g_atexit_n = 0;
  file_unlock();
  for (int i = n - 1; i >= 0; --i) {
    if (g_atexit[i]) g_atexit[i]();
  }
}

i32 TRT_GUEST_ABI crt_atexit(void* fn) {
  if (!fn) return -1;
  file_lock();
  if (g_atexit_n >= k_max_atexit) { file_unlock(); return -1; }
  g_atexit[g_atexit_n++] = (pfn_initterm_cb)fn;
  file_unlock();
  return 0;
}

void TRT_GUEST_ABI crt__cexit(void) { run_atexit(); }

void TRT_GUEST_ABI crt_exit(i32 code) {
  run_atexit();
  if (auto fn = (pfn_ExitProcess)k32_sym("ExitProcess")) fn((u32)code);
  _exit(code);
}

// .212 DIAG: _exit/_Exit/quick_exit were NOT in the msvcrt bridge table, so
// a guest calling them fell through to the real CRT and died with zero
// logging. Log + exit via the logged ExitProcess path (no atexit: _exit
// semantics). If the .211 silent death is one of these, the log line names it.
void TRT_GUEST_ABI crt__exit(i32 code) {
  char line[64];
  snprintf(line, sizeof(line), "msvcrt: _exit(%d) called (no atexit)", code);
  trt_core_log(line);
  if (auto fn = (pfn_ExitProcess)k32_sym("ExitProcess")) fn((u32)code);
  _exit(code);
}
void TRT_GUEST_ABI crt__Exit(i32 code) {
  char line[64];
  snprintf(line, sizeof(line), "msvcrt: _Exit(%d) called (no atexit)", code);
  trt_core_log(line);
  if (auto fn = (pfn_ExitProcess)k32_sym("ExitProcess")) fn((u32)code);
  _exit(code);
}
void TRT_GUEST_ABI crt_quick_exit(i32 code) {
  // quick_exit runs at_quick_exit handlers, then _exit. The bridge has no
  // at_quick_exit registry, so this is _exit with a distinct log line.
  char line[64];
  snprintf(line, sizeof(line), "msvcrt: quick_exit(%d) called", code);
  trt_core_log(line);
  if (auto fn = (pfn_ExitProcess)k32_sym("ExitProcess")) fn((u32)code);
  _exit(code);
}

void TRT_GUEST_ABI crt_abort(void) {
  trt_core_log("msvcrt: abort() called -> ExitProcess(3)");
  if (auto fn = (pfn_ExitProcess)k32_sym("ExitProcess")) fn(3);
  _exit(3);
}

void TRT_GUEST_ABI crt__amsg_exit(i32 rterr) {
  char line[96];
  snprintf(line, sizeof(line), "msvcrt: _amsg_exit(%d)", rterr);
  trt_core_log(line);
  crt_exit(255);
}

void TRT_GUEST_ABI crt__assert(const char* expr, const char* file, u32 line) {
  char buf[256];
  snprintf(buf, sizeof(buf), "msvcrt: assertion failed: %s (%s:%u)",
           expr ? expr : "?", file ? file : "?", line);
  trt_core_log(buf);
  crt_abort();
}

u64 TRT_GUEST_ABI crt__beginthreadex(void* security, u32 stack_size,
                                     void* start_addr, void* arglist,
                                     u32 initflag, u32* thrdaddr) {
  auto fn = (pfn_CreateThread)k32_sym("CreateThread");
  if (!fn) { set_errno_from_host(EAGAIN); return 0; }
  return (u64)(uintptr_t)fn(security, (u64)stack_size, start_addr, arglist,
                            initflag, thrdaddr);
}

void TRT_GUEST_ABI crt__endthreadex(u32 retval) {
  if (auto fn = (pfn_ExitThread)k32_sym("ExitThread")) fn(retval);
}

// ---------- _setjmp / longjmp (Win64 ABI) ---------------------------------
#if !defined(_WIN32) && defined(__x86_64__)
__attribute__((naked)) i32 TRT_GUEST_ABI crt__setjmp(void*, void*) {
  __asm__ volatile(
      "movq %rdx, 0x00(%rcx)\n"
      "movq %rbx, 0x08(%rcx)\n"
      "leaq 8(%rsp), %rax\n"
      "movq %rax, 0x10(%rcx)\n"
      "movq %rbp, 0x18(%rcx)\n"
      "movq %rsi, 0x20(%rcx)\n"
      "movq %rdi, 0x28(%rcx)\n"
      "movq %r12, 0x30(%rcx)\n"
      "movq %r13, 0x38(%rcx)\n"
      "movq %r14, 0x40(%rcx)\n"
      "movq %r15, 0x48(%rcx)\n"
      "movq (%rsp), %rax\n"
      "movq %rax, 0x50(%rcx)\n"
      "movdqu %xmm6,  0x60(%rcx)\n"
      "movdqu %xmm7,  0x70(%rcx)\n"
      "movdqu %xmm8,  0x80(%rcx)\n"
      "movdqu %xmm9,  0x90(%rcx)\n"
      "movdqu %xmm10, 0xa0(%rcx)\n"
      "movdqu %xmm11, 0xb0(%rcx)\n"
      "movdqu %xmm12, 0xc0(%rcx)\n"
      "movdqu %xmm13, 0xd0(%rcx)\n"
      "movdqu %xmm14, 0xe0(%rcx)\n"
      "movdqu %xmm15, 0xf0(%rcx)\n"
      "xorl %eax, %eax\n"
      "ret\n");
}

__attribute__((naked)) void TRT_GUEST_ABI crt_longjmp(void*, i32) {
  __asm__ volatile(
      "movl %edx, %eax\n"
      "testl %eax, %eax\n"
      "jnz 1f\n"
      "movl $1, %eax\n"
      "1:\n"
      "movq 0x08(%rcx), %rbx\n"
      "movq 0x18(%rcx), %rbp\n"
      "movq 0x20(%rcx), %rsi\n"
      "movq 0x28(%rcx), %rdi\n"
      "movq 0x30(%rcx), %r12\n"
      "movq 0x38(%rcx), %r13\n"
      "movq 0x40(%rcx), %r14\n"
      "movq 0x48(%rcx), %r15\n"
      "movdqu 0x60(%rcx), %xmm6\n"
      "movdqu 0x70(%rcx), %xmm7\n"
      "movdqu 0x80(%rcx), %xmm8\n"
      "movdqu 0x90(%rcx), %xmm9\n"
      "movdqu 0xa0(%rcx), %xmm10\n"
      "movdqu 0xb0(%rcx), %xmm11\n"
      "movdqu 0xc0(%rcx), %xmm12\n"
      "movdqu 0xd0(%rcx), %xmm13\n"
      "movdqu 0xe0(%rcx), %xmm14\n"
      "movdqu 0xf0(%rcx), %xmm15\n"
      "movq 0x10(%rcx), %rsp\n"
      "jmpq *0x50(%rcx)\n");
}
#else
i32 TRT_GUEST_ABI crt__setjmp(void* env, void* frame) {
  (void)frame;
  return _setjmp((_JBTYPE*)env);
}
void TRT_GUEST_ABI crt_longjmp(void* env, i32 val) {
  ::longjmp((_JBTYPE*)env, val);
}
#endif

// ---------- memory allocation ---------------------------------------------
// Fail-visible huge-allocation probe: a straight pass-through malloc only
// returns NULL on absurd sizes, and the mixer retry-storm at
// CowData::_copy_on_write (measured: tid=11 hot loop, error path strings
// "core/os/memory.cpp"/"alloc_static") is exactly a NULL from here. Log the
// size when it exceeds any plausible game allocation so the guest-side
// garbage-size source becomes measurable.
//
// Budget: CRT allocations are real committed host memory, so they charge
// the ledger (commit + VA) like HeapAlloc does. Sizes are tracked in a
// registry (free needs the size to discharge); a pointer the registry
// never saw is freed uncharged, exactly like HeapFree treats foreign
// pointers.
namespace {
std::mutex g_crt_alloc_mu;
std::unordered_map<void*, size_t> g_crt_alloc;

bool crt_charge(size_t n) {
  if (!n) return true;
  if (trt_mem_budget_charge_va((u64)n) != 0) return false;
  if (trt_mem_budget_charge_commit((u64)n) != 0) {
    trt_mem_budget_discharge_va((u64)n);
    return false;
  }
  return true;
}
void crt_discharge(size_t n) {
  if (!n) return;
  trt_mem_budget_discharge_commit((u64)n);
  trt_mem_budget_discharge_va((u64)n);
}
void crt_track(void* p, size_t n) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_crt_alloc_mu);
  g_crt_alloc[p] = n;
}
bool crt_untrack(void* p, size_t* n_out) {
  if (!p) return false;
  std::lock_guard<std::mutex> lk(g_crt_alloc_mu);
  auto it = g_crt_alloc.find(p);
  if (it == g_crt_alloc.end()) return false;
  if (n_out) *n_out = it->second;
  g_crt_alloc.erase(it);
  return true;
}
size_t crt_tracked_size(void* p) {
  if (!p) return 0;
  std::lock_guard<std::mutex> lk(g_crt_alloc_mu);
  auto it = g_crt_alloc.find(p);
  return it == g_crt_alloc.end() ? 0 : it->second;
}
}  // namespace

// .251 FIX: forget the CRT allocation registry on budget reset. After
// a guest force-kill, trt_mem_budget_reset() zeroes the ledgers, but
// the dead guest's tracked blocks stay in this registry; when teardown
// frees them, crt_free discharges against the zeroed ledger and trips
// the over-discharge tripwire (the .250 9/16-byte warnings, 13 lines
// after the reset). Clearing the registry makes those frees land as
// foreign pointers (freed uncharged, memory still released), matching
// the reset's fresh-start fiction. Called only from
// trt_mem_budget_reset(), after the guest thread is joined, so no
// guest thread can be mid-alloc; a live host thread freeing a tracked
// block across the clear either discharges before it (normal) or finds
// no entry after it (freed uncharged) -- both safe.
extern "C" void trt_crt_forget_all(void) {
  std::lock_guard<std::mutex> lk(g_crt_alloc_mu);
  g_crt_alloc.clear();
}

void* TRT_GUEST_ABI crt_malloc(u64 sz) {
  // Settings knob: memory_budget_mb caps the single-allocation refusal
  // threshold. Default 256 MB; a user can raise it for big allocators
  // (Unreal-class) or lower it to catch runaway allocations early.
  // Weak symbol (declared at file scope below): offline test binaries
  // without settings.cpp keep the 256 MB default (weak resolves to 0).
  u64 cap = 268435456ull;  // 256 MB default
  const int mb = trt_settings_memory_budget_mb ? trt_settings_memory_budget_mb() : 0;
  if (mb > 0) cap = (u64)mb * 1048576ull;
  if (sz > cap) {
    static unsigned huge_named = 0;  // racy counter; only used for log caps
    const unsigned n = huge_named;
    if (n < 8) {
      char hl[96];
      snprintf(hl, sizeof(hl),
               "msvcrt: malloc(sz=%llu) -> NULL (huge; call #%u)",
               (unsigned long long)sz, n + 1);
      trt_core_log(hl);
      huge_named = n + 1;
    }
    return nullptr;
  }
  const size_t n = (size_t)sz;
  if (!crt_charge(n)) return nullptr;  // budget refusal (1455/8 set)
  void* p = malloc(n ? n : 1);
  if (!p) {
    crt_discharge(n);
    return nullptr;
  }
  crt_track(p, n);
  return p;
}
void* TRT_GUEST_ABI crt_calloc(u64 n, u64 sz) {
  const size_t total = (size_t)n * (size_t)sz;
  if (n && sz && total / (size_t)n != (size_t)sz) return nullptr;  // overflow
  if (!crt_charge(total)) return nullptr;
  void* p = calloc((size_t)n, (size_t)sz);
  if (!p) {
    crt_discharge(total);
    return nullptr;
  }
  crt_track(p, total);
  return p;
}
// defined below; realloc(p, 0) frees
void TRT_GUEST_ABI crt_free(void* p);
void* TRT_GUEST_ABI crt_realloc(void* p, u64 sz) {
  if (!p) return crt_malloc(sz);
  if (!sz) { crt_free(p); return nullptr; }
  const size_t old = crt_tracked_size(p);
  const size_t want = (size_t)sz;
  // charge only the delta (a realloc that grows by a few bytes near the
  // ceiling must not refuse spuriously); discharge the shrink after success
  const size_t delta = want > old ? want - old : 0;
  if (delta && !crt_charge(delta)) return nullptr;
  void* np = realloc(p, want);
  if (!np) {
    if (delta) crt_discharge(delta);
    return nullptr;
  }
  crt_untrack(p, nullptr);
  crt_track(np, want);
  if (old > want) crt_discharge(old - want);
  return np;
}
void  TRT_GUEST_ABI crt_free(void* p) {
  if (!p) return;
  size_t n = 0;
  if (crt_untrack(p, &n)) crt_discharge(n);
  free(p);
}

void* TRT_GUEST_ABI crt__aligned_malloc(u64 sz, u64 align) {
  if (align < sizeof(void*)) align = sizeof(void*);
  if ((align & (align - 1)) != 0) { set_errno_from_host(EINVAL); return nullptr; }
  const size_t n = (size_t)sz;
  if (!crt_charge(n)) return nullptr;
  void* p = nullptr;
#ifdef _WIN32
  p = _aligned_malloc(n ? n : 1, (size_t)align);
#else
  if (posix_memalign(&p, (size_t)align, n ? n : 1) != 0) p = nullptr;
#endif
  if (!p) {
    crt_discharge(n);
    return nullptr;
  }
  crt_track(p, n);
  return p;
}

void TRT_GUEST_ABI crt__aligned_free(void* p) {
  if (!p) return;
  size_t n = 0;
  if (crt_untrack(p, &n)) crt_discharge(n);
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

// ---------- mem / string / ctype ------------------------------------------
void* TRT_GUEST_ABI crt_memcpy(void* d, const void* s, u64 n) {
  return (d && s && n) ? memcpy(d, s, (size_t)n) : d;
}
void* TRT_GUEST_ABI crt_memmove(void* d, const void* s, u64 n) {
  return (d && s && n) ? memmove(d, s, (size_t)n) : d;
}
void* TRT_GUEST_ABI crt_memset(void* d, i32 c, u64 n) {
  return (d && n) ? memset(d, c, (size_t)n) : d;
}
i32 TRT_GUEST_ABI crt_memcmp(const void* a, const void* b, u64 n) {
  return memcmp(a, b, (size_t)n);
}
void* TRT_GUEST_ABI crt_memchr(const void* s, i32 c, u64 n) {
  return (void*)memchr(s, c, (size_t)n);
}
i32 TRT_GUEST_ABI crt__memicmp(const void* a, const void* b, u64 n) {
  const u8* pa = (const u8*)a;
  const u8* pb = (const u8*)b;
  for (u64 i = 0; i < n; ++i) {
    int da = tolower(pa[i]), db = tolower(pb[i]);
    if (da != db) return da - db;
  }
  return 0;
}

u64   TRT_GUEST_ABI crt_strlen(const char* s) { return s ? (u64)strlen(s) : 0; }
i32   TRT_GUEST_ABI crt_strcmp(const char* a, const char* b) { return strcmp(a, b); }
i32   TRT_GUEST_ABI crt_strncmp(const char* a, const char* b, u64 n) { return strncmp(a, b, (size_t)n); }
i32   TRT_GUEST_ABI crt_strcoll(const char* a, const char* b) { return strcmp(a, b); }
u64   TRT_GUEST_ABI crt_strxfrm(char* d, const char* s, u64 n) {
  size_t l = strlen(s);
  if (d && n > 0) {
    size_t c = l < (size_t)n ? l : (size_t)n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return (u64)l;
}
i32   TRT_GUEST_ABI crt__stricmp(const char* a, const char* b) {
  while (*a && *b) {
    int d = tolower((u8)*a) - tolower((u8)*b);
    if (d) return d;
    ++a; ++b;
  }
  return tolower((u8)*a) - tolower((u8)*b);
}
char* TRT_GUEST_ABI crt_strcpy(char* d, const char* s) { return strcpy(d, s); }
i32   TRT_GUEST_ABI crt_strcpy_s(char* d, u64 sz, const char* s) {
  if (!d || !sz || !s) { set_errno_from_host(EINVAL); return EINVAL; }
  size_t l = strlen(s);
  if (l >= sz) { d[0] = 0; set_errno_from_host(ERANGE); return ERANGE; }
  memcpy(d, s, l + 1);
  return 0;
}
char* TRT_GUEST_ABI crt_strncpy(char* d, const char* s, u64 n) { return strncpy(d, s, (size_t)n); }
char* TRT_GUEST_ABI crt_strcat(char* d, const char* s) { return strcat(d, s); }
i32   TRT_GUEST_ABI crt_strcat_s(char* d, u64 sz, const char* s) {
  if (!d || !sz || !s) { set_errno_from_host(EINVAL); return EINVAL; }
  size_t dl = strlen(d), sl = strlen(s);
  if (dl + sl >= sz) { d[0] = 0; set_errno_from_host(ERANGE); return ERANGE; }
  memcpy(d + dl, s, sl + 1);
  return 0;
}
char* TRT_GUEST_ABI crt_strncat(char* d, const char* s, u64 n) { return strncat(d, s, (size_t)n); }
char* TRT_GUEST_ABI crt_strchr(const char* s, i32 c) { return (char*)strchr(s, c); }
char* TRT_GUEST_ABI crt_strrchr(const char* s, i32 c) { return (char*)strrchr(s, c); }
char* TRT_GUEST_ABI crt_strstr(const char* h, const char* n) { return (char*)strstr(h, n); }
char* TRT_GUEST_ABI crt_strpbrk(const char* s, const char* a) { return (char*)strpbrk(s, a); }
u64   TRT_GUEST_ABI crt_strcspn(const char* s, const char* r) { return (u64)strcspn(s, r); }
u64   TRT_GUEST_ABI crt_strspn(const char* s, const char* a) { return (u64)strspn(s, a); }
char* TRT_GUEST_ABI crt__strdup(const char* s) {
  if (!s) return nullptr;
  const size_t n = strlen(s) + 1;
  char* p = (char*)crt_malloc((u64)n);  // charged + tracked (freed via free)
  if (p) memcpy(p, s, n);
  return p;
}
char* TRT_GUEST_ABI crt_strerror(i32 e) { return strerror(e); }

i64   TRT_GUEST_ABI crt_strtol(const char* s, char** end, i32 base) { return (i64)strtol(s, end, base); }
u64   TRT_GUEST_ABI crt_strtoul(const char* s, char** end, i32 base) { return (u64)strtoul(s, end, base); }
i64   TRT_GUEST_ABI crt__strtoi64(const char* s, char** end, i32 base) { return (i64)strtoll(s, end, base); }
u64   TRT_GUEST_ABI crt__strtoui64(const char* s, char** end, i32 base) { return (u64)strtoull(s, end, base); }
i32   TRT_GUEST_ABI crt_atoi(const char* s) { return s ? atoi(s) : 0; }
double TRT_GUEST_ABI crt_atof(const char* s) { return s ? atof(s) : 0.0; }
i32   TRT_GUEST_ABI crt__itoa_s(i32 val, char* buf, u64 sz, i32 radix) {
  if (!buf || sz == 0 || radix < 2 || radix > 36) return EINVAL;
  static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char tmp[40];
  int pos = 0;
  bool neg = (val < 0 && radix == 10);
  u32 uv = neg ? (u32)(-(i64)val) : (u32)val;
  do { tmp[pos++] = dig[uv % (u32)radix]; uv /= (u32)radix; } while (uv);
  if (neg) tmp[pos++] = '-';
  if ((u64)pos + 1 > sz) { buf[0] = 0; return ERANGE; }
  for (int i = 0; i < pos; ++i) buf[i] = tmp[pos - 1 - i];
  buf[pos] = 0;
  return 0;
}

char* TRT_GUEST_ABI crt__ultoa(u32 value, char* buf, i32 radix) {
  if (!buf || radix < 2 || radix > 36) {
    set_errno_from_host(EINVAL);
    return nullptr;
  }
  const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
  char reversed[32];
  size_t length = 0;
  do {
    reversed[length++] = digits[value % static_cast<u32>(radix)];
    value /= static_cast<u32>(radix);
  } while (value);
  for (size_t i = 0; i < length; ++i) buf[i] = reversed[length - i - 1];
  buf[length] = '\0';
  return buf;
}

// Wide UTF-16LE string functions
u64 TRT_GUEST_ABI crt_wcslen(const u16* s) {
  if (!s) return 0;
  u64 n = 0;
  while (s[n]) ++n;
  return n;
}
i32 TRT_GUEST_ABI crt_wcscmp(const u16* a, const u16* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (i32)*a - (i32)*b;
}
i32 TRT_GUEST_ABI crt_wcscoll(const u16* a, const u16* b) { return crt_wcscmp(a, b); }
u64 TRT_GUEST_ABI crt_wcsxfrm(u16* d, const u16* s, u64 n) {
  u64 l = crt_wcslen(s);
  if (d && n > 0) {
    u64 c = l < n ? l : n - 1;
    memcpy(d, s, (size_t)c * 2);
    d[c] = 0;
  }
  return l;
}
u16* TRT_GUEST_ABI crt_wcscpy(u16* d, const u16* s) {
  u16* p = d;
  while ((*p++ = *s++) != 0) {}
  return d;
}
i32 TRT_GUEST_ABI crt_wcscpy_s(u16* d, u64 sz, const u16* s) {
  if (!d || !sz || !s) return EINVAL;
  u64 l = crt_wcslen(s);
  if (l >= sz) { d[0] = 0; return ERANGE; }
  memcpy(d, s, (size_t)(l + 1) * 2);
  return 0;
}
i64 TRT_GUEST_ABI crt_wcstol(const u16* s, u16** end, i32 base) {
  char buf[128];
  u16_to_ascii(s, buf, sizeof(buf));
  char* e8 = nullptr;
  long v = strtol(buf, &e8, base);
  if (end) *end = (u16*)(s + (e8 ? (size_t)(e8 - buf) : 0));
  return (i64)v;
}
u64 TRT_GUEST_ABI crt_mbstowcs(u16* d, const char* s, u64 n) {
  if (!s) return 0;
  size_t l = strlen(s);
  if (!d) return (u64)l;
  u64 i = 0;
  for (; i < n && s[i]; ++i) d[i] = (u16)(u8)s[i];
  if (i < n) d[i] = 0;
  return i;
}
u64 TRT_GUEST_ABI crt_wcstombs(char* d, const u16* s, u64 n) {
  if (!s) return 0;
  u64 l = crt_wcslen(s);
  if (!d) return l;
  u64 i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i] < 0x80 ? (char)s[i] : '?';
  if (i < n) d[i] = 0;
  return i;
}

i32 TRT_GUEST_ABI crt_isalnum(i32 c)  { return isalnum((u8)c); }
i32 TRT_GUEST_ABI crt_isalpha(i32 c)  { return isalpha((u8)c); }
i32 TRT_GUEST_ABI crt_isdigit(i32 c)  { return isdigit((u8)c); }
i32 TRT_GUEST_ABI crt_isspace(i32 c)  { return isspace((u8)c); }
i32 TRT_GUEST_ABI crt_isxdigit(i32 c) { return isxdigit((u8)c); }
i32 TRT_GUEST_ABI crt_tolower(i32 c)  { return (c == -1) ? -1 : tolower((u8)c); }
i32 TRT_GUEST_ABI crt_toupper(i32 c)  { return (c == -1) ? -1 : toupper((u8)c); }
u16 TRT_GUEST_ABI crt_towlower(u16 c) { return (c < 128) ? (u16)tolower((u8)c) : c; }
u16 TRT_GUEST_ABI crt_towupper(u16 c) { return (c < 128) ? (u16)toupper((u8)c) : c; }
i32 TRT_GUEST_ABI crt_iswctype(u16 c, u16 desc) {
  if (c >= 128) return 0;
  u16 mask = 0;
  if (isupper(c))  mask |= 0x0001;
  if (islower(c))  mask |= 0x0002;
  if (isdigit(c))  mask |= 0x0004;
  if (isspace(c))  mask |= 0x0008;
  if (ispunct(c))  mask |= 0x0010;
  if (iscntrl(c))  mask |= 0x0020;
  if (c == ' ')    mask |= 0x0040;
  if (isxdigit(c)) mask |= 0x0080;
  if (isalpha(c))  mask |= 0x0100;
  return (mask & desc) ? 1 : 0;
}

// ---------- locale --------------------------------------------------------
struct win_lconv {
  char* decimal_point; char* thousands_sep; char* grouping;
  char* int_curr_symbol; char* currency_symbol;
  char* mon_decimal_point; char* mon_thousands_sep; char* mon_grouping;
  char* positive_sign; char* negative_sign;
  char  int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space,
        n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
  u16*  _W_decimal_point; u16* _W_thousands_sep; u16* _W_int_curr_symbol;
  u16*  _W_currency_symbol; u16* _W_mon_decimal_point; u16* _W_mon_thousands_sep;
  u16*  _W_positive_sign; u16* _W_negative_sign;
};
char g_dot[] = ".";
char g_empty_s[] = "";
u16  g_wdot[] = {'.', 0};
u16  g_wempty[] = {0};
win_lconv g_lconv = {
    g_dot, g_empty_s, g_empty_s, g_empty_s, g_empty_s,
    g_empty_s, g_empty_s, g_empty_s, g_empty_s, g_empty_s,
    127, 127, 127, 127, 127, 127, 127, 127,
    g_wdot, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty
};
char g_c_locale[] = "C";

void* TRT_GUEST_ABI crt_localeconv(void) { return &g_lconv; }
char* TRT_GUEST_ABI crt_setlocale(i32 cat, const char* loc) {
  (void)cat; (void)loc;
  return g_c_locale;
}

// ---------- time / environment --------------------------------------------
i64 TRT_GUEST_ABI crt__time64(i64* t) {
  i64 v = (i64)time(nullptr);
  if (t) *t = v;
  return v;
}
i32 TRT_GUEST_ABI crt_clock(void) { return (i32)clock(); }
void TRT_GUEST_ABI crt__tzset(void) {}

struct win_tm {
  i32 tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
#if defined(_WIN32)
__declspec(thread) win_tm t_gmtime_buf;
#else
__thread win_tm t_gmtime_buf;
#endif

win_tm* TRT_GUEST_ABI crt__gmtime64(const i64* t) {
  if (!t) return nullptr;
  time_t tt = (time_t)*t;
  struct tm res;
#ifdef _WIN32
  if (gmtime_s(&res, &tt) != 0) return nullptr;
#else
  if (!gmtime_r(&tt, &res)) return nullptr;
#endif
  t_gmtime_buf.tm_sec = res.tm_sec;
  t_gmtime_buf.tm_min = res.tm_min;
  t_gmtime_buf.tm_hour = res.tm_hour;
  t_gmtime_buf.tm_mday = res.tm_mday;
  t_gmtime_buf.tm_mon = res.tm_mon;
  t_gmtime_buf.tm_year = res.tm_year;
  t_gmtime_buf.tm_wday = res.tm_wday;
  t_gmtime_buf.tm_yday = res.tm_yday;
  t_gmtime_buf.tm_isdst = res.tm_isdst;
  return &t_gmtime_buf;
}

u64 TRT_GUEST_ABI crt_strftime(char* buf, u64 maxsz, const char* fmt,
                               const win_tm* wt) {
  if (!buf || !maxsz || !fmt || !wt) return 0;
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_sec = wt->tm_sec; t.tm_min = wt->tm_min; t.tm_hour = wt->tm_hour;
  t.tm_mday = wt->tm_mday; t.tm_mon = wt->tm_mon; t.tm_year = wt->tm_year;
  t.tm_wday = wt->tm_wday; t.tm_yday = wt->tm_yday; t.tm_isdst = wt->tm_isdst;
  return (u64)strftime(buf, (size_t)maxsz, fmt, &t);
}

u64 TRT_GUEST_ABI crt_wcsftime(u16* buf, u64 maxsz, const u16* fmt,
                               const win_tm* wt) {
  if (!buf || !maxsz || !fmt || !wt) return 0;
  char f8[256], out8[512];
  u16_to_ascii(fmt, f8, sizeof(f8));
  u64 r = crt_strftime(out8, sizeof(out8), f8, wt);
  if (r == 0) { buf[0] = 0; return 0; }
  u64 i = 0;
  for (; i < maxsz - 1 && out8[i]; ++i) buf[i] = (u16)(u8)out8[i];
  buf[i] = 0;
  return i;
}

struct env_cache_entry { char key[128]; char val[1024]; bool used; };
env_cache_entry g_env_cache[64];

char* TRT_GUEST_ABI crt_getenv(const char* name) {
  if (!name) return nullptr;
  auto fn = (pfn_GetEnvA)k32_sym("GetEnvironmentVariableA");
  if (!fn) return nullptr;
  char tmp[1024];
  u32 n = fn(name, tmp, sizeof(tmp));
  if (n == 0 || n >= sizeof(tmp)) return nullptr;
  file_lock();
  int slot = -1;
  for (int i = 0; i < 64; ++i) {
    if (g_env_cache[i].used && strcmp(g_env_cache[i].key, name) == 0) {
      slot = i;
      break;
    }
    if (!g_env_cache[i].used && slot < 0) slot = i;
  }
  if (slot < 0) slot = 0;
  g_env_cache[slot].used = true;
  snprintf(g_env_cache[slot].key, sizeof(g_env_cache[slot].key), "%s", name);
  snprintf(g_env_cache[slot].val, sizeof(g_env_cache[slot].val), "%s", tmp);
  char* res = g_env_cache[slot].val;
  file_unlock();
  return res;
}

i32 TRT_GUEST_ABI crt__putenv_s(const char* name, const char* val) {
  if (!name) { set_errno_from_host(EINVAL); return EINVAL; }
  if (auto fn = (pfn_SetEnvA)k32_sym("SetEnvironmentVariableA")) {
    return fn(name, val) ? 0 : EINVAL;
  }
  return EINVAL;
}

// ---------- math / sort / rand --------------------------------------------
double TRT_GUEST_ABI crt_acos(double x)  { return acos(x); }
double TRT_GUEST_ABI crt_asin(double x)  { return asin(x); }
double TRT_GUEST_ABI crt_atan(double x)  { return atan(x); }
double TRT_GUEST_ABI crt_ceil(double x)  { return ceil(x); }
double TRT_GUEST_ABI crt_cosh(double x)  { return cosh(x); }
double TRT_GUEST_ABI crt_log10(double x) { return log10(x); }
double TRT_GUEST_ABI crt_sinh(double x)  { return sinh(x); }
double TRT_GUEST_ABI crt_tan(double x)   { return tan(x); }
double TRT_GUEST_ABI crt_tanh(double x)  { return tanh(x); }
double TRT_GUEST_ABI crt__hypot(double x, double y) { return hypot(x, y); }
double TRT_GUEST_ABI crt__nextafter(double x, double y) { return nextafter(x, y); }

u32 g_rand_seed = 1;
void TRT_GUEST_ABI crt_srand(u32 s) { g_rand_seed = s; }
i32  TRT_GUEST_ABI crt_rand(void) {
  g_rand_seed = g_rand_seed * 214013u + 2531011u;
  return (i32)((g_rand_seed >> 16) & 0x7FFFu);
}

typedef i32 (TRT_GUEST_ABI* pfn_cmp)(const void*, const void*);

void swap_bytes(u8* a, u8* b, u64 sz) {
  for (u64 i = 0; i < sz; ++i) { u8 t = a[i]; a[i] = b[i]; b[i] = t; }
}

void TRT_GUEST_ABI crt_qsort(void* base, u64 n, u64 sz, void* cmp_fn) {
  if (!base || n < 2 || sz == 0 || !cmp_fn) return;
  auto cmp = (pfn_cmp)cmp_fn;
  u8* b = (u8*)base;
  // Heapsort guarantees O(N log N) worst-case without unbounded recursion.
  for (u64 start = n / 2; start-- > 0;) {
    u64 root = start;
    while (root * 2 + 1 < n) {
      u64 child = root * 2 + 1;
      if (child + 1 < n && cmp(b + child * sz, b + (child + 1) * sz) < 0)
        ++child;
      if (cmp(b + root * sz, b + child * sz) >= 0) break;
      swap_bytes(b + root * sz, b + child * sz, sz);
      root = child;
    }
  }
  for (u64 end = n - 1; end > 0; --end) {
    swap_bytes(b, b + end * sz, sz);
    u64 root = 0;
    while (root * 2 + 1 < end) {
      u64 child = root * 2 + 1;
      if (child + 1 < end && cmp(b + child * sz, b + (child + 1) * sz) < 0)
        ++child;
      if (cmp(b + root * sz, b + child * sz) >= 0) break;
      swap_bytes(b + root * sz, b + child * sz, sz);
      root = child;
    }
  }
}

void* TRT_GUEST_ABI crt_bsearch(const void* key, const void* base, u64 n,
                                u64 sz, void* cmp_fn) {
  if (!key || !base || !n || !sz || !cmp_fn) return nullptr;
  auto cmp = (pfn_cmp)cmp_fn;
  const u8* b = (const u8*)base;
  u64 lo = 0, hi = n;
  while (lo < hi) {
    u64 mid = lo + (hi - lo) / 2;
    const void* elem = b + mid * sz;
    i32 c = cmp(key, elem);
    if (c == 0) return (void*)elem;
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return nullptr;
}

void* TRT_GUEST_ABI crt_signal(i32 sig, void* fn) {
  if (sig < 0 || sig >= 32) { set_errno_from_host(EINVAL); return (void*)(uintptr_t)-1; }
  void* prev = g_sig_handlers[sig];
  g_sig_handlers[sig] = fn;
  return prev;
}

// .235 FIX: long-path fopen retry (vfs.cpp) -- the shader-cache file
// opens land here (CRT funnel), not in CreateFileW (0 witness hits
// in .234). Declared here, not in trt.h (breadth-agent-owned).
extern "C" FILE* trt_vfs_fopen_retry(const char* host, const char* mode);

// ---------- FILE* / VFS I/O -----------------------------------------------
msvcrt_FILE* open_vfs_file(const char* p8, const u16* p16, const char* m) {
  if ((!p8 && !p16) || !m) { set_errno_from_host(EINVAL); return nullptr; }
  const bool wr = mode_writes(m);
  char host[1024];
  resolve_guest_path(p8, p16, wr, host, sizeof(host));
  if (wr) {
    trt_vfs_ensure_parents(host);
    // Copy-on-write when opening existing Z: file with r+ / a / a+
    if ((m[0] == 'r' || m[0] == 'a') && trt_vfs_stat(host, nullptr, nullptr) != 0) {
      char ro[1024];
      int is_dir = 0;
      if (resolve_guest_path(p8, p16, false, ro, sizeof(ro)) &&
          strcmp(ro, host) != 0 && trt_vfs_stat(ro, &is_dir, nullptr) == 0 && !is_dir) {
        FILE* in = fopen(ro, "rb");
        if (in) {
          FILE* cp = fopen(host, "wb");
          if (cp) {
            char cbuf[16384];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), in)) > 0) fwrite(cbuf, 1, n, cp);
            fclose(cp);
          }
          fclose(in);
        }
      }
    }
  }
  FILE* osf = trt_vfs_fopen_retry(host, m);
  if (!osf) {
    set_errno_from_host(errno);
    return nullptr;
  }
  if (wr) {
    if (p16) trt_vfs_mark_z_deleted_utf16(p16, 0, 0);
    else if (p8) {
      u16 wtmp[512];
      ascii_to_u16(p8, wtmp, 512);
      trt_vfs_mark_z_deleted_utf16(wtmp, 0, 0);
    }
  }
#ifdef _WIN32
  int fd = _fileno(osf);
#else
  int fd = fileno(osf);
#endif
  msvcrt_FILE* gf = alloc_file_slot(osf, fd);
  if (!gf) {
    fclose(osf);
    set_errno_from_host(EMFILE);
    return nullptr;
  }
  return gf;
}

void* TRT_GUEST_ABI crt_fopen(const char* p, const char* m) {
  return open_vfs_file(p, nullptr, m);
}
void* TRT_GUEST_ABI crt__wfopen(const u16* p, const u16* m) {
  char m8[32];
  u16_to_ascii(m, m8, sizeof(m8));
  return open_vfs_file(nullptr, p, m8);
}
i32 TRT_GUEST_ABI crt__wfopen_s(void** out, const u16* p, const u16* m) {
  if (!out) return EINVAL;
  *out = crt__wfopen(p, m);
  return *out ? 0 : (t_msvcrt_errno ? t_msvcrt_errno : ENOENT);
}
void* TRT_GUEST_ABI crt__wfsopen(const u16* p, const u16* m, i32 shflag) {
  (void)shflag;
  return crt__wfopen(p, m);
}

i32 TRT_GUEST_ABI crt_freopen_s(void** out, const char* p, const char* m, void* gf) {
  if (!out) return EINVAL;
  int slot = -1;
  FILE* old = os_file_of(gf, &slot);
  if (slot < 0) return EINVAL;
  const bool wr = mode_writes(m);
  char host[1024];
  resolve_guest_path(p, nullptr, wr, host, sizeof(host));
  if (wr) trt_vfs_ensure_parents(host);
  FILE* nf = freopen(host, m, old);
  if (!nf) { *out = nullptr; return errno; }
  file_lock();
  g_os_files[slot] = nf;
#ifdef _WIN32
  g_os_fds[slot] = _fileno(nf);
#else
  g_os_fds[slot] = fileno(nf);
#endif
  file_unlock();
  *out = gf;
  return 0;
}

i32 TRT_GUEST_ABI crt_fclose(void* gf) {
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (!osf || slot < 3) return EOF;
  file_lock();
  g_os_files[slot] = nullptr;
  g_os_fds[slot] = -1;
  memset(&g_files[slot], 0, sizeof(g_files[slot]));
  file_unlock();
  return fclose(osf);
}

u64 TRT_GUEST_ABI crt_fread(void* ptr, u64 sz, u64 nm, void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? (u64)fread(ptr, (size_t)sz, (size_t)nm, osf) : 0;
}
u64 TRT_GUEST_ABI crt_fwrite(const void* ptr, u64 sz, u64 nm, void* gf) {
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (!osf) return 0;
  if (slot == 1 || slot == 2) {
    const size_t tot = (size_t)(sz * nm);
    char msg[256];
    const size_t n = tot < 220 ? tot : 220;
    snprintf(msg, sizeof(msg), "msvcrt: %.*s", (int)n, (const char*)ptr);
    trt_core_log(msg);
    return nm;
  }
  return (u64)fwrite(ptr, (size_t)sz, (size_t)nm, osf);
}
i32 TRT_GUEST_ABI crt_fflush(void* gf) {
  if (!gf) return fflush(nullptr);
  FILE* osf = os_file_of(gf);
  return osf ? fflush(osf) : EOF;
}
i32 TRT_GUEST_ABI crt_feof(void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? feof(osf) : 1;
}
i32 TRT_GUEST_ABI crt_ferror(void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? ferror(osf) : 1;
}
i32 TRT_GUEST_ABI crt_fseek(void* gf, i32 off, i32 whence) {
  FILE* osf = os_file_of(gf);
  return osf ? fseek(osf, off, whence) : -1;
}
i32 TRT_GUEST_ABI crt_ftell(void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? (i32)ftell(osf) : -1;
}
void TRT_GUEST_ABI crt_rewind(void* gf) {
  if (FILE* osf = os_file_of(gf)) rewind(osf);
}
i32 TRT_GUEST_ABI crt_fgetpos(void* gf, i64* pos) {
  FILE* osf = os_file_of(gf);
  if (!osf || !pos) return -1;
#ifdef _WIN32
  *pos = _ftelli64(osf);
#else
  *pos = (i64)ftello(osf);
#endif
  return (*pos < 0) ? -1 : 0;
}
i32 TRT_GUEST_ABI crt_fsetpos(void* gf, const i64* pos) {
  FILE* osf = os_file_of(gf);
  if (!osf || !pos) return -1;
#ifdef _WIN32
  return _fseeki64(osf, *pos, SEEK_SET);
#else
  return fseeko(osf, (off_t)*pos, SEEK_SET);
#endif
}
i64 TRT_GUEST_ABI crt__ftelli64(void* gf) {
  FILE* osf = os_file_of(gf);
  if (!osf) return -1;
#ifdef _WIN32
  return (i64)_ftelli64(osf);
#else
  return (i64)ftello(osf);
#endif
}
i32 TRT_GUEST_ABI crt__fseeki64(void* gf, i64 off, i32 whence) {
  FILE* osf = os_file_of(gf);
  if (!osf) return -1;
#ifdef _WIN32
  return _fseeki64(osf, off, whence);
#else
  return fseeko(osf, (off_t)off, whence);
#endif
}
char* TRT_GUEST_ABI crt_fgets(char* s, i32 n, void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? fgets(s, n, osf) : nullptr;
}
i32 TRT_GUEST_ABI crt_getc(void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? fgetc(osf) : EOF;
}
i32 TRT_GUEST_ABI crt_ungetc(i32 c, void* gf) {
  FILE* osf = os_file_of(gf);
  return osf ? ungetc(c, osf) : EOF;
}
i32 TRT_GUEST_ABI crt_fputc(i32 c, void* gf) {
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (!osf) return EOF;
  if (slot == 1 || slot == 2) return (u8)c;
  return fputc(c, osf);
}
i32 TRT_GUEST_ABI crt_putc(i32 c, void* gf) { return crt_fputc(c, gf); }
i32 TRT_GUEST_ABI crt_putchar(i32 c) { return (u8)c; }
i32 TRT_GUEST_ABI crt_fputs(const char* s, void* gf) {
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (!osf || !s) return EOF;
  if (slot == 1 || slot == 2) {
    char msg[256];
    snprintf(msg, sizeof(msg), "msvcrt: %.220s", s);
    trt_core_log(msg);
    return 0;
  }
  return fputs(s, osf);
}
i32 TRT_GUEST_ABI crt_puts(const char* s) {
  if (!s) return EOF;
  char msg[256];
  snprintf(msg, sizeof(msg), "msvcrt: %.220s", s);
  trt_core_log(msg);
  return 0;
}
i32 TRT_GUEST_ABI crt_getwc(void* gf) {
  FILE* osf = os_file_of(gf);
  if (!osf) return 0xFFFF;
  int b0 = fgetc(osf);
  if (b0 == EOF) return 0xFFFF;
  int b1 = fgetc(osf);
  if (b1 == EOF) return 0xFFFF;
  return (b1 << 8) | b0;
}
i32 TRT_GUEST_ABI crt_fputwc(u16 c, void* gf) {
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (!osf) return 0xFFFF;
  if (slot == 1 || slot == 2) return c;
  u8 buf[2] = {(u8)(c & 0xFF), (u8)(c >> 8)};
  return fwrite(buf, 1, 2, osf) == 2 ? (i32)c : 0xFFFF;
}
i32 TRT_GUEST_ABI crt_putwc(u16 c, void* gf) { return crt_fputwc(c, gf); }
i32 TRT_GUEST_ABI crt_ungetwc(u16 c, void* gf) {
  FILE* osf = os_file_of(gf);
  if (!osf) return 0xFFFF;
  ungetc((int)(c >> 8), osf);
  ungetc((int)(c & 0xFF), osf);
  return c;
}
void TRT_GUEST_ABI crt_setbuf(void* gf, char* buf) {
  if (FILE* osf = os_file_of(gf)) setbuf(osf, buf);
}
i32 TRT_GUEST_ABI crt_setvbuf(void* gf, char* buf, i32 mode, u64 sz) {
  FILE* osf = os_file_of(gf);
  return osf ? setvbuf(osf, buf, mode, (size_t)sz) : -1;
}
i32 TRT_GUEST_ABI crt_remove(const char* path) {
  if (!path) { set_errno_from_host(EINVAL); return -1; }
  u16 w[512];
  ascii_to_u16(path, w, 512);
  u32 werr = 0;
  if (trt_vfs_delete_file_utf16(w, &werr) == 0) return 0;
  set_errno_from_host(werr == 5 ? EACCES : ENOENT);
  return -1;
}
void TRT_GUEST_ABI crt_perror(const char* s) {
  char msg[256];
  snprintf(msg, sizeof(msg), "msvcrt: perror: %s: %s", s ? s : "",
           strerror(t_msvcrt_errno));
  trt_core_log(msg);
}

i32 TRT_GUEST_ABI crt__fileno(void* gf) {
  file_lock();
  init_files_locked();
  int slot = file_slot_of(gf);
  int res = (slot >= 0) ? slot : -1;
  file_unlock();
  return res;
}
void* TRT_GUEST_ABI crt__fdopen(i32 gfd, const char* m) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0 || !m) return nullptr;
#ifdef _WIN32
  FILE* f = _fdopen(hfd, m);
#else
  FILE* f = fdopen(hfd, m);
#endif
  if (!f) return nullptr;
  return alloc_file_slot(f, hfd);
}
i32 TRT_GUEST_ABI crt__read(i32 gfd, void* buf, u32 cnt) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) return -1;
#ifdef _WIN32
  return _read(hfd, buf, cnt);
#else
  return (i32)read(hfd, buf, cnt);
#endif
}
i32 TRT_GUEST_ABI crt__write(i32 gfd, const void* buf, u32 cnt) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) return -1;
  if (hfd == 1 || hfd == 2) return (i32)cnt;
#ifdef _WIN32
  return _write(hfd, buf, cnt);
#else
  return (i32)write(hfd, buf, cnt);
#endif
}
i64 TRT_GUEST_ABI crt__lseeki64(i32 gfd, i64 off, i32 origin) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) return -1;
#ifdef _WIN32
  return _lseeki64(hfd, off, origin);
#else
  return (i64)lseek(hfd, (off_t)off, origin);
#endif
}
i64 TRT_GUEST_ABI crt__telli64(i32 gfd) {
  const i64 offset = crt__lseeki64(gfd, 0, SEEK_CUR);
  if (offset < 0) set_errno_from_host(errno ? errno : EBADF);
  return offset;
}
i64 TRT_GUEST_ABI crt__filelengthi64(i32 gfd) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) return -1;
#ifdef _WIN32
  struct _stat64 st;
  if (_fstat64(hfd, &st) != 0) return -1;
#else
  struct stat st;
  if (fstat(hfd, &st) != 0) return -1;
#endif
  return (i64)st.st_size;
}
i32 TRT_GUEST_ABI crt__chsize(i32 gfd, i32 size) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) return -1;
#ifdef _WIN32
  return _chsize(hfd, size);
#else
  return ftruncate(hfd, (off_t)size);
#endif
}
i32 TRT_GUEST_ABI crt__fstat(i32 gfd, win64_stat32* out) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0 || !out) return -1;
#ifdef _WIN32
  struct _stat64 st;
  if (_fstat64(hfd, &st) != 0) { set_errno_from_host(errno); return -1; }
#else
  struct stat st;
  if (fstat(hfd, &st) != 0) { set_errno_from_host(errno); return -1; }
#endif
  memset(out, 0, sizeof(*out));
  out->st_mode = (u16)st.st_mode;
  out->st_nlink = (i16)st.st_nlink;
  out->st_size = (i32)st.st_size;
  out->w_atime = (i32)st.st_atime;
  out->w_mtime = (i32)st.st_mtime;
  out->w_ctime = (i32)st.st_ctime;
  return 0;
}
i32 TRT_GUEST_ABI crt__fstat64(i32 gfd, win64_stat64* out) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0 || !out) return -1;
#ifdef _WIN32
  struct _stat64 st;
  if (_fstat64(hfd, &st) != 0) { set_errno_from_host(errno); return -1; }
#else
  struct stat st;
  if (fstat(hfd, &st) != 0) { set_errno_from_host(errno); return -1; }
#endif
  memset(out, 0, sizeof(*out));
  out->st_mode = (u16)st.st_mode;
  out->st_nlink = (i16)st.st_nlink;
  out->st_size = (i64)st.st_size;
  out->w_atime = (i64)st.st_atime;
  out->w_mtime = (i64)st.st_mtime;
  out->w_ctime = (i64)st.st_ctime;
  return 0;
}
i64 TRT_GUEST_ABI crt__get_osfhandle(i32 gfd) {
  int hfd = host_fd_of(gfd);
  if (hfd < 0) { set_errno_from_host(EBADF); return -1; }
#ifdef _WIN32
  return (i64)_get_osfhandle(hfd);
#else
  return (i64)hfd;
#endif
}
i32 TRT_GUEST_ABI crt__wchdir(const u16* dir) {
  auto fn = (pfn_SetCurrentDirW)k32_sym("SetCurrentDirectoryW");
  if (fn && dir && fn(dir)) return 0;
  set_errno_from_host(ENOENT);
  return -1;
}
i32 TRT_GUEST_ABI crt__chdir(const char* dir) {
  if (!dir) { set_errno_from_host(EINVAL); return -1; }
  u16 w[512];
  ascii_to_u16(dir, w, 512);
  return crt__wchdir(w);
}
u16* TRT_GUEST_ABI crt__wgetcwd(u16* buf, i32 maxlen) {
  auto fn = (pfn_GetCurrentDirW)k32_sym("GetCurrentDirectoryW");
  if (!fn) return nullptr;
  u16 tmp[512];
  u32 n = fn(512, tmp);
  if (!n) return nullptr;
  if (!buf) {
    u64 need = (u64)n + 1;
    u16* p = (u16*)crt_malloc(need * 2);  // charged + tracked
    if (p) memcpy(p, tmp, (size_t)need * 2);
    return p;
  }
  if ((u32)maxlen <= n) { set_errno_from_host(ERANGE); return nullptr; }
  memcpy(buf, tmp, ((size_t)n + 1) * 2);
  return buf;
}
char* TRT_GUEST_ABI crt__getcwd(char* buf, i32 maxlen) {
  auto fn = (pfn_GetCurrentDirW)k32_sym("GetCurrentDirectoryW");
  if (!fn) return nullptr;
  u16 wtmp[512];
  u32 n = fn(512, wtmp);
  if (!n) return nullptr;
  char tmp[512];
  u16_to_ascii(wtmp, tmp, sizeof(tmp));
  u32 len = (u32)strlen(tmp);
  if (!buf) {
    char* p = (char*)crt_malloc((u64)len + 1);  // charged + tracked
    if (p) memcpy(p, tmp, (size_t)len + 1);
    return p;
  }
  if (maxlen <= (i32)len) { set_errno_from_host(ERANGE); return nullptr; }
  memcpy(buf, tmp, (size_t)len + 1);
  return buf;
}

typedef u32  (TRT_GUEST_ABI* pfn_GetEnvW)(const u16*, u16*, u32);
typedef i32  (TRT_GUEST_ABI* pfn_SetEnvW)(const u16*, const u16*);
typedef void* (TRT_GUEST_ABI* pfn_FindFirstW)(const u16*, void*);
typedef i32  (TRT_GUEST_ABI* pfn_FindNextW)(void*, void*);
typedef i32  (TRT_GUEST_ABI* pfn_FindClose)(void*);

u16 g_wgetenv_ring[16][1024] = {};
u32 g_wgetenv_idx = 0;

u16* TRT_GUEST_ABI crt__wgetenv(const u16* name) {
  if (!name || !name[0]) { set_errno_from_host(EINVAL); return nullptr; }
  auto fn = (pfn_GetEnvW)k32_sym("GetEnvironmentVariableW");
  if (!fn) return nullptr;
  u16 tmp[1024];
  u32 n = fn(name, tmp, 1024);
  if (n == 0 || n >= 1024) return nullptr;
  crt_lock(7);
  u16* slot = g_wgetenv_ring[g_wgetenv_idx++ & 15u];
  memcpy(slot, tmp, ((size_t)n + 1) * 2);
  crt_unlock(7);
  return slot;
}
i32 TRT_GUEST_ABI crt__wputenv_s(const u16* name, const u16* value) {
  if (!name || !name[0] || !value) { set_errno_from_host(EINVAL); return EINVAL; }
  auto fn = (pfn_SetEnvW)k32_sym("SetEnvironmentVariableW");
  if (!fn) { set_errno_from_host(EINVAL); return EINVAL; }
  return fn(name, value[0] ? value : nullptr) ? 0 : EINVAL;
}
i32 TRT_GUEST_ABI crt__wputenv(const u16* envstr) {
  if (!envstr) { set_errno_from_host(EINVAL); return -1; }
  const u16* eq = envstr;
  while (*eq && *eq != (u16)'=') ++eq;
  if (*eq != (u16)'=' || eq == envstr) { set_errno_from_host(EINVAL); return -1; }
  size_t klen = (size_t)(eq - envstr);
  if (klen >= 256) { set_errno_from_host(EINVAL); return -1; }
  u16 key[256];
  memcpy(key, envstr, klen * 2);
  key[klen] = 0;
  const u16* val = eq + 1;
  return crt__wputenv_s(key, val) == 0 ? 0 : -1;
}

static i64 ft_bytes_to_time64(const u8* ft) {
  u64 lo = *(const u32*)(ft + 0);
  u64 hi = *(const u32*)(ft + 4);
  u64 v = (hi << 32) | lo;
  if (v < 116444736000000000ULL) return 0;
  return (i64)((v - 116444736000000000ULL) / 10000000ULL);
}

enum find_kind {
  FK_64 = 0, FK_W64 = 1, FK_64I32 = 2, FK_W64I32 = 3,
  FK_32 = 4, FK_W32 = 5, FK_32I64 = 6, FK_W32I64 = 7,
};

static void fill_msvcrt_find_data(int kind, const u8* wfd, void* dst) {
  const u32 attrib = (*(const u32*)(wfd + 0)) & 0x37u;
  const i64 tc = ft_bytes_to_time64(wfd + 4);
  const i64 ta = ft_bytes_to_time64(wfd + 12);
  const i64 tw = ft_bytes_to_time64(wfd + 20);
  const u64 sz = ((u64)*(const u32*)(wfd + 28) << 32) | *(const u32*)(wfd + 32);
  const u16* wname = (const u16*)(wfd + 44);
  u8* d = (u8*)dst;
  switch (kind) {
    case FK_64:
      memset(d, 0, 304);
      *(u32*)(d + 0) = attrib; *(i64*)(d + 8) = tc; *(i64*)(d + 16) = ta;
      *(i64*)(d + 24) = tw; *(i64*)(d + 32) = (i64)sz;
      u16_to_ascii(wname, (char*)(d + 40), 260);
      break;
    case FK_W64:
      memset(d, 0, 560);
      *(u32*)(d + 0) = attrib; *(i64*)(d + 8) = tc; *(i64*)(d + 16) = ta;
      *(i64*)(d + 24) = tw; *(i64*)(d + 32) = (i64)sz;
      for (size_t i = 0; i < 259 && wname[i]; ++i) ((u16*)(d + 40))[i] = wname[i];
      break;
    case FK_64I32:
      memset(d, 0, 296);
      *(u32*)(d + 0) = attrib; *(i64*)(d + 8) = tc; *(i64*)(d + 16) = ta;
      *(i64*)(d + 24) = tw; *(u32*)(d + 32) = (u32)sz;
      u16_to_ascii(wname, (char*)(d + 36), 260);
      break;
    case FK_W64I32:
      memset(d, 0, 560);
      *(u32*)(d + 0) = attrib; *(i64*)(d + 8) = tc; *(i64*)(d + 16) = ta;
      *(i64*)(d + 24) = tw; *(u32*)(d + 32) = (u32)sz;
      for (size_t i = 0; i < 259 && wname[i]; ++i) ((u16*)(d + 36))[i] = wname[i];
      break;
    case FK_32:
      memset(d, 0, 280);
      *(u32*)(d + 0) = attrib; *(i32*)(d + 4) = (i32)tc; *(i32*)(d + 8) = (i32)ta;
      *(i32*)(d + 12) = (i32)tw; *(u32*)(d + 16) = (u32)sz;
      u16_to_ascii(wname, (char*)(d + 20), 260);
      break;
    case FK_W32:
      memset(d, 0, 540);
      *(u32*)(d + 0) = attrib; *(i32*)(d + 4) = (i32)tc; *(i32*)(d + 8) = (i32)ta;
      *(i32*)(d + 12) = (i32)tw; *(u32*)(d + 16) = (u32)sz;
      for (size_t i = 0; i < 259 && wname[i]; ++i) ((u16*)(d + 20))[i] = wname[i];
      break;
    case FK_32I64:
      memset(d, 0, 288);
      *(u32*)(d + 0) = attrib; *(i32*)(d + 4) = (i32)tc; *(i32*)(d + 8) = (i32)ta;
      *(i32*)(d + 12) = (i32)tw; *(i64*)(d + 16) = (i64)sz;
      u16_to_ascii(wname, (char*)(d + 24), 260);
      break;
    case FK_W32I64:
      memset(d, 0, 544);
      *(u32*)(d + 0) = attrib; *(i32*)(d + 4) = (i32)tc; *(i32*)(d + 8) = (i32)ta;
      *(i32*)(d + 12) = (i32)tw; *(i64*)(d + 16) = (i64)sz;
      for (size_t i = 0; i < 259 && wname[i]; ++i) ((u16*)(d + 24))[i] = wname[i];
      break;
  }
}

static i64 msvcrt_findfirst_impl(const char* pat8, const u16* pat16, void* data, int kind) {
  if ((!pat8 && !pat16) || !data) { set_errno_from_host(EINVAL); return -1; }
  auto fn = (pfn_FindFirstW)k32_sym("FindFirstFileW");
  if (!fn) { set_errno_from_host(ENOENT); return -1; }
  u16 wbuf[512];
  const u16* wp = pat16;
  if (!wp) { ascii_to_u16(pat8, wbuf, 512); wp = wbuf; }
  u8 wfd[592];
  void* h = fn(wp, wfd);
  if (h == (void*)(uintptr_t)-1) { set_errno_from_host(ENOENT); return -1; }
  fill_msvcrt_find_data(kind, wfd, data);
  return (i64)(intptr_t)h;
}

static i32 msvcrt_findnext_impl(i64 handle, void* data, int kind) {
  if (handle == -1 || !data) { set_errno_from_host(EINVAL); return -1; }
  auto fn = (pfn_FindNextW)k32_sym("FindNextFileW");
  if (!fn) { set_errno_from_host(ENOENT); return -1; }
  u8 wfd[592];
  if (!fn((void*)(intptr_t)handle, wfd)) { set_errno_from_host(ENOENT); return -1; }
  fill_msvcrt_find_data(kind, wfd, data);
  return 0;
}

i64 TRT_GUEST_ABI crt__findfirst64(const char* p, void* d) { return msvcrt_findfirst_impl(p, nullptr, d, FK_64); }
i64 TRT_GUEST_ABI crt__wfindfirst64(const u16* p, void* d) { return msvcrt_findfirst_impl(nullptr, p, d, FK_W64); }
i64 TRT_GUEST_ABI crt__findfirst64i32(const char* p, void* d) { return msvcrt_findfirst_impl(p, nullptr, d, FK_64I32); }
i64 TRT_GUEST_ABI crt__wfindfirst64i32(const u16* p, void* d) { return msvcrt_findfirst_impl(nullptr, p, d, FK_W64I32); }
i64 TRT_GUEST_ABI crt__findfirst32(const char* p, void* d) { return msvcrt_findfirst_impl(p, nullptr, d, FK_32); }
i64 TRT_GUEST_ABI crt__wfindfirst32(const u16* p, void* d) { return msvcrt_findfirst_impl(nullptr, p, d, FK_W32); }
i64 TRT_GUEST_ABI crt__findfirst32i64(const char* p, void* d) { return msvcrt_findfirst_impl(p, nullptr, d, FK_32I64); }
i64 TRT_GUEST_ABI crt__wfindfirst32i64(const u16* p, void* d) { return msvcrt_findfirst_impl(nullptr, p, d, FK_W32I64); }

i32 TRT_GUEST_ABI crt__findnext64(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_64); }
i32 TRT_GUEST_ABI crt__wfindnext64(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_W64); }
i32 TRT_GUEST_ABI crt__findnext64i32(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_64I32); }
i32 TRT_GUEST_ABI crt__wfindnext64i32(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_W64I32); }
i32 TRT_GUEST_ABI crt__findnext32(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_32); }
i32 TRT_GUEST_ABI crt__wfindnext32(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_W32); }
i32 TRT_GUEST_ABI crt__findnext32i64(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_32I64); }
i32 TRT_GUEST_ABI crt__wfindnext32i64(i64 h, void* d) { return msvcrt_findnext_impl(h, d, FK_W32I64); }

i32 TRT_GUEST_ABI crt__findclose(i64 h) {
  if (h == -1) { set_errno_from_host(EINVAL); return -1; }
  auto fn = (pfn_FindClose)k32_sym("FindClose");
  return (fn && fn((void*)(intptr_t)h)) ? 0 : -1;
}

// ---------- __builtin_ms_va_list formatting -------------------------------
#if defined(_WIN32)
typedef va_list ms_va_list;
#  define ms_va_start(ap, last) va_start(ap, last)
#  define ms_va_arg(ap, type)   va_arg(ap, type)
#  define ms_va_end(ap)         va_end(ap)
#else
typedef __builtin_ms_va_list ms_va_list;
#  define ms_va_start(ap, last) __builtin_ms_va_start(ap, last)
#  define ms_va_arg(ap, type)   va_arg(ap, type)
#  define ms_va_end(ap)         __builtin_ms_va_end(ap)
#endif

int format_ms_vsnprintf(char* out, size_t cap, const char* fmt, ms_va_list ap) {
  size_t pos = 0;
  auto putc_out = [&](char c) {
    if (out && pos + 1 < cap) out[pos] = c;
    ++pos;
  };
  auto puts_out = [&](const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) putc_out(s[i]);
  };
  for (const char* p = fmt; p && *p; ++p) {
    if (*p != '%') { putc_out(*p); continue; }
    ++p;
    if (*p == '%') { putc_out('%'); continue; }
    char spec[32];
    size_t si = 0;
    spec[si++] = '%';
    bool zero = false, left = false;
    while ((*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') &&
           si + 1 < sizeof(spec)) {
      if (*p == '0') zero = true;
      if (*p == '-') left = true;
      spec[si++] = *p++;
    }
    int width = -1, prec = -1;
    if (*p == '*') {
      width = ms_va_arg(ap, int);
      ++p;
      if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", width);
    } else {
      while (isdigit((u8)*p) && si + 1 < sizeof(spec)) {
        if (width < 0) width = 0;
        width = width * 10 + (*p - '0');
        spec[si++] = *p++;
      }
    }
    if (*p == '.') {
      if (si + 1 < sizeof(spec)) spec[si++] = '.';
      ++p;
      if (*p == '*') {
        prec = ms_va_arg(ap, int);
        ++p;
        if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", prec);
      } else {
        prec = 0;
        while (isdigit((u8)*p) && si + 1 < sizeof(spec)) {
          prec = prec * 10 + (*p - '0');
          spec[si++] = *p++;
        }
      }
    }
    int len_mod = 0;  // 1='l', 2='ll'/'I64', 3='z'/'t'
    if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len_mod = 2; p += 3; }
    else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { len_mod = 0; p += 3; }
    else if (p[0] == 'l' && p[1] == 'l') { len_mod = 2; p += 2; }
    else if (*p == 'l') { len_mod = 1; ++p; }
    else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
    else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'I') { len_mod = 3; ++p; }

    char conv = *p;
    if (!conv) break;

    if (conv == 's' || conv == 'S') {
      const bool wide = (conv == 'S' || len_mod == 1);
      if (wide) {
        const u16* ws = ms_va_arg(ap, const u16*);
        char tmp[512];
        if (!ws) snprintf(tmp, sizeof(tmp), "(null)");
        else u16_to_ascii(ws, tmp, sizeof(tmp));
        size_t l = strlen(tmp);
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(zero ? '0' : ' ');
        puts_out(tmp, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(' ');
      } else {
        const char* s = ms_va_arg(ap, const char*);
        if (!s) s = "(null)";
        size_t l = strlen(s);
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(zero ? '0' : ' ');
        puts_out(s, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(' ');
      }
      continue;
    }
    if (conv == 'c' || conv == 'C') {
      int ch = ms_va_arg(ap, int);
      putc_out((char)(u8)ch);
      continue;
    }

    char tmp[128];
    if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
        conv == 'g' || conv == 'G') {
      spec[si] = conv; spec[si + 1] = 0;
      double d = ms_va_arg(ap, double);
      int n = snprintf(tmp, sizeof(tmp), spec, d);
      if (n > 0) puts_out(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    } else if (conv == 'p') {
      void* vp = ms_va_arg(ap, void*);
      int n = snprintf(tmp, sizeof(tmp), "%p", vp);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else if (conv == 'd' || conv == 'i') {
      i64 v = (len_mod >= 2) ? ms_va_arg(ap, i64) : (i64)ms_va_arg(ap, i32);
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (long long)v);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
      u64 v = (len_mod >= 2) ? ms_va_arg(ap, u64) : (u64)(u32)ms_va_arg(ap, u32);
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else {
      putc_out(conv);
    }
  }
  if (out && cap > 0) out[(pos < cap) ? pos : (cap - 1)] = 0;
  return (int)pos;
}

// Careful! On Win64 (and Clang __builtin_ms_va_list), va_list passed to
// vsnprintf/vfprintf IS literally `char*` pointing to stacked 64-bit arg slots!
// Reading slots as *(u64*)cur / *(double*)cur matches Win64 va_list 100%!
int format_ms_va_ptr(char* out, size_t cap, const char* fmt, const u64* va) {
  size_t pos = 0, ai = 0;
  auto next_u64 = [&]() -> u64 { return va ? va[ai++] : 0; };
  auto putc_out = [&](char c) {
    if (out && pos + 1 < cap) out[pos] = c;
    ++pos;
  };
  auto puts_out = [&](const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) putc_out(s[i]);
  };
  for (const char* p = fmt; p && *p; ++p) {
    if (*p != '%') { putc_out(*p); continue; }
    ++p;
    if (*p == '%') { putc_out('%'); continue; }
    char spec[32];
    size_t si = 0;
    spec[si++] = '%';
    bool zero = false, left = false;
    while ((*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') &&
           si + 1 < sizeof(spec)) {
      if (*p == '0') zero = true;
      if (*p == '-') left = true;
      spec[si++] = *p++;
    }
    int width = -1, prec = -1;
    if (*p == '*') {
      width = (int)(i32)next_u64();
      ++p;
      if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", width);
    } else {
      while (isdigit((u8)*p) && si + 1 < sizeof(spec)) {
        if (width < 0) width = 0;
        width = width * 10 + (*p - '0');
        spec[si++] = *p++;
      }
    }
    if (*p == '.') {
      if (si + 1 < sizeof(spec)) spec[si++] = '.';
      ++p;
      if (*p == '*') {
        prec = (int)(i32)next_u64();
        ++p;
        if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", prec);
      } else {
        prec = 0;
        while (isdigit((u8)*p) && si + 1 < sizeof(spec)) {
          prec = prec * 10 + (*p - '0');
          spec[si++] = *p++;
        }
      }
    }
    int len_mod = 0;
    if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len_mod = 2; p += 3; }
    else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { len_mod = 0; p += 3; }
    else if (p[0] == 'l' && p[1] == 'l') { len_mod = 2; p += 2; }
    else if (*p == 'l') { len_mod = 1; ++p; }
    else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
    else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'I') { len_mod = 3; ++p; }
    char conv = *p;
    if (!conv) break;

    if (conv == 's' || conv == 'S') {
      u64 ptrv = next_u64();
      const bool wide = (conv == 'S' || len_mod == 1);
      if (wide) {
        const u16* ws = (const u16*)(uintptr_t)ptrv;
        char tmp[512];
        if (!ws) snprintf(tmp, sizeof(tmp), "(null)");
        else u16_to_ascii(ws, tmp, sizeof(tmp));
        size_t l = strlen(tmp);
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(zero ? '0' : ' ');
        puts_out(tmp, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(' ');
      } else {
        const char* s = (const char*)(uintptr_t)ptrv;
        if (!s) s = "(null)";
        size_t l = strlen(s);
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(zero ? '0' : ' ');
        puts_out(s, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_out(' ');
      }
      continue;
    }
    if (conv == 'c' || conv == 'C') {
      putc_out((char)(u8)next_u64());
      continue;
    }
    char tmp[128];
    if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
        conv == 'g' || conv == 'G') {
      u64 bits = next_u64();
      double d = 0.0;
      memcpy(&d, &bits, 8);
      spec[si] = conv; spec[si + 1] = 0;
      int n = snprintf(tmp, sizeof(tmp), spec, d);
      if (n > 0) puts_out(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    } else if (conv == 'p') {
      void* vp = (void*)(uintptr_t)next_u64();
      int n = snprintf(tmp, sizeof(tmp), "%p", vp);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else if (conv == 'd' || conv == 'i') {
      u64 raw = next_u64();
      i64 v = (len_mod >= 2) ? (i64)raw : (i64)(i32)raw;
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (long long)v);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
      u64 raw = next_u64();
      u64 v = (len_mod >= 2) ? raw : (u64)(u32)raw;
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
      if (n > 0) puts_out(tmp, (size_t)n);
    } else {
      putc_out(conv);
    }
  }
  if (out && cap > 0) out[(pos < cap) ? pos : (cap - 1)] = 0;
  return (int)pos;
}

i32 TRT_GUEST_ABI crt__vsnprintf_real(char* buf, u64 cnt, const char* fmt, const u64* va) {
  int n = format_ms_va_ptr(buf, (size_t)cnt, fmt, va);
  return (buf && (size_t)n >= (size_t)cnt) ? -1 : n;
}

i32 TRT_GUEST_ABI crt__vsnprintf_s(char* buf, u64 sz, u64 maxcnt,
                                   const char* fmt, const u64* va) {
  if (!buf || sz == 0 || !fmt) return -1;
  size_t lim = (maxcnt == ~(u64)0 || maxcnt + 1 > sz) ? (size_t)sz : (size_t)maxcnt + 1;
  int n = format_ms_va_ptr(buf, lim, fmt, va);
  return ((size_t)n >= lim && maxcnt != ~(u64)0) ? -1 : n;
}

i32 TRT_GUEST_ABI crt__vscprintf(const char* fmt, const u64* va) {
  return format_ms_va_ptr(nullptr, 0, fmt, va);
}

i32 TRT_GUEST_ABI crt_vfprintf(void* gf, const char* fmt, const u64* va) {
  char buf[1024];
  int n = format_ms_va_ptr(buf, sizeof(buf), fmt, va);
  crt_fwrite(buf, 1, (u64)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), gf);
  return n;
}

i32 TRT_GUEST_ABI crt_fprintf(void* gf, const char* fmt, ...) {
  char buf[1024];
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, sizeof(buf), fmt, ap);
  ms_va_end(ap);
  int put = (n < (int)sizeof(buf)) ? (n < 0 ? 0 : n) : (int)(sizeof(buf) - 1);
  crt_fwrite(buf, 1, (u64)put, gf);
  return n;
}

// ---------- wide formatters + sprintf / sscanf ----------------------------
template <typename NextU64>
int format_ms_ws_impl(u16* out, size_t cap, const u16* wfmt, NextU64 next_u64) {
  size_t pos = 0;
  auto putc_w = [&](u16 c) {
    if (out && pos + 1 < cap) out[pos] = c;
    ++pos;
  };
  auto puts_w = [&](const u16* s, size_t n) {
    for (size_t i = 0; i < n; ++i) putc_w(s[i]);
  };
  auto puts_a = [&](const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) putc_w((u16)(u8)s[i]);
  };
  for (const u16* p = wfmt; p && *p; ++p) {
    if (*p != '%') { putc_w(*p); continue; }
    ++p;
    if (*p == '%') { putc_w('%'); continue; }
    char spec[32];
    size_t si = 0;
    spec[si++] = '%';
    bool zero = false, left = false;
    while ((*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') &&
           si + 1 < sizeof(spec)) {
      if (*p == '0') zero = true;
      if (*p == '-') left = true;
      spec[si++] = (char)*p++;
    }
    int width = -1, prec = -1;
    if (*p == '*') {
      width = (int)(i32)next_u64();
      ++p;
      if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", width);
    } else {
      while (*p >= '0' && *p <= '9' && si + 1 < sizeof(spec)) {
        if (width < 0) width = 0;
        width = width * 10 + (*p - '0');
        spec[si++] = (char)*p++;
      }
    }
    if (*p == '.') {
      if (si + 1 < sizeof(spec)) spec[si++] = '.';
      ++p;
      if (*p == '*') {
        prec = (int)(i32)next_u64();
        ++p;
        if (si + 12 < sizeof(spec)) si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", prec);
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9' && si + 1 < sizeof(spec)) {
          prec = prec * 10 + (*p - '0');
          spec[si++] = (char)*p++;
        }
      }
    }
    int len_mod = 0;
    if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len_mod = 2; p += 3; }
    else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { len_mod = 0; p += 3; }
    else if (p[0] == 'l' && p[1] == 'l') { len_mod = 2; p += 2; }
    else if (*p == 'l') { len_mod = 1; ++p; }
    else if (*p == 'h') { len_mod = 4; ++p; if (*p == 'h') ++p; }
    else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'I') { len_mod = 2; ++p; }
    const u16 conv16 = *p;
    if (!conv16) break;
    const char conv = (conv16 < 0x80) ? (char)conv16 : '?';
    if (conv == 's' || conv == 'S') {
      const u64 ptrv = next_u64();
      const bool narrow = (conv == 'S') || (len_mod == 4);
      if (!narrow) {
        static const u16 wnull[] = {'(','n','u','l','l',')',0};
        const u16* ws = (const u16*)(uintptr_t)ptrv;
        if (!ws) ws = wnull;
        size_t l = 0;
        while (ws[l]) ++l;
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_w(zero ? '0' : ' ');
        puts_w(ws, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_w(' ');
      } else {
        const char* s = (const char*)(uintptr_t)ptrv;
        if (!s) s = "(null)";
        size_t l = strlen(s);
        if (prec >= 0 && (size_t)prec < l) l = (size_t)prec;
        if (!left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_w(zero ? '0' : ' ');
        puts_a(s, l);
        if (left && width > (int)l) for (int k = 0; k < width - (int)l; ++k) putc_w(' ');
      }
      continue;
    }
    if (conv == 'c' || conv == 'C') {
      putc_w((u16)next_u64());
      continue;
    }
    char tmp[128];
    if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
        conv == 'g' || conv == 'G') {
      const u64 bits = next_u64();
      double d = 0.0;
      memcpy(&d, &bits, 8);
      spec[si] = conv; spec[si + 1] = 0;
      int n = snprintf(tmp, sizeof(tmp), spec, d);
      if (n > 0) puts_a(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    } else if (conv == 'p') {
      void* vp = (void*)(uintptr_t)next_u64();
      int n = snprintf(tmp, sizeof(tmp), "%p", vp);
      if (n > 0) puts_a(tmp, (size_t)n);
    } else if (conv == 'd' || conv == 'i') {
      u64 raw = next_u64();
      i64 v = (len_mod == 2) ? (i64)raw : (i64)(i32)raw;
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (long long)v);
      if (n > 0) puts_a(tmp, (size_t)n);
    } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
      u64 raw = next_u64();
      u64 v = (len_mod == 2) ? raw : (u64)(u32)raw;
      if (si + 3 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0; }
      int n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
      if (n > 0) puts_a(tmp, (size_t)n);
    } else {
      putc_w(conv16);
    }
  }
  if (out && cap > 0) out[(pos < cap) ? pos : (cap - 1)] = 0;
  return (int)pos;
}

i32 TRT_GUEST_ABI crt_vsnprintf_alias(char* buf, u64 cnt, const char* fmt, const u64* va) {
  return format_ms_va_ptr(buf, (size_t)cnt, fmt, va);
}
i32 TRT_GUEST_ABI crt_snprintf(char* buf, u64 cnt, const char* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, (size_t)cnt, fmt, ap);
  ms_va_end(ap);
  return n;
}
i32 TRT_GUEST_ABI crt__snprintf(char* buf, u64 cnt, const char* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, (size_t)cnt, fmt, ap);
  ms_va_end(ap);
  return (buf && (size_t)n >= (size_t)cnt) ? -1 : n;
}
i32 TRT_GUEST_ABI crt__snprintf_s(char* buf, u64 sz, u64 maxcnt, const char* fmt, ...) {
  if (!buf || sz == 0 || !fmt) return -1;
  size_t lim = (maxcnt == ~(u64)0 || maxcnt + 1 > sz) ? (size_t)sz : (size_t)maxcnt + 1;
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, lim, fmt, ap);
  ms_va_end(ap);
  return ((size_t)n >= lim && maxcnt != ~(u64)0) ? -1 : n;
}
i32 TRT_GUEST_ABI crt_vsprintf(char* buf, const char* fmt, const u64* va) {
  return format_ms_va_ptr(buf, buf ? 65536u : 0u, fmt, va);
}
i32 TRT_GUEST_ABI crt_vsprintf_s(char* buf, u64 sz, const char* fmt, const u64* va) {
  if (!buf || sz == 0 || !fmt) return -1;
  return format_ms_va_ptr(buf, (size_t)sz, fmt, va);
}
i32 TRT_GUEST_ABI crt_sprintf(char* buf, const char* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, buf ? 65536u : 0u, fmt, ap);
  ms_va_end(ap);
  return n;
}
i32 TRT_GUEST_ABI crt_sprintf_s(char* buf, u64 sz, const char* fmt, ...) {
  if (!buf || sz == 0 || !fmt) return -1;
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_vsnprintf(buf, (size_t)sz, fmt, ap);
  ms_va_end(ap);
  return n;
}
i32 TRT_GUEST_ABI crt__vsnwprintf(u16* buf, u64 cnt, const u16* fmt, const u64* va) {
  size_t ai = 0;
  int n = format_ms_ws_impl(buf, (size_t)cnt, fmt, [&]() -> u64 { return va ? va[ai++] : 0; });
  return (buf && (size_t)n >= (size_t)cnt) ? -1 : n;
}
i32 TRT_GUEST_ABI crt__vsnwprintf_s(u16* buf, u64 sz, u64 maxcnt, const u16* fmt, const u64* va) {
  if (!buf || sz == 0 || !fmt) return -1;
  size_t lim = (maxcnt == ~(u64)0 || maxcnt + 1 > sz) ? (size_t)sz : (size_t)maxcnt + 1;
  size_t ai = 0;
  int n = format_ms_ws_impl(buf, lim, fmt, [&]() -> u64 { return va ? va[ai++] : 0; });
  return ((size_t)n >= lim && maxcnt != ~(u64)0) ? -1 : n;
}
i32 TRT_GUEST_ABI crt_vswprintf(u16* buf, u64 cnt, const u16* fmt, const u64* va) {
  size_t ai = 0;
  return format_ms_ws_impl(buf, (size_t)cnt, fmt, [&]() -> u64 { return va ? va[ai++] : 0; });
}
i32 TRT_GUEST_ABI crt__vscwprintf(const u16* fmt, const u64* va) {
  size_t ai = 0;
  return format_ms_ws_impl(nullptr, 0, fmt, [&]() -> u64 { return va ? va[ai++] : 0; });
}
i32 TRT_GUEST_ABI crt__snwprintf(u16* buf, u64 cnt, const u16* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_ws_impl(buf, (size_t)cnt, fmt, [&]() -> u64 { return ms_va_arg(ap, u64); });
  ms_va_end(ap);
  return (buf && (size_t)n >= (size_t)cnt) ? -1 : n;
}
i32 TRT_GUEST_ABI crt__snwprintf_s(u16* buf, u64 sz, u64 maxcnt, const u16* fmt, ...) {
  if (!buf || sz == 0 || !fmt) return -1;
  size_t lim = (maxcnt == ~(u64)0 || maxcnt + 1 > sz) ? (size_t)sz : (size_t)maxcnt + 1;
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_ws_impl(buf, lim, fmt, [&]() -> u64 { return ms_va_arg(ap, u64); });
  ms_va_end(ap);
  return ((size_t)n >= lim && maxcnt != ~(u64)0) ? -1 : n;
}
i32 TRT_GUEST_ABI crt_swprintf(u16* buf, u64 cnt, const u16* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_ws_impl(buf, (size_t)cnt, fmt, [&]() -> u64 { return ms_va_arg(ap, u64); });
  ms_va_end(ap);
  return n;
}
i32 TRT_GUEST_ABI crt_swprintf_s(u16* buf, u64 sz, const u16* fmt, ...) {
  if (!buf || sz == 0 || !fmt) return -1;
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int n = format_ms_ws_impl(buf, (size_t)sz, fmt, [&]() -> u64 { return ms_va_arg(ap, u64); });
  ms_va_end(ap);
  return n;
}

int sscanf_impl(const char* str, const char* fmt, ms_va_list ap, bool secure_sizes) {
  if (!str || !fmt) return -1;
  const char* s = str;
  const char* f = fmt;
  int assigned = 0;
  while (*f) {
    if (isspace((u8)*f)) {
      while (isspace((u8)*f)) ++f;
      while (isspace((u8)*s)) ++s;
      continue;
    }
    if (*f != '%') {
      if (*s != *f) return assigned;
      ++s; ++f;
      continue;
    }
    ++f;
    if (*f == '%') {
      if (*s != '%') return assigned;
      ++s; ++f;
      continue;
    }
    bool suppress = false;
    if (*f == '*') { suppress = true; ++f; }
    int width = 0;
    while (isdigit((u8)*f)) { width = width * 10 + (*f - '0'); ++f; }
    int len_mod = 0;
    if (f[0] == 'l' && f[1] == 'l') { len_mod = 2; f += 2; }
    else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { len_mod = 2; f += 3; }
    else if (*f == 'l') { len_mod = 1; ++f; }
    else if (*f == 'h') { len_mod = 4; ++f; if (*f == 'h') { len_mod = 5; ++f; } }
    else if (*f == 'z') { len_mod = 2; ++f; }
    const char conv = *f++;
    if (!conv) break;
    if (conv == 'n') {
      if (!suppress) {
        void* p = ms_va_arg(ap, void*);
        if (p) *(int*)p = (int)(s - str);
      }
      continue;
    }
    if (conv == 'c') {
      const int cnt = width ? width : 1;
      char* dst = suppress ? nullptr : ms_va_arg(ap, char*);
      u64 cap = (!suppress && secure_sizes) ? ms_va_arg(ap, u64) : (u64)cnt;
      for (int i = 0; i < cnt; ++i) {
        if (!*s) return assigned ? assigned : -1;
        if (dst && (u64)i < cap) dst[i] = *s;
        ++s;
      }
      if (!suppress) ++assigned;
      continue;
    }
    while (isspace((u8)*s)) ++s;
    if (!*s) return assigned ? assigned : -1;
    if (conv == 's') {
      char* dst = suppress ? nullptr : ms_va_arg(ap, char*);
      u64 cap = (!suppress && secure_sizes) ? ms_va_arg(ap, u64) : 65536u;
      if (cap == 0) return assigned;
      u64 n = 0;
      while (*s && !isspace((u8)*s) && (!width || (int)n < width)) {
        if (dst && n + 1 < cap) dst[n] = *s;
        ++n; ++s;
      }
      if (n == 0) return assigned;
      if (dst) dst[n < cap ? n : cap - 1] = 0;
      if (!suppress) ++assigned;
    } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
      char tmp[128];
      int n = 0;
      const char* start = s;
      if ((conv == 'd' || conv == 'i') && (*s == '-' || *s == '+') && (!width || n < width)) {
        tmp[n++] = *s++;
      }
      while (*s && n < 126 && (!width || n < width)) {
        bool ok = false;
        if (conv == 'x' || conv == 'X') ok = isxdigit((u8)*s) || ((n <= 2) && (*s == 'x' || *s == 'X'));
        else if (conv == 'o') ok = (*s >= '0' && *s <= '7');
        else if (conv == 'i') ok = isxdigit((u8)*s) || *s == 'x' || *s == 'X';
        else ok = isdigit((u8)*s) != 0;
        if (!ok) break;
        tmp[n++] = *s++;
      }
      tmp[n] = 0;
      char* endp = nullptr;
      const int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : (conv == 'i' ? 0 : 10));
      if (conv == 'd' || conv == 'i') {
        long long v = strtoll(tmp, &endp, base);
        if (!endp || endp == tmp) { s = start; return assigned; }
        s = start + (endp - tmp);
        if (!suppress) {
          void* p = ms_va_arg(ap, void*);
          if (len_mod == 2) *(i64*)p = (i64)v;
          else if (len_mod == 4) *(i16*)p = (i16)v;
          else if (len_mod == 5) *(signed char*)p = (signed char)v;
          else *(i32*)p = (i32)v;
          ++assigned;
        }
      } else {
        unsigned long long v = strtoull(tmp, &endp, base);
        if (!endp || endp == tmp) { s = start; return assigned; }
        s = start + (endp - tmp);
        if (!suppress) {
          void* p = ms_va_arg(ap, void*);
          if (len_mod == 2) *(u64*)p = (u64)v;
          else if (len_mod == 4) *(u16*)p = (u16)v;
          else if (len_mod == 5) *(u8*)p = (u8)v;
          else *(u32*)p = (u32)v;
          ++assigned;
        }
      }
    } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
      char tmp[128];
      int n = 0;
      const char* start = s;
      while (*s && !isspace((u8)*s) && n < 126 && (!width || n < width)) tmp[n++] = *s++;
      tmp[n] = 0;
      char* endp = nullptr;
      double d = strtod(tmp, &endp);
      if (!endp || endp == tmp) { s = start; return assigned; }
      s = start + (endp - tmp);
      if (!suppress) {
        void* p = ms_va_arg(ap, void*);
        if (len_mod == 1) *(double*)p = d;
        else *(float*)p = (float)d;
        ++assigned;
      }
    } else {
      return assigned;
    }
  }
  return assigned;
}

i32 TRT_GUEST_ABI crt_sscanf(const char* str, const char* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int r = sscanf_impl(str, fmt, ap, false);
  ms_va_end(ap);
  return r;
}
i32 TRT_GUEST_ABI crt_sscanf_s(const char* str, const char* fmt, ...) {
  ms_va_list ap;
  ms_va_start(ap, fmt);
  int r = sscanf_impl(str, fmt, ap, true);
  ms_va_end(ap);
  return r;
}

// ---------- Additional C math / string / wide / VFS filesystem APIs -------
double TRT_GUEST_ABI crt_sin(double x) { return sin(x); }
double TRT_GUEST_ABI crt_cos(double x) { return cos(x); }
double TRT_GUEST_ABI crt_sqrt(double x) { return sqrt(x); }
double TRT_GUEST_ABI crt_pow(double x, double y) { return pow(x, y); }
double TRT_GUEST_ABI crt_floor(double x) { return floor(x); }
double TRT_GUEST_ABI crt_fmod(double x, double y) { return fmod(x, y); }
double TRT_GUEST_ABI crt_atan2(double y, double x) { return atan2(y, x); }
double TRT_GUEST_ABI crt_fabs(double x) { return fabs(x); }
double TRT_GUEST_ABI crt_exp(double x) { return exp(x); }
double TRT_GUEST_ABI crt_log(double x) { return log(x); }
double TRT_GUEST_ABI crt_log2(double x) { return log2(x); }
double TRT_GUEST_ABI crt_trunc(double x) { return trunc(x); }
double TRT_GUEST_ABI crt_round(double x) { return round(x); }
double TRT_GUEST_ABI crt_ldexp(double x, i32 e) { return ldexp(x, e); }
double TRT_GUEST_ABI crt_frexp(double x, i32* e) { int ee = 0; double r = frexp(x, &ee); if (e) *e = ee; return r; }
double TRT_GUEST_ABI crt_modf(double x, double* iptr) { return modf(x, iptr); }
float  TRT_GUEST_ABI crt_sinf(float x) { return sinf(x); }
float  TRT_GUEST_ABI crt_cosf(float x) { return cosf(x); }
float  TRT_GUEST_ABI crt_sqrtf(float x) { return sqrtf(x); }
float  TRT_GUEST_ABI crt_powf(float x, float y) { return powf(x, y); }
float  TRT_GUEST_ABI crt_floorf(float x) { return floorf(x); }
float  TRT_GUEST_ABI crt_fmodf(float x, float y) { return fmodf(x, y); }
float  TRT_GUEST_ABI crt_atan2f(float y, float x) { return atan2f(y, x); }
float  TRT_GUEST_ABI crt_fabsf(float x) { return fabsf(x); }
float  TRT_GUEST_ABI crt_expf(float x) { return expf(x); }
float  TRT_GUEST_ABI crt_logf(float x) { return logf(x); }
i32    TRT_GUEST_ABI crt__isnan(double x) { return isnan(x) ? 1 : 0; }
i32    TRT_GUEST_ABI crt__finite(double x) { return isfinite(x) ? 1 : 0; }
double TRT_GUEST_ABI crt__copysign(double x, double y) { return copysign(x, y); }

u16* TRT_GUEST_ABI crt_wcscat(u16* d, const u16* s) {
  u16* p = d + crt_wcslen(d);
  while ((*p++ = *s++) != 0) {}
  return d;
}
u16* TRT_GUEST_ABI crt_wcsncat(u16* d, const u16* s, u64 n) {
  u16* p = d + crt_wcslen(d);
  u64 i = 0;
  for (; i < n && s[i]; ++i) p[i] = s[i];
  p[i] = 0;
  return d;
}
u16* TRT_GUEST_ABI crt_wcsncpy(u16* d, const u16* s, u64 n) {
  u64 i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
i32 TRT_GUEST_ABI crt_wcsncmp(const u16* a, const u16* b, u64 n) {
  for (u64 i = 0; i < n; ++i) {
    if (a[i] != b[i] || a[i] == 0) return (i32)a[i] - (i32)b[i];
  }
  return 0;
}
u16* TRT_GUEST_ABI crt_wcschr(const u16* s, u16 c) {
  for (; *s; ++s) if (*s == c) return (u16*)s;
  return (c == 0) ? (u16*)s : nullptr;
}
u16* TRT_GUEST_ABI crt_wcsrchr(const u16* s, u16 c) {
  const u16* last = nullptr;
  for (; *s; ++s) if (*s == c) last = s;
  return (c == 0) ? (u16*)s : (u16*)last;
}
u16* TRT_GUEST_ABI crt_wcsstr(const u16* h, const u16* n) {
  if (!*n) return (u16*)h;
  const u64 nlen = crt_wcslen(n);
  for (; *h; ++h) if (crt_wcsncmp(h, n, nlen) == 0) return (u16*)h;
  return nullptr;
}
i32 TRT_GUEST_ABI crt__wcsicmp(const u16* a, const u16* b) {
  while (*a && *b) {
    int d = (int)crt_towlower(*a) - (int)crt_towlower(*b);
    if (d) return d;
    ++a; ++b;
  }
  return (int)crt_towlower(*a) - (int)crt_towlower(*b);
}
i32 TRT_GUEST_ABI crt__wcsnicmp(const u16* a, const u16* b, u64 n) {
  for (u64 i = 0; i < n; ++i) {
    int d = (int)crt_towlower(a[i]) - (int)crt_towlower(b[i]);
    if (d || a[i] == 0) return d;
  }
  return 0;
}
i32 TRT_GUEST_ABI crt__strnicmp(const char* a, const char* b, u64 n) {
  for (u64 i = 0; i < n; ++i) {
    int d = tolower((u8)a[i]) - tolower((u8)b[i]);
    if (d || a[i] == 0) return d;
  }
  return 0;
}
char* TRT_GUEST_ABI crt__strlwr(char* s) {
  for (char* p = s; p && *p; ++p) *p = (char)tolower((u8)*p);
  return s;
}
u16* TRT_GUEST_ABI crt__wcslwr(u16* s) {
  for (u16* p = s; p && *p; ++p) *p = crt_towlower(*p);
  return s;
}
u16* TRT_GUEST_ABI crt__wcsdup(const u16* s) {
  if (!s) return nullptr;
  const size_t bytes = (size_t)(crt_wcslen(s) + 1) * 2u;
  u16* p = (u16*)crt_malloc((u64)bytes);  // charged + tracked
  if (p) memcpy(p, s, bytes);
  return p;
}
u64 TRT_GUEST_ABI crt_wcstoul(const u16* s, u16** end, i32 base) {
  char buf[128];
  u16_to_ascii(s, buf, sizeof(buf));
  char* e8 = nullptr;
  unsigned long v = strtoul(buf, &e8, base);
  if (end) *end = (u16*)(s + (e8 ? (size_t)(e8 - buf) : 0));
  return (u64)v;
}
i64 TRT_GUEST_ABI crt__wcstoi64(const u16* s, u16** end, i32 base) {
  char buf[128];
  u16_to_ascii(s, buf, sizeof(buf));
  char* e8 = nullptr;
  long long v = strtoll(buf, &e8, base);
  if (end) *end = (u16*)(s + (e8 ? (size_t)(e8 - buf) : 0));
  return (i64)v;
}
u64 TRT_GUEST_ABI crt__wcstoui64(const u16* s, u16** end, i32 base) {
  char buf[128];
  u16_to_ascii(s, buf, sizeof(buf));
  char* e8 = nullptr;
  unsigned long long v = strtoull(buf, &e8, base);
  if (end) *end = (u16*)(s + (e8 ? (size_t)(e8 - buf) : 0));
  return (u64)v;
}
i32 TRT_GUEST_ABI crt_islower(i32 c)  { return islower((u8)c); }
i32 TRT_GUEST_ABI crt_isupper(i32 c)  { return isupper((u8)c); }
i32 TRT_GUEST_ABI crt_ispunct(i32 c)  { return ispunct((u8)c); }
i32 TRT_GUEST_ABI crt_iscntrl(i32 c)  { return iscntrl((u8)c); }
i32 TRT_GUEST_ABI crt_isprint(i32 c)  { return isprint((u8)c); }
i32 TRT_GUEST_ABI crt_isgraph(i32 c)  { return isgraph((u8)c); }
i32 TRT_GUEST_ABI crt_iswdigit(u16 c) { return (c < 128) ? isdigit((u8)c) : 0; }
i32 TRT_GUEST_ABI crt_iswalpha(u16 c) { return (c < 128) ? isalpha((u8)c) : 0; }
i32 TRT_GUEST_ABI crt_iswspace(u16 c) { return (c < 128) ? isspace((u8)c) : 0; }
i32 TRT_GUEST_ABI crt_iswalnum(u16 c) { return (c < 128) ? isalnum((u8)c) : 0; }
i32 TRT_GUEST_ABI crt_iswupper(u16 c) { return (c < 128) ? isupper((u8)c) : 0; }
i32 TRT_GUEST_ABI crt_iswlower(u16 c) { return (c < 128) ? islower((u8)c) : 0; }

struct win64_stat64i32 {
  u32 st_dev;
  u16 st_ino;
  u16 st_mode;
  i16 st_nlink;
  i16 st_uid;
  i16 st_gid;
  u16 _pad;
  u32 st_rdev;
  i32 st_size;
  i64 w_atime;
  i64 w_mtime;
  i64 w_ctime;
};

i32 access_path_impl(const char* p8, const u16* p16, i32 mode) {
  if (!p8 && !p16) { set_errno_from_host(EINVAL); return -1; }
  char host[1024];
  resolve_guest_path(p8, p16, (mode & 2) != 0, host, sizeof(host));
#ifdef _WIN32
  int r = _access(host, mode & 6);
#else
  int am = 0;
  if (mode == 0) am = F_OK;
  else {
    if (mode & 4) am |= R_OK;
    if (mode & 2) am |= W_OK;
  }
  int r = access(host, am);
#endif
  if (r != 0) set_errno_from_host(errno);
  return r;
}

i32 stat_path_impl(const char* p8, const u16* p16, int kind, void* out_buf) {
  if ((!p8 && !p16) || !out_buf) { set_errno_from_host(EINVAL); return -1; }
  char host[1024];
  resolve_guest_path(p8, p16, false, host, sizeof(host));
#ifdef _WIN32
  struct _stat64 st;
  if (_stat64(host, &st) != 0) { set_errno_from_host(errno); return -1; }
#else
  struct stat st;
  if (stat(host, &st) != 0) { set_errno_from_host(errno); return -1; }
#endif
  if (kind == 0) {
    win64_stat32* o = (win64_stat32*)out_buf;
    memset(o, 0, sizeof(*o));
    o->st_mode = (u16)st.st_mode;
    o->st_nlink = (i16)st.st_nlink;
    o->st_size = (i32)st.st_size;
    o->w_atime = (i32)st.st_atime;
    o->w_mtime = (i32)st.st_mtime;
    o->w_ctime = (i32)st.st_ctime;
  } else if (kind == 1) {
    win64_stat64* o = (win64_stat64*)out_buf;
    memset(o, 0, sizeof(*o));
    o->st_mode = (u16)st.st_mode;
    o->st_nlink = (i16)st.st_nlink;
    o->st_size = (i64)st.st_size;
    o->w_atime = (i64)st.st_atime;
    o->w_mtime = (i64)st.st_mtime;
    o->w_ctime = (i64)st.st_ctime;
  } else {
    win64_stat64i32* o = (win64_stat64i32*)out_buf;
    memset(o, 0, sizeof(*o));
    o->st_mode = (u16)st.st_mode;
    o->st_nlink = (i16)st.st_nlink;
    o->st_size = (i32)st.st_size;
    o->w_atime = (i64)st.st_atime;
    o->w_mtime = (i64)st.st_mtime;
    o->w_ctime = (i64)st.st_ctime;
  }
  return 0;
}

i32 TRT_GUEST_ABI crt__access(const char* p, i32 mode) { return access_path_impl(p, nullptr, mode); }
i32 TRT_GUEST_ABI crt__waccess(const u16* p, i32 mode) { return access_path_impl(nullptr, p, mode); }
i32 TRT_GUEST_ABI crt__stat(const char* p, void* st) { return stat_path_impl(p, nullptr, 0, st); }
i32 TRT_GUEST_ABI crt__stat64(const char* p, void* st) { return stat_path_impl(p, nullptr, 1, st); }
i32 TRT_GUEST_ABI crt__stat64i32(const char* p, void* st) { return stat_path_impl(p, nullptr, 2, st); }
i32 TRT_GUEST_ABI crt__wstat(const u16* p, void* st) { return stat_path_impl(nullptr, p, 0, st); }
i32 TRT_GUEST_ABI crt__wstat64(const u16* p, void* st) { return stat_path_impl(nullptr, p, 1, st); }
i32 TRT_GUEST_ABI crt__wstat64i32(const u16* p, void* st) { return stat_path_impl(nullptr, p, 2, st); }

i32 TRT_GUEST_ABI crt__unlink(const char* p) { return crt_remove(p); }
i32 TRT_GUEST_ABI crt__wunlink(const u16* p) {
  if (!p) { set_errno_from_host(EINVAL); return -1; }
  u32 werr = 0;
  if (trt_vfs_delete_file_utf16(p, &werr) == 0) return 0;
  set_errno_from_host(werr == 5 ? EACCES : ENOENT);
  return -1;
}
i32 TRT_GUEST_ABI crt__mkdir(const char* p) {
  if (!p) return -1;
  char host[1024];
  resolve_guest_path(p, nullptr, true, host, sizeof(host));
  trt_vfs_ensure_parents(host);
#ifdef _WIN32
  int r = _mkdir(host);
#else
  int r = mkdir(host, 0755);
#endif
  if (r == 0) {
    u16 w[512];
    ascii_to_u16(p, w, 512);
    trt_vfs_mark_z_deleted_utf16(w, 0, 1);
  }
  return r;
}
i32 TRT_GUEST_ABI crt__wmkdir(const u16* p) {
  if (!p) return -1;
  char host[1024];
  resolve_guest_path(nullptr, p, true, host, sizeof(host));
  trt_vfs_ensure_parents(host);
#ifdef _WIN32
  int r = _mkdir(host);
#else
  int r = mkdir(host, 0755);
#endif
  if (r == 0) trt_vfs_mark_z_deleted_utf16(p, 0, 1);
  return r;
}
i32 TRT_GUEST_ABI crt__rmdir(const char* p) {
  if (!p) { set_errno_from_host(EINVAL); return -1; }
  u16 w[512];
  ascii_to_u16(p, w, 512);
  u32 werr = 0;
  if (trt_vfs_remove_dir_utf16(w, &werr) == 0) return 0;
  set_errno_from_host(werr == 145 ? ENOTEMPTY : ENOENT);
  return -1;
}
i32 TRT_GUEST_ABI crt__wrmdir(const u16* p) {
  if (!p) { set_errno_from_host(EINVAL); return -1; }
  u32 werr = 0;
  if (trt_vfs_remove_dir_utf16(p, &werr) == 0) return 0;
  set_errno_from_host(werr == 145 ? ENOTEMPTY : ENOENT);
  return -1;
}
typedef i32 (TRT_GUEST_ABI* pfn_MoveFileExW)(const u16*, const u16*, u32);
i32 TRT_GUEST_ABI crt__wrename(const u16* oldp, const u16* newp) {
  if (!oldp || !newp) { set_errno_from_host(EINVAL); return -1; }
  auto fn = (pfn_MoveFileExW)k32_sym("MoveFileExW");
  if (fn && fn(oldp, newp, 0)) return 0;
  set_errno_from_host(ENOENT);
  return -1;
}
i32 TRT_GUEST_ABI crt_rename(const char* oldp, const char* newp) {
  if (!oldp || !newp) { set_errno_from_host(EINVAL); return -1; }
  u16 w1[512], w2[512];
  ascii_to_u16(oldp, w1, 512);
  ascii_to_u16(newp, w2, 512);
  return crt__wrename(w1, w2);
}
i32 TRT_GUEST_ABI crt__chmod(const char* p, i32 mode) {
  if (!p) return -1;
  char host[1024];
  resolve_guest_path(p, nullptr, true, host, sizeof(host));
#ifdef _WIN32
  return _chmod(host, mode);
#else
  return chmod(host, (mode_t)mode);
#endif
}
i32 TRT_GUEST_ABI crt__wchmod(const u16* p, i32 mode) {
  if (!p) return -1;
  char host[1024];
  resolve_guest_path(nullptr, p, true, host, sizeof(host));
#ifdef _WIN32
  return _chmod(host, mode);
#else
  return chmod(host, (mode_t)mode);
#endif
}

i32 TRT_GUEST_ABI crt_fwprintf(void* gf, const u16* wfmt, ...) {
  u16 wbuf[512];
  ms_va_list ap;
  ms_va_start(ap, wfmt);
  int n = format_ms_ws_impl(wbuf, sizeof(wbuf) / sizeof(wbuf[0]), wfmt,
                            [&]() -> u64 { return ms_va_arg(ap, u64); });
  ms_va_end(ap);
  char f8[512];
  u16_to_ascii(wbuf, f8, sizeof(f8));
  int slot = -1;
  FILE* osf = os_file_of(gf, &slot);
  if (osf && slot != 1 && slot != 2) {
    fwrite(f8, 1, strlen(f8), osf);
  } else {
    trt_core_log(f8);
  }
  return n;
}

const trt_named_fn k_msvcrt_fns[] = {
    // CRT globals (data pointers)
    {"__initenv",          (void*)&g_msvcrt_initenv},
    {"_commode",           (void*)&g_msvcrt_commode},
    {"_fmode",             (void*)&g_msvcrt_fmode},
    {"_timezone",          (void*)&g_msvcrt_timezone},
    {"_tzname",            (void*)g_msvcrt_tzname},
    // Startup, locks, threads, signals, setjmp
    {"___lc_codepage_func",(void*)&crt___lc_codepage_func},
    {"___mb_cur_max_func", (void*)&crt___mb_cur_max_func},
    {"__iob_func",         (void*)&crt___iob_func},
    {"__getmainargs",      (void*)&crt___getmainargs},
    {"__set_app_type",     (void*)&crt___set_app_type},
    {"__setusermatherr",   (void*)&crt___setusermatherr},
    {"_initterm",          (void*)&crt__initterm},
    {"_initterm_e",        (void*)&crt__initterm_e},
    {"_errno",             (void*)&crt__errno},
    {"_lock",              (void*)&crt_lock},
    {"_unlock",            (void*)&crt_unlock},
    {"_getpid",            (void*)&crt__getpid},
    {"_set_error_mode",    (void*)&crt__set_error_mode},
    {"_getmaxstdio",       (void*)&crt__getmaxstdio},
    {"_setmaxstdio",       (void*)&crt__setmaxstdio},
    {"_beginthreadex",     (void*)&crt__beginthreadex},
    {"_endthreadex",       (void*)&crt__endthreadex},
    {"atexit",             (void*)&crt_atexit},
    {"_cexit",             (void*)&crt__cexit},
    {"exit",               (void*)&crt_exit},
    {"_exit",              (void*)&crt__exit},
    {"_Exit",              (void*)&crt__Exit},
    {"quick_exit",         (void*)&crt_quick_exit},
    {"abort",              (void*)&crt_abort},
    {"_amsg_exit",         (void*)&crt__amsg_exit},
    {"_assert",            (void*)&crt__assert},
    {"signal",             (void*)&crt_signal},
    {"_setjmp",            (void*)&crt__setjmp},
    {"longjmp",            (void*)&crt_longjmp},
    // Heap
    {"malloc",             (void*)&crt_malloc},
    {"calloc",             (void*)&crt_calloc},
    {"realloc",            (void*)&crt_realloc},
    {"free",               (void*)&crt_free},
    {"_aligned_malloc",    (void*)&crt__aligned_malloc},
    {"_aligned_free",      (void*)&crt__aligned_free},
    // Mem / String / Ctype
    {"memcpy",             (void*)&crt_memcpy},
    {"memmove",            (void*)&crt_memmove},
    {"memset",             (void*)&crt_memset},
    {"memcmp",             (void*)&crt_memcmp},
    {"memchr",             (void*)&crt_memchr},
    {"_memicmp",           (void*)&crt__memicmp},
    {"strlen",             (void*)&crt_strlen},
    {"strcmp",             (void*)&crt_strcmp},
    {"strncmp",            (void*)&crt_strncmp},
    {"strcoll",            (void*)&crt_strcoll},
    {"strxfrm",            (void*)&crt_strxfrm},
    {"_stricmp",           (void*)&crt__stricmp},
    {"strcpy",             (void*)&crt_strcpy},
    {"strcpy_s",           (void*)&crt_strcpy_s},
    {"strncpy",            (void*)&crt_strncpy},
    {"strcat",             (void*)&crt_strcat},
    {"strcat_s",           (void*)&crt_strcat_s},
    {"strncat",            (void*)&crt_strncat},
    {"strchr",             (void*)&crt_strchr},
    {"strrchr",            (void*)&crt_strrchr},
    {"strstr",             (void*)&crt_strstr},
    {"strpbrk",            (void*)&crt_strpbrk},
    {"strcspn",            (void*)&crt_strcspn},
    {"strspn",             (void*)&crt_strspn},
    {"_strdup",            (void*)&crt__strdup},
    {"strerror",           (void*)&crt_strerror},
    {"strtol",             (void*)&crt_strtol},
    {"strtoul",            (void*)&crt_strtoul},
    {"_strtoi64",          (void*)&crt__strtoi64},
    {"_strtoui64",         (void*)&crt__strtoui64},
    {"atoi",               (void*)&crt_atoi},
    {"atof",               (void*)&crt_atof},
    {"_itoa_s",            (void*)&crt__itoa_s},
    {"_ultoa",             (void*)&crt__ultoa},
    {"wcslen",             (void*)&crt_wcslen},
    {"wcscmp",             (void*)&crt_wcscmp},
    {"wcscoll",            (void*)&crt_wcscoll},
    {"wcsxfrm",            (void*)&crt_wcsxfrm},
    {"wcscpy",             (void*)&crt_wcscpy},
    {"wcscpy_s",           (void*)&crt_wcscpy_s},
    {"wcstol",             (void*)&crt_wcstol},
    {"mbstowcs",           (void*)&crt_mbstowcs},
    {"wcstombs",           (void*)&crt_wcstombs},
    {"isalnum",            (void*)&crt_isalnum},
    {"isalpha",            (void*)&crt_isalpha},
    {"isdigit",            (void*)&crt_isdigit},
    {"isspace",            (void*)&crt_isspace},
    {"isxdigit",           (void*)&crt_isxdigit},
    {"tolower",            (void*)&crt_tolower},
    {"toupper",            (void*)&crt_toupper},
    {"towlower",           (void*)&crt_towlower},
    {"towupper",           (void*)&crt_towupper},
    {"iswctype",           (void*)&crt_iswctype},
    // Locale / Time / Env
    {"localeconv",         (void*)&crt_localeconv},
    {"setlocale",          (void*)&crt_setlocale},
    {"_time64",            (void*)&crt__time64},
    {"_gmtime64",          (void*)&crt__gmtime64},
    {"_tzset",             (void*)&crt__tzset},
    {"clock",              (void*)&crt_clock},
    {"strftime",           (void*)&crt_strftime},
    {"wcsftime",           (void*)&crt_wcsftime},
    {"getenv",             (void*)&crt_getenv},
    {"_putenv_s",          (void*)&crt__putenv_s},
    {"_wgetenv",           (void*)&crt__wgetenv},
    {"_wputenv",           (void*)&crt__wputenv},
    {"_wputenv_s",         (void*)&crt__wputenv_s},
    // Math / Sort / Rand
    {"acos",               (void*)&crt_acos},
    {"asin",               (void*)&crt_asin},
    {"atan",               (void*)&crt_atan},
    {"ceil",               (void*)&crt_ceil},
    {"cosh",               (void*)&crt_cosh},
    {"log10",              (void*)&crt_log10},
    {"sinh",               (void*)&crt_sinh},
    {"tan",                (void*)&crt_tan},
    {"tanh",               (void*)&crt_tanh},
    {"_hypot",             (void*)&crt__hypot},
    {"_nextafter",         (void*)&crt__nextafter},
    {"srand",              (void*)&crt_srand},
    {"rand",               (void*)&crt_rand},
    {"qsort",              (void*)&crt_qsort},
    {"bsearch",            (void*)&crt_bsearch},
    // File / FD / Dir / Printf
    {"fopen",              (void*)&crt_fopen},
    {"_wfopen",            (void*)&crt__wfopen},
    {"_wfopen_s",          (void*)&crt__wfopen_s},
    {"_wfsopen",           (void*)&crt__wfsopen},
    {"freopen_s",          (void*)&crt_freopen_s},
    {"fclose",             (void*)&crt_fclose},
    {"fread",              (void*)&crt_fread},
    {"fwrite",             (void*)&crt_fwrite},
    {"fflush",             (void*)&crt_fflush},
    {"feof",               (void*)&crt_feof},
    {"ferror",             (void*)&crt_ferror},
    {"fseek",              (void*)&crt_fseek},
    {"ftell",              (void*)&crt_ftell},
    {"rewind",             (void*)&crt_rewind},
    {"fgetpos",            (void*)&crt_fgetpos},
    {"fsetpos",            (void*)&crt_fsetpos},
    {"_fseeki64",          (void*)&crt__fseeki64},
    {"_ftelli64",          (void*)&crt__ftelli64},
    {"fgets",              (void*)&crt_fgets},
    {"getc",               (void*)&crt_getc},
    {"ungetc",             (void*)&crt_ungetc},
    {"fputc",              (void*)&crt_fputc},
    {"putc",               (void*)&crt_putc},
    {"putchar",            (void*)&crt_putchar},
    {"fputs",              (void*)&crt_fputs},
    {"puts",               (void*)&crt_puts},
    {"getwc",              (void*)&crt_getwc},
    {"fputwc",             (void*)&crt_fputwc},
    {"putwc",              (void*)&crt_putwc},
    {"ungetwc",            (void*)&crt_ungetwc},
    {"setbuf",             (void*)&crt_setbuf},
    {"setvbuf",            (void*)&crt_setvbuf},
    {"remove",             (void*)&crt_remove},
    {"perror",             (void*)&crt_perror},
    {"_fileno",            (void*)&crt__fileno},
    {"_fdopen",            (void*)&crt__fdopen},
    {"_read",              (void*)&crt__read},
    {"_write",             (void*)&crt__write},
    {"_lseeki64",          (void*)&crt__lseeki64},
    {"_telli64",           (void*)&crt__telli64},
    {"_filelengthi64",     (void*)&crt__filelengthi64},
    {"_chsize",            (void*)&crt__chsize},
    {"_fstat",             (void*)&crt__fstat},
    {"_fstat64",           (void*)&crt__fstat64},
    {"_get_osfhandle",     (void*)&crt__get_osfhandle},
    {"_wchdir",            (void*)&crt__wchdir},
    {"_chdir",             (void*)&crt__chdir},
    {"_wgetcwd",           (void*)&crt__wgetcwd},
    {"_getcwd",            (void*)&crt__getcwd},
    {"_findfirst",         (void*)&crt__findfirst64i32},
    {"_findnext",          (void*)&crt__findnext64i32},
    {"_wfindfirst",        (void*)&crt__wfindfirst64i32},
    {"_wfindnext",         (void*)&crt__wfindnext64i32},
    {"_findfirst64",       (void*)&crt__findfirst64},
    {"_findnext64",        (void*)&crt__findnext64},
    {"_wfindfirst64",      (void*)&crt__wfindfirst64},
    {"_wfindnext64",       (void*)&crt__wfindnext64},
    {"_findfirst64i32",    (void*)&crt__findfirst64i32},
    {"_findnext64i32",     (void*)&crt__findnext64i32},
    {"_wfindfirst64i32",   (void*)&crt__wfindfirst64i32},
    {"_wfindnext64i32",    (void*)&crt__wfindnext64i32},
    {"_findfirst32",       (void*)&crt__findfirst32},
    {"_findnext32",        (void*)&crt__findnext32},
    {"_wfindfirst32",      (void*)&crt__wfindfirst32},
    {"_wfindnext32",       (void*)&crt__wfindnext32},
    {"_findfirst32i64",    (void*)&crt__findfirst32i64},
    {"_findnext32i64",     (void*)&crt__findnext32i64},
    {"_wfindfirst32i64",   (void*)&crt__wfindfirst32i64},
    {"_wfindnext32i64",    (void*)&crt__wfindnext32i64},
    {"_findclose",         (void*)&crt__findclose},
    {"_vsnprintf",         (void*)&crt__vsnprintf_real},
    {"_vsnprintf_s",       (void*)&crt__vsnprintf_s},
    {"_vscprintf",         (void*)&crt__vscprintf},
    {"vsnprintf",          (void*)&crt_vsnprintf_alias},
    {"snprintf",           (void*)&crt_snprintf},
    {"_snprintf",          (void*)&crt__snprintf},
    {"_snprintf_s",        (void*)&crt__snprintf_s},
    {"vsprintf",           (void*)&crt_vsprintf},
    {"vsprintf_s",         (void*)&crt_vsprintf_s},
    {"sprintf",            (void*)&crt_sprintf},
    {"sprintf_s",          (void*)&crt_sprintf_s},
    {"_vsnwprintf",        (void*)&crt__vsnwprintf},
    {"_vsnwprintf_s",      (void*)&crt__vsnwprintf_s},
    {"vswprintf",          (void*)&crt_vswprintf},
    {"_vscwprintf",        (void*)&crt__vscwprintf},
    {"_snwprintf",         (void*)&crt__snwprintf},
    {"_snwprintf_s",       (void*)&crt__snwprintf_s},
    {"swprintf",           (void*)&crt_swprintf},
    {"swprintf_s",         (void*)&crt_swprintf_s},
    {"sscanf",             (void*)&crt_sscanf},
    {"sscanf_s",           (void*)&crt_sscanf_s},
    {"vfprintf",           (void*)&crt_vfprintf},
    {"fprintf",            (void*)&crt_fprintf},
    {"fwprintf",           (void*)&crt_fwprintf},
    // Math
    {"sin",                (void*)&crt_sin},
    {"cos",                (void*)&crt_cos},
    {"sqrt",               (void*)&crt_sqrt},
    {"pow",                (void*)&crt_pow},
    {"floor",              (void*)&crt_floor},
    {"fmod",               (void*)&crt_fmod},
    {"atan2",              (void*)&crt_atan2},
    {"fabs",               (void*)&crt_fabs},
    {"exp",                (void*)&crt_exp},
    {"log",                (void*)&crt_log},
    {"log2",               (void*)&crt_log2},
    {"trunc",              (void*)&crt_trunc},
    {"round",              (void*)&crt_round},
    {"ldexp",              (void*)&crt_ldexp},
    {"frexp",              (void*)&crt_frexp},
    {"modf",               (void*)&crt_modf},
    {"sinf",               (void*)&crt_sinf},
    {"cosf",               (void*)&crt_cosf},
    {"sqrtf",              (void*)&crt_sqrtf},
    {"powf",               (void*)&crt_powf},
    {"floorf",             (void*)&crt_floorf},
    {"fmodf",              (void*)&crt_fmodf},
    {"atan2f",             (void*)&crt_atan2f},
    {"fabsf",              (void*)&crt_fabsf},
    {"expf",               (void*)&crt_expf},
    {"logf",               (void*)&crt_logf},
    {"_isnan",             (void*)&crt__isnan},
    {"_finite",            (void*)&crt__finite},
    {"_copysign",          (void*)&crt__copysign},
    // Extra strings & ctype
    {"wcscat",             (void*)&crt_wcscat},
    {"wcsncat",            (void*)&crt_wcsncat},
    {"wcsncpy",            (void*)&crt_wcsncpy},
    {"wcsncmp",            (void*)&crt_wcsncmp},
    {"wcschr",             (void*)&crt_wcschr},
    {"wcsrchr",            (void*)&crt_wcsrchr},
    {"wcsstr",             (void*)&crt_wcsstr},
    {"_wcsicmp",           (void*)&crt__wcsicmp},
    {"_wcsnicmp",          (void*)&crt__wcsnicmp},
    {"_strnicmp",          (void*)&crt__strnicmp},
    {"_strlwr",            (void*)&crt__strlwr},
    {"_wcslwr",            (void*)&crt__wcslwr},
    {"_wcsdup",            (void*)&crt__wcsdup},
    {"wcstoul",            (void*)&crt_wcstoul},
    {"_wcstoi64",          (void*)&crt__wcstoi64},
    {"_wcstoui64",         (void*)&crt__wcstoui64},
    {"islower",            (void*)&crt_islower},
    {"isupper",            (void*)&crt_isupper},
    {"ispunct",            (void*)&crt_ispunct},
    {"iscntrl",            (void*)&crt_iscntrl},
    {"isprint",            (void*)&crt_isprint},
    {"isgraph",            (void*)&crt_isgraph},
    {"iswdigit",           (void*)&crt_iswdigit},
    {"iswalpha",           (void*)&crt_iswalpha},
    {"iswspace",           (void*)&crt_iswspace},
    {"iswalnum",           (void*)&crt_iswalnum},
    {"iswupper",           (void*)&crt_iswupper},
    {"iswlower",           (void*)&crt_iswlower},
    // FS / Stat / Rename
    {"_access",            (void*)&crt__access},
    {"_waccess",           (void*)&crt__waccess},
    {"_stat",              (void*)&crt__stat},
    {"_stat64",            (void*)&crt__stat64},
    {"_stat64i32",         (void*)&crt__stat64i32},
    {"_wstat",             (void*)&crt__wstat},
    {"_wstat64",           (void*)&crt__wstat64},
    {"_wstat64i32",        (void*)&crt__wstat64i32},
    {"_unlink",            (void*)&crt__unlink},
    {"_wunlink",           (void*)&crt__wunlink},
    {"_mkdir",             (void*)&crt__mkdir},
    {"_wmkdir",            (void*)&crt__wmkdir},
    {"_rmdir",             (void*)&crt__rmdir},
    {"_wrmdir",            (void*)&crt__wrmdir},
    {"rename",             (void*)&crt_rename},
    {"_wrename",           (void*)&crt__wrename},
    {"_chmod",             (void*)&crt__chmod},
    {"_wchmod",            (void*)&crt__wchmod},
};

}  // namespace

extern "C" int trt_bridge_register_msvcrt(void) {
  trt_module_register("msvcrt.dll", TRT_ROUTE_BUILTIN, k_msvcrt_fns,
                      (int)(sizeof(k_msvcrt_fns) / sizeof(k_msvcrt_fns[0])));
  return 0;
}
