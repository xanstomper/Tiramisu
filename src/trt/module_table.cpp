// tiramisu/src/trt/module_table.cpp
// Builtin module registry + routing policy.
//
// Routing classes (from xboxnative's console evidence, re-derived):
//   SYSTEM  — console ships the real DLL (d3d11, dxgi, ws2_32, bcrypt,
//             kernelbase, ole32...): host binds via LoadLibrary+GetProcAddress.
//   BUILTIN — Tiramisu implements it to Wine's observable semantics
//             (kernel32 core, user32 window/message layer on CoreWindow).
//   STUB    — exported, logs when called, returns 0: fail-visible, never
//             fail-silent. Every STUB hit in a log is roadmap work.
#include "trt.h"
#include "apiset.h"
#include "thunk.h"
#include "images.h"
#include "pe_exports.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool name_eq(const char* a, const char* b) {
  size_t i = 0;
  for (; a[i] && b[i]; ++i)
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  return a[i] == b[i];  // both ended (b may be the shorter registered name)
}

struct builtin_mod {
  char name[32];
  trt_route route;
  // the function table is OWNED by the module: several bridges add to the
  // same module (kernel32 gets memory, thread, system and core functions),
  // so registering must MERGE, not replace. Capacity 256: kernel32's merged
  // surface passed 156 (core 98 + mem 7 + sys 25 + thread 26) and the old
  // cap of 128 SILENTLY dropped the tail — games importing the dropped
  // tail (GetSystemTimeAsFileTime, EncodePointer, GetCommandLineW...) died
  // at bind with no named cause. OrTMM audit found it.
  trt_named_fn fns[2048];
  int fn_count;
};
// The table must hold every module a game might import: the defaults
// (70+) plus the builtin bridges. Overflow is a bug, not a shrug - it is
// logged, because a dropped registration means an unresolved import.
builtin_mod g_modules[384];
int g_module_count = 0;

// host-installed binding for SYSTEM-route modules (UWP: LoadLibraryW +
// GetProcAddress of the console's real DLLs). NULL => SYSTEM routes fail.
trt_system_load g_system_load = nullptr;

// alias routing: desktop DLL name -> the name that exists on the console.
// This is the #1 hidden boot-killer: a game importing vcruntime140.dll fails
// at LOAD time unless we route it to the console's vcruntime140_app.dll.
struct dll_alias { const char* guest_name; const char* system_name; };
const dll_alias k_dll_aliases[] = {
    {"vcruntime140.dll", "vcruntime140_app.dll"},
    {"vcruntime140_1.dll", "vcruntime140_1_app.dll"},
    {"msvcp140.dll", "msvcp140_app.dll"},
    {"msvcp140_1.dll", "msvcp140_1_app.dll"},
    {"msvcp140_2.dll", "msvcp140_2_app.dll"},
    {"msvcp140_atomic_wait.dll", "msvcp140_atomic_wait_app.dll"},
    {"msvcp140_codecvt_ids.dll", "msvcp140_codecvt_ids_app.dll"},
    {"concrt140.dll", "concrt140_app.dll"},
    {"vcomp140.dll", "vcomp140_app.dll"},
    // XInput on console = XINPUTUAP.dll (the SDK's xinput.lib imports it)
    {"xinput1_4.dll", "xinputuap.dll"},
    {"xinput1_3.dll", "xinputuap.dll"},
    {"xinput9_1_0.dll", "xinputuap.dll"},
    {"xinput1_2.dll", "xinputuap.dll"},
    {"xinput1_1.dll", "xinputuap.dll"},
    {"xapauthenticode.dll", "xapauthenticode.dll"},
    // shader compiler: real UWP component, same name
    {"d3dcompiler_47.dll", "d3dcompiler_47.dll"},
    {"d3dcompiler_46.dll", "d3dcompiler_47.dll"},
    {"d3dcompiler_43.dll", "d3dcompiler_47.dll"},
    // d3d9: bridged by bridge_d3d9 (BUILTIN overrides this default)
    {"d3d9.dll", nullptr},
    // steam api: named failure until M6 (no DRM bypass, ever)
    {"steam_api64.dll", nullptr},
    {"steam_api.dll", nullptr},
};

const char* dll_alias_lookup(const char* name);  // defined below (case-insens)

int find_module(const char* name) {
  for (int i = 0; i < g_module_count; ++i)
    if (name_eq(name, g_modules[i].name)) return i;
  return -1;
}

// alias routing: desktop DLL name -> console system DLL name (case-insensitive)
const char* dll_alias_lookup(const char* name) {
  for (const dll_alias& a : k_dll_aliases) {
    size_t i = 0;
    for (; a.guest_name[i] && name[i]; ++i)
      if (lower_ascii(a.guest_name[i]) != lower_ascii(name[i])) break;
    if (a.guest_name[i] == 0 && name[i] == 0) return a.system_name;
  }
  return nullptr;
}

// Native DLL override: a game-shipped DLL (dxvk's d3d9.dll, cnc-ddraw,
// dgVoodoo, the VC runtime, ...) shadows Tiramisu's builtin bridge for that
// module — the same WINEDLLOVERRIDES=native idea Proton uses. This is how
// existing third-party implementations get wired in: drop the DLL next to
// the game's .exe and it is loaded instead of our bridge.
//
// Only API modules are overridable; core system modules (kernel32, ntdll,
// user32, ...) are NEVER shadowed because a fake would break the loader's
// own assumptions.
bool is_overridable_module(const char* name) {
  static const char* kNoOverride[] = {
      "kernel32.dll", "ntdll.dll", "user32.dll", "gdi32.dll",
      "advapi32.dll", "ole32.dll", "oleaut32.dll", "comctl32.dll",
      "shell32.dll", "shlwapi.dll", "ws2_32.dll", "crypt32.dll",
      "bcrypt.dll", "sechost.dll", "rpcrt4.dll", "kernelbase.dll",
      "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll",
      "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
      "combase.dll", "dwrite.dll", "d2d1.dll", "d3d10.dll", "d3d12.dll",
      "dxgi.dll", "version.dll",
      // Match the built-in Steamworks route used by LoadLibrary and the
      // sibling resolver. Native Steamworks expects a running desktop client.
      "steam_api64.dll", "steam_api.dll",
  };
  for (const char* no : kNoOverride)
    if (name_eq(name, no)) return false;
  return true;
}

// Try to load <search_dir>/<module> and resolve `symbol` from it. Returns
// nullptr when no native DLL exists or the symbol isn't exported.
void* try_native_override(const char* module, const char* symbol,
                          u16 ordinal) {
  if (!module || !is_overridable_module(module)) return nullptr;
  const char* dir = trt_runtime_search_dir();
  if (!dir || !dir[0]) return nullptr;
  char host[1024];
  snprintf(host, sizeof(host), "%s/%s", dir, module);
  struct stat st{};
#ifdef _WIN32
  // The UWP (WINAPI_FAMILY_APP) UCRT hides the POSIX S_IS* macros; the CRT
  // file-mode bit is the same value.
  if (stat(host, &st) != 0 || !(st.st_mode & _S_IFREG)) return nullptr;
#else
  if (stat(host, &st) != 0 || !S_ISREG(st.st_mode)) return nullptr;
#endif

  // already loaded? reuse the registry handle
  trt_image* img = trt_images_find(module);
  if (!img) {
    img = trt_load_pe(host, module, 1 /*run DllMain*/);
    if (!img) return nullptr;
  }
  if (!img) return nullptr;
  return trt_pe_find_export(trt_runtime_backend(), trt_runtime_resolver(),
                            img, symbol, ordinal);
}

// resolve() adapter the loader consumes
void* resolve_through_table(void* user, const char* module,
                            const char* symbol, u16 ordinal) {
  (void)user;

  // ---- native DLL override -------------------------------------------
  // A game-shipped DLL shadows our builtin bridge for overridable API
  // modules (dxvk's d3d9.dll, cnc-ddraw, dgVoodoo, ...). Proton's
  // WINEDLLOVERRIDES=native equivalent.
  if (void* fn = try_native_override(module, symbol, ordinal)) return fn;

  // Ordinal-only import (hint/name parse produced no symbol, e.g. OrTMM's
  // COMCTL32 bare hint/ord 380): synthesize the "#<ordinal>" name the
  // builtin tables register so a BUILTIN route can bind it.
  char ord_name[16];
  if (!symbol && ordinal) {
    snprintf(ord_name, sizeof(ord_name), "#%u", (unsigned)ordinal);
    symbol = ord_name;
  }

  // ---- API-set contracts --------------------------------------------
  // "api-ms-win-core-synch-l1-2-0.dll" is not a file on the console; it
  // is a contract name. Redirect it to the DLL that implements it, then
  // resolve normally. The contract name is also tried literally against
  // the console, because some OS images DO carry the schema.
  const char* target = module;
  const char* contract_target = trt_apiset_target(module);
  if (contract_target) {
    target = contract_target;
    char line[224];
    snprintf(line, sizeof(line), "trt: apiset %s -> %s", module, target);
    trt_core_log(line);
  }

  const int mi = find_module(target);
  if (mi < 0) {
    // unknown module: not a hard fail — try the console's real DLL by the
    // same name (dwrite, d2d1, xmcore, secs... exist as system components),
    // then known aliases. Failure stays NAMED in the log (not silent).
    if (g_system_load) {
      if (void* fn = g_system_load(target, symbol)) return fn;
      if (target != module)
        if (void* fn2 = g_system_load(module, symbol)) return fn2;
      if (const char* alias = dll_alias_lookup(target))
        if (void* fn3 = g_system_load(alias, symbol)) return fn3;
      if (const char* alias2 = dll_alias_lookup(module))
        if (void* fn4 = g_system_load(alias2, symbol)) return fn4;
    }
    return nullptr;  // loader names module!symbol in its error
  }
  const builtin_mod& m = g_modules[mi];
  switch (m.route) {
    case TRT_ROUTE_STUB: {
      // fail-visible: the call site names itself when it is reached
      char sym[128];
      if (symbol) snprintf(sym, sizeof(sym), "%s", symbol);
      else snprintf(sym, sizeof(sym), "#%u", (unsigned)ordinal);
      return trt_thunk_make(trt_core_backend(), "STUB", target, sym);
    }
    case TRT_ROUTE_BUILTIN:
      if (symbol) {
        for (int i = 0; i < m.fn_count; ++i)
          if (strcmp(m.fns[i].name, symbol) == 0) return (void*)m.fns[i].fn;
        // Some SDK headers declare their exports inside C++ without
        // extern "C", so the import comes in MSVC-mangled:
        //   __imp_?CreateDXGIFactory2@@YAJAEBU_GUID@@PEAPEAX@Z
        // Fall back to the base name between '?' and the first '@@'.
        if (symbol[0] == '?') {
          const char* at = strstr(symbol, "@@");
          if (at && (size_t)(at - symbol) < 128) {
            char base[128];
            const size_t len = (size_t)(at - symbol - 1);
            memcpy(base, symbol + 1, len);
            base[len] = 0;
            for (int i = 0; i < m.fn_count; ++i)
              if (strcmp(m.fns[i].name, base) == 0) return (void*)m.fns[i].fn;
          }
        }
        // builtin module, unknown symbol: fall through to system by alias
        if (g_system_load) {
          if (void* fn = g_system_load(target, symbol)) return fn;
          if (target != module)
            if (void* fn2 = g_system_load(module, symbol)) return fn2;
          if (const char* alias = dll_alias_lookup(target))
            if (void* fn3 = g_system_load(alias, symbol)) return fn3;
        }
      }
      return nullptr;                       // named failure in loader log
    case TRT_ROUTE_SYSTEM:
      if (g_system_load) {
        if (void* fn = g_system_load(target, symbol)) return fn;
        if (target != module)
          if (void* fn2 = g_system_load(module, symbol)) return fn2;
        if (const char* alias = dll_alias_lookup(target))
          if (void* fn3 = g_system_load(alias, symbol)) return fn3;
      }
      // SYSTEM module with an attached fallback table: the console binds its
      // real DLL above and never reaches this; a host offline run has no
      // system DLL, so registered fallback exports serve — each must fail
      // NAMED when called (never ok-stubs).
      for (int i = 0; i < m.fn_count; ++i)
        if (symbol && strcmp(m.fns[i].name, symbol) == 0)
          return (void*)m.fns[i].fn;
      return nullptr;
    default:
      return nullptr;
  }
}

} // namespace

extern "C" {

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count) {
  if (!name) return;
  // last registration wins: a bridge registering "kernel32.dll" after the
  // defaults must REPLACE the default entry, not shadow it (find_module
  // returns the first match).
  const int existing = find_module(name);
  builtin_mod* m = nullptr;
  if (existing >= 0) {
    m = &g_modules[existing];
    // merging into an existing BUILTIN module: keep what is there and add
    // the new functions; a function that already exists is REPLACED by the
    // newer registration (a test harness or a later bridge can override)
    // a BUILTIN registration UPGRADES a STUB module (steam_api64 was a stub
    // until the auth bridge replaced it); a STUB registration never
    // downgrades an existing BUILTIN
    if (route == TRT_ROUTE_STUB && m->route == TRT_ROUTE_BUILTIN) return;
    if (m->route == TRT_ROUTE_BUILTIN && route == TRT_ROUTE_BUILTIN && fns) {
      for (int i = 0; i < fn_count; ++i) {
        bool replaced = false;
        for (int j = 0; j < m->fn_count; ++j) {
          if (strcmp(m->fns[j].name, fns[i].name) == 0) {
            m->fns[j] = fns[i];
            replaced = true;
            break;
          }
        }
        if (!replaced) {
          if (m->fn_count >= (int)(sizeof(m->fns) / sizeof(m->fns[0]))) {
            // fail-visible: a silently-dropped symbol is a boot death with
            // no named cause (the exact bug class the OrTMM audit caught)
            char line[192];
            snprintf(line, sizeof(line),
                     "trt: MODULE TABLE OVERFLOW - %s!%s NOT registered "
                     "(raise builtin_mod::fns capacity)",
                     name, fns[i].name);
            trt_core_log(line);
            continue;
          }
          m->fns[m->fn_count++] = fns[i];
        }
      }
      return;
    }
  } else {
    if (g_module_count >= (int)(sizeof(g_modules) / sizeof(g_modules[0]))) {
      char line[160];
      snprintf(line, sizeof(line),
               "trt: MODULE TABLE FULL - %s was NOT registered", name);
      trt_core_log(line);
      return;
    }
    m = &g_modules[g_module_count++];
  }
  memset(m, 0, sizeof(*m));
  strncpy(m->name, name, sizeof(m->name) - 1);
  m->route = route;
  const int cap = (int)(sizeof(m->fns) / sizeof(m->fns[0]));
  int copied = 0;
  for (; copied < fn_count && copied < cap; ++copied)
    m->fns[copied] = fns[copied];
  m->fn_count = copied;
  if (copied < fn_count) {
    char line[192];
    snprintf(line, sizeof(line),
             "trt: MODULE TABLE OVERFLOW - %s clipped %d/%d exports",
             name, copied, fn_count);
    trt_core_log(line);
  }
}

trt_route trt_module_route(const char* name) {
  const int mi = find_module(name);
  return mi < 0 ? TRT_ROUTE_MISSING : g_modules[mi].route;
}

// ---- offline bind-simulation query (corpus dry-run) -----------------------
// Same lookup discipline as resolve_through_table: apiset redirect first,
// then the registered table. Returns nonzero iff a BUILTIN route can bind
// the NAMED symbol (or the route always binds: STUB thunk / SYSTEM host).
// Unknown module or unknown BUILTIN symbol -> 0: the caller decides via its
// SYSTEM whitelist / game-dir search, exactly like the live loader would.
int trt_module_query(const char* module, const char* symbol) {
  if (!module) return 0;
  const char* target = trt_apiset_target(module);
  if (!target) target = module;
  const int mi = find_module(target);
  if (mi < 0) return 0;
  const builtin_mod& m = g_modules[mi];
  if (m.route == TRT_ROUTE_STUB || m.route == TRT_ROUTE_SYSTEM) return 1;
  if (m.route != TRT_ROUTE_BUILTIN || !symbol) return 0;
  for (int i = 0; i < m.fn_count; ++i)
    if (strcmp(m.fns[i].name, symbol) == 0) return 1;
  // MSVC-mangled imports: match the base name between '?' and '@@'
  // (same fallback resolve_through_table applies)
  if (symbol[0] == '?') {
    const char* at = strstr(symbol, "@@");
    if (at && (size_t)(at - symbol) < 128) {
      const size_t len = (size_t)(at - symbol - 1);
      char base[128];
      memcpy(base, symbol + 1, len);
      base[len] = 0;
      for (int i = 0; i < m.fn_count; ++i)
        if (strcmp(m.fns[i].name, base) == 0) return 1;
    }
  }
  return 0;
}

int trt_module_query_ordinal(const char* module, u16 ordinal) {
  if (!module || !ordinal) return 0;
  const char* target = trt_apiset_target(module);
  if (!target) target = module;
  const int mi = find_module(target);
  if (mi < 0) return 0;
  const builtin_mod& m = g_modules[mi];
  if (m.route == TRT_ROUTE_STUB || m.route == TRT_ROUTE_SYSTEM) return 1;
  if (m.route != TRT_ROUTE_BUILTIN) return 0;
  // ordinal-only BUILTIN binds iff the table registered "#<ordinal>"
  char ord_name[16];
  snprintf(ord_name, sizeof(ord_name), "#%u", (unsigned)ordinal);
  for (int i = 0; i < m.fn_count; ++i)
    if (strcmp(m.fns[i].name, ord_name) == 0) return 1;
  return 0;
}

void trt_set_system_loader(trt_system_load fn) { g_system_load = fn; }

void trt_resolver_default(trt_resolver* out) {
  out->user = nullptr;
  out->resolve = &resolve_through_table;
}

// exported so the guest-image resolver (images.cpp) can fall back to the
// module table for system modules
void* trt_resolve_module(void* user, const char* module, const char* symbol,
                         u16 ordinal) {
  return resolve_through_table(user, module, symbol, ordinal);
}

// stub entry point: one shared address; host-side logging wraps real call
// observation later (v0: presence in the IAT is itself logged per module)
void trt_stub_thunk(void) {}

// ---------------------------------------------------------------------------
// Module coverage for real PC games.
//
// A shipped game imports far more than kernel32/user32. Every name it
// imports must resolve to SOMETHING at load time or the game never starts:
//   - SYSTEM: the console ships the real DLL (host binds it by name)
//   - BUILTIN: we implement it (kernel32 core, user32 window layer)
//   - STUB: not implemented here; binds to a fail-visible thunk so the
//           game still boots and the log names the missing piece
// Registration never overwrites a module the bridges already registered.
namespace {

struct mod_default { const char* name; trt_route route; };

const mod_default k_defaults[] = {
    // ---- graphics / presentation (console ships these) ----
    {"d3d11.dll", TRT_ROUTE_SYSTEM},
    // Xbox UWP ships D3D12 (UWP app model; DirectStorage sits on it).
    // Provenance: Xbox UWP D3D12 is exposed through the API set schema and
    // LoadLibrary("d3d12.dll") resolves in the AppContainer.
    {"d3d12.dll", TRT_ROUTE_SYSTEM},
    {"d3d12core.dll", TRT_ROUTE_SYSTEM},
    // DirectStorage: games normally SHIP dstorage.dll/dstoragecore.dll in
    // their own directory (the Go Next corpus does); the console also
    // carries them, so route SYSTEM as fallback when the game does not
    // bring its own copy.
    {"dstorage.dll", TRT_ROUTE_SYSTEM},
    {"dstoragecore.dll", TRT_ROUTE_SYSTEM},
    {"dxgi.dll", TRT_ROUTE_SYSTEM},
    {"d3dcompiler_47.dll", TRT_ROUTE_SYSTEM},
    {"dcomp.dll", TRT_ROUTE_SYSTEM},
    {"d2d1.dll", TRT_ROUTE_SYSTEM},
    {"dwrite.dll", TRT_ROUTE_SYSTEM},
    // gdi32: no SYSTEM route — console LoadLibrary("gdi32.dll") fails in an
    // AppContainer (desktop-shell DLL), so a fake SYSTEM bind would HIDE
    // the gap. bridge_gdi registers it BUILTIN; this default is BUILTIN too
    // so the route is honest even before boot.
    {"gdi32.dll", TRT_ROUTE_BUILTIN},
    {"comctl32.dll", TRT_ROUTE_BUILTIN},  // M?: common-controls acceptance
    {"dwmapi.dll", TRT_ROUTE_SYSTEM},
    // UIAutomationCore: UnityPlayer imports the Uia* accessibility surface;
    // console has no UIA — STUB binds fail-visible, calls name themselves
    {"uiautomationcore.dll", TRT_ROUTE_STUB},
    // GoW (DECIMA PC) imports WLDAP32 by ordinals; no console wldap32 exists
    // — STUB binds fail-visible so a real call names itself
    {"wldap32.dll", TRT_ROUTE_STUB},
    {"uxtheme.dll", TRT_ROUTE_BUILTIN},
    {"msimg32.dll", TRT_ROUTE_BUILTIN},
    {"vulkan-1.dll", TRT_ROUTE_BUILTIN},
    {"comdlg32.dll", TRT_ROUTE_BUILTIN},
    {"oleacc.dll", TRT_ROUTE_BUILTIN},

    // ---- COM / OLE / WinRT ----
    {"ole32.dll", TRT_ROUTE_BUILTIN},  // M7: COM boot (bridge_ole)
    {"oleaut32.dll", TRT_ROUTE_BUILTIN},  // M7: BSTR/variant (bridge_ole)
    {"combase.dll", TRT_ROUTE_SYSTEM},

    // ---- core runtime / CRT ----
    {"ucrtbase.dll", TRT_ROUTE_SYSTEM},
    {"api-ms-win-crt-runtime-l1-1-0.dll", TRT_ROUTE_SYSTEM},
    {"ntdll.dll", TRT_ROUTE_SYSTEM},

    // ---- networking ----
    {"ws2_32.dll", TRT_ROUTE_SYSTEM},
    {"mswsock.dll", TRT_ROUTE_BUILTIN},
    {"winhttp.dll", TRT_ROUTE_SYSTEM},
    {"wininet.dll", TRT_ROUTE_BUILTIN},   // STAGE 5: WinINet not available to
                                          // UWP — honest-runtime bridge
    {"dnsapi.dll", TRT_ROUTE_BUILTIN},
    {"iphlpapi.dll", TRT_ROUTE_BUILTIN},  // STAGE 5: honest no-data bridge
    {"netapi32.dll", TRT_ROUTE_BUILTIN},
    {"mpr.dll", TRT_ROUTE_BUILTIN},
    {"dhcpcsvc.dll", TRT_ROUTE_BUILTIN},
    {"normaliz.dll", TRT_ROUTE_BUILTIN},

    // ---- crypto / security / services ----
    {"bcrypt.dll", TRT_ROUTE_SYSTEM},
    {"ncrypt.dll", TRT_ROUTE_BUILTIN},
    {"crypt32.dll", TRT_ROUTE_BUILTIN},   // STAGE 5: no cert store on console
                                          // — honest-fail bridge
    {"cryptui.dll", TRT_ROUTE_BUILTIN},
    {"secur32.dll", TRT_ROUTE_BUILTIN},
    {"sechost.dll", TRT_ROUTE_BUILTIN},
    {"sspicli.dll", TRT_ROUTE_BUILTIN},
    {"wintrust.dll", TRT_ROUTE_BUILTIN},
    {"credui.dll", TRT_ROUTE_BUILTIN},

    // ---- shell / env / misc ----
    {"shlwapi.dll", TRT_ROUTE_BUILTIN},  // M7: Path* helpers (bridge_shlwapi)
    {"shell32.dll", TRT_ROUTE_BUILTIN},  // M7: SHGetFolderPath (bridge_shell)
    {"shfolder.dll", TRT_ROUTE_BUILTIN},  // M7: shim onto SHGetFolderPath
    {"shcore.dll", TRT_ROUTE_SYSTEM},
    {"version.dll", TRT_ROUTE_SYSTEM},
    {"propsys.dll", TRT_ROUTE_BUILTIN},
    {"userenv.dll", TRT_ROUTE_BUILTIN},
    {"psapi.dll", TRT_ROUTE_BUILTIN},     // STAGE 5: real guest module list
                                          // from the image registry
    {"hid.dll", TRT_ROUTE_SYSTEM},
    {"setupapi.dll", TRT_ROUTE_SYSTEM},
    {"avrt.dll", TRT_ROUTE_SYSTEM},
    {"imm32.dll", TRT_ROUTE_BUILTIN},     // STAGE 5: no IME on console —
                                          // honest no-IME bridge
    {"usp10.dll", TRT_ROUTE_BUILTIN},
    {"wtsapi32.dll", TRT_ROUTE_BUILTIN},
    {"dbghelp.dll", TRT_ROUTE_BUILTIN},   // STAGE 5: symbol records + real
                                          // text crash-report bridge
    {"dbgcore.dll", TRT_ROUTE_BUILTIN},   // STAGE 5: dump engine (same surface)
    {"dxcore.dll", TRT_ROUTE_BUILTIN},
    {"directxmath.dll", TRT_ROUTE_BUILTIN},

    // ---- input: XInput (console = XINPUTUAP) ----
    {"xinput1_4.dll", TRT_ROUTE_SYSTEM},

    // ---- no console equivalent: fail-visible work items ----
    // (d3d9/d3d9core default to STUB here; trt_bridge_boot upgrades them
    // to the BUILTIN D3D9->D3D11 bridge)
    {"d3d9.dll", TRT_ROUTE_STUB},
    {"d3d9core.dll", TRT_ROUTE_STUB},
    {"d3d8.dll", TRT_ROUTE_BUILTIN},     // M7: D3D8 -> same backend hooks
    {"openal32.dll", TRT_ROUTE_BUILTIN}, // M7: OpenAL 1.1 -> console sink
    {"dsound.dll", TRT_ROUTE_BUILTIN},   // M6: DirectSound -> console sink
    {"ddraw.dll", TRT_ROUTE_BUILTIN},    // M6: DirectDraw -> backend present
    {"opengl32.dll", TRT_ROUTE_BUILTIN}, // OpenGL 1.x-4.x -> GLSL->HLSL -> D3D11
    {"glu32.dll", TRT_ROUTE_STUB},
    {"dinput8.dll", TRT_ROUTE_STUB},     // console input = XInput
    {"dinput.dll", TRT_ROUTE_STUB},
    {"gameinput.dll", TRT_ROUTE_STUB},   // GDK input
    {"advapi32.dll", TRT_ROUTE_STUB},    // M4: registry (HKLM/HKCU hive)
    {"msacm32.dll", TRT_ROUTE_BUILTIN},  // M6: codec probes (PCM only)
    {"msvfw32.dll", TRT_ROUTE_BUILTIN},  // M6: no AVI codecs (skip logos)
    {"winspool.drv", TRT_ROUTE_BUILTIN}, // M6: no printers
    {"d3dx9_43.dll", TRT_ROUTE_BUILTIN}, // M6: D3DX math/textures/sprites
    {"d3dx9_40.dll", TRT_ROUTE_BUILTIN},
    {"d3dx9_36.dll", TRT_ROUTE_BUILTIN},
    {"d3dx9_34.dll", TRT_ROUTE_BUILTIN},
    {"d3dx9_32.dll", TRT_ROUTE_BUILTIN},
    {"d3dx9_30.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_43.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_39.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_38.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_37.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_36.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_35.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_34.dll", TRT_ROUTE_BUILTIN},
    {"d3dx10_33.dll", TRT_ROUTE_BUILTIN},
    {"d3dx11_43.dll", TRT_ROUTE_BUILTIN},
    {"d3dx11_47.dll", TRT_ROUTE_BUILTIN},
    // steam: registered BUILTIN by bridge_steam (auth-only, no DRM bypass);
    // this STUB entry is the fallback if the bridge never registered
    {"steam_api64.dll", TRT_ROUTE_STUB},
    {"steam_api.dll", TRT_ROUTE_STUB},
    // Media Foundation + DXVA (overridden BUILTIN by bridge_mf)
    {"mfplat.dll", TRT_ROUTE_BUILTIN},
    {"mfreadwrite.dll", TRT_ROUTE_BUILTIN},
    {"mf.dll", TRT_ROUTE_BUILTIN},
    {"mfuuid.dll", TRT_ROUTE_BUILTIN},
    {"evr.dll", TRT_ROUTE_BUILTIN},
    {"dxva2.dll", TRT_ROUTE_BUILTIN},
    // Legacy XAudio2/X3DAudio/XAPOFX (overridden BUILTIN by bridge_audio)
    {"xaudio2_7.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_6.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_5.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_4.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_3.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_2.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_1.dll", TRT_ROUTE_BUILTIN},
    {"xaudio2_0.dll", TRT_ROUTE_BUILTIN},
    {"x3daudio1_7.dll", TRT_ROUTE_BUILTIN},
    {"xapofx1_5.dll", TRT_ROUTE_BUILTIN},
    // Power / Telemetry / WER / Session (overridden BUILTIN by bridge_sys_extras)
    {"powrprof.dll", TRT_ROUTE_BUILTIN},
    {"pdh.dll", TRT_ROUTE_BUILTIN},
    {"wer.dll", TRT_ROUTE_BUILTIN},
};

} // namespace

void trt_modules_register_defaults(void) {
  for (size_t i = 0; i < sizeof(k_defaults) / sizeof(k_defaults[0]); ++i) {
    if (find_module(k_defaults[i].name) >= 0) continue;  // bridge wins
    trt_module_register(k_defaults[i].name, k_defaults[i].route, nullptr, 0);
  }
}

} // extern "C"
