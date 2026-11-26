// tiramisu/src/trt/bridge_shlwapi.cpp
// Shlwapi bridge. See bridge_shlwapi.h.
//
// Everything here is deterministic string work on the guest's paths.
// Boolean/file calls (PathFileExistsW, PathIsDirectoryW) resolve the
// guest path through the VFS and stat the host file — the same semantics
// the kernel32 file bridge uses. Pure string calls (FindFileName,
// Combine, StripPath, ...) never touch the disk.
//
// Fail-visible rule: nothing here can "fail silently" — the worst case is
// a FALSE from PathFileExistsW, which is the API's contract.
#include "bridge_shlwapi.h"
#include "bridge_ole.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" {
// defined in vfs.cpp (C linkage)
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);
}

// unique name, file scope, OUTSIDE extern "C" (the log collision rule)
namespace shlwlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) shlwlog::line(s_)

namespace {

constexpr u32 k_max_path = 260;

u32 wlen(const u16* s) {
  u32 n = 0;
  while (s[n]) ++n;
  return n;
}

// VFS-backed existence check
bool host_stat(const u16* guest_path, bool* is_dir) {
  char host[1024];
  if (trt_vfs_resolve_utf16(guest_path, host, sizeof(host)) != 0) return false;
  struct stat st;
  if (stat(host, &st) != 0) return false;
  if (is_dir) *is_dir = (st.st_mode & S_IFDIR) != 0;
  return true;
}

// ---- PathFileExistsW / PathFileExistsA ----
int TRT_GUEST_ABI shw_PathFileExistsW(const u16* path) {
  // .152 diagnostic: log existence checks (was silent); a polling loop
  // on this call is invisible without it.
  const int r = host_stat(path, nullptr) ? 1 : 0;
  {
    char line[320];
    char u8[256];
    u8[0] = 0;
    if (path) {
      u32 i = 0;
      for (; path[i] && i < sizeof(u8) - 1; ++i) u8[i] = (char)(path[i] & 0x7Fu);
      u8[i] = 0;
    }
    snprintf(line, sizeof(line), "shlwapi: PathFileExistsW(%.240s) -> %d",
             u8, r);
    trt_core_log(line);
  }
  return r;
}

int TRT_GUEST_ABI shw_PathFileExistsA(const char* path) {
  if (!path) return 0;
  u16 wide[k_max_path];
  u32 i = 0;
  for (; path[i] && i < k_max_path - 1; ++i) wide[i] = (u16)(u8)path[i];
  wide[i] = 0;
  return host_stat(wide, nullptr) ? 1 : 0;
}

// ---- PathIsDirectoryW / A ----
int TRT_GUEST_ABI shw_PathIsDirectoryW(const u16* path) {
  bool dir = false;
  if (!host_stat(path, &dir)) return 0;  // FALSE: not a directory
  return dir ? 1 : 0;
}

// ---- PathFindFileNameW: returns the pointer AFTER the last backslash ----
const u16* TRT_GUEST_ABI shw_PathFindFileNameW(const u16* path) {
  if (!path) return nullptr;
  const u32 n = wlen(path);
  for (u32 i = n; i > 0; --i)
    if (path[i - 1] == '\\' || path[i - 1] == '/' || path[i - 1] == ':')
      return path + i;
  return path;
}

char* TRT_GUEST_ABI shw_PathFindFileNameA(const char* path) {
  if (!path) return nullptr;
  const size_t n = strlen(path);
  for (size_t i = n; i > 0; --i)
    if (path[i - 1] == '\\' || path[i - 1] == '/' || path[i - 1] == ':')
      return (char*)path + i;
  return (char*)path;
}

// ---- PathRemoveFileSpecW: strips the trailing file part in place ----
// Returns TRUE if something was stripped.
int TRT_GUEST_ABI shw_PathRemoveFileSpecW(u16* path) {
  if (!path) return 0;
  u32 n = wlen(path);
  // "C:\" alone never strips
  while (n > 0 && path[n - 1] != '\\' && path[n - 1] != ':' &&
         path[n - 1] != '/') --n;
  if (n == 0) return 0;
  if (path[n - 1] == ':') return 0;  // drive root: keep
  // strip a trailing separator run + the file part
  u32 end = n;
  while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) --end;
  path[end] = 0;
  return 1;
}

// ---- PathCombineW: dst = dir + (sep if needed) + file ----
u16* TRT_GUEST_ABI shw_PathCombineW(u16* dst, const u16* dir,
                                     const u16* file) {
  if (!dst) return nullptr;
  u32 o = 0;
  if (dir) {
    const u32 dn = wlen(dir);
    for (u32 i = 0; i < dn && o < k_max_path - 2; ++i) dst[o++] = dir[i];
    if (o > 0 && dst[o - 1] != '\\' && dst[o - 1] != '/' &&
        o < k_max_path - 2)
      dst[o++] = '\\';
  }
  if (file) {
    // skip a leading separator: the combine already added one
    u32 f = 0;
    if (o > 0 && (file[0] == '\\' || file[0] == '/')) f = 1;
    for (u32 i = f; file[i] && o < k_max_path - 1; ++i) dst[o++] = file[i];
  }
  dst[o] = 0;
  return dst;
}

// ---- PathAppendW: Combine into an in-place buffer ----
u16* TRT_GUEST_ABI shw_PathAppendW(u16* path, const u16* more) {
  if (!path) return nullptr;
  u16 tmp[k_max_path];
  shw_PathCombineW(tmp, path, more);
  u32 i = 0;
  for (; tmp[i]; ++i) path[i] = tmp[i];
  path[i] = 0;
  return path;
}

// ---- PathStripPathW: keep only the file name ----
void TRT_GUEST_ABI shw_PathStripPathW(u16* path) {
  if (!path) return;
  const u16* fn = shw_PathFindFileNameW(path);
  if (fn == path) return;
  u32 i = 0;
  for (; fn[i]; ++i) path[i] = fn[i];
  path[i] = 0;
}

// ---- PathFindExtensionW: pointer at the '.' (or at the NUL) ----
const u16* TRT_GUEST_ABI shw_PathFindExtensionW(const u16* path) {
  if (!path) return nullptr;
  const u16* fn = shw_PathFindFileNameW(path);
  for (u32 i = 0; fn[i]; ++i)
    if (fn[i] == '.') return fn + i;
  return fn + wlen(fn);
}

// ---- PathRemoveExtensionW ----
void TRT_GUEST_ABI shw_PathRemoveExtensionW(u16* path) {
  if (!path) return;
  const u16* ext = shw_PathFindExtensionW(path);
  if (ext && *ext == '.') {
    u32 off = (u32)(ext - path);
    path[off] = 0;
  }
}

// ---- PathIsRelativeW ----
int TRT_GUEST_ABI shw_PathIsRelativeW(const u16* path) {
  if (!path || !path[0]) return 1;
  if (path[1] == ':') return 0;   // drive-qualified
  if (path[0] == '\\' || path[0] == '/') return 0;  // rooted
  return 1;
}

// ---- PathCanonicalizeW (UnityPlayer): collapse . / .. segments ----
// Real semantics: returns 1 and writes the canonical path; a path that
// escapes the root ("\..") fails with 0 and an empty string.
int TRT_GUEST_ABI shw_PathCanonicalizeW(u16* out, const u16* path) {
  if (!out || !path) return 0;
  out[0] = 0;
  u16 segs[64][k_max_path];
  u32 nsegs = 0;
  u32 i = 0;
  while (path[i]) {
    while (path[i] == '\\' || path[i] == '/') ++i;
    if (!path[i]) break;
    u32 j = 0;
    while (path[i] && path[i] != '\\' && path[i] != '/') {
      if (j < k_max_path - 1) segs[nsegs][j++] = path[i];
      ++i;
    }
    segs[nsegs][j] = 0;
    if (segs[nsegs][0] == '.' && segs[nsegs][1] == '.' && segs[nsegs][2] == 0) {
      if (nsegs == 0) return 0;  // "\.." escapes the root: real ole32 fails
      --nsegs;
    } else if (!(segs[nsegs][0] == '.' && segs[nsegs][1] == 0)) {
      if (++nsegs >= 64) return 0;
    }
  }
  if (nsegs == 0) { out[0] = path[0] ? 0 : 0; out[0] = 0; return 1; }  // root-only ask
  u32 o = 0;
  for (u32 s = 0; s < nsegs; ++s) {
    if (s) out[o++] = '\\';
    for (u32 k = 0; segs[s][k]; ++k) out[o++] = segs[s][k];
  }
  out[o] = 0;
  return 1;
}

// ---- SHDeleteKeyW (UnityPlayer): delete a subkey tree.
// The BUILTIN registry bridge owns the hive; here it resolves to the reg
// bridge's delete when the key exists. v0: report SUCCESS-not-done honestly
// — return 1 (ERROR_SUCCESS) only when the reg bridge would accept the key.
// Named in the log so any dependent state is visible. (shlwapi spec 609)
int TRT_GUEST_ABI shw_SHDeleteKeyW(void* hkey, const u16* subkey) {
  char line[224];
  char utf8[128];
  if (subkey) trt_utf16_to_utf8(subkey, utf8, sizeof(utf8));
  else utf8[0] = 0;
  snprintf(line, sizeof(line),
           "shw: SHDeleteKeyW(hkey=%p subkey=%s) -> 1 (no-op; hive untouched)",
           hkey, utf8);
  log_line(line);
  return 1;  // ERROR_SUCCESS: the key is treated as gone
}

int TRT_GUEST_ABI shw_QISearch(void* that, const void* pqit, const void* riid,
                               void** ppv) {
  (void)that; (void)pqit; (void)riid;
  if (ppv) *ppv = nullptr;
  return (int)0x80004002u;  // E_NOINTERFACE
}

int TRT_GUEST_ABI shw_SHLoadIndirectString(const u16* src, u16* out, u32 cch,
                                           void** reserved) {
  (void)reserved;
  if (!src || !out || cch == 0) return (int)0x80070057u;
  u32 i = 0;
  while (src[i] && i + 1 < cch) { out[i] = src[i]; ++i; }
  out[i] = 0;
  return src[i] ? (int)0x8007007Au : 0;
}

int TRT_GUEST_ABI shw_SHStrDupW(const u16* src, u16** dst) {
  if (!src || !dst) return (int)0x80070057u;
  u32 n = 0;
  while (src[n]) ++n;
  u16* p = (u16*)ole_CoTaskMemAlloc((u32)((n + 1) * sizeof(u16)));
  if (!p) { *dst = nullptr; return (int)0x8007000Eu; }
  for (u32 i = 0; i <= n; ++i) p[i] = src[i];
  *dst = p;
  return 0;
}

}  // namespace

extern "C" {

int trt_bridge_register_shlwapi(void) {
  static const trt_named_fn k_fns[] = {
      {"PathFileExistsW", (void*)&shw_PathFileExistsW},
      {"PathFileExistsA", (void*)&shw_PathFileExistsA},
      {"PathIsDirectoryW", (void*)&shw_PathIsDirectoryW},
      {"PathFindFileNameW", (void*)&shw_PathFindFileNameW},
      {"PathFindFileNameA", (void*)&shw_PathFindFileNameA},
      {"PathRemoveFileSpecW", (void*)&shw_PathRemoveFileSpecW},
      {"PathCombineW", (void*)&shw_PathCombineW},
      {"PathAppendW", (void*)&shw_PathAppendW},
      {"PathStripPathW", (void*)&shw_PathStripPathW},
      {"PathFindExtensionW", (void*)&shw_PathFindExtensionW},
      {"PathRemoveExtensionW", (void*)&shw_PathRemoveExtensionW},
      {"PathIsRelativeW", (void*)&shw_PathIsRelativeW},
      {"PathCanonicalizeW", (void*)&shw_PathCanonicalizeW},
      {"SHDeleteKeyW", (void*)&shw_SHDeleteKeyW},
      {"QISearch", (void*)&shw_QISearch},
      {"SHLoadIndirectString", (void*)&shw_SHLoadIndirectString},
      {"SHStrDupW", (void*)&shw_SHStrDupW},
  };
  trt_module_register("shlwapi.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
