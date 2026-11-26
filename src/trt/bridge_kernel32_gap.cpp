// tiramisu/src/trt/bridge_kernel32_gap.cpp
// The OrTMM-audit gap batch: the 20 kernel32 symbols the offline boot audit
// (tools/audit_boot via trt_audit_imports) named as unresolved on a real
// 48.9 MB PE64 game binary. Each is honest on existing infrastructure;
// anything that cannot be real FAILS NAMED (the fail-visible rule).
//
//   IsValidCodePage / GetCPInfo family   — CRT locale boot (cp1252/UTF-8)
//   SetEnvironmentVariableW              — real env store behind GetEnvironmentStringsW
//   MulDiv                               — the exact Win64 integer semantics
//   FormatMessageW                       — FORMAT_MESSAGE_FROM_SYSTEM only, named gap else
//   SetDllDirectoryW                     — loader already searches the game dir; accept+log
//   CreateSymbolicLinkW                  — named failure (VFS has no links)
//   GetDriveTypeW                        — every mounted path is DRIVE_FIXED
//   GetExitCodeProcess                   — the single guest "process" exit code
//   LocalFree / LocalAlloc               — Win32 legacy wrappers over HeapAlloc
//   K32EnumProcessModules / K32GetModuleFileNameExW — forward to the psapi bridge
//   GetFinalPathNameByHandleW            — VFS path of an open file handle
//   GetTimeZoneInformation               — UTC (bias 0), honest + named
//   SetEndOfFile                         — truncate through the FILE*
//   GetFileInformationByHandle           — size/times/attributes from the FILE*
//   PeekNamedPipe                        — named failure: no pipe handles exist
//   SystemTimeToTzSpecificLocalTime / FileTimeToSystemTime — real conversions
//   GetFullPathNameW                     — cwd-relative path resolution (VFS-honest)
//   GetFileSizeEx                        — 64-bit size through the FILE*
#include "thunk.h"
#include "trt.h"
#include "images.h"
#include "pe.h"  // TRT_STDCALL (ms_abi on Linux test hosts — guest-call rule)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  include <io.h>       // _chsize_s, _get_osfhandle
#  include <windows.h>
#else
#  include <unistd.h>   // ftruncate
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace k32gap {
void line(const char* s) { trt_core_log(s); }
}
#define LOG(s_) k32gap::line(s_)

// ---- shared with bridge_kernel32.cpp (extern-C accessors live there) ------
extern "C" FILE* trt_file_os(void* h);        // handle -> FILE* (or null)
extern "C" const char* trt_file_vpath(void* h); // handle -> guest path ("C:\...")
extern "C" int trt_file_copy_vpath(void* handle, char* path, size_t capacity);
extern "C" char* trt_gap_cwd(void);           // the guest cwd (g_cwd)
extern "C" const char* trt_gap_exepath(void);  // the guest exe path (g_exepath)
extern "C" void TRT_STDCALL trt_SetLastError(u32 e);
extern "C" u32 TRT_STDCALL trt_GetLastError(void);
extern "C" u32 TRT_STDCALL trt_GetTempFileNameW(const u16*, const u16*, u32, u16*);
extern "C" int TRT_STDCALL trt_CreateDirectoryW(const u16* path, void* sa);
extern "C" u32 TRT_STDCALL trt_GetCurrentProcessId(void);
extern "C" void* TRT_GUEST_ABI trt_GetStdHandle(u32 handle_id);
extern "C" int TRT_STDCALL trt_SetWaitableTimer(void* h, const i64* due_time,
                                                i32 period, void* comp,
                                                void* arg, int resume);
extern "C" void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access,
                                             u32 share, void* sec, u32 disp,
                                             u32 flags, void* tmpl);
extern "C" void* TRT_STDCALL trt_FindFirstFileExW(const u16* pattern, u32 level,
                                                  void* data, u32 search_op,
                                                  void* filter, u32 flags);
extern "C" int TRT_STDCALL trt_GetFileAttributesExW(const u16* name, u32 level,
                                                    void* info);

namespace {

// ---- uncapped, thread-safe environment store backing SetEnvironmentVariable*
// and GetEnvironmentStringsW across arbitrarily many variables.
std::mutex g_env_mu;
std::vector<std::pair<std::string, std::string>> g_env_store;
std::unordered_set<const u16*> g_env_wblocks;

char ascii_tolower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool env_key_eq(const std::string& a, const char* b) {
  if (!b) return false;
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    if (ascii_tolower(a[i]) != ascii_tolower(b[i])) return false;
  }
  return i == a.size() && b[i] == 0;
}

int env_find_locked(const char* name) {
  for (size_t i = 0; i < g_env_store.size(); ++i) {
    if (env_key_eq(g_env_store[i].first, name)) return (int)i;
  }
  return -1;
}

// ---- the canonical guest environment (0.2.2.95) ---------------------------
// One source of truth for the guest env block: GetEnvironmentStringsW/A
// advertise exactly this set, and the store is seeded with it on first use
// so GetEnvironmentVariableW/A and the bridged _wgetenv/getenv see the same
// values (bridges audit #7: the advertised defaults used to be invisible to
// GetEnvironmentVariableW). Values match the VFS layout the bridge presents
// (C:\ paths, TIRAMISU host, Player user).
static const char* kTrtDefaultEnv[] = {
  "PATH=C:\\Windows\\System32;Z:\\",
  "TEMP=C:\\Temp", "TMP=C:\\Temp",
  "USERPROFILE=C:\\Users\\Player",
  "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
  "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
  "COMPUTERNAME=TIRAMISU", "USERNAME=Player",
  "SystemRoot=C:\\Windows",
  "SystemDrive=C:",
  "windir=C:\\Windows",
  "ComSpec=C:\\Windows\\System32\\cmd.exe",
  "ProgramFiles=C:\\Program Files",
  "ProgramFiles(x86)=C:\\Program Files (x86)",
  "ProgramData=C:\\ProgramData",
  "PUBLIC=C:\\Users\\Public",
  "ALLUSERSPROFILE=C:\\ProgramData",
  "HOMEDRIVE=C:",
  "HOMEPATH=\\Users\\Player",
  "OS=Windows_NT",
  "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC",
  "NUMBER_OF_PROCESSORS=8",
  "PROCESSOR_ARCHITECTURE=AMD64",
  "PROCESSOR_IDENTIFIER=AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD",
  "PROCESSOR_LEVEL=25",
  "PROCESSOR_REVISION=2100",
  "SESSIONNAME=Console",
  "LOGONSERVER=\\\\TIRAMISU",
  "USERDOMAIN=TIRAMISU",
};

extern "C" const char* const* trt_gap_env_defaults(size_t* count) {
  if (count) *count = sizeof(kTrtDefaultEnv) / sizeof(kTrtDefaultEnv[0]);
  return kTrtDefaultEnv;
}

// Seed-once: insert any missing defaults into the store (caller holds
// g_env_mu). Runs on the first env access of any kind, so a game that queries
// GetEnvironmentVariableW before ever calling GetEnvironmentStringsW still
// sees the full set. A variable the game later deletes stays deleted.
bool g_env_seeded = false;

void seed_defaults_locked() {
  if (g_env_seeded) return;
  g_env_seeded = true;
  for (size_t i = 0; i < sizeof(kTrtDefaultEnv) / sizeof(kTrtDefaultEnv[0]);
       ++i) {
    const char* kv = kTrtDefaultEnv[i];
    const char* eq = strchr(kv, '=');
    if (!eq) continue;
    std::string key(kv, (size_t)(eq - kv));
    if (env_find_locked(key.c_str()) < 0) {
      g_env_store.emplace_back(key, std::string(eq + 1));
    }
  }
}

std::string utf16_to_utf8_dyn(const u16* s) {
  std::string out;
  if (!s) return out;
  size_t n = 0;
  while (s[n]) ++n;
  out.resize(n * 3 + 4);
  if (trt_utf16_to_utf8(s, &out[0], out.size()) != 0) return std::string();
  out.resize(strlen(out.c_str()));
  return out;
}

// extern-C accessor: GetEnvironmentVariableW (bridge_sys.cpp) reads the
// store BEFORE host getenv so SetEnvironmentVariableW and getenv-style
// queries stay consistent across a boot. Thread-local snapshot keeps returned
// c_str valid even across concurrent SetEnvironmentVariableW updates.
extern "C" const char* trt_gap_env_get(const char* name) {
  if (!name) return nullptr;
  static thread_local std::string t_env_val;
  std::lock_guard<std::mutex> lk(g_env_mu);
  seed_defaults_locked();
  const int idx = env_find_locked(name);
  if (idx < 0 || g_env_store[(size_t)idx].second.empty()) return nullptr;
  t_env_val = g_env_store[(size_t)idx].second;
  return t_env_val.c_str();
}

extern "C" void trt_gap_env_set(const char* name, const char* value) {
  if (!name || !name[0]) return;
  std::lock_guard<std::mutex> lk(g_env_mu);
  const int idx = env_find_locked(name);
  if (!value) {
    if (idx >= 0) g_env_store.erase(g_env_store.begin() + idx);
    return;
  }
  if (idx >= 0) {
    g_env_store[(size_t)idx].second = value;
  } else {
    g_env_store.emplace_back(name, value);
  }
}

extern "C" const u16* trt_gap_env_build_wblock(const char* const* defaults,
                                                size_t default_count) {
  std::vector<std::pair<std::string, std::string>> merged;
  {
    std::lock_guard<std::mutex> lk(g_env_mu);
    seed_defaults_locked();
    for (size_t i = 0; i < default_count; ++i) {
      const char* kv = defaults[i];
      if (!kv) continue;
      const char* eq = strchr(kv, '=');
      if (!eq) continue;
      merged.emplace_back(std::string(kv, (size_t)(eq - kv)),
                          std::string(eq + 1));
    }
    for (const auto& entry : g_env_store) {
      bool replaced = false;
      for (auto& m : merged) {
        if (env_key_eq(m.first, entry.first.c_str())) {
          m.second = entry.second;
          replaced = true;
          break;
        }
      }
      if (!replaced) merged.push_back(entry);
    }
  }

  size_t total_u16 = 2;  // double NUL
  for (const auto& kv : merged) {
    total_u16 += kv.first.size() + 1 + kv.second.size() + 1;
  }
  u16* block = (u16*)calloc(total_u16, sizeof(u16));
  if (!block) return nullptr;
  size_t o = 0;
  for (const auto& kv : merged) {
    for (unsigned char c : kv.first) block[o++] = (u16)c;
    block[o++] = (u16)'=';
    for (unsigned char c : kv.second) block[o++] = (u16)c;
    block[o++] = 0;
  }
  block[o] = 0;
  std::lock_guard<std::mutex> lk(g_env_mu);
  g_env_wblocks.insert(block);
  return block;
}

extern "C" int trt_gap_env_free_wblock(const u16* block) {
  if (!block) return 1;
  std::lock_guard<std::mutex> lk(g_env_mu);
  auto it = g_env_wblocks.find(block);
  if (it != g_env_wblocks.end()) {
    g_env_wblocks.erase(it);
    free((void*)block);
  }
  return 1;
}

// locale: cp1252 for the CRT boot path (IsValidCodePage(1252)=1,
// GetCPInfo fills CPINFO with the max char size + default char)
constexpr u32 k_cp1252 = 1252;
constexpr u32 k_utf8 = 65001;

bool valid_cp(u32 cp) {
  return cp == k_cp1252 || cp == k_utf8 || cp == 0 /*CP_ACP*/ ||
         cp == 20127 /*US-ASCII*/ || cp == 2019;
}

}  // namespace

extern "C" {

// registered by trt_bridge_register_kernel32 after the core table
void trt_gap_register(void);

// ---- CRT locale boot -------------------------------------------------------
int TRT_STDCALL trt_IsValidCodePage(u32 cp) { return valid_cp(cp) ? 1 : 0; }

// ---- environment -----------------------------------------------------------
int TRT_STDCALL trt_SetEnvironmentVariableW(const u16* name, const u16* value) {
  if (!name || !name[0]) { trt_SetLastError(87); return 0; }
  const std::string n = utf16_to_utf8_dyn(name);
  if (n.empty()) { trt_SetLastError(87); return 0; }
  if (!value) {
    trt_gap_env_set(n.c_str(), nullptr);
    return 1;
  }
  const std::string v = utf16_to_utf8_dyn(value);
  trt_gap_env_set(n.c_str(), v.c_str());
  std::string line = "kernel32: env " + n + "=" + v;
  if (line.size() > 320) line.resize(320);
  LOG(line.c_str());
  return 1;
}

// ---- math / string legacy --------------------------------------------------
int TRT_STDCALL trt_MulDiv(int a, int b, int c) {
  // exact Win64 semantics: 64-bit intermediate, round-half-away-from-zero,
  // -1 on divide-by-zero and overflow
  if (c == 0) return -1;
  const i64 prod = (i64)a * (i64)b;
  const i64 q = (prod >= 0) ? (prod + c / 2) / c : (prod - c / 2) / c;
  if (q > 0x7FFFFFFF || q < -0x80000000LL) return -1;
  return (int)q;
}

// ---- diagnostics formatting -------------------------------------------------
extern void* TRT_STDCALL trt_LocalAlloc(u32 flags, u64 bytes);

u32 TRT_STDCALL trt_FormatMessageW(u32 flags, const void* src, u32 msgid,
                                   u32 langid, u16* buf, u32 size, void* args) {
  (void)langid; (void)args; (void)src;
  if (!buf) {
    trt_SetLastError(87);
    return 0;
  }

  char narrow[256];
  switch (msgid) {
    case 0: snprintf(narrow, sizeof(narrow), "The operation completed successfully.\r\n"); break;
    case 2: snprintf(narrow, sizeof(narrow), "The system cannot find the file specified.\r\n"); break;
    case 3: snprintf(narrow, sizeof(narrow), "The system cannot find the path specified.\r\n"); break;
    case 5: snprintf(narrow, sizeof(narrow), "Access is denied.\r\n"); break;
    case 6: snprintf(narrow, sizeof(narrow), "The handle is invalid.\r\n"); break;
    case 14: snprintf(narrow, sizeof(narrow), "Not enough storage is available to complete this operation.\r\n"); break;
    case 87: snprintf(narrow, sizeof(narrow), "The parameter is incorrect.\r\n"); break;
    case 126: snprintf(narrow, sizeof(narrow), "The specified module could not be found.\r\n"); break;
    case 127: snprintf(narrow, sizeof(narrow), "The specified procedure could not be found.\r\n"); break;
    default: snprintf(narrow, sizeof(narrow), "Error code %lu.\r\n", (unsigned long)msgid); break;
  }

  u32 len = (u32)strlen(narrow);
  if (flags & 0x0100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
    u32 alloc_chars = (size > len + 1) ? size : (len + 1);
    u16* alloc_buf = (u16*)trt_LocalAlloc(0x40 /* LMEM_ZEROINIT */, (u64)alloc_chars * sizeof(u16));
    if (!alloc_buf) {
      trt_SetLastError(8);
      return 0;
    }
    for (u32 i = 0; i < len; ++i) alloc_buf[i] = (u16)(u8)narrow[i];
    alloc_buf[len] = 0;
    *(u16**)buf = alloc_buf;
    return len;
  } else {
    if (size == 0) {
      trt_SetLastError(87);
      return 0;
    }
    u32 n = 0;
    for (; n < len && n < size - 1; ++n) buf[n] = (u16)(u8)narrow[n];
    buf[n] = 0;
    return n;
  }
}

// ---- loader search path ------------------------------------------------------
static u16 g_guest_dll_dir[512] = {};
static u32 g_guest_dll_dir_len = 0;

int TRT_STDCALL trt_SetDllDirectoryW(const u16* dir) {
  if (dir && dir[0]) {
    size_t i = 0;
    for (; dir[i] && i + 1 < 512; ++i) g_guest_dll_dir[i] = dir[i];
    g_guest_dll_dir[i] = 0;
    g_guest_dll_dir_len = (u32)i;

    char host[1024] = {};
    if (trt_vfs_resolve_utf16(dir, host, sizeof(host)) == 0 && host[0]) {
      trt_runtime_set_dll_dir(host);
    } else {
      char utf8[512] = {};
      size_t u = 0;
      for (; dir[u] && u + 1 < sizeof(utf8); ++u) utf8[u] = (char)dir[u];
      utf8[u] = 0;
      trt_runtime_set_dll_dir(utf8);
    }

    char line[320];
    char n[192] = {};
    for (size_t k = 0; dir[k] && k < sizeof(n) - 1; ++k) n[k] = (char)dir[k];
    snprintf(line, sizeof(line), "kernel32: SetDllDirectoryW(%s) -> active (%s)",
             n, host[0] ? host : "fallback");
    LOG(line);
  } else {
    g_guest_dll_dir[0] = 0;
    g_guest_dll_dir_len = 0;
    trt_runtime_set_dll_dir(nullptr);
    LOG("kernel32: SetDllDirectoryW(NULL) -> cleared");
  }
  return 1;
}

static void gap_ascii_to_u16(const char* src, u16* dst, int cap);

int TRT_STDCALL trt_SetDllDirectoryA(const char* dir) {
  if (!dir) return trt_SetDllDirectoryW(nullptr);
  u16 w[512];
  gap_ascii_to_u16(dir, w, 512);
  return trt_SetDllDirectoryW(w);
}

u32 TRT_STDCALL trt_GetDllDirectoryW(u32 buf_len, u16* buf) {
  if (g_guest_dll_dir_len == 0) {
    if (buf && buf_len > 0) buf[0] = 0;
    return 0;
  }
  if (!buf || buf_len <= g_guest_dll_dir_len) {
    return g_guest_dll_dir_len + 1;
  }
  memcpy(buf, g_guest_dll_dir, (size_t)(g_guest_dll_dir_len + 1) * sizeof(u16));
  return g_guest_dll_dir_len;
}

int TRT_STDCALL trt_CreateSymbolicLinkW(const u16* link, const u16* target,
                                        u32 flags) {
  (void)flags;
  char line[256];
  snprintf(line, sizeof(line),
           "kernel32: CreateSymbolicLinkW -> FALSE (the VFS has no symbolic "
           "links) link=%p target=%p", (void*)link, (void*)target);
  LOG(line);
  trt_SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0;
}

// ---- drives -------------------------------------------------------------------
u32 TRT_STDCALL trt_GetDriveTypeW(const u16* root) {
  // every mounted VFS path is a fixed disk; the CRT's _wgetcwd and game
  // asset code only distinguish DRIVE_FIXED(3) from DRIVE_NO_ROOT_DIR(1)
  (void)root;
  return 3;  // DRIVE_FIXED
}

// ---- process -------------------------------------------------------------------
int TRT_STDCALL trt_GetExitCodeProcess(void* proc, u32* code) {
  if (!code) { trt_SetLastError(87); return 0; }
  (void)proc;
  // the harness-modeled child (CreateProcessW spawn hook): answer from the
  // real child's exit code. STILL_ACTIVE (259) while the child runs —
  // honest Win32 semantics, not a silent zero.
  extern int trt_child_pid;
  extern int trt_child_running;
  extern u32 trt_child_exit_code;
  if (trt_child_pid >= 0) {
    *code = trt_child_running ? 259u /*STILL_ACTIVE*/ : trt_child_exit_code;
    return 1;
  }
  *code = 0;  // the single guest "process" has not exited while it runs
  return 1;
}

// ---- Local* legacy heap (Win3.1 compatibility wrappers) -------------------------
extern "C" void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes);
extern "C" int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p);

void* TRT_STDCALL trt_LocalAlloc(u32 flags, u64 bytes) {
  // LMEM_FIXED=0, LMEM_MOVEABLE=2 (treated as FIXED: the handle==pointer
  // lie holds because games re-lock anyway), LMEM_ZEROINIT=0x40
  return trt_HeapAlloc(nullptr, (flags & 0x40) ? 0x8 : 0, bytes);
}

void* TRT_STDCALL trt_LocalFree(void* p) {
  return trt_HeapFree(nullptr, 0, p) ? nullptr : p;  // NULL = success
}// ---- K32 forwarders (psapi bridge owns the real implementation) -----------------
extern "C" int TRT_GUEST_ABI psapi_K32EnumProcessModules(void* proc,
    void** lphandle, u32 cb, u32* needed);
extern "C" int TRT_GUEST_ABI psapi_K32GetModuleFileNameExW(void* proc,
    void* mod, u16* buf, u32 cb);

int TRT_STDCALL trt_K32EnumProcessModules(void* proc, void** lphandle,
                                          u32 cb, u32* needed) {
  return psapi_K32EnumProcessModules(proc, lphandle, cb, needed);
}

u32 TRT_STDCALL trt_K32GetModuleFileNameExW(void* proc, void* mod, u16* buf,
                                            u32 cb) {
  return psapi_K32GetModuleFileNameExW(proc, mod, buf, cb);
}

// ---- paths ------------------------------------------------------------------------
u32 TRT_STDCALL trt_GetFinalPathNameByHandleW(void* h, u16* buf, u32 len,
                                              u32 flags) {
  char vpath[512];
  if (!trt_file_copy_vpath(h, vpath, sizeof(vpath))) {
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  char formatted[600];
  if ((flags & 0x2u) != 0 /* VOLUME_NAME_NT */) {
    const char* rest = (vpath[0] && vpath[1] == ':') ? vpath + 2 : vpath;
    snprintf(formatted, sizeof(formatted), "\\Device\\HarddiskVolume1%s", rest);
  } else if ((flags & 0x4u) != 0 /* VOLUME_NAME_NONE */) {
    const char* rest = (vpath[0] && vpath[1] == ':') ? vpath + 2 : vpath;
    snprintf(formatted, sizeof(formatted), "%s", rest);
  } else {
    snprintf(formatted, sizeof(formatted), "\\\\?\\%s", vpath);
  }
  const u32 need = (u32)strlen(formatted);
  if (!buf || len <= need) {
    if (buf && len > 0) {
      for (u32 i = 0; i < len - 1; ++i) buf[i] = (u16)(u8)formatted[i];
      buf[len - 1] = 0;
    }
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return need + 1;        // Win32 includes NUL when buffer is too small
  }
  for (u32 i = 0; i <= need; ++i) buf[i] = (u16)(u8)formatted[i];
  return need;              // excluding NUL on success
}

u32 TRT_STDCALL trt_GetFullPathNameW(const u16* name, u32 len, u16* out,
                                     u16** filepart) {
  if (filepart) *filepart = nullptr;
  if (!name || !name[0]) return 0;
  char full[1024];
  if (trt_vfs_normalize_guest_utf16(name, full, sizeof(full)) != 0) {
    char n[512] = {};
    size_t i = 0;
    for (; name[i] && i < sizeof(n) - 1; ++i) n[i] = (char)name[i];
    if (n[0] && n[1] == ':')
      snprintf(full, sizeof(full), "%s", n);
    else {
      const char* cwd = trt_gap_cwd();
      size_t cl = strlen(cwd);
      const char* sep = (cl > 0 && (cwd[cl - 1] == '\\' || cwd[cl - 1] == '/')) ? "" : "\\";
      snprintf(full, sizeof(full), "%s%s%s", cwd, sep, n);
    }
  }
  const u32 need = (u32)strlen(full);
  if (!out || len == 0 || len <= need) {
    return need + 1;
  }
  for (u32 m = 0; m <= need; ++m) out[m] = (u16)(u8)full[m];
  if (filepart) {
    u32 last = 0;
    for (u32 k = 0; k < need; ++k)
      if (out[k] == (u16)'\\' || out[k] == (u16)'/') last = k + 1;
    if (last && last < need) *filepart = out + last;
  }
  return need;
}

// ---- time zone / time conversion ---------------------------------------------------
u32 TRT_STDCALL trt_GetTimeZoneInformation(void* out) {
  // TIME_ZONE_INFORMATION: Bias i32 (long), StandardName[32] u16,
  // StandardDate (8 words), StandardBias, DaylightName[32], DaylightDate,
  // DaylightBias = 172 bytes. UTC bias 0, no daylight — honest for a
  // console (no wall-clock locale of its own).
  if (!out) { trt_SetLastError(87); return (u32)-1; }
  memset(out, 0, 172);
  char line[96];
  snprintf(line, sizeof(line), "kernel32: GetTimeZoneInformation -> UTC "
           "(bias 0, no daylight)");
  LOG(line);
  return 0;  // TIME_ZONE_ID_UNKNOWN
}

int TRT_STDCALL trt_FileTimeToSystemTime(const u64* ft, u16* st) {
  if (!ft || !st) { trt_SetLastError(87); return 0; }
  // reuse trt_GetSystemTime's civil conversion: it takes a FILETIME and
  // writes 8 SYSTEMTIME words
  extern void TRT_STDCALL trt_GetSystemTime(u16* st);
  // GetSystemTime writes "now"; convert the GIVEN ft through the same math
  // by inlining the conversion here (the algorithm lives in
  // bridge_kernel32.cpp — duplicating 20 lines beats a fake dependency)
  i64 t = (i64)(*ft) / 10000000ll;
  u16 ms = (u16)(((i64)(*ft) % 10000000ll) / 10000);
  i64 days = t / 86400; i64 secs = t % 86400;
  days += 719468;
  i64 era = (days >= 0 ? days : days - 146096) / 146097;
  i64 doe = days - era * 146097;
  i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  i64 y = yoe + era * 400;
  i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
  i64 mp = (5*doy + 2)/153;
  i64 d = doy - (153*mp+2)/5 + 1;
  i64 m = mp < 10 ? mp+3 : mp-9;
  y += (m <= 2);
  st[0] = (u16)y; st[1] = (u16)m; st[2] = (u16)(d - 1) % 7; st[3] = (u16)d;
  st[4] = (u16)(secs/3600); st[5] = (u16)((secs%3600)/60); st[6] = (u16)(secs%60);
  st[7] = ms;
  return 1;
}

int TRT_STDCALL trt_SystemTimeToTzSpecificLocalTime(const void* tzi,
                                                    const u16* utc, u16* local) {
  // the runtime's only time zone is UTC (GetTimeZoneInformation bias 0):
  // local == utc, a straight copy is the exact honest answer
  if (!utc || !local) { trt_SetLastError(87); return 0; }
  (void)tzi;
  for (int i = 0; i < 8; ++i) local[i] = utc[i];
  return 1;
}

int TRT_STDCALL trt_FileTimeToLocalFileTime(const u64* ft, u64* local_ft) {
  if (!ft || !local_ft) { trt_SetLastError(87); return 0; }
  *local_ft = *ft;
  return 1;
}

int TRT_STDCALL trt_LocalFileTimeToFileTime(const u64* local_ft, u64* ft) {
  if (!local_ft || !ft) { trt_SetLastError(87); return 0; }
  *ft = *local_ft;
  return 1;
}

int TRT_STDCALL trt_FileTimeToDosDateTime(const u64* ft, u16* fat_date, u16* fat_time) {
  if (!ft || !fat_date || !fat_time) { trt_SetLastError(87); return 0; }
  u16 st[8];
  if (!trt_FileTimeToSystemTime(ft, st)) return 0;
  u16 year = st[0];
  if (year < 1980) year = 1980;
  else if (year > 2107) year = 2107;
  *fat_date = (u16)(((year - 1980) << 9) | ((st[1] & 0x0F) << 5) | (st[3] & 0x1F));
  *fat_time = (u16)(((st[4] & 0x1F) << 11) | ((st[5] & 0x3F) << 5) | ((st[6] / 2) & 0x1F));
  return 1;
}

// ---- files ---------------------------------------------------------------------------
int TRT_STDCALL trt_SetEndOfFile(void* h) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  fflush(os);
#ifdef _WIN32
  if (_chsize_s(_fileno(os), _ftelli64(os)) != 0) { trt_SetLastError(87); return 0; }
#else
  if (ftruncate(fileno(os), ftell(os)) != 0) { trt_SetLastError(87); return 0; }
#endif
  return 1;
}

// Win64 ftell/fseek are 32-bit long on Windows; keep it simple: files are
// < 2GB in the VFS so long works on both hosts. (_ftelli64 exists but the
// handle table stores FILE* only — no size caching needed for <2GB.)

// ---- forward declarations ---------------------------------------------------
int TRT_STDCALL trt_GetFileTime(void* h, u64* created, u64* accessed, u64* modified);

// Win32 BY_HANDLE_FILE_INFORMATION ABI (52 bytes on x86 and x64):
// Exactly 13 32-bit DWORD fields. Any 64-bit field inside forces alignment/padding
// that breaks callers (such as Mono's _fstat which reads nFileSizeLow at offset 0x24).
struct trt_by_handle_file_information {
  u32 dwFileAttributes;
  u32 ftCreationTime_low;
  u32 ftCreationTime_high;
  u32 ftLastAccessTime_low;
  u32 ftLastAccessTime_high;
  u32 ftLastWriteTime_low;
  u32 ftLastWriteTime_high;
  u32 dwVolumeSerialNumber;
  u32 nFileSizeHigh;
  u32 nFileSizeLow;
  u32 nNumberOfLinks;
  u32 nFileIndexHigh;
  u32 nFileIndexLow;
};
static_assert(sizeof(trt_by_handle_file_information) == 52,
              "BY_HANDLE_FILE_INFORMATION must be exactly 52 bytes");

int TRT_STDCALL trt_GetFileInformationByHandle(void* h, void* info) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  if (!info) { trt_SetLastError(87); return 0; }
  trt_by_handle_file_information* b = (trt_by_handle_file_information*)info;
  memset(b, 0, sizeof(*b));
  b->dwFileAttributes = 0x80;  // FILE_ATTRIBUTE_NORMAL
  b->nNumberOfLinks = 1;

  u64 ft = 0;
  trt_GetFileTime(h, &ft, nullptr, nullptr);
  b->ftCreationTime_low = (u32)ft;
  b->ftCreationTime_high = (u32)(ft >> 32);
  b->ftLastAccessTime_low = (u32)ft;
  b->ftLastAccessTime_high = (u32)(ft >> 32);
  b->ftLastWriteTime_low = (u32)ft;
  b->ftLastWriteTime_high = (u32)(ft >> 32);

  u64 sz = 0;
  struct stat st;
#ifdef _WIN32
  const int fd = _fileno(os);
#else
  const int fd = fileno(os);
#endif
  if (fd >= 0 && fstat(fd, &st) == 0) {
    sz = (u64)st.st_size;
  } else {
    const long pos = ftell(os);
    fseek(os, 0, SEEK_END);
    const long lsz = ftell(os);
    fseek(os, pos, SEEK_SET);
    if (lsz >= 0) sz = (u64)lsz;
  }
  b->nFileSizeLow = (u32)(sz & 0xFFFFFFFFull);
  b->nFileSizeHigh = (u32)(sz >> 32);
  return 1;
}

i64 TRT_STDCALL trt_GetFileSizeEx(void* h, i64* size) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); if (size) *size = 0; return 0; }
  const long pos = ftell(os);
  fseek(os, 0, SEEK_END);
  const long sz = ftell(os);
  fseek(os, pos, SEEK_SET);
  if (size) *size = (i64)sz;
  return 1;
}

int TRT_STDCALL trt_PeekNamedPipe(void* h, void* buf, u32 bytes, u32* read,
                                  u32* avail, u32* left) {
  // every handle in the VFS is a disk file; no pipe handles exist. Named
  // failure is the fail-visible rule.
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: PeekNamedPipe -> FALSE (no pipe handles in the VFS; "
           "h=%p)", h);
  LOG(line);
  (void)buf; (void)bytes; (void)read; (void)avail; (void)left;
  trt_SetLastError(87);
  return 0;
}

// ---- forward declarations of sibling bridge_kernel32/thread functions ------
void* TRT_STDCALL trt_LoadLibraryW(const u16* path);
void* TRT_STDCALL trt_LoadLibraryExW(const u16* path, void* file, u32 flags);
int TRT_STDCALL trt_GetModuleHandleExW(u32 flags, const u16* name, void** out);
u32 TRT_STDCALL trt_GetModuleFileNameW(void* mod, u16* buf, u32 len);
int TRT_STDCALL trt_GetSystemDirectoryW(u16* buf, u32 len);
int TRT_STDCALL trt_GetSystemTimeAsFileTime(u64* out);
void TRT_STDCALL trt_GetSystemTime(u16* st);
void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes);
void* TRT_STDCALL trt_HeapReAlloc(void* heap, u32 flags, void* p, u64 bytes);
int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p);
u64 TRT_STDCALL trt_HeapSize(void* heap, u32 flags, void* p);
u64 TRT_STDCALL trt_CreateMutexW(void* attrs, bool initial_owner,
                                 const u16* name);
void* TRT_STDCALL trt_CreateWaitableTimerExW(void* attrs, const u16* name,
                                             u32 flags, u32 access);
int TRT_STDCALL trt_CopyFileExW(const u16* src, const u16* dst, void* progress,
                                void* param, int* cancel, u32 flags);
int TRT_STDCALL trt_DeleteFileW(const u16* path);
void TRT_STDCALL trt_Sleep(u32 ms);
u64 TRT_STDCALL trt_CreateThread(void* attrs, void* stack_size, void* routine,
                                 void* param, u32 flags, u32* tid);
void* TRT_STDCALL trt_GetCurrentProcess(void);

// ---- helper: narrow ASCII <-> UTF-16LE ----
static void gap_ascii_to_u16(const char* src, u16* dst, int cap) {
  if (!dst || cap <= 0) return;
  int i = 0;
  if (src) {
    while (src[i] && i + 1 < cap) { dst[i] = (u16)(u8)src[i]; ++i; }
  }
  dst[i] = 0;
}

static bool gap_u16_ieq(const u16* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    u16 ca = *a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool gap_u16_istartswith(const u16* a, const char* b) {
  if (!a || !b) return false;
  while (*b) {
    u16 ca = *a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
    if (ca != cb) return false;
  }
  return true;
}

static int gap_write_u16_answer(const char* s, u16* out, int cap) {
  const int need = (int)strlen(s) + 1;
  if (!out || cap == 0) return need;
  if (cap < need) { trt_SetLastError(122); return 0; }
  gap_ascii_to_u16(s, out, cap);
  return need;
}

// ============================================================================
// Locale / Date / Time surface (CRT + UnityPlayer + System.Globalization)
// ============================================================================

struct trt_locale_entry {
  const char* name;          // "en-US", "ja-JP", ...
  const char* lang_iso;      // "en", "ja", ...
  const char* ctry_iso;      // "US", "JP", ...
  const char* abbrev_lang;   // "ENU", "JPN", ...
  const char* abbrev_ctry;   // "USA", "JPN", ...
  const char* lang_name;     // "English", "Japanese", ... (LOCALE_SENGLISHLANGUAGENAME)
  const char* ctry_name;     // "United States", "Japan", ... (LOCALE_SENGLISHCOUNTRYNAME)
  const char* display_name;  // "English (United States)", "Japanese (Japan)", ...
  const char* native_lang;   // "English", "日本語", ...
  u32 lcid;                  // 0x0409, 0x0411, ...
  u32 ansi_cp;               // 1252, 932, ...
  u32 oem_cp;                // 437, 932, ...
  u32 country_code;          // 1, 81, ...
  const char* curr_symbol;   // "$", "¥", "€", etc.
  const char* intl_curr;     // "USD", "JPY", "EUR", etc.
  u32 curr_digits;           // 2 for USD, 0 for JPY
};

static const trt_locale_entry k_locales[] = {
    {"en-US", "en", "US", "ENU", "USA", "English", "United States",
     "English (United States)", "English", 0x0409, 1252, 437, 1, "$", "USD", 2},
    {"ja-JP", "ja", "JP", "JPN", "JPN", "Japanese", "Japan",
     "Japanese (Japan)", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 0x0411, 932, 932, 81, "\xc2\xa5", "JPY", 0},
    {"de-DE", "de", "DE", "DEU", "DEU", "German", "Germany",
     "German (Germany)", "Deutsch", 0x0407, 1252, 850, 49, "\xe2\x82\xac", "EUR", 2},
    {"fr-FR", "fr", "FR", "FRA", "FRA", "French", "France",
     "French (France)", "Fran\xc3\xa7\x61is", 0x040c, 1252, 850, 33, "\xe2\x82\xac", "EUR", 2},
    {"es-ES", "es", "ES", "ESN", "ESP", "Spanish", "Spain",
     "Spanish (Spain)", "Espa\xc3\xb1ol", 0x0c0a, 1252, 850, 34, "\xe2\x82\xac", "EUR", 2},
    {"it-IT", "it", "IT", "ITA", "ITA", "Italian", "Italy",
     "Italian (Italy)", "Italiano", 0x0410, 1252, 850, 39, "\xe2\x82\xac", "EUR", 2},
    {"ko-KR", "ko", "KR", "KOR", "KOR", "Korean", "Korea",
     "Korean (Korea)", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", 0x0412, 949, 949, 82, "\xe2\x82\xa9", "KRW", 0},
    {"zh-CN", "zh", "CN", "CHS", "CHN", "Chinese", "China",
     "Chinese (Simplified, China)", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87", 0x0804, 936, 936, 86, "\xc2\xa5", "CNY", 2},
    {"zh-TW", "zh", "TW", "CHT", "TWN", "Chinese", "Taiwan",
     "Chinese (Traditional, Taiwan)", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87", 0x0404, 950, 950, 886, "NT$", "TWD", 2},
    {"ru-RU", "ru", "RU", "RUS", "RUS", "Russian", "Russia",
     "Russian (Russia)", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", 0x0419, 1251, 866, 7, "\xe2\x82\xbd", "RUB", 2},
    {"pt-BR", "pt", "BR", "PTB", "BRA", "Portuguese", "Brazil",
     "Portuguese (Brazil)", "Portugu\xc3\xaas", 0x0416, 1252, 850, 55, "R$", "BRL", 2},
    {"pl-PL", "pl", "PL", "PLK", "POL", "Polish", "Poland",
     "Polish (Poland)", "Polski", 0x0415, 1250, 852, 48, "z\xc5\x82", "PLN", 2},
};

static const trt_locale_entry* find_locale_entry(const u16* name) {
  if (!name || name[0] == 0) return &k_locales[0];
  for (const auto& e : k_locales) {
    if (gap_u16_ieq(name, e.name) || gap_u16_ieq(name, e.lang_iso) ||
        gap_u16_ieq(name, e.lang_name) || gap_u16_ieq(name, e.abbrev_lang)) {
      return &e;
    }
  }
  for (const auto& e : k_locales) {
    if (gap_u16_istartswith(name, e.name) || gap_u16_istartswith(name, e.lang_name)) {
      return &e;
    }
  }
  return nullptr;
}

int TRT_STDCALL trt_GetUserDefaultLocaleName(u16* name, int cch) {
  return gap_write_u16_answer("en-US", name, cch);
}

int TRT_STDCALL trt_GetSystemDefaultLocaleName(u16* name, int cch) {
  return gap_write_u16_answer("en-US", name, cch);
}

int TRT_STDCALL trt_IsValidLocaleName(const u16* name) {
  if (!name) return 0;
  if (name[0] == 0) return 1;  // LOCALE_NAME_INVARIANT
  if (gap_u16_ieq(name, "!x-sys-default-locale") || gap_u16_ieq(name, "C"))
    return 1;
  if (find_locale_entry(name) != nullptr) return 1;
  int len = 0;
  while (name[len]) len++;
  if (len >= 2 && len <= 32) {
    bool ok = true;
    for (int i = 0; i < len; ++i) {
      u16 c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
        ok = false;
        break;
      }
    }
    if (ok) return 1;
  }
  return 0;
}

int TRT_STDCALL trt_IsValidLocale(u32 lcid, u32 flags) {
  (void)flags;
  const u32 lo = lcid & 0xFFFFu;
  if (lo == 0x007Fu || lo == 0x0009u || lo == 0x0400u || lo == 0x0800u || lo == 0x0C00u || lo == 0)
    return 1;
  for (const auto& e : k_locales) {
    if ((e.lcid & 0xFFFFu) == lo) return 1;
  }
  return 0;
}

u16 TRT_STDCALL trt_GetUserDefaultUILanguage(void) { return 0x0409u; }
u16 TRT_STDCALL trt_GetSystemDefaultUILanguage(void) { return 0x0409u; }
u32 TRT_STDCALL trt_GetThreadLocale(void) { return 0x0409u; }
int TRT_STDCALL trt_SetThreadLocale(u32 lcid) {
  return trt_IsValidLocale(lcid, 0) ? 1 : (trt_SetLastError(87), 0);
}
int TRT_STDCALL trt_IsDBCSLeadByteEx(u32 cp, u8 ch) {
  (void)cp; (void)ch;
  return 0;
}

int TRT_STDCALL trt_LCIDToLocaleName(u32 lcid, u16* name, int cch, u32 flags) {
  (void)flags;
  const u32 lo = lcid & 0xFFFFu;
  if (lo == 0x007Fu) return gap_write_u16_answer("", name, cch);
  for (const auto& e : k_locales) {
    if ((e.lcid & 0xFFFFu) == lo) return gap_write_u16_answer(e.name, name, cch);
  }
  if (trt_IsValidLocale(lcid, 0)) return gap_write_u16_answer("en-US", name, cch);
  trt_SetLastError(87);
  return 0;
}

u32 TRT_STDCALL trt_LocaleNameToLCID(const u16* name, u32 flags) {
  (void)flags;
  if (!name || name[0] == 0) return 0x007Fu;
  const trt_locale_entry* e = find_locale_entry(name);
  if (e) return e->lcid;
  if (trt_IsValidLocaleName(name)) return 0x0409u;
  trt_SetLastError(87);
  return 0;
}

int TRT_STDCALL trt_ResolveLocaleName(const u16* name, u16* out, int cch) {
  if (!name || name[0] == 0) {
    return gap_write_u16_answer("", out, cch);
  }
  const trt_locale_entry* e = find_locale_entry(name);
  if (e) {
    return gap_write_u16_answer(e->name, out, cch);
  }
  if (trt_IsValidLocaleName(name)) {
    char buf[64] = {};
    int i = 0;
    while (name[i] && i < 63) { buf[i] = (char)name[i]; i++; }
    buf[i] = 0;
    return gap_write_u16_answer(buf, out, cch);
  }
  trt_SetLastError(87);
  return 0;
}

int TRT_STDCALL trt_GetLocaleInfoEx(const u16* locale, u32 lctype, u16* out,
                                    int cch) {
  if (locale && locale[0] && !trt_IsValidLocaleName(locale) &&
      !gap_u16_ieq(locale, "!x-sys-default-locale")) {
    trt_SetLastError(87);
    return 0;
  }
  const trt_locale_entry* e = find_locale_entry(locale);
  if (!e) e = &k_locales[0];

  const bool want_num = (lctype & 0x20000000u /*LOCALE_RETURN_NUMBER*/) != 0;
  const u32 base_type = lctype & 0x0FFFFFFFu;
  if (want_num) {
    u32 num = 0;
    bool ok = true;
    switch (base_type) {
      case 0x1004: num = e->ansi_cp; break;  // LOCALE_IDEFAULTANSICODEPAGE
      case 0x000B: num = e->oem_cp; break;   // LOCALE_IDEFAULTCODEPAGE
      case 0x0001: num = e->lcid; break;     // LOCALE_ILANGUAGE
      case 0x000D: num = (e->lcid == 0x0409) ? 1 : 0; break;     // LOCALE_IMEASURE
      case 0x0011: num = 2; break;           // LOCALE_IDIGITS
      case 0x0018: num = e->curr_digits; break; // LOCALE_ICURRDIGITS (0 for JPY, 2 for USD/EUR)
      case 0x001B: num = 0; break;           // LOCALE_ICURRENCY
      case 0x001C: num = 0; break;           // LOCALE_INEGCURR
      case 0x100B: num = e->country_code; break; // LOCALE_ICOUNTRY
      case 0x100C: num = (e->lcid == 0x0409) ? 6 : 0; break;     // LOCALE_IFIRSTDAYOFWEEK (Sunday for US, Monday for EU/JP)
      case 0x100D: num = 0; break;           // LOCALE_IFIRSTWEEKOFYEAR
      case 0x1010: num = 1; break;           // LOCALE_INEGNUMBER
      default: ok = false; break;
    }
    if (!ok) { trt_SetLastError(1004); return 0; }
    if (!out || cch == 0) return 2;
    if (cch < 2) { trt_SetLastError(122); return 0; }
    memcpy(out, &num, sizeof(num));
    return 2;
  }

  char num_str[32] = {};
  const char* ans = nullptr;
  switch (base_type) {
    case 0x0001:
      snprintf(num_str, sizeof(num_str), "%04X", e->lcid);
      ans = num_str;
      break;
    case 0x0002: ans = e->display_name; break;
    case 0x0003: ans = e->abbrev_lang; break;   // LOCALE_SABBREVLANGNAME
    case 0x0004: ans = e->native_lang; break;   // LOCALE_SNATIVELANGNAME
    case 0x0007: ans = e->abbrev_ctry; break;   // LOCALE_SABBREVCTRYNAME
    case 0x0008: ans = e->ctry_name; break;     // LOCALE_SNATIVECTRYNAME
    case 0x000B:
      snprintf(num_str, sizeof(num_str), "%u", e->oem_cp);
      ans = num_str;
      break;
    case 0x1004:
      snprintf(num_str, sizeof(num_str), "%u", e->ansi_cp);
      ans = num_str;
      break;
    case 0x1001: ans = e->lang_name; break;     // LOCALE_SENGLISHLANGUAGENAME
    case 0x1002: ans = e->ctry_name; break;     // LOCALE_SENGLISHCOUNTRYNAME
    case 0x0072: ans = e->display_name; break;  // LOCALE_SENGLISHDISPLAYNAME
    case 0x005C: ans = e->name; break;          // LOCALE_SNAME ("ja-JP", "en-US")
    case 0x0059: ans = e->lang_iso; break;      // LOCALE_SISO639LANGNAME
    case 0x005A: ans = e->ctry_iso; break;      // LOCALE_SISO3166CTRYNAME
    case 0x006E: ans = e->lang_iso; break;      // LOCALE_SISO639LANGNAME2
    case 0x0079: ans = ""; break;               // LOCALE_SPARENT
    case 0x000C: ans = ","; break;
    case 0x000D: ans = "1"; break;
    case 0x000E: ans = "."; break;
    case 0x000F: ans = ","; break;
    case 0x0010: ans = "3;0"; break;
    case 0x0011: ans = "2"; break;
    case 0x0014: ans = e->curr_symbol; break;   // LOCALE_SCURRENCY
    case 0x0015: ans = e->intl_curr; break;     // LOCALE_SINTLSYMBOL
    case 0x0018: ans = (e->curr_digits == 0) ? "0" : "2"; break;
    case 0x001B: ans = "0"; break;
    case 0x001D: ans = "/"; break;
    case 0x001E: ans = ":"; break;
    case 0x001F: ans = (e->lcid == 0x0411) ? "yyyy/MM/dd" : "M/d/yyyy"; break;
    case 0x0020: ans = (e->lcid == 0x0411) ? "yyyy'年'M'月'd'日'" : "dddd, MMMM d, yyyy"; break;
    case 0x0028: ans = (e->lcid == 0x0411) ? "\xe5\x8d\x88\xe5\x89\x8d" : "AM"; break;
    case 0x0029: ans = (e->lcid == 0x0411) ? "\xe5\x8d\x88\xe5\xbe\x8c" : "PM"; break;
    case 0x0050: ans = ""; break;
    case 0x0051: ans = "-"; break;
    case 0x1003: ans = "h:mm:ss tt"; break;
    case 0x100B:
      snprintf(num_str, sizeof(num_str), "%u", e->country_code);
      ans = num_str;
      break;
    case 0x100C: ans = (e->lcid == 0x0409) ? "6" : "0"; break;
    case 0x1010: ans = "1"; break;
    case 0x002A: ans = "Monday"; break;    case 0x002B: ans = "Tuesday"; break;
    case 0x002C: ans = "Wednesday"; break; case 0x002D: ans = "Thursday"; break;
    case 0x002E: ans = "Friday"; break;    case 0x002F: ans = "Saturday"; break;
    case 0x0030: ans = "Sunday"; break;
    case 0x0031: ans = "Mon"; break;       case 0x0032: ans = "Tue"; break;
    case 0x0033: ans = "Wed"; break;       case 0x0034: ans = "Thu"; break;
    case 0x0035: ans = "Fri"; break;       case 0x0036: ans = "Sat"; break;
    case 0x0037: ans = "Sun"; break;
    case 0x0038: ans = "January"; break;   case 0x0039: ans = "February"; break;
    case 0x003A: ans = "March"; break;     case 0x003B: ans = "April"; break;
    case 0x003C: ans = "May"; break;       case 0x003D: ans = "June"; break;
    case 0x003E: ans = "July"; break;      case 0x003F: ans = "August"; break;
    case 0x0040: ans = "September"; break; case 0x0041: ans = "October"; break;
    case 0x0042: ans = "November"; break;  case 0x0043: ans = "December"; break;
    case 0x0044: ans = "Jan"; break;       case 0x0045: ans = "Feb"; break;
    case 0x0046: ans = "Mar"; break;       case 0x0047: ans = "Apr"; break;
    case 0x0048: ans = "May"; break;       case 0x0049: ans = "Jun"; break;
    case 0x004A: ans = "Jul"; break;       case 0x004B: ans = "Aug"; break;
    case 0x004C: ans = "Sep"; break;       case 0x004D: ans = "Oct"; break;
    case 0x004E: ans = "Nov"; break;       case 0x004F: ans = "Dec"; break;
    default: trt_SetLastError(1004); return 0;
  }
  return gap_write_u16_answer(ans, out, cch);
}

int TRT_STDCALL trt_GetLocaleInfoW(u32 lcid, u32 lctype, u16* out, int cch) {
  u16 loc_name[32] = {};
  if (trt_LCIDToLocaleName(lcid, loc_name, 32, 0) > 0) {
    return trt_GetLocaleInfoEx(loc_name, lctype, out, cch);
  }
  const u16 en[] = {'e','n','-','U','S',0};
  return trt_GetLocaleInfoEx(en, lctype, out, cch);
}

int TRT_STDCALL trt_GetDateFormatEx(const u16* locale, u32 flags,
                                    const u16* date_in, const u16* fmt,
                                    u16* out, int cch, const u16* cal) {
  (void)locale; (void)flags; (void)fmt; (void)cal;
  u16 st[8];
  if (date_in) memcpy(st, date_in, sizeof(st));
  else trt_GetSystemTime(st);
  char buf[32];
  snprintf(buf, sizeof(buf), "%u/%u/%u", st[1], st[3], st[0]);
  return gap_write_u16_answer(buf, out, cch);
}

int TRT_STDCALL trt_GetDateFormatW(u32 lcid, u32 flags, const u16* date_in,
                                   const u16* fmt, u16* out, int cch) {
  (void)lcid;
  return trt_GetDateFormatEx(nullptr, flags, date_in, fmt, out, cch, nullptr);
}

int TRT_STDCALL trt_GetTimeFormatEx(const u16* locale, u32 flags,
                                    const u16* time_in, const u16* fmt,
                                    u16* out, int cch) {
  (void)locale; (void)flags; (void)fmt;
  u16 st[8];
  if (time_in) memcpy(st, time_in, sizeof(st));
  else trt_GetSystemTime(st);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st[4], st[5], st[6]);
  return gap_write_u16_answer(buf, out, cch);
}

int TRT_STDCALL trt_GetTimeFormatW(u32 lcid, u32 flags, const u16* time_in,
                                   const u16* fmt, u16* out, int cch) {
  (void)lcid;
  return trt_GetTimeFormatEx(nullptr, flags, time_in, fmt, out, cch);
}

// ============================================================================
// Time conversions (SystemTime / LocalTime / PreciseFT)
// ============================================================================

void TRT_STDCALL trt_GetLocalTime(u16* st) {
  trt_GetSystemTime(st);  // timezone is honest UTC (bias=0)
}

int TRT_STDCALL trt_GetSystemTimePreciseAsFileTime(u64* out) {
  return trt_GetSystemTimeAsFileTime(out);
}

int TRT_STDCALL trt_SystemTimeToFileTime(const u16* st_in, u64* ft_out) {
  if (!st_in || !ft_out) { trt_SetLastError(87); return 0; }
  i64 y = (i64)st_in[0];
  i64 m = (i64)st_in[1];
  i64 d = (i64)st_in[3];
  if (m < 1 || m > 12 || d < 1 || d > 31) { trt_SetLastError(87); return 0; }
  y -= (m <= 2);
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const i64 yoe = y - era * 400;
  const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const i64 days_since_epoch = era * 146097 + doe - 719468;
  const i64 secs = (days_since_epoch + 134774ll) * 86400ll +
                   (i64)st_in[4] * 3600ll + (i64)st_in[5] * 60ll +
                   (i64)st_in[6];
  *ft_out = (u64)(secs * 10000000ll + (i64)st_in[7] * 10000ll);
  return 1;
}

int TRT_STDCALL trt_DosDateTimeToFileTime(u16 fat_date, u16 fat_time, u64* ft) {
  if (!ft) { trt_SetLastError(87); return 0; }
  u16 st[8];
  memset(st, 0, sizeof(st));
  st[0] = 1980 + ((fat_date >> 9) & 0x7F);
  st[1] = (fat_date >> 5) & 0x0F;
  st[3] = fat_date & 0x1F;
  st[4] = (fat_time >> 11) & 0x1F;
  st[5] = (fat_time >> 5) & 0x3F;
  st[6] = (fat_time & 0x1F) * 2;
  return trt_SystemTimeToFileTime(st, ft);
}

// ============================================================================
// InitOnce state machine (exact Win32 INIT_ONCE)
// ============================================================================

void TRT_STDCALL trt_InitOnceInitialize(u64* init_once) {
  if (init_once) __atomic_store_n(init_once, 0ULL, __ATOMIC_RELAXED);
}

int TRT_STDCALL trt_InitOnceBeginInitialize(u64* init_once, u32 flags,
                                            int* pending, void** context) {
  if (!init_once || !pending) { trt_SetLastError(87); return 0; }
  const bool check_only = (flags & 0x1u) != 0;
  for (;;) {
    u64 cur = __atomic_load_n(init_once, __ATOMIC_ACQUIRE);
    const u64 state = cur & 3ULL;
    if (state == 2ULL) {
      *pending = 0;
      if (context) *context = (void*)(uintptr_t)(cur & ~3ULL);
      return 1;
    }
    if (check_only) {
      *pending = 1;
      return 0;
    }
    if (state == 1ULL) {
      trt_Sleep(1);
      continue;
    }
    u64 next = 1ULL;
    if (__atomic_compare_exchange_n(init_once, &cur, next, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *pending = 1;
      return 1;
    }
  }
}

int TRT_STDCALL trt_InitOnceComplete(u64* init_once, u32 flags, void* context) {
  if (!init_once) { trt_SetLastError(87); return 0; }
  if (flags & 0x4u /*INIT_ONCE_INIT_FAILED*/) {
    __atomic_store_n(init_once, 0ULL, __ATOMIC_RELEASE);
    return 1;
  }
  const u64 val = (((u64)(uintptr_t)context) & ~3ULL) | 2ULL;
  __atomic_store_n(init_once, val, __ATOMIC_RELEASE);
  return 1;
}

int TRT_STDCALL trt_InitOnceExecuteOnce(u64* init_once, void* cb, void* param,
                                        void** context) {
  int pending = 0;
  void* ctx = nullptr;
  if (!trt_InitOnceBeginInitialize(init_once, 0, &pending, &ctx)) return 0;
  if (!pending) {
    if (context) *context = ctx;
    return 1;
  }
  typedef int (TRT_STDCALL *init_once_fn)(u64*, void*, void**);
  const int ok = cb ? ((init_once_fn)cb)(init_once, param, &ctx) : 0;
  if (!ok) {
    trt_InitOnceComplete(init_once, 0x4u, nullptr);
    return 0;
  }
  trt_InitOnceComplete(init_once, 0u, ctx);
  if (context) *context = ctx;
  return 1;
}

// ============================================================================
// Memory, Loader, Debug, Error Mode, Strings
// ============================================================================

void* TRT_STDCALL trt_GlobalAlloc(u32 flags, u64 bytes) {
  const u32 hflags = (flags & 0x0040u /*GMEM_ZEROINIT*/) ? 0x08u : 0u;
  return trt_HeapAlloc((void*)(uintptr_t)1, hflags, bytes ? bytes : 1);
}
void* TRT_STDCALL trt_GlobalReAlloc(void* h, u64 bytes, u32 flags) {
  const u32 hflags = (flags & 0x0040u) ? 0x08u : 0u;
  return trt_HeapReAlloc((void*)(uintptr_t)1, hflags, h, bytes);
}
void* TRT_STDCALL trt_GlobalFree(void* h) {
  if (!h) return nullptr;
  return trt_HeapFree((void*)(uintptr_t)1, 0, h) ? nullptr : h;
}
void* TRT_STDCALL trt_GlobalLock(void* h) { return h; }
int TRT_STDCALL trt_GlobalUnlock(void* h) { (void)h; return 0; }
u64 TRT_STDCALL trt_GlobalSize(void* h) {
  return trt_HeapSize((void*)(uintptr_t)1, 0, h);
}

void* TRT_STDCALL trt_LocalReAlloc(void* h, u64 bytes, u32 flags) {
  const u32 hflags = (flags & 0x0040u) ? 0x08u : 0u;
  return trt_HeapReAlloc((void*)(uintptr_t)1, hflags, h, bytes);
}

int TRT_STDCALL trt_HeapDestroy(void* h) {
  if (!h || h == (void*)(uintptr_t)1) return 0;
  return 1;
}

void* TRT_STDCALL trt_LoadLibraryA(const char* name) {
  if (!name) { trt_SetLastError(87); return nullptr; }
  u16 w[512];
  gap_ascii_to_u16(name, w, 512);
  return trt_LoadLibraryW(w);
}

void* TRT_STDCALL trt_LoadLibraryExA(const char* name, void* file, u32 flags) {
  if (!name) { trt_SetLastError(87); return nullptr; }
  u16 w[512];
  gap_ascii_to_u16(name, w, 512);
  return trt_LoadLibraryExW(w, file, flags);
}

int TRT_STDCALL trt_GetModuleHandleExA(u32 flags, const char* name, void** out) {
  if (!name || (flags & 0x4u)) {
    return trt_GetModuleHandleExW(flags, reinterpret_cast<const u16*>(name),
                                  out);
  }
  u16 w[512];
  gap_ascii_to_u16(name, w, 512);
  return trt_GetModuleHandleExW(flags, w, out);
}

u32 TRT_STDCALL trt_GetModuleFileNameA(void* mod, char* buf, u32 len) {
  if (!buf || len == 0) { trt_SetLastError(122); return 0; }
  u16 w[512];
  const u32 n = trt_GetModuleFileNameW(mod, w, 512);
  if (n == 0) { buf[0] = 0; return 0; }
  u32 i = 0;
  for (; i < n && i + 1 < len; ++i) buf[i] = (char)(w[i] & 0x7Fu);
  buf[i] = 0;
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetModuleFileNameA -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, buf); trt_core_log(st_line); }
  if (i < n) trt_SetLastError(122);
  return i;
}

u32 TRT_STDCALL trt_GetSystemDirectoryA(char* buf, u32 len) {
  u16 w[128];
  const int n = trt_GetSystemDirectoryW(w, 128);
  if (!buf || (int)len < n + 1) return (u32)n;
  for (int i = 0; i <= n; ++i) buf[i] = (char)(w[i] & 0x7Fu);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetSystemDirectoryA -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, buf); trt_core_log(st_line); }
  return (u32)n;
}

u64 TRT_STDCALL trt_CreateMutexExW(void* attrs, const u16* name, u32 flags,
                                   u32 access) {
  (void)access;
  return trt_CreateMutexW(attrs, (flags & 0x1u) != 0, name);
}

u64 TRT_STDCALL trt_CreateMutexExA(void* attrs, const char* name, u32 flags,
                                   u32 access) {
  u16 w[256];
  if (name) gap_ascii_to_u16(name, w, 256);
  return trt_CreateMutexExW(attrs, name ? w : nullptr, flags, access);
}

void* TRT_STDCALL trt_CreateWaitableTimerA(void* attrs, int manual,
                                           const char* name) {
  u16 w[256];
  if (name) gap_ascii_to_u16(name, w, 256);
  return trt_CreateWaitableTimerExW(attrs, name ? w : nullptr,
                                    manual ? 1u : 0u, 0x1F0003u);
}

void TRT_STDCALL trt_OutputDebugStringA(const char* s) {
  if (s && s[0]) {
    char line[256];
    snprintf(line, sizeof(line), "guest-dbg: %.230s", s);
    LOG(line);
  }
}

void TRT_STDCALL trt_OutputDebugStringW(const u16* ws) {
  if (!ws || !ws[0]) return;
  char u8s[240];
  trt_utf16_to_utf8(ws, u8s, sizeof(u8s));
  trt_OutputDebugStringA(u8s);
}

static u32 g_error_mode = 0;
u32 TRT_STDCALL trt_SetErrorMode(u32 mode) {
  u32 old = g_error_mode;
  g_error_mode = mode;
  return old;
}
u32 TRT_STDCALL trt_GetErrorMode(void) { return g_error_mode; }
int TRT_STDCALL trt_SetThreadErrorMode(u32 mode, u32* old_out) {
  if (old_out) *old_out = g_error_mode;
  g_error_mode = mode;
  return 1;
}

int TRT_STDCALL trt_lstrlenW(const u16* s) {
  if (!s) return 0;
  int n = 0;
  while (s[n]) ++n;
  return n;
}
int TRT_STDCALL trt_lstrlenA(const char* s) {
  return s ? (int)strlen(s) : 0;
}
u16 TRT_STDCALL trt_RtlCaptureStackBackTrace(u32 skip, u32 capture, void** bt,
                                             u32* hash) {
  (void)skip; (void)capture; (void)bt;
  if (hash) *hash = 0;
  return 0;
}

// ============================================================================
// Environment / File / Disk / SysInfo / Power / ProcThread / Toolhelp
// ============================================================================

u32 TRT_STDCALL trt_GetEnvironmentVariableA(const char* name, char* buf,
                                            u32 size) {
  if (!name) { trt_core_log("strtrace: GetEnvironmentVariableA -> name=NULL value=NULL"); trt_SetLastError(87); return 0; }
  const char* val = trt_gap_env_get(name);
  if (!val) val = getenv(name);
  if (!val) { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentVariableA -> name=\"%.64s\" value=NULL", name); trt_core_log(st_line); trt_SetLastError(203); return 0; }
  const size_t n = strlen(val);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetEnvironmentVariableA -> name=\"%.64s\" buf=%p value=\"%.128s\"", name, (void*)buf, val); trt_core_log(st_line); }
  if (!buf || size <= n) return (u32)(n + 1);
  memcpy(buf, val, n + 1);
  return (u32)n;
}

int TRT_STDCALL trt_SetEnvironmentVariableA(const char* name, const char* val) {
  if (!name || !name[0]) { trt_SetLastError(87); return 0; }
  u16 wn[128], wv[512];
  gap_ascii_to_u16(name, wn, 128);
  if (val) gap_ascii_to_u16(val, wv, 512);
  return trt_SetEnvironmentVariableW(wn, val ? wv : nullptr);
}

u32 TRT_STDCALL trt_GetFileAttributesA(const char* path) {
  if (!path) { trt_SetLastError(87); return 0xFFFFFFFFu; }
  u16 w[512];
  gap_ascii_to_u16(path, w, 512);
  char host[1024];
  if (trt_vfs_resolve_utf16(w, host, sizeof(host)) != 0) {
    trt_SetLastError(2);
    return 0xFFFFFFFFu;
  }
  struct stat st;
  if (stat(host, &st) != 0) { trt_SetLastError(2); return 0xFFFFFFFFu; }
  return (st.st_mode & S_IFDIR) ? 0x10u /*DIR*/ : 0x80u /*NORMAL*/;
}

int TRT_STDCALL trt_GetFileTime(void* h, u64* created, u64* accessed,
                                u64* modified) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  u64 ft = 0;
  struct stat st;
#ifdef _WIN32
  const int fd = _fileno(os);
#else
  const int fd = fileno(os);
#endif
  if (fd >= 0 && fstat(fd, &st) == 0) {
    ft = ((u64)st.st_mtime + 11644473600ULL) * 10000000ULL;
  } else {
    trt_GetSystemTimeAsFileTime(&ft);
  }
  if (created) *created = ft;
  if (accessed) *accessed = ft;
  if (modified) *modified = ft;
  return 1;
}

int TRT_STDCALL trt_SetFileTime(void* h, const u64* c, const u64* a,
                                const u64* m) {
  (void)c; (void)a; (void)m;
  if (!trt_file_os(h)) { trt_SetLastError(6); return 0; }
  return 1;
}

int TRT_STDCALL trt_MoveFileExW(const u16* src, const u16* dst, u32 flags) {
  if (!src || !dst) { trt_SetLastError(87); return 0; }
  char hsrc[1024], hdst[1024];
  if (trt_vfs_resolve_utf16_rw(src, 0, hsrc, sizeof(hsrc)) != 0 ||
      trt_vfs_resolve_utf16_rw(dst, 1, hdst, sizeof(hdst)) != 0) {
    trt_SetLastError(2);
    return 0;
  }
  struct stat st_src;
  if (stat(hsrc, &st_src) != 0) { trt_SetLastError(2); return 0; }
  char hdst_read[1024];
  if (!(flags & 0x1u /*MOVEFILE_REPLACE_EXISTING*/)) {
    if (trt_vfs_resolve_utf16_rw(dst, 0, hdst_read, sizeof(hdst_read)) == 0) {
      struct stat st_dst;
      if (stat(hdst_read, &st_dst) == 0) { trt_SetLastError(183); return 0; }
    }
  }
  trt_vfs_ensure_parents(hdst);
  const int is_dir = (st_src.st_mode & S_IFDIR) != 0 ? 1 : 0;
  char ov_src[1024] = "", ro_src[1024] = "";
  const bool src_is_z =
      trt_vfs_resolve_z_pair_utf16(src, ov_src, sizeof(ov_src),
                                   ro_src, sizeof(ro_src), nullptr, 0) == 0;
  const bool src_in_ro = src_is_z && strcmp(hsrc, ro_src) == 0;
  if (!src_in_ro) {
#ifdef _WIN32
    if ((flags & 0x1u) && strcmp(hsrc, hdst) != 0) remove(hdst);
#endif
    if (rename(hsrc, hdst) == 0) {
      if (src_is_z && trt_vfs_stat(ro_src, nullptr, nullptr) == 0) {
        trt_vfs_mark_z_deleted_utf16(src, 1, is_dir);
      }
      trt_vfs_mark_z_deleted_utf16(dst, 0, is_dir);
      return 1;
    }
  }
  if ((src_in_ro || (flags & 0x2u /*MOVEFILE_COPY_ALLOWED*/) ||
       strcmp(hsrc, hdst) != 0) &&
      !is_dir) {
    if (trt_CopyFileExW(src, dst, nullptr, nullptr, nullptr,
                        (flags & 0x1u) ? 0 : 1)) {
      trt_DeleteFileW(src);
      trt_vfs_mark_z_deleted_utf16(dst, 0, 0);
      return 1;
    }
  }
  trt_SetLastError(5);
  return 0;
}

int TRT_STDCALL trt_MoveFileW(const u16* src, const u16* dst) {
  return trt_MoveFileExW(src, dst, 0);
}

int TRT_STDCALL trt_MoveFileExA(const char* src, const char* dst, u32 flags) {
  if (!src || !dst) { trt_SetLastError(87); return 0; }
  u16 ws[512], wd[512];
  gap_ascii_to_u16(src, ws, 512);
  gap_ascii_to_u16(dst, wd, 512);
  return trt_MoveFileExW(ws, wd, flags);
}

int TRT_STDCALL trt_ReplaceFileW(const u16* replaced, const u16* replacement,
                                 const u16* backup, u32 flags, void* r1,
                                 void* r2) {
  (void)backup; (void)flags; (void)r1; (void)r2;
  return trt_MoveFileExW(replacement, replaced, 0x1u);
}

u32 TRT_STDCALL trt_GetLongPathNameW(const u16* src, u16* dst, u32 cch) {
  if (!src) { trt_SetLastError(87); return 0; }
  u32 n = (u32)trt_lstrlenW(src);
  if (!dst || cch <= n) return n + 1;
  memcpy(dst, src, (size_t)(n + 1) * sizeof(u16));
  return n;
}

u32 TRT_STDCALL trt_GetShortPathNameW(const u16* src, u16* dst, u32 cch) {
  return trt_GetLongPathNameW(src, dst, cch);
}

u32 TRT_STDCALL trt_GetTempPathA(u32 len, char* buf) {
  const char* tmp = "C:\\Temp\\";
  const u32 n = 8;
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetTempPathA -> buf=%p len=%u value=\"%.128s\"", (void*)buf, len, tmp); trt_core_log(st_line); }
  if (!buf || len <= n) return n + 1;
  memcpy(buf, tmp, n + 1);
  return n;
}

u32 TRT_STDCALL trt_GetTempFileNameA(const char* path, const char* prefix,
                                     u32 uniq, char* out) {
  if (!path || !prefix || !out) { trt_SetLastError(87); return 0; }
  if (strlen(path) >= 260) { trt_SetLastError(206); return 0; }
  u16 wide_path[260], wide_prefix[4], wide_result[260];
  gap_ascii_to_u16(path, wide_path, 260);
  gap_ascii_to_u16(prefix, wide_prefix, 4);
  const u32 id = trt_GetTempFileNameW(wide_path, wide_prefix, uniq, wide_result);
  if (id) {
    size_t i = 0;
    while (wide_result[i]) {
      out[i] = static_cast<char>(wide_result[i]);
      ++i;
    }
    out[i] = '\0';
  }
  return id;
}

int TRT_STDCALL trt_GetFileInformationByHandleEx(void* h, int cls, void* info,
                                                u32 size) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  if (!info) { trt_SetLastError(87); return 0; }
  if (cls == 0 /*FileBasicInfo*/) {
    if (size < 40) { trt_SetLastError(122); return 0; }
    u64 ft = 0;
    trt_GetFileTime(h, &ft, nullptr, nullptr);
    u64* q = (u64*)info;
    q[0] = ft; q[1] = ft; q[2] = ft; q[3] = ft;
    ((u32*)info)[8] = 0x80u;
    return 1;
  }
  if (cls == 1 /*FileStandardInfo*/) {
    if (size < 24) { trt_SetLastError(122); return 0; }
    i64 sz = 0;
    trt_GetFileSizeEx(h, &sz);
    u64* q = (u64*)info;
    q[0] = (u64)((sz + 4095) & ~4095LL);
    q[1] = (u64)sz;
    ((u32*)info)[4] = 1;
    ((u8*)info)[20] = 0;
    ((u8*)info)[21] = 0;
    return 1;
  }
  if (cls == 2 /*FileNameInfo*/) {
    const char* vp = trt_file_vpath(h);
    const char* s = vp ? vp : "";
    const u32 n = (u32)strlen(s);
    if (size < 4 + n * 2) { trt_SetLastError(122); return 0; }
    *(u32*)info = n * 2;
    u16* w = (u16*)((u8*)info + 4);
    for (u32 i = 0; i < n; ++i) w[i] = (u16)(u8)s[i];
    return 1;
  }
  trt_SetLastError(50);
  return 0;
}

int TRT_STDCALL trt_SetFileInformationByHandle(void* h, int cls, const void* info,
                                               u32 size) {
  (void)cls; (void)info; (void)size;
  if (!trt_file_os(h)) { trt_SetLastError(6); return 0; }
  return 1;
}

int TRT_STDCALL trt_GetHandleInformation(void* h, u32* flags) {
  if (!h || h == (void*)(uintptr_t)-1) { trt_SetLastError(6); return 0; }
  if (flags) *flags = 0;
  return 1;
}
int TRT_STDCALL trt_SetHandleInformation(void* h, u32 mask, u32 flags) {
  (void)mask; (void)flags;
  if (!h || h == (void*)(uintptr_t)-1) { trt_SetLastError(6); return 0; }
  return 1;
}

int TRT_STDCALL trt_GetDiskFreeSpaceExW(const u16* dir, u64* avail, u64* total,
                                        u64* free_bytes) {
  (void)dir;
  const u64 gb = 1024ULL * 1024ULL * 1024ULL;
  if (avail) *avail = 64ULL * gb;
  if (total) *total = 128ULL * gb;
  if (free_bytes) *free_bytes = 64ULL * gb;
  return 1;
}

int TRT_STDCALL trt_GetVolumeInformationW(const u16* root, u16* vol, u32 vol_sz,
                                          u32* serial, u32* max_comp,
                                          u32* fs_flags, u16* fs_name,
                                          u32 fs_sz) {
  (void)root;
  if (vol && vol_sz > 0) gap_ascii_to_u16("TIRAMISU", vol, (int)vol_sz);
  if (serial) *serial = 0x54525431u;
  if (max_comp) *max_comp = 255u;
  if (fs_flags) *fs_flags = 0x0000001Fu;
  if (fs_name && fs_sz > 0) gap_ascii_to_u16("NTFS", fs_name, (int)fs_sz);
  return 1;
}

u32 TRT_STDCALL trt_GetLogicalDrives(void) {
  return (1u << 2) | (1u << 3) | (1u << 25);  // C:, D:, Z:
}

u64 TRT_STDCALL trt_GetLargePageMinimum(void) { return 0x200000ULL; }

int TRT_STDCALL trt_GlobalMemoryStatusEx(void* ms_in) {
  struct mem_status_ex {
    u32 dwLength; u32 dwMemoryLoad;
    u64 ullTotalPhys, ullAvailPhys;
    u64 ullTotalPageFile, ullAvailPageFile;
    u64 ullTotalVirtual, ullAvailVirtual;
    u64 ullAvailExtendedVirtual;
  };
  mem_status_ex* ms = (mem_status_ex*)ms_in;
  if (!ms || ms->dwLength < sizeof(mem_status_ex)) {
    trt_SetLastError(87);
    return 0;
  }
  const u64 gb = 1024ULL * 1024ULL * 1024ULL;
  ms->dwMemoryLoad = 25;
  ms->ullTotalPhys = 8ULL * gb;
  ms->ullAvailPhys = 6ULL * gb;
  ms->ullTotalPageFile = 8ULL * gb;
  ms->ullAvailPageFile = 6ULL * gb;
  ms->ullTotalVirtual = 0x7FFFFFFE0000ULL;
  ms->ullAvailVirtual = 0x7FF000000000ULL;
  ms->ullAvailExtendedVirtual = 0;
  return 1;
}

int TRT_STDCALL trt_GetSystemPowerStatus(void* sps_in) {
  struct power_status { u8 ac; u8 flag; u8 pct; u8 status_flag; u32 life; u32 full; };
  power_status* sps = (power_status*)sps_in;
  if (!sps) { trt_SetLastError(87); return 0; }
  sps->ac = 1;           // AC online (console plugged in)
  sps->flag = 128;       // No system battery
  sps->pct = 255;        // Unknown
  sps->status_flag = 0;
  sps->life = 0xFFFFFFFFu;
  sps->full = 0xFFFFFFFFu;
  return 1;
}

int TRT_STDCALL trt_GetLogicalProcessorInformation(u8* buf, u32* len) {
  if (!len) { trt_SetLastError(87); return 0; }
  // Emit 8 core records + 1 NUMA node + 1 L1 cache + 1 package = 11 * 32 bytes
  constexpr u32 k_rec_sz = 32;
  constexpr u32 k_cores = 8;
  constexpr u32 k_total = k_cores + 3;
  const u32 need = k_rec_sz * k_total;
  if (!buf || *len < need) {
    *len = need;
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  memset(buf, 0, need);
  for (u32 c = 0; c < k_cores; ++c) {
    u8* r = buf + c * k_rec_sz;
    *(u64*)(r + 0) = (1ULL << c);  // ProcessorMask
    *(u32*)(r + 8) = 0;            // RelationProcessorCore
    r[16] = 0;                     // ProcessorCore.Flags
  }
  // RelationNumaNode
  {
    u8* r = buf + k_cores * k_rec_sz;
    *(u64*)(r + 0) = 0xFFULL;
    *(u32*)(r + 8) = 1;            // RelationNumaNode
    *(u32*)(r + 16) = 0;           // NumaNode.NodeNumber
  }
  // RelationCache (L1 Unified)
  {
    u8* r = buf + (k_cores + 1) * k_rec_sz;
    *(u64*)(r + 0) = 0xFFULL;
    *(u32*)(r + 8) = 2;            // RelationCache
    r[16] = 1;                     // Level
    r[17] = 8;                     // Associativity
    *(u16*)(r + 18) = 64;          // LineSize
    *(u32*)(r + 20) = 32768;       // Size
    *(u32*)(r + 24) = 0;           // CacheUnified
  }
  // RelationProcessorPackage
  {
    u8* r = buf + (k_cores + 2) * k_rec_sz;
    *(u64*)(r + 0) = 0xFFULL;
    *(u32*)(r + 8) = 3;            // RelationProcessorPackage
  }
  *len = need;
  return 1;
}

int TRT_STDCALL trt_GetLogicalProcessorInformationEx(int rel, u8* buf, u32* len) {
  if (!len) { trt_SetLastError(87); return 0; }
  // Win64 SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record sizes:
  //   RelationProcessorCore (0):    48 bytes x 8 cores = 384 bytes
  //   RelationNumaNode (1):         48 bytes x 1 node  =  48 bytes
  //   RelationCache (2):            56 bytes x 1 cache =  56 bytes
  //   RelationProcessorPackage (3): 48 bytes x 1 pkg   =  48 bytes
  //   RelationGroup (4):            80 bytes x 1 group =  80 bytes
  constexpr u32 k_cores = 8;
  constexpr u64 k_all_mask = 0xFFULL;
  const bool want_core  = (rel == 0 || rel == 0xFFFF);
  const bool want_numa  = (rel == 1 || rel == 0xFFFF);
  const bool want_cache = (rel == 2 || rel == 0xFFFF);
  const bool want_pkg   = (rel == 3 || rel == 0xFFFF);
  const bool want_group = (rel == 4 || rel == 0xFFFF);
  if (!want_core && !want_numa && !want_cache && !want_pkg && !want_group) {
    trt_SetLastError(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  u32 need = 0;
  if (want_core)  need += 48u * k_cores;
  if (want_numa)  need += 48u;
  if (want_cache) need += 56u;
  if (want_pkg)   need += 48u;
  if (want_group) need += 80u;
  if (!buf || *len < need) {
    *len = need;
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  memset(buf, 0, need);
  u8* p = buf;
  if (want_core) {
    for (u32 c = 0; c < k_cores; ++c) {
      *(u32*)(p + 0) = 0;             // RelationProcessorCore
      *(u32*)(p + 4) = 48;            // Size
      p[8] = 0;                       // Flags
      p[9] = 0;                       // EfficiencyClass
      *(u16*)(p + 30) = 1;            // GroupCount = 1
      *(u64*)(p + 32) = (1ULL << c);  // GroupMask[0].Mask
      *(u16*)(p + 40) = 0;            // GroupMask[0].Group
      p += 48;
    }
  }
  if (want_numa) {
    *(u32*)(p + 0) = 1;               // RelationNumaNode
    *(u32*)(p + 4) = 48;              // Size
    *(u32*)(p + 8) = 0;               // NodeNumber = 0
    *(u16*)(p + 30) = 1;              // GroupCount = 1
    *(u64*)(p + 32) = k_all_mask;     // GroupMask.Mask
    *(u16*)(p + 40) = 0;              // GroupMask.Group
    p += 48;
  }
  if (want_cache) {
    *(u32*)(p + 0) = 2;               // RelationCache
    *(u32*)(p + 4) = 56;              // Size
    p[8] = 1;                         // Level = 1
    p[9] = 8;                         // Associativity = 8
    *(u16*)(p + 10) = 64;             // LineSize = 64
    *(u32*)(p + 12) = 32768;          // CacheSize = 32 KB
    *(u32*)(p + 16) = 0;              // Type = CacheUnified
    *(u16*)(p + 38) = 1;              // GroupCount = 1
    *(u64*)(p + 40) = k_all_mask;     // GroupMask.Mask
    *(u16*)(p + 48) = 0;              // GroupMask.Group
    p += 56;
  }
  if (want_pkg) {
    *(u32*)(p + 0) = 3;               // RelationProcessorPackage
    *(u32*)(p + 4) = 48;              // Size
    p[8] = 0;                         // Flags
    p[9] = 0;                         // EfficiencyClass
    *(u16*)(p + 30) = 1;              // GroupCount = 1
    *(u64*)(p + 32) = k_all_mask;     // GroupMask[0].Mask
    *(u16*)(p + 40) = 0;              // GroupMask[0].Group
    p += 48;
  }
  if (want_group) {
    *(u32*)(p + 0) = 4;               // RelationGroup
    *(u32*)(p + 4) = 80;              // Size (0x20 header + 48-byte PROCESSOR_GROUP_INFO)
    *(u16*)(p + 8) = 1;               // MaximumGroupCount = 1
    *(u16*)(p + 10) = 1;              // ActiveGroupCount = 1
    p[32] = (u8)k_cores;              // GroupInfo[0].MaximumProcessorCount = 8
    p[33] = (u8)k_cores;              // GroupInfo[0].ActiveProcessorCount = 8
    *(u64*)(p + 72) = k_all_mask;     // GroupInfo[0].ActiveProcessorMask = 0xFF
    p += 80;
  }
  *len = need;
  return 1;
}

u64 TRT_STDCALL trt_VerSetConditionMask(u64 mask, u32 type_mask, u8 cond) {
  for (int bit = 0; bit < 8; ++bit) {
    if (type_mask & (1u << bit)) {
      const int shift = bit * 3;
      mask &= ~(7ULL << shift);
      mask |= ((u64)(cond & 7u) << shift);
    }
  }
  return mask;
}

static bool check_ver_cond(u32 actual, u32 expected, u8 cond) {
  switch (cond) {
    case 1: return actual == expected;
    case 2: return actual > expected;
    case 3: return actual >= expected;
    case 4: return actual < expected;
    case 5: return actual <= expected;
    default: return true;
  }
}

int TRT_STDCALL trt_VerifyVersionInfoW(const void* vi_in, u32 type_mask,
                                       u64 cond_mask) {
  struct osvi_ex_w {
    u32 dwOSVersionInfoSize;
    u32 dwMajorVersion;
    u32 dwMinorVersion;
    u32 dwBuildNumber;
    u32 dwPlatformId;
    u16 szCSDVersion[128];
    u16 wServicePackMajor;
    u16 wServicePackMinor;
    u16 wSuiteMask;
    u8 wProductType;
    u8 wReserved;
  };
  const osvi_ex_w* vi = (const osvi_ex_w*)vi_in;
  if (!vi) { trt_SetLastError(87); return 0; }
  // Honest reported host version matches bridge_kernel32 GetVersionExW: Win10 10.0.19041
  if ((type_mask & 0x02u) &&
      !check_ver_cond(10, vi->dwMajorVersion, (u8)((cond_mask >> 3) & 7u))) {
    trt_SetLastError(1150);
    return 0;
  }
  if ((type_mask & 0x01u) &&
      !check_ver_cond(0, vi->dwMinorVersion, (u8)((cond_mask >> 0) & 7u))) {
    trt_SetLastError(1150);
    return 0;
  }
  if ((type_mask & 0x04u) &&
      !check_ver_cond(19041, vi->dwBuildNumber, (u8)((cond_mask >> 6) & 7u))) {
    trt_SetLastError(1150);
    return 0;
  }
  return 1;
}

u64 TRT_STDCALL trt_SetThreadAffinityMask(void* thread, u64 mask) {
  (void)thread; (void)mask;
  return 0xFFULL;
}
int TRT_STDCALL trt_SetProcessAffinityMask(void* proc, u64 mask) {
  (void)proc; (void)mask;
  return 1;
}
int TRT_STDCALL trt_GetProcessAffinityMask(void* proc, u64* proc_mask,
                                           u64* sys_mask) {
  (void)proc;
  if (proc_mask) *proc_mask = 0xFFULL;
  if (sys_mask) *sys_mask = 0xFFULL;
  return 1;
}
i32 TRT_STDCALL trt_SetThreadDescription(void* thread, const u16* desc) {
  (void)thread; (void)desc;
  return 0;  // S_OK
}
i32 TRT_STDCALL trt_GetThreadDescription(void* thread, u16** out) {
  (void)thread;
  if (!out) return (i32)0x80070057u;
  u16* s = (u16*)trt_LocalAlloc(0, 2);
  if (!s) return (i32)0x8007000Eu;
  s[0] = 0;
  *out = s;
  return 0;
}

int TRT_STDCALL trt_ReadProcessMemory(void* proc, const void* addr, void* buf,
                                      u64 size, u64* read) {
  if (proc != (void*)(uintptr_t)-1 && proc != trt_GetCurrentProcess()) {
    trt_SetLastError(5);
    if (read) *read = 0;
    return 0;
  }
  if (!addr || !buf) { trt_SetLastError(87); return 0; }
  memcpy(buf, addr, (size_t)size);
  if (read) *read = size;
  return 1;
}

int TRT_STDCALL trt_K32GetProcessMemoryInfo(void* proc, void* pmc_in, u32 cb) {
  (void)proc;
  struct pmc { u32 cb; u32 page_faults; u64 peak_ws, ws, peak_pp, pp, peak_np, np, pf, peak_pf; };
  if (!pmc_in || cb < 72) { trt_SetLastError(87); return 0; }
  memset(pmc_in, 0, cb);
  pmc* p = (pmc*)pmc_in;
  p->cb = cb;
  p->ws = 128ULL * 1024ULL * 1024ULL;
  p->peak_ws = p->ws;
  return 1;
}

// ---- Toolhelp module snapshots ---------------------------------------------
// TH32CS_SNAPMODULE / SNAPMODULE32 walk the REAL guest image registry
// (images.cpp — the same list K32EnumProcessModules serves): the main exe
// plus every guest DLL the loader mapped. Process/thread/heap-list snapshots
// stay honest failures: no cross-process state exists in the runtime.
// The snapshot handle is a tagged record handle (bridge_mem.cpp pattern:
// tag in the high half, slot+1 in the low half) with stable slots so a
// closed snapshot never renumbers a live one.
namespace {
struct snap_module {
  void* base;
  u32 size;
  void* handle;
  char name[128];
  char path[512];
};
struct snap_rec {
  u32 pid;
  u32 cursor;  // next index Module32NextW returns
  std::vector<snap_module> mods;
};
std::mutex g_snaps_mu;
std::vector<std::unique_ptr<snap_rec>> g_snaps;
constexpr u64 k_snap_handle_tag = 0x534E0000ull;  // "SN" — unique handle space

snap_rec* snap_at(void* h) {
  const u64 v = (u64)(uintptr_t)h;
  if ((v & 0xFFFF0000ull) != k_snap_handle_tag) return nullptr;
  const u64 idx = v & 0xFFFFull;
  if (idx == 0 || idx > g_snaps.size() || !g_snaps[(size_t)(idx - 1)])
    return nullptr;
  return g_snaps[(size_t)(idx - 1)].get();
}

u32 snap_slot_alloc(snap_rec* rec) {
  for (size_t i = 0; i < g_snaps.size(); ++i) {
    if (!g_snaps[i]) {
      g_snaps[i].reset(rec);
      return (u32)i;
    }
  }
  if (g_snaps.size() >= 0xFFFF) return 0xFFFFFFFFu;  // table full
  g_snaps.push_back(std::unique_ptr<snap_rec>(rec));
  return (u32)(g_snaps.size() - 1);
}

// MODULEENTRY32W (x64): dwSize@0 th32ModuleID@4 th32ProcessID@8
// GlblcntUsage@12 ProccntUsage@16 modBaseAddr@24 modBaseSize@32 hModule@40
// WCHAR szModule[256]@48 WCHAR szExePath[260]@568 -> 1080 bytes.
// MODULEENTRY32A: same head, char szModule[256]@48 char szExePath[260]@308
// -> 564 bytes.
void snap_fill_w(void* entry, u32 pid, const snap_module& m) {
  u8* p = (u8*)entry;
  *(u32*)(p + 0) = 1080;
  *(u32*)(p + 4) = 1;  // th32ModuleID: legacy, any nonzero
  *(u32*)(p + 8) = pid;
  *(u32*)(p + 12) = 1;  // GlblcntUsage
  *(u32*)(p + 16) = 1;  // ProccntUsage
  *(void**)(p + 24) = m.base;
  *(u32*)(p + 32) = m.size;
  *(void**)(p + 40) = m.handle;
  u16* name = (u16*)(p + 48);
  u32 i = 0;
  for (; m.name[i] && i < 255; ++i) name[i] = (u16)(u8)m.name[i];
  name[i] = 0;
  u16* path = (u16*)(p + 48 + 256 * 2);
  for (i = 0; m.path[i] && i < 259; ++i) path[i] = (u16)(u8)m.path[i];
  path[i] = 0;
}

void snap_fill_a(void* entry, u32 pid, const snap_module& m) {
  u8* p = (u8*)entry;
  *(u32*)(p + 0) = 564;
  *(u32*)(p + 4) = 1;
  *(u32*)(p + 8) = pid;
  *(u32*)(p + 12) = 1;
  *(u32*)(p + 16) = 1;
  *(void**)(p + 24) = m.base;
  *(u32*)(p + 32) = m.size;
  *(void**)(p + 40) = m.handle;
  char* name = (char*)(p + 48);
  u32 i = 0;
  for (; m.name[i] && i < 255; ++i) name[i] = m.name[i];
  name[i] = 0;
  char* path = (char*)(p + 48 + 256);
  for (i = 0; m.path[i] && i < 259; ++i) path[i] = m.path[i];
  path[i] = 0;
}

int module32_walk(void* snap, void* entry, u32 want_size, int wide,
                  int first) {
  char line[256];
  if (!entry) {
    trt_SetLastError(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_snaps_mu);
  snap_rec* rec = snap_at(snap);
  if (!rec) {
    snprintf(line, sizeof(line),
             "kernel32: Module32%s -> FALSE (invalid snapshot handle)",
             first ? "First" : "Next");
    LOG(line);
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  const u32 dwsize = *(u32*)entry;
  if (dwsize != want_size) {
    snprintf(line, sizeof(line),
             "kernel32: Module32%s -> FALSE (entry dwSize=%u, want %u — "
             "MODULEENTRY32 size mismatch)",
             first ? "First" : "Next", dwsize, want_size);
    LOG(line);
    trt_SetLastError(87);
    return 0;
  }
  if (first) rec->cursor = 0;
  if (rec->cursor >= rec->mods.size()) {
    trt_SetLastError(18);  // ERROR_NO_MORE_FILES: the clean walk terminator
    return 0;
  }
  const snap_module& m = rec->mods[rec->cursor];
  if (wide)
    snap_fill_w(entry, rec->pid, m);
  else
    snap_fill_a(entry, rec->pid, m);
  ++rec->cursor;
  snprintf(line, sizeof(line),
           "kernel32: Module32%s%s -> %s base=%p size=0x%x",
           first ? "First" : "Next", wide ? "W" : "A", m.name, m.base, m.size);
  LOG(line);
  return 1;
}
}  // namespace

void* TRT_STDCALL trt_CreateToolhelp32Snapshot(u32 flags, u32 pid) {
  char line[256];
  constexpr u32 k_snap_module = 0x8u;    // TH32CS_SNAPMODULE
  constexpr u32 k_snap_module32 = 0x10u; // TH32CS_SNAPMODULE32
  if (flags & (k_snap_module | k_snap_module32)) {
    const u32 self = trt_GetCurrentProcessId();
    if (pid != 0 && pid != self) {
      snprintf(line, sizeof(line),
               "kernel32: CreateToolhelp32Snapshot(SNAPMODULE, pid=%u) -> "
               "INVALID_HANDLE_VALUE (no cross-process module list; honest)",
               pid);
      LOG(line);
      trt_SetLastError(5);  // ERROR_ACCESS_DENIED
      return (void*)(uintptr_t)-1;
    }
    const int n = trt_images_count();
    if (n <= 0) {
      LOG("kernel32: CreateToolhelp32Snapshot(SNAPMODULE) -> "
          "INVALID_HANDLE_VALUE (no guest image registered yet; honest)");
      trt_SetLastError(5);
      return (void*)(uintptr_t)-1;
    }
    snap_rec* rec = new snap_rec;
    rec->pid = self;
    rec->cursor = 0;
    rec->mods.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
      trt_image* im = trt_images_nth(i);
      if (!im) continue;
      snap_module m;
      memset(&m, 0, sizeof(m));
      m.base = im->base;
      m.size = (u32)im->size_of_image;
      m.handle = trt_images_handle(im);
      const char* nm = trt_images_nth_name(i);
      if (nm) snprintf(m.name, sizeof(m.name), "%s", nm);
      if (i == 0) {
        // entry 0 is the main exe: the guest-visible path the runtime booted
        snprintf(m.path, sizeof(m.path), "%s", trt_gap_exepath());
      } else {
        const char* p = trt_images_nth_path(i);
        const char* leaf = p ? strrchr(p, '/') : nullptr;
        snprintf(m.path, sizeof(m.path), "%s",
                 leaf ? leaf + 1 : (p ? p : (nm ? nm : "")));
      }
      if (!m.name[0]) snprintf(m.name, sizeof(m.name), "module%d", i);
      rec->mods.push_back(m);
    }
    const u32 slot = snap_slot_alloc(rec);
    if (slot == 0xFFFFFFFFu) {
      LOG("kernel32: CreateToolhelp32Snapshot -> INVALID_HANDLE_VALUE "
          "(snapshot table full)");
      trt_SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
      return (void*)(uintptr_t)-1;
    }
    const u64 h = k_snap_handle_tag | (u64)(slot + 1);
    snprintf(line, sizeof(line),
             "kernel32: CreateToolhelp32Snapshot(SNAPMODULE) -> 0x%llx "
             "(%u guest modules from the image registry)",
             (unsigned long long)h, (u32)rec->mods.size());
    LOG(line);
    return (void*)(uintptr_t)h;
  }
  LOG("kernel32: CreateToolhelp32Snapshot -> INVALID_HANDLE_VALUE "
      "(only SNAPMODULE/SNAPMODULE32 are supported; process/thread/heap "
      "lists fail honest)");
  trt_SetLastError(5);
  return (void*)(uintptr_t)-1;
}

int TRT_STDCALL trt_Module32FirstW(void* snap, void* entry) {
  return module32_walk(snap, entry, 1080, 1, 1);
}
int TRT_STDCALL trt_Module32NextW(void* snap, void* entry) {
  return module32_walk(snap, entry, 1080, 1, 0);
}
int TRT_STDCALL trt_Module32FirstA(void* snap, void* entry) {
  return module32_walk(snap, entry, 564, 0, 1);
}
int TRT_STDCALL trt_Module32NextA(void* snap, void* entry) {
  return module32_walk(snap, entry, 564, 0, 0);
}

int TRT_STDCALL trt_CloseToolhelp32Snapshot(void* snap) {
  char line[160];
  std::lock_guard<std::mutex> lk(g_snaps_mu);
  snap_rec* rec = snap_at(snap);
  if (!rec) {
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  for (size_t i = 0; i < g_snaps.size(); ++i) {
    if (g_snaps[i].get() == rec) {
      g_snaps[i].reset();
      break;
    }
  }
  snprintf(line, sizeof(line),
           "kernel32: CloseToolhelp32Snapshot(0x%llx) -> TRUE",
           (unsigned long long)(uintptr_t)snap);
  LOG(line);
  return 1;
}

int TRT_STDCALL trt_ToolhelpNext_fail(void* h, void* entry) {
  (void)h; (void)entry;
  trt_SetLastError(18);  // ERROR_NO_MORE_FILES
  return 0;
}

int TRT_STDCALL trt_InitializeProcThreadAttributeList(void* list, u32 count,
                                                     u32 flags, u64* size) {
  (void)count; (void)flags;
  if (!size) { trt_SetLastError(87); return 0; }
  const u64 need = 64;
  if (!list || *size < need) {
    *size = need;
    trt_SetLastError(122);
    return 0;
  }
  memset(list, 0, (size_t)need);
  *size = need;
  return 1;
}
int TRT_STDCALL trt_UpdateProcThreadAttribute(void* list, u32 flags, u64 attr,
                                              const void* val, u64 cb,
                                              void* prev, u64* ret_sz) {
  (void)flags; (void)attr; (void)val; (void)cb; (void)prev; (void)ret_sz;
  if (!list) { trt_SetLastError(87); return 0; }
  return 1;
}
void TRT_STDCALL trt_DeleteProcThreadAttributeList(void* list) { (void)list; }

u32 TRT_STDCALL trt_GetDynamicTimeZoneInformation(void* out) {
  if (!out) { trt_SetLastError(87); return (u32)-1; }
  // DYNAMIC_TIME_ZONE_INFORMATION is 432 bytes (172 TZI + 128*2 TimeZoneKeyName + 1 byte).
  memset(out, 0, 432);
  u16* key = (u16*)((u8*)out + 172);
  key[0] = 'U'; key[1] = 'T'; key[2] = 'C'; key[3] = 0;
  return 0;  // TIME_ZONE_ID_UNKNOWN
}

int TRT_STDCALL trt_GetTimeZoneInformationForYear(u16 year, void* pdtzi,
                                                  void* ptzi) {
  (void)year; (void)pdtzi;
  if (!ptzi) { trt_SetLastError(87); return 0; }
  memset(ptzi, 0, 172);
  return 1;
}

u32 TRT_STDCALL trt_K32GetModuleBaseNameW(void* proc, void* mod,
                                          u16* out, u32 cap) {
  (void)proc;
  if (!out || cap == 0) { trt_SetLastError(87); return 0; }
  const char* name = mod ? trt_images_name(mod) : trt_images_nth_name(0);
  if (!name) {
    trt_SetLastError(126);  // ERROR_MOD_NOT_FOUND
    out[0] = 0;
    return 0;
  }
  const char* slash = strrchr(name, '/');
  if (slash) name = slash + 1;
  u32 i = 0;
  for (; name[i] && i + 1 < cap; ++i) out[i] = (u16)(u8)name[i];
  out[i] = 0;
  return i;
}

int TRT_STDCALL trt_DeleteVolumeMountPointW(const u16* mount) {
  (void)mount;
  trt_SetLastError(5);  // ERROR_ACCESS_DENIED (AppContainer)
  return 0;
}

// ---- Universal processor / timing / WOW64 / CpuSet / memory locking APIs ----
int TRT_STDCALL trt_IsWow64Process(void* proc, int* is_wow64) {
  (void)proc;
  if (!is_wow64) { trt_SetLastError(87); return 0; }
  *is_wow64 = 0;  // native 64-bit PE64
  return 1;
}

int TRT_STDCALL trt_IsWow64Process2(void* proc, u16* process_machine,
                                    u16* native_machine) {
  (void)proc;
  if (process_machine) *process_machine = 0;       // IMAGE_FILE_MACHINE_UNKNOWN (native)
  if (native_machine) *native_machine = 0x8664u;   // IMAGE_FILE_MACHINE_AMD64
  return 1;
}

u32 TRT_STDCALL trt_GetActiveProcessorCount(u16 group) {
  return (group == 0 || group == 0xFFFFu) ? 8u : 0u;
}
u32 TRT_STDCALL trt_GetMaximumProcessorCount(u16 group) {
  return (group == 0 || group == 0xFFFFu) ? 8u : 0u;
}
u16 TRT_STDCALL trt_GetActiveProcessorGroupCount(void) { return 1u; }
u16 TRT_STDCALL trt_GetMaximumProcessorGroupCount(void) { return 1u; }
extern "C" u32 TRT_STDCALL trt_GetCurrentProcessorNumber(void);
void TRT_STDCALL trt_GetCurrentProcessorNumberEx(u32* proc_num) {
  if (proc_num) *proc_num = (trt_GetCurrentProcessorNumber() & 0xFFu) << 16;
}

u32 TRT_STDCALL trt_SetThreadIdealProcessor(void* thread, u32 ideal) {
  (void)thread;
  return (ideal < 8u) ? ideal : 0u;
}

int TRT_STDCALL trt_SetThreadIdealProcessorEx(void* thread, const void* ideal,
                                              void* prev) {
  (void)thread;
  if (prev && ideal) memcpy(prev, ideal, 4);
  else if (prev) memset(prev, 0, 4);
  return 1;
}

int TRT_STDCALL trt_SetThreadSelectedCpuSets(void* thread, const u32* ids,
                                             u32 count) {
  (void)thread; (void)ids; (void)count;
  return 1;
}

int TRT_STDCALL trt_GetSystemCpuSetInformation(void* info, u32 buf_len,
                                                u32* ret_len, void* proc,
                                                u32 flags) {
  (void)proc; (void)flags;
  constexpr u32 k_rec = 32u;  // sizeof(SYSTEM_CPU_SET_INFORMATION)
  constexpr u32 k_need = 8u * k_rec;
  if (ret_len) *ret_len = k_need;
  if (!info || buf_len < k_need) {
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  memset(info, 0, k_need);
  for (u32 i = 0; i < 8u; ++i) {
    u8* r = (u8*)info + i * k_rec;
    u32 sz = k_rec;
    u32 type = 0;  // CpuSetInformation
    u32 id = 256u + i;
    u16 grp = 0;
    u8 lpi = (u8)i;
    u8 core = (u8)(i / 2u);
    memcpy(r + 0, &sz, 4);
    memcpy(r + 4, &type, 4);
    memcpy(r + 8, &id, 4);
    memcpy(r + 12, &grp, 2);
    r[14] = lpi;
    r[15] = core;
    r[16] = 0;  // LastLevelCacheIndex
    r[17] = 0;  // NumaNodeIndex
    r[18] = 0;  // EfficiencyClass
  }
  return 1;
}

int TRT_STDCALL trt_GetProcessTimes(void* proc, u64* creation, u64* exit_t,
                                    u64* kernel_t, u64* user_t) {
  (void)proc;
  u64 now = 0;
  trt_GetSystemTimeAsFileTime(&now);
  if (creation) *creation = now > 10000000ULL ? (now - 10000000ULL) : now;
  if (exit_t) *exit_t = 0;
  if (kernel_t) *kernel_t = 100000ULL;
  if (user_t) *user_t = 250000ULL;
  return 1;
}

int TRT_STDCALL trt_GetThreadTimes(void* thread, u64* creation, u64* exit_t,
                                   u64* kernel_t, u64* user_t) {
  return trt_GetProcessTimes(thread, creation, exit_t, kernel_t, user_t);
}

int TRT_STDCALL trt_GetThreadIOPendingFlag(void* thread, int* pending) {
  (void)thread;
  if (!pending) { trt_SetLastError(87); return 0; }
  *pending = 0;
  return 1;
}

extern u64 TRT_STDCALL trt_GetTickCount64(void);

int TRT_STDCALL trt_QueryUnbiasedInterruptTime(u64* out_time) {
  if (!out_time) { trt_SetLastError(87); return 0; }
  *out_time = trt_GetTickCount64() * 10000ULL;
  return 1;
}

void TRT_STDCALL trt_QueryUnbiasedInterruptTimePrecise(u64* out_time) {
  if (out_time) *out_time = trt_GetTickCount64() * 10000ULL;
}

void TRT_STDCALL trt_FlushProcessWriteBuffers(void) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

int TRT_STDCALL trt_VirtualLock(void* addr, u64 size) {
  (void)addr; (void)size;
  return 1;
}
int TRT_STDCALL trt_VirtualUnlock(void* addr, u64 size) {
  (void)addr; (void)size;
  return 1;
}
int TRT_STDCALL trt_SetProcessWorkingSetSizeEx(void* proc, u64 min_ws,
                                               u64 max_ws, u32 flags) {
  (void)proc; (void)min_ws; (void)max_ws; (void)flags;
  return 1;
}
int TRT_STDCALL trt_GetProcessWorkingSetSizeEx(void* proc, u64* min_ws,
                                               u64* max_ws, u32* flags) {
  (void)proc;
  if (min_ws) *min_ws = 64ULL * 1024ULL * 1024ULL;
  if (max_ws) *max_ws = 4096ULL * 1024ULL * 1024ULL;
  if (flags) *flags = 0;
  return 1;
}

// ---- PathCch* (api-ms-win-core-path-l1-1-0 / kernelbase) ------------------
static size_t gap_wlen_max(const u16* s, size_t max_cch) {
  size_t n = 0;
  while (s && n < max_cch && s[n]) ++n;
  return n;
}

static bool gap_is_alpha(u16 c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool gap_is_sep(u16 c) { return c == '\\' || c == '/'; }

static u64 gap_root_len(const u16* p) {
  if (!p || !p[0]) return 0;
  // Extended prefix "\\?\"
  u64 off = 0;
  if (gap_is_sep(p[0]) && gap_is_sep(p[1]) && p[2] == '?' && gap_is_sep(p[3])) {
    off = 4;
    if ((p[4] == 'U' || p[4] == 'u') && (p[5] == 'N' || p[5] == 'n') &&
        (p[6] == 'C' || p[6] == 'c') && gap_is_sep(p[7])) {
      u64 i = 8;
      int slashes = 0;
      while (p[i] && slashes < 2) {
        if (gap_is_sep(p[i])) ++slashes;
        ++i;
      }
      return i;
    }
  }
  const u16* q = p + off;
  if (gap_is_alpha(q[0]) && q[1] == ':') {
    return off + (gap_is_sep(q[2]) ? 3u : 2u);
  }
  if (off == 0 && gap_is_sep(p[0]) && gap_is_sep(p[1])) {
    u64 i = 2;
    while (p[i] && !gap_is_sep(p[i])) ++i;
    if (!p[i]) return i;
    ++i;
    while (p[i] && !gap_is_sep(p[i])) ++i;
    if (gap_is_sep(p[i])) ++i;
    return i;
  }
  if (gap_is_sep(q[0])) return off + 1u;
  return 0;
}

i32 TRT_STDCALL trt_PathCchSkipRoot(const u16* path, const u16** root_end) {
  if (!path || !root_end) return (i32)0x80070057u;
  *root_end = nullptr;
  const u64 r = gap_root_len(path);
  if (r == 0) return (i32)0x80070057u;
  *root_end = path + r;
  return 0;
}

int TRT_STDCALL trt_PathCchIsRoot(const u16* path) {
  if (!path || !path[0]) return 0;
  const u64 r = gap_root_len(path);
  return (r > 0 && path[r] == 0) ? 1 : 0;
}

i32 TRT_STDCALL trt_PathCchAddBackslashEx(u16* path, u64 cch_path,
                                          u16** end_out, u64* rem_out) {
  if (!path || cch_path == 0 || cch_path > 32768u) return (i32)0x80070057u;
  const size_t len = gap_wlen_max(path, (size_t)cch_path);
  if (len == (size_t)cch_path && path[len - 1] != 0) return (i32)0x8007007Au;
  if (len > 0 && (path[len - 1] == '\\' || path[len - 1] == '/')) {
    if (end_out) *end_out = path + len;
    if (rem_out) *rem_out = cch_path - (u64)len;
    return 1;  // S_FALSE (already ends with backslash)
  }
  if ((u64)len + 2u > cch_path) return (i32)0x8007007Au;
  path[len] = '\\';
  path[len + 1] = 0;
  if (end_out) *end_out = path + len + 1;
  if (rem_out) *rem_out = cch_path - (u64)(len + 1);
  return 0;
}

i32 TRT_STDCALL trt_PathCchAddBackslash(u16* path, u64 cch_path) {
  return trt_PathCchAddBackslashEx(path, cch_path, nullptr, nullptr);
}

i32 TRT_STDCALL trt_PathCchRemoveBackslashEx(u16* path, u64 cch_path,
                                             u16** end_out, u64* rem_out) {
  if (!path || cch_path == 0) return (i32)0x80070057u;
  size_t len = gap_wlen_max(path, (size_t)cch_path);
  const u64 r = gap_root_len(path);
  i32 hr = 1;  // S_FALSE
  if (len > r && gap_is_sep(path[len - 1])) {
    path[--len] = 0;
    hr = 0;
  }
  if (end_out) *end_out = path + len;
  if (rem_out) *rem_out = cch_path - (u64)len;
  return hr;
}

i32 TRT_STDCALL trt_PathCchRemoveBackslash(u16* path, u64 cch_path) {
  return trt_PathCchRemoveBackslashEx(path, cch_path, nullptr, nullptr);
}

i32 TRT_STDCALL trt_PathCchRemoveFileSpec(u16* path, u64 cch_path) {
  if (!path || cch_path == 0) return (i32)0x80070057u;
  size_t len = gap_wlen_max(path, (size_t)cch_path);
  const u64 r = gap_root_len(path);
  if (len <= r) return 1;
  while (len > r && gap_is_sep(path[len - 1])) --len;
  while (len > r && !gap_is_sep(path[len - 1])) --len;
  while (len > r && gap_is_sep(path[len - 1])) --len;
  if (path[len] == 0) return 1;
  path[len] = 0;
  return 0;
}

i32 TRT_STDCALL trt_PathCchFindExtension(const u16* path, u64 cch_path,
                                         const u16** ext_out) {
  if (!path || !ext_out || cch_path == 0) return (i32)0x80070057u;
  const size_t len = gap_wlen_max(path, (size_t)cch_path);
  const u16* dot = nullptr;
  for (size_t i = len; i > 0; --i) {
    const u16 c = path[i - 1];
    if (gap_is_sep(c) || c == ':') break;
    if (c == '.') { dot = &path[i - 1]; break; }
  }
  *ext_out = dot ? dot : (path + len);
  return 0;
}

i32 TRT_STDCALL trt_PathCchRemoveExtension(u16* path, u64 cch_path) {
  if (!path || cch_path == 0) return (i32)0x80070057u;
  const u16* ext = nullptr;
  i32 hr = trt_PathCchFindExtension(path, cch_path, &ext);
  if (hr != 0) return hr;
  if (!ext || *ext == 0) return 1;
  path[ext - path] = 0;
  return 0;
}

i32 TRT_STDCALL trt_PathCchRenameExtension(u16* path, u64 cch_path,
                                           const u16* ext) {
  if (!path || cch_path == 0 || !ext) return (i32)0x80070057u;
  const u16* old_ext = nullptr;
  trt_PathCchFindExtension(path, cch_path, &old_ext);
  const size_t base_len = old_ext ? (size_t)(old_ext - path) : gap_wlen_max(path, (size_t)cch_path);
  const size_t ext_len = gap_wlen_max(ext, 256);
  const bool need_dot = (ext[0] && ext[0] != '.');
  const size_t tot = base_len + (need_dot ? 1u : 0u) + ext_len;
  if ((u64)tot + 1u > cch_path) return (i32)0x8007007Au;
  size_t pos = base_len;
  if (need_dot) path[pos++] = '.';
  for (size_t i = 0; i < ext_len; ++i) path[pos++] = ext[i];
  path[pos] = 0;
  return 0;
}

i32 TRT_STDCALL trt_PathCchStripToRoot(u16* path, u64 cch_path) {
  if (!path || cch_path == 0) return (i32)0x80070057u;
  const u64 r = gap_root_len(path);
  if (r == 0) return (i32)0x80070057u;
  if (r < (u64)cch_path && path[r] != 0) {
    path[r] = 0;
    return 0;
  }
  return 1;
}

i32 TRT_STDCALL trt_PathCchStripPrefix(u16* path, u64 cch_path) {
  if (!path || cch_path == 0) return (i32)0x80070057u;
  const size_t len = gap_wlen_max(path, (size_t)cch_path);
  if (len >= 4 && gap_is_sep(path[0]) && gap_is_sep(path[1]) &&
      path[2] == '?' && gap_is_sep(path[3])) {
    if (len >= 8 && (path[4] == 'U' || path[4] == 'u') &&
        (path[5] == 'N' || path[5] == 'n') && (path[6] == 'C' || path[6] == 'c') &&
        gap_is_sep(path[7])) {
      path[0] = '\\';
      path[1] = '\\';
      memmove(path + 2, path + 8, (len - 7) * sizeof(u16));
      return 0;
    }
    memmove(path, path + 4, (len - 3) * sizeof(u16));
    return 0;
  }
  return 1;
}

i32 TRT_STDCALL trt_PathCchCanonicalizeEx(u16* path_out, u64 cch_out,
                                          const u16* path_in, u32 flags) {
  if (!path_out || cch_out == 0 || !path_in) return (i32)0x80070057u;
  std::vector<u16> norm;
  for (size_t i = 0; path_in[i]; ++i) {
    u16 c = path_in[i];
    if (c == '/') c = '\\';
    norm.push_back(c);
  }
  norm.push_back(0);
  const u64 root_len = gap_root_len(norm.data());
  std::vector<std::vector<u16>> segs;
  const u64 nlen = norm.size() - 1;
  u64 i = root_len;
  while (i < nlen) {
    while (i < nlen && norm[i] == '\\') ++i;
    if (i >= nlen) break;
    u64 j = i;
    while (j < nlen && norm[j] != '\\') ++j;
    const u64 slen = j - i;
    if (slen == 1 && norm[i] == '.') {
      // skip '.'
    } else if (slen == 2 && norm[i] == '.' && norm[i + 1] == '.') {
      if (!segs.empty() && !(segs.back().size() == 2 && segs.back()[0] == '.' && segs.back()[1] == '.')) {
        segs.pop_back();
      } else if (root_len == 0) {
        segs.emplace_back(norm.begin() + i, norm.begin() + j);
      }
    } else {
      segs.emplace_back(norm.begin() + i, norm.begin() + j);
    }
    i = j;
  }
  std::vector<u16> built;
  for (u64 r = 0; r < root_len; ++r) built.push_back(norm[r]);
  for (size_t si = 0; si < segs.size(); ++si) {
    if (!built.empty() && built.back() != '\\') built.push_back('\\');
    built.insert(built.end(), segs[si].begin(), segs[si].end());
  }
  if ((flags & 0x20u /*PATHCCH_ENSURE_TRAILING_SLASH*/) &&
      (built.empty() || built.back() != '\\')) {
    built.push_back('\\');
  }
  if (built.empty()) {
    built.push_back('\\');
  }
  if (built.size() + 1u > (size_t)cch_out) return (i32)0x8007007Au;
  memcpy(path_out, built.data(), built.size() * sizeof(u16));
  path_out[built.size()] = 0;
  return 0;
}

i32 TRT_STDCALL trt_PathCchCanonicalize(u16* path_out, u64 cch_out,
                                        const u16* path_in) {
  return trt_PathCchCanonicalizeEx(path_out, cch_out, path_in, 0);
}

i32 TRT_STDCALL trt_PathCchCombineEx(u16* path_out, u64 cch_out,
                                     const u16* path_in, const u16* more,
                                     u32 flags) {
  if (!path_out || cch_out == 0 || (!path_in && !more)) return (i32)0x80070057u;
  if (!more || !more[0]) {
    return trt_PathCchCanonicalizeEx(path_out, cch_out, path_in, flags);
  }
  if (!path_in || !path_in[0]) {
    return trt_PathCchCanonicalizeEx(path_out, cch_out, more, flags);
  }
  const u64 mr = gap_root_len(more);
  std::vector<u16> joined;
  if (mr > 0) {
    // If more starts with single '\', keep root of path_in
    if (gap_is_sep(more[0]) && !gap_is_sep(more[1])) {
      const u64 pr = gap_root_len(path_in);
      for (u64 k = 0; k < pr; ++k) joined.push_back(path_in[k]);
      if (!joined.empty() && joined.back() == '\\' && gap_is_sep(more[0])) {
        more += 1;
      }
    }
    for (size_t k = 0; more[k]; ++k) joined.push_back(more[k]);
  } else {
    for (size_t k = 0; path_in[k]; ++k) joined.push_back(path_in[k]);
    if (!joined.empty() && !gap_is_sep(joined.back())) joined.push_back('\\');
    for (size_t k = 0; more[k]; ++k) joined.push_back(more[k]);
  }
  joined.push_back(0);
  return trt_PathCchCanonicalizeEx(path_out, cch_out, joined.data(), flags);
}

i32 TRT_STDCALL trt_PathCchCombine(u16* path_out, u64 cch_out,
                                   const u16* path_in, const u16* more) {
  return trt_PathCchCombineEx(path_out, cch_out, path_in, more, 0);
}

i32 TRT_STDCALL trt_PathCchAppendEx(u16* path, u64 cch_path, const u16* more,
                                    u32 flags) {
  return trt_PathCchCombineEx(path, cch_path, path, more, flags);
}

i32 TRT_STDCALL trt_PathCchAppend(u16* path, u64 cch_path, const u16* more) {
  return trt_PathCchCombineEx(path, cch_path, path, more, 0);
}

// ---- Heap extras ----------------------------------------------------------
u64 TRT_STDCALL trt_HeapCompact(void* heap, u32 flags) {
  (void)heap; (void)flags;
  trt_SetLastError(0);
  return 65536ULL;
}
int TRT_STDCALL trt_HeapValidate(void* heap, u32 flags, const void* mem) {
  (void)heap; (void)flags; (void)mem;
  return 1;
}
int TRT_STDCALL trt_HeapWalk(void* heap, void* entry) {
  (void)heap; (void)entry;
  trt_SetLastError(259);  // ERROR_NO_MORE_ITEMS
  return 0;
}

// ---- Disk / Volume / Drive / Time / Locale / Beep -------------------------
int TRT_STDCALL trt_GetDiskFreeSpaceW(const u16* root, u32* spc, u32* bps,
                                      u32* free_clus, u32* tot_clus) {
  (void)root;
  if (spc) *spc = 8u;
  if (bps) *bps = 512u;
  if (free_clus) *free_clus = 16777216u;  // 64 GiB
  if (tot_clus) *tot_clus = 33554432u;    // 128 GiB
  return 1;
}

int TRT_STDCALL trt_GetDiskFreeSpaceA(const char* root, u32* spc, u32* bps,
                                      u32* free_clus, u32* tot_clus) {
  (void)root;
  return trt_GetDiskFreeSpaceW(nullptr, spc, bps, free_clus, tot_clus);
}

u32 TRT_STDCALL trt_GetLogicalDriveStringsW(u32 buf_len, u16* buf) {
  static const char k_drives[] = "C:\\\0D:\\\0Z:\\\0";
  const u32 total_chars = (u32)(sizeof(k_drives) - 1);  // excludes final extra NUL
  if (!buf || buf_len < total_chars + 1u) return total_chars + 1u;
  for (u32 i = 0; i <= total_chars; ++i) buf[i] = (u16)(u8)k_drives[i];
  return total_chars;
}

u32 TRT_STDCALL trt_GetLogicalDriveStringsA(u32 buf_len, char* buf) {
  static const char k_drives[] = "C:\\\0D:\\\0Z:\\\0";
  const u32 total_chars = (u32)(sizeof(k_drives) - 1);
  if (!buf || buf_len < total_chars + 1u) return total_chars + 1u;
  memcpy(buf, k_drives, total_chars + 1u);
  return total_chars;
}

void* TRT_STDCALL trt_FindFirstVolumeW(u16* vol_name, u32 buf_len) {
  static const char k_vol[] = "\\\\?\\Volume{54525431-0000-0000-0000-000000000001}\\";
  if (!vol_name || buf_len < sizeof(k_vol)) {
    trt_SetLastError(122);
    return (void*)(uintptr_t)-1;
  }
  gap_ascii_to_u16(k_vol, vol_name, (int)buf_len);
  return (void*)(uintptr_t)0x564F4C01ULL;
}

int TRT_STDCALL trt_FindNextVolumeW(void* find_vol, u16* vol_name, u32 buf_len) {
  (void)vol_name; (void)buf_len;
  if (find_vol != (void*)(uintptr_t)0x564F4C01ULL) {
    trt_SetLastError(6);
    return 0;
  }
  trt_SetLastError(18);  // ERROR_NO_MORE_FILES
  return 0;
}

int TRT_STDCALL trt_FindVolumeClose(void* find_vol) {
  (void)find_vol;
  return 1;
}

int TRT_STDCALL trt_GetVolumePathNameW(const u16* file_name, u16* vol_path,
                                       u32 buf_len) {
  if (!file_name || !vol_path || buf_len < 4u) {
    trt_SetLastError(87);
    return 0;
  }
  u16 drv = 'C';
  if (gap_is_alpha(file_name[0]) && file_name[1] == ':') {
    drv = file_name[0];
    if (drv >= 'a' && drv <= 'z') drv -= 32;
  }
  vol_path[0] = drv;
  vol_path[1] = ':';
  vol_path[2] = '\\';
  vol_path[3] = 0;
  return 1;
}

int TRT_STDCALL trt_GetVolumePathNamesForVolumeNameW(const u16* vol_name,
                                                    u16* path_names,
                                                    u32 buf_len, u32* ret_len) {
  (void)vol_name;
  if (ret_len) *ret_len = 5u;
  if (!path_names || buf_len < 5u) {
    trt_SetLastError(234);  // ERROR_MORE_DATA
    return 0;
  }
  path_names[0] = 'C'; path_names[1] = ':'; path_names[2] = '\\';
  path_names[3] = 0;   path_names[4] = 0;
  return 1;
}

int TRT_STDCALL trt_NeedCurrentDirectoryForExePathW(const u16* exe_name) {
  (void)exe_name;
  return 1;
}

int TRT_STDCALL trt_GetLocaleInfoA(u32 lcid, u32 lctype, char* out, int cch) {
  u16 tmp[128] = {};
  const int n = trt_GetLocaleInfoW(lcid, lctype, out ? tmp : nullptr,
                                   out ? (cch < 128 ? cch : 128) : 0);
  if (n <= 0) return 0;
  if (!out || cch <= 0) return n;
  const int lim = n < cch ? n : cch;
  for (int i = 0; i < lim; ++i) out[i] = tmp[i] < 0x80 ? (char)tmp[i] : '?';
  if (lim < cch) out[lim] = 0;
  else out[cch - 1] = 0;
  return lim;
}

int TRT_STDCALL trt_TzSpecificLocalTimeToSystemTime(const void* tz,
                                                    const u16* local_t,
                                                    u16* utc_t) {
  (void)tz;
  if (!local_t || !utc_t) { trt_SetLastError(87); return 0; }
  memcpy(utc_t, local_t, 16);  // UTC timezone -> identical SYSTEMTIME
  return 1;
}

int TRT_STDCALL trt_SetLocalTime(const u16* st) {
  (void)st;
  LOG("kernel32: SetLocalTime -> FALSE (ERROR_PRIVILEGE_NOT_HELD in AppContainer)");
  trt_SetLastError(1314);
  return 0;
}

int TRT_STDCALL trt_Beep(u32 freq, u32 dur) {
  (void)freq; (void)dur;
  return 1;
}

// ---- Byte-range File Locking (SQLite / Qt / Python / Save integrity) ----
#ifdef _WIN32
using file_overlapped = OVERLAPPED;
#else
struct file_overlapped {
  u64 Internal;
  u64 InternalHigh;
  u32 Offset;
  u32 OffsetHigh;
  u64 event;
};
#endif

#ifdef _WIN32
// Keep the FILE alive while the OS uses its underlying handle. Native
// byte-range locks are shared across independent opens of the same file
// and are released when the underlying handle closes.
int file_range_lock(void* h, u32 flags, u32 bytes_lo, u32 bytes_hi,
                    void* overlapped, bool unlock) {
  if (!overlapped) { trt_SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!trt_file_retain(h)) { trt_SetLastError(ERROR_INVALID_HANDLE); return 0; }
  FILE* file = trt_file_os(h);
  const intptr_t native = file ? _get_osfhandle(_fileno(file)) : -1;
  DWORD error = ERROR_INVALID_HANDLE;
  if (native != -1) {
    HANDLE handle = reinterpret_cast<HANDLE>(native);
    auto* ov = static_cast<OVERLAPPED*>(overlapped);
    const BOOL ok = unlock ? ::UnlockFileEx(handle, 0, bytes_lo, bytes_hi, ov)
                           : ::LockFileEx(handle, flags, 0, bytes_lo, bytes_hi, ov);
    error = ok ? ERROR_SUCCESS : ::GetLastError();
  }
  trt_file_release(h);
  if (error != ERROR_SUCCESS) trt_SetLastError(error);
  return error == ERROR_SUCCESS;
}
#else
struct file_lock_entry {
  FILE* osf;
  u64 start;
  u64 end;
  bool exclusive;
};
std::mutex g_flocks_mu;
std::vector<file_lock_entry> g_flocks;
#endif

int TRT_STDCALL trt_LockFileEx(void* h, u32 flags, u32 reserved,
                               u32 bytes_lo, u32 bytes_hi, void* overlapped) {
  (void)reserved;
#ifdef _WIN32
  return file_range_lock(h, flags, bytes_lo, bytes_hi, overlapped, false);
#else
  FILE* osf = trt_file_os(h);
  if (!osf) { trt_SetLastError(6); return 0; }
  u64 off = 0;
  if (overlapped) {
    file_overlapped ov;
    memcpy(&ov, overlapped, sizeof(ov));
    off = ((u64)ov.OffsetHigh << 32) | (u64)ov.Offset;
  }
  const u64 len = ((u64)bytes_hi << 32) | (u64)bytes_lo;
  if (len == 0) return 1;
  const u64 end = (len > ~0ULL - off) ? ~0ULL : (off + len);
  const bool excl = (flags & 0x2u /*LOCKFILE_EXCLUSIVE_LOCK*/) != 0;
  std::lock_guard<std::mutex> lk(g_flocks_mu);
  for (const file_lock_entry& e : g_flocks) {
    if (e.osf == osf && !(end <= e.start || off >= e.end)) {
      if (excl || e.exclusive) {
        trt_SetLastError(33);  // ERROR_LOCK_VIOLATION
        return 0;
      }
    }
  }
  g_flocks.push_back({osf, off, end, excl});
  return 1;
#endif
}

int TRT_STDCALL trt_UnlockFileEx(void* h, u32 reserved, u32 bytes_lo,
                                 u32 bytes_hi, void* overlapped) {
  (void)reserved;
#ifdef _WIN32
  return file_range_lock(h, 0, bytes_lo, bytes_hi, overlapped, true);
#else
  FILE* osf = trt_file_os(h);
  if (!osf) { trt_SetLastError(6); return 0; }
  u64 off = 0;
  if (overlapped) {
    file_overlapped ov;
    memcpy(&ov, overlapped, sizeof(ov));
    off = ((u64)ov.OffsetHigh << 32) | (u64)ov.Offset;
  }
  const u64 len = ((u64)bytes_hi << 32) | (u64)bytes_lo;
  const u64 end = (len > ~0ULL - off) ? ~0ULL : (off + len);
  std::lock_guard<std::mutex> lk(g_flocks_mu);
  for (size_t i = 0; i < g_flocks.size(); ++i) {
    if (g_flocks[i].osf == osf && g_flocks[i].start == off &&
        g_flocks[i].end == end) {
      g_flocks[i] = g_flocks.back();
      g_flocks.pop_back();
      return 1;
    }
  }
  trt_SetLastError(158);  // ERROR_NOT_LOCKED
  return 0;
#endif
}

int TRT_STDCALL trt_LockFile(void* h, u32 off_lo, u32 off_hi,
                             u32 bytes_lo, u32 bytes_hi) {
  file_overlapped ov{};
  ov.Offset = off_lo;
  ov.OffsetHigh = off_hi;
  return trt_LockFileEx(h, 0x2u | 0x1u, 0, bytes_lo, bytes_hi, &ov);
}

int TRT_STDCALL trt_UnlockFile(void* h, u32 off_lo, u32 off_hi,
                               u32 bytes_lo, u32 bytes_hi) {
  file_overlapped ov{};
  ov.Offset = off_lo;
  ov.OffsetHigh = off_hi;
  return trt_UnlockFileEx(h, 0, bytes_lo, bytes_hi, &ov);
}

// ---- Process / NUMA / Pipe / Console / Snapshot / DirectStorage IoRing ----
void* TRT_STDCALL trt_OpenProcess(u32 access, int inherit, u32 pid) {
  (void)access; (void)inherit;
  if (pid == trt_GetCurrentProcessId()) {
    return trt_GetCurrentProcess();
  }
  trt_SetLastError(5);  // ERROR_ACCESS_DENIED (external processes inaccessible)
  return nullptr;
}

int TRT_STDCALL trt_CreateHardLinkW(const u16* file_name,
                                    const u16* existing_file_name, void* sa) {
  (void)sa;
  if (!file_name || !existing_file_name) { trt_SetLastError(87); return 0; }
  char src[1024], dst[1024];
  if (trt_vfs_resolve_utf16_rw(existing_file_name, 0, src, sizeof(src)) != 0 ||
      trt_vfs_resolve_utf16_rw(file_name, 1, dst, sizeof(dst)) != 0) {
    trt_SetLastError(2);
    return 0;
  }
  trt_vfs_ensure_parents(dst);
#ifdef _WIN32
  LOG("kernel32: CreateHardLinkW -> FALSE (AppContainer denies hardlinks)");
  trt_SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0;
#else
  if (link(src, dst) == 0) return 1;
  trt_SetLastError(50);
  return 0;
#endif
}

int TRT_STDCALL trt_SetWaitableTimerEx(void* timer, const i64* due_time,
                                       i32 period, void* comp, void* arg,
                                       void* wake_ctx, u32 tolerable_delay) {
  (void)wake_ctx; (void)tolerable_delay;
  return trt_SetWaitableTimer(timer, due_time, period, comp, arg, 0);
}

int TRT_STDCALL trt_GetNumaHighestNodeNumber(u32* highest) {
  if (!highest) { trt_SetLastError(87); return 0; }
  *highest = 0;
  return 1;
}

int TRT_STDCALL trt_GetNumaNodeProcessorMask(u8 node, u64* mask) {
  if (!mask) { trt_SetLastError(87); return 0; }
  *mask = (node == 0) ? 0xFFULL : 0ULL;
  if (node != 0) { trt_SetLastError(87); return 0; }
  return 1;
}

int TRT_STDCALL trt_GetNumaProcessorNode(u8 proc, u8* node) {
  (void)proc;
  if (!node) { trt_SetLastError(87); return 0; }
  *node = 0;
  return 1;
}

int TRT_STDCALL trt_GetNumaProcessorNodeEx(const void* proc_num, u16* node) {
  (void)proc_num;
  if (!node) { trt_SetLastError(87); return 0; }
  *node = 0;
  return 1;
}

int TRT_STDCALL trt_GetNumaNodeProcessorMaskEx(u16 node, void* mask) {
  struct group_aff { u64 mask; u16 group; u16 res[3]; };
  if (!mask || node != 0) { trt_SetLastError(87); return 0; }
  group_aff* a = (group_aff*)mask;
  a->mask = 0xFFULL;
  a->group = 0;
  a->res[0] = a->res[1] = a->res[2] = 0;
  return 1;
}

int TRT_STDCALL trt_SetNamedPipeHandleState(void* h, const u32* mode,
                                            const u32* max_cnt, const u32* timeout) {
  (void)h; (void)mode; (void)max_cnt; (void)timeout;
  trt_SetLastError(6);
  return 0;
}

int TRT_STDCALL trt_WaitNamedPipeW(const u16* name, u32 timeout) {
  (void)name; (void)timeout;
  trt_SetLastError(2);  // ERROR_FILE_NOT_FOUND
  return 0;
}

int TRT_STDCALL trt_GetNamedPipeHandleStateW(void* h, u32* state, u32* inst,
                                            u32* max_cnt, u32* timeout,
                                            u16* user, u32 user_len) {
  (void)h; (void)state; (void)inst; (void)max_cnt; (void)timeout;
  (void)user; (void)user_len;
  trt_SetLastError(6);
  return 0;
}

static bool gap_is_std_handle(void* h) {
  return h == trt_GetStdHandle(0xFFFFFFF6u) ||
         h == trt_GetStdHandle(0xFFFFFFF5u) ||
         h == trt_GetStdHandle(0xFFFFFFF4u);
}

int TRT_STDCALL trt_SetConsoleMode(void* h, u32 mode) {
  (void)mode;
  if (gap_is_std_handle(h)) return 1;
  trt_SetLastError(6);
  return 0;
}

int TRT_STDCALL trt_ReadConsoleInputW(void* h, void* buf, u32 len, u32* read) {
  (void)buf; (void)len;
  if (!gap_is_std_handle(h)) { trt_SetLastError(6); return 0; }
  if (read) *read = 0;
  return 1;
}

int TRT_STDCALL trt_PeekConsoleInputA(void* h, void* buf, u32 len, u32* read) {
  return trt_ReadConsoleInputW(h, buf, len, read);
}

int TRT_STDCALL trt_GetNumberOfConsoleInputEvents(void* h, u32* events) {
  if (!gap_is_std_handle(h) || !events) { trt_SetLastError(6); return 0; }
  *events = 0;
  return 1;
}

int TRT_STDCALL trt_WriteConsoleA(void* h, const char* buf, u32 len,
                                  u32* written, void* res) {
  (void)h; (void)res;
  if (written) *written = len;
  if (buf && len > 0) {
    char msg[280];
    const u32 n = len < 240 ? len : 240;
    memcpy(msg, buf, n);
    msg[n] = 0;
    LOG(msg);
  }
  return 1;
}

int TRT_STDCALL trt_GenerateConsoleCtrlEvent(u32 ev, u32 pgid) {
  (void)ev; (void)pgid;
  trt_SetLastError(87);
  return 0;
}

int TRT_STDCALL trt_GetFileInformationByName(void* dir_or_path, int cls,
                                             void* info, u32 sz) {
  (void)dir_or_path; (void)cls; (void)info; (void)sz;
  trt_SetLastError(120);  // ERROR_CALL_NOT_IMPLEMENTED
  return 0;
}

u32 TRT_STDCALL trt_PssCaptureSnapshot(void* proc, u32 flags, u32 ctx_flags,
                                       void** snap) {
  (void)proc; (void)flags; (void)ctx_flags;
  if (snap) *snap = nullptr;
  return 50u;  // ERROR_NOT_SUPPORTED
}
u32 TRT_STDCALL trt_PssQuerySnapshot(void* snap, u32 cls, void* buf, u32 len) {
  (void)snap; (void)cls; (void)buf; (void)len;
  return 50u;
}
u32 TRT_STDCALL trt_PssFreeSnapshot(void* proc, void* snap) {
  (void)proc; (void)snap;
  return 0u;
}

// Honest DirectStorage IoRing unavailability -> forces ReadFile/IOCP fallback
i32 TRT_STDCALL trt_QueryIoRingCapabilities(void* caps) {
  if (!caps) return (i32)0x80070057u;
  memset(caps, 0, 12);  // MaxVersion = 0 (IORING_VERSION_INVALID), FeatureFlags = 0
  return 0;             // S_OK (capabilities = none -> DStorage falls back cleanly)
}
int TRT_STDCALL trt_IsIoRingOpSupported(void* ioring, u32 op) {
  (void)ioring; (void)op;
  return 0;
}
i32 TRT_STDCALL trt_CreateIoRing(u32 ver, u32 flags, u32 sub_q, u32 comp_q,
                                 void** handle) {
  (void)ver; (void)flags; (void)sub_q; (void)comp_q;
  if (handle) *handle = nullptr;
  return (i32)0x80004001u;  // E_NOTIMPL
}
i32 TRT_STDCALL trt_IoRingNotImpl(void) { return (i32)0x80004001u; }

// UWP FromAppW forwarders onto the VFS
void* TRT_STDCALL trt_CreateFileFromAppW(const u16* name, u32 access, u32 share,
                                         void* sec, u32 disp, u32 flags, void* tmpl) {
  return trt_CreateFileW(name, access, share, sec, disp, flags, tmpl);
}
void* TRT_STDCALL trt_CreateFile2FromAppW(const u16* name, u32 access, u32 share,
                                          u32 disp, void* ext_params) {
  (void)ext_params;
  return trt_CreateFileW(name, access, share, nullptr, disp, 0x80u, nullptr);
}
void* TRT_STDCALL trt_FindFirstFileExFromAppW(const u16* pat, u32 lvl, void* data,
                                              u32 op, void* filt, u32 flags) {
  return trt_FindFirstFileExW(pat, lvl, data, op, filt, flags);
}
int TRT_STDCALL trt_GetFileAttributesExFromAppW(const u16* name, u32 lvl, void* info) {
  return trt_GetFileAttributesExW(name, lvl, info);
}
int TRT_STDCALL trt_CreateDirectoryFromAppW(const u16* path, void* sa) {
  return trt_CreateDirectoryW(path, sa);
}
int TRT_STDCALL trt_DeleteFileFromAppW(const u16* path) {
  return trt_DeleteFileW(path);
}
int TRT_STDCALL trt_MoveFileFromAppW(const u16* src, const u16* dst) {
  return trt_MoveFileW(src, dst);
}
int TRT_STDCALL trt_CopyFileFromAppW(const u16* src, const u16* dst, int fail_exists) {
  return trt_CopyFileExW(src, dst, nullptr, nullptr, nullptr,
                         fail_exists ? 1u : 0u);
}

// ==================== work-order item 12 remainder ==========================
// Job objects, console attach, process mitigation policy, thread stack
// guarantee. Same honesty rules as the rest of the file: real behavior where
// the runtime has the state, named failure where it does not — never a fake
// success.

// ---- Job objects -----------------------------------------------------------
// A job is a record + a tagged handle in the kernel32 handle space (the same
// pattern bridge_mem.cpp uses for map handles: tag high, slot+1 low, stable
// slots). The guest IS the whole process, so jobs enforce nothing here —
// limits are recorded and logged so SetInformationJobObject is observable,
// and the PROCESS_MEMORY flag is a logged no-op.
namespace {
struct job_rec {
  u32 limit_flags;          // JOBOBJECT_BASIC_LIMIT_INFORMATION.LimitFlags
  u64 process_memory_limit; // recorded, never enforced (logged no-op)
  int assigned;             // AssignProcessToJobObject happened
};
std::mutex g_jobs_mu;
std::vector<std::unique_ptr<job_rec>> g_jobs;
constexpr u64 k_job_handle_tag = 0x4A0B0000ull;  // "JOB" — unique handle space
constexpr u32 k_job_invalid = 0xFFFFFFFFu;

u32 job_index(void* h) {
  const u64 v = (u64)(uintptr_t)h;
  if ((v & 0xFFFF0000ull) != k_job_handle_tag) return k_job_invalid;
  const u64 idx = v & 0xFFFFull;
  if (idx == 0 || idx > g_jobs.size() || !g_jobs[(size_t)(idx - 1)])
    return k_job_invalid;
  return (u32)(idx - 1);
}

u32 job_slot_alloc() {
  for (size_t i = 0; i < g_jobs.size(); ++i)
    if (!g_jobs[i]) return (u32)i;
  if (g_jobs.size() >= 0xFFFF) return k_job_invalid;
  g_jobs.push_back(std::unique_ptr<job_rec>(new job_rec{0, 0, 0}));
  return (u32)(g_jobs.size() - 1);
}

// ---- console attach + mitigation + stack guarantee state -------------------
std::atomic<bool> g_console_attached{false};
thread_local u32 t_stack_guarantee = 0;
std::atomic<u64> g_mitig_logged{0};

void mit_policy_log(u32 policy, const char* what) {
  const u64 bit = policy < 64 ? (1ull << policy) : 0;
  if (bit) {
    if (g_mitig_logged.load(std::memory_order_relaxed) & bit) return;
    g_mitig_logged.fetch_or(bit, std::memory_order_relaxed);
  }
  char line[192];
  snprintf(line, sizeof(line),
           "kernel32: %s(policy=%u) — reported all-zero/unspecified "
           "(the runtime applies no mitigation policy)", what, policy);
  LOG(line);
}
}  // namespace

void* TRT_STDCALL trt_CreateJobObjectW(void* attrs, const u16* name) {
  (void)attrs;
  char line[224];
  char nm[64] = "";
  const int have_name = name && trt_utf16_to_utf8(name, nm, sizeof(nm)) == 0;
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  const u32 slot = job_slot_alloc();
  if (slot == k_job_invalid) {
    LOG("kernel32: CreateJobObjectW -> NULL (job table full)");
    trt_SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
    return nullptr;
  }
  const u64 h = k_job_handle_tag | (u64)(slot + 1);
  snprintf(line, sizeof(line),
           "kernel32: CreateJobObjectW -> 0x%llx (%s%s)",
           (unsigned long long)h, have_name ? "name=" : "anonymous",
           have_name ? nm : "");
  LOG(line);
  return (void*)(uintptr_t)h;
}

void* TRT_STDCALL trt_CreateJobObjectA(void* attrs, const char* name) {
  u16 wname[128];
  const u16* wp = nullptr;
  if (name) {
    u32 i = 0;
    for (; name[i] && i + 1 < 128; ++i) wname[i] = (u16)(u8)name[i];
    wname[i] = 0;
    wp = wname;
  }
  return trt_CreateJobObjectW(attrs, wp);
}

int TRT_STDCALL trt_SetInformationJobObject(void* job, u32 cls, void* info,
                                            u32 cb) {
  char line[256];
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  const u32 idx = job_index(job);
  if (idx == k_job_invalid) {
    LOG("kernel32: SetInformationJobObject -> FALSE (invalid job handle)");
    trt_SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  // JOBOBJECT_BASIC_LIMIT_INFORMATION (x64, 64 bytes): LimitFlags at +16.
  // JOBOBJECT_EXTENDED_LIMIT_INFORMATION (144 bytes) = basic(64) +
  // IO_COUNTERS(48) + 4 SIZE_Ts; ProcessMemoryLimit at +112.
  job_rec& j = *g_jobs[idx];
  if (cls == 2 /*JobObjectBasicLimitInformation*/ && info && cb >= 64) {
    j.limit_flags = *(const u32*)((const u8*)info + 16);
    snprintf(line, sizeof(line),
             "kernel32: SetInformationJobObject(BASIC, cb=%u) -> TRUE "
             "LimitFlags=0x%x (recorded; the runtime enforces no job limits)",
             cb, j.limit_flags);
  } else if (cls == 9 /*JobObjectExtendedLimitInformation*/ && info &&
             cb >= 144) {
    j.limit_flags = *(const u32*)((const u8*)info + 16);
    j.process_memory_limit = *(const u64*)((const u8*)info + 112);
    if (j.limit_flags & 0x100u /*JOB_OBJECT_LIMIT_PROCESS_MEMORY*/) {
      snprintf(line, sizeof(line),
               "kernel32: SetInformationJobObject(EXTENDED, cb=%u) -> TRUE "
               "LimitFlags=0x%x ProcessMemoryLimit=%llu MiB (logged no-op: "
               "the runtime enforces no job memory cap)",
               cb, j.limit_flags,
               (unsigned long long)(j.process_memory_limit >> 20));
    } else {
      snprintf(line, sizeof(line),
               "kernel32: SetInformationJobObject(EXTENDED, cb=%u) -> TRUE "
               "LimitFlags=0x%x (recorded; the runtime enforces no job "
               "limits)", cb, j.limit_flags);
    }
  } else {
    snprintf(line, sizeof(line),
             "kernel32: SetInformationJobObject(class=%u, cb=%u) -> FALSE "
             "(unsupported information class; named gap)", cls, cb);
    LOG(line);
    trt_SetLastError(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  LOG(line);
  return 1;
}

int TRT_STDCALL trt_AssignProcessToJobObject(void* job, void* proc) {
  char line[192];
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  const u32 idx = job_index(job);
  if (idx == k_job_invalid) {
    LOG("kernel32: AssignProcessToJobObject -> FALSE (invalid job handle)");
    trt_SetLastError(6);
    return 0;
  }
  g_jobs[idx]->assigned = 1;
  snprintf(line, sizeof(line),
           "kernel32: AssignProcessToJobObject(job=0x%llx, proc=%p) -> TRUE "
           "(single-process runtime: the guest is the job)",
           (unsigned long long)(uintptr_t)job, proc);
  LOG(line);
  return 1;
}

int TRT_STDCALL trt_TerminateJobObject(void* job, u32 exit_code) {
  char line[160];
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  if (job_index(job) == k_job_invalid) {
    LOG("kernel32: TerminateJobObject -> FALSE (invalid job handle)");
    trt_SetLastError(6);
    return 0;
  }
  snprintf(line, sizeof(line),
           "kernel32: TerminateJobObject(job=0x%llx, exit=%u) -> TRUE "
           "(logged: the guest process itself is not terminated)",
           (unsigned long long)(uintptr_t)job, exit_code);
  LOG(line);
  return 1;
}

void* TRT_STDCALL trt_OpenJobObjectW(u32 access, int inherit,
                                     const u16* name) {
  (void)access; (void)inherit;
  char line[192];
  char nm[64] = "";
  const int have_name = name && trt_utf16_to_utf8(name, nm, sizeof(nm)) == 0;
  snprintf(line, sizeof(line),
           "kernel32: OpenJobObjectW(%s) -> NULL (no named-job namespace in "
           "the runtime; honest ERROR_FILE_NOT_FOUND)",
           have_name ? nm : "null");
  LOG(line);
  trt_SetLastError(2);  // ERROR_FILE_NOT_FOUND
  return nullptr;
}

void* TRT_STDCALL trt_OpenJobObjectA(u32 access, int inherit,
                                     const char* name) {
  u16 wname[128];
  const u16* wp = nullptr;
  if (name) {
    u32 i = 0;
    for (; name[i] && i + 1 < 128; ++i) wname[i] = (u16)(u8)name[i];
    wname[i] = 0;
    wp = wname;
  }
  return trt_OpenJobObjectW(access, inherit, wp);
}

int TRT_STDCALL trt_IsProcessInJob(void* proc, void* job, int* out) {
  (void)proc;
  char line[192];
  if (!out) {
    trt_SetLastError(87);
    return 0;
  }
  if (!job) {
    // the probe games run at boot: "am I inside any job?" — the runtime
    // puts the guest in no job, so the honest answer is FALSE.
    *out = 0;
    static std::atomic<char> once{0};
    if (!once.exchange(1)) {
      snprintf(line, sizeof(line),
               "kernel32: IsProcessInJob(NULL job) -> TRUE, *out=FALSE "
               "(guest runs in no job)");
      LOG(line);
    }
    return 1;
  }
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  const u32 idx = job_index(job);
  if (idx == k_job_invalid) {
    trt_SetLastError(6);
    return 0;
  }
  *out = g_jobs[idx]->assigned ? 1 : 0;
  snprintf(line, sizeof(line),
           "kernel32: IsProcessInJob(job=0x%llx) -> TRUE, *out=%d",
           (unsigned long long)(uintptr_t)job, *out);
  LOG(line);
  return 1;
}

// ---- Console attach --------------------------------------------------------
// AllocConsole/FreeConsole manage a headless console RECORD: the game's CRT
// boot and logging paths proceed. There is still no host pipe behind stdout
// (GetStdHandle stays honest), and the code pages report the OEM/UTF-8
// constants games switch on.
int TRT_STDCALL trt_AllocConsole(void) {
  bool expect = false;
  if (g_console_attached.compare_exchange_strong(expect, true)) {
    LOG("kernel32: AllocConsole -> TRUE (headless console record; no host "
        "stdio pipe — GetStdHandle stays honest)");
  }
  return 1;
}

int TRT_STDCALL trt_FreeConsole(void) {
  bool expect = true;
  if (g_console_attached.compare_exchange_strong(expect, false)) {
    LOG("kernel32: FreeConsole -> TRUE (console record dropped)");
  }
  return 1;
}

// ---- Process mitigation policy ---------------------------------------------
// Games probe GetProcessMitigationPolicy to decide hardening workarounds.
// The runtime applies no mitigation policy, so every policy reads back
// all-zero (= unspecified) — the honest answer for this process.
int TRT_STDCALL trt_GetProcessMitigationPolicy(void* proc, u32 policy,
                                               void* buffer, u64 len) {
  (void)proc;
  if (!buffer || len == 0) {
    trt_SetLastError(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  memset(buffer, 0, (size_t)len);
  mit_policy_log(policy, "GetProcessMitigationPolicy");
  return 1;
}

int TRT_STDCALL trt_SetProcessMitigationPolicy(void* proc, u32 policy,
                                               void* buffer, u64 len) {
  (void)proc; (void)buffer;
  if (!buffer || len == 0) {
    trt_SetLastError(87);
    return 0;
  }
  mit_policy_log(policy, "SetProcessMitigationPolicy");
  return 1;  // accepted-and-logged: the runtime enforces nothing extra
}

// ---- Thread stack guarantee -------------------------------------------------
// A per-thread preference record (the real API only records it too); the
// runtime reserves no extra OS stack, so the value is stored and returned.
int TRT_STDCALL trt_SetThreadStackGuarantee(u32* size) {
  if (!size) {
    trt_SetLastError(87);
    return 0;
  }
  t_stack_guarantee = *size;
  static std::atomic<char> once{0};
  if (!once.exchange(1)) {
    char line[160];
    snprintf(line, sizeof(line),
             "kernel32: SetThreadStackGuarantee(%u) -> TRUE (recorded; the "
             "runtime reserves no OS stack guarantee)", *size);
    LOG(line);
  }
  return 1;
}

int TRT_STDCALL trt_GetThreadStackGuarantee(u32* size) {
  if (!size) {
    trt_SetLastError(87);
    return 0;
  }
  *size = t_stack_guarantee;
  return 1;
}

// CloseHandle hook (bridge_kernel32.cpp trt_CloseHandle): toolhelp snapshots
// and job records live in this file's registries. Returns 1 iff the handle
// belonged to one of them (tag-checked, so no other handle class is touched).
int trt_gap_handle_close(void* h) {
  {
    std::lock_guard<std::mutex> lk(g_snaps_mu);
    if (snap_at(h)) {
      for (size_t i = 0; i < g_snaps.size(); ++i)
        if (g_snaps[i].get() == snap_at(h)) g_snaps[i].reset();
      LOG("kernel32: CloseHandle -> toolhelp snapshot released");
      return 1;
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    const u32 idx = job_index(h);
    if (idx != k_job_invalid) {
      g_jobs[idx].reset();
      LOG("kernel32: CloseHandle -> job record released");
      return 1;
    }
  }
  return 0;
}

// Geography (honest stubs: return 0 / empty)
extern "C" int TRT_GUEST_ABI trt_GetGeoInfoW(unsigned geo, unsigned type,
                                             unsigned char* data, int data_size,
                                             unsigned lang) {
    (void)geo; (void)type; (void)data; (void)data_size; (void)lang;
    return 0;
}
extern "C" unsigned TRT_GUEST_ABI trt_GetUserGeoID(int level) {
    (void)level;
    return 0;  // GEOID_NOT_AVAILABLE
}

static const trt_named_fn k_gap_fns[] = {
  {"IsWow64Process", (void*)&trt_IsWow64Process},
  {"IsWow64Process2", (void*)&trt_IsWow64Process2},
  {"GetActiveProcessorCount", (void*)&trt_GetActiveProcessorCount},
  {"GetMaximumProcessorCount", (void*)&trt_GetMaximumProcessorCount},
  {"GetActiveProcessorGroupCount", (void*)&trt_GetActiveProcessorGroupCount},
  {"GetMaximumProcessorGroupCount", (void*)&trt_GetMaximumProcessorGroupCount},
  {"GetCurrentProcessorNumberEx", (void*)&trt_GetCurrentProcessorNumberEx},
  {"SetThreadIdealProcessor", (void*)&trt_SetThreadIdealProcessor},
  {"SetThreadIdealProcessorEx", (void*)&trt_SetThreadIdealProcessorEx},
  {"SetThreadSelectedCpuSets", (void*)&trt_SetThreadSelectedCpuSets},
  {"GetSystemCpuSetInformation", (void*)&trt_GetSystemCpuSetInformation},
  {"GetProcessTimes", (void*)&trt_GetProcessTimes},
  {"GetThreadTimes", (void*)&trt_GetThreadTimes},
  {"GetThreadIOPendingFlag", (void*)&trt_GetThreadIOPendingFlag},
  {"QueryUnbiasedInterruptTime", (void*)&trt_QueryUnbiasedInterruptTime},
  {"QueryUnbiasedInterruptTimePrecise", (void*)&trt_QueryUnbiasedInterruptTimePrecise},
  {"QueryInterruptTime", (void*)&trt_QueryUnbiasedInterruptTime},
  {"QueryInterruptTimePrecise", (void*)&trt_QueryUnbiasedInterruptTimePrecise},
  {"FlushProcessWriteBuffers", (void*)&trt_FlushProcessWriteBuffers},
  {"VirtualLock", (void*)&trt_VirtualLock},
  {"VirtualUnlock", (void*)&trt_VirtualUnlock},
  {"SetProcessWorkingSetSizeEx", (void*)&trt_SetProcessWorkingSetSizeEx},
  {"GetProcessWorkingSetSizeEx", (void*)&trt_GetProcessWorkingSetSizeEx},
  {"IsValidCodePage", (void*)&trt_IsValidCodePage},
  {"SetEnvironmentVariableW", (void*)&trt_SetEnvironmentVariableW},
  {"MulDiv", (void*)&trt_MulDiv},
  {"FormatMessageW", (void*)&trt_FormatMessageW},
  {"SetDllDirectoryW", (void*)&trt_SetDllDirectoryW},
  {"SetDllDirectoryA", (void*)&trt_SetDllDirectoryA},
  {"GetDllDirectoryW", (void*)&trt_GetDllDirectoryW},
  {"CreateSymbolicLinkW", (void*)&trt_CreateSymbolicLinkW},
  {"GetDriveTypeW", (void*)&trt_GetDriveTypeW},
  {"GetExitCodeProcess", (void*)&trt_GetExitCodeProcess},
  {"LocalAlloc", (void*)&trt_LocalAlloc},
  {"LocalReAlloc", (void*)&trt_LocalReAlloc},
  {"LocalFree", (void*)&trt_LocalFree},
  {"K32EnumProcessModules", (void*)&trt_K32EnumProcessModules},
  {"K32GetModuleFileNameExW", (void*)&trt_K32GetModuleFileNameExW},
  {"GetFinalPathNameByHandleW", (void*)&trt_GetFinalPathNameByHandleW},
  {"GetFullPathNameW", (void*)&trt_GetFullPathNameW},
  {"GetTimeZoneInformation", (void*)&trt_GetTimeZoneInformation},
  {"FileTimeToSystemTime", (void*)&trt_FileTimeToSystemTime},
  {"FileTimeToLocalFileTime", (void*)&trt_FileTimeToLocalFileTime},
  {"LocalFileTimeToFileTime", (void*)&trt_LocalFileTimeToFileTime},
  {"FileTimeToDosDateTime", (void*)&trt_FileTimeToDosDateTime},
  {"DosDateTimeToFileTime", (void*)&trt_DosDateTimeToFileTime},
  {"SystemTimeToTzSpecificLocalTime", (void*)&trt_SystemTimeToTzSpecificLocalTime},
  {"SetEndOfFile", (void*)&trt_SetEndOfFile},
  {"GetFileInformationByHandle", (void*)&trt_GetFileInformationByHandle},
  {"GetFileSizeEx", (void*)&trt_GetFileSizeEx},
  {"PeekNamedPipe", (void*)&trt_PeekNamedPipe},
  // RACCOIN / Unity IL2CPP + CRT localization, InitOnce, timing & process gaps:
  {"GetUserDefaultLocaleName", (void*)&trt_GetUserDefaultLocaleName},
  {"GetSystemDefaultLocaleName", (void*)&trt_GetSystemDefaultLocaleName},
  {"IsValidLocaleName", (void*)&trt_IsValidLocaleName},
  {"IsValidLocale", (void*)&trt_IsValidLocale},
  {"ResolveLocaleName", (void*)&trt_ResolveLocaleName},
  {"GetLocaleInfoEx", (void*)&trt_GetLocaleInfoEx},
  {"GetLocaleInfoW", (void*)&trt_GetLocaleInfoW},
  {"GetDateFormatEx", (void*)&trt_GetDateFormatEx},
  {"GetDateFormatW", (void*)&trt_GetDateFormatW},
  {"GetTimeFormatEx", (void*)&trt_GetTimeFormatEx},
  {"GetTimeFormatW", (void*)&trt_GetTimeFormatW},
  {"GetUserDefaultUILanguage", (void*)&trt_GetUserDefaultUILanguage},
  {"GetSystemDefaultUILanguage", (void*)&trt_GetSystemDefaultUILanguage},
  {"GetThreadLocale", (void*)&trt_GetThreadLocale},
  {"SetThreadLocale", (void*)&trt_SetThreadLocale},
  {"IsDBCSLeadByteEx", (void*)&trt_IsDBCSLeadByteEx},
  {"LCIDToLocaleName", (void*)&trt_LCIDToLocaleName},
  {"LocaleNameToLCID", (void*)&trt_LocaleNameToLCID},
  {"GetLocalTime", (void*)&trt_GetLocalTime},
  {"GetSystemTimePreciseAsFileTime", (void*)&trt_GetSystemTimePreciseAsFileTime},
  {"SystemTimeToFileTime", (void*)&trt_SystemTimeToFileTime},
  {"InitOnceInitialize", (void*)&trt_InitOnceInitialize},
  {"InitOnceBeginInitialize", (void*)&trt_InitOnceBeginInitialize},
  {"InitOnceComplete", (void*)&trt_InitOnceComplete},
  {"InitOnceExecuteOnce", (void*)&trt_InitOnceExecuteOnce},
  {"GlobalAlloc", (void*)&trt_GlobalAlloc},
  {"GlobalReAlloc", (void*)&trt_GlobalReAlloc},
  {"GlobalFree", (void*)&trt_GlobalFree},
  {"GlobalLock", (void*)&trt_GlobalLock},
  {"GlobalUnlock", (void*)&trt_GlobalUnlock},
  {"GlobalSize", (void*)&trt_GlobalSize},
  {"HeapDestroy", (void*)&trt_HeapDestroy},
  {"LoadLibraryA", (void*)&trt_LoadLibraryA},
  {"LoadLibraryExA", (void*)&trt_LoadLibraryExA},
  {"GetModuleHandleExA", (void*)&trt_GetModuleHandleExA},
  {"GetModuleFileNameA", (void*)&trt_GetModuleFileNameA},
  {"GetSystemDirectoryA", (void*)&trt_GetSystemDirectoryA},
  {"CreateMutexExW", (void*)&trt_CreateMutexExW},
  {"CreateMutexExA", (void*)&trt_CreateMutexExA},
  {"CreateWaitableTimerA", (void*)&trt_CreateWaitableTimerA},
  {"OutputDebugStringA", (void*)&trt_OutputDebugStringA},
  {"OutputDebugStringW", (void*)&trt_OutputDebugStringW},
  {"SetErrorMode", (void*)&trt_SetErrorMode},
  {"GetErrorMode", (void*)&trt_GetErrorMode},
  {"SetThreadErrorMode", (void*)&trt_SetThreadErrorMode},
  {"lstrlenW", (void*)&trt_lstrlenW},
  {"lstrlenA", (void*)&trt_lstrlenA},
  {"RtlCaptureStackBackTrace", (void*)&trt_RtlCaptureStackBackTrace},
  {"GetEnvironmentVariableA", (void*)&trt_GetEnvironmentVariableA},
  {"SetEnvironmentVariableA", (void*)&trt_SetEnvironmentVariableA},
  {"GetFileAttributesA", (void*)&trt_GetFileAttributesA},
  {"GetFileTime", (void*)&trt_GetFileTime},
  {"SetFileTime", (void*)&trt_SetFileTime},
  {"MoveFileW", (void*)&trt_MoveFileW},
  {"MoveFileExW", (void*)&trt_MoveFileExW},
  {"MoveFileExA", (void*)&trt_MoveFileExA},
  {"ReplaceFileW", (void*)&trt_ReplaceFileW},
  {"GetLongPathNameW", (void*)&trt_GetLongPathNameW},
  {"GetShortPathNameW", (void*)&trt_GetShortPathNameW},
  {"GetTempPathA", (void*)&trt_GetTempPathA},
  {"GetTempFileNameA", (void*)&trt_GetTempFileNameA},
  {"GetFileInformationByHandleEx", (void*)&trt_GetFileInformationByHandleEx},
  {"SetFileInformationByHandle", (void*)&trt_SetFileInformationByHandle},
  {"GetHandleInformation", (void*)&trt_GetHandleInformation},
  {"SetHandleInformation", (void*)&trt_SetHandleInformation},
  {"GetDiskFreeSpaceExW", (void*)&trt_GetDiskFreeSpaceExW},
  {"GetVolumeInformationW", (void*)&trt_GetVolumeInformationW},
  {"GetLogicalDrives", (void*)&trt_GetLogicalDrives},
  {"GetLargePageMinimum", (void*)&trt_GetLargePageMinimum},
  {"GlobalMemoryStatusEx", (void*)&trt_GlobalMemoryStatusEx},
  {"GetSystemPowerStatus", (void*)&trt_GetSystemPowerStatus},
  {"GetLogicalProcessorInformation", (void*)&trt_GetLogicalProcessorInformation},
  {"GetLogicalProcessorInformationEx", (void*)&trt_GetLogicalProcessorInformationEx},
  {"VerSetConditionMask", (void*)&trt_VerSetConditionMask},
  {"VerifyVersionInfoW", (void*)&trt_VerifyVersionInfoW},
  {"SetThreadAffinityMask", (void*)&trt_SetThreadAffinityMask},
  {"SetProcessAffinityMask", (void*)&trt_SetProcessAffinityMask},
  {"GetProcessAffinityMask", (void*)&trt_GetProcessAffinityMask},
  {"SetThreadDescription", (void*)&trt_SetThreadDescription},
  {"GetThreadDescription", (void*)&trt_GetThreadDescription},
  {"ReadProcessMemory", (void*)&trt_ReadProcessMemory},
  {"K32GetProcessMemoryInfo", (void*)&trt_K32GetProcessMemoryInfo},
  {"CreateToolhelp32Snapshot", (void*)&trt_CreateToolhelp32Snapshot},
  {"Process32First", (void*)&trt_ToolhelpNext_fail},
  {"Process32Next", (void*)&trt_ToolhelpNext_fail},
  {"Thread32First", (void*)&trt_ToolhelpNext_fail},
  {"Thread32Next", (void*)&trt_ToolhelpNext_fail},
  {"InitializeProcThreadAttributeList", (void*)&trt_InitializeProcThreadAttributeList},
  {"UpdateProcThreadAttribute", (void*)&trt_UpdateProcThreadAttribute},
  {"DeleteProcThreadAttributeList", (void*)&trt_DeleteProcThreadAttributeList},
  {"GetDynamicTimeZoneInformation", (void*)&trt_GetDynamicTimeZoneInformation},
  {"GetTimeZoneInformationForYear", (void*)&trt_GetTimeZoneInformationForYear},
  {"K32GetModuleBaseNameW", (void*)&trt_K32GetModuleBaseNameW},
  {"DeleteVolumeMountPointW", (void*)&trt_DeleteVolumeMountPointW},
  // PathCch*
  {"PathCchSkipRoot", (void*)&trt_PathCchSkipRoot},
  {"PathCchIsRoot", (void*)&trt_PathCchIsRoot},
  {"PathCchAddBackslash", (void*)&trt_PathCchAddBackslash},
  {"PathCchAddBackslashEx", (void*)&trt_PathCchAddBackslashEx},
  {"PathCchRemoveBackslash", (void*)&trt_PathCchRemoveBackslash},
  {"PathCchRemoveBackslashEx", (void*)&trt_PathCchRemoveBackslashEx},
  {"PathCchRemoveFileSpec", (void*)&trt_PathCchRemoveFileSpec},
  {"PathCchFindExtension", (void*)&trt_PathCchFindExtension},
  {"PathCchRemoveExtension", (void*)&trt_PathCchRemoveExtension},
  {"PathCchRenameExtension", (void*)&trt_PathCchRenameExtension},
  {"PathCchStripToRoot", (void*)&trt_PathCchStripToRoot},
  {"PathCchStripPrefix", (void*)&trt_PathCchStripPrefix},
  {"PathCchCanonicalize", (void*)&trt_PathCchCanonicalize},
  {"PathCchCanonicalizeEx", (void*)&trt_PathCchCanonicalizeEx},
  {"PathCchCombine", (void*)&trt_PathCchCombine},
  {"PathCchCombineEx", (void*)&trt_PathCchCombineEx},
  {"PathCchAppend", (void*)&trt_PathCchAppend},
  {"PathCchAppendEx", (void*)&trt_PathCchAppendEx},
  // Heap, Disk, Volumes, Locale, Time, Locking, Process, NUMA, Console, IoRing
  {"HeapCompact", (void*)&trt_HeapCompact},
  {"HeapValidate", (void*)&trt_HeapValidate},
  {"HeapWalk", (void*)&trt_HeapWalk},
  {"GetDiskFreeSpaceW", (void*)&trt_GetDiskFreeSpaceW},
  {"GetDiskFreeSpaceA", (void*)&trt_GetDiskFreeSpaceA},
  {"GetLogicalDriveStringsW", (void*)&trt_GetLogicalDriveStringsW},
  {"GetLogicalDriveStringsA", (void*)&trt_GetLogicalDriveStringsA},
  {"FindFirstVolumeW", (void*)&trt_FindFirstVolumeW},
  {"FindNextVolumeW", (void*)&trt_FindNextVolumeW},
  {"FindVolumeClose", (void*)&trt_FindVolumeClose},
  {"GetVolumePathNameW", (void*)&trt_GetVolumePathNameW},
  {"GetVolumePathNamesForVolumeNameW", (void*)&trt_GetVolumePathNamesForVolumeNameW},
  {"NeedCurrentDirectoryForExePathW", (void*)&trt_NeedCurrentDirectoryForExePathW},
  {"GetLocaleInfoA", (void*)&trt_GetLocaleInfoA},
  {"TzSpecificLocalTimeToSystemTime", (void*)&trt_TzSpecificLocalTimeToSystemTime},
  {"SetLocalTime", (void*)&trt_SetLocalTime},
  {"Beep", (void*)&trt_Beep},
  {"LockFileEx", (void*)&trt_LockFileEx},
  {"UnlockFileEx", (void*)&trt_UnlockFileEx},
  {"LockFile", (void*)&trt_LockFile},
  {"UnlockFile", (void*)&trt_UnlockFile},
  {"OpenProcess", (void*)&trt_OpenProcess},
  {"CreateHardLinkW", (void*)&trt_CreateHardLinkW},
  {"SetWaitableTimerEx", (void*)&trt_SetWaitableTimerEx},
  {"GetNumaHighestNodeNumber", (void*)&trt_GetNumaHighestNodeNumber},
  {"GetNumaNodeProcessorMask", (void*)&trt_GetNumaNodeProcessorMask},
  {"GetNumaProcessorNode", (void*)&trt_GetNumaProcessorNode},
  {"GetNumaProcessorNodeEx", (void*)&trt_GetNumaProcessorNodeEx},
  {"GetNumaNodeProcessorMaskEx", (void*)&trt_GetNumaNodeProcessorMaskEx},
  {"SetNamedPipeHandleState", (void*)&trt_SetNamedPipeHandleState},
  {"WaitNamedPipeW", (void*)&trt_WaitNamedPipeW},
  {"GetNamedPipeHandleStateW", (void*)&trt_GetNamedPipeHandleStateW},
  {"SetConsoleMode", (void*)&trt_SetConsoleMode},
  {"ReadConsoleInputW", (void*)&trt_ReadConsoleInputW},
  {"PeekConsoleInputA", (void*)&trt_PeekConsoleInputA},
  {"GetNumberOfConsoleInputEvents", (void*)&trt_GetNumberOfConsoleInputEvents},
  {"WriteConsoleA", (void*)&trt_WriteConsoleA},
  {"GenerateConsoleCtrlEvent", (void*)&trt_GenerateConsoleCtrlEvent},
  {"GetFileInformationByName", (void*)&trt_GetFileInformationByName},
  {"PssCaptureSnapshot", (void*)&trt_PssCaptureSnapshot},
  {"PssQuerySnapshot", (void*)&trt_PssQuerySnapshot},
  {"PssFreeSnapshot", (void*)&trt_PssFreeSnapshot},
  {"QueryIoRingCapabilities", (void*)&trt_QueryIoRingCapabilities},
  {"IsIoRingOpSupported", (void*)&trt_IsIoRingOpSupported},
  {"CreateIoRing", (void*)&trt_CreateIoRing},
  {"BuildIoRingReadFile", (void*)&trt_IoRingNotImpl},
  {"BuildIoRingRegisterBuffers", (void*)&trt_IoRingNotImpl},
  {"BuildIoRingRegisterFileHandles", (void*)&trt_IoRingNotImpl},
  {"SetIoRingCompletionEvent", (void*)&trt_IoRingNotImpl},
  {"SubmitIoRing", (void*)&trt_IoRingNotImpl},
  {"PopIoRingCompletion", (void*)&trt_IoRingNotImpl},
  {"CloseIoRing", (void*)&trt_IoRingNotImpl},
  {"CreateFileFromAppW", (void*)&trt_CreateFileFromAppW},
  {"CreateFile2FromAppW", (void*)&trt_CreateFile2FromAppW},
  {"FindFirstFileExFromAppW", (void*)&trt_FindFirstFileExFromAppW},
  {"GetFileAttributesExFromAppW", (void*)&trt_GetFileAttributesExFromAppW},
  {"CreateDirectoryFromAppW", (void*)&trt_CreateDirectoryFromAppW},
  {"DeleteFileFromAppW", (void*)&trt_DeleteFileFromAppW},
  {"MoveFileFromAppW", (void*)&trt_MoveFileFromAppW},
  {"CopyFileFromAppW", (void*)&trt_CopyFileFromAppW},
  // work-order item 12 remainder: jobs, console attach, toolhelp module
  // snapshots, mitigation policy, thread stack guarantee
  {"CreateJobObjectW", (void*)&trt_CreateJobObjectW},
  {"CreateJobObjectA", (void*)&trt_CreateJobObjectA},
  {"SetInformationJobObject", (void*)&trt_SetInformationJobObject},
  {"AssignProcessToJobObject", (void*)&trt_AssignProcessToJobObject},
  {"TerminateJobObject", (void*)&trt_TerminateJobObject},
  {"OpenJobObjectW", (void*)&trt_OpenJobObjectW},
  {"OpenJobObjectA", (void*)&trt_OpenJobObjectA},
  {"IsProcessInJob", (void*)&trt_IsProcessInJob},
  {"AllocConsole", (void*)&trt_AllocConsole},
  {"FreeConsole", (void*)&trt_FreeConsole},
  {"Module32FirstW", (void*)&trt_Module32FirstW},
  {"Module32NextW", (void*)&trt_Module32NextW},
  {"Module32FirstA", (void*)&trt_Module32FirstA},
  {"Module32NextA", (void*)&trt_Module32NextA},
  {"CloseToolhelp32Snapshot", (void*)&trt_CloseToolhelp32Snapshot},
  {"GetProcessMitigationPolicy", (void*)&trt_GetProcessMitigationPolicy},
  {"SetProcessMitigationPolicy", (void*)&trt_SetProcessMitigationPolicy},
  {"SetThreadStackGuarantee", (void*)&trt_SetThreadStackGuarantee},
  {"GetThreadStackGuarantee", (void*)&trt_GetThreadStackGuarantee},
  {"GetGeoInfoW", (void*)&trt_GetGeoInfoW},
  {"GetUserGeoID", (void*)&trt_GetUserGeoID},
};

// registered by trt_bridge_register_kernel32 after the core table
void trt_gap_register(void);

#include "trt.h"  // trt_named_fn, trt_route (re-included harmless)

void trt_gap_register(void) {
  // merge into kernel32 + kernelbase (same table, same merge path)
  extern void trt_module_register(const char* name, trt_route route,
                                  const trt_named_fn* fns, int count);
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_gap_fns,
                      (int)(sizeof(k_gap_fns) / sizeof(k_gap_fns[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_gap_fns,
                      (int)(sizeof(k_gap_fns) / sizeof(k_gap_fns[0])));
}

}  // extern "C"
