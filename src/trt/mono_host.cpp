// tiramisu/src/trt/mono_host.cpp
// Mono embedding for managed (.NET) games — Path B (Celeste).
// See mono_host.h for the contract. The Linux path below embeds via a
// dlopen'd libmono-2.0; the Windows/UWP path embeds the statically
// linked Mono built by third_party/mono-uwp (same 6.8.0.105 runtime,
// interpreter mode forced — the AppContainer blocks JIT code
// generation). Main is entered via mono_jit_exec — the same entry the mono
// CLI uses — because it sets the entry assembly: invoking Main by hand
// via mono_runtime_invoke leaves Assembly.GetEntryAssembly() NULL, and
// Celeste's Engine static constructor dereferences it (NRE in
// Monocle.Engine..cctor). The AppDomain base directory is set to the
// game's directory so content and relative paths resolve like the CLI.
//
// FNA PLATFORM: before Main runs, trt_mono_run applies
// cfg->fna_platform as environment variables (the FNA dispatch reads
// them in its static constructor) and registers the Tiramisu backend's
// native internal calls (fna_platform.cpp) with the runtime, so the
// backend's ProgramInit finds its native bridge.
#include "mono_host.h"

#include "fna_platform.h"
#include "trt_trace.h"
#include "bridge_fmod_studio.h"  // trt_fmod_studio_pcm_cache_bytes (watchdog memory line)

#include <cstdint>
#include <cstdlib>  // malloc/free for the heap probe
#include <cstdio>
#include <csignal>  // SIGABRT handler (.126 diagnostic)
#include <cstring>
#include <vector>

namespace {

// ---- logging + last error (platform-independent) ----
void (*g_log)(const char* line) = nullptr;

void mono_log(const char* line) {
  if (g_log) {
    g_log(line);
  } else {
    std::fprintf(stderr, "mono: %s\n", line);
  }
}

char g_last_error[512] = "";

int fail(const char* detail) {
  std::snprintf(g_last_error, sizeof(g_last_error), "%s", detail);
  char line[600];
  std::snprintf(line, sizeof(line), "FAIL %s", detail);
  mono_log(line);
  return TRT_ERR_MONO;
}

// ---- PE parsing helpers (platform-independent) ----
u16 rd16(const u8* p) {
  return (u16)((u16)p[0] | ((u16)p[1] << 8));
}
u32 rd32(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

}  // namespace

extern "C" const char* trt_mono_last_error(void) { return g_last_error; }

extern "C" int trt_mono_host_present(void) {
#if defined(TRT_NO_MONO_HOST)
  return 0;  // this build has no managed runtime at all
#else
  return 1;
#endif
}

extern "C" int trt_pe_is_managed(const u8* file, size_t file_size) {
  // DOS header: MZ + e_lfanew.
  if (!file || file_size < 64) return 0;
  if (rd16(file) != 0x5A4D) return 0;
  const u32 pe_off = rd32(file + 0x3C);
  // PE signature + COFF header (20 bytes) + optional-header magic.
  if ((size_t)pe_off + 24 + 2 > file_size) return 0;
  if (std::memcmp(file + pe_off, "PE\0\0", 4) != 0) return 0;
  const u8* coff = file + pe_off + 4;
  const u16 opt_size = rd16(coff + 16);
  const u8* opt = coff + 20;
  if ((size_t)pe_off + 24 + opt_size > file_size) return 0;
  if (opt_size < 2) return 0;
  const u16 magic = rd16(opt);
  // Data-directory count lives at different offsets in PE32 (0x5C) vs
  // PE32+ (0x6C); directory 14 (COM descriptor) follows the count.
  size_t dd_base;
  if (magic == 0x10B) {  // PE32
    if (opt_size < 0x60 + 8 * 15) return 0;
    dd_base = 0x60;
  } else if (magic == 0x20B) {  // PE32+
    if (opt_size < 0x70 + 8 * 15) return 0;
    dd_base = 0x70;
  } else {
    return 0;
  }
  const u8* dd = opt + dd_base + 14 * 8;
  const u32 clr_rva = rd32(dd);
  const u32 clr_size = rd32(dd + 4);
  return clr_rva != 0 && clr_size != 0;
}

#if defined(_WIN32)
// ---- Windows/UWP: real embedding, statically linked Mono ----
// The Mono runtime is built by third_party/mono-uwp/build-mono-uwp.sh
// (stock 6.8.0.105, SGen, interpreter-capable) and linked directly
// into the exe — no dlopen/LoadLibrary: the AppContainer has no
// libmono DLL to load, and direct linking is the UWP equivalent of
// dlopen. Interpreter mode is FORCED: AppContainer processes cannot
// allocate executable memory (dynamic code generation is blocked), so
// JIT code cannot run; the interpreter is proven for Celeste on Linux.
// Console-verification-pending: this wiring is buildable + linkable,
// but whether Celeste runs under it on the Series S needs a console
// test (no UWP runtime exists on Linux).
#include <mono/utils/mono-logger.h>  // mono_trace_set_* — route Mono stderr/log into tiramisu.log
#include <atomic>
#include <chrono>
#include <thread>
#include <windows.h>  // GetModuleFileNameW, SetEnvironmentVariableW,
// GetFileAttributesA, WideCharToMultiByte

// The Mono headers provide exact declarations; the calls below are
// direct (statically linked), so a signature drift fails at link time
// instead of corrupting a call at runtime.
#include <mono/jit/jit.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/blob.h>
#include <mono/metadata/image.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/object.h>
#include <mono/metadata/exception.h>

// WinRT MemoryManager for the watchdog memory line (patch 06): the UWP
// app memory usage + limit. Inside the enclosing _WIN32 block.
#include <winrt/base.h>  // winrt::hresult_error
#include <winrt/Windows.System.h>  // MemoryManager::AppMemoryUsage

// domain.h is not shipped in the Ubuntu libmono-2.0-dev header set, so
// mono_domain_set_config is declared directly (same as the Linux path).
extern "C" void mono_domain_set_config(MonoDomain* domain, const char* base_dir,
                                       const char* config_file_name);

// mono_jit_set_aot_only is declared in mono/mini/jit.h (not in the shipped
// header set), so it is declared directly here (same as mono_domain_set_config).
extern "C" void mono_jit_set_aot_only(int aot_only);

// mono_gc_get_heap_size / mono_gc_get_used_size are declared in
// mono/metadata/mono-gc.h (mono-gc.h:118-119), which is not in the
// shipped header set, so they are declared directly here (same as
// mono_domain_set_config). Verified exported from libmonosgen-uwp.lib
// (T symbols in mono-symbols.txt). Used by the watchdog memory line.
extern "C" int64_t mono_gc_get_heap_size(void);
extern "C" int64_t mono_gc_get_used_size(void);

namespace {

// dllmaps for the assembly-load hook (same contract as Linux: set for
// the duration of one run; Mono has no hook uninstall, and one run per
// process is the contract).
const trt_mono_dllmap* g_hook_dllmaps_uwp = nullptr;
int g_hook_dllmap_count_uwp = 0;

// --- UWP Mono diagnostics: the console has no stderr, so a fault inside
// mono_jit_init used to kill the app with no log line and no crash dump.
// Route Mono's own print/log output into tiramisu.log, guard mono_jit_init
// with SEH, and record the fault address + stack so the failing native
// function can be identified from the console log alone.
void mono_print_to_log(const char* str, mono_bool is_stdout) {
  (void)is_stdout;
  if (str && *str) mono_log(str);
}
void mono_log_to_log(const char* domain, const char* level,
                     const char* message, mono_bool fatal, void* user_data) {
  (void)user_data;
  char line[1024];
  std::snprintf(line, sizeof(line), "mono[%s/%s]%s %s",
                domain ? domain : "?", level ? level : "?",
                fatal ? " FATAL" : "", message ? message : "");
  mono_log(line);
}
void mono_unhandled_hook(MonoObject* exc, void* user_data) {
  (void)user_data;
  const char* name = "?";
  if (exc) {
    MonoClass* k = mono_object_get_class(exc);
    if (k) name = mono_class_get_name(k);
  }
  char line[768];
  std::snprintf(line, sizeof(line), "MONO UNHANDLED EXCEPTION: %s", name);
  mono_log(line);
  if (exc) {
    MonoObject* inner_exc = nullptr;
    MonoString* str = mono_object_to_string(exc, &inner_exc);
    if (str) {
      char* msg = mono_string_to_utf8(str);
      if (msg) {
        std::snprintf(line, sizeof(line), "MONO EXCEPTION MSG: %.600s", msg);
        mono_log(line);
        mono_free(msg);
      }
    }
  }
}

LONG WINAPI mono_veh(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT) {
    char line[320];
    std::snprintf(line, sizeof(line), "VEH: exception 0x%08lX at %p",
                  (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
    mono_log(line);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// .126 diagnostic: eglib g_assert aborts via abort() -> SIGABRT, with the
// assert text on stderr only. Log the abort via the allocation-free trace
// writer (survives stdio buffering loss) and exit with code 3 so the
// termination mode is distinguishable from a kernel kill / clean exit.
void sigabrt_handler(int sig) {
  (void)sig;
  trt_trace("TRACE SIGABRT (g_assert/abort) - process aborting");
  std::fflush(nullptr);
  _exit(3);
}

// Catch C++ std::terminate (uncaught C++ exception) and unhandled SEH
// on ANY thread — both write via the allocation-free trace writer.
void tiramisu_terminate_handler() {
  trt_trace("TRACE std::terminate - process aborting");
  std::fflush(nullptr);
  _exit(4);
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
  if (ep && ep->ExceptionRecord) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "TRACE UNHANDLED SEH 0x%08lX at %p",
                  (unsigned long)ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
    trt_trace(line);
    std::fflush(nullptr);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

struct JitInitFault {
  DWORD code = 0;
  void* address = nullptr;
  void* module_base = nullptr;
  DWORD flags = 0;
  ULONG_PTR info0 = 0;
  ULONG_PTR info1 = 0;
  USHORT frames = 0;
  void* stack[24] = {};
};
JitInitFault g_jit_fault;
int jit_init_filter(EXCEPTION_POINTERS* ep) {
  if (ep && ep->ExceptionRecord) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    g_jit_fault.code = r->ExceptionCode;
    g_jit_fault.address = r->ExceptionAddress;
    g_jit_fault.flags = r->ExceptionFlags;
    if (r->NumberParameters > 0) g_jit_fault.info0 = r->ExceptionInformation[0];
    if (r->NumberParameters > 1) g_jit_fault.info1 = r->ExceptionInformation[1];
  }
  g_jit_fault.module_base = reinterpret_cast<void*>(GetModuleHandleW(nullptr));
  g_jit_fault.frames =
      CaptureStackBackTrace(0, 24, g_jit_fault.stack, nullptr);
  return EXCEPTION_EXECUTE_HANDLER;
}

void assembly_load_hook_uwp(MonoAssembly* assembly, void* user_data) {
  (void)user_data;
  MonoImage* image = mono_assembly_get_image(assembly);
  if (!image) return;
  {
    const char* nm = mono_image_get_name(image);
    char line[320];
    std::snprintf(line, sizeof(line), "mono image: %s", nm ? nm : "?");
    mono_log(line);
  }
  // FNA3D is exported from the exe itself (FNA3DAPI __declspec(dllexport)
  // under _WIN32): map it to __Internal so Mono resolves the P/Invokes
  // via GetProcAddress on the main module. Installed into EVERY image,
  // before any P/Invoke in it can resolve. FAudio is mapped the same way:
  // the core + Tiramisu UWP platform layer are compiled into the host.
  mono_dllmap_insert(image, "FNA3D", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "FAudio", nullptr, "__Internal", nullptr);
#ifdef _WIN32
  // Managed Celeste P/Invokes: the real bridges are compiled into this
  // UWP host and exported (host-bridges.def), so map them to __Internal.
  // The Linux harness uses cfg->dllmaps to the real .so files instead.
  // Also install GLOBAL dllmaps (NULL image). The per-image maps above are
  // only consulted for the image the hook saw; FNA's P/Invokes resolve from
  // a forwarded assembly context where the per-image map is not applied.
  mono_dllmap_insert(nullptr, "FNA3D", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "FAudio", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "fmod", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "fmodstudio", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "CSteamworks", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "sdkencryptedappticket", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(nullptr, "System.Native", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "fmod", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "fmodstudio", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "CSteamworks", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "sdkencryptedappticket", nullptr, "__Internal", nullptr);
  mono_dllmap_insert(image, "System.Native", nullptr, "__Internal", nullptr);
#endif
  if (!g_hook_dllmaps_uwp || g_hook_dllmap_count_uwp <= 0) return;
  for (int i = 0; i < g_hook_dllmap_count_uwp; ++i) {
    mono_dllmap_insert(image, g_hook_dllmaps_uwp[i].module, nullptr,
                       g_hook_dllmaps_uwp[i].target, nullptr);
  }
}

// UTF-16 -> UTF-8 (for the path from GetModuleFileNameW). The buffer is
// always NUL-terminated; returns false on overflow or conversion error.
bool utf16_to_utf8(const wchar_t* wide, char* out, size_t out_cap) {
  if (!wide || !out || out_cap == 0) return false;
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_cap,
                                    nullptr, nullptr);
  if (n <= 0) {
    out[0] = 0;
    return false;
  }
  return true;
}

// Install dir of the running exe (the UWP package's install location):
// the directory containing tiramisu.exe. The Mono framework assemblies
// live in <install>/mono/4.5/ and the machine config in
// <install>/etc/mono/config (packaged by tools/build-host.sh).
bool install_dir(char* out, size_t out_cap) {
  wchar_t path_w[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path_w, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  // Strip the file name (accept both separators; the path uses
  // backslashes, but be liberal).
  wchar_t* slash = nullptr;
  for (wchar_t* p = path_w; *p; ++p) {
    if (*p == L'\\' || *p == L'/') slash = p;
  }
  if (!slash) return false;
  *slash = 0;
  return utf16_to_utf8(path_w, out, out_cap);
}

// Mono's assembly search path for managed XNA titles. The offline gate sets
// cfg.assemblies_path to third_party/fna/out; on console the host leaves it
// unset, so default to the FNA substitutes shipped in <install>/fna.
static char g_default_assemblies_path[1120];
bool default_assemblies_path() {
  char install[1024];
  if (!install_dir(install, sizeof(install))) return false;
  const int n = std::snprintf(g_default_assemblies_path,
                              sizeof(g_default_assemblies_path), "%s\\fna",
                              install);
  return n > 0 && static_cast<size_t>(n) < sizeof(g_default_assemblies_path);
}

}  // namespace

extern "C" int trt_mono_run(const trt_mono_config* cfg,
                            const char* const* args, int nargs,
                            int* exit_code_out) {
  g_last_error[0] = 0;
  if (exit_code_out) *exit_code_out = 0;
  if (!cfg || !cfg->exe_path || !cfg->exe_path[0])
    return fail("trt_mono_run: no exe_path");
  if (cfg->log) g_log = cfg->log;

  // UWP: Mono/game stderr goes nowhere. Redirect it to a file in the
  // game dir so g_error()/Console.Error messages survive a silent exit.
  static bool stderr_redirected = false;
  if (!stderr_redirected && cfg->exe_path) {
    stderr_redirected = true;
    const char* slash = nullptr;
    for (const char* q = cfg->exe_path; *q; ++q)
      if (*q == '/' || *q == '\\') slash = q;
    char logpath[1200];
    if (slash) {
      const size_t n = (size_t)(slash - cfg->exe_path);
      std::memcpy(logpath, cfg->exe_path, n);
      std::memcpy(logpath + n, "\\mono-stderr.log", 17);
    } else {
      std::snprintf(logpath, sizeof(logpath), "mono-stderr.log");
    }
    wchar_t wide[1300];
    // .126: verify the redirect actually took (mono-stderr.log was 0 B
    // in .122-.125 - either nothing was written or the redirect failed
    // silently), capture stdout too (eglib may write there), and force
    // both streams unbuffered - abort() does NOT flush stdio buffers,
    // so a buffered assert message would be lost on the g_assert path.
    bool err_ok = false;
    bool out_ok = false;
    if (MultiByteToWideChar(CP_UTF8, 0, logpath, -1, wide, 1300) > 0) {
      FILE* f_err = _wfreopen(wide, L"w", stderr);
      FILE* f_out = _wfreopen(wide, L"a", stdout);
      if (f_err) std::setvbuf(stderr, nullptr, _IONBF, 0);
      if (f_out) std::setvbuf(stdout, nullptr, _IONBF, 0);
      err_ok = (f_err != nullptr);
      out_ok = (f_out != nullptr);
    }
    {
      char line[256];
      std::snprintf(line, sizeof(line),
                    "126 diag: stderr->mono-stderr.log %s, stdout %s",
                    err_ok ? "ok" : "FAILED", out_ok ? "ok" : "FAILED");
      mono_log(line);
    }
  }

  // Install Mono diagnostic handlers FIRST: on the console there is no
  // console, so Mono's stderr/log vanishes — route it into tiramisu.log.
  mono_trace_set_print_handler(mono_print_to_log);
  mono_trace_set_printerr_handler(mono_print_to_log);
  mono_trace_set_log_handler(mono_log_to_log, nullptr);

  // FNA platform selection via environment variables, as on Linux —
  // but through SetEnvironmentVariableW (the UCRT setenv/unsetenv are
  // not available under UWP; the Win32 API is the direct equivalent
  // and works in the AppContainer). Passing NULL deletes the variable.
  if (cfg->fna_platform && cfg->fna_platform[0]) {
    if (std::strcmp(cfg->fna_platform, "tiramisu") == 0) {
      SetEnvironmentVariableW(L"TIRAMISU_FNA_PLATFORM", L"1");
      SetEnvironmentVariableW(L"FNA_PLATFORM_BACKEND", L"TIRAMISU");
      mono_log("fna platform: tiramisu (host surface + host input)");
    } else if (std::strcmp(cfg->fna_platform, "sdl2") == 0) {
      SetEnvironmentVariableW(L"TIRAMISU_FNA_PLATFORM", nullptr);
      SetEnvironmentVariableW(L"FNA_PLATFORM_BACKEND", L"SDL2");
      mono_log("fna platform: sdl2");
    } else {
      char detail[512];
      std::snprintf(detail, sizeof(detail),
                    "unknown fna_platform \"%s\" (want \"tiramisu\" or "
                    "\"sdl2\")",
                    cfg->fna_platform);
      return fail(detail);
    }
  }

  // Framework + config dirs: derived from the install dir unless the
  // caller overrode them. The layout (packaged by build-host.sh) is
  // <install>/mono/4.5/*.dll + <install>/mono/gac/... and
  // <install>/etc/mono/config, mirroring Linux's /usr/lib + /etc split.
  char install[1024];
  if (!install_dir(install, sizeof(install)))
    return fail("trt_mono_run: cannot determine install dir");
  char framework_dir[1024];
  char config_dir[1120];
  std::snprintf(framework_dir, sizeof(framework_dir), "%s",
                cfg->framework_dir && cfg->framework_dir[0]
                    ? cfg->framework_dir
                    : install);
  if (cfg->config_dir && cfg->config_dir[0]) {
    std::snprintf(config_dir, sizeof(config_dir), "%s", cfg->config_dir);
  } else {
    std::snprintf(config_dir, sizeof(config_dir), "%s\\etc", install);
  }

  // Dirs FIRST, then the machine config: mono_config_parse(nullptr)
  // reads <config_dir>/mono/config, and the compiled-in default config
  // dir is meaningless for a UWP package. (Linux does config first
  // because its compiled-in default, /etc, is already right.)
  mono_set_dirs(framework_dir, config_dir);
  mono_config_parse(nullptr);
  if (cfg->assemblies_path && cfg->assemblies_path[0])
    mono_set_assemblies_path(cfg->assemblies_path);
  else if (default_assemblies_path())
    mono_set_assemblies_path(g_default_assemblies_path);
  {
    char line[2300];
    std::snprintf(line, sizeof(line), "mono dirs: framework=%s config=%s",
                  framework_dir, config_dir);
    mono_log(line);
  }

  // Diagnostic: route Mono GC/thread events into tiramisu.log so a
  // stop-the-world deadlock is visible. .126 adds dll,asm (surfaces
  // DllImport resolution traces, suppressed by default) and a 64 MB
  // nursery: if the death is the first-finalization JIT compile
  // (gc.c:296-306), deferring the first GC past startup lets the game
  // proceed past Steamworks.NET - the decisive discriminator.
  SetEnvironmentVariableW(L"MONO_LOG_MASK", L"gc,thread,dll,asm");
  SetEnvironmentVariableW(L"MONO_LOG_LEVEL", L"debug");
  // The Xbox trace parked Mono inside drain_gray_stack_concurrent and
  // never returned from the native WaitOnAddress barrier. Select SGen's
  // supported serial major collector on UWP; this avoids the concurrent
  // marker worker barrier while preserving the normal SGen heap/nursery.
  SetEnvironmentVariableW(L"MONO_GC_PARAMS", L"nursery-size=64m,major=marksweep");

  // Interpreter mode is FORCED on UWP: the AppContainer blocks dynamic
  // code generation, so JIT code cannot execute; the interpreter is the
  // only execution engine. (cfg->interpreter is ignored here — there is
  // no JIT path on UWP to select.)
  {
    char* opt_argv[2] = {(char*)"tiramisu-mono", (char*)"--interpreter"};
    mono_jit_parse_options(2, opt_argv);
    mono_log("mode: interpreter (forced on UWP; no JIT in AppContainer)");
  // Diagnostic: can Mono's __Internal path see the exported bridge
  // functions in this exe? These are the flat P/Invoke entry points.
  {
    HMODULE self = GetModuleHandleW(nullptr);
    const char* probes[] = {"Init", "RestartAppIfNecessary", "FMOD_System_Create",
                            "FNA3D_CreateDevice", "TiramisuFNA_Ping"};
    char line[256];
    std::snprintf(line, sizeof(line), "self module: %p", (void*)self);
    mono_log(line);
    for (const char* sym : probes) {
      void* fn = (void*)GetProcAddress(self, sym);
      std::snprintf(line, sizeof(line), "export %s: %s", sym,
                    fn ? "FOUND" : "MISSING");
      mono_log(line);
    }
  }

  }

  // NOTE (.124): mono_aot_only is NOT set before mono_jit_init - .123
  // proved it kills mini_init itself (died before mscorlib loaded).
  // It is set AFTER init succeeds (below), so init runs normally
  // (patches 28+29 handle it) and Main runs with no trampolines.

  // Install the dllmap hook BEFORE opening any assembly so every image —
  // the exe, its managed DLLs, lazily-loaded assemblies — gets the maps.
  g_hook_dllmaps_uwp = cfg->dllmaps;
  g_hook_dllmap_count_uwp = cfg->dllmap_count;
  mono_install_assembly_load_hook(assembly_load_hook_uwp, nullptr);

  {
    char line[700];
    std::snprintf(line, sizeof(line), "opening %s", cfg->exe_path);
    mono_log(line);
  }
  // .126 diagnostic: install the VEH (defined since .105 but never
  // installed - a non-main-thread AV was totally silent) and a SIGABRT
  // handler (g_assert aborts via abort()). Both must be in place before
  // mono_jit_init so the whole run is covered.
  {
    // AddVectoredExceptionHandler is not declared in the UWP-partitioned
    // headers, so resolve it at runtime from kernel32.dll; if it is
    // unavailable the VEH is skipped (logged) instead of failing the link.
    using AddVehFn =
        PVOID (WINAPI*)(ULONG, LONG (WINAPI*)(EXCEPTION_POINTERS*));
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pAddVeh = k32 ? reinterpret_cast<AddVehFn>(
                              GetProcAddress(k32, "AddVectoredExceptionHandler"))
                       : nullptr;
    PVOID veh = pAddVeh ? pAddVeh(1, mono_veh) : nullptr;
    std::signal(SIGABRT, sigabrt_handler);
    std::set_terminate(tiramisu_terminate_handler);
    HMODULE k32b = GetModuleHandleW(L"kernel32.dll");
    using SetUefFn = LONG (WINAPI*)(LONG (WINAPI*)(EXCEPTION_POINTERS*));
    auto pSetUef = k32b ? reinterpret_cast<SetUefFn>(
        GetProcAddress(k32b, "SetUnhandledExceptionFilter")) : nullptr;
    if (pSetUef) pSetUef(unhandled_filter);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "126 diag: VEH %s, SIGABRT handler installed, "
                  "nursery=64m",
                  veh ? "installed" : "FAILED");
    mono_log(line);
  }
  mono_log("mono_jit_init: calling (SEH-guarded)");
  MonoDomain* domain = nullptr;
  __try {
    domain = mono_jit_init(
        cfg->domain_name ? cfg->domain_name : "tiramisu-mono");
  } __except (jit_init_filter(GetExceptionInformation())) {
    domain = nullptr;
  }
  if (g_jit_fault.code != 0) {
    const auto base = reinterpret_cast<ULONG_PTR>(g_jit_fault.module_base);
    const auto addr = reinterpret_cast<ULONG_PTR>(g_jit_fault.address);
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "mono_jit_init SEH 0x%08lX at %p (modbase=%p rva=0x%llX) "
                  "flags=0x%lX info0=0x%llX info1=0x%llX frames=%u",
                  (unsigned long)g_jit_fault.code, g_jit_fault.address,
                  g_jit_fault.module_base,
                  (unsigned long long)(addr >= base ? addr - base : 0),
                  (unsigned long)g_jit_fault.flags,
                  (unsigned long long)g_jit_fault.info0,
                  (unsigned long long)g_jit_fault.info1,
                  (unsigned)g_jit_fault.frames);
    mono_log(detail);
    for (USHORT i = 0; i < g_jit_fault.frames; ++i) {
      const auto fa = reinterpret_cast<ULONG_PTR>(g_jit_fault.stack[i]);
      char frame[160];
      std::snprintf(frame, sizeof(frame), "  jit_frame[%u] %p rva=0x%llX",
                    (unsigned)i, g_jit_fault.stack[i],
                    (unsigned long long)(fa >= base ? fa - base : 0));
      mono_log(frame);
    }
    return fail("mono_jit_init raised SEH");
  }
  if (!domain) return fail("mono_jit_init returned NULL");
  mono_log("mono_jit_init: domain ok");

  // NOTE (.125): mono_aot_only is NOT set here. .123 (pre-init) died in
  // mini_init; .124 (post-init) died in mono_assembly_open. aot_only
  // without AOT images is not viable - Mono needs AOT images for every
  // assembly it loads in aot-only mode. The .122 baseline (no aot_only)
  // is restored; the analyst's fix lands on top of it.

  // Register the Tiramisu backend's native internal calls now, before
  // any managed code runs (same as Linux).
  trt_fna_register_internal_calls(
      (trt_fna_add_internal_call_fn)mono_add_internal_call);
  mono_log("fna platform: native internal calls registered");

  // Set the AppDomain base directory to the game's directory and apply
  // the game's .config file if it has one (Celeste.exe.config pins the
  // v4.0/.NET 4.5 runtime). Must happen before the main assembly is
  // opened. Always pass the .config path, like the mono CLI: this Mono
  // build's mono_domain_set_config strlen()s it unconditionally, so
  // NULL segfaults. Mono ignores a missing file. Paths on Windows use
  // backslashes; accept both separators.
  {
    const char* slash = nullptr;
    for (const char* p = cfg->exe_path; *p; ++p) {
      if (*p == '/' || *p == '\\') slash = p;
    }
    char game_dir[1024];
    if (!slash) {
      std::snprintf(game_dir, sizeof(game_dir), ".");
    } else if (slash == cfg->exe_path) {
      std::snprintf(game_dir, sizeof(game_dir), "\\");
    } else {
      const size_t len = (size_t)(slash - cfg->exe_path);
      if (len >= sizeof(game_dir))
        return fail("exe_path directory too long");
      std::memcpy(game_dir, cfg->exe_path, len);
      game_dir[len] = 0;
    }
    char config_path[1120];
    std::snprintf(config_path, sizeof(config_path), "%s.config", cfg->exe_path);
    const bool has_config =
        (GetFileAttributesA(config_path) != INVALID_FILE_ATTRIBUTES);
    // UWP: the process CWD is the read-only package install dir, so a
    // game that writes a relative path (Celeste: error_log.txt) gets
    // UnauthorizedAccessException. Point CWD at the writable game dir.
    wchar_t game_dir_w[1200];
    if (MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, game_dir_w, 1200) > 0)
      SetCurrentDirectoryW(game_dir_w);
    mono_domain_set_config(domain, game_dir, config_path);
    char line[1200];
    std::snprintf(line, sizeof(line), "domain base dir: %s%s", game_dir,
                  has_config ? " (exe .config applied)" : "");
    mono_log(line);
  }

  trt_trace("TRACE assembly open enter");
  MonoAssembly* assembly = mono_domain_assembly_open(domain, cfg->exe_path);
  if (!assembly) {
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "mono_domain_assembly_open failed for %s", cfg->exe_path);
    return fail(detail);
  }
  trt_trace("TRACE assembly open ok");

  // Enter Main via mono_jit_exec — the same entry the mono CLI uses. It
  // sets the entry assembly (so Assembly.GetEntryAssembly() works —
  // Celeste's Engine static constructor dereferences it) and invokes
  // Main with argv[1..] as its string[] args; argv[0] is the exe path.
  // It returns the game's exit code, or -1 when Main is missing or an
  // unhandled managed exception escapes. A game whose Main legitimately
  // returns -1 would be misreported as a failure; Celeste's Main is
  // void, so -1 is unambiguous here.
  mono_install_unhandled_exception_hook(mono_unhandled_hook, nullptr);
  mono_log("unhandled exception hook installed");
  mono_log("invoking managed Main");
  std::atomic<bool> wd_stop{false};
  std::thread wd([&wd_stop]() {
    // Tracing watchdog: uses the allocation-free trace writer (NOT
    // mono_log, which blocks on the heap lock / g_file_mu during the
    // freeze) and probes heap health each cycle: a hung malloc = the
    // CRT heap lock is held (H1). Per cycle: PROBE enter -> malloc ->
    // PROBE ok. If "enter" appears with no "ok", the heap is locked.
    for (int i = 0; i < 40 && !wd_stop.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      if (!wd_stop.load()) {
        char t[96];
        std::snprintf(t, sizeof(t), "TRACE watchdog: Main still running (%ds)",
                      (i + 1) * 3);
        trt_trace(t);
        trt_trace("TRACE PROBE malloc enter");
        void* p = std::malloc(64);
        if (p) {
          trt_trace("TRACE PROBE malloc ok");
          std::free(p);
        } else {
          trt_trace("TRACE PROBE malloc NULL (out of memory)");
        }
        // Memory instrumentation (oom-analysis.md section 6.1): the
        // DECISIVE measurement for the .141 ceiling — if app usage
        // climbs toward the 5 GB limit before the death, it is OOM; if
        // it stays flat (~500 MB), it is a silent crash. Allocation-free
        // (stack buffer + trt_trace), like the rest of this watchdog.
        {
          char m[192];
          try {
            using winrt::Windows::System::MemoryManager;
            const uint64_t usage = MemoryManager::AppMemoryUsage();
            const uint64_t limit = MemoryManager::AppMemoryUsageLimit();
            const uint64_t pcm = trt_fmod_studio_pcm_cache_bytes();
            const int64_t heap = mono_gc_get_heap_size();
            const int64_t used = mono_gc_get_used_size();
            std::snprintf(m, sizeof(m),
                          "TRACE mem: app %llu/%llu MiB, pcm %llu MiB, "
                          "sgen heap %lld used %lld MiB",
                          (unsigned long long)(usage / (1024 * 1024)),
                          (unsigned long long)(limit / (1024 * 1024)),
                          (unsigned long long)(pcm / (1024 * 1024)),
                          (long long)(heap / (1024 * 1024)),
                          (long long)(used / (1024 * 1024)));
          } catch (const winrt::hresult_error&) {
            std::snprintf(m, sizeof(m),
                          "TRACE mem: app usage query failed");
          }
          trt_trace(m);
        }
      }
    }
  });
  std::vector<char*> exec_argv;
  exec_argv.reserve((size_t)(nargs > 0 ? nargs : 0) + 1);
  exec_argv.push_back(const_cast<char*>(cfg->exe_path));
  for (int i = 0; i < nargs; ++i)
    exec_argv.push_back(const_cast<char*>(args[i] ? args[i] : ""));
  int exec_rc = -999;
  trt_trace("TRACE mono_jit_exec enter");
  __try {
    exec_rc = mono_jit_exec(
        domain, assembly, (int)exec_argv.size(), exec_argv.data());
  } __except (jit_init_filter(GetExceptionInformation())) {
    char ex[256];
    std::snprintf(ex, sizeof(ex),
                  "managed Main raised SEH 0x%08lX at %p",
                  (unsigned long)g_jit_fault.code, g_jit_fault.address);
    mono_log(ex);
    trt_trace(ex);
    exec_rc = -999;
  }
  trt_trace("TRACE mono_jit_exec returned");
  wd_stop.store(true);
  if (wd.joinable()) wd.join();
  if (exec_rc == -1)
    return fail("mono_jit_exec failed: no Main or unhandled managed "
                "exception (see stderr)");
  if (exit_code_out) *exit_code_out = exec_rc;
  {
    char line[96];
    std::snprintf(line, sizeof(line), "managed Main returned (exit code %d)",
                  exec_rc);
    mono_log(line);
  }
  mono_jit_cleanup(domain);
  return TRT_OK;
}

#else
// ---- Linux: real embedding via dlopen'd libmono-2.0 ----
#include <cstdlib>  // setenv/unsetenv (FNA platform selection)
#include <dlfcn.h>
#include <unistd.h>

// The Mono headers provide exact declarations; the function-pointer table
// below is built from them with decltype, so a signature drift fails at
// compile time instead of corrupting a call at runtime.
#include <mono/jit/jit.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/blob.h>
#include <mono/metadata/image.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/object.h>

// domain.h is not shipped in the Ubuntu libmono-2.0-dev header set, so
// mono_domain_set_config is declared directly (signature per Mono's
// embedding docs and upstream mono/metadata/domain.h; the symbol is
// exported by libmonosgen-2.0 and resolved via dlsym like the rest).
extern "C" void mono_domain_set_config(MonoDomain* domain, const char* base_dir,
                                       const char* config_file_name);

namespace {

#define MONO_FN(name) decltype(&name) p_##name = nullptr
struct MonoApi {
  MONO_FN(mono_config_parse);
  MONO_FN(mono_set_dirs);
  MONO_FN(mono_set_assemblies_path);
  MONO_FN(mono_jit_parse_options);
  MONO_FN(mono_jit_init);
  MONO_FN(mono_jit_cleanup);
  MONO_FN(mono_domain_set_config);
  MONO_FN(mono_install_assembly_load_hook);
  MONO_FN(mono_domain_assembly_open);
  MONO_FN(mono_assembly_get_image);
  MONO_FN(mono_jit_exec);
  MONO_FN(mono_dllmap_insert);
  MONO_FN(mono_add_internal_call);
};
#undef MONO_FN

MonoApi g_mono;
void* g_libmono = nullptr;

// dllmaps for the assembly-load hook (set for the duration of one run;
// Mono has no hook uninstall, and one run per process is the contract).
const trt_mono_dllmap* g_hook_dllmaps = nullptr;
int g_hook_dllmap_count = 0;

void assembly_load_hook(MonoAssembly* assembly, void* user_data) {
  (void)user_data;
  if (!g_hook_dllmaps || g_hook_dllmap_count <= 0) return;
  MonoImage* image = g_mono.p_mono_assembly_get_image(assembly);
  if (!image) return;
  // Insert BEFORE any P/Invoke in this image can resolve: the hook fires
  // at load time, and P/Invoke targets bind lazily on first call.
  for (int i = 0; i < g_hook_dllmap_count; ++i) {
    g_mono.p_mono_dllmap_insert(image, g_hook_dllmaps[i].module, nullptr,
                                g_hook_dllmaps[i].target, nullptr);
  }
}

bool load_libmono(const char* explicit_path, char* detail, size_t detail_cap) {
  const char* candidates[4];
  int n = 0;
  if (explicit_path && explicit_path[0]) candidates[n++] = explicit_path;
  candidates[n++] = "libmono-2.0.so.1";
  candidates[n++] = "libmonosgen-2.0.so.1";
  candidates[n++] = "libmonoboehm-2.0.so.1";
  const char* last_err = "";
  for (int i = 0; i < n; ++i) {
    g_libmono = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
    if (g_libmono) {
      char line[512];
      std::snprintf(line, sizeof(line), "libmono loaded: %s", candidates[i]);
      mono_log(line);
      return true;
    }
    const char* e = dlerror();
    if (e) last_err = e;
  }
  std::snprintf(detail, detail_cap, "dlopen libmono failed: %s", last_err);
  return false;
}

#define RESOLVE(api, name)                                                    \
  do {                                                                        \
    api.p_##name = reinterpret_cast<decltype(&name)>(dlsym(g_libmono, #name)); \
    if (!api.p_##name) {                                                      \
      std::snprintf(detail, detail_cap, "libmono lacks symbol %s", #name);    \
      return false;                                                           \
    }                                                                         \
  } while (0)

bool resolve_api(char* detail, size_t detail_cap) {
  MonoApi& api = g_mono;
  RESOLVE(api, mono_config_parse);
  RESOLVE(api, mono_set_dirs);
  RESOLVE(api, mono_set_assemblies_path);
  RESOLVE(api, mono_jit_parse_options);
  RESOLVE(api, mono_jit_init);
  RESOLVE(api, mono_jit_cleanup);
  RESOLVE(api, mono_domain_set_config);
  RESOLVE(api, mono_install_assembly_load_hook);
  RESOLVE(api, mono_domain_assembly_open);
  RESOLVE(api, mono_assembly_get_image);
  RESOLVE(api, mono_jit_exec);
  RESOLVE(api, mono_dllmap_insert);
  RESOLVE(api, mono_add_internal_call);
  return true;
}
#undef RESOLVE

}  // namespace

extern "C" int trt_mono_run(const trt_mono_config* cfg,
                            const char* const* args, int nargs,
                            int* exit_code_out) {
  g_last_error[0] = 0;
  if (exit_code_out) *exit_code_out = 0;
  if (!cfg || !cfg->exe_path || !cfg->exe_path[0])
    return fail("trt_mono_run: no exe_path");
  if (cfg->log) g_log = cfg->log;

  // FNA platform selection: applied as env vars before Main runs (the
  // FNA dispatch reads them in its static constructor). An unknown
  // value fails the run: silently running the wrong backend would
  // present a headless game as working.
  if (cfg->fna_platform && cfg->fna_platform[0]) {
    if (std::strcmp(cfg->fna_platform, "tiramisu") == 0) {
      ::setenv("TIRAMISU_FNA_PLATFORM", "1", 1);
      ::setenv("FNA_PLATFORM_BACKEND", "TIRAMISU", 1);
      mono_log("fna platform: tiramisu (host surface + host input)");
    } else if (std::strcmp(cfg->fna_platform, "sdl2") == 0) {
      ::unsetenv("TIRAMISU_FNA_PLATFORM");
      ::setenv("FNA_PLATFORM_BACKEND", "SDL2", 1);
      mono_log("fna platform: sdl2");
    } else {
      char detail[512];
      std::snprintf(detail, sizeof(detail),
                    "unknown fna_platform \"%s\" (want \"tiramisu\" or "
                    "\"sdl2\")",
                    cfg->fna_platform);
      return fail(detail);
    }
  }

  char detail[512];
  if (!load_libmono(cfg->libmono_path, detail, sizeof(detail)))
    return fail(detail);
  if (!resolve_api(detail, sizeof(detail))) return fail(detail);
  MonoApi& api = g_mono;

  // Machine config first (system dllmaps for the framework assemblies),
  // then dirs, then the assembly search path.
  api.p_mono_config_parse(nullptr);
  api.p_mono_set_dirs(cfg->framework_dir ? cfg->framework_dir : "/usr/lib",
                      cfg->config_dir ? cfg->config_dir : "/etc");
  // Offline callers (build-test's Celeste gate) pass third_party/fna/out
  // explicitly; the console host goes through the _WIN32 path above.
  if (cfg->assemblies_path && cfg->assemblies_path[0])
    api.p_mono_set_assemblies_path(cfg->assemblies_path);

  // Interpreter mode (no JIT) must be selected before mono_jit_init.
  // JIT is the default: it is what the Path B spike proved.
  if (cfg->interpreter) {
    char* opt_argv[2] = {(char*)"tiramisu-mono", (char*)"--interpreter"};
    api.p_mono_jit_parse_options(2, opt_argv);
    mono_log("mode: interpreter (no JIT)");
  } else {
    mono_log("mode: JIT");
  }

  // Install the dllmap hook BEFORE opening any assembly so every image —
  // the exe, its managed DLLs, lazily-loaded assemblies — gets the maps.
  g_hook_dllmaps = cfg->dllmaps;
  g_hook_dllmap_count = cfg->dllmap_count;
  api.p_mono_install_assembly_load_hook(assembly_load_hook, nullptr);

  {
    char line[700];
    std::snprintf(line, sizeof(line), "opening %s", cfg->exe_path);
    mono_log(line);
  }
  MonoDomain* domain = api.p_mono_jit_init(
      cfg->domain_name ? cfg->domain_name : "tiramisu-mono");
  if (!domain) return fail("mono_jit_init returned NULL");

  // Register the Tiramisu backend's native internal calls now, before
  // any managed code runs: the FNA static constructor (inside Main)
  // may select the Tiramisu backend, and its ProgramInit pings the
  // bridge. Registered unconditionally — the bindings are inert unless
  // managed code calls them, so the SDL2 path is unaffected.
  trt_fna_register_internal_calls(
      (trt_fna_add_internal_call_fn)api.p_mono_add_internal_call);
  mono_log("fna platform: native internal calls registered");

  // Set the AppDomain base directory to the game's directory and apply
  // the game's .config file if it has one (Celeste.exe.config pins the
  // v4.0/.NET 4.5 runtime). Must happen before the main assembly is
  // opened. Without this, BaseDirectory is the process cwd and content
  // loads look in the wrong place (TitleContainer is BaseDirectory-
  // relative); the mono CLI does exactly this in its startup path.
  {
    const char* slash = std::strrchr(cfg->exe_path, '/');
    char game_dir[1024];
    if (!slash) {
      std::snprintf(game_dir, sizeof(game_dir), ".");
    } else if (slash == cfg->exe_path) {
      std::snprintf(game_dir, sizeof(game_dir), "/");
    } else {
      const size_t len = (size_t)(slash - cfg->exe_path);
      if (len >= sizeof(game_dir))
        return fail("exe_path directory too long");
      std::memcpy(game_dir, cfg->exe_path, len);
      game_dir[len] = 0;
    }
    char config_path[1120];
    std::snprintf(config_path, sizeof(config_path), "%s.config", cfg->exe_path);
    // Always pass the .config path, like the mono CLI: this Mono
    // build's mono_domain_set_config strlen()s it unconditionally, so
    // NULL segfaults (found by the FNA probe, which ships no .config;
    // Celeste.exe.config exists, so the Celeste path never hit it).
    // Mono ignores a missing file.
    const bool has_config = (::access(config_path, F_OK) == 0);
    api.p_mono_domain_set_config(domain, game_dir, config_path);
    // Publish the game dir for the dllmap bridges: the Steam door reads
    // steam_appid.txt from $TIRAMISU_GAME_DIR at Init (the way the real
    // Steam client finds the AppID).
    ::setenv("TIRAMISU_GAME_DIR", game_dir, 1);
    char line[1200];
    std::snprintf(line, sizeof(line), "domain base dir: %s%s", game_dir,
                  has_config ? " (exe .config applied)" : "");
    mono_log(line);
  }

  MonoAssembly* assembly =
      api.p_mono_domain_assembly_open(domain, cfg->exe_path);
  if (!assembly) {
    std::snprintf(detail, sizeof(detail),
                  "mono_domain_assembly_open failed for %s", cfg->exe_path);
    return fail(detail);
  }
  trt_trace("TRACE assembly open ok");

  // Enter Main via mono_jit_exec — the same entry the mono CLI uses. It
  // sets the entry assembly (so Assembly.GetEntryAssembly() works —
  // Celeste's Engine static constructor dereferences it) and invokes
  // Main with argv[1..] as its string[] args; argv[0] is the exe path.
  // It returns the game's exit code, or -1 when Main is missing or an
  // unhandled managed exception escapes (the exception + managed stack
  // trace are printed to stderr by the runtime itself). A game whose
  // Main legitimately returns -1 would be misreported as a failure;
  // Celeste's Main is void, so -1 is unambiguous here.
  mono_log("invoking managed Main");
  std::vector<char*> exec_argv;
  exec_argv.reserve((size_t)(nargs > 0 ? nargs : 0) + 1);
  exec_argv.push_back(const_cast<char*>(cfg->exe_path));
  for (int i = 0; i < nargs; ++i)
    exec_argv.push_back(const_cast<char*>(args[i] ? args[i] : ""));
  const int exec_rc = api.p_mono_jit_exec(
      domain, assembly, (int)exec_argv.size(), exec_argv.data());
  if (exec_rc == -1)
    return fail("mono_jit_exec failed: no Main or unhandled managed "
                "exception (see stderr)");
  if (exit_code_out) *exit_code_out = exec_rc;
  {
    char line[96];
    std::snprintf(line, sizeof(line), "managed Main returned (exit code %d)",
                  exec_rc);
    mono_log(line);
  }
  api.p_mono_jit_cleanup(domain);
  return TRT_OK;
}

#endif  // _WIN32
