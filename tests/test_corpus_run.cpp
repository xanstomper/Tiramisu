// tiramisu/tests/test_corpus_run.cpp
// STAGE 6 — RUN-DEPTH: execute REAL game DLL code offline.
//
// The corpus load matrix (test_corpus.cpp) proves imports BIND ("LOAD OK").
// This harness proves the next frontier: whether real game code EXECUTES.
// The difference between "binds" and "boots" is DllMain — every game DLL
// runs its own initialization the moment Windows loads it, and that code
// exercises bridge paths no synthetic guest ever did.
//
// For every corpus PE (DLLs only — for a DLL, AddressOfEntryPoint IS
// DllMain):
//
//   1. map + bind + seal exactly like the loader tests (mmap backend,
//      trt_load_pe -> trt_map_pe -> trt_seal_image, W^X, game-dir sibling
//      search). Then EXECUTE the real entry point with the Win64 ABI:
//        INT_PTR DllMain(HINSTANCE hinst, DWORD reason=1 (PROCESS_ATTACH),
//                        LPVOID reserved=NULL)
//      on the SAME thread — the exact call images.cpp makes on console.
//
//   2. BEFORE any entry execution: SIGSEGV / SIGILL / SIGBUS handlers
//      (sigaction, SA_SIGINFO). On fault: a NAMED crash report — PE name,
//      fault address + offset from the image base, faulting RIP offset from
//      base, and the last 40 trt log lines (the loader logs every bridge
//      call via trt_core_log; the log lives in a mmap(MAP_SHARED) ring so
//      the report survives the fault) — then _exit(42).
//      A crash is a DISCOVERY ARTIFACT: the last bridge lines name which
//      surface real code hit before dying.
//
//   3. Verdicts (both legitimate; the report IS the work list):
//        RUN OK <name>: DllMain(PROCESS_ATTACH) returned.
//        RUN FAILED <name>: SIGSEGV at +0x... — last bridge: <line>
//
//   4. ONE attempt per invocation (no retry loops; you iterate by fixing
//      bridges between runs). A per-DLL watchdog (alarm) bounds a hang.
//
// Isolation: each DLL runs in a FORKED child, so a guest that corrupts the
// host process or dies unhandlable cannot take the whole matrix with it.
// The parent only decodes exit status; the child prints its own verdict.
//
// Exit 0 iff every PE got a verdict (RUN OK or RUN FAILED). A fault is a
// legitimate outcome and never fails the gate — but the report names it.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/pe.h"
#include "../src/trt/pe_exports.h"
#include "../src/trt/engine_synthesizer.h"
#include "../src/trt/input.h"
#include "../src/trt/thunk.h"
#include "../src/trt/bridge_reg.h"
#include "../src/trt/bridge_ws2.h"
#include "../src/trt/x86_cpu.h"

// bridge_sys.cpp: Win8+ temp-path (shared answer with GetTempPathW).
// Declared inside extern "C" so the guest-ABI symbol name matches.
extern "C" int __attribute__((ms_abi)) trt_GetTempPathW(u32 size, u16* buf);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utility>

// tests/corpus_ucrt_wrappers.c: ms_abi wrappers over host libc for the UCRT
// C-library surface (the MinGW DLLs' DllMain CRT init calloc's first thing —
// the blanket-0 answer NULL'd it and the guest aborted; see the wrapper file).
void* ucrt_wrap_resolve(const char* symbol);
#include <sys/stat.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// shared log ring: mmap(MAP_SHARED|MAP_ANONYMOUS) so the parent can read the
// last lines after the child dies, and so the signal handler (async-signal
// context) can dump it with raw write() only.
// ---------------------------------------------------------------------------
#define TRT_RING_LINES 512
#define TRT_RING_LINE  200
struct trt_ring {
  unsigned long count;                        // total lines written
  char last_bridge[TRT_RING_LINE];            // last "trt:" line (named surface)
  char lines[TRT_RING_LINES][TRT_RING_LINE];
};
static trt_ring* g_ring = nullptr;
static char g_pe_name[128] = "";
static unsigned long long g_image_base = 0;

static void ring_put(const char* line) {
  if (!g_ring) return;
  const unsigned long i = g_ring->count++ % TRT_RING_LINES;
  snprintf(g_ring->lines[i], TRT_RING_LINE, "%s", line);
  if (strncmp(line, "trt:", 4) == 0)
    snprintf(g_ring->last_bridge, TRT_RING_LINE, "%s", line);
}

// raw-write a whole string (async-signal-safe syscalls only in the handler)
static void wout(const char* s) {
  size_t n = strlen(s);
  while (n) { ssize_t w = write(2, s, n); if (w <= 0) break; s += w; n -= (size_t)w; }
}

// dump the last `howmany` lines of the ring with raw write()
static void ring_dump_last(int howmany) {
  if (!g_ring) return;
  const unsigned long total = g_ring->count;
  const unsigned long start = total > (unsigned long)howmany
                                  ? total - (unsigned long)howmany : 0;
  for (unsigned long i = start; i < total; ++i) {
    wout("  [trt] ");
    wout(g_ring->lines[i % TRT_RING_LINES]);
    wout("\n");
  }
}

// the ring line index of the LAST line whose text mentions bridge activity:
// the loader logs every bind ("trt: STUB <dll>!<sym>") and every bridge
// call reaches trt_thunk_dispatch which logs "trt: <kind> <dll>!<sym> called
// (unimplemented)". The LAST such line names the surface real code hit last.
static const char* last_bridge_line(void) {
  if (!g_ring || !g_ring->last_bridge[0]) return "(none — fault before any bridge call)";
  return g_ring->last_bridge;
}

// the first raw log line after a given ring index — unused hook for future
// surface back-tracing (kept out to avoid unused warnings)
// ---------------------------------------------------------------------------

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

namespace exe_boot {
void observe(const char* line);   // STAGE 7 depth-ladder detection (below)
int  child_boot(const char* path);  // the EXE boot child (below)
}

extern "C" void trt_log_ring(const char* line) {
  ring_put(line);
  exe_boot::observe(line);  // STAGE 7 depth-ladder detection
  printf("  [trt] %s\n", line);
}

// ---------------------------------------------------------------------------
// fault handler: the named crash report. Installed BEFORE entry execution.
// ---------------------------------------------------------------------------
static void fault_report(int sig, siginfo_t* si, void* uctx_v) {
  ucontext_t* uc = (ucontext_t*)uctx_v;
  unsigned long long rip = 0;
#if defined(__x86_64__)
  rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
#endif
  {
    char m[256];
    snprintf(m, sizeof(m), "FAULT: sig=%d si_code=%d addr=%p rip=0x%llx\n",
             sig, si ? si->si_code : 0, si ? si->si_addr : nullptr, rip);
    ssize_t w = ::write(2, m, strlen(m)); (void)w;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (mf) {
      char lbuf[256];
      while (fgets(lbuf, sizeof(lbuf), mf)) {
        unsigned long long start = 0, end = 0;
        if (sscanf(lbuf, "%llx-%llx", &start, &end) == 2) {
          if (rip >= start && rip < end) {
            ::write(2, "  RIP-MAP: ", 11);
            ::write(2, lbuf, strlen(lbuf));
            break;
          }
        }
      }
      fclose(mf);
    }
    unsigned char* code_bytes = (unsigned char*)(uintptr_t)rip;
    char dis[256];
    snprintf(dis, sizeof(dis), "  BYTES at RIP: %02x %02x %02x %02x %02x %02x %02x %02x (tid=%d)\n",
             code_bytes[0], code_bytes[1], code_bytes[2], code_bytes[3],
             code_bytes[4], code_bytes[5], code_bytes[6], code_bytes[7],
             (int)syscall(SYS_gettid));
    ::write(2, dis, strlen(dis));
    if (rip > 0x1000) {
      char dump[512];
      int n = snprintf(dump, sizeof(dump), "  CODE DUMP [rip-0x20..rip+0x20]:\n   ");
      const unsigned char* p = (const unsigned char*)(uintptr_t)(rip - 0x20);
      for (int i = 0; i < 64; ++i) {
        n += snprintf(dump + n, sizeof(dump) - n, " %02x", p[i]);
        if (i == 31) n += snprintf(dump + n, sizeof(dump) - n, " [RIP]");
        if ((i + 1) % 16 == 0 && i < 63) n += snprintf(dump + n, sizeof(dump) - n, "\n   ");
      }
      n += snprintf(dump + n, sizeof(dump) - n, "\n");
      ::write(2, dump, strlen(dump));
    }
    ::write(2, "BEFORE trt_dispatch_hw_fault\n", 29);
    int handled = trt_dispatch_hw_fault(sig, si, uctx_v);
    ::write(2, "AFTER trt_dispatch_hw_fault\n", 28);
    if (handled) return;
  }
  unsigned long long rsp = 0, rax = 0, rbx = 0, rcx = 0, rdx = 0, rdi = 0, rsi = 0, rbp = 0;
  unsigned long long r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
#if defined(__x86_64__)
  rsp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
  rax = (unsigned long long)uc->uc_mcontext.gregs[REG_RAX];
  rbx = (unsigned long long)uc->uc_mcontext.gregs[REG_RBX];
  rcx = (unsigned long long)uc->uc_mcontext.gregs[REG_RCX];
  rdx = (unsigned long long)uc->uc_mcontext.gregs[REG_RDX];
  rdi = (unsigned long long)uc->uc_mcontext.gregs[REG_RDI];
  rsi = (unsigned long long)uc->uc_mcontext.gregs[REG_RSI];
  rbp = (unsigned long long)uc->uc_mcontext.gregs[REG_RBP];
  r8  = (unsigned long long)uc->uc_mcontext.gregs[REG_R8];
  r9  = (unsigned long long)uc->uc_mcontext.gregs[REG_R9];
  r10 = (unsigned long long)uc->uc_mcontext.gregs[REG_R10];
  r11 = (unsigned long long)uc->uc_mcontext.gregs[REG_R11];
  r12 = (unsigned long long)uc->uc_mcontext.gregs[REG_R12];
  r13 = (unsigned long long)uc->uc_mcontext.gregs[REG_R13];
  r14 = (unsigned long long)uc->uc_mcontext.gregs[REG_R14];
  r15 = (unsigned long long)uc->uc_mcontext.gregs[REG_R15];
#endif
  const unsigned long long fault = (unsigned long long)(uintptr_t)si->si_addr;
  const char* signame = sig == SIGSEGV ? "SIGSEGV"
                      : sig == SIGILL  ? "SIGILL"
                      : sig == SIGTRAP ? "SIGTRAP"
                                       : "SIGBUS";

  char buf[TRT_RING_LINE];
  wout("\n==== TIRAMISU CRASH REPORT (discovery artifact) ====\n");
  snprintf(buf, sizeof(buf), "PE: %s (host_tid=%d)\n", g_pe_name, (int)syscall(SYS_gettid)); wout(buf);
  snprintf(buf, sizeof(buf), "signal: %s (code %d)\n", signame, si->si_code); wout(buf);
  snprintf(buf, sizeof(buf), "fault address: 0x%llx", fault);
  wout(buf);
  if (g_image_base && fault >= g_image_base) {
    snprintf(buf, sizeof(buf), "  (= image base + 0x%llx, inside the guest image)\n",
             fault - g_image_base);
    wout(buf);
  } else {
    wout("  (outside the guest image — host-side or thunk fault)\n");
  }
  snprintf(buf, sizeof(buf), "faulting RIP: 0x%llx", rip);
  wout(buf);
  if (g_image_base && rip >= g_image_base) {
    snprintf(buf, sizeof(buf), "  (= image base + 0x%llx, guest code)\n",
             rip - g_image_base);
    wout(buf);
  } else {
    wout("  (outside the guest image)\n");
  }
  snprintf(buf, sizeof(buf), "RSP: 0x%llx RAX: 0x%llx RBX: 0x%llx RCX: 0x%llx RDX: 0x%llx\n", rsp, rax, rbx, rcx, rdx); wout(buf);
  snprintf(buf, sizeof(buf), "RSI: 0x%llx RDI: 0x%llx RBP: 0x%llx R8: 0x%llx R9: 0x%llx\n", rsi, rdi, rbp, r8, r9); wout(buf);
  snprintf(buf, sizeof(buf), "R10: 0x%llx R11: 0x%llx R12: 0x%llx R13: 0x%llx R14: 0x%llx R15: 0x%llx\n", r10, r11, r12, r13, r14, r15); wout(buf);
  if (rsp) {
    wout("stack dump:\n");
    for (int si_idx = 0; si_idx < 16; ++si_idx) {
      unsigned long long* sp_ptr = (unsigned long long*)(uintptr_t)(rsp + si_idx * 8);
      // verify pointer is readable
      unsigned long long val = 0;
      if (mincore((void*)((uintptr_t)sp_ptr & ~4095ULL), 4096, (unsigned char*)&val) == 0) {
        val = *sp_ptr;
        snprintf(buf, sizeof(buf), "  [RSP+0x%02x] 0x%016llx", si_idx * 8, val);
        wout(buf);
        for (int m = 0; m < trt_images_count(); ++m) {
          trt_image* img = trt_images_nth(m);
          if (img && val >= (uintptr_t)img->base && val < (uintptr_t)img->base + img->size_of_image) {
            snprintf(buf, sizeof(buf), "  (in %s + 0x%llx)", trt_images_nth_name(m), (unsigned long long)(val - (uintptr_t)img->base));
            wout(buf);
            break;
          }
        }
        wout("\n");
      }
    }
  }
  snprintf(buf, sizeof(buf), "last bridge: %s\n", last_bridge_line()); wout(buf);
  wout("last trt log lines:\n");
  ring_dump_last(40);
  wout("==== end crash report ====\n");
  fflush(stdout);
  _exit(42);  // the named-fault exit code
}

static void install_fault_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fault_report;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // recursive depth guarded in trt_dispatch_hw_fault
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGTRAP, &sa, nullptr);
}

static void hang_kill(int, siginfo_t*, void* uctx_v) {
  ucontext_t* uc = (ucontext_t*)uctx_v;
  unsigned long long rip = 0, rsp = 0;
#if defined(__x86_64__)
  if (uc) {
    rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
    rsp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
  }
#endif
  char buf[512];
  int off = snprintf(buf, sizeof(buf),
                     "\n==== WATCHDOG SIGALRM: rip=0x%llx rsp=0x%llx frames=[",
                     rip, rsp);
  if (rsp > 0x10000) {
    const unsigned long long* sp = (const unsigned long long*)(uintptr_t)rsp;
    int shown = 0;
    for (int i = 0; i < 128 && shown < 10 && off < 460; ++i) {
      const unsigned long long v = sp[i];
      if ((v >= 0x400000ULL && v < 0x600000ULL) ||
          (v >= 0x180000000ULL && v < 0x183000000ULL)) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0x%llx ", v);
        ++shown;
      }
    }
  }
  snprintf(buf + off, sizeof(buf) - (size_t)off, "] ====\n");
  wout(buf);
  _exit(43);
}  // 43 = watchdog, distinct from 42

// 45 = SKIP verdict (a machine that is neither PE64 nor PE32: the loader
// has no path for it at all)
// 46 = LOAD-ONLY verdict (M7 Phase 1: a 32-bit guest mapped + sealed, but
// its DllMain/entry cannot execute without the Phase 2 x86 emulator)
static void print_x86_skip(const char* path, u16 machine) {
  const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  printf("SKIP %s: unsupported machine 0x%04x (neither PE64 nor PE32)\n",
         name, machine);
}

// --run32: set by the parent before fork, so every child inherits it.
static int g_run32 = 0;

// M7 Phase 2: run a PE32 image on the clean-room x86 engine.
// Load-only was the Phase 1 verdict. With --run32 the mapped 32-bit image is
// executed on the 4GB guest reservation, so the boot ladder reports what the
// guest code itself does instead of stopping at the seal. TRT_X86_HOTLOOP
// names a spin ("HOT LOOP 0x..."), and the engine's return value, instruction
// count and final EIP are printed as the verdict -- never a bare "returned".
static int run_pe32_image(trt_image* img, const char* name) {
  x86_context ctx = {};
  x86_cpu_init(&ctx);
  ctx.log = [](const char* s) {
    printf("  [x86] %s\n", s);
    trt_log_ring(s);
  };
  printf("  run32: executing PE32 entry at image+0x%llx (guest base 0x%08X)\n",
         (unsigned long long)img->entry_rva, (unsigned)img->guest_base);
  fflush(stdout);
  const int rc = x86_run_pe32(&ctx, img->base, img->size_of_image,
                              img->guest_base, img->entry_rva, 0x00100000,
                              0x00100000, img);
  printf("  run32: engine rc=%d after %llu instructions (eip=0x%08X "
         "eax=0x%08X esp=0x%08X ebp=0x%08X)\n",
         rc, (unsigned long long)ctx.insn_count, ctx.regs.eip, ctx.regs.eax,
         ctx.regs.esp, ctx.regs.ebp);
  fflush(stdout);
  x86_mem_free(&ctx);
  return rc;
}

static void print_pe32_load_only(const char* path, const char* what) {
  const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  printf("%s LOAD-ONLY %s: mapped+sealed (M7 Phase 1; execution needs the "
         "Phase 2 x86 emulator)\n", what, name);
}

// ---------------------------------------------------------------------------
// mmap backend (same shape as test_corpus / test_pe_loader)
// ---------------------------------------------------------------------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[65536];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  // STAGE 6 correction: honor the console VirtualAlloc contract — a fixed
  // preferred-base allocation FAILS when the range is already taken. The
  // old unconditional MAP_FIXED silently overmapped: baselib.dll and
  // GameAssembly.dll both prefer 0x180000000, so baselib overwrote
  // GameAssembly's first 0x3C000 bytes and its section headers (found live
  // when GameAssembly's seal read baselib's headers and its entry died on
  // corrupted pages). trt_map_pe already handles a NULL here: it retries
  // anywhere and runs the relocation pass.
  if (preferred_base) {
    const uintptr_t want = (uintptr_t)preferred_base;
    const uintptr_t want_end = want + bytes;
    int active = 0;
    for (int i = 0; i < g_extent_count; ++i) {
      if (!g_extents[i].base) continue;
      ++active;
      const uintptr_t b = (uintptr_t)g_extents[i].base;
      const uintptr_t e = b + g_extents[i].len;
      if (want < e && b < want_end) {
        char line[160];
        snprintf(line, sizeof(line),
                 "trt: preferred base %llx unavailable (extent %p..%p taken)",
                 (unsigned long long)want, g_extents[i].base,
                 (void*)(uintptr_t)e);
        trt_log_ring(line);
        return nullptr;
      }
    }
    if (active >= 65536) return nullptr;
  }
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  int slot = -1;
  for (int i = 0; i < g_extent_count; ++i) {
    if (!g_extents[i].base) { slot = i; break; }
  }
  if (slot < 0 && g_extent_count < 65536) slot = g_extent_count++;
  if (slot >= 0) {
    g_extents[slot].base = p;
    g_extents[slot].len = bytes;
  }
  return p;
}

int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}

void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}

// STAGE 7 (OrTMM): host-side .pdata unwind registration. The guest's CRT
// init walks its own x64 unwind tables (RtlLookupFunctionEntry over
// RtlCaptureContext's context) and VirtualProtects .pdata R->RW to edit the
// unwind EpiloguePacking bits — with no way to re-register the edited
// range with the HOST unwinder, the host unwinder cannot find the guest's
// function entries and the guest faults. libunwind exposes the same
// registration semantics for DWARF (.eh_frame), but not PE .pdata; the
// honest Linux-host equivalent of RtlAddFunctionTable for THIS test is a
// no-op success (the console host uses the real RtlAddFunctionTable —
// src/host/main.cpp:uwp_register_unwind). The guest still gets its
// R->RW VirtualProtect, which is what the CRT edits .pdata through.
int linux_register_unwind(void* image_base, const void* pdata, u32 count) {
  (void)image_base; (void)pdata; (void)count;
  return 0;
}

// ---------------------------------------------------------------------------
// the CONSOLE's system loader (same offline model as test_corpus.cpp —
// whitelist of P/U class DLLs; anything else routes STUB or fails named).
// ---------------------------------------------------------------------------
struct console_dll { const char* name; };
const console_dll k_console_dlls[] = {
    {"hid.dll"}, {"setupapi.dll"}, {"version.dll"}, {"shcore.dll"}, {"avrt.dll"},
    {"d3d11.dll"}, {"dxgi.dll"}, {"d2d1.dll"}, {"dwrite.dll"}, {"dcomp.dll"},
    {"xinput1_4.dll"}, {"ws2_32.dll"}, {"d3d12.dll"}, {"d3d12core.dll"},
    {"bcrypt.dll"}, {"winhttp.dll"}, {"wldap32.dll"},
    {"d3dcompiler_47.dll"}, {"d3dcompiler_46.dll"}, {"d3dcompiler_43.dll"},
    {"kernel32.dll"}, {"kernelbase.dll"}, {"ntdll.dll"}, {"ucrtbase.dll"},
    {"combase.dll"}, {"user32.dll"}, {"advapi32.dll"},
    {"vcruntime140.dll"}, {"msvcp140.dll"},
    {"vcruntime140_app.dll"}, {"msvcp140_app.dll"},
    {"dstorage.dll"}, {"dstoragecore.dll"},
    {"bink2w64.dll"}, {"binkw64.dll"}, {"bink2w32.dll"},
    {"oo2core_6_win64.dll"}, {"oo2core_8_win64.dll"}, {"oo2core_5_win64.dll"},
};

char wl_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool wl_ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (wl_lower(*a) != wl_lower(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

bool whitelist_has(const char* module) {
  if (!module) return false;
  if (wl_lower(module[0]) == 'a' && wl_lower(module[1]) == 'p' &&
      wl_lower(module[2]) == 'i' && module[3] == '-' && module[4] == 'm' &&
      module[5] == 's')
    return true;  // api-ms-win-* contract prefix
  for (const console_dll& d : k_console_dlls)
    if (wl_ieq(d.name, module)) return true;
  return false;
}

// Everything a whitelisted SYSTEM module binds to in this offline model.
// TRT_GUEST_ABI (ms_abi) — real guest code WILL call these with the Win64
// ABI in this harness (unlike the load-only corpus, where it never ran),
// so the ABI here is load-bearing: a SysV body called through the guest's
// IAT would corrupt rcx/rdx/r8/r9 arg registers.
//
// STAGE 7: the blanket "answers 0 for everything" bind turned out to be a
// real boot blocker, measured live under gdb: UnityPlayer.dll's memory
// kernel calls ntdll!NtQuerySystemInformation(SystemBasicInformation) to
// size its bucket allocator; a blanket-0 answer = "query failed" = an
// EMPTY allocator descriptor ([+0x50]=0), and the first alloc(0x78)
// takes the un-initialized slow path, returns NULL, and the caller
// SIGSEGVs storing through it (fault 0x1816da6a6, one instruction after
// the vcall — this cost a five-boot gdb trace to pin). The honest-minimal
// answer is NOT 0 for a kernel that on real Windows ANSWERS: it is the
// truthful structure the guest reads. Everything still unimplemented
// keeps the honest 0 / fail-visible answer.
#include "../src/trt/bridge_d3d.h"

static int g_mock_d3d11 = 0;

namespace mock_gpu {

struct com_obj { const void** vtbl; };

static u64 TRT_GUEST_ABI ret_zero(void) { return 0; }
static int TRT_GUEST_ABI addref_one(void*) { return 1; }
static int TRT_GUEST_ABI ret_e_notimpl(void*) { return (int)0x80004001u; }
static int TRT_GUEST_ABI ret_dxgi_not_found(void*) { return (int)0x887A0002u; }
static int TRT_GUEST_ABI qi_self(void* self, const void*, void** ppv) {
  if (!ppv) return (int)0x80004003u;
  *ppv = self;
  return 0;
}

extern com_obj g_dev, g_ctx, g_dxgi_dev, g_adapter, g_output, g_factory, g_sc, g_res;

static int guid_match(const void* g, u32 d1, u16 d2, u16 d3) {
  if (!g) return 0;
  const u32* p32 = (const u32*)g;
  const u16* p16 = (const u16*)g;
  return p32[0] == d1 && p16[2] == d2 && p16[3] == d3;
}

static int TRT_GUEST_ABI dev_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return (int)0x80004003u;
  // IUnknown / ID3D11Device / ID3D11Device1..5
  const u32* p32 = (const u32*)riid;
  if ((p32 && p32[0] == 0u && p32[1] == 0u && p32[2] == 0x000000C0u &&
       p32[3] == 0x46000000u) ||
      guid_match(riid, 0xdb6f6ddbu, 0xac77, 0x4e88) ||
      guid_match(riid, 0xa04bfb29u, 0x08ef, 0x43d6) ||
      guid_match(riid, 0x9d06dffau, 0xb057, 0x4a71) ||
      guid_match(riid, 0x8ffde202u, 0xa0e7, 0x45df)) {
    *ppv = self;
    return 0;
  }
  // IDXGIDevice / IDXGIDevice1 / IDXGIDevice2 / IDXGIDevice3
  if (guid_match(riid, 0x54ec77fau, 0x1377, 0x44e6) ||
      guid_match(riid, 0x77db970fu, 0x6276, 0x48ba) ||
      guid_match(riid, 0x05008617u, 0xfbfd, 0x4051) ||
      guid_match(riid, 0x6007896cu, 0x3244, 0x4afdu)) {
    *ppv = &g_dxgi_dev;
    return 0;
  }
  *ppv = nullptr;
  return (int)0x80004002u;  // E_NOINTERFACE
}

static int TRT_GUEST_ABI out2_res(void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI out3_res(void*, void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI out4_res(void*, void*, void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI out5_res(void*, void*, void*, void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI out6_res(void*, void*, void*, void*, void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI create_deferred_ctx(void*, u32, void** out) {
  if (out) *out = &g_ctx;
  return 0;
}
static int TRT_GUEST_ABI check_fmt_support(void*, u32, u32* out) {
  if (out) *out = 0xFFFFFFFFu;
  return 0;
}
static int TRT_GUEST_ABI check_msaa(void*, u32, u32, u32* out) {
  if (out) *out = 1;
  return 0;
}
static void TRT_GUEST_ABI check_counter_info(void*, u32* out) {
  if (out) memset(out, 0, 12);
}
static int TRT_GUEST_ABI check_feat_support(void*, u32, void* data, u32 sz) {
  if (data && sz) memset(data, 0, sz);
  return 0;
}
static u32 TRT_GUEST_ABI dev_get_fl(void*) { return 0xB000u; }
static void TRT_GUEST_ABI dev_get_ctx(void*, void** out) {
  if (out) *out = &g_ctx;
}

static u8* g_map_scratch = nullptr;
static const size_t k_map_scratch_sz = 256 * 1024 * 1024;
struct mapped_sub { void* pData; u32 RowPitch; u32 DepthPitch; };
static int TRT_GUEST_ABI ctx_map(void*, void*, u32, u32, u32, mapped_sub* m) {
  if (m) {
    if (!g_map_scratch) {
      g_map_scratch = (u8*)mmap(nullptr, k_map_scratch_sz, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    m->pData = g_map_scratch ? g_map_scratch : (void*)0x10000;
    m->RowPitch = 16384;
    m->DepthPitch = 16384 * 1024;
  }
  return 0;
}
static int TRT_GUEST_ABI ctx_get_data(void*, void*, void* data, u32 sz, u32) {
  if (data && sz) memset(data, 0, sz);
  return 0;
}
static void TRT_GUEST_ABI child_get_dev(void*, void** out) {
  if (out) *out = &g_dev;
}

static int TRT_GUEST_ABI dxgi_dev_get_adapter(void*, void** out) {
  if (out) *out = &g_adapter;
  return 0;
}
static int TRT_GUEST_ABI dxgi_dev_get_parent(void*, const void*, void** out) {
  if (out) *out = &g_adapter;
  return 0;
}
static int TRT_GUEST_ABI adapter_get_parent(void*, const void*, void** out) {
  if (out) *out = &g_factory;
  return 0;
}
static int TRT_GUEST_ABI adapter_enum_outputs(void*, u32 idx, void** out) {
  if (idx == 0 && out) { *out = &g_output; return 0; }
  if (out) *out = nullptr;
  return (int)0x887A0002u;
}
static int TRT_GUEST_ABI adapter_get_desc(void*, u8* d) {
  if (!d) return (int)0x80070057u;
  memset(d, 0, 312);
  const char* s = "Tiramisu Mock GPU";
  for (int i = 0; s[i]; ++i) { d[i * 2] = (u8)s[i]; d[i * 2 + 1] = 0; }
  *(u32*)(d + 256) = 0x1002u;           // VendorId
  *(u32*)(d + 260) = 0x13FEu;           // DeviceId
  *(u64*)(d + 272) = 4ULL << 30;        // DedicatedVideoMemory
  *(u64*)(d + 288) = 4ULL << 30;        // SharedSystemMemory
  return 0;
}
static int TRT_GUEST_ABI output_get_desc(void*, u8* d) {
  if (!d) return (int)0x80070057u;
  memset(d, 0, 96);
  const char* s = "\\\\.\\DISPLAY1";
  for (int i = 0; s[i]; ++i) { d[i * 2] = (u8)s[i]; d[i * 2 + 1] = 0; }
  *(u32*)(d + 64) = 0; *(u32*)(d + 68) = 0;
  *(u32*)(d + 72) = 1280; *(u32*)(d + 76) = 720;
  *(u32*)(d + 80) = 1;                  // AttachedToDesktop
  *(u32*)(d + 84) = 1;                  // DXGI_MODE_ROTATION_IDENTITY
  *(u64*)(d + 88) = 0x10001ULL;         // Monitor
  return 0;
}
static int TRT_GUEST_ABI output_get_modes(void*, u32 fmt, u32, u32* num, u32* modes) {
  if (!num) return (int)0x80070057u;
  if (!modes) { *num = 1; return 0; }
  if (*num == 0) return (int)0x887A0001u;
  memset(modes, 0, 28);
  modes[0] = 1280; modes[1] = 720;
  modes[2] = 60; modes[3] = 1;
  modes[4] = fmt ? fmt : 28u;
  *num = 1;
  return 0;
}
static int TRT_GUEST_ABI factory_enum_adapters(void*, u32 idx, void** out) {
  if (idx == 0 && out) { *out = &g_adapter; return 0; }
  if (out) *out = nullptr;
  return (int)0x887A0002u;
}
static int TRT_GUEST_ABI factory_create_sc(void*, void*, void*, void** out) {
  if (out) *out = &g_sc;
  return 0;
}
static int TRT_GUEST_ABI factory_create_sc_hwnd(void*, void*, void*, void*, void*, void*, void** out) {
  if (out) *out = &g_sc;
  return 0;
}
static int TRT_GUEST_ABI sc_get_buf(void*, u32, const void*, void** out) {
  if (out) *out = &g_res;
  return 0;
}
static int TRT_GUEST_ABI sc_get_containing_output(void*, void** out) {
  if (out) *out = &g_output;
  return 0;
}
static int TRT_GUEST_ABI sc_get_desc(void*, u32* d) {
  if (!d) return (int)0x80070057u;
  memset(d, 0, 72);
  d[0] = 1280; d[1] = 720; d[2] = 60; d[3] = 1; d[4] = 28;
  d[7] = 1; d[9] = 0x20; d[10] = 2; d[12] = 1;
  return 0;
}
static void TRT_GUEST_ABI res_get_desc(void*, u32* d) {
  if (!d) return;
  memset(d, 0, 44);
  d[0] = 1280; d[1] = 720; d[2] = 1; d[3] = 1; d[4] = 28; d[5] = 1; d[8] = 0x20;
}
static void TRT_GUEST_ABI res_get_type(void*, u32* t) { if (t) *t = 3; }

static const void* g_dev_vtbl[50];
static const void* g_ctx_vtbl[144];
static const void* g_dxgi_dev_vtbl[20];
static const void* g_adapter_vtbl[20];
static const void* g_output_vtbl[20];
static const void* g_factory_vtbl[32];
static const void* g_sc_vtbl[32];
static const void* g_res_vtbl[24];

com_obj g_dev      = {g_dev_vtbl};
com_obj g_ctx      = {g_ctx_vtbl};
com_obj g_dxgi_dev = {g_dxgi_dev_vtbl};
com_obj g_adapter  = {g_adapter_vtbl};
com_obj g_output   = {g_output_vtbl};
com_obj g_factory  = {g_factory_vtbl};
com_obj g_sc       = {g_sc_vtbl};
com_obj g_res      = {g_res_vtbl};

// ---- per-slot hit counters: the frame-loop probe --------------------------
// Every mock slot starts as a counting thunk (returns 0 like ret_zero) and
// records which vtbl slots the guest actually calls. Named handlers overwrite
// their slots AFTER the counting fill, so positions are never guessed — the
// histogram is measured, then the boot report prints it. This is how the
// render loop gets proven offline: thousands of context calls + a repeating
// swapchain slot = Unity's frame loop spinning; the repeating SC slot is
// Present (called once per frame).
static u64 g_dev_hits[50];
static u64 g_ctx_hits[144];
static u64 g_sc_hits[32];
template <int N> static i64 TRT_GUEST_ABI dev_thunk(void) { ++g_dev_hits[N]; return 0; }
template <int N> static i64 TRT_GUEST_ABI ctx_thunk(void) { ++g_ctx_hits[N]; return 0; }
template <int N> static i64 TRT_GUEST_ABI sc_thunk(void) { ++g_sc_hits[N]; return 0; }
template <int N> void fill_counting(const void** v) { v[N] = (const void*)&dev_thunk<N>; }
template <int... Is> void fill_counting_seq(const void** v, int which,
                                            std::integer_sequence<int, Is...>) {
  ((v[Is] = which == 0   ? (const void*)&dev_thunk<Is>
           : which == 1 ? (const void*)&ctx_thunk<Is>
                        : (const void*)&sc_thunk<Is>),
   ...);
}
static void fill_counting_all(const void** v, int n, int which) {
  if (n == 50) fill_counting_seq(v, which, std::make_integer_sequence<int, 50>{});
  else if (n == 144) fill_counting_seq(v, which, std::make_integer_sequence<int, 144>{});
  else if (n == 32) fill_counting_seq(v, which, std::make_integer_sequence<int, 32>{});
}
static void mock_dump_hits(void) {
  u64 dev_total = 0, ctx_total = 0, sc_total = 0;
  for (int i = 0; i < 50; ++i) dev_total += g_dev_hits[i];
  for (int i = 0; i < 144; ++i) ctx_total += g_ctx_hits[i];
  for (int i = 0; i < 32; ++i) sc_total += g_sc_hits[i];
  printf("  mock gpu calls        : dev=%llu ctx=%llu sc=%llu\n",
         (unsigned long long)dev_total, (unsigned long long)ctx_total,
         (unsigned long long)sc_total);
  // top-6 context slots (draws/state sets) + every hit swapchain slot
  int top[6] = {-1, -1, -1, -1, -1, -1};
  for (int i = 0; i < 144; ++i) {
    for (int k = 0; k < 6; ++k) {
      if (top[k] < 0 || g_ctx_hits[i] > g_ctx_hits[top[k]]) {
        for (int m = 5; m > k; --m) top[m] = top[m - 1];
        top[k] = i;
        break;
      }
    }
  }
  printf("  mock ctx top slots    :");
  for (int k = 0; k < 6; ++k)
    if (top[k] >= 0 && g_ctx_hits[top[k]] > 0)
      printf(" %d(x%llu)", top[k], (unsigned long long)g_ctx_hits[top[k]]);
  printf("\n  mock sc hit slots     :");
  for (int i = 0; i < 32; ++i)
    if (g_sc_hits[i])
      printf(" %d(x%llu)", i, (unsigned long long)g_sc_hits[i]);
  printf("\n");
}

static void fill_vtbl(const void** v, int n, const void* def) {
  for (int i = 0; i < n; ++i) v[i] = def;
  v[0] = (const void*)&qi_self;
  v[1] = (const void*)&addref_one;
  v[2] = (const void*)&addref_one;
}

static void init(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;
  fill_counting_all(g_dev_vtbl, 50, 0);  // counting default; named slots below
  g_dev_vtbl[0]  = (const void*)&dev_qi;
  g_dev_vtbl[3]  = (const void*)&out4_res;   // CreateBuffer
  g_dev_vtbl[4]  = (const void*)&out4_res;   // CreateTexture1D
  g_dev_vtbl[5]  = (const void*)&out4_res;   // CreateTexture2D
  g_dev_vtbl[6]  = (const void*)&out4_res;   // CreateTexture3D
  g_dev_vtbl[7]  = (const void*)&out4_res;   // CreateShaderResourceView
  g_dev_vtbl[8]  = (const void*)&out4_res;   // CreateUnorderedAccessView
  g_dev_vtbl[9]  = (const void*)&out4_res;   // CreateRenderTargetView
  g_dev_vtbl[10] = (const void*)&out4_res;   // CreateDepthStencilView
  g_dev_vtbl[11] = (const void*)&out6_res;   // CreateInputLayout
  g_dev_vtbl[12] = (const void*)&out5_res;   // CreateVertexShader
  g_dev_vtbl[13] = (const void*)&out5_res;   // CreateGeometryShader
  g_dev_vtbl[15] = (const void*)&out5_res;   // CreatePixelShader
  g_dev_vtbl[16] = (const void*)&out5_res;   // CreateHullShader
  g_dev_vtbl[17] = (const void*)&out5_res;   // CreateDomainShader
  g_dev_vtbl[18] = (const void*)&out5_res;   // CreateComputeShader
  g_dev_vtbl[19] = (const void*)&out2_res;   // CreateClassLinkage
  g_dev_vtbl[20] = (const void*)&out3_res;   // CreateBlendState
  g_dev_vtbl[21] = (const void*)&out3_res;   // CreateDepthStencilState
  g_dev_vtbl[22] = (const void*)&out3_res;   // CreateRasterizerState
  g_dev_vtbl[23] = (const void*)&out3_res;   // CreateSamplerState
  g_dev_vtbl[24] = (const void*)&out3_res;   // CreateQuery
  g_dev_vtbl[25] = (const void*)&out3_res;   // CreatePredicate
  g_dev_vtbl[26] = (const void*)&out3_res;   // CreateCounter
  g_dev_vtbl[27] = (const void*)&create_deferred_ctx;
  g_dev_vtbl[28] = (const void*)&out4_res;   // OpenSharedResource
  g_dev_vtbl[29] = (const void*)&check_fmt_support;
  g_dev_vtbl[30] = (const void*)&check_msaa;
  g_dev_vtbl[31] = (const void*)&check_counter_info;
  g_dev_vtbl[32] = (const void*)&ret_e_notimpl;
  g_dev_vtbl[33] = (const void*)&check_feat_support;
  g_dev_vtbl[34] = (const void*)&ret_dxgi_not_found;
  g_dev_vtbl[37] = (const void*)&dev_get_fl;
  g_dev_vtbl[40] = (const void*)&dev_get_ctx;

  fill_counting_all(g_ctx_vtbl, 144, 1);
  g_ctx_vtbl[3]  = (const void*)&child_get_dev;
  g_ctx_vtbl[14] = (const void*)&ctx_map;
  g_ctx_vtbl[29] = (const void*)&ctx_get_data;

  fill_vtbl(g_dxgi_dev_vtbl, 20, (const void*)&ret_zero);
  g_dxgi_dev_vtbl[5] = (const void*)&ret_dxgi_not_found;
  g_dxgi_dev_vtbl[6] = (const void*)&dxgi_dev_get_parent;
  g_dxgi_dev_vtbl[7] = (const void*)&dxgi_dev_get_adapter;

  fill_vtbl(g_adapter_vtbl, 20, (const void*)&ret_zero);
  g_adapter_vtbl[5]  = (const void*)&ret_dxgi_not_found;
  g_adapter_vtbl[6]  = (const void*)&adapter_get_parent;
  g_adapter_vtbl[7]  = (const void*)&adapter_enum_outputs;
  g_adapter_vtbl[8]  = (const void*)&adapter_get_desc;
  g_adapter_vtbl[10] = (const void*)&adapter_get_desc;

  fill_vtbl(g_output_vtbl, 20, (const void*)&ret_zero);
  g_output_vtbl[6] = (const void*)&adapter_get_parent;
  g_output_vtbl[7] = (const void*)&output_get_desc;
  g_output_vtbl[8] = (const void*)&output_get_modes;

  fill_vtbl(g_factory_vtbl, 32, (const void*)&ret_zero);
  g_factory_vtbl[5]  = (const void*)&ret_dxgi_not_found;
  g_factory_vtbl[7]  = (const void*)&factory_enum_adapters;
  g_factory_vtbl[10] = (const void*)&factory_create_sc;
  g_factory_vtbl[12] = (const void*)&factory_enum_adapters;
  g_factory_vtbl[15] = (const void*)&factory_create_sc_hwnd;

  fill_counting_all(g_sc_vtbl, 32, 2);
  g_sc_vtbl[5]  = (const void*)&ret_dxgi_not_found;
  g_sc_vtbl[6]  = (const void*)&adapter_get_parent;
  g_sc_vtbl[7]  = (const void*)&child_get_dev;
  g_sc_vtbl[9]  = (const void*)&sc_get_buf;
  g_sc_vtbl[12] = (const void*)&sc_get_desc;
  g_sc_vtbl[15] = (const void*)&sc_get_containing_output;
  g_sc_vtbl[18] = (const void*)&sc_get_desc;
  g_sc_vtbl[23] = (const void*)&ret_dxgi_not_found;

  fill_vtbl(g_res_vtbl, 24, (const void*)&ret_zero);
  g_res_vtbl[3]  = (const void*)&child_get_dev;
  g_res_vtbl[7]  = (const void*)&res_get_type;
  g_res_vtbl[10] = (const void*)&res_get_desc;

  trt_gpu_hooks hooks{};
  hooks.device = &g_dev;
  hooks.context = &g_ctx;
  hooks.swapchain = &g_sc;
  hooks.factory = &g_factory;
  hooks.feature_level = 0xB000u;
  hooks.width = 1280;
  hooks.height = 720;
  trt_gpu_set_hooks(&hooks);
}

}  // namespace mock_gpu

u64 TRT_GUEST_ABI console_hresult_nointerface(void) { return 0x80004002ULL; }

u64 TRT_GUEST_ABI console_system_fn(void) {
  // STAGE 7: name every blanket-0 bind the boot actually hits — these are
  // the next surfaces to implement (each line is a discovery artifact).
  static _Thread_local char sym_name[96] = "";
  (void)sym_name;
  return 0;
}

// ---- SYSTEM_BASIC_INFORMATION (ntdll!NtQuerySystemInformation class 0) --
// Layout (winternl.h): ULONG Reserved; ULONG TimerResolution; ULONG
// PageSize; ULONG NumberOfProcessors; ULONG Reserved2[5]; ULONG
// MinimumApplicationAddress; ULONG MaximumApplicationAddress; ...
// UnityPlayer reads PageSize (must be 4096) and the address window, and
// gates its bucket-allocator init on the query SUCCEEDING
// (NTSTATUS >= 0). Same numbers the GetSystemInfo bridge already reports.
struct sys_basic_info {
  u32 reserved;
  u32 timer_resolution;
  u32 page_size;
  u32 number_of_processors;
  u32 reserved2[5];
  u32 minimum_application_address;
  u32 maximum_application_address;
  u32 reserved3[2];
  u64 padding_to_0x30[2];  // record size UnityPlayer's copy loop assumes
};
// NTSTATUS STATUS_SUCCESS = 0; class gate: >= 0 means success
u64 TRT_GUEST_ABI console_nt_query_system_info(u32 cls, void* out, u32 len,
                                               u32* needed) {
  if (cls != 0 /*SystemBasicInformation*/) {
    // honest: unknown class = not implemented here
    if (needed) *needed = 0;
    return (u64)(i64)-1;  // STATUS_INVALID_INFO_CLASS
  }
  if (needed) *needed = (u32)sizeof(sys_basic_info);
  if (!out || len < sizeof(sys_basic_info)) return (u64)0xC0000004u; // STATUS_INFO_LENGTH_MISMATCH
  sys_basic_info* bi = (sys_basic_info*)out;
  memset(bi, 0, sizeof(*bi));
  bi->timer_resolution = 156250;   // 15.6 ms tick, real machines
  bi->page_size = 4096;
  bi->number_of_processors = 8;
  bi->minimum_application_address = 0x10000;
  bi->maximum_application_address = 0x7FFEFFFF;
  return 0;  // STATUS_SUCCESS
}

struct console_named {
  const char* module;
  const char* symbol;
  void* fn;
};

// the SYSTEM surface the boot actually queries (measured: a blanket-0
// kernel turned UnityPlayer's allocator init into an empty descriptor and
// a NULL-store SIGSEGV one boot later). Unlisted symbols keep the 0 bind.
const console_named k_console_named_fns[] = {
    {"ntdll.dll", "NtQuerySystemInformation", (void*)&console_nt_query_system_info},
    {"ntdll.dll", "ZwQuerySystemInformation", (void*)&console_nt_query_system_info},
    // STAGE 7 (dstoragecore): its obfuscation runtime jumps through IAT slot
    // 0x18005e640 — covered by NO import FirstThunk span (ntdll's ends at
    // 0x5e628) and 0 in the file: an unbound DELAY-import-style slot. The
    // runtime's lazy loader wrote the resolved advapi32!SystemFunction036
    // there; until the pe_loader binds delay imports, this slot must hand
    // back a live function or the runtime's jmp through it SIGSEGVs. Bind
    // the same honest deterministic-fill bridge (never a fake success).
    {"kernel32.dll", "SystemFunction036", (void*)&trt_bridge_SystemFunction036},
    {"kernel32.dll", "GetSystemInfo", nullptr},  // BUILTIN already answers; guard
    // Win8+ temp-path the UCRT imports; blanket-0 made the UCRT temp-name
    // loop spin to the backstop. Same guest-style answer as GetTempPathW.
    {"kernel32.dll", "GetTempPath2W", (void*)&trt_GetTempPathW},
    {"d3d12.dll", "D3D12GetInterface", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "D3D12CreateDevice", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "D3D12GetDebugInterface", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "D3D12SerializeRootSignature", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "D3D12SerializeVersionedRootSignature", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#101", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#102", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#103", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#104", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#105", (void*)&console_hresult_nointerface},
    {"d3d12.dll", "#106", (void*)&console_hresult_nointerface},
};
const int k_console_named_count =
    (int)(sizeof(k_console_named_fns) / sizeof(k_console_named_fns[0]));

void* console_system_load(const char* module, const char* symbol) {
  if (!module) return nullptr;
  if (!whitelist_has(module)) return nullptr;
  if (symbol) {
    if (strncmp(symbol, "Unity", 5) == 0) return nullptr;
    if (wl_ieq(module, "ws2_32.dll")) {
      if (void* w = trt_ws2_resolve(symbol)) return w;
    }
    // STAGE 7: named surface first — the guest reads real answers here
    for (int i = 0; i < k_console_named_count; ++i) {
      if (wl_ieq(k_console_named_fns[i].module, module) &&
          wl_ieq(k_console_named_fns[i].symbol, symbol)) {
        if (k_console_named_fns[i].fn) return k_console_named_fns[i].fn;
        break;  // named but no fn: fall through to the blanket 0 bind
      }
    }
    // MinGW/UCRT C-library surface: real ms_abi wrappers over host libc.
    // ucrtbase!calloc answered 0 -> libwinpthread's first allocation NULL'd
    // -> store fault -> EH bounce loop -> abort (measured on libstdc++-6.dll).
    if (wl_ieq(module, "ucrtbase.dll")) {
      if (void* f = ucrt_wrap_resolve(symbol)) return f;
    }
    // STAGE 7 discovery: name every blanket-0 bind of a whitelisted SYSTEM
    // symbol — each line is a candidate surface for the next fix-cycle.
    fprintf(stderr, "  [trt] SYSTEM bind (blanket-0): %s!%s\n", module, symbol);
  }
  (void)symbol;  // name or ordinal: one shared Win64-ABI address either way
  return (void*)&console_system_fn;
}

// ---------------------------------------------------------------------------
// child: map + seal + execute DllMain for ONE PE
// ---------------------------------------------------------------------------
int child_run(const char* path) {
  setvbuf(stdout, NULL, _IONBF, 0);

  install_fault_handlers();
  {
    struct sigaction sa_alrm = {};
    sa_alrm.sa_sigaction = hang_kill;
    sa_alrm.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &sa_alrm, nullptr);
  }
  alarm(180);  // ONE attempt, bounded: a hung DllMain is a named outcome too

  const char* slash = strrchr(path, '/');
  char g_game_dir[1024];
  if (slash) {
    const size_t dlen = (size_t)(slash - path);
    if (dlen < sizeof(g_game_dir)) { memcpy(g_game_dir, path, dlen); g_game_dir[dlen] = 0; }
    else g_game_dir[0] = 0;
  } else { g_game_dir[0] = '.'; g_game_dir[1] = 0; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &trt_log_ring,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);
  // STAGE 6: full boot semantics — TLS callbacks AND the entry point run
  trt_set_image_code_run(1);
  trt_runtime_init(&be, g_game_dir);
  trt_images_reset();

  const char* name = slash ? slash + 1 : path;
  snprintf(g_pe_name, sizeof(g_pe_name), "%s", name);

  // machine gate BEFORE the map attempt (same named-skip discipline as the
  // load corpus): a 32-bit DLL is a known frontier, not a loader gap.
  {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("RUN FAILED %s: cannot read file\n", name); return 44; }
    unsigned char hdr[0x400];
    const size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
      printf("RUN FAILED %s: no MZ\n", name);
      return 44;
    }
    const unsigned nt_off =
        (unsigned)hdr[0x3c] | ((unsigned)hdr[0x3d] << 8) |
        ((unsigned)hdr[0x3e] << 16) | ((unsigned)hdr[0x3f] << 24);
    if (nt_off + 8 > got) { printf("RUN FAILED %s: e_lfanew out of file\n", name); return 44; }
    const u16 machine = (u16)(hdr[nt_off + 4] | (hdr[nt_off + 5] << 8));
    if (machine == 0x014c) {
      // M7 Phase 1: load-only — map + seal, never run the 32-bit entry.
      // Code-run is forced OFF for this child (the loader's own PE32 guard
      // in images.cpp is the backstop) and run_entry=0.
      trt_set_image_code_run(0);
      trt_image* img32 = trt_load_pe(path, name, 0);
      if (!img32) {
        printf("RUN FAILED %s: PE32 map/seal (see [trt] lines — the named loader gap)\n", name);
        fflush(stdout);
        return 44;
      }
      if (g_run32 && img32->entry_rva) {
        const int rc32 = run_pe32_image(img32, name);
        if (rc32 != X86_EXEC_OK) {
          printf("RUN FAILED %s: x86 execution stopped (rc=%d)\n", name, rc32);
          fflush(stdout);
          return 44;
        }
        printf("RUN OK %s: PE32 entry finished on the x86 engine\n", name);
        fflush(stdout);
        return 0;
      }
      print_pe32_load_only(path, "RUN");
      fflush(stdout);
      return 46;
    }
    if (machine != 0x8664) {
      print_x86_skip(path, machine);
      return 45;
    }
  }

  trt_image* img = trt_load_pe(path, name, 1);
  if (!img) {
    printf("RUN FAILED %s: map/seal (see [trt] lines — the named loader gap)\n", name);
    fflush(stdout);
    return 44;
  }
  g_image_base = (unsigned long long)(uintptr_t)img->base;

  if (!img->entry_rva) {
    printf("RUN SKIPPED %s: no entry point (no DllMain to execute)\n", name);
    fflush(stdout);
    return 0;
  }

  printf("  run: executing DllMain at image+0x%llx (hinst=%p reason=1 reserved=NULL)\n",
         (unsigned long long)img->entry_rva, (void*)(uintptr_t)img->base);
  // The entry already ran inside trt_load_pe (run_entry=1, same thread,
  // Win64 ABI via trt_call_guest). Reaching here means it returned.
  printf("RUN OK %s: DllMain(PROCESS_ATTACH) returned.\n", name);
  fflush(stdout);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// parent: fork per PE, decode the child's outcome
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("usage: test_corpus_run [--exe] [--run32] <pe> [<pe> ...]\n");
    printf("  (default)   run DllMain(PROCESS_ATTACH) on each DLL\n");
    printf("  --exe       boot each EXE: CRT startup -> WinMain -> engine\n");
    printf("  --run32     execute a PE32 EXE on the x86 engine (not load-only)\n");
    printf("  --synth     classify each argument as a game DIRECTORY\n");
    return 2;
  }

  // Synthesizer phase: real-directory classification, no guest code executed.
  if (strcmp(argv[1], "--synth") == 0) {
    int bad = 0;
    for (int a = 2; a < argc; ++a) {
      trt_engine_profile p;
      printf("\n==== SYNTH %s ====\n", argv[a]);
      if (!trt_synthesize_engine(argv[a], nullptr, &p)) {
        printf("SYNTH FAILED %s: no candidate executable in directory\n", argv[a]);
        ++bad;
        continue;
      }
      printf("SYNTH %s:\n  engine=%s\n  arch=%s\n  exe=%s\n  gfx=%s\n  audio=%s\n"
             "  steam=%s appid=%u\n  imports=%zu\n",
             p.game_name.c_str(), trt_engine_type_name(p.engine_type),
             p.is_managed ? "Managed (.NET)" : (p.is_pe32 ? "PE32" : "PE64"),
             p.main_exe_rel.c_str(), trt_graphics_api_name(p.preferred_gfx),
             trt_audio_middleware_name(p.preferred_audio),
             p.has_steam ? "yes" : "no", p.steam_appid, p.imported_modules.size());
      for (const auto& m : p.imported_modules) printf("    import %s\n", m.c_str());
      for (const auto& s : p.synthetic_shims) printf("    shim   %s\n", s.c_str());
      for (const auto& c : p.cmdline_args) printf("    arg    %s\n", c.c_str());
    }
    printf("\nSYNTH SUMMARY: %d classified, %d failed\n", (argc - 2) - bad, bad);
    return bad ? 1 : 0;
  }

  g_ring = (trt_ring*)mmap(nullptr, sizeof(trt_ring), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (g_ring == MAP_FAILED) { fprintf(stderr, "ring mmap failed\n"); return 2; }
  memset(g_ring, 0, sizeof(*g_ring));

  // STAGE 7: --exe selects the EXE-entry boot phase; --mock-d3d11 installs mock COM GPU hooks (opt-in)
  int first = 1;
  int exe_mode = 0;
  while (first < argc && (strcmp(argv[first], "--exe") == 0 ||
                          strcmp(argv[first], "--mock-d3d11") == 0 ||
                          strcmp(argv[first], "--run32") == 0)) {
    if (strcmp(argv[first], "--exe") == 0) exe_mode = 1;
    else if (strcmp(argv[first], "--mock-d3d11") == 0) g_mock_d3d11 = 1;
    else { g_run32 = 1; exe_mode = 1; }  // --run32 implies the EXE boot phase
    ++first;
  }
  if (argc <= first) {
    printf("usage: test_corpus_run [--exe] [--mock-d3d11] [--run32] <pe> ...\n");
    printf("       test_corpus_run --synth <game-dir> ...\n");
    return 2;
  }

  // EXE phase: one forked boot per EXE, same isolation contract
  if (exe_mode) {
    int n_ok = 0, n_failed = 0, n_skipped = 0, n_load_only = 0;
    for (int a = first; a < argc; ++a) {
      const char* path = argv[a];
      const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
      memset(g_ring, 0, sizeof(*g_ring));
      g_image_base = 0;
      g_pe_name[0] = 0;
      printf("\n==== BOOT %s ====\n", name);
      // Synthesize + preflight in the parent, dry-run so the corpus folder is
      // not written to. The verdict names the wall before a child boots into
      // it: this is the measured preflight for a real title, not a claim.
      {
        setenv("TRT_SYNTH_DRY_RUN", "1", 1);
        std::string full = path;
        const size_t sl = full.find_last_of('/');
        std::string exe_rel = sl == std::string::npos ? full : full.substr(sl + 1);
        std::string gdir = sl == std::string::npos ? std::string(".") : full.substr(0, sl);
        trt_engine_profile p;
        if (trt_synthesize_engine(gdir.c_str(), exe_rel.c_str(), &p)) {
          trt_autonomous_provision_runtime(nullptr, &p);
          trt_host_caps caps{};
          caps.d3d11_device = g_mock_d3d11 ? 1 : 0;  // the mock COM pair, if asked
          caps.mono_host = 1;                        // this build links Mono
          trt_preflight_report rep{};
          const int nb = trt_engine_launch_preflight(&p, &caps, &rep);
          printf("PREFLIGHT %s: %u blocker(s) [%s / %s / %s] %zu note(s)\n",
                 name, (unsigned)nb, trt_engine_type_name(p.engine_type),
                 trt_graphics_api_name(p.preferred_gfx),
                 p.payload_complete ? "payload complete" : "payload incomplete",
                 rep.notes.size());
          for (const auto& l : rep.lines) printf("    BLOCK %s\n", l.c_str());
          for (const auto& n : rep.notes) printf("    note  %s\n", n.c_str());
        } else {
          printf("PREFLIGHT %s: refused (see the named synthesizer line above)\n", name);
        }
        unsetenv("TRT_SYNTH_DRY_RUN");
      }
      pid_t pid = fork();
      if (pid < 0) { printf("BOOT FAILED %s: fork failed\n", name); ++n_failed; continue; }
      if (pid == 0) _exit(exe_boot::child_boot(path));
      int st = 0;
      waitpid(pid, &st, 0);
      if (WIFEXITED(st)) {
        const int code = WEXITSTATUS(st);
        if (code == 0) ++n_ok;
        else if (code == 42) {
          printf("BOOT FAILED %s: guest fault/hang backstop (exit 42)\n", name);
          ++n_failed;
        } else if (code == 43) {
          printf("BOOT FAILED %s: watchdog exit 43\n", name);
          ++n_failed;
        } else if (code == 44 || code == 45 || code == 46) {
          if (code == 45) ++n_skipped;
          else if (code == 46) ++n_load_only;  // PE32 mapped, no execution yet
          else ++n_failed;  // named by the child
        } else {
          printf("BOOT EXITED %s: child exit %d\n", name, code);
          ++n_ok;  // an ExitProcess-shaped exit is a NAMED boot outcome
        }
      } else if (WIFSIGNALED(st)) {
        printf("BOOT FAILED %s: died on signal %d without the named report\n",
               name, WTERMSIG(st));
        ring_dump_last(40);
        ++n_failed;
      } else { printf("BOOT FAILED %s: unknown wait state\n", name); ++n_failed; }
    }
    printf("\nCORPUS-EXE: %d booted, %d failed, %d skipped, %d load-only (PE32)\n",
           n_ok, n_failed, n_skipped, n_load_only);
    printf("the depth ladder + named blockers above are the deliverable.\n");
    return n_failed ? 1 : 0;
  }

  int n_ok = 0, n_failed = 0, n_no_verdict = 0, n_skipped = 0, n_load_only = 0;
  for (int a = first; a < argc; ++a) {
    const char* path = argv[a];
    const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    // fresh ring per PE (the crash report must be THIS dll's last lines)
    memset(g_ring, 0, sizeof(*g_ring));
    g_image_base = 0;
    g_pe_name[0] = 0;

    printf("-- %s --\n", name);
    pid_t pid = fork();
    if (pid < 0) {
      printf("RUN FAILED %s: fork failed\n", name);
      ++n_no_verdict;
      continue;
    }
    if (pid == 0) _exit(child_run(path));

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) {
      const int code = WEXITSTATUS(st);
      if (code == 0) { ++n_ok; }                        // verdict printed by child
      else if (code == 42) {
        printf("RUN FAILED %s: guest fault (named crash report above, exit 42)\n", name);
        ++n_failed;
      } else if (code == 43) {
        printf("RUN FAILED %s: watchdog — DllMain did not return in 180s\n", name);
        ++n_failed;
      } else if (code == 44) {
        ++n_failed;  // map/seal failure, named by the child
      } else if (code == 45) {
        ++n_skipped;  // unsupported machine — named frontier, not a failure
      } else if (code == 46) {
        ++n_load_only;  // PE32 mapped+sealed, no execution yet (M7 Phase 1)
      } else {
        printf("RUN FAILED %s: child exit %d (unnamed)\n", name, code);
        ++n_no_verdict;
      }
    } else if (WIFSIGNALED(st)) {
      printf("RUN FAILED %s: died on signal %d without the named report\n", name,
             WTERMSIG(st));
      printf("  last bridge: %s\n", last_bridge_line());
      ring_dump_last(40);
      ++n_no_verdict;
    } else {
      printf("RUN FAILED %s: unknown wait state\n", name);
      ++n_no_verdict;
    }
  }

  printf("\nCORPUS-RUN: %d run ok, %d run failed, %d skipped, %d load-only (PE32), %d no-verdict\n",
         n_ok, n_failed, n_skipped, n_load_only, n_no_verdict);
  printf("run-depth verdicts are the work list: a RUN FAILED names the bridge\n"
         "surface real code hit last; fix it, add its own test, run again.\n");
  // Faults are legitimate discovery outcomes: the gate is "every PE got a
  // verdict", not "everything runs" — that is what keeps the harness green
  // while the work list is still open.
  return n_no_verdict ? 1 : 0;
}

// ===========================================================================
// STAGE 7 — EXE-ENTRY BOOT DEPTH: execute a real game EXE's CRT startup,
// WinMain and (if reached) the engine loop, OFFLINE, on the same PE64 path.
//
// A DLL boots through DllMain; an EXE must first run its MSVC CRT startup
// (the code before WinMain), which touches a kernel32 surface the DLL
// corpus never did: FLS, heap sizing, code pages, SEH registration,
// GetStartupInfoW, command lines, EncodePointer. Only if ALL of that
// returns honest answers does the CRT reach WinMain — and a Unity EXE's
// WinMain is one call: UnityPlayer!UnityMain2.
//
// Per EXE (one attempt, fork-isolated like the DLL phase):
//   1. map + seal the EXE with run_entry=0 (an EXE entry is NOT DllMain);
//      the GAMEDIR sibling search loads UnityPlayer.dll with DllMain=1 first.
//   2. install the ExitProcess/RaiseException traps (longjmp back here),
//      a 10s pump watchdog (trt_bridge_post_quit + WM_QUIT + named log) and
//      a SIGALRM hard backstop (exit 42).
//   3. execute the entry as `int WINAPI WinMain(hinst,0,0,0)` — Win64 ABI.
//   4. verdict from the NAMED DEPTH LADDER (grepped from the log ring):
//      CRT-STARTUP OK / WINMAIN ENTERED / UNITYMAIN2 CALLED / WINDOW
//      CREATED / D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT.
//   5. ExitProcess -> "BOOT EXITED <name>: ExitProcess(rc=<n>) at <bridge>"
//      (a legitimate, named outcome); WM_QUIT via pump -> "BOOT PUMPED".
// ===========================================================================

#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

extern "C" {
extern void (*trt_boot_exit_trap)(int code);   // defined in bridge_kernel32.cpp
extern void (*trt_boot_raise_trap)(u32 code);  // defined in bridge_kernel32.cpp
unsigned trt_msg_dispatched(void);
void trt_bridge_post_quit(int code);
void* trt_GetActiveWindow(void);  // bridge_user32.cpp: guest's top window
}

namespace exe_boot {

struct boot_state {
  jmp_buf back;              // longjmp target in the boot thread
  volatile int trapped;      // 0=running, 1=ExitProcess, 2=RaiseException
  volatile int exit_code;
  volatile unsigned raise_code;
  volatile int watchdog_fired;
  volatile int pump_started; // GetMessageW/PeekMessageW seen in the ring
  volatile int quit_seen;    // WM_QUIT observed in the log
  volatile int winmain_entered;
  volatile int crt_ok;
  volatile int unitymain2;
  volatile int window_created;
  volatile int d3d11_device;
  volatile int is_child;     // 1 = forked CreateProcessW child (onefile MAIN)
};

boot_state g_boot;

void exit_trap(int code) {
  g_boot.trapped = 1;
  g_boot.exit_code = code;
  longjmp(g_boot.back, 1);
}

void raise_trap(unsigned code) {
  g_boot.trapped = 2;
  g_boot.raise_code = code;
  longjmp(g_boot.back, 1);
}

// the watchdog: force the quit path the console's shell would eventually
// force — but measure IDLENESS, not wall time. RACCOIN under desktop load
// spends >10s in il2cpp metadata load + a 40-thread init storm; the old
// wall-clock watchdog fired WM_CLOSE mid-initialization, the shutdown raced
// the plugin load, and UnityPlayer NULL-called during teardown (measured:
// SIGSEGV at 0x0 in UnityPlayer +0xa37d63, intermittent with machine load).
// observe() stamps every trt log line as engine activity; the watchdog fires
// after 10s of SILENCE (a hung/idle pump) or a 120s hard wall (a noisy
// guest that never quits still gets bounded).
static unsigned long boot_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}
static volatile unsigned long g_last_activity_ms;
static volatile int g_activity_started;
void* watchdog_fn(void*) {
  g_last_activity_ms = boot_now_ms();
  g_activity_started = 1;
  for (int i = 0; i < 1200; ++i) {  // 100ms steps, 120s hard wall
    struct timespec ts{0, 100 * 1000000};
    nanosleep(&ts, nullptr);
    if (g_boot.quit_seen || g_boot.trapped) return nullptr;
    const unsigned long now = boot_now_ms();
    const unsigned long since_activity = now - g_last_activity_ms;
    if (since_activity < 10000ul) continue;  // engine is working: let it
    break;                                   // 10s silent: hung or idle-pump
  }
  g_boot.watchdog_fired = 1;
  trt_core_log("trt: boot-watchdog fired (10s without engine activity or 120s wall) -> posting WM_CLOSE");
  // Godot's Windows quit path is WM_CLOSE -> guest WndProc ->
  // WINDOW_EVENT_CLOSE_REQUEST -> SceneTree quit (measured against
  // godot 4.3 display_server_windows.cpp / window.cpp / scene_tree.cpp:
  // process_events uses PeekMessageW+DispatchMessageW and NEVER reads
  // WM_QUIT). Post WM_CLOSE to the guest's top window so the guest's own
  // quit logic runs; WM_QUIT stays as the queue fallback.
  void* hwnd = trt_GetActiveWindow();
  trt_msg_post(0x0010 /*WM_CLOSE*/, 0, (long long)(uintptr_t)hwnd);
  trt_msg_post(0x0012 /*WM_QUIT*/, 0, 0);
  trt_bridge_post_quit(0);
  return nullptr;
}

// depth-ladder detection: called from trt_log_ring (same thread as the guest)
void observe(const char* line) {
  if (g_activity_started) g_last_activity_ms = boot_now_ms();  // engine alive
  // NOTE: UnityMain2 is NOT detected here. It is called guest->guest (no
  // bridge line ever mentions the call) and matching "UnityMain2" anywhere
  // false-positives on the IMPORT-BIND line ("trt: bind
  // UnityPlayer.dll!UnityMain2"). The real probe patches the guest's IAT
  // slot (patch_unitymain2_probe below) and sets g_unitymain2_hit directly.
  if (strstr(line, "CreateWindowExW")) g_boot.window_created = 1;
  if ((strstr(line, "D3D11CreateDevice") && !strstr(line, "no host device") &&
       !strstr(line, "-> 0x887")) ||
      strstr(line, "d3d: FL contract")) {
    g_boot.d3d11_device = 1;
  }
  if (strstr(line, "GetMessageW") || strstr(line, "PeekMessageW"))
    g_boot.pump_started = 1;
  if (strstr(line, "WM_QUIT")) g_boot.quit_seen = 1;
}

const char* marker(int v) { return v ? "REACHED" : "-"; }

// ---- UnityMain2 call probe --------------------------------------------------
// The guest calls UnityPlayer!UnityMain2 guest->guest: no bridge runs, so no
// log line names the call. To prove the call REALLY happened (not just the
// import bind), patch the guest's IAT slot for that import with a host-side
// shim (ms_abi — Win64 callable) that sets a flag and tail-jumps to the real
// function with the argument registers intact. Executed bytes prove the
// call; a bind line alone never could. The flag/pointer live at file scope
// with C linkage: the naked asm shim references them unmangled.
extern "C" {
volatile int g_unitymain2_hit = 0;
void* g_unitymain2_real = nullptr;         // set by the patcher before entry
}
extern "C" void trt_unitymain2_probe_shim(void);  // asm shim below

// NAKED asm shim — the IAT slot points here. It must be byte-for-byte
// ABI-invisible: no prologue, no stack writes, nothing but a flag store and
// a tail-jump. A normal C shim has a prologue (push rbp/regs), so when it
// tail-jumps, the top of stack is a saved register instead of the guest's
// RETURN ADDRESS — UnityMain2 would run with a corrupted return path
// (measured: ehfix18 SIGSEGV at UnityPlayer+0x8004e2). rax/r10/r11 are
// volatile in Win64, so the guest cannot observe the clobbers; rcx/rdx/r8/r9
// (the argument registers) and rbp/rsp are untouched.
__asm__(/* guest-callable (Win64); assembled locally too */
    ".text\n"
    ".globl trt_unitymain2_probe_shim\n"
    "trt_unitymain2_probe_shim:\n"
    "  movl $1, g_unitymain2_hit(%rip)\n"
    "  movq g_unitymain2_real(%rip), %r10\n"
    "  jmp *%r10\n");

namespace {

// local readers (pe_loader's are file-local)
inline u32 probe_rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
inline u64 probe_rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

} // namespace

// Find the mapped UnityPlayer image's UnityMain2 export and the EXE's IAT
// slot that references it; overwrite the slot with the shim address.
// A named log on every failure path (fail-visible). The IAT lives in
// non-exec data: already RW on the Linux gate (everything maps RW until
// seal flips EXEC sections); no protect dance needed.
static void patch_unitymain2_probe(trt_image* exe, trt_image* up) {
  if (!exe || !up) return;
  // 1. the real export address (Win64 ABI callable). be/rs are unused for
  //    non-forwarded exports (pe_exports only consults rs for forwarders).
  void* real_fn = trt_pe_find_export(nullptr, nullptr, up, "UnityMain2", 0);
  const char* target_name = "UnityMain2";
  if (!real_fn) {
    real_fn = trt_pe_find_export(nullptr, nullptr, up, "UnityMain", 0);
    target_name = "UnityMain";
  }
  if (!real_fn) {
    trt_core_log("trt: UnityMain(2) probe: export not found in UnityPlayer (probe off)");
    return;
  }
  g_unitymain2_real = real_fn;
  // 2. walk the EXE's import directory for the UnityPlayer.dll IAT slot
  const trt_dos_header* dos = (const trt_dos_header*)exe->base;
  const trt_nt_headers64* nt =
      (const trt_nt_headers64*)(exe->base + dos->e_lfanew);
  const trt_data_directory* imp =
      &nt->optional_header.directories[TRT_PE_DIR_IMPORT];
  if (!imp->virtual_address || !imp->size) {
    trt_core_log("trt: UnityMain2 probe: EXE has no import directory (probe off)");
    return;
  }
  u64 slot = 0;
  for (const u8* d = exe->base + imp->virtual_address;; d += 20) {
    // IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk(0) TimeDateStamp(4)
    // ForwarderChain(8) Name(12) FirstThunk(16)
    const u32 oft_rva = probe_rd32(d);
    const u32 name_rva = probe_rd32(d + 12);
    const u32 first_thunk = probe_rd32(d + 16);
    if (!name_rva) break;  // terminator
    if (name_rva >= exe->size_of_image) break;
    const char* mod = (const char*)(exe->base + name_rva);
    if (first_thunk && first_thunk < exe->size_of_image &&
        strcasecmp(mod, "UnityPlayer.dll") == 0) {
      const u32 thunk_rva = oft_rva ? oft_rva : first_thunk;
      for (const u8* t = exe->base + thunk_rva; ; t += 8) {
        const u64 thunk = probe_rd64(t);
        if (!thunk) break;
        if (thunk >> 63) break;  // ordinal import: not our target
        const u32 hint_rva = (u32)(thunk & 0xFFFFFFFF);
        if (hint_rva && hint_rva + 2 < exe->size_of_image) {
          const char* sym = (const char*)(exe->base + hint_rva + 2);
          if (strcmp(sym, target_name) == 0 || strcmp(sym, "UnityMain2") == 0 || strcmp(sym, "UnityMain") == 0) {
            // the WRITABLE slot is the IAT (FirstThunk), not the lookup table
            slot = (u64)(uintptr_t)(exe->base + first_thunk + (t - (exe->base + thunk_rva)));
            break;
          }
        }
      }
    }
    if (slot) break;
  }
  if (!slot) {
    trt_core_log("trt: UnityMain2 probe: IAT slot not found in the EXE (probe off)");
    return;
  }
  // 3. point the IAT slot at the naked asm shim (ABI-invisible: flag store +
  //    tail-jump, no stack writes — the guest's return address stays on top)
  const void* shim = (const void*)(uintptr_t)&trt_unitymain2_probe_shim;
  memcpy((void*)(uintptr_t)slot, &shim, 8);
  char line[128];
  snprintf(line, sizeof(line),
           "trt: UnityMain2 probe armed (IAT slot %llx -> probe shim)",
           (unsigned long long)slot);
  trt_core_log(line);
}

// ---- the boot-execution core, shared by child_boot and the modeled child --
// img/name come from the caller; all bridge state (mounts, env store, cwd,
// heap, message ring) is INHERITED as-is. Returns the guest exit code.
struct boot_img_ref { void* base; unsigned long long entry_rva; };
static boot_img_ref g_boot_img;      // captured for the spawn hook's child
static char g_boot_name[128];

static int boot_run_entry(void* img_v, const char* name) {
  trt_image* img = (trt_image*)img_v;
  memset(&g_boot, 0, sizeof(g_boot));
  install_fault_handlers();
  {
    struct sigaction sa_alrm = {};
    sa_alrm.sa_sigaction = hang_kill;
    sa_alrm.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &sa_alrm, nullptr);
  }
  alarm(120);  // hard backstop reset for this process

  trt_boot_exit_trap = &exit_trap;
  trt_boot_raise_trap = &raise_trap;
  if (g_mock_d3d11) mock_gpu::init();
  pthread_t wd;
  pthread_create(&wd, nullptr, &watchdog_fn, nullptr);

  int rc = 0;
  if (setjmp(g_boot.back) == 0) {
    void* entry = img->base + img->entry_rva;
    printf("  boot: executing EXE entry at image+0x%llx as WinMain(hinst=%p,0,0,0)%s\n",
           (unsigned long long)img->entry_rva, (void*)(uintptr_t)img->base,
           g_boot.is_child ? "  [ONEFILE CHILD]" : "");
    // prove the UnityMain2 call by patching the EXE's IAT slot (see above)
    if (!g_boot.is_child) {
      for (int i = 0, n = trt_images_count(); i < n; ++i) {
        const char* nm = trt_images_nth_name(i);
        if (nm && strstr(nm, "UnityPlayer")) {
          patch_unitymain2_probe(img, trt_images_nth(i));
          break;
        }
      }
    }
    g_boot.crt_ok = 1;  // reaching the call means the CRT surface passed init
    trt_call_guest_img(entry, img, (u64)(uintptr_t)img->base, 0, 0, 0);
    // WinMain returned without ExitProcess: a legitimate end state
    trt_core_log("trt: BOOT WinMain returned (no ExitProcess) — pump phase follows");
    g_boot.winmain_entered = 1;
    // drive the pump from the host side: GetMessageW would block forever
    // in this model, so give the watchdog a moment, then report honestly
    for (int i = 0; i < 30 && !g_boot.quit_seen; ++i) {
      struct timespec ts{0, 100 * 1000000};
      nanosleep(&ts, nullptr);
    }
  } else {
    g_boot.winmain_entered = 1;  // the trap fired from inside WinMain's subtree
  }

  pthread_join(wd, nullptr);
  trt_boot_exit_trap = nullptr;
  trt_boot_raise_trap = nullptr;

  // ---- the NAMED DEPTH LADDER report ----
  printf("\n==== TIRAMISU BOOT DEPTH LADDER: %s%s ====\n", name,
         g_boot.is_child ? " (onefile child)" : "");
  printf("  CRT-STARTUP OK        : REACHED (CRT init called; trap-eligible)\n");
  printf("  WINMAIN ENTERED       : %s\n", marker(g_boot.winmain_entered));
  printf("  UNITYMAIN2 CALLED     : %s\n",
         g_unitymain2_hit ? "REACHED (IAT-probed)" : "-");
  printf("  WINDOW CREATED        : %s\n", marker(g_boot.window_created));
  printf("  D3D11 DEVICE ACQUIRED : %s\n", marker(g_boot.d3d11_device));
  printf("  PUMP ENTERED          : %s\n", marker(g_boot.pump_started));
  printf("  PUMP QUIT             : %s\n", marker(g_boot.quit_seen));
  printf("  messages dispatched   : %u\n", trt_msg_dispatched());
  mock_gpu::mock_dump_hits();

  if (g_boot.trapped == 1) {
    printf("\nBOOT EXITED %s: ExitProcess(rc=%d) at last bridge: %s\n", name,
           g_boot.exit_code, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  } else if (g_boot.trapped == 2) {
    printf("\nBOOT FAILED %s: guest RaiseException(0x%08x) — no SEH walk "
           "(last bridge: %s)\n", name, g_boot.raise_code, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  } else if (g_boot.quit_seen) {
    printf("\nBOOT PUMPED %s: %u messages dispatched, WM_QUIT received%s\n",
           name, trt_msg_dispatched(),
           g_boot.watchdog_fired ? " (watchdog-forced)" : " (guest-quit)");
    if (g_boot.watchdog_fired)
      printf("last trt log lines:\n"), ring_dump_last(40);
  } else {
    printf("\nBOOT STALLED %s: no ExitProcess, no WM_QUIT (last bridge: %s)\n",
           name, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  }
  printf("==== end boot report ====\n");
  fflush(stdout);
  return g_boot.exit_code;
}

// ---- the modeled child process (CreateProcessW spawn hook) ----------------
// PyInstaller's onefile parent MUST re-spawn its own exe as a child
// (pyi_utils_create_child); the console denies child processes, so the
// harness models the OS: fork a real child that re-boots the INHERITED
// image (bridge state — mounts, env store, heap, message ring — arrives
// copy-on-write, exactly what a Windows child would inherit).
static trt_image* g_spawn_img = nullptr;   // captured in child_boot
static char g_boot_path[1100];             // corpus path (for the child's reload)
static int spawn_child_exit = 0;

// runs in the FORKED CHILD: re-boot the inherited image end-to-end
static int spawn_child_main(void) {
  g_boot.is_child = 1;
  g_image_base = 0;  // crash-report classifier: re-capture in this process
  // a real Windows child starts FRESH: new image load (the fork inherited
  // the parent's mutated mapping — re-running CRT init on it faults).
  // Mounts, env store and cwd are inherited as-is, which is exactly what a
  // Windows child sees (same drives, same environment block).
  trt_images_reset();
  trt_image* img = trt_load_pe(g_boot_path, g_boot_name, 0);
  if (!img) {
    printf("  [onefile child] BOOT FAILED %s: map/seal (named loader gap)\n",
           g_boot_name);
    fflush(stdout);
    return 44;
  }
  g_spawn_img = img;
  g_image_base = (unsigned long long)(uintptr_t)img->base;
  const int rc = boot_run_entry((void*)img, g_boot_name);
  printf("  [onefile child] exit rc=%d\n", rc);
  fflush(stdout);
  return rc;
}

static void boot_spawn_hook(const char* exe_path, const char* cmdline,
                            int* pid_out) {
  (void)exe_path; (void)cmdline;
  if (pid_out) *pid_out = -1;
  trt_core_log("kernel32: spawn hook: forking onefile child (inherited image)");

  // the child reads _PYI_* through GetEnvironmentVariableW -> the bridge
  // env store (inherited); process_level resolves to MAIN, so it takes the
  // onefile-child path and does NOT recurse
  pid_t pid = fork();
  if (pid < 0) {
    trt_core_log("kernel32: spawn hook fork FAILED (named; errno visible)");
    return;
  }
  if (pid == 0) {
    _exit(spawn_child_main());
  }
  // ---- parent (the guest, still inside CreateProcessW): wait inline ----
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFEXITED(st)) {
    spawn_child_exit = WEXITSTATUS(st);
    char line[160];
    snprintf(line, sizeof(line),
             "kernel32: spawn hook: onefile child pid=%d exited rc=%d",
             pid, spawn_child_exit);
    trt_core_log(line);
  } else if (WIFSIGNALED(st)) {
    spawn_child_exit = 128 + WTERMSIG(st);
    char line[160];
    snprintf(line, sizeof(line),
             "kernel32: spawn hook: onefile child pid=%d died on signal %d",
             pid, WTERMSIG(st));
    trt_core_log(line);
  } else {
    spawn_child_exit = -1;
    trt_core_log("kernel32: spawn hook: onefile child unknown wait state");
  }
  // record for GetExitCodeProcess (the child is dead: not running)
  trt_child_running = 0;
  trt_child_exit_code = (u32)(unsigned)spawn_child_exit;
  if (pid_out) *pid_out = (int)pid;
}

int child_boot(const char* path) {
  setvbuf(stdout, NULL, _IONBF, 0);
  memset(&g_boot, 0, sizeof(g_boot));

  install_fault_handlers();
  {
    struct sigaction sa_alrm = {};
    sa_alrm.sa_sigaction = hang_kill;
    sa_alrm.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &sa_alrm, nullptr);
  }
  alarm(120);  // hard backstop: 2 minutes per EXE, then exit 42 via handler

  const char* slash = strrchr(path, '/');
  char g_game_dir[1024];
  if (slash) {
    const size_t dlen = (size_t)(slash - path);
    if (dlen < sizeof(g_game_dir)) { memcpy(g_game_dir, path, dlen); g_game_dir[dlen] = 0; }
    else g_game_dir[0] = 0;
  } else { g_game_dir[0] = '.'; g_game_dir[1] = 0; }
  const char* name = slash ? slash + 1 : path;
  snprintf(g_pe_name, sizeof(g_pe_name), "%s", name);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &trt_log_ring,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);
  trt_set_image_code_run(1);  // UnityPlayer.dll's DllMain runs on load
  trt_runtime_init(&be, g_game_dir);
  trt_images_reset();

  // STAGE 7 harness: the guest must see itself the way a real process does —
  // its own image at Z:\<name>. PyInstaller in particular re-opens the exe
  // path to read its embedded PKG archive; without the mount + exe-path it
  // dies "Could not load PyInstaller's embedded PKG archive" -> ExitProcess(-1)
  // before the CRT ever reaches WinMain. (Same contract as src/host/main.cpp
  // lines 372-374.)
  trt_vfs_mount("z:", g_game_dir);
  // STAGE 7: a real Windows process ALWAYS has a writable C: drive with an
  // existing %TEMP%. GetTempPathW answers "C:\Temp\" (matching the TEMP env)
  // and PyInstaller's UCRT _wtempnam probes that dir with
  // GetFileAttributesExW BEFORE generating any candidate name — with no C:
  // mount the probe fails forever and the candidate loop spun 2.5M opens
  // (watchdog exit 43). Mount a scratch C: and create Temp on it.
  {
    char ctpl[] = "/tmp/opencode/tiramisu-drivec.XXXXXX";
    if (mkdtemp(ctpl)) {
      trt_vfs_mount("c:", ctpl);
      char tdir[1100];
      snprintf(tdir, sizeof(tdir), "%s/Temp", ctpl);
      mkdir(tdir, 0755);
    }
  }
  char zpath[1100];
  snprintf(zpath, sizeof(zpath), "Z:\\%s", name);
  trt_bridge_set_exe_path(zpath);
  trt_bridge_set_cwd("Z:\\");
  // Optional guest argv (offline reproduction of a console launch, e.g.
  // TRT_CMD_ARGS="hello.py" test_corpus_run --exe python.exe).
  if (const char* boot_args = getenv("TRT_CMD_ARGS"))
    trt_bridge_set_cmd_args(boot_args);

  // machine gate (same discipline as the DLL phase)
  FILE* f = fopen(path, "rb");
  if (!f) { printf("BOOT FAILED %s: cannot read file\n", name); return 44; }
  unsigned char hdr[0x400];
  const size_t got = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    printf("BOOT FAILED %s: no MZ\n", name);
    return 44;
  }
  const unsigned nt_off = (unsigned)hdr[0x3c] | ((unsigned)hdr[0x3d] << 8) |
                          ((unsigned)hdr[0x3e] << 16) | ((unsigned)hdr[0x3f] << 24);
  if (nt_off + 8 > got) { printf("BOOT FAILED %s: e_lfanew out of file\n", name); return 44; }
  const u16 machine = (u16)(hdr[nt_off + 4] | (hdr[nt_off + 5] << 8));
  if (machine == 0x014c) {
    // M7 Phase 1: load-only — map + seal, never run the 32-bit entry.
    trt_set_image_code_run(0);
    trt_image* img32 = trt_load_pe(path, name, 0);
    if (!img32) {
      printf("BOOT FAILED %s: PE32 map/seal (see [trt] lines — the named loader gap)\n",
             name);
      fflush(stdout);
      return 44;
    }
    if (g_run32 && img32->entry_rva) {
      const int rc32 = run_pe32_image(img32, name);
      if (rc32 != X86_EXEC_OK) {
        printf("BOOT FAILED %s: x86 execution stopped (rc=%d)\n", name, rc32);
        fflush(stdout);
        return 44;
      }
      printf("BOOT OK %s: PE32 entry finished on the x86 engine\n", name);
      fflush(stdout);
      return 0;
    }
    print_pe32_load_only(path, "BOOT");
    fflush(stdout);
    return 46;
  }
  if (machine != 0x8664) {
    print_x86_skip(path, machine);
    return 45;
  }

  // map + seal the EXE; run_entry=0 — an EXE entry is WinMain, not DllMain
  trt_image* img = trt_load_pe(path, name, 0);
  if (!img) {
    printf("BOOT FAILED %s: map/seal (see [trt] lines — the named loader gap)\n",
           name);
    fflush(stdout);
    return 44;
  }
  g_image_base = (unsigned long long)(uintptr_t)img->base;
  if (!img->entry_rva) {
    printf("BOOT FAILED %s: no entry point\n", name);
    return 44;
  }

  // traps + watchdog (now inside boot_run_entry, shared with the modeled
  // onefile child)
  trt_boot_spawn_hook = &boot_spawn_hook;  // CreateProcessW -> real fork
  g_spawn_img = img;
  snprintf(g_boot_name, sizeof(g_boot_name), "%s", name);
  snprintf(g_boot_path, sizeof(g_boot_path), "%s", path);

  // ---- the boot ----
  const int boot_rc = boot_run_entry((void*)img, name);
  (void)boot_rc;  // the ladder report above already named the outcome; the
                  // corpus verdict counts a named ExitProcess as a booted EXE
  return 0;
}

} // namespace exe_boot
