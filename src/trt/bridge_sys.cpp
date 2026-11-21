// tiramisu/src/trt/bridge_sys.cpp
// The system-information and file APIs every engine touches at startup.
//
// Games query the OS version, the CPU/memory layout, the temp path, the
// ANSI/Unicode code pages and the file system before they will start. A
// missing answer here is often a silent "game quits at launch".
#include "trt.h"
#include "thunk.h"
#include "iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#  define mkdir_portable(p) _mkdir(p)
#  define rmdir_portable(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define mkdir_portable(p) mkdir((p), 0755)
#  define rmdir_portable(p) rmdir(p)
#endif

#include <errno.h>

namespace {

size_t wcslen_(const u16* s) {
  size_t n = 0;
  while (s && s[n]) ++n;
  return n;
}

void utf16_to_utf8(const u16* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap; ++i) {
    if (in[i] < 0x80) out[o++] = (char)in[i];
    else if (in[i] < 0x800) {
      if (o + 2 >= cap) break;
      out[o++] = (char)(0xC0 | (in[i] >> 6));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    } else {
      if (o + 3 >= cap) break;
      out[o++] = (char)(0xE0 | (in[i] >> 12));
      out[o++] = (char)(0x80 | ((in[i] >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);

// ---- system information ----

// Host-settings hook (weak: absent when trt runs without the host layer —
// offline tests). File-scope: clang rejects attributed block-scope externs.
extern "C" void trt_settings_os(int*, int*, int*) __attribute__((weak));

// GetVersionExW(OSVERSIONINFOW*) — reports the USER-SET OS personality
// (dashboard settings), not a hardcoded constant: engines gate install/run
// paths on the version, and Win11-only builds check build >= 22000.
u32 TRT_STDCALL trt_GetVersionExW(void* info) {
  if (!info) return 0;
  // values come from the host settings layer (defaults: Win10 22H2)
  int mj = 10, mn = 0, bd = 19045;
  if (trt_settings_os) trt_settings_os(&mj, &mn, &bd);
  // LPOSTROSVERSIONINFOW: dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion,
  // dwBuildNumber, dwPlatformId, szCSDVersion[128]
  u32* p = (u32*)info;
  p[0] = 276;   // sizeof(OSVERSIONINFOW)
  p[1] = (u32)mj;
  p[2] = (u32)mn;
  p[3] = (u32)bd;
  p[4] = 2;     // VER_PLATFORM_WIN32_NT
  return 1;
}

u32 TRT_STDCALL trt_GetVersion(void) {
  int mj = 10, mn = 0, bd = 19045;
  if (trt_settings_os) trt_settings_os(&mj, &mn, &bd);
  // major<<16 | minor<<8 | build-hi-clip (Win10 = 0x0A000006 shape)
  return ((u32)mj << 16) | ((u32)mn << 8) | (u32)((bd >> 14) & 0xFFu);
}

// GetSystemInfo(SYSTEM_INFO*)
void TRT_STDCALL trt_GetSystemInfo(void* si) {
  if (!si) return;
  // Win64 SYSTEM_INFO layout (48 bytes):
  //   +0x00 u16 wProcessorArchitecture (9 = PROCESSOR_ARCHITECTURE_AMD64)
  //   +0x02 u16 wReserved (0)
  //   +0x04 u32 dwPageSize (4096)
  //   +0x08 u64 lpMinimumApplicationAddress (0x10000)
  //   +0x10 u64 lpMaximumApplicationAddress (0x00007FFFFFFEFFFF)
  //   +0x18 u64 dwActiveProcessorMask (0xFF = 8 logical processors)
  //   +0x20 u32 dwNumberOfProcessors (8)
  //   +0x24 u32 dwProcessorType (8664 = PROCESSOR_AMD_X8664)
  //   +0x28 u32 dwAllocationGranularity (0x10000 = 64 KB)
  //   +0x2C u16 wProcessorLevel (6)
  //   +0x2E u16 wProcessorRevision (0)
  memset(si, 0, 48);
  u8* b = (u8*)si;
  *(u16*)(b + 0x00) = 9;                        // PROCESSOR_ARCHITECTURE_AMD64
  *(u16*)(b + 0x02) = 0;                        // wReserved
  *(u32*)(b + 0x04) = 4096;                     // dwPageSize
  *(u64*)(b + 0x08) = 0x0000000000010000ULL;    // lpMinimumApplicationAddress
  *(u64*)(b + 0x10) = 0x00007FFFFFFEFFFFULL;    // lpMaximumApplicationAddress
  *(u64*)(b + 0x18) = 0x00000000000000FFULL;    // dwActiveProcessorMask
  *(u32*)(b + 0x20) = 8;                        // dwNumberOfProcessors
  *(u32*)(b + 0x24) = 8664;                     // PROCESSOR_AMD_X8664
  *(u32*)(b + 0x28) = 0x10000;                  // dwAllocationGranularity
  *(u16*)(b + 0x2C) = 6;                        // wProcessorLevel
  *(u16*)(b + 0x2E) = 0;                        // wProcessorRevision
}

// GetNativeSystemInfo(SYSTEM_INFO*) - same numbers, no WOW64 downgrade
void TRT_STDCALL trt_GetNativeSystemInfo(void* si) {
  trt_GetSystemInfo(si);
}

// GetSystemTimes(idle, kernel, user)
int TRT_STDCALL trt_GetSystemTimes(u64* idle, u64* kernel, u64* user) {
  const u64 now = (u64)clock() / (CLOCKS_PER_SEC / 1000);
  if (idle) *idle = now / 2;
  if (kernel) *kernel = now;
  if (user) *user = now / 2;
  return 1;
}

// .102 diagnostic: log every environment lookup (name + found/missing), so a
// missing var handed to a string function shows up in the log. Capped at 128
// calls; the counter is a plain static (boot is effectively single-threaded
// and the cap is a flood guard, not a correctness mechanism).
static void env_log_w(const char* tag, const char* name, int found, u32 len) {
  static unsigned env_calls = 0;
  if (env_calls >= 128) {
    if (env_calls == 128)
      trt_core_log("envtrace: env log cap reached (128), suppressing");
    ++env_calls;
    return;
  }
  ++env_calls;
  char l[384];
  if (found)
    snprintf(l, sizeof(l), "envtrace: %s(\"%s\") -> found len=%u", tag, name,
             len);
  else
    snprintf(l, sizeof(l), "envtrace: %s(\"%s\") -> MISSING", tag, name);
  trt_core_log(l);
}

u32 TRT_STDCALL trt_GetEnvironmentVariableW(const u16* name, u16* buf,
                                               u32 size) {
  if (!name) {
    env_log_w("GetEnvironmentVariableW", "<NULL>", 0, 0);
    return 0;
  }
  char n[256];
  utf16_to_utf8(name, n, sizeof(n));
  // the SetEnvironmentVariableW store first (cross-call consistency: a
  // value the guest WROTE must be readable back — PyInstaller's onefile
  // child flow depends on _PYI_APPLICATION_HOME_DIR), then host getenv
  const char* v = nullptr;
  {
    extern const char* trt_gap_env_get(const char*);
    v = trt_gap_env_get(n);
  }
  if (!v) v = getenv(n);
  if (!v) {
    env_log_w("GetEnvironmentVariableW", n, 0, 0);
    return 0;
  }
  const u32 need = (u32)strlen(v) + 1;
  env_log_w("GetEnvironmentVariableW", n, 1, need - 1);
  if (buf && size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)v[i];
    return need;
  }
  return need;
}

u32 TRT_STDCALL trt_ExpandEnvironmentStringsW(const u16* in, u16* out,
                                                u32 size) {
  if (!in) {
    trt_core_log("strtrace: ExpandEnvironmentStringsW(NULL) -> 0");
    return 0;
  }
  char src[1024], dst[2048];
  utf16_to_utf8(in, src, sizeof(src));
  const char* base = nullptr;
  {
    extern const char* trt_gap_env_get(const char*);
    base = trt_gap_env_get(src);
  }
  if (!base) base = getenv(src);
  snprintf(dst, sizeof(dst), "%s", base ? base : src);
  const u32 need = (u32)strlen(dst) + 1;
  if (out && size >= need) {
    for (u32 i = 0; i < need; ++i) out[i] = (u16)(u8)dst[i];
  }
  return need;
}

u32 TRT_STDCALL trt_GetACP(void) { return 1252; }
u32 TRT_STDCALL trt_GetOEMCP(void) { return 437; }
u32 TRT_STDCALL trt_GetUserDefaultLCID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetUserDefaultLangID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetSystemDefaultLCID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetSystemDefaultLangID(void) { return 0x0409; }

int TRT_STDCALL trt_GetComputerNameW(u16* buf, u32* size) {
  const char* host = getenv("COMPUTERNAME");
  if (!host) host = "TIRAMISU";
  const u32 need = (u32)strlen(host) + 1;
  if (buf && size && *size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)host[i];
    *size = need;
    return 1;
  }
  if (size) *size = need;
  return 0;
}

// ---- wide/ANSI conversion ----

// MultiByteToWideChar(codepage, flags, src, srclen, dst, dstlen)
int TRT_STDCALL trt_MultiByteToWideChar(u32 codepage, u32 flags,
                                          const char* src, int srclen,
                                          u16* dst, int dstlen) {
  (void)codepage; (void)flags;
  if (!src) return 0;
  const int len = srclen < 0 ? (int)strlen(src) : srclen;
  if (!dst || dstlen <= 0) return (srclen < 0) ? (len + 1) : len;
  int n = 0;
  for (int i = 0; i < len; ++i) {
    const u8 c = (u8)src[i];
    u16 w = c;
    if (c >= 0x80) {  // latin-1 -> utf-16 for the common range
      w = (u16)(0x00C0 | (c >> 6));
      if (n + 1 >= dstlen) break;
      dst[n++] = w;
      w = (u16)(0x0080 | (c & 0x3F));
    }
    if (n + 1 > dstlen) break;
    dst[n++] = w;
  }
  if (n < dstlen) dst[n] = 0;
  return n;
}

// WideCharToMultiByte(codepage, flags, src, srclen, dst, dstlen, def, used)
int TRT_STDCALL trt_WideCharToMultiByte(u32 codepage, u32 flags,
                                          const u16* src, int srclen, char* dst,
                                          int dstlen, const char* def,
                                          void* used) {
  (void)codepage; (void)flags; (void)def; (void)used;
  if (!src) return 0;
  const int len = srclen < 0 ? (int)wcslen_(src) : srclen;
  if (!dst || dstlen <= 0) return len;
  int n = 0;
  for (int i = 0; i < len && n < dstlen; ++i) {
    const u16 c = src[i];
    if (c < 0x80) {
      dst[n++] = (char)c;
    } else if (c < 0x800) {
      if (n + 1 >= dstlen) break;
      dst[n++] = (char)(0xC0 | (c >> 6));
      dst[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      if (n + 2 >= dstlen) break;
      dst[n++] = (char)(0xE0 | (c >> 12));
      dst[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      dst[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  return n;
}

// ---- files and directories ----

u32 TRT_STDCALL trt_GetTempPathW(u32 size, u16* buf) {
  // GUEST-style path under a mounted drive. Returning the HOST path ("/tmp/")
  // used to poison every derived path: PyInstaller built "/tmp\_MEIxxxx"
  // which the resolver treated as a RELATIVE guest path (host-absolute paths
  // are not guest paths), so the temp dir never materialised and the
  // candidate-name probe looped for 2.5M opens (watchdog exit 43). The
  // environment block already advertises TEMP=C:\Temp — match it.
  const char* t = "C:\\Temp\\";
  const u32 need = (u32)strlen(t) + 1;
  if (buf && size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)t[i];
    return need;
  }
  return need;
}

u32 TRT_STDCALL trt_GetTempFileNameW(const u16* dir, const u16* prefix,
                                       u32 unique, u16* buf) {
  char d[256], p[64];
  utf16_to_utf8(dir, d, sizeof(d));
  utf16_to_utf8(prefix, p, sizeof(p));
  char out[512];
  snprintf(out, sizeof(out), "%s%s%u.tmp", d, p, unique);
  const u32 need = (u32)strlen(out) + 1;
  if (buf) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)out[i];
  }
  return need;
}

u32 TRT_STDCALL trt_GetWindowsDirectoryW(u16* buf, u32 size) {
  const char* w = "C:\\Windows";
  const u32 need = (u32)strlen(w) + 1;
  if (buf && size >= need)
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)w[i];
  return need;
}

u32 TRT_STDCALL trt_GetFileAttributesW(const u16* path) {
  int is_dir = 0;
  // disc image: answer from the ISO records, never extract
  if (trt_vfs_is_image_utf16(path)) {
    char rel[1024] = "";
    struct trt_iso* iso = trt_vfs_image_for_guest_utf16(path, rel, sizeof(rel));
    if (!iso || trt_iso_stat(iso, rel, &is_dir, nullptr) != 0)
      return 0xFFFFFFFFu;
    return is_dir ? 0x10u : 0x80u;
  }
  char p[1024];
  if (trt_vfs_resolve_utf16_rw(path, 0, p, sizeof(p)) != 0)
    utf16_to_utf8(path, p, sizeof(p));
  if (trt_vfs_stat(p, &is_dir, nullptr) != 0)
    return 0xFFFFFFFFu;  // INVALID_FILE_ATTRIBUTES
  return is_dir ? 0x10u /*FILE_ATTRIBUTE_DIRECTORY*/ : 0x80u /*FILE_ATTRIBUTE_NORMAL*/;
}

int TRT_STDCALL trt_SetFileAttributesW(const u16* path, u32 attrs) {
  (void)path; (void)attrs;
  return 1;
}

extern void TRT_STDCALL trt_SetLastError(u32 err);

int TRT_STDCALL trt_DeleteFileW(const u16* path) {
  if (trt_vfs_is_image_utf16(path)) {
    trt_SetLastError(19);  // ERROR_WRITE_PROTECT: a disc is read-only
    return 0;
  }
  u32 werr = 0;
  if (trt_vfs_delete_file_utf16(path, &werr) == 0) return 1;
  (void)werr;
  return 0;
}

int TRT_STDCALL trt_CreateDirectoryW(const u16* path, void* sa) {
  (void)sa;
  char p[1024];
  if (trt_vfs_resolve_utf16_rw(path, 1, p, sizeof(p)) != 0) {
    utf16_to_utf8(path, p, sizeof(p));  // fail-visible: try the raw name
  }
  if (trt_vfs_mkdir_p(p) == 0) {
    trt_vfs_mark_z_deleted_utf16(path, 0, 1);
    return 1;
  }
  return 0;
}

int TRT_STDCALL trt_RemoveDirectoryW(const u16* path) {
  if (trt_vfs_is_image_utf16(path)) {
    trt_SetLastError(19);  // ERROR_WRITE_PROTECT
    return 0;
  }
  u32 werr = 0;
  if (trt_vfs_remove_dir_utf16(path, &werr) == 0) return 1;
  (void)werr;
  return 0;
}

int TRT_STDCALL trt_CopyFileW(const u16* src, const u16* dst, bool fail) {
  char s[1024], d[1024], d_read[1024];
  if (trt_vfs_resolve_utf16_rw(src, 0, s, sizeof(s)) != 0)
    utf16_to_utf8(src, s, sizeof(s));
  if (trt_vfs_resolve_utf16_rw(dst, 1, d, sizeof(d)) != 0)
    utf16_to_utf8(dst, d, sizeof(d));
  if (trt_vfs_resolve_utf16_rw(dst, 0, d_read, sizeof(d_read)) != 0)
    snprintf(d_read, sizeof(d_read), "%s", d);
  if (fail && trt_vfs_stat(d_read, nullptr, nullptr) == 0) return 0;
  trt_vfs_ensure_parents(d);
  FILE* in = fopen(s, "rb");
  if (!in) return 0;
  FILE* out = fopen(d, "wb");
  if (!out) { fclose(in); return 0; }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  trt_vfs_mark_z_deleted_utf16(dst, 0, 0);
  return 1;
}

int trt_bridge_register_system(void) {
  static const trt_named_fn k_fns[] = {
      {"GetVersionExW", (void*)&trt_GetVersionExW},
      {"GetVersion", (void*)&trt_GetVersion},
      {"GetSystemInfo", (void*)&trt_GetSystemInfo},
      {"GetNativeSystemInfo", (void*)&trt_GetNativeSystemInfo},
      {"GetSystemTimes", (void*)&trt_GetSystemTimes},
      {"GetEnvironmentVariableW", (void*)&trt_GetEnvironmentVariableW},
      {"ExpandEnvironmentStringsW", (void*)&trt_ExpandEnvironmentStringsW},
      {"GetACP", (void*)&trt_GetACP},
      {"GetOEMCP", (void*)&trt_GetOEMCP},
      {"GetUserDefaultLCID", (void*)&trt_GetUserDefaultLCID},
      {"GetUserDefaultLangID", (void*)&trt_GetUserDefaultLangID},
      {"GetSystemDefaultLCID", (void*)&trt_GetSystemDefaultLCID},
      {"GetSystemDefaultLangID", (void*)&trt_GetSystemDefaultLangID},
      {"GetComputerNameW", (void*)&trt_GetComputerNameW},
      {"MultiByteToWideChar", (void*)&trt_MultiByteToWideChar},
      {"WideCharToMultiByte", (void*)&trt_WideCharToMultiByte},
      {"GetTempPathW", (void*)&trt_GetTempPathW},
      // Win8+ sibling the UCRT itself imports (kernel32!GetTempPath2W);
      // blanket-0 bind made UCRT's temp-name loop spin -> boot backstop.
      {"GetTempPath2W", (void*)&trt_GetTempPathW},
      {"GetTempFileNameW", (void*)&trt_GetTempFileNameW},
      {"GetWindowsDirectoryW", (void*)&trt_GetWindowsDirectoryW},
      {"GetFileAttributesW", (void*)&trt_GetFileAttributesW},
      {"SetFileAttributesW", (void*)&trt_SetFileAttributesW},
      {"DeleteFileW", (void*)&trt_DeleteFileW},
      {"CreateDirectoryW", (void*)&trt_CreateDirectoryW},
      {"RemoveDirectoryW", (void*)&trt_RemoveDirectoryW},
      {"CopyFileW", (void*)&trt_CopyFileW},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
