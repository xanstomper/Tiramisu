// tiramisu/src/trt/bridge_dbghelp.cpp
// dbghelp + dbgcore bridge. See bridge_dbghelp.h.
//
// Symbol search: SymInitialize succeeds and creates a real symbol-search
// record; SymFromAddr honestly fails (no PDBs on console, named log).
// MiniDumpWriteDump writes a REAL TEXT crash report — exception address,
// thread id, and the module list from the guest image registry — through
// the VFS into the game dir, returns TRUE, and names the dump path in the
// log. A real diagnostic artifact replacing the ETW we do not have.
#include "bridge_dbghelp.h"
#include "thunk.h"
#include "images.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define TRT_GETPID() (unsigned long)GetCurrentProcessId()
#else
#include <unistd.h>
#define TRT_GETPID() (unsigned long)getpid()
#endif

namespace {
[[maybe_unused]] constexpr int k_s_ok = 0;

namespace dbglog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) dbglog::line(s_)

// utf16 helper
[[maybe_unused]] void w2a(const u16* in, char* out, u32 cap) {
  u32 o = 0;
  if (in)
    for (; in[o] && o + 1 < cap; ++o) out[o] = (char)in[o];
  out[o] = 0;
}

// ---- symbol-search records (one per SymInitialize) ----
struct sym_rec { u32 tag; void* base; };
sym_rec g_sym[16];
u32 g_sym_options = 0;
void* sym_alloc(void* base) {
  for (u32 i = 0; i < 16; ++i) {
    if (!g_sym[i].tag) {
      g_sym[i].tag = 0x5359u;  // 'SY'
      g_sym[i].base = base;
      return (void*)(uintptr_t)(0x53590000u | i);
    }
  }
  return nullptr;
}
[[maybe_unused]] bool sym_valid(void* h) {
  const u32 v = (u32)(uintptr_t)h;
  return (v >> 16) == 0x5359u && (v & 0xFFFFu) < 16 && g_sym[v & 0xFFFFu].tag;
}

// the guest path a dump lands at (game dir through the VFS)
void dump_guest_path(char* out, u32 cap) {
  snprintf(out, cap, "C:\\crash-%lu.txt", (unsigned long)time(nullptr));
}

}  // namespace

extern "C" {
// defined in vfs.cpp (C linkage)
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);
}

extern "C" {

// ---- symbol search: real record, honest no-symbols answer ----
int TRT_GUEST_ABI dbg_SymInitializeW(void* proc, const u16* path, int invade) {
  char line[192];
  (void)invade;
  void* h = sym_alloc(proc);
  snprintf(line, sizeof(line),
           "dbghelp: SymInitializeW -> S_OK (symbol-search record created; "
           "search path=%s; no PDBs on console so lookups answer FALSE)",
           path ? "given" : "null");
  log_line(line);
  return h ? 1 : 0;
}

int TRT_GUEST_ABI dbg_SymInitializeA(void* proc, const char* path, int invade) {
  (void)path; (void)invade;
  char line[192];
  void* h = sym_alloc(proc);
  snprintf(line, sizeof(line),
           "dbghelp: SymInitializeA -> S_OK (symbol-search record created; "
           "no PDBs on console so lookups answer FALSE)");
  log_line(line);
  return h ? 1 : 0;
}

int TRT_GUEST_ABI dbg_SymFromAddr(void* proc, u64 addr, u64* displacement,
                                  void* symbol) {
  char line[192];
  (void)proc; (void)symbol;
  if (displacement) *displacement = 0;
  snprintf(line, sizeof(line),
           "dbghelp: SymFromAddr -> FALSE (no PDBs on console; addr=0x%llx "
           "stays unnamed)", (unsigned long long)addr);
  log_line(line);
  return 0;
}

int TRT_GUEST_ABI dbg_SymGetLineFromAddr64(void* proc, u64 addr, u32* displacement,
                                           void* line64) {
  char line[192];
  (void)proc; (void)line64;
  if (displacement) *displacement = 0;
  snprintf(line, sizeof(line),
           "dbghelp: SymGetLineFromAddr64 -> FALSE (no PDBs on console; "
           "addr=0x%llx)", (unsigned long long)addr);
  log_line(line);
  return 0;
}

u64 TRT_GUEST_ABI dbg_SymGetModuleBase64(void* proc, u64 addr) {
  char line[160];
  (void)proc;
  // Real Win32 returns the base of the module containing addr, 0 when no
  // module matches. The console bridge keeps no crash-time module table
  // (no PDBs, no symbol server), so every address stays unattributed —
  // the same honesty as SymFromAddr -> FALSE. Steam's crash handler uses
  // this for log attribution only; a 0 base is a legal non-match there.
  snprintf(line, sizeof(line),
           "dbghelp: SymGetModuleBase64 -> 0 (no module table on console; "
           "addr=0x%llx stays unattributed)", (unsigned long long)addr);
  log_line(line);
  return 0;
}

// Real Win32 returns the RUNTIME_FUNCTION (unwind info) for addr so
// StackWalk64 can unwind; the console bridge keeps no crash-time module
// table, so no entry is resolvable — NULL, the documented no-match value.
// Steam's crash handler treats this as "cannot attribute stack frames".
void* TRT_GUEST_ABI dbg_SymFunctionTableAccess64(void* proc, u64 addr) {
  char line[160];
  (void)proc;
  snprintf(line, sizeof(line),
           "dbghelp: SymFunctionTableAccess64 -> NULL (no module table on "
           "console; addr=0x%llx unwind unattributed)",
           (unsigned long long)addr);
  log_line(line);
  return nullptr;
}

// Real Win32 walks the stack via the provided machine context; without a
// module table/unwind reader it fails — FALSE is the documented failure.
// Steam logs the failure and continues (measured crash-handler shape:
// "SymInitialize() failed: %llu" path exists in its strings).
int TRT_GUEST_ABI dbg_StackWalk64(u32 machine, void* hprocess, void* hthread,
                                  void* frame, void* context,
                                  void* read_mem, void* xfer_func,
                                  void* get_module_base, void* ftab_access) {
  char line[128];
  (void)machine; (void)hprocess; (void)hthread; (void)frame; (void)context;
  (void)read_mem; (void)xfer_func; (void)get_module_base; (void)ftab_access;
  snprintf(line, sizeof(line),
           "dbghelp: StackWalk64 -> FALSE (no unwind support on console)");
  log_line(line);
  return 0;
}

int TRT_GUEST_ABI dbg_SymCleanup(void* proc) {
  for (u32 i = 0; i < 16; ++i) {
    if (g_sym[i].tag && g_sym[i].base == proc) {
      g_sym[i].tag = 0;
      g_sym[i].base = nullptr;
    }
  }
  return 1;
}

u32 TRT_GUEST_ABI dbg_SymSetOptions(u32 options) {
  const u32 prev = g_sym_options;
  g_sym_options = options;
  return prev;
}

u32 TRT_GUEST_ABI dbg_SymGetOptions(void) {
  return g_sym_options;
}

u64 TRT_GUEST_ABI dbg_SymLoadModuleEx(void* proc, void* file, const u16* name,
                                      const u16* image, u64 base, u32 size,
                                      void* data, u32 flags) {
  char line[224];
  (void)proc; (void)file; (void)data; (void)flags;
  snprintf(line, sizeof(line),
           "dbghelp: SymLoadModuleEx -> base 0x%llx registered (no symbols "
           "loadable: no PDBs on console)", (unsigned long long)base);
  log_line(line);
  return base;  // the base is real even though no symbols attach
}

void* TRT_GUEST_ABI dbg_SymRegisterFunctionEntryCallback64(void* proc,
                                                           void* cb,
                                                           u64 user) {
  char line[160];
  snprintf(line, sizeof(line),
           "dbghelp: SymRegisterFunctionEntryCallback64 -> NULL (no unwinder "
           "callback surface on console)");
  log_line(line);
  (void)proc; (void)cb; (void)user;
  return nullptr;
}

void* TRT_GUEST_ABI dbg_ImageDirectoryEntryToDataEx(void* base, int image,
                                                    u16 dir, u32* size,
                                                    void* hdr) {
  char line[160];
  snprintf(line, sizeof(line),
           "dbghelp: ImageDirectoryEntryToDataEx -> NULL (named: not "
           "implemented on console)");
  log_line(line);
  (void)base; (void)image; (void)dir; (void)size; (void)hdr;
  return nullptr;
}

// ---- MiniDumpWriteDump: a REAL text crash report ---------------------------
// Writes through the VFS into the game dir (C:\crash-<time>.txt): exception
// address, thread id, and the loaded guest module list from the image
// registry. Returns TRUE and names the dump path in the log.
int TRT_GUEST_ABI dbg_MiniDumpWriteDump(void* proc, u32 pid, void* file_h,
                                        u32 type, void* exc, void* user,
                                        void* cb) {
  (void)pid; (void)file_h; (void)type; (void)user; (void)cb;
  char line[256];

  // exception address if the caller passed an exception struct (the first
  // 8 bytes of EXCEPTION_POINTERS is the ExceptionRecord*, whose [0] is
  // ExceptionCode and [8] ExceptionAddress — dereferenced guarded)
  u64 exc_addr = 0;
  u32 exc_code = 0;
  if (exc) {
    const void* rec = *(void* const*)exc;
    if (rec) {
      exc_code = *(const u32*)rec;
      exc_addr = *(const u64*)((const u8*)rec + 8);
    }
  }

  char guest_path[128];
  dump_guest_path(guest_path, sizeof(guest_path));
  char host[1024];
  u16 wide[128];
  u32 i = 0;
  for (; guest_path[i] && i < 127; ++i) wide[i] = (u16)(u8)guest_path[i];
  wide[i] = 0;
  if (trt_vfs_resolve_utf16(wide, host, sizeof(host)) != 0) {
    snprintf(line, sizeof(line),
             "dbghelp: MiniDumpWriteDump -> FALSE (dump path unresolvable: %s)",
             guest_path);
    log_line(line);
    return 0;
  }
  FILE* f = fopen(host, "wb");
  if (!f) {
    snprintf(line, sizeof(line),
             "dbghelp: MiniDumpWriteDump -> FALSE (cannot write %s)", host);
    log_line(line);
    return 0;
  }
  fprintf(f, "TIRAMISU CRASH REPORT (text minidump)\n");
  fprintf(f, "  time: %lu\n", (unsigned long)time(nullptr));
  fprintf(f, "  exception code: 0x%08x\n", exc_code);
  fprintf(f, "  exception address: 0x%llx\n", (unsigned long long)exc_addr);
  fprintf(f, "  thread id: %lu\n", TRT_GETPID());
  fprintf(f, "  modules:\n");
  const int n = trt_images_count();
  for (int m = 0; m < n; ++m) {
    const char* name = trt_images_nth_name(m);
    trt_image* img = trt_images_nth(m);
    if (!name || !img) continue;
    fprintf(f, "    %-48s base=0x%llx size=0x%llx\n", name,
            (unsigned long long)(uintptr_t)img->base,
            (unsigned long long)img->size_of_image);
  }
  fprintf(f, "  (text minidump replaces the ETW the console does not have)\n");
  fclose(f);
  snprintf(line, sizeof(line),
           "dbghelp: MiniDumpWriteDump -> TRUE (text crash report: %s)",
           guest_path);
  log_line(line);
  return 1;
}

int trt_bridge_register_dbghelp(void) {
  static const trt_named_fn k_fns[] = {
      {"SymInitialize", (void*)&dbg_SymInitializeA},
      {"SymInitializeW", (void*)&dbg_SymInitializeW},
      {"SymInitializeA", (void*)&dbg_SymInitializeA},
      {"SymCleanup", (void*)&dbg_SymCleanup},
      {"SymSetOptions", (void*)&dbg_SymSetOptions},
      {"SymGetOptions", (void*)&dbg_SymGetOptions},
      {"SymFromAddr", (void*)&dbg_SymFromAddr},
      {"SymFromAddrW", (void*)&dbg_SymFromAddr},
      {"SymFromAddr64", (void*)&dbg_SymFromAddr},
      {"SymGetSymFromAddr64", (void*)&dbg_SymFromAddr},
      {"SymGetLineFromAddr64", (void*)&dbg_SymGetLineFromAddr64},
      {"SymGetLineFromAddrW64", (void*)&dbg_SymGetLineFromAddr64},
      {"SymGetLineFromAddr", (void*)&dbg_SymGetLineFromAddr64},
      {"SymGetModuleBase64", (void*)&dbg_SymGetModuleBase64},
      {"SymGetModuleBase", (void*)&dbg_SymGetModuleBase64},
      {"SymFunctionTableAccess64", (void*)&dbg_SymFunctionTableAccess64},
      {"SymFunctionTableAccess", (void*)&dbg_SymFunctionTableAccess64},
      {"StackWalk64", (void*)&dbg_StackWalk64},
      {"StackWalk", (void*)&dbg_StackWalk64},
      {"SymLoadModuleEx", (void*)&dbg_SymLoadModuleEx},
      {"SymRegisterFunctionEntryCallback64",
       (void*)&dbg_SymRegisterFunctionEntryCallback64},
      {"ImageDirectoryEntryToDataEx", (void*)&dbg_ImageDirectoryEntryToDataEx},
      {"MiniDumpWriteDump", (void*)&dbg_MiniDumpWriteDump},
  };
  trt_module_register("dbghelp.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  // dbgcore: the dump-write engine MiniDumpWriteDump sits on — same surface
  trt_module_register("dbgcore.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
