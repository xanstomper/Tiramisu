// tiramisu/src/trt/bridge_shell.cpp
// Shell32 bridge. See bridge_shell.h.
//
// Folder mapping (guest C:\ is LocalState\drive_c):
//   CSIDL_PROFILE            -> C:\users\player
//   CSIDL_MYDOCUMENTS        -> C:\users\player\Documents
//   CSIDL_LOCAL_APPDATA      -> C:\users\player\AppData\Local
//   CSIDL_ROAMING_APPDATA    -> C:\users\player\AppData\Roaming
//   CSIDL_COMMON_APPDATA     -> C:\ProgramData
//   CSIDL_PERSONAL == MYDOCUMENTS; the rest fail visibly with the csidl in
//   the log (a game that needs CSIDL_PROGRAM_FILES is telling us something).
//
// SHGetKnownFolderPath maps the FOLDERID_* GUIDs games actually pass
// (Documents {FDD39AD0..}, LocalAppData {F1B32785..}, Profile {5E6C858F..},
// Roaming AppData {3EB685DB..}, ProgramData {62AB5D82..}).
//
// Both wide and ANSI entry points fill a MAX_PATH (260) buffer; the VFS
// mkdirs the folder when the SHF_FLAG_CREATE bit (0x8000) is set.
#include "bridge_shell.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>  // _mkdir
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

namespace shellhelpers {
inline int mk_dir(const char* p) {
#ifdef _WIN32
  return _mkdir(p);
#else
  return mkdir(p, 0755);
#endif
}
}

extern "C" {
// defined in vfs.cpp (C linkage)
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);
}

// unique name, file scope, OUTSIDE extern "C" (the log collision rule)
namespace shelllog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) shelllog::line(s_)

namespace {

constexpr int S_OK_ = 0;
constexpr int E_FAIL_ = (int)0x80004005;
constexpr int E_INVALIDARG_ = (int)0x80070057;
constexpr int E_HANDLE_ = (int)0x80070006;
constexpr u32 k_max_path = 260;
constexpr u32 CSIDL_FLAG_CREATE = 0x8000;

// csidl -> vfs path (utf8). Returns 0 = known, 1 = unsupported (named).
int csidl_path(u32 csidl, char* out, u32 cap) {
  u32 base = csidl & ~CSIDL_FLAG_CREATE;
  switch (base) {
    case 0x0028:  // CSIDL_PROFILE
      snprintf(out, cap, "C:\\users\\player");
      break;
    case 0x0005:  // CSIDL_PERSONAL / CSIDL_MYDOCUMENTS
      snprintf(out, cap, "C:\\users\\player\\Documents");
      break;
    case 0x001C:  // CSIDL_LOCAL_APPDATA
      snprintf(out, cap, "C:\\users\\player\\AppData\\Local");
      break;
    case 0x001A:  // CSIDL_ROAMING_APPDATA
      snprintf(out, cap, "C:\\users\\player\\AppData\\Roaming");
      break;
    case 0x0023:  // CSIDL_COMMON_APPDATA
      snprintf(out, cap, "C:\\ProgramData");
      break;
    case 0x0026:  // CSIDL_LOCAL_APPDATA low integrity is the same dir here
      snprintf(out, cap, "C:\\users\\player\\AppData\\Local\\Low");
      break;
    case 0x0020:  // CSIDL_DESKTOPDIRECTORY
      snprintf(out, cap, "C:\\users\\player\\Desktop");
      break;
    case 0x0027:  // CSIDL_MYPICTURES
      snprintf(out, cap, "C:\\users\\player\\Pictures");
      break;
    default:
      return 1;
  }
  return 0;
}

// mkdir through the VFS so the game can write its save immediately
void ensure_dir(const char* vfs_path) {
  // resolve through the VFS to the host path, mkdir each component
  u16 wide[k_max_path];
  u32 n = 0;
  for (; vfs_path[n] && n < k_max_path - 1; ++n)
    wide[n] = (u16)(u8)vfs_path[n];
  wide[n] = 0;
  char host[1024];
  if (trt_vfs_resolve_utf16(wide, host, sizeof(host)) != 0) return;
  for (char* p = host + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      shellhelpers::mk_dir(host);  // EEXIST is fine
      *p = '/';
    }
  }
  shellhelpers::mk_dir(host);
}

void utf8_to_utf16(const char* in, u16* out, u32 cap) {
  u32 o = 0;
  for (u32 i = 0; in[i] && o + 1 < cap; ++i) out[o++] = (u16)(u8)in[i];
  out[o] = 0;
}

int TRT_GUEST_ABI shell_SHGetFolderPathW(void* hwnd, u32 csidl, void* token,
                                         u32 flags, u16* path_out) {
  (void)hwnd; (void)token;
  if (!path_out) return E_INVALIDARG_;
  char p8[k_max_path];
  if (csidl_path(csidl, p8, sizeof(p8)) != 0) {
    char line[96];
    snprintf(line, sizeof(line),
             "shell: SHGetFolderPathW csidl=0x%x -> E_FAIL (no mapping)", csidl);
    log_line(line);
    return E_FAIL_;
  }
  if (flags & CSIDL_FLAG_CREATE) ensure_dir(p8);
  utf8_to_utf16(p8, path_out, k_max_path);
  char line[160];
  snprintf(line, sizeof(line), "shell: SHGetFolderPathW csidl=0x%x -> %s%s",
           csidl, p8, (flags & CSIDL_FLAG_CREATE) ? " (created)" : "");
  log_line(line);
  return S_OK_;
}

int TRT_GUEST_ABI shell_SHGetFolderPathA(void* hwnd, u32 csidl, void* token,
                                         u32 flags, char* path_out) {
  if (!path_out) return E_INVALIDARG_;
  if (csidl_path(csidl, path_out, k_max_path) != 0) {
    char line[96];
    snprintf(line, sizeof(line),
             "shell: SHGetFolderPathA csidl=0x%x -> E_FAIL (no mapping)", csidl);
    log_line(line);
    return E_FAIL_;
  }
  if (flags & CSIDL_FLAG_CREATE) ensure_dir(path_out);
  char line[160];
  snprintf(line, sizeof(line), "shell: SHGetFolderPathA csidl=0x%x -> %s",
           csidl, path_out);
  log_line(line);
  return S_OK_;
}

// ---- FOLDERID GUIDs (first 8 bytes identify; games use these five) ----
struct guid8 { u32 a; u16 b, c; };
int knownfolder_path(const u8* g, char* out, u32 cap) {
  const guid8* id = (const guid8*)g;
  struct { u32 a; u16 b, c; const char* path; } table[] = {
      // FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
      {0xFDD39AD0u, 0x238F, 0x46AF, "C:\\users\\player\\Documents"},
      // FOLDERID_LocalAppData {F1B32785-6FBA-4FCF-9D55-7B8E7F157091}
      {0xF1B32785u, 0x6FBA, 0x4FCF, "C:\\users\\player\\AppData\\Local"},
      // FOLDERID_Profile {5E6C858F-0E22-4760-9AFE-EA3317B67173}
      {0x5E6C858Fu, 0x0E22, 0x4760, "C:\\users\\player"},
      // FOLDERID_RoamingAppData {3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}
      {0x3EB685DBu, 0x65F9, 0x4CF6, "C:\\users\\player\\AppData\\Roaming"},
      // FOLDERID_ProgramData {62AB5D82-FDC1-4DC3-9A70-5B0E9E5C6EB4}
      {0x62AB5D82u, 0xFDC1, 0x4DC3, "C:\\ProgramData"},
      // FOLDERID_SavedGames {4C5C32FA-BB47-4C5C-B1A7-17439CE19972}
      {0x4C5C32FAu, 0xBB47, 0x4C5C, "C:\\users\\player\\Saved Games"},
      // FOLDERID_LocalAppDataLow {A520A1A4-1780-4FF6-BD18-167343C5AF16}
      // (Unity Application.persistentDataPath)
      {0xA520A1A4u, 0x1780, 0x4FF6, "C:\\users\\player\\AppData\\LocalLow"},
      // FOLDERID_Desktop {B4BFCC3A-DB2C-424C-B029-7FE99A87C641}
      {0xB4BFCC3Au, 0xDB2C, 0x424C, "C:\\users\\player\\Desktop"},
      // FOLDERID_Pictures {33E28130-4E1E-4676-835A-98395C3BC3BB}
      {0x33E28130u, 0x4E1E, 0x4676, "C:\\users\\player\\Pictures"},
      // FOLDERID_Music {4BD8D571-6D19-48D3-BE97-422220080E43}
      {0x4BD8D571u, 0x6D19, 0x48D3, "C:\\users\\player\\Music"},
      // FOLDERID_Videos {18989B1D-99B5-455B-841C-AB7C74E4DDFC}
      {0x18989B1Du, 0x99B5, 0x455B, "C:\\users\\player\\Videos"},
      // FOLDERID_Downloads {374DE290-123F-4565-9164-39C4925E467B}
      {0x374DE290u, 0x123F, 0x4565, "C:\\users\\player\\Downloads"},
      // FOLDERID_Public {DFDF76A2-C82A-4D63-906A-5644AC457385}
      {0xDFDF76A2u, 0xC82A, 0x4D63, "C:\\users\\Public"},
  };
  for (u32 i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    if (id->a == table[i].a && id->b == table[i].b && id->c == table[i].c) {
      snprintf(out, cap, "%s", table[i].path);
      return 0;
    }
  }
  return 1;
}

int TRT_GUEST_ABI shell_SHGetKnownFolderPath(const u8* rfid, u32 flags,
                                             void* token, u16** path_out) {
  (void)flags; (void)token;
  if (!path_out) return E_INVALIDARG_;
  *path_out = nullptr;
  if (!rfid) return E_INVALIDARG_;
  char p8[k_max_path];
  if (knownfolder_path(rfid, p8, sizeof(p8)) != 0) {
    char guid[40];
    // print the first field for the log
    char line[96];
    guid8 id;
    memcpy(&id, rfid, sizeof(id));
    snprintf(guid, sizeof(guid), "{%08X-%04X-%04X-...}", id.a, id.b, id.c);
    snprintf(line, sizeof(line),
             "shell: SHGetKnownFolderPath %s -> E_FAIL (no mapping)",
             guid);
    log_line(line);
    return E_FAIL_;
  }
  ensure_dir(p8);
  const u32 len = (u32)strlen(p8);
  u16* buf = (u16*)malloc(((size_t)len + 1) * sizeof(u16));
  if (!buf) return (int)0x8007000Eu;  // E_OUTOFMEMORY
  utf8_to_utf16(p8, buf, len + 1);
  *path_out = buf;
  char line[160];
  snprintf(line, sizeof(line), "shell: SHGetKnownFolderPath -> %s", p8);
  log_line(line);
  return S_OK_;
}

// ---- SHGetMalloc: an IMalloc that is the CRT heap ----
// The SDK-exact IMalloc vtable: Alloc=3, Realloc=4, Free=5, GetSize=6,
// DidAlloc=7, HeapMinimize=8 (IUnknown 0..2).
struct malloc_vt {
  void* fns[9];
};

u32 TRT_GUEST_ABI im_QueryInterface(void*, const void*, void**) { return (u32)E_FAIL_; }
u32 TRT_GUEST_ABI im_AddRef(void*) { return 2; }
u32 TRT_GUEST_ABI im_Release(void*) { return 1; }
void* TRT_GUEST_ABI im_Alloc(void*, u32 bytes) { return bytes ? malloc(bytes) : nullptr; }
void* TRT_GUEST_ABI im_Realloc(void*, void* p, u32 bytes) { return realloc(p, bytes); }
void TRT_GUEST_ABI im_Free(void*, void* p) { free(p); }
u32 TRT_GUEST_ABI im_GetSize(void*, void*) { return 0; }
i32 TRT_GUEST_ABI im_DidAlloc(void*, void*) { return -1; }  // maybe
void TRT_GUEST_ABI im_HeapMinimize(void*) {}

malloc_vt g_malloc_vt = {
    (void*)&im_QueryInterface, (void*)&im_AddRef, (void*)&im_Release,
    (void*)&im_Alloc,          (void*)&im_Realloc, (void*)&im_Free,
    (void*)&im_GetSize,        (void*)&im_DidAlloc, (void*)&im_HeapMinimize};
void* g_malloc_obj = &g_malloc_vt;  // the object's [0] is its vtable pointer

int TRT_GUEST_ABI shell_SHGetMalloc(void** out) {
  if (!out) return E_INVALIDARG_;
  *out = &g_malloc_obj;
  return S_OK_;
}

// ---- ShellExecute: no shell on console; fail visibly and name the verb ----
int TRT_GUEST_ABI shell_ShellExecuteExW(void* info) {
  char line[128];
  snprintf(line, sizeof(line),
           "shell: ShellExecuteExW -> 0 (no shell on console; %s)",
           info ? "info struct present" : "null info");
  log_line(line);
  return 0;  // FALSE: the caller checks and falls back
}

void* TRT_GUEST_ABI shell_ShellExecuteW(void* hwnd, const u16* verb,
                                        const u16* file, const u16* args,
                                        const u16* dir, int show) {
  (void)hwnd; (void)show; (void)dir; (void)args;
  char verb8[32] = "?";
  if (verb) {
    u32 i = 0;
    for (; verb[i] && i < sizeof(verb8) - 1; ++i) verb8[i] = (char)verb[i];
    verb8[i] = 0;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "shell: ShellExecuteW verb=%s -> 2 (SE_ERR_NOASSOC: no shell)",
           verb8);
  log_line(line);
  return (void*)(uintptr_t)2;  // SE_ERR_NOASSOC, a named failure
}

// ---- ILFree/ILCreateFromPath come in pairs; no shell item parsing here ----
void TRT_GUEST_ABI shell_ILFree(void* pidl) { free(pidl); }

// ---- SHCreateDirectoryExW (UnityCrashHandler): create a directory path ----
// Real semantics: creates nested dirs, returns ERROR_ALREADY_EXISTS (183)
// when the leaf already exists, ERROR_SUCCESS (0) on creation.
int TRT_GUEST_ABI shell_SHCreateDirectoryExW(void* hwnd, const u16* path,
                                             void* attr) {
  (void)hwnd; (void)attr;
  char host[1024];
  if (!path || trt_vfs_resolve_utf16(path, host, sizeof(host)) != 0) {
    char line[96];
    snprintf(line, sizeof(line), "shell: SHCreateDirectoryExW -> path unresolvable");
    log_line(line);
    return (int)0x8007000B;  // E_INVALIDARG-shaped: named failure
  }
  char line[224];
  const int rc = shellhelpers::mk_dir(host);
  if (rc == 0) {
    snprintf(line, sizeof(line), "shell: SHCreateDirectoryExW %s -> created", host);
    log_line(line);
    return 0;
  }
  if (errno == EEXIST) {
    snprintf(line, sizeof(line),
             "shell: SHCreateDirectoryExW %s -> ERROR_ALREADY_EXISTS", host);
    log_line(line);
    return 183;
  }
  snprintf(line, sizeof(line),
           "shell: SHCreateDirectoryExW %s -> mkdir failed (errno=%d)", host, errno);
  log_line(line);
  return (int)0x80070005;  // E_ACCESSDENIED-shaped: named failure
}

// ---- CommandLineToArgvW (UnityPlayer + crash handler): standard argv split
// Real semantics: returns pointer to [argv0..argvN, NULL] in one buffer;
// *lpNumArgs gets the count. Program name is argv0.
u16** TRT_GUEST_ABI shell_CommandLineToArgvW(const u16* cmdline, i32* count) {
  if (!count) return nullptr;
  *count = 0;
  if (!cmdline) return nullptr;  // real ole32 faults; we fail named
  // tokenize honoring quotes
  static u16* argv_buf[64];
  static u16 storage[64][512];
  static u16* tail[2] = {nullptr, nullptr};
  u32 nargs = 0;
  u32 i = 0;
  while (cmdline[i] && nargs < 64) {
    while (cmdline[i] == ' ' || cmdline[i] == '\t') ++i;
    if (!cmdline[i]) break;
    u32 j = 0;
    bool in_quotes = false;
    while (cmdline[i] && (in_quotes || (cmdline[i] != ' ' && cmdline[i] != '\t'))) {
      if (cmdline[i] == '"') { in_quotes = !in_quotes; ++i; continue; }
      if (j < 512 - 1) storage[nargs][j++] = cmdline[i];
      ++i;
    }
    storage[nargs][j] = 0;
    argv_buf[nargs] = storage[nargs];
    ++nargs;
  }
  tail[0] = nullptr;
  argv_buf[nargs] = nullptr;
  *count = (i32)nargs;
  char line[96];
  snprintf(line, sizeof(line), "shell: CommandLineToArgvW -> %u args", nargs);
  log_line(line);
  return argv_buf;
}

// ---- SHFileOperationW (UnityPlayer): file copy/move/delete/rename.
// v0: no shell file ops — fail VISIBLY with the op code named so the work
// item is in the log, never silently pretend success.
int TRT_GUEST_ABI shell_SHFileOperationW(void* op) {
  char line[128];
  snprintf(line, sizeof(line),
           "shell: SHFileOperationW -> 0xDE7A (named: no shell file ops yet)");
  log_line(line);
  return 0xDE7A;
}

void TRT_GUEST_ABI shell_DragAcceptFiles(void* hwnd, int accept) {
  (void)hwnd; (void)accept;
}

u32 TRT_GUEST_ABI shell_DragQueryFileW(void* hdrop, u32 idx, u16* buf, u32 cch) {
  (void)hdrop; (void)idx;
  if (buf && cch > 0) buf[0] = 0;
  return 0;
}

int TRT_GUEST_ABI shell_SHCreateItemFromParsingName(const u16* path, void* pbc,
                                                   const void* riid,
                                                   void** ppv) {
  (void)path; (void)pbc; (void)riid;
  if (ppv) *ppv = nullptr;
  return (int)0x80004001u;  // E_NOTIMPL (fail-visible)
}

int TRT_GUEST_ABI shell_SHGetPropertyStoreForWindow(void* hwnd,
                                                    const void* riid,
                                                    void** ppv) {
  (void)hwnd; (void)riid;
  if (ppv) *ppv = nullptr;
  return (int)0x80004001u;
}

int TRT_GUEST_ABI shell_SetCurrentProcessExplicitAppUserModelID(const u16* id) {
  (void)id;
  return 0;  // S_OK (AppContainer AppUserModelID is host-assigned)
}

int TRT_GUEST_ABI shell_Shell_NotifyIconGetRect(const void* nid, void* rect) {
  (void)nid;
  if (rect) memset(rect, 0, 16);
  return (int)0x80004005u;  // E_FAIL: no tray icon surface on Xbox
}

int TRT_GUEST_ABI shell_Shell_NotifyIconW(u32 msg, void* nid) {
  (void)msg; (void)nid;
  return 0;  // FALSE: no system tray on console UWP
}

// ---- shell file dialogs + drag-drop finishing (Cataclysm-BN imports).
// No window manager on console: dialogs answer "cancelled" (fail-visible,
// the API's own named failure), DragFinish just frees the host-side drop. ----
int TRT_GUEST_ABI shell_SHBrowseForFolderW(void* browse) {
  (void)browse;
  return 0;  // NULL pidl = user cancelled the folder picker
}

int TRT_GUEST_ABI shell_SHGetPathFromIDListW(u64 pidl, u16* buf) {
  // A NULL pidl from the cancelled picker above converts to FALSE/empty.
  if (!pidl || !buf) return 0;
  buf[0] = 0;
  return 0;  // FALSE
}

void TRT_GUEST_ABI shell_DragFinish(u64 hdrop) {
  (void)hdrop;  // host-side GlobalFree equivalent: nothing guest-visible
}

}  // namespace

extern "C" {
int trt_bridge_register_shell(void) {
  static const trt_named_fn k_fns[] = {
      {"SHGetFolderPathW", (void*)&shell_SHGetFolderPathW},
      {"SHGetFolderPathA", (void*)&shell_SHGetFolderPathA},
      {"SHGetKnownFolderPath", (void*)&shell_SHGetKnownFolderPath},
      {"SHGetMalloc", (void*)&shell_SHGetMalloc},
      {"ShellExecuteExW", (void*)&shell_ShellExecuteExW},
      {"ShellExecuteW", (void*)&shell_ShellExecuteW},
      {"ILFree", (void*)&shell_ILFree},
      {"SHCreateDirectoryExW", (void*)&shell_SHCreateDirectoryExW},
      {"CommandLineToArgvW", (void*)&shell_CommandLineToArgvW},
      {"SHFileOperationW", (void*)&shell_SHFileOperationW},
      {"DragAcceptFiles", (void*)&shell_DragAcceptFiles},
      {"DragQueryFileW", (void*)&shell_DragQueryFileW},
      {"SHCreateItemFromParsingName", (void*)&shell_SHCreateItemFromParsingName},
      {"SHGetPropertyStoreForWindow", (void*)&shell_SHGetPropertyStoreForWindow},
      {"SetCurrentProcessExplicitAppUserModelID",
       (void*)&shell_SetCurrentProcessExplicitAppUserModelID},
      {"Shell_NotifyIconGetRect", (void*)&shell_Shell_NotifyIconGetRect},
      {"Shell_NotifyIconW", (void*)&shell_Shell_NotifyIconW},
      {"SHBrowseForFolderW", (void*)&shell_SHBrowseForFolderW},
      {"SHGetPathFromIDListW", (void*)&shell_SHGetPathFromIDListW},
      {"DragFinish", (void*)&shell_DragFinish},
  };
  trt_module_register("shell32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  // shfolder.dll is a thin shim onto SHGetFolderPath — same export set
  trt_module_register("shfolder.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
