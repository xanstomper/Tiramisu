// tiramisu/src/trt/images.cpp
// Guest image registry, on-demand DLL loading, and the Win64 call helper.
#include "images.h"
#include "pe_exports.h"
#include "thunk.h"
#include "x86_32.h"
#include "dynamic_api.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if defined(_WIN32)
#  include <intrin.h>
#endif

// Escaped-guest-exception landing pad (Linux harness only): the innermost
// active host→guest call boundary. trt_call_guest_img arms it before calling
// in; the terminal EH path longjmps here when a guest C++ exception escapes
// the guest images (UnityPlayer native wrapper-catch emulation).
// Per-thread: each worker thread arms its own pad around its guest call.
thread_local eh_escape_state g_eh_escape = {0, {}};
// Per-thread host→guest call nesting depth (trt_call_guest_img/trt_call_guest).
// EH resume records are tagged with it; records retire when the call that
// created them returns (a record can only be consumed by walks running inside
// that same guest call).
thread_local unsigned g_guest_call_depth = 0;

namespace {

#define TRT_MAX_GUEST_IMAGES 256
#define TRT_HANDLE_TAG 0x4D000000u

struct guest_image {
  char name[64];      // "engine.dll"
  char path[512];     // host path it came from
  trt_image* img;
  void* handle;
  bool is_dll;
  bool entry_called;
  bool thread_calls_disabled;
};

guest_image g_images[TRT_MAX_GUEST_IMAGES];
int g_image_count = 0;
// the main guest image (the exe), recorded by trt_images_register_main:
// GetModuleHandle(NULL) / FindResource(NULL) must answer with the EXE, not
// whichever image happened to register first (a DLL can register before
// the exe while the exe's own imports bind).
trt_image* g_main_image = nullptr;
std::mutex g_img_mu;

const trt_backend* g_be = nullptr;
trt_resolver g_rs;
char g_search_dir[512] = "";
char g_content_root[512] = "";
char g_set_dll_dir[512] = "";
struct extra_dll_dir {
  char path[512];
  bool used;
};
extra_dll_dir g_extra_dll_dirs[32] = {};
char g_load_dir_stack[16][512] = {};
int g_load_dir_depth = 0;
int g_load_depth = 0;
// Load-only mode (corpus dry-run): map + bind + seal guest DLLs but do NOT
// run their TLS callbacks or DllMain — the corpus test proves the load
// path, it does not execute game code. Default 1 = full boot semantics.
int g_run_image_code = 1;

char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (lower_of(*a) != lower_of(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

bool path_ieq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = (*a == '\\') ? '/' : lower_of(*a);
    char cb = (*b == '\\') ? '/' : lower_of(*b);
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == *b;
}

const char* base_name(const char* path) {
  const char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

u8* read_whole_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return nullptr; }
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

bool has_dot_in_base(const char* s) {
  const char* b = base_name(s);
  return strchr(b, '.') != nullptr;
}

bool ends_with_dot(const char* s) {
  size_t n = strlen(s);
  return n > 0 && s[n - 1] == '.';
}

bool probe_file_exists(const char* path) {
  if (!path || !path[0]) return false;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

bool join_dir_and_name(const char* dir, const char* rel, char* out, size_t cap) {
  if (!dir || !dir[0] || !rel || !rel[0] || cap < 4) return false;
  size_t dl = strlen(dir);
  while (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) --dl;
  if (dl == 0) return false;
  char clean_rel[512];
  size_t rl = 0;
  for (; rel[rl] && rl + 1 < sizeof(clean_rel); ++rl) {
    clean_rel[rl] = (rel[rl] == '\\') ? '/' : rel[rl];
  }
  clean_rel[rl] = 0;
  const int w = snprintf(out, cap, "%.*s/%s", (int)dl, dir, clean_rel);
  return w > 0 && (size_t)w < cap;
}

bool probe_dll_in_dir(const char* dir, const char* rel_or_name,
                      char* out, size_t cap) {
  if (!join_dir_and_name(dir, rel_or_name, out, cap)) return false;
  if (probe_file_exists(out)) return true;
  if (!has_dot_in_base(rel_or_name) && !ends_with_dot(rel_or_name)) {
    char with_ext[1030];
    const int n = snprintf(with_ext, sizeof(with_ext), "%s.dll", out);
    if (n > 0 && (size_t)n < cap && probe_file_exists(with_ext)) {
      memcpy(out, with_ext, (size_t)n + 1);
      return true;
    }
  }
  return false;
}

struct LoadDirGuard {
  bool pushed = false;
  explicit LoadDirGuard(const char* host_path) {
    if (!host_path || g_load_dir_depth >= 16) return;
    const char* s1 = strrchr(host_path, '/');
    const char* s2 = strrchr(host_path, '\\');
    const char* sep = (s1 && s2) ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
    if (!sep || sep == host_path) return;
    size_t len = (size_t)(sep - host_path);
    if (len >= sizeof(g_load_dir_stack[0])) return;
    memcpy(g_load_dir_stack[g_load_dir_depth], host_path, len);
    g_load_dir_stack[g_load_dir_depth][len] = 0;
    ++g_load_dir_depth;
    pushed = true;
  }
  ~LoadDirGuard() {
    if (pushed && g_load_dir_depth > 0) --g_load_dir_depth;
  }
};

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
// runtime context
// ---------------------------------------------------------------------------

void trt_runtime_init(const trt_backend* be, const char* search_dir) {
  g_be = be;
  if (search_dir) snprintf(g_search_dir, sizeof(g_search_dir), "%s", search_dir);
  else g_search_dir[0] = 0;
  g_content_root[0] = 0;
  g_set_dll_dir[0] = 0;
  memset(g_extra_dll_dirs, 0, sizeof(g_extra_dll_dirs));
  g_load_dir_depth = 0;
  g_rs.user = nullptr;
  g_rs.resolve = &trt_resolve_full;
}

void trt_runtime_set_content_root(const char* host_dir) {
  const int n = snprintf(g_content_root, sizeof(g_content_root), "%s", host_dir ? host_dir : "");
  if (n < 0 || static_cast<size_t>(n) >= sizeof(g_content_root)) g_content_root[0] = 0;
}

void trt_runtime_set_dll_dir(const char* host_dir) {
  if (!host_dir || !host_dir[0]) {
    g_set_dll_dir[0] = 0;
    return;
  }
  snprintf(g_set_dll_dir, sizeof(g_set_dll_dir), "%s", host_dir);
}

int trt_runtime_add_dll_dir(const char* host_dir) {
  if (!host_dir || !host_dir[0]) return -1;
  for (int i = 0; i < 32; ++i) {
    if (!g_extra_dll_dirs[i].used) {
      snprintf(g_extra_dll_dirs[i].path, sizeof(g_extra_dll_dirs[i].path), "%s",
               host_dir);
      g_extra_dll_dirs[i].used = true;
      return i;
    }
  }
  return -1;
}

int trt_runtime_remove_dll_dir(int slot) {
  if (slot < 0 || slot >= 32 || !g_extra_dll_dirs[slot].used) return 0;
  g_extra_dll_dirs[slot].used = false;
  g_extra_dll_dirs[slot].path[0] = 0;
  return 1;
}

int trt_runtime_find_dll(const char* name_or_rel, char* out, size_t cap) {
  if (!name_or_rel || !name_or_rel[0] || !out || cap == 0) return -1;
  // 1. Directory of the DLL(s) currently being loaded (top down): handles
  //    plugins importing sibling plugins in Plugins/x86_64 or PySide6/.
  for (int i = g_load_dir_depth - 1; i >= 0; --i) {
    if (probe_dll_in_dir(g_load_dir_stack[i], name_or_rel, out, cap)) return 0;
  }
  // 2. Game / application root directory
  if (g_search_dir[0] && probe_dll_in_dir(g_search_dir, name_or_rel, out, cap)) {
    return 0;
  }
  // 3. SetDllDirectoryW path
  if (g_set_dll_dir[0] &&
      probe_dll_in_dir(g_set_dll_dir, name_or_rel, out, cap)) {
    return 0;
  }
  // 4. AddDllDirectory entries
  for (int i = 31; i >= 0; --i) {
    if (g_extra_dll_dirs[i].used &&
        probe_dll_in_dir(g_extra_dll_dirs[i].path, name_or_rel, out, cap)) {
      return 0;
    }
  }
  // The merged game root may supply DLLs outside the executable directory.
  if (g_content_root[0] && probe_dll_in_dir(g_content_root, name_or_rel, out, cap)) {
    return 0;
  }
  return -1;
}

const trt_backend* trt_runtime_backend(void) { return g_be; }

const char* trt_runtime_search_dir(void) { return g_search_dir; }

trt_resolver* trt_runtime_resolver(void) { return &g_rs; }

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

trt_image* trt_images_find(const char* name) {
  if (!name) return nullptr;
  const char* base = base_name(name);
  for (int i = 0; i < g_image_count; ++i)
    if (ieq(g_images[i].name, base)) return g_images[i].img;
  if (!has_dot_in_base(base) && !ends_with_dot(base)) {
    char with_ext[128];
    if (snprintf(with_ext, sizeof(with_ext), "%s.dll", base) < (int)sizeof(with_ext)) {
      for (int i = 0; i < g_image_count; ++i)
        if (ieq(g_images[i].name, with_ext)) return g_images[i].img;
    }
  }
  return nullptr;
}

trt_image* trt_images_by_handle(void* handle) {
  if (!handle) return nullptr;
  // STAGE 7: HMODULE == image base (guest code dereferences handles as a
  // PE header pointer); fall back to the legacy synthetic tag
  const u64 v = (u64)(uintptr_t)handle;
  for (int i = 0; i < g_image_count; ++i)
    if (g_images[i].img &&
        (u64)(uintptr_t)g_images[i].img->base == v) return g_images[i].img;
  if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
  const int idx = (int)(v & 0xFFFFu);
  if (idx >= g_image_count) return nullptr;
  return g_images[idx].img;
}

int trt_fault_is_guest(const void* addr) {
  if (!addr) return 0;
  const uintptr_t p = (uintptr_t)addr;
  std::lock_guard<std::mutex> lk(g_img_mu);
  for (int i = 0; i < g_image_count; ++i) {
    const trt_image* im = g_images[i].img;
    if (!im || !im->base || im->size_of_image == 0) continue;
    const uintptr_t b = (uintptr_t)im->base;
    if (p >= b && p < b + (uintptr_t)im->size_of_image) return 1;
  }
  return 0;
}

trt_image* trt_images_first(void) {
  // the MAIN image (the exe), not merely the first registered: a DLL can
  // register before the exe while the exe's own imports bind, so slot 0
  // is a DLL for those games. Fall back to slot 0 when no main was
  // registered (corpus loads map DLLs only).
  if (g_main_image) return g_main_image;
  return g_image_count > 0 ? g_images[0].img : nullptr;
}

void trt_images_register_main(trt_image* img, const char* guest_name) {
  if (!img) return;
  std::lock_guard<std::mutex> lk(g_img_mu);
  if (g_image_count >= TRT_MAX_GUEST_IMAGES) return;
  const char* base = base_name(guest_name ? guest_name : "main.exe");
  guest_image& g = g_images[g_image_count];
  memset(&g, 0, sizeof(g));
  snprintf(g.name, sizeof(g.name), "%s", base);
  snprintf(g.path, sizeof(g.path), "<mapped>");
  g.img = img;
  g_main_image = img;
  // STAGE 7: HMODULE == image base on Windows (guest code dereferences it:
  // UnityCrashHandler64's CRT does cmp $0x5a4d,(%rax) on the handle right
  // after GetModuleHandleW — a synthetic tag faults there)
  g.handle = (void*)(uintptr_t)img->base;
  ++g_image_count;
}

void trt_images_register_alias(trt_image* img, const char* alias_name) {
  if (!img || !alias_name || !alias_name[0]) return;
  std::lock_guard<std::mutex> lk(g_img_mu);
  if (g_image_count >= TRT_MAX_GUEST_IMAGES) return;
  const char* base = base_name(alias_name);
  for (int i = 0; i < g_image_count; ++i) {
    if (ieq(g_images[i].name, base)) return;
  }
  guest_image& g = g_images[g_image_count];
  memset(&g, 0, sizeof(g));
  snprintf(g.name, sizeof(g.name), "%s", base);
  snprintf(g.path, sizeof(g.path), "<alias>");
  g.img = img;
  g.handle = (void*)(uintptr_t)img->base;
  ++g_image_count;
}

// enumeration for the psapi/dbghelp bridges (the REAL guest module list).
// Locking matches the writer (trt_images_register_main holds g_img_mu):
// unlocked readers raced register/resize and returned garbage entries — the
// unwinder then "validated" metadata against a bogus image and crashed.
int trt_images_count(void) {
  std::lock_guard<std::mutex> lk(g_img_mu);
  return g_image_count;
}

trt_image* trt_images_nth(int idx) {
  std::lock_guard<std::mutex> lk(g_img_mu);
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].img : nullptr;
}

const char* trt_images_nth_name(int idx) {
  std::lock_guard<std::mutex> lk(g_img_mu);
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].name : nullptr;
}

const char* trt_images_nth_path(int idx) {
  std::lock_guard<std::mutex> lk(g_img_mu);
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].path : nullptr;
}

void* trt_images_handle(trt_image* img) {
  for (int i = 0; i < g_image_count; ++i)
    if (g_images[i].img == img) return g_images[i].handle;
  return nullptr;
}

const char* trt_images_name(void* handle) {
  if (handle) {
    // STAGE 7: HMODULE == image base; fall back to the legacy tag
    const u64 v = (u64)(uintptr_t)handle;
    for (int i = 0; i < g_image_count; ++i)
      if (g_images[i].img &&
          (u64)(uintptr_t)g_images[i].img->base == v) return g_images[i].name;
    if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
    const int idx = (int)(v & 0xFFFFu);
    if (idx >= g_image_count) return nullptr;
    return g_images[idx].name;
  }
  return nullptr;
}

// ---- synthetic system-module handles (STAGE 7, OrTMM) ---------------------
// LoadLibraryW("kernel32.dll") / GetModuleHandleW("api-ms-win-core-...") on
// modules the runtime implements but that exist as no host file. Each synthetic
// module carries a real minimal PE32+ header in readable memory so HMODULE
// returned to guest CRT/engine PE-header walkers (such as Godot's
// dinput8.dll import walk or Unity's MZ check) never faults.
#define TRT_MAX_SYNTH_MODULES 128
struct synth_module {
  alignas(16) u8 pe_hdr[512];
  char name[64];
};
synth_module g_synth[TRT_MAX_SYNTH_MODULES];
int g_synth_count = 0;

static void write_u16_le(u8* p, u16 v) {
  p[0] = (u8)(v & 0xFFu);
  p[1] = (u8)((v >> 8) & 0xFFu);
}

static void write_u32_le(u8* p, u32 v) {
  p[0] = (u8)(v & 0xFFu);
  p[1] = (u8)((v >> 8) & 0xFFu);
  p[2] = (u8)((v >> 16) & 0xFFu);
  p[3] = (u8)((v >> 24) & 0xFFu);
}

static void init_minimal_pe_header(u8* hdr) {
  memset(hdr, 0, 512);
  hdr[0] = 'M'; hdr[1] = 'Z';
  write_u32_le(hdr + 0x3c, 0x80u);       // e_lfanew -> 0x80
  hdr[0x80] = 'P'; hdr[0x81] = 'E';
  write_u16_le(hdr + 0x84, 0x8664u);     // IMAGE_FILE_MACHINE_AMD64
  write_u16_le(hdr + 0x86, 0);           // NumberOfSections = 0
  write_u16_le(hdr + 0x94, 0x00F0u);     // SizeOfOptionalHeader = 240
  write_u16_le(hdr + 0x96, 0x2022u);     // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DLL
  write_u16_le(hdr + 0x98, 0x020Bu);     // PE32+ Magic
  write_u32_le(hdr + 0xD0, 0x1000u);     // SizeOfImage
  write_u32_le(hdr + 0xD4, 0x0200u);     // SizeOfHeaders
  write_u32_le(hdr + 0x104, 16u);        // NumberOfRvaAndSizes
}

void* trt_images_synthetic_handle(const char* module_name) {
  if (!module_name || !module_name[0]) return nullptr;
  // stable: same name -> same handle for the process lifetime
  for (int i = 0; i < g_synth_count; ++i)
    if (ieq(g_synth[i].name, module_name)) return (void*)&g_synth[i].pe_hdr[0];
  if (g_synth_count >= TRT_MAX_SYNTH_MODULES) return nullptr;
  synth_module& s = g_synth[g_synth_count];
  memset(&s, 0, sizeof(s));
  init_minimal_pe_header(s.pe_hdr);
  snprintf(s.name, sizeof(s.name), "%s", module_name);
  ++g_synth_count;
  return (void*)&s.pe_hdr[0];
}

int trt_images_is_synthetic(void* handle) {
  if (!handle || g_synth_count <= 0) return 0;
  const uintptr_t p = (uintptr_t)handle;
  const uintptr_t b = (uintptr_t)&g_synth[0];
  const uintptr_t e = (uintptr_t)&g_synth[g_synth_count];
  if (p < b || p >= e) return 0;
  return ((p - b) % sizeof(synth_module)) == 0 ? 1 : 0;
}

const char* trt_images_synth_name(void* handle) {
  if (!trt_images_is_synthetic(handle)) return nullptr;
  const int idx = (int)(((uintptr_t)handle - (uintptr_t)&g_synth[0]) /
                        sizeof(synth_module));
  return g_synth[idx].name;
}

// ---------------------------------------------------------------------------
// resolver: guest images first, then the module table
// ---------------------------------------------------------------------------

static bool is_forced_builtin_module(const char* name) {
  const char* base = base_name(name);
  return ieq(base, "steam_api64.dll") ||
         ieq(base, "steam_api.dll");
}

// Try to load a DLL the game ships with us (from the game's directory).
static trt_image* try_load_sibling(const char* module) {
  if (!g_be || g_load_depth > 32) return nullptr;
  if (is_forced_builtin_module(module)) return nullptr;
  const char* base = base_name(module);
  char host[1024];
  if (trt_runtime_find_dll(base, host, sizeof(host)) != 0) return nullptr;
  char line[320];
  snprintf(line, sizeof(line), "trt: loading guest DLL %s", base_name(host));
  trt_core_log(line);
  return trt_load_pe(host, base_name(host), 1);
}

static void* resolve_guest_symbol(const char* module, const char* symbol,
                                  u16 ordinal, bool optional) {
  if (!module) return nullptr;
  // Respect the loaded image and normal sibling DLL search in either path.
  trt_image* img = trt_images_find(module);
  if (!img) img = try_load_sibling(module);
  if (img) {
    const auto* resolver = optional ? trt_runtime_export_resolver() : &g_rs;
    void* fn = trt_pe_find_export(g_be, resolver, img, symbol, ordinal);
    if (fn || optional) return fn;
  }
  return optional ? trt_resolve_export(module, symbol, ordinal)
                  : trt_resolve_module(nullptr, module, symbol, ordinal);
}

void* trt_resolve_full(void*, const char* module, const char* symbol, u16 ordinal) {
  return resolve_guest_symbol(module, symbol, ordinal, false);
}

const trt_resolver* trt_runtime_export_resolver(void) {
  static const trt_resolver resolver{nullptr,
      [](void*, const char* module, const char* symbol, u16 ordinal) -> void* {
    return resolve_guest_symbol(module, symbol, ordinal, true);
  }};
  return &resolver;
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

trt_image* trt_load_pe(const char* host_path, const char* guest_name,
                       int run_entry) {
  if (!g_be || !host_path) return nullptr;
  // STAGE 7 guard: re-entrant trt_load_pe (the exe's static CRT init or a
  // DllMain loads a DLL) would run tls callbacks again for the SAME image
  // object that already ran them — each pass burns a TEB TLS index and the
  // exe's index slot would end up pointing at the FIRST block while later
  // callbacks write later blocks. The load is the same file: skip the
  // callback/entry re-run, return the already-mapped image.
  const char* req_base = base_name(guest_name ? guest_name : host_path);
  char req_with_ext[128] = "";
  if (!has_dot_in_base(req_base) && !ends_with_dot(req_base)) {
    snprintf(req_with_ext, sizeof(req_with_ext), "%s.dll", req_base);
  }
  for (int i = 0; i < g_image_count; ++i)
    if (path_ieq(g_images[i].path, host_path) ||
        ieq(g_images[i].name, req_base) ||
        (req_with_ext[0] && ieq(g_images[i].name, req_with_ext))) {
      char line[320];
      snprintf(line, sizeof(line), "trt: load_pe %s -> already mapped (slot %d), re-entry guard",
               g_images[i].name, i);
      trt_core_log(line);
      return g_images[i].img;
    }
  size_t size = 0;
  u8* file = read_whole_file(host_path, &size);
  if (!file) {
    char line[320];
    snprintf(line, sizeof(line), "trt: cannot read %s", host_path);
    trt_core_log(line);
    return nullptr;
  }

  LoadDirGuard load_dir_guard(host_path);
  trt_image* img = nullptr;
  ++g_load_depth;
  const int rc = trt_map_pe(g_be, &g_rs, file, size, &img);
  --g_load_depth;
  free(file);
  if (rc != TRT_OK || !img) {
    char line[320];
    snprintf(line, sizeof(line), "trt: guest image %s failed to map (%s)",
             guest_name ? guest_name : host_path, trt_errstr(rc));
    trt_core_log(line);
    if (img) trt_unmap(g_be, img);
    return nullptr;
  }

  // register BEFORE the entry runs: the DLL may load another DLL that
  // imports back into this one
  const char* base = base_name(guest_name ? guest_name : host_path);
  int slot = -1;
  {
    std::lock_guard<std::mutex> lk(g_img_mu);
    if (g_image_count < TRT_MAX_GUEST_IMAGES) {
      slot = g_image_count;
      guest_image& g = g_images[slot];
      memset(&g, 0, sizeof(g));
      snprintf(g.name, sizeof(g.name), "%s", base);
      snprintf(g.path, sizeof(g.path), "%s", host_path);
      g.img = img;
      // STAGE 7: HMODULE == image base (see trt_images_register_main).
      // M7 Phase 1: a 32-bit HMODULE must be a 32-bit-safe value — the guest
      // VA base, not the 64-bit host pointer (a 32-bit slot cannot hold it).
      g.handle = img->is_pe32 ? (void*)(uintptr_t)img->guest_base
                              : (void*)(uintptr_t)img->base;
      g.is_dll = true;
      ++g_image_count;
    }
  }

  if (trt_seal_image(g_be, img) != TRT_OK) {
    char line[320];
    snprintf(line, sizeof(line), "trt: seal failed for %s", base);
    trt_core_log(line);
    return nullptr;
  }

  // PE32 code runs through the clean-room x86 interpreter. In load-only mode
  // (corpus dry-run) nothing executes, exactly like the PE64 path.
  if (img->is_pe32 && !g_run_image_code) {
    char line[320];
    snprintf(line, sizeof(line),
             "trt: %s is PE32: load-only (x86 engine armed, code not run)",
             base);
    trt_core_log(line);
  }
  // TLS callbacks, then the entry point (DllMain for a DLL). The legacy 32-bit
  // runner executes a private copy and copies the guest's data writes back
  // into the image, which is sealed RX at this point: unseal for that window
  // and re-seal afterwards, so no page is ever writable and executable.
  if (img->is_pe32 && g_run_image_code) trt_unseal_image(g_be, img);
  if (img->is_pe32 && g_run_image_code) {
    const u32 hinst = img->guest_base;
    for (int i = 0; i < img->tls_callback_count; ++i) {
      const u32 args[3] = {hinst, 1, 0};
      const u32 cb_va = img->guest_base + (u32)img->tls_callback_rvas[i];
      u32 eax = 0;
      const int rc = trt_x86_run_cdecl(img, cb_va, args, 200000, &eax);
      char line[288];
      snprintf(line, sizeof(line),
               "trt: PE32 TLS callback %d on %s rc=%d eax=%u", i, base, rc, eax);
      trt_core_log(line);
    }
    if (run_entry && img->entry_rva) {
      const u32 args[3] = {hinst, 1u, 0};  // DLL_PROCESS_ATTACH
      u32 eax = 0;
      const int rc = trt_x86_run_cdecl(
          img, img->guest_base + (u32)img->entry_rva, args, 1000000, &eax);
      char line[320];
      snprintf(line, sizeof(line),
               "trt: PE32 %s entry call rc=%d eax=%u", base, rc, eax);
      trt_core_log(line);
    }
  }
  if (!img->is_pe32 && g_run_image_code)
    for (int i = 0; i < img->tls_callback_count; ++i) {
      void* cb = img->base + img->tls_callback_rvas[i];
      trt_call_guest_img(cb, img, (u64)(uintptr_t)img->base, 1, 0, 0);
    }
  if (!img->is_pe32 && run_entry && g_run_image_code && img->entry_rva) {
    void* handle = trt_images_handle(img);
    char line[320];
    snprintf(line, sizeof(line),
             "trt: %s entry call handle=%p reason=1 (DLL_PROCESS_ATTACH)",
             base, handle);
    trt_core_log(line);
    const u64 rc_entry = trt_call_guest_img(img->base + img->entry_rva, img,
                                        (u64)(uintptr_t)handle, 1, 0, 0);
    snprintf(line, sizeof(line), "trt: %s entry returned 0x%llx", base,
             (unsigned long long)rc_entry);
    trt_core_log(line);
    if (rc_entry != 0 && slot >= 0) {
      std::lock_guard<std::mutex> lk(g_img_mu);
      if (slot < g_image_count && g_images[slot].img == img) {
        g_images[slot].entry_called = true;
      }
    }
  }
  return img;
}

int trt_images_disable_thread_calls(void* handle) {
  if (!handle) return 0;
  if (trt_images_is_synthetic(handle)) return 1;
  const u64 hv = (u64)(uintptr_t)handle;
  std::lock_guard<std::mutex> lk(g_img_mu);
  for (int i = 0; i < g_image_count; ++i) {
    if (g_images[i].handle == handle ||
        (g_images[i].img && (u64)(uintptr_t)g_images[i].img->base == hv)) {
      g_images[i].thread_calls_disabled = true;
      return 1;
    }
  }
  return 0;
}

void trt_images_notify_thread(int reason) {
  if (!g_run_image_code) return;
  if (reason != 2 && reason != 3) return;
  struct notify_entry {
    trt_image* img;
    void* handle;
    bool call_entry;
  };
  notify_entry snap[TRT_MAX_GUEST_IMAGES];
  int snap_count = 0;
  {
    std::lock_guard<std::mutex> lk(g_img_mu);
    snap_count = g_image_count;
    for (int i = 0; i < snap_count; ++i) {
      const guest_image& g = g_images[i];
      snap[i].img = g.img;
      snap[i].handle = g.handle;
      snap[i].call_entry = g.is_dll && g.entry_called &&
                           !g.thread_calls_disabled &&
                           g.img && g.img->entry_rva != 0;
    }
  }
  if (reason == 2) {
    for (int i = 0; i < snap_count; ++i) {
      trt_image* img = snap[i].img;
      if (!img) continue;
      if (img->is_pe32) continue;  // M7 Phase 1: no 32-bit code runs natively
      for (int t = 0; t < img->tls_callback_count; ++t) {
        trt_call_guest_img(img->base + img->tls_callback_rvas[t], img,
                           (u64)(uintptr_t)img->base, 2, 0, 0);
      }
      if (snap[i].call_entry) {
        trt_call_guest_img(img->base + img->entry_rva, img,
                           (u64)(uintptr_t)snap[i].handle, 2, 0, 0);
      }
    }
  } else {
    for (int i = snap_count - 1; i >= 0; --i) {
      trt_image* img = snap[i].img;
      if (!img) continue;
      if (img->is_pe32) continue;  // M7 Phase 1: no 32-bit code runs natively
      if (snap[i].call_entry) {
        trt_call_guest_img(img->base + img->entry_rva, img,
                           (u64)(uintptr_t)snap[i].handle, 3, 0, 0);
      }
      for (int t = 0; t < img->tls_callback_count; ++t) {
        trt_call_guest_img(img->base + img->tls_callback_rvas[t], img,
                           (u64)(uintptr_t)img->base, 3, 0, 0);
      }
    }
  }
}

// Load-only mode toggle: 0 = map/bind/seal without running TLS callbacks or
// DllMain (corpus dry-run), 1 (default) = full boot semantics.
void trt_set_image_code_run(int enable) { g_run_image_code = enable ? 1 : 0; }

void trt_images_reset(void) {
  // M7 Phase 1: the guest-VA reservations belong to the registry's images;
  // a reset orphans those images, so their guest ranges become free again.
  // (The trap registry is NOT reset: trap addresses in already-mapped IATs
  // stay valid for the process lifetime.)
  trt_pe32_guest_reset();
  std::lock_guard<std::mutex> lk(g_img_mu);
  g_image_count = 0;
  g_main_image = nullptr;
  g_set_dll_dir[0] = 0;
  memset(g_extra_dll_dirs, 0, sizeof(g_extra_dll_dirs));
  g_load_dir_depth = 0;
}

// ---------------------------------------------------------------------------
// calling guest code from host code
// ---------------------------------------------------------------------------

u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d) {
  ++g_guest_call_depth;
  const u64 r = trt_call_guest_img(fn, nullptr, a, b, c, d);
  --g_guest_call_depth;
  return r;
}

u64 trt_call_guest_img(void* fn, trt_image* img, u64 a, u64 b, u64 c, u64 d) {
  if (!fn) return 0;
#if defined(_WIN32)
  // the host IS Win64: the guest ABI is the host ABI, except gs:[0x58]
  // (ThreadLocalStoragePointer) must point to THIS OS thread's guest .tls
  // array with native static TLS pointers preserved in the reserved prefix.
  (void)img;
  void** guest_tls = trt_guest_tls_array();
  static thread_local void** native_tls = nullptr;
  // NT_TIB.Self points at the actual TEB, even while its TLS vector is
  // temporarily replaced. Use SEH cleanup since a guest can fault or exit
  // through the host's nonlocal exit trap.
  auto* const teb_tls_slot = reinterpret_cast<void** volatile*>(
      __readgsqword(0x30) + 0x58);
  void** const saved = *teb_tls_slot;
  const bool outer = native_tls == nullptr;
  if (outer) native_tls = saved;
  const bool swapped = guest_tls && saved != guest_tls;
  if (swapped) {
    trt_guest_tls_bind_host_slots(native_tls, guest_tls);
    *teb_tls_slot = guest_tls;
  }
  using guest_fn = u64 (*)(u64, u64, u64, u64);
  __try {
    return reinterpret_cast<guest_fn>(fn)(a, b, c, d);
  } __finally {
    if (swapped) *teb_tls_slot = saved;
    if (outer) native_tls = nullptr;
  }
#else
  // Linux test harness: the host is SysV, the guest is Win64. Marshal
  // rcx/rdx/r8/r9 and keep the stack correct for the Win64 callee:
  //   - Win64 requires rsp % 16 == 0 AT THE CALL INSTRUCTION, so the callee
  //     enters with rsp % 16 == 8. The old "subq $40" left rsp % 16 == 8 at
  //     the call (entry rsp % 16 == 0) — the guest booted "fine" until real
  //     MSVC code (dstorage.dll DllMain -> QueryPerformanceCounter) ran a
  //     movaps prologue and faulted. STAGE 6 run-depth found this; the
  //     load-only corpus never could (it never executed the entry).
  //   - 32 bytes of shadow space, exactly what Win64 guarantees.
  //   - the args are read through MEMORY constraints, because with a
  //    register constraint the compiler may allocate the base pointer to
  //    rcx and the first "mov 0(%[buf]), %rcx" would destroy the pointer
  //    before the next argument is read;
  //  - the args are read BEFORE %rsp is adjusted. The compiler computes
  //    the memory addresses relative to the current %rsp, so touching %rsp
  //    first makes every address stale (the guest then gets garbage).
  u64 args[4] = {a, b, c, d};
  u64 ret = 0;
  ++g_guest_call_depth;
  // Linux EH boundary: a C++ exception escaping the guest must not unwind
  // host ELF frames (the host personality cannot walk Win64 frames and a
  // forced-unwind would run guest destructors twice). setjmp BEFORE the call;
  // when an escaped exception longjmps here, the call reports a synthetic
  // error return instead of unwinding into the host. The pad must be armed
  // with a setjmp-local copy of the jmp_buf: setjmp may leave the buffer
  // incomplete until it returns (measured raccoin-ehfix14: arming with the
  // buffer itself and longjmping before setjmp filled it = SIGSEGV).
  jmp_buf eh_jb;
  const unsigned char eh_prev_active = g_eh_escape.active;
  jmp_buf eh_prev_jb;
  if (eh_prev_active) memcpy(eh_prev_jb, g_eh_escape.jb, sizeof(jmp_buf));
  if (setjmp(eh_jb) == 0) {
    g_eh_escape.active = 1;
    memcpy(g_eh_escape.jb, eh_jb, sizeof(jmp_buf));
  } else {
    // Escaped C++ exception (UnityPlayer native wrapper-catch emulation):
    // the guest caller sees a synthetic error return instead of a host
    // unwind. Every swallow is logged by the terminal path before jumping.
    g_eh_escape.active = eh_prev_active;
    if (eh_prev_active) memcpy(g_eh_escape.jb, eh_prev_jb, sizeof(jmp_buf));
    extern void trt_eh_retire_resume_records(unsigned depth);
    trt_eh_retire_resume_records(g_guest_call_depth);
    --g_guest_call_depth;
    return 0;
  }
  // STAGE 7: if the image has a guest TEB (TLS directory), point the guest
  // gs base at it for the duration of the call and restore the host gs
  // base after (the host is libc/glibc: its gs base is the thread pointer
  // and MUST come back). r11 is caller-saved in Win64, so it also carries
  // the saved host gs base across the guest call.
  //
  // STAGE 7 (third, onefile-child measured): that r11 carrier was WRONG.
  // r11 is VOLATILE in Win64: guest code legally clobbers it (OrTMM.exe
  // __chkstk even does `xor %r11,%r11` at 0x14000c8dd), so the restore
  // `wrgsbase %r11` wrote 0 back into gs. The guest kept running on gs
  // base 0 until the next TEB read, `mov %gs:0x10,%r11` in the SAME
  // __chkstk, faulted at address 0x10 inside the onefile child right
  // after python313.dll's DllMain returned (gdb-measured live: gs == 0 at
  // RIP 0x14000c8ec). The host gs base now travels in a stack slot at
  // rsp+40 relative to the call rsp: 8 bytes ABOVE the 32-byte shadow
  // space, memory that no Win64-conforming callee writes.
  //
  // STAGE 7 (second): the harness rsp is NOT guaranteed 16-aligned at this
  // asm. Win64 requires rsp % 16 == 0 AT the call instruction; a skewed
  // caller makes the ENTIRE guest stack skewed and the first movaps in any
  // bridge function faults (measured live under gdb: UnityCrashHandler64
  // CRT startup -> QueryPerformanceCounter -> movaps %xmm15,0xa0(%rsp)
  // SIGSEGV). Align rsp, save the original on the guest stack, restore
  // with popq %rsp (pop into rsp: the +8 is overridden by the popped value).
  (void)img;
  const u64 teb = (u64)(uintptr_t)trt_guest_teb_acquire(nullptr);
  asm volatile(
      "movq %[a0], %%rcx\n"
      "movq %[a1], %%rdx\n"
      "movq %[a2], %%r8\n"
      "movq %[a3], %%r9\n"
      "movq %%rsp, %%r10\n"
      "andq $-16, %%rsp\n"
      "subq $8, %%rsp\n"
      "pushq %%r10\n"             // original rsp, recovered with popq %%rsp
      "subq $32, %%rsp\n"
      "testq %[teb], %[teb]\n"
      "jnz 7f\n"
      "call *%[fn]\n"
      "jmp 8f\n"
      "7:\n"
      "rdgsbase %%r11\n"          // save the host thread pointer...
      "movq %%r11, 40(%%rsp)\n"   // ...into the slot above the shadow space
      "wrgsbase %[teb]\n"         // guest gs -> the TEB
      "call *%[fn]\n"
      "movq 40(%%rsp), %%r11\n"   // host gs back (stack, not a volatile reg)
      "wrgsbase %%r11\n"
      "8:\n"
      "addq $32, %%rsp\n"
      "popq %%rsp\n"              // exact original rsp back
      : "=a"(ret)
      : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
        [a3] "m"(args[3]), [fn] "r"(fn), [teb] "r"(teb)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  g_eh_escape.active = eh_prev_active;
  if (eh_prev_active) memcpy(g_eh_escape.jb, eh_prev_jb, sizeof(jmp_buf));
  // Retire this call's EH resume records (and any deeper-nested leftovers):
  // walks can only consume records inside the guest call that created them.
  extern void trt_eh_retire_resume_records(unsigned depth);
  trt_eh_retire_resume_records(g_guest_call_depth);
  --g_guest_call_depth;
  return ret;
#endif
}

} // extern "C"
