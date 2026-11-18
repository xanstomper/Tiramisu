// tiramisu/tests/test_corpus.cpp
// The real-game corpus load matrix, hardware-free — with the STAGE 4
// FILE-LEVEL DRY-RUN audit:
//
//   for every PE on argv: read the file bytes, check MZ + machine (x86
//   guests are SKIPped), then DRY-RUN the import directory DIRECTLY FROM
//   THE FILE (no mapping, no code execution): parse the PE import table
//   in-place and bind-simulate every import:
//
//     BUILTIN binds iff the symbol is registered  -> trt_module_query /
//                                                   trt_module_query_ordinal
//     APISET contracts                            -> trt_apiset_target, then
//                                                   the target module
//     SYSTEM binds iff on the console whitelist   -> whitelist_has()
//     the PE file's OWN DIRECTORY is searched FIRST for game-shipped DLLs
//       (mirror of the console host searching the game dir via VFS): a
//       sibling DLL binds whatever it exports (export table check via the
//       same on-file walk; a sibling that itself fails the dry-run reports
//       its own gap line)
//     everything else fails NAMED
//
//   The dry-run prints one report line per imported DLL REGARDLESS of what
//   the live map does afterwards:
//
//     <dll> route=<SYSTEM|BUILTIN|STUB|APISET|GAMEDIR> imports=<n> bound=<n>
//           unresolved=<names-or-dash>
//
//   Then the LIVE map attempt runs (the exact path test_real_load proves:
//   mmap backend -> trt_map_pe -> trt_seal_image through
//   trt_resolver_default + trt_modules_register_defaults + the game-dir
//   image loader). Verdict per PE:
//     LOAD OK <name>                       live map succeeded
//     LOAD FAILED <name>: <first named failure>
//   The PER-DLL REPORT EXISTS EITHER WAY — that is what turns a boot
//   failure into the complete work list offline.
//
//   Summary: "CORPUS: <n> ok, <m> failed, <k> skipped". Exit 0 iff no
//   LOAD FAILED.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/pe.h"
#include "../src/trt/thunk.h"
#include "../src/trt/apiset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <dirent.h>
#include <strings.h>

static int g_uncertain_bound = 0;  // SYSTEM(C) binds across the corpus
static int g_route_mismatch = 0;   // a bind route outside the proven set
static int g_route_total = 0;      // DLL report lines bound in the live walk
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

// ---------------- mmap backend (same shape as test_real_load) --------------
namespace {

struct stub_rec { char name[96]; int count; };
stub_rec g_stub_dlls[64];
int g_stub_count = 0;

char stub_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
bool stub_ieq(const char* a, const char* b) {
  size_t i = 0;
  for (; a[i] && b[i]; ++i)
    if (stub_lower(a[i]) != stub_lower(b[i])) return false;
  return a[i] == b[i];
}

int stub_count_for(const char* dll) {
  for (int i = 0; i < g_stub_count; ++i)
    if (stub_ieq(g_stub_dlls[i].name, dll)) return g_stub_dlls[i].count;
  return 0;
}

void stub_reset() { g_stub_count = 0; }

struct extent { void* base; size_t len; };
extent g_extents[65536];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  if (preferred_base) {
    const uintptr_t want = (uintptr_t)preferred_base;
    const uintptr_t want_end = want + bytes;
    int active = 0;
    for (int i = 0; i < g_extent_count; ++i) {
      if (!g_extents[i].base) continue;
      ++active;
      const uintptr_t b = (uintptr_t)g_extents[i].base;
      const uintptr_t e = b + g_extents[i].len;
      if (want < e && b < want_end) return nullptr;
    }
    if (active >= 65536) return nullptr;
  }
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
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

void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  // STUB bind observation: module_table routes a STUB symbol through
  // trt_thunk_make, which logs exactly:
  //   trt: STUB <dll>!<sym> -> fail-visible thunk
  // Count them per-dll so the report can say stubs=<n> honestly (how many
  // of the dll's bound imports are fail-visible thunks vs real bindings).
  if (const char* m = strstr(line, "trt: STUB ")) {
    m += 10;  // past "trt: STUB "
    const char* bang = strchr(m, '!');
    if (bang && (size_t)(bang - m) < sizeof(g_stub_dlls[0].name)) {
      char dll[96];
      snprintf(dll, sizeof(dll), "%.*s", (int)(bang - m), m);
      // case-insensitive match against the tracked set
      for (int i = 0; i < g_stub_count; ++i) {
        if (stub_ieq(g_stub_dlls[i].name, dll)) { ++g_stub_dlls[i].count; return; }
      }
      if (g_stub_count < 64) {
        snprintf(g_stub_dlls[g_stub_count].name,
                 sizeof(g_stub_dlls[0].name), "%s", dll);
        g_stub_dlls[g_stub_count].count = 1;
        ++g_stub_count;
      }
    }
  }
}

// ---------------- the CONSOLE's system loader (offline model) --------------
// Provenance of the whitelist (docs/02-bridge-surface.md + module_table.cpp):
//
//   The SYSTEM route means "the console ships the real DLL". The fixed
//   whitelist below is the console reality this test models:
//
//   - xboxnative console evidence (the k_defaults SYSTEM set in
//     module_table.cpp): hid, setupapi, version, shcore, avrt, d3d11, dxgi,
//     d2d1, dwrite, dcomp, xinput1_4 (as XINPUTUAP), ws2_32, ucrtbase.
//   - STAGE 4 addition, module_table.cpp: d3d12.dll / d3d12core.dll (Xbox
//     UWP ships D3D12 — UWP API set) and dstorage.dll / dstoragecore.dll
//     (games normally ship their own; SYSTEM is the console fallback).
//   - api-ms-win-* contracts: not files on the console; the apiset layer
//     (apiset.cpp) redirects them to their implementing DLL BEFORE the
//     system loader is consulted, so every contract prefix is accepted.
//   - the UWP core binaries EVERY AppContainer process provably has:
//     kernel32, kernelbase, ntdll, user32, combase.
//   - CRT-family DLLs real MSVC-linked games import by name (vcruntime140/
//     msvcp140 via their _app aliases in module_table.cpp).
//
//   Desktop-shell DLLs (gdi32, comctl32, shlwapi) are DELIBERATELY NOT
//   whitelisted: console LoadLibrary for them fails in an AppContainer, so
//   a fake SYSTEM bind would hide that. Games must get them from BUILTIN
//   bridges (bridge_gdi / bridge_comctl / bridge_shlwapi) — the corpus
//   report proves it with stubs=0 on those lines.
//
//   Anything NOT on the whitelist returns failure -> the import routes
//   STUB (if the module table has it) or fails the load NAMED. Never silent.
//   NOTE: advapi32 is deliberately NOT whitelisted: the module table routes
//   it STUB, but our registry bridge (bridge_reg) registers it BUILTIN with
//   the Reg* surface only — token APIs (OpenProcessToken & co) fall through
//   to BUILTIN-symbol-missing -> system loader, which also says no: the
//   import fails NAMED. That is the honest work list, not a fake bind.
// Provenance classes for the offline console model. The class rides the
// report line so a LOAD OK can never be misread as "console-boot-proven"
// for a DLL whose console presence is only assumed:
//   P = PROVEN    — the xboxnative donor lane loaded it on console hardware
//   U = UWP       — in the documented UWP API family (OS ships it for
//                   AppContainer processes; the surface is the OS's)
//   C = UNCERTAIN — a real game imports it; console presence unproven.
//                   The console run decides (a named LoadLibrary failure
//                   there is the answer, and the corpus already predicted it)
struct console_dll { const char* name; char cls; };
const console_dll k_console_dlls[] = {
    // ---- P: xboxnative donor lane (hardware-proven) ----
    {"hid.dll", 'P'}, {"setupapi.dll", 'P'}, {"version.dll", 'P'},
    {"shcore.dll", 'P'}, {"avrt.dll", 'P'},
    // ---- U: documented UWP API family ----
    {"d3d11.dll", 'U'}, {"dxgi.dll", 'U'}, {"d2d1.dll", 'U'},
    {"dwrite.dll", 'U'}, {"dcomp.dll", 'U'}, {"xinput1_4.dll", 'U'},
    {"ws2_32.dll", 'U'}, {"d3d12.dll", 'U'}, {"d3d12core.dll", 'U'},
    {"bcrypt.dll", 'U'}, {"winhttp.dll", 'U'},
    {"kernel32.dll", 'U'}, {"kernelbase.dll", 'U'}, {"ntdll.dll", 'U'},
    {"ucrtbase.dll", 'U'}, {"combase.dll", 'U'}, {"user32.dll", 'U'},
    {"advapi32.dll", 'U'},  // token half UWP-documented; registry half is BUILTIN
    // ---- CRT redist aliases (distributable, ship-in-package class) ----
    {"vcruntime140.dll", 'U'}, {"msvcp140.dll", 'U'},
    {"vcruntime140_app.dll", 'U'}, {"msvcp140_app.dll", 'U'},
    // ---- C->BUILTIN conversion rule (STAGE 5): a C-class DLL never boots
    //      deterministically; convert to BUILTIN honest-runtime and the boot
    //      stops depending on unproven console presence.
    //      dbghelp, dbgcore, crypt32, psapi, iphlpapi, wininet, imm32 have
    //      been converted (bridge_wininet/psapi/iphlpapi/imm32/crypt32/
    //      dbghelp) and LEFT this whitelist entirely, like comctl32/gdi32/
    //      shlwapi before them. The whitelist keeps ONLY P and U classes.
    // dstorage/dstoragecore: converted to BUILTIN (bridge_dstorage) — the
    // C->BUILTIN rule below. UnityPlayer 6 imports DStorageGetFactory even
    // when the game does not ship the DLL, so a SYSTEM(C) route made the
    // boot depend on unproven console presence.
    // NOTE: gdi32 / comctl32 / shlwapi are NOT here — desktop-shell DLLs,
    // console LoadLibrary fails in an AppContainer. They are BUILTIN
    // bridges now; whitelisting them would fake the bind.
};

char wl_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool wl_ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (wl_lower(*a) != wl_lower(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

char whitelist_class(const char* module) {
  if (!module) return '?';
  for (const console_dll& d : k_console_dlls)
    if (wl_ieq(d.name, module)) return d.cls;
  return '?';
}

bool whitelist_has(const char* module) {
  if (!module) return false;
  // api-ms-win-* contracts: accepted by prefix (the apiset layer usually
  // redirects first; some OS images also carry the schema literally).
  if (wl_lower(module[0]) == 'a' && wl_lower(module[1]) == 'p' &&
      wl_lower(module[2]) == 'i' && module[3] == '-' && module[4] == 'm' &&
      module[5] == 's')
    return true;
  for (const console_dll& d : k_console_dlls)
    if (wl_ieq(d.name, module)) return true;
  return false;
}

// The address a whitelisted SYSTEM module's function binds to in this
// offline model. On the console this is the real GetProcAddress result;
// here it is one shared Win64-ABI function that returns 0. Load-only test:
// the entry point is never run, so nothing reaches it. (We deliberately do
// NOT route these through trt_thunk_make: the shared thunk table's page
// layout only holds 64 entries per allocated page, and a corpus-sized
// symbol flood overruns it. On the console these binds are real
// GetProcAddress results anyway.)
u64 TRT_GUEST_ABI console_system_fn(void) { return 0; }

void* console_system_load(const char* module, const char* symbol) {
  if (!module) return nullptr;
  if (!whitelist_has(module)) return nullptr;   // routes STUB / fails named
  // Ordinal-only import (symbol == NULL): on the console this is
  // GetProcAddress by ordinal against the real DLL, which binds. Bind a
  // generic Win64-ABI address (never called in this load-only test).
  if (!symbol) return (void*)&console_system_fn;  // ordinal import
  return (void*)&console_system_fn;               // name import: real-on-console
}

// ---------------- file read ----------------
u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n ? (size_t)n : 1);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u16 rd16b(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 rd32b(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
u64 rd64b(const u8* p) {
  u64 v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

const char* base_name(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// route label for a report line (apiset contracts report as APISET)
void route_label(const char* dll, char* out, size_t cap) {
  if (trt_apiset_is_contract(dll)) { snprintf(out, cap, "APISET"); return; }
  switch (trt_module_route(dll)) {
    case TRT_ROUTE_SYSTEM: {
      char c = whitelist_class(dll);
      snprintf(out, cap, c == 'P' ? "SYSTEM(P)" : (c == 'U' ? "SYSTEM(U)" : "SYSTEM(C)"));
      break;
    }
    case TRT_ROUTE_BUILTIN: snprintf(out, cap, "BUILTIN"); break;
    case TRT_ROUTE_STUB:    snprintf(out, cap, "STUB");    break;
    default:                snprintf(out, cap, "SYSTEM");  break; // console-real
  }
}

// ---------------------------------------------------------------------------
// STAGE 4: FILE-LEVEL DRY-RUN AUDIT
//
// Parse the import directory directly from the file bytes (RVA -> file
// offset via the section table) and bind-simulate every import. Prints the
// same per-DLL report lines as the live walk, plus the first named gap.
// Returns 0 iff every import binds; *first_gap gets "<dll>!<sym>".
//
// Bind simulation, in loader order (mirrors trt_resolve_full +
// resolve_through_table):
//   1. game-dir sibling DLL that exports the symbol (the console host
//      searches the game dir FIRST via VFS)
//   2. APISET contract -> target module, then module table query
//   3. BUILTIN iff trt_module_query says the fn is registered
//   4. STUB always binds (fail-visible thunk)
//   5. SYSTEM iff on the console whitelist
//   6. otherwise NAMED failure
// ---------------------------------------------------------------------------

// File PE header accessors through the packed trt_* structs (no raw offset
// math — the struct layout IS the PE layout, pack(1)).
struct file_pe {
  const u8* file; size_t size;
  const trt_nt_headers64* nt;
  const trt_section_header* secs; u32 nsec;
};
bool file_pe_open(const u8* file, size_t size, file_pe* out) {
  if (size < 0x40 + 8 || rd16b(file) != 0x5A4D) return false;
  const u64 nt_off = rd32b(file + 0x3C);
  if (nt_off + sizeof(trt_nt_headers64) > size ||
      rd32b(file + nt_off) != 0x00004550)
    return false;
  out->file = file;
  out->size = size;
  out->nt = (const trt_nt_headers64*)(file + nt_off);
  out->nsec = rd16b((const u8*)&out->nt->file_header.number_of_sections);
  if (out->nsec == 0 || out->nsec > 96) return false;
  out->secs = (const trt_section_header*)(
      (const u8*)&out->nt->optional_header +
      rd16b((const u8*)&out->nt->file_header.size_of_optional_header));
  if ((const u8*)out->secs + (size_t)out->nsec * sizeof(trt_section_header) >
      file + size)
    return false;
  return true;
}

// RVA -> file offset through the section table (fall back to the headers
// region where RVA == file offset). 0 = unreadable (BSS tail / out of file).
u64 rva_to_file(const u8* file, size_t size, const trt_section_header* secs,
                u32 nsec, u64 rva) {
  for (u32 i = 0; i < nsec; ++i) {
    const u64 va = rd32b((const u8*)&secs[i].virtual_address);
    const u64 vsz = rd32b((const u8*)&secs[i].virtual_size);
    const u64 rsz = rd32b((const u8*)&secs[i].size_of_raw_data);
    const u64 roff = rd32b((const u8*)&secs[i].pointer_to_raw_data);
    if (rva >= va && rva < va + (vsz ? vsz : rsz)) {
      const u64 delta = rva - va;
      if (delta < rsz && roff + delta < size) return roff + delta;
      return 0;  // in BSS tail / out of file: no bytes to inspect
    }
  }
  // headers region: RVA == file offset
  if (rva + 1 < size) return rva;
  return 0;
}

// Does a file PE export this name or ordinal? (dry-run export table walk)
// MSVC-mangled imports ("?CreateDXGIFactory2@@...") also match the export
// whose decorated name starts with the same base between '?' and the first
// '@' (the same fallback resolve_through_table applies).
bool file_exports(const u8* file, size_t size, const u8* sec_base, u32 nsec,
                  const char* name, u16 ordinal) {
  file_pe pe;
  if (!file_pe_open(file, size, &pe)) return false;
  (void)sec_base; (void)nsec;
  const trt_data_directory* dir =
      &pe.nt->optional_header.directories[TRT_PE_DIR_EXPORT];
  if (!dir->virtual_address || !dir->size) return false;
  const u64 ed_off =
      rva_to_file(file, size, pe.secs, pe.nsec, dir->virtual_address);
  if (!ed_off || ed_off + 40 > size) return false;
  const u32 n_funcs = rd32b(file + ed_off + 20);
  const u32 n_names = rd32b(file + ed_off + 24);
  const u32 ord_base = rd32b(file + ed_off + 16);
  if (n_funcs > (1u << 20) || n_names > (1u << 20)) return false;

  if (name) {
    const u64 names_off = rva_to_file(file, size, pe.secs, pe.nsec,
                                      rd32b(file + ed_off + 32));
    if (!names_off) return false;
    char base[128];
    const char* base_at = nullptr;
    if (name[0] == '?') {
      const char* at = strstr(name, "@@");
      if (at && (size_t)(at - name) < 128) {
        const size_t len = (size_t)(at - name - 1);
        memcpy(base, name + 1, len);
        base[len] = 0;
        base_at = base;
      }
    }
    for (u32 i = 0; i < n_names; ++i) {
      const u64 nm_rva = rd32b(file + names_off + (size_t)i * 4);
      const u64 nm_off = rva_to_file(file, size, pe.secs, pe.nsec, nm_rva);
      if (!nm_off || nm_off >= size) continue;
      const char* sym = (const char*)(file + nm_off);
      if (strncmp(sym, name, 256) == 0) return true;
      if (base_at && strncmp(sym, base_at, strlen(base_at)) == 0 &&
          (sym[strlen(base_at)] == '@' || sym[strlen(base_at)] == 0))
        return true;
    }
    return false;
  }
  // ordinal import: within [base, base + count)
  return ordinal >= ord_base && (u32)ordinal - ord_base < n_funcs;
}

// The game dir being tested (set before each PE). Sibling search = the
// console host searching the game dir via VFS.
char g_game_dir[1024] = "";

// Sibling DLL file in the game dir, or nullptr. Caller frees.
u8* find_game_sibling(const char* dll, char* out_path, size_t cap,
                      size_t* out_size) {
  if (!g_game_dir[0]) return nullptr;
  char path[1280];
  snprintf(path, sizeof(path), "%s/%s", g_game_dir, dll);
  size_t sz = 0;
  u8* buf = read_file(path, &sz);
  if (!buf) {
    // the import table's case need not match the shipped file's case (the
    // loader resolves case-insensitively; the dry-run probe must too)
    DIR* d = opendir(g_game_dir);
    if (d) {
      struct dirent* e;
      while ((e = readdir(d))) {
        if (strcasecmp(e->d_name, dll) == 0) {
          snprintf(path, sizeof(path), "%s/%s", g_game_dir, e->d_name);
          buf = read_file(path, &sz);
          break;
        }
      }
      closedir(d);
    }
    if (!buf) return nullptr;
  }
  snprintf(out_path, cap, "%s", path);
  *out_size = sz;
  return buf;
}

// A file PE's section table (for rva_to_file on siblings) — struct-based.
bool file_sec_table(const u8* file, size_t size, const trt_section_header** secs,
                    u32* nsec) {
  file_pe pe;
  if (!file_pe_open(file, size, &pe)) return false;
  *secs = pe.secs;
  *nsec = pe.nsec;
  return true;
}

struct dry_result {
  int imports = 0;
  int bound = 0;
  char unresolved[512];
  char first_gap[224];
};

void append_csv(char* dst, size_t cap, const char* item) {
  if (!dst || !item || cap == 0) return;
  size_t len = strlen(dst);
  if (len + 1 >= cap) return;
  if (len > 0) {
    dst[len++] = ',';
    dst[len] = 0;
  }
  size_t ilen = strlen(item);
  size_t room = cap - 1 - len;
  if (ilen > room) ilen = room;
  memcpy(dst + len, item, ilen);
  dst[len + ilen] = 0;
}

// dry-run one import descriptor's thunks (file offsets, not RVAs)
void dry_thunks(const u8* file, size_t size, const trt_section_header* secs,
                u32 nsec, const char* dll_name, u64 int_off, u64 iat_off,
                dry_result* out) {
  out->unresolved[0] = 0;
  out->first_gap[0] = 0;
  if (!int_off || int_off + 8 > size) return;  // nothing readable to walk
  for (u64 t = 0;; t += 8) {
    const u64 t_off = int_off + t;
    if (t_off + 8 > size) break;
    const u64 thunk = rd64b(file + t_off);
    if (thunk == 0) break;
    ++out->imports;
    const char* sym_name = nullptr;
    u16 ordinal = 0;
    if (thunk >> 63) {
      ordinal = (u16)(thunk & 0xFFFF);
    } else {
      const u64 hn_off = rva_to_file(file, size, secs,
                                     nsec, (u32)(thunk & 0x7FFFFFFFu));
      if (!hn_off || hn_off + 2 >= size) {
        char one[160];
        snprintf(one, sizeof(one), "%s!<bad hint/name rva>", dll_name);
        if (!out->first_gap[0]) snprintf(out->first_gap, sizeof(out->first_gap), "%s", one);
        append_csv(out->unresolved, sizeof(out->unresolved), one);
        continue;
      }
      sym_name = (const char*)(file + hn_off + 2);
      if (strnlen(sym_name, 256) == 256) {
        char one[160];
        snprintf(one, sizeof(one), "%s!<unterminated symbol>", dll_name);
        if (!out->first_gap[0]) snprintf(out->first_gap, sizeof(out->first_gap), "%s", one);
        append_csv(out->unresolved, sizeof(out->unresolved), one);
        continue;
      }
    }
    (void)iat_off;

    bool binds = false;

    // ---- 1. game-dir sibling FIRST (console host searches the game dir
    //         via VFS before consulting the system) ----
    char sib_path[1280];
    size_t sib_size = 0;
    u8* sib = find_game_sibling(dll_name, sib_path, sizeof(sib_path), &sib_size);
    if (sib) {
      const u8* sib_sec = nullptr;
      u32 sib_nsec = 0;
      const trt_section_header* sib_secs = nullptr;
      if (rd16b(sib) == 0x5A4D &&
          file_sec_table(sib, sib_size, &sib_secs, &sib_nsec)) {
        binds = file_exports(sib, sib_size, (const u8*)sib_secs, sib_nsec,
                             sym_name, ordinal);
      }
      free(sib);
    }

    // ---- 2/3/4. module table (+ apiset redirect), STUB always binds ----
    if (!binds && sym_name) {
      binds = trt_module_query(dll_name, sym_name) != 0;
    } else if (!binds && ordinal) {
      binds = trt_module_query_ordinal(dll_name, ordinal) != 0;
    }

    // ---- 5. SYSTEM iff on the console whitelist ----
    if (!binds && whitelist_has(dll_name)) binds = true;

    if (binds) {
      ++out->bound;
    } else {
      char one[160];
      if (sym_name) snprintf(one, sizeof(one), "%s!%s", dll_name, sym_name);
      else snprintf(one, sizeof(one), "%s!#%u", dll_name, (unsigned)ordinal);
      if (!out->first_gap[0]) snprintf(out->first_gap, sizeof(out->first_gap), "%s", one);
      append_csv(out->unresolved, sizeof(out->unresolved), one);
    }
  }
}

void dry_run_pe(const char* path, const char* name, const u8* file,
                size_t size) {
  (void)path;
  file_pe pe;
  if (!file_pe_open(file, size, &pe)) {
    printf("  dry-run: PE HEADERS UNREADABLE\n");
    return;
  }
  const trt_data_directory* imp_dir =
      &pe.nt->optional_header.directories[TRT_PE_DIR_IMPORT];
  const u32 imp_va = imp_dir->virtual_address;
  const u32 imp_sz = imp_dir->size;
  if (!imp_va || !imp_sz) {
    printf("  dry-run: (no import directory)\n");
    return;
  }
  const u64 imp_off = rva_to_file(file, size, pe.secs, pe.nsec, imp_va);
  if (!imp_off) {
    printf("  dry-run: IMPORT DIR UNREADABLE (rva=%x)\n", imp_va);
    return;
  }

  printf("  dry-run (file-level import audit, game dir first):\n");
  for (u32 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz &&
                  imp_off + d + sizeof(trt_import_descriptor) <= size;
       d += sizeof(trt_import_descriptor)) {
    const u8* dp = file + imp_off + d;
    const u32 oft = rd32b(dp);
    const u32 name_rva = rd32b(dp + 12);
    const u32 iat_rva = rd32b(dp + 16);
    if (!iat_rva && !name_rva) break;  // terminator
    const u64 nm_off = rva_to_file(file, size, pe.secs, pe.nsec, name_rva);
    if (!nm_off || nm_off >= size) {
      printf("    <bad dll name rva %x> route=? imports=0 bound=0\n", name_rva);
      continue;
    }
    char dll_name[96];
    snprintf(dll_name, sizeof(dll_name), "%s", (const char*)(file + nm_off));
    const u64 int_off = rva_to_file(file, size, pe.secs, pe.nsec,
                                    oft ? oft : iat_rva);
    dry_result r;
    if (int_off) {
      dry_thunks(file, size, pe.secs, pe.nsec, dll_name, int_off, 0, &r);
    } else {
      r.imports = 0; r.bound = 0;
      r.unresolved[0] = 0; r.first_gap[0] = 0;
    }

    // route label: a dll the game ships in its own dir reports GAMEDIR
    char route[16];
    char sib_probe[1280];
    size_t sib_sz = 0;
    u8* sib = find_game_sibling(dll_name, sib_probe, sizeof(sib_probe), &sib_sz);
    if (sib) { snprintf(route, sizeof(route), "GAMEDIR"); free(sib); }
    else route_label(dll_name, route, sizeof(route));
    ++g_route_total;  // boot-determinism ledger: every reported DLL line

    printf("    %s route=%s imports=%d bound=%d stubs=%d unresolved=%s\n",
           dll_name, route, r.imports, r.bound, stub_count_for(dll_name),
           r.unresolved[0] ? r.unresolved : "-");
  }
}

// ---------------------------------------------------------------------------
// live map + bound-import walk (unchanged discipline from STAGE 3)
// ---------------------------------------------------------------------------

int run_corpus(int argc, char** argv) {
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};

  // boot exactly like the console host does (idempotent across the corpus)
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);
  // Load-only: map/bind/seal game DLLs, never run their DllMain/TLS
  trt_set_image_code_run(0);

  printf("tiramisu real-game corpus load matrix\n");

  int n_ok = 0, n_failed = 0, n_skipped = 0;
  bool any_failed = false;

  for (int a = 1; a < argc; ++a) {
    const char* path = argv[a];
    const char* name = base_name(path);

    // the game dir for this PE = the directory containing it (the console
    // host searches the game dir via VFS). The runtime resolver uses it to
    // load game-shipped DLLs on demand (images.cpp try_load_sibling).
    {
      const char* slash = strrchr(path, '/');
      if (slash) {
        const size_t dlen = (size_t)(slash - path);
        if (dlen < sizeof(g_game_dir)) {
          memcpy(g_game_dir, path, dlen);
          g_game_dir[dlen] = 0;
        } else {
          g_game_dir[0] = 0;
        }
      } else {
        g_game_dir[0] = '.';
        g_game_dir[1] = 0;
      }
    }
    trt_runtime_init(&be, g_game_dir);

    size_t size = 0;
    u8* file = read_file(path, &size);
    if (!file) {
      printf("LOAD FAILED %s: cannot read file\n", name);
      ++n_failed; any_failed = true;
      continue;
    }

    // ---- MZ + PE machine gate ----
    if (size < 0x40 || rd16b(file) != 0x5A4D) {
      printf("LOAD FAILED %s: no MZ\n", name);
      ++n_failed; any_failed = true; free(file); continue;
    }
    const u32 nt_off = rd32b(file + 0x3C);
    if ((u64)nt_off + 6 > size || rd32b(file + nt_off) != 0x00004550) {
      printf("LOAD FAILED %s: no PE\\0\\0 signature\n", name);
      ++n_failed; any_failed = true; free(file); continue;
    }
    const u16 machine = rd16b(file + nt_off + 4);
    if (machine != 0x8664) {
      printf("SKIP %s: x86 guest (M7 x86 engine not present)\n", name);
      ++n_skipped; free(file); continue;
    }

    // ---- STAGE 4: the file-level dry-run audit runs BEFORE the live map
    // and produces the full per-DLL report even when the live load dies.
    // Fresh guest-image registry per PE: a stale sibling from the previous
    // game dir must never satisfy this game's imports.
    trt_images_reset();
    stub_reset();  // per-PE stub counts (the report is per-DLL per-PE)
    printf("-- %s --\n", name);
    dry_run_pe(path, name, file, size);

    // ---- live map + seal: the exe goes through the runtime loader
    // (game-dir sibling search), exactly like test_libload / the console
    // host. Preferred base 0 = the image's own ImageBase.
    stub_reset();  // fresh per-PE stub observation for the live report
    trt_image* img = trt_load_pe(path, name, 0);
    if (!img) {
      printf("LOAD FAILED %s: map (see [trt] lines above for the named gap)\n", name);
      ++n_failed; any_failed = true; free(file); continue;
    }

    // ---- walk the bound image's import table: one line per DLL ----
    const trt_dos_header* dos = (const trt_dos_header*)img->base;
    const trt_nt_headers64* nt =
        (const trt_nt_headers64*)(img->base + dos->e_lfanew);
    const u64 size_of_image = img->size_of_image;
    const u64 imp_va = rd32b((const u8*)&nt->optional_header.directories[1].virtual_address);
    const u64 imp_sz = rd32b((const u8*)&nt->optional_header.directories[1].size);

    if (imp_va && imp_sz && imp_va + imp_sz <= size_of_image) {
      printf("  live (bound image import walk):\n");
      for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
           d += sizeof(trt_import_descriptor)) {
        const trt_import_descriptor* desc =
            (const trt_import_descriptor*)(img->base + imp_va + d);
        if (!desc->first_thunk && !desc->name) break;  // terminator
        if ((u64)desc->name >= size_of_image ||
            (u64)desc->first_thunk >= size_of_image)
          break;
        const char* dll = (const char*)(img->base + desc->name);
        char dll_name[96];
        snprintf(dll_name, sizeof(dll_name), "%s", dll);

        // imports: walk the INT (fall back to the IAT when the INT is absent)
        const u64 names_rva = desc->original_first_thunk
                                  ? desc->original_first_thunk
                                  : desc->first_thunk;
        int imports = 0, n_unresolved = 0;
        char unresolved[512];
        unresolved[0] = 0;
        for (u64 t = names_rva;; t += 8) {
          if (t + 8 > size_of_image) break;
          const u64 thunk = rd64b(img->base + t);
          if (thunk == 0) break;
          ++imports;
          // bound check: the loader overwrote every IAT slot with a
          // non-null address (real fn or fail-visible thunk)
          const u64 bound_slot =
              rd64b(img->base + desc->first_thunk + (t - names_rva));
          if (!bound_slot) {
            char one[160];
            if (thunk >> 63)
              snprintf(one, sizeof(one), "#%u", (unsigned)(thunk & 0xFFFF));
            else {
              const char* sym = (const char*)(img->base + thunk + 2);
              snprintf(one, sizeof(one), "%.*s", 120, sym);
            }
            append_csv(unresolved, sizeof(unresolved), one);
            ++n_unresolved;
          }
        }

        // stub count for this dll: observed from the loader's bind log
        // (trt_thunk_make logs "trt: STUB <dll>!<sym>" per STUB bind)
        const int stubs = stub_count_for(dll_name);

        char route[16];
        // the live loader resolves game-shipped DLLs from the game dir first
        // (same rule as the dry-run walk): a sibling file means GAMEDIR.
        char sib_live[1280];
        size_t sib_live_sz = 0;
        u8* sib = find_game_sibling(dll_name, sib_live, sizeof(sib_live),
                                    &sib_live_sz);
        if (sib) {
          snprintf(route, sizeof(route), "GAMEDIR");
          free(sib);
        } else {
          route_label(dll_name, route, sizeof(route));
        }
        if (strncmp(route, "SYSTEM(C)", 9) == 0) ++g_uncertain_bound;
        ++g_route_total;
        if (strncmp(route, "BUILTIN", 7) != 0 && strncmp(route, "STUB", 4) != 0 &&
            strncmp(route, "APISET", 6) != 0 && strncmp(route, "GAMEDIR", 7) != 0 &&
            strncmp(route, "SYSTEM(P)", 9) != 0 && strncmp(route, "SYSTEM(U)", 9) != 0)
          g_route_mismatch = 1;  // unproven console presence crept in
        printf("    %s route=%s imports=%d bound=%d stubs=%d unresolved=%s\n",
               dll_name, route, imports, imports - n_unresolved, stubs,
               unresolved[0] ? unresolved : "-");
      }
    } else {
      printf("  live: (no import directory)\n");
    }

    printf("LOAD OK %s\n", name);
    ++n_ok;
    free(file);
  }

  printf("\nCORPUS: %d ok, %d failed, %d skipped", n_ok, n_failed, n_skipped);
  if (g_uncertain_bound)
    printf("  [NOTE: %d DLLs bound via UNCERTAIN SYSTEM routes (C) - console presence unproven, the TV run names them]", g_uncertain_bound);
  printf("\n");
  printf("BOOT-DETERMINISM: %d DLL report lines, every import bound a PROVEN route: BUILTIN, APISET, GAMEDIR, SYSTEM(P/U) or fail-visible STUB - none depend on unproven console presence%s\n",
         g_route_total,
         g_route_mismatch ? "  [MISMATCH: an unproven route appeared]" : "");
  return any_failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_corpus <pe> [<pe> ...]\n");
    return 2;
  }
  return run_corpus(argc, argv);
}
