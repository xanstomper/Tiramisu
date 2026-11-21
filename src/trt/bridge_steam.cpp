// tiramisu/src/trt/bridge_steam.cpp
// Steamworks for the guest - AUTH ONLY.
//
// The project rule: the Steam layer is an auth bridge, NOT a DRM bypass.
// A DRM'd Steam build checks that Steam is running, that the user owns the
// app, and it downloads/validates its own executable parts through the
// Steam client. Tiramisu cannot and will not emulate the client, so:
//
//   - steam_api64.dll exposes the REAL Steamworks surface with the REAL
//     vtable layouts, backed by an anonymous local user ("TiramisuPlayer").
//   - Non-DRM'd games (most GOG/itch titles, some Steam builds launched
//     without the client) boot and their achievements/overlay calls
//     succeed harmlessly.
//   - DRM'd builds call SteamAPI_RestartAppIfNecessary / SteamAPI_Init and
//     then verify their own files - that verification FAILS, the game
//     quits, and the log says exactly which check refused. That is a
//     correct, VISIBLE failure, not a bypass.
//
// Everything is fail-visible: an unimplemented callback names itself.
#include "bridge_steam.h"
#include "input.h"
#include "thunk.h"

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

constexpr int S_OK_ = 0;
constexpr int E_FAIL_ = 0x80004005;

// ---- the app context ----
u32 g_app_id = 0;
int g_initialized = 0;

// ---- callback / callresult registration (the overlay + achievements use it) ----
struct callback_entry {
  void* callback;
  int (*run)(void* self, void* param);
};
callback_entry g_callbacks[32];
int g_callback_count = 0;

} // namespace

extern "C" {

// ---- lifecycle ----

// SteamAPI_Init() / SteamAPI_InitEx(ctx)
int TRT_GUEST_ABI trt_SteamAPI_Init(void) {
  // Isolated diagnostic: no Steam client is available in the UWP runtime.
  g_initialized = 0;
  trt_core_log("trt: SteamAPI_Init -> 0 (diagnostic: no Steam client)");
  return 0;
}
int TRT_GUEST_ABI trt_SteamAPI_InitEx(void* ctx) {
  g_initialized = 1;
  char line[128];
  snprintf(line, sizeof(line), "trt: SteamAPI_InitEx(ctx=%p) -> 1 (auth-only)", ctx);
  trt_core_log(line);
  return 1;
}
int TRT_GUEST_ABI trt_SteamAPI_InitFlat(char* out_err_msg) {
  g_initialized = 1;
  if (out_err_msg) out_err_msg[0] = 0;
  trt_core_log("trt: SteamAPI_InitFlat -> OK (auth-only)");
  return 0;  // k_ESteamAPIInitResult_OK
}
void TRT_GUEST_ABI trt_SteamAPI_Shutdown(void) { g_initialized = 0; }
void trt_steam_set_appid(u32 app_id) { g_app_id = app_id; }

// SteamAPI_RestartAppIfNecessary(appid): DRM'd builds call this FIRST. We
// return false ("no restart needed") so a non-DRM'd build keeps booting; a
// DRM'd build then fails its OWN file verification, visibly.
int TRT_GUEST_ABI trt_SteamAPI_RestartAppIfNecessary(u32 app_id) {
  g_app_id = app_id;
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 RestartAppIfNecessary(appid=%u) -> 0 (auth-only,"
           " no DRM bypass)", app_id);
  trt_core_log(line);
  return 0;
}

// SteamAPI_IsSteamRunning()
int TRT_GUEST_ABI trt_SteamAPI_IsSteamRunning(void) {
  trt_core_log("trt: SteamAPI_IsSteamRunning -> 0 (diagnostic: no Steam client)");
  return 0;
}

// ---- user ----

// SteamUser()->GetSteamID(): an anonymous local user
u64 TRT_GUEST_ABI trt_SteamAPI_GetSteamID(void) {
  // Isolated GoW diagnostic: report no authenticated user to test whether
  // the synthetic SteamID is involved in the pre-main null-read fault.
  trt_core_log("trt: SteamAPI_GetSteamID -> 0000000000000000 (diagnostic no-user)");
  return 0;
}

// ---- friends / overlay / utils: harmless successes ----
int TRT_GUEST_ABI trt_SteamAPI_ActivateGameOverlay(const char* url) {
  (void)url;
  return 1;  // the overlay is a no-op on the console
}
int TRT_GUEST_ABI trt_SteamAPI_SetRichPresence(const char* key,
                                               const char* value) {
  (void)key; (void)value;
  return 1;
}
void TRT_GUEST_ABI trt_SteamAPI_RunCallbacks(void) {
  // the registered callbacks run through the real callback dispatch; a game
  // that waits for an achievement callback gets it when it earns one
  for (int i = 0; i < g_callback_count; ++i) {
    if (g_callbacks[i].run) g_callbacks[i].run(g_callbacks[i].callback, nullptr);
  }
}

// SteamAPI_RegisterCallback(callback, id) / UnregisterCallback
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallback(void* callback, int id) {
  (void)id;
  if (callback && g_callback_count < 32) {
    g_callbacks[g_callback_count].callback = callback;
    g_callbacks[g_callback_count].run = nullptr;  // the game fills the vtable
    ++g_callback_count;
  }
}
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallback(void* callback) {
  (void)callback;
}

// ---- steam apps: the OWNERSHIP question. Fail-visible on purpose ----

// SteamApps()->BIsSubscribedApp(appid): we answer honestly: in the Tiramisu
// auth model there is no Steam license server, so ownership is UNKNOWN and
// a DRM check keyed on it will fail - visibly, with this log line.
int TRT_GUEST_ABI trt_SteamAPI_BIsSubscribedApp(u32 app_id) {
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 BIsSubscribedApp(appid=%u) -> 0 (no license"
           " server: a DRM build will refuse here, by design)", app_id);
  trt_core_log(line);
  return 0;
}
int TRT_GUEST_ABI trt_SteamAPI_BIsDlcInstalled(u32 dlc_id) {
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 BIsDlcInstalled(dlc=%u) -> 0 (no license"
           " server)", dlc_id);
  trt_core_log(line);
  return 0;
}
const char* TRT_GUEST_ABI trt_SteamAPI_GetCurrentGameLanguage(void) {
  return "english";
}

int TRT_GUEST_ABI trt_SteamAPI_GetHSteamUser(void) {
  const int result = g_initialized ? 1 : 0;
  char line[112];
  snprintf(line, sizeof(line), "trt: SteamAPI_GetHSteamUser -> %d", result);
  trt_core_log(line);
  return result;
}
int TRT_GUEST_ABI trt_SteamAPI_GetHSteamPipe(void) { return g_initialized ? 1 : 0; }

void TRT_GUEST_ABI trt_SteamAPI_RegisterCallResult(void* cb, u64 call_id) {
  (void)cb; (void)call_id;
}
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallResult(void* cb, u64 call_id) {
  (void)cb; (void)call_id;
}

void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_Init(void) {}
void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_RunFrame(int pipe) { (void)pipe; }
int TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_GetNextCallback(int pipe, void* msg) {
  (void)pipe; (void)msg;
  return 0;
}
void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_FreeLastCallback(int pipe) { (void)pipe; }

// Flat Steamworks SDK interface singletons (no virtual calls needed when callers
// use flat SteamAPI_ISteam* helpers; vtable slots are always non-null and
// safe so C++ interface callers never jump to 0, while BIsSubscribed* always
// returns honest 0).
void* TRT_GUEST_ABI trt_SteamInternal_CreateInterface(const char* version);

namespace {
struct fake_steam_iface { const void** vtbl; };

static const char* TRT_GUEST_ABI steam_empty_str(void) { return ""; }
static u64 TRT_GUEST_ABI steam_noop_zero(void) {
  // The zero-return fallback is intentionally fail-visible while diagnosing
  // game startup. Record the first callers so an unexpected Steam interface
  // method cannot silently look like a valid successful result.
  static std::atomic<unsigned> calls{0};
  const unsigned n = calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 32) {
    char line[144];
    snprintf(line, sizeof(line),
             "trt: Steam zero stub call=%u caller=%p", n + 1,
             __builtin_return_address(0));
    trt_core_log(line);
  }
  return 0;
}
#define Z4 (const void*)&steam_noop_zero, (const void*)&steam_noop_zero, \
           (const void*)&steam_noop_zero, (const void*)&steam_noop_zero
#define Z8 Z4, Z4
#define Z16 Z8, Z8

static const void* g_steam_safe_vtbl[64] = {Z16, Z16, Z16, Z16};

static unsigned char g_steam_ifaces[25][64] = {};
static void* steam_get_iface(int idx) {
  if (idx < 0 || idx >= 25) idx = 0;
  void** vptr = (void**)&g_steam_ifaces[idx][0];
  if (!*vptr) *vptr = (void*)g_steam_safe_vtbl;
  return (void*)&g_steam_ifaces[idx][0];
}

// ---- ISteamApps ----
int TRT_GUEST_ABI st_apps_BIsSubscribed(void*) {
  trt_core_log("trt: steam ISteamApps::BIsSubscribed -> 0 (no license server)");
  return 0;
}
int TRT_GUEST_ABI st_apps_BIsLowViolence(void*) { return 0; }
int TRT_GUEST_ABI st_apps_BIsCybercafe(void*) { return 0; }
int TRT_GUEST_ABI st_apps_BIsVACBanned(void*) { return 0; }
const char* TRT_GUEST_ABI st_apps_GetLang(void*) { return "english"; }
const char* TRT_GUEST_ABI st_apps_GetAvailLangs(void*) { return "english"; }
int TRT_GUEST_ABI st_apps_BIsSubscribedApp(void*, u32 app_id) {
  return trt_SteamAPI_BIsSubscribedApp(app_id);
}
int TRT_GUEST_ABI st_apps_BIsDlcInstalled(void*, u32 dlc_id) {
  return trt_SteamAPI_BIsDlcInstalled(dlc_id);
}
u32 TRT_GUEST_ABI st_apps_GetAppInstallDir(void*, u32, char* pchFolder, u32 cap) {
  if (pchFolder && cap >= 4) {
    pchFolder[0] = 'Z';
    pchFolder[1] = ':';
    pchFolder[2] = '\\';
    pchFolder[3] = '\0';
    return 3;
  }
  if (pchFolder && cap > 0) pchFolder[0] = '\0';
  return 0;
}
const char* TRT_GUEST_ABI st_apps_GetLaunchQueryParam(void*, const char*) {
  return "";
}
int TRT_GUEST_ABI st_apps_GetAppBuildId(void*) { return 1; }
const void* g_steam_apps_vtbl[64] = {
    (const void*)&st_apps_BIsSubscribed,       // 0
    (const void*)&st_apps_BIsLowViolence,      // 1
    (const void*)&st_apps_BIsCybercafe,        // 2
    (const void*)&st_apps_BIsVACBanned,        // 3
    (const void*)&st_apps_GetLang,             // 4
    (const void*)&st_apps_GetAvailLangs,       // 5
    (const void*)&st_apps_BIsSubscribedApp,    // 6
    (const void*)&st_apps_BIsDlcInstalled,     // 7
    (const void*)&steam_noop_zero,             // 8  BIsSubscribedFromFreeWeekend
    (const void*)&steam_noop_zero,             // 9  BGetDLCDataByIndex
    (const void*)&steam_noop_zero,             // 10 InstallDLC
    (const void*)&steam_noop_zero,             // 11 UninstallDLC
    (const void*)&steam_noop_zero,             // 12 RequestAppProofOfPurchaseKey
    (const void*)&steam_noop_zero,             // 13 GetCurrentBetaName
    (const void*)&steam_noop_zero,             // 14 MarkContentCorrupt
    (const void*)&steam_noop_zero,             // 15 GetInstalledDepots
    (const void*)&st_apps_GetAppInstallDir,    // 16 GetAppInstallDir
    (const void*)&steam_noop_zero,             // 17 BIsAppInstalled
    (const void*)&steam_noop_zero,             // 18 GetAppOwner
    (const void*)&st_apps_GetLaunchQueryParam, // 19 GetLaunchQueryParam
    (const void*)&steam_noop_zero,             // 20 GetDlcDownloadProgress
    (const void*)&st_apps_GetAppBuildId,       // 21 GetAppBuildId
    (const void*)&steam_noop_zero,             // 22
    (const void*)&steam_noop_zero,             // 23
    Z8, Z16, Z16,
};
fake_steam_iface g_steam_apps_obj = {g_steam_apps_vtbl};

// ---- ISteamUser ----
int TRT_GUEST_ABI st_user_GetHSteamUser(void* self) {
  const int result = g_initialized ? 1 : 0;
  char line[128];
  snprintf(line, sizeof(line), "trt: ISteamUser.GetHSteamUser(self=%p) -> %d", self, result);
  trt_core_log(line);
  return result;
}
int TRT_GUEST_ABI st_user_BLoggedOn(void* self) {
  const int result = g_initialized ? 1 : 0;
  char line[128];
  snprintf(line, sizeof(line), "trt: ISteamUser.BLoggedOn(self=%p) -> %d", self, result);
  trt_core_log(line);
  return result;
}
u64 TRT_GUEST_ABI st_user_GetSteamID(void* self) {
  const u64 id = trt_SteamAPI_GetSteamID();
  char line[144];
  snprintf(line, sizeof(line), "trt: ISteamUser.GetSteamID(self=%p) -> %016llx",
           self, (unsigned long long)id);
  trt_core_log(line);
  return id;
}
const void* g_steam_user_vtbl[64] = {
    (const void*)&st_user_GetHSteamUser, // 0
    (const void*)&st_user_BLoggedOn,     // 1
    (const void*)&st_user_GetSteamID,    // 2
    (const void*)&steam_noop_zero,       // 3
    Z4, Z8, Z16, Z16, Z16,
};
fake_steam_iface g_steam_user_obj = {g_steam_user_vtbl};

// ---- ISteamUtils ----
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceAppActive(void*) { return 1; }
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceComputerActive(void*) { return 1; }
int TRT_GUEST_ABI st_utils_GetConnectedUniverse(void*) { return 1; }
u32 TRT_GUEST_ABI st_utils_GetServerRealTime(void*) { return 1700000000u; }
const char* TRT_GUEST_ABI st_utils_GetIPCountry(void*) { return "US"; }
unsigned char TRT_GUEST_ABI st_utils_GetCurrentBatteryPower(void*) { return 255; }
u32 TRT_GUEST_ABI st_utils_GetAppID(void*) { return g_app_id; }
const void* g_steam_utils_vtbl[64] = {
    (const void*)&st_utils_GetSecondsSinceAppActive,      // 0
    (const void*)&st_utils_GetSecondsSinceComputerActive, // 1
    (const void*)&st_utils_GetConnectedUniverse,          // 2
    (const void*)&st_utils_GetServerRealTime,             // 3
    (const void*)&st_utils_GetIPCountry,                  // 4
    (const void*)&steam_noop_zero,                        // 5 GetImageSize
    (const void*)&steam_noop_zero,                        // 6 GetImageRGBA
    (const void*)&steam_noop_zero,                        // 7 GetCSERIPPort
    (const void*)&st_utils_GetCurrentBatteryPower,        // 8 GetCurrentBatteryPower
    (const void*)&st_utils_GetAppID,                      // 9 GetAppID
    (const void*)&steam_noop_zero,                        // 10
    (const void*)&steam_noop_zero,                        // 11
    Z4, Z16, Z16, Z16,
};
fake_steam_iface g_steam_utils_obj = {g_steam_utils_vtbl};

// ---- ISteamFriends ----
const char* TRT_GUEST_ABI st_friends_GetPersonaName(void*) {
  return "TiramisuPlayer";
}
int TRT_GUEST_ABI st_friends_GetPersonaState(void*) { return 0; }
const void* g_steam_friends_vtbl[64] = {
    (const void*)&st_friends_GetPersonaName,  // 0 GetPersonaName
    (const void*)&steam_noop_zero,            // 1 SetPersonaName
    (const void*)&st_friends_GetPersonaState, // 2 GetPersonaState
    (const void*)&steam_noop_zero,            // 3 GetFriendCount
    Z4, Z8, Z16, Z16, Z16,
};
fake_steam_iface g_steam_friends_obj = {g_steam_friends_vtbl};

// ---- ISteamRemoteStorage (VFS-backed local cloud saves) ----
constexpr int kSteamCloudMaxFiles = 256;
constexpr i32 kSteamCloudMaxBytes = 64 * 1024 * 1024;
std::mutex g_steam_rs_mu;
int g_steam_cloud_enabled = 1;
u32 g_steam_rs_loaded_appid = 0xffffffffu;
char g_steam_rs_names[kSteamCloudMaxFiles][260] = {};
int g_steam_rs_count = 0;

static int rs_normalize_rel(const char* name, char* out, size_t cap) {
  if (!name || !name[0] || cap < 2) return -1;
  size_t j = 0;
  const char* p = name;
  while (*p == '/' || *p == '\\') ++p;
  if (!*p) return -1;
  for (; *p; ++p) {
    if (*p == ':') return -1;
    char c = (*p == '/') ? '\\' : *p;
    if (c == '\\' && j > 0 && out[j - 1] == '\\') continue;
    if (j + 1 >= cap) return -1;
    out[j++] = c;
  }
  while (j > 0 && out[j - 1] == '\\') --j;
  out[j] = '\0';
  if (!j || out[0] == '.') return -1;
  for (size_t i = 0; i < j;) {
    size_t s = i;
    while (i < j && out[i] != '\\') ++i;
    size_t len = i - s;
    if (len == 0 || (len == 1 && out[s] == '.') ||
        (len == 2 && out[s] == '.' && out[s + 1] == '.'))
      return -1;
    if (i < j) ++i;
  }
  return 0;
}

static int rs_resolve_raw(const char* rel, int for_write,
                          char* host_out, size_t host_cap) {
  char guest[512];
  snprintf(guest, sizeof(guest),
           "C:\\users\\player\\AppData\\Local\\Tiramisu\\SteamCloud\\%u\\%s",
           (unsigned)g_app_id, rel);
  u16 wguest[512];
  size_t k = 0;
  for (; guest[k] && k + 1 < 512; ++k) wguest[k] = (unsigned char)guest[k];
  wguest[k] = 0;
  if (trt_vfs_resolve_utf16_rw(wguest, for_write, host_out, host_cap) != 0)
    return -1;
  if (for_write) trt_vfs_ensure_parents(host_out);
  return 0;
}

static int rs_resolve_host(const char* name, int for_write,
                           char* norm_out, size_t norm_cap,
                           char* host_out, size_t host_cap) {
  char norm[260];
  if (rs_normalize_rel(name, norm, sizeof(norm)) != 0) return -1;
  if (norm_out && norm_cap > 0) snprintf(norm_out, norm_cap, "%s", norm);
  return rs_resolve_raw(norm, for_write, host_out, host_cap);
}

static void rs_save_index_locked(void) {
  char idx_host[512];
  if (rs_resolve_raw(".steamcloud_index", 1, idx_host, sizeof(idx_host)) != 0)
    return;
  FILE* f = fopen(idx_host, "wb");
  if (!f) return;
  for (int i = 0; i < g_steam_rs_count; ++i) {
    if (g_steam_rs_names[i][0]) fprintf(f, "%s\n", g_steam_rs_names[i]);
  }
  fclose(f);
}

static void rs_load_index_locked(void) {
  if (g_steam_rs_loaded_appid == g_app_id) return;
  g_steam_rs_loaded_appid = g_app_id;
  g_steam_rs_count = 0;
  char idx_host[512];
  if (rs_resolve_raw(".steamcloud_index", 0, idx_host, sizeof(idx_host)) != 0)
    return;
  FILE* f = fopen(idx_host, "rb");
  if (!f) return;
  char line[260];
  while (g_steam_rs_count < kSteamCloudMaxFiles &&
         fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    char norm[260], fhost[512];
    int is_dir = 0;
    if (len > 0 &&
        rs_resolve_host(line, 0, norm, sizeof(norm), fhost, sizeof(fhost)) == 0 &&
        trt_vfs_stat(fhost, &is_dir, nullptr) == 0 && !is_dir) {
      snprintf(g_steam_rs_names[g_steam_rs_count++], 260, "%s", norm);
    }
  }
  fclose(f);
}

static void rs_index_add_locked(const char* norm) {
  rs_load_index_locked();
  for (int i = 0; i < g_steam_rs_count; ++i) {
    if (strcmp(g_steam_rs_names[i], norm) == 0) return;
  }
  if (g_steam_rs_count < kSteamCloudMaxFiles) {
    snprintf(g_steam_rs_names[g_steam_rs_count++], 260, "%s", norm);
    rs_save_index_locked();
  }
}

static void rs_index_remove_locked(const char* norm) {
  rs_load_index_locked();
  for (int i = 0; i < g_steam_rs_count; ++i) {
    if (strcmp(g_steam_rs_names[i], norm) == 0) {
      for (int j = i + 1; j < g_steam_rs_count; ++j) {
        snprintf(g_steam_rs_names[j - 1], 260, "%s", g_steam_rs_names[j]);
      }
      --g_steam_rs_count;
      rs_save_index_locked();
      return;
    }
  }
}

static int TRT_GUEST_ABI st_rs_FileWrite(void*, const char* pchFile,
                                         const void* pvData, i32 cubData) {
  if (cubData < 0 || cubData > kSteamCloudMaxBytes) return 0;
  if (cubData > 0 && !pvData) return 0;
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 1, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  FILE* f = fopen(host, "wb");
  if (!f) return 0;
  size_t wrote = cubData > 0 ? fwrite(pvData, 1, (size_t)cubData, f) : 0;
  fclose(f);
  if (wrote != (size_t)cubData) return 0;
  rs_index_add_locked(norm);
  return 1;
}

static i32 TRT_GUEST_ABI st_rs_FileRead(void*, const char* pchFile,
                                        void* pvData, i32 cubDataToRead) {
  if (!pvData || cubDataToRead <= 0) return 0;
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 0, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  FILE* f = fopen(host, "rb");
  if (!f) return 0;
  size_t got = fread(pvData, 1, (size_t)cubDataToRead, f);
  fclose(f);
  rs_index_add_locked(norm);
  return (i32)got;
}

static int TRT_GUEST_ABI st_rs_FileForget(void*, const char*) { return 1; }

static int TRT_GUEST_ABI st_rs_FileDelete(void*, const char* pchFile) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 1, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  int rc = (remove(host) == 0) ? 1 : 0;
  rs_index_remove_locked(norm);
  return rc;
}

static int TRT_GUEST_ABI st_rs_FileExists(void*, const char* pchFile) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 0, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  int is_dir = 0;
  if (trt_vfs_stat(host, &is_dir, nullptr) == 0 && !is_dir) {
    rs_index_add_locked(norm);
    return 1;
  }
  return 0;
}

static int TRT_GUEST_ABI st_rs_FilePersisted(void* self, const char* pchFile) {
  return st_rs_FileExists(self, pchFile);
}

static i32 TRT_GUEST_ABI st_rs_GetFileSize(void*, const char* pchFile) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 0, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  int is_dir = 0;
  u64 sz = 0;
  if (trt_vfs_stat(host, &is_dir, &sz) == 0 && !is_dir) {
    return sz > (u64)kSteamCloudMaxBytes ? kSteamCloudMaxBytes : (i32)sz;
  }
  return 0;
}

static i64 TRT_GUEST_ABI st_rs_GetFileTimestamp(void* self, const char* pchFile) {
  if (!st_rs_FileExists(self, pchFile)) return 0;
  return (i64)time(nullptr);
}

static i32 TRT_GUEST_ABI st_rs_GetFileCount(void*) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  rs_load_index_locked();
  return g_steam_rs_count;
}

static const char* TRT_GUEST_ABI st_rs_GetFileNameAndSize(void*, int iFile,
                                                          int* pnSize) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  rs_load_index_locked();
  if (iFile < 0 || iFile >= g_steam_rs_count) {
    if (pnSize) *pnSize = 0;
    return "";
  }
  static char s_name_ret[260];
  snprintf(s_name_ret, sizeof(s_name_ret), "%s", g_steam_rs_names[iFile]);
  char host[512];
  int is_dir = 0;
  u64 sz = 0;
  if (pnSize) {
    if (rs_resolve_host(s_name_ret, 0, nullptr, 0, host, sizeof(host)) == 0 &&
        trt_vfs_stat(host, &is_dir, &sz) == 0 && !is_dir) {
      *pnSize = sz > (u64)kSteamCloudMaxBytes ? kSteamCloudMaxBytes : (int)sz;
    } else {
      *pnSize = 0;
    }
  }
  return s_name_ret;
}

static int TRT_GUEST_ABI st_rs_GetQuota(void*, u64* total, u64* avail) {
  if (total) *total = 100ULL * 1024ULL * 1024ULL;
  if (avail) *avail = 100ULL * 1024ULL * 1024ULL;
  return 1;
}

static int TRT_GUEST_ABI st_rs_IsCloudEnabledForAccount(void*) { return 1; }
static int TRT_GUEST_ABI st_rs_IsCloudEnabledForApp(void*) {
  return g_steam_cloud_enabled;
}
static void TRT_GUEST_ABI st_rs_SetCloudEnabledForApp(void*, int enabled) {
  g_steam_cloud_enabled = enabled ? 1 : 0;
}

const void* g_steam_rs_vtbl[64] = {
    (const void*)&st_rs_FileWrite,                // 0
    (const void*)&st_rs_FileRead,                 // 1
    (const void*)&steam_noop_zero,                // 2 FileWriteAsync
    (const void*)&steam_noop_zero,                // 3 FileReadAsync
    (const void*)&steam_noop_zero,                // 4 FileReadAsyncComplete
    (const void*)&st_rs_FileForget,               // 5
    (const void*)&st_rs_FileDelete,               // 6
    (const void*)&steam_noop_zero,                // 7 FileShare
    (const void*)&steam_noop_zero,                // 8 SetSyncPlatforms
    (const void*)&steam_noop_zero,                // 9 FileWriteStreamOpen
    (const void*)&steam_noop_zero,                // 10 FileWriteStreamWriteChunk
    (const void*)&steam_noop_zero,                // 11 FileWriteStreamClose
    (const void*)&steam_noop_zero,                // 12 FileWriteStreamCancel
    (const void*)&st_rs_FileExists,               // 13
    (const void*)&st_rs_FilePersisted,            // 14
    (const void*)&st_rs_GetFileSize,              // 15
    (const void*)&st_rs_GetFileTimestamp,         // 16
    (const void*)&steam_noop_zero,                // 17 GetSyncPlatforms
    (const void*)&st_rs_GetFileCount,             // 18
    (const void*)&st_rs_GetFileNameAndSize,       // 19
    (const void*)&st_rs_GetQuota,                 // 20
    (const void*)&st_rs_IsCloudEnabledForAccount, // 21
    (const void*)&st_rs_IsCloudEnabledForApp,     // 22
    (const void*)&st_rs_SetCloudEnabledForApp,    // 23
    Z8, Z16, Z16,
};
fake_steam_iface g_steam_rs_obj = {g_steam_rs_vtbl};

// ---- ISteamInput (backed by 4-slot XInput) ----
constexpr u64 kSteamInputHandleBase = 0x545254494E000001ULL;
static int input_handle_to_idx(u64 handle, u32* out_idx) {
  if (handle < kSteamInputHandleBase) return 0;
  u64 d = handle - kSteamInputHandleBase;
  if (d >= 4) return 0;
  *out_idx = (u32)d;
  return 1;
}

static int TRT_GUEST_ABI st_input_Init(void*, int /*bExplicitlyCallRunFrame*/) {
  return 1;
}
static int TRT_GUEST_ABI st_input_Shutdown(void*) {
  for (u32 i = 0; i < 4; ++i) trt_pad_set_vibration(i, 0, 0);
  return 1;
}
static void TRT_GUEST_ABI st_input_RunFrame(void*, int /*bReserved*/) {}
static int TRT_GUEST_ABI st_input_GetConnectedControllers(void*, u64* handles_out) {
  int count = 0;
  for (u32 i = 0; i < 4; ++i) {
    if (trt_pad_connected(i)) {
      if (handles_out) handles_out[count] = kSteamInputHandleBase + i;
      ++count;
    }
  }
  return count;
}
static u64 TRT_GUEST_ABI st_input_GetControllerForGamepadIndex(void*, int index) {
  if (index < 0 || index >= 4) return 0;
  if (!trt_pad_connected((u32)index)) return 0;
  return kSteamInputHandleBase + (u64)index;
}
static int TRT_GUEST_ABI st_input_GetGamepadIndexForController(void*, u64 handle) {
  u32 idx = 0;
  if (!input_handle_to_idx(handle, &idx)) return -1;
  return trt_pad_connected(idx) ? (int)idx : -1;
}
static int TRT_GUEST_ABI st_input_GetInputTypeForHandle(void*, u64 handle) {
  u32 idx = 0;
  if (!input_handle_to_idx(handle, &idx)) return 0;
  // k_ESteamInputType_XBoxOneController = 3
  return trt_pad_connected(idx) ? 3 : 0;
}
static void TRT_GUEST_ABI st_input_TriggerVibration(void*, u64 handle,
                                                  u16 left_speed,
                                                  u16 right_speed) {
  u32 idx = 0;
  if (input_handle_to_idx(handle, &idx)) {
    trt_pad_set_vibration(idx, left_speed, right_speed);
  }
}
static void TRT_GUEST_ABI st_input_TriggerVibrationExtended(
    void* self, u64 handle, u16 left, u16 right, u16 /*lt*/, u16 /*rt*/) {
  st_input_TriggerVibration(self, handle, left, right);
}
const void* g_steam_input_vtbl[64] = {
    (const void*)&st_input_Init,                        // 0
    (const void*)&st_input_Shutdown,                    // 1
    (const void*)&steam_noop_zero,                      // 2 SetInputActionManifestFilePath
    (const void*)&st_input_RunFrame,                    // 3
    (const void*)&steam_noop_zero,                      // 4 BWaitForData
    (const void*)&steam_noop_zero,                      // 5 BNewDataAvailable
    (const void*)&st_input_GetConnectedControllers,     // 6
    Z8, Z16, Z16, Z16,
};
fake_steam_iface g_steam_input_obj = {g_steam_input_vtbl};

// ---- ISteamClient ----
static int TRT_GUEST_ABI st_client_CreateSteamPipe(void*) { return 1; }
static int TRT_GUEST_ABI st_client_BReleaseSteamPipe(void*, int /*pipe*/) { return 1; }
static int TRT_GUEST_ABI st_client_ConnectToGlobalUser(void*, int /*pipe*/) { return 1; }
static int TRT_GUEST_ABI st_client_CreateLocalUser(void*, int* pipe, int /*acct*/) {
  if (pipe) *pipe = 1;
  return 1;
}
static void TRT_GUEST_ABI st_client_ReleaseUser(void*, int, int) {}
static void* TRT_GUEST_ABI st_client_GetISteamUser(void*, int, int, const char*) {
  return &g_steam_user_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamGameServer(void*, int, int, const char*) {
  return steam_get_iface(19);
}
static void TRT_GUEST_ABI st_client_SetLocalIPBinding(void*, const void*, u16) {}
static void* TRT_GUEST_ABI st_client_GetISteamFriends(void*, int, int, const char*) {
  return &g_steam_friends_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamUtils(void*, int, const char*) {
  return &g_steam_utils_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamMatchmaking(void*, int, int, const char*) {
  return steam_get_iface(1);
}
static void* TRT_GUEST_ABI st_client_GetISteamMatchmakingServers(void*, int, int, const char*) {
  return steam_get_iface(2);
}
static void* TRT_GUEST_ABI st_client_GetISteamGenericInterface(void*, int, int, const char* ver) {
  return trt_SteamInternal_CreateInterface(ver);
}
static void* TRT_GUEST_ABI st_client_GetISteamUserStats(void*, int, int, const char*) {
  return steam_get_iface(5);
}
static void* TRT_GUEST_ABI st_client_GetISteamGameServerStats(void*, int, int, const char*) {
  return steam_get_iface(21);
}
static void* TRT_GUEST_ABI st_client_GetISteamApps(void*, int, int, const char*) {
  return &g_steam_apps_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamNetworking(void*, int, int, const char*) {
  return steam_get_iface(6);
}
static void* TRT_GUEST_ABI st_client_GetISteamRemoteStorage(void*, int, int, const char*) {
  return &g_steam_rs_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamScreenshots(void*, int, int, const char*) {
  return steam_get_iface(7);
}
static void* TRT_GUEST_ABI st_client_GetISteamGameSearch(void*, int, int, const char*) {
  return steam_get_iface(22);
}
static void TRT_GUEST_ABI st_client_RunFrame(void*) {}
static u32 TRT_GUEST_ABI st_client_GetIPCCallCount(void*) { return 1; }
static void TRT_GUEST_ABI st_client_SetWarningMessageHook(void*, void*) {}
static int TRT_GUEST_ABI st_client_BShutdownIfAllPipesClosed(void*) { return 1; }
static void* TRT_GUEST_ABI st_client_GetISteamHTTP(void*, int, int, const char*) {
  return steam_get_iface(9);
}
static void* TRT_GUEST_ABI st_client_GetISteamUnifiedMessages(void*, int, int, const char*) {
  return steam_get_iface(23);
}
static void* TRT_GUEST_ABI st_client_GetISteamController(void*, int, int, const char*) {
  return &g_steam_input_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamUGC(void*, int, int, const char*) {
  return steam_get_iface(11);
}
static void* TRT_GUEST_ABI st_client_GetISteamAppList(void*, int, int, const char*) {
  return steam_get_iface(14);
}
static void* TRT_GUEST_ABI st_client_GetISteamMusic(void*, int, int, const char*) {
  return steam_get_iface(8);
}
static void* TRT_GUEST_ABI st_client_GetISteamMusicRemote(void*, int, int,
                                                        const char*) {
  return steam_get_iface(24);  // Stable empty interface; all operations return false.
}
static void* TRT_GUEST_ABI st_client_GetISteamHTMLSurface(void*, int, int, const char*) {
  return steam_get_iface(12);
}
static void* TRT_GUEST_ABI st_client_GetISteamInventory(void*, int, int, const char*) {
  return steam_get_iface(13);
}
static void* TRT_GUEST_ABI st_client_GetISteamVideo(void*, int, int, const char*) {
  return steam_get_iface(15);
}
static void* TRT_GUEST_ABI st_client_GetISteamParentalSettings(void*, int, int, const char*) {
  return steam_get_iface(16);
}
static void* TRT_GUEST_ABI st_client_GetISteamInput(void*, int, int, const char*) {
  return &g_steam_input_obj;
}
static void* TRT_GUEST_ABI st_client_GetISteamParties(void*, int, int, const char*) {
  return steam_get_iface(3);
}
static void* TRT_GUEST_ABI st_client_GetISteamRemotePlay(void*, int, int, const char*) {
  return steam_get_iface(17);
}

const void* g_steam_client_vtbl[64] = {
    (const void*)&st_client_CreateSteamPipe,            // 0
    (const void*)&st_client_BReleaseSteamPipe,          // 1
    (const void*)&st_client_ConnectToGlobalUser,        // 2
    (const void*)&st_client_CreateLocalUser,            // 3
    (const void*)&st_client_ReleaseUser,                // 4
    (const void*)&st_client_GetISteamUser,              // 5
    (const void*)&st_client_GetISteamGameServer,        // 6
    (const void*)&st_client_SetLocalIPBinding,          // 7
    (const void*)&st_client_GetISteamFriends,           // 8
    (const void*)&st_client_GetISteamUtils,             // 9
    (const void*)&st_client_GetISteamMatchmaking,       // 10
    (const void*)&st_client_GetISteamMatchmakingServers,// 11
    (const void*)&st_client_GetISteamGenericInterface,  // 12
    (const void*)&st_client_GetISteamUserStats,         // 13
    (const void*)&st_client_GetISteamGameServerStats,   // 14
    (const void*)&st_client_GetISteamApps,              // 15
    (const void*)&st_client_GetISteamNetworking,        // 16
    (const void*)&st_client_GetISteamRemoteStorage,     // 17
    (const void*)&st_client_GetISteamScreenshots,       // 18
    (const void*)&st_client_GetISteamGameSearch,        // 19
    (const void*)&st_client_RunFrame,                   // 20
    (const void*)&st_client_GetIPCCallCount,            // 21
    (const void*)&st_client_SetWarningMessageHook,    // 22
    (const void*)&st_client_BShutdownIfAllPipesClosed,// 23
    (const void*)&st_client_GetISteamHTTP,              // 24
    (const void*)&st_client_GetISteamUnifiedMessages,   // 25
    (const void*)&st_client_GetISteamController,        // 26
    (const void*)&st_client_GetISteamUGC,               // 27
    (const void*)&st_client_GetISteamAppList,           // 28
    (const void*)&st_client_GetISteamMusic,             // 29
    (const void*)&st_client_GetISteamMusicRemote,       // 30
    (const void*)&st_client_GetISteamHTMLSurface,       // 31
    (const void*)&steam_noop_zero,                      // 32 deprecated
    (const void*)&steam_noop_zero,                      // 33 deprecated
    (const void*)&steam_noop_zero,                      // 34 callback check
    (const void*)&st_client_GetISteamInventory,         // 35
    (const void*)&st_client_GetISteamVideo,             // 36
    (const void*)&st_client_GetISteamParentalSettings,  // 37
    (const void*)&st_client_GetISteamInput,             // 38
    (const void*)&st_client_GetISteamParties,           // 39
    (const void*)&st_client_GetISteamRemotePlay,        // 40
    (const void*)&steam_noop_zero,                      // 41
    (const void*)&steam_noop_zero,                      // 42
    (const void*)&steam_noop_zero,                      // 43
    (const void*)&steam_noop_zero,                      // 44
    (const void*)&steam_noop_zero,                      // 45
    (const void*)&steam_noop_zero,                      // 46
    (const void*)&steam_noop_zero,                      // 47
    Z16,
};
fake_steam_iface g_steam_client_obj = {g_steam_client_vtbl};

#undef Z16
#undef Z8
#undef Z4
} // namespace

void* TRT_GUEST_ABI trt_SteamInternal_CreateInterface(const char* version) {
  char call_line[160];
  snprintf(call_line, sizeof(call_line), "trt: SteamInternal_CreateInterface('%s')",
           version ? version : "(null)");
  trt_core_log(call_line);
  if (!version) return steam_get_iface(20);
  if (strncmp(version, "SteamClient", 11) == 0 ||
      strncmp(version, "STEAMCLIENT_INTERFACE_VERSION", 29) == 0)
    return &g_steam_client_obj;
  if (strncmp(version, "STEAMAPPS_INTERFACE_VERSION", 27) == 0 ||
      strncmp(version, "SteamApps", 9) == 0)
    return &g_steam_apps_obj;
  if ((strncmp(version, "SteamUser", 9) == 0 &&
       strncmp(version, "SteamUserStats", 14) != 0) ||
      (strncmp(version, "STEAMUSER_INTERFACE_VERSION", 27) == 0))
    return &g_steam_user_obj;
  if (strncmp(version, "SteamUserStats", 14) == 0 ||
      strncmp(version, "STEAMUSERSTATS_INTERFACE_VERSION", 32) == 0)
    return steam_get_iface(5);
  if (strncmp(version, "SteamUtils", 10) == 0 ||
      strncmp(version, "STEAMUTILS_INTERFACE_VERSION", 28) == 0)
    return &g_steam_utils_obj;
  if (strncmp(version, "SteamFriends", 12) == 0 ||
      strncmp(version, "STEAMFRIENDS_INTERFACE_VERSION", 30) == 0)
    return &g_steam_friends_obj;
  if (strncmp(version, "SteamRemoteStorage", 18) == 0 ||
      strncmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION", 36) == 0)
    return &g_steam_rs_obj;
  if (strncmp(version, "SteamMusicRemote", 16) == 0 ||
      strncmp(version, "STEAMMUSICREMOTE_INTERFACE_VERSION", 34) == 0)
    return steam_get_iface(24);
  if (strncmp(version, "SteamInput", 10) == 0 ||
      strncmp(version, "STEAMINPUT_INTERFACE_VERSION", 28) == 0 ||
      strncmp(version, "SteamController", 15) == 0 ||
      strncmp(version, "STEAMCONTROLLER_INTERFACE_VERSION", 33) == 0)
    return &g_steam_input_obj;
  char line[128];
  snprintf(line, sizeof(line),
           "trt: SteamInternal_CreateInterface('%s') -> auth-only iface",
           version);
  trt_core_log(line);
  return steam_get_iface(20);
}

void* TRT_GUEST_ABI trt_SteamInternal_FindOrCreateUserInterface(int user,
                                                                const char* ver) {
  void* result = trt_SteamInternal_CreateInterface(ver);
  char line[192];
  snprintf(line, sizeof(line),
           "trt: FindOrCreateUserInterface(user=%d ver=%s) -> %p",
           user, ver ? ver : "(null)", result);
  trt_core_log(line);
  return result;
}

void* TRT_GUEST_ABI trt_SteamInternal_FindOrCreateGameServerInterface(int user,
                                                                      const char* ver) {
  (void)user;
  return trt_SteamInternal_CreateInterface(ver);
}

void* TRT_GUEST_ABI trt_SteamInternal_ContextInit(void* ctx_init_data) {
  if (!ctx_init_data) {
    trt_core_log("trt: SteamInternal_ContextInit(data=null) -> null");
    return nullptr;
  }
  struct init_block {
    void (TRT_GUEST_ABI *init_fn)(void*);
    uintptr_t counter;
    u8 ctx_storage[1];
  };
  init_block* blk = (init_block*)ctx_init_data;
  void* ctx = (void*)blk->ctx_storage;
  void* init_fn = (void*)blk->init_fn;
  const uintptr_t before = blk->counter;
  u64 ctx_before[4] = {};
  memcpy(ctx_before, ctx, sizeof(ctx_before));
  char ctx_before_line[240];
  snprintf(ctx_before_line, sizeof(ctx_before_line),
           "trt: Steam context bytes before init: %016llx %016llx %016llx %016llx",
           (unsigned long long)ctx_before[0], (unsigned long long)ctx_before[1],
           (unsigned long long)ctx_before[2], (unsigned long long)ctx_before[3]);
  trt_core_log(ctx_before_line);
  if (before != 1) {
    blk->counter = 1;
    if (blk->init_fn) {
      char before_line[160];
      snprintf(before_line, sizeof(before_line),
               "trt: SteamInternal_ContextInit invoking init_fn=%p ctx=%p",
               init_fn, ctx);
      trt_core_log(before_line);
      blk->init_fn(ctx);
      trt_core_log("trt: SteamInternal_ContextInit init_fn returned");
    } else {
      trt_core_log("trt: SteamInternal_ContextInit has null init_fn");
    }
  } else {
    trt_core_log("trt: SteamInternal_ContextInit skipped init_fn (counter already 1)");
  }
  u64 ctx_after[4] = {};
  memcpy(ctx_after, ctx, sizeof(ctx_after));
  char ctx_after_line[240];
  snprintf(ctx_after_line, sizeof(ctx_after_line),
           "trt: Steam context bytes after init: %016llx %016llx %016llx %016llx",
           (unsigned long long)ctx_after[0], (unsigned long long)ctx_after[1],
           (unsigned long long)ctx_after[2], (unsigned long long)ctx_after[3]);
  trt_core_log(ctx_after_line);
  char line[224];
  snprintf(line, sizeof(line),
           "trt: SteamInternal_ContextInit(data=%p init_fn=%p counter=%llu->%llu ctx=%p)",
           ctx_init_data, init_fn, (unsigned long long)before,
           (unsigned long long)blk->counter, ctx);
  trt_core_log(line);
  return ctx;
}

void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v021(void) {
  trt_core_log("trt: SteamAPI_SteamUser_v021 -> local auth-only ISteamUser");
  return &g_steam_user_obj;
}
void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v023(void) {
  trt_core_log("trt: SteamAPI_SteamUser_v023 -> local auth-only ISteamUser");
  return &g_steam_user_obj;
}
void* TRT_GUEST_ABI trt_SteamAPI_SteamApps_v008(void) { return &g_steam_apps_obj; }
void* TRT_GUEST_ABI trt_SteamAPI_SteamUtils_v010(void) { return &g_steam_utils_obj; }
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsSubscribedApp(void*, u32 app_id) {
  return trt_SteamAPI_BIsSubscribedApp(app_id);
}
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsDlcInstalled(void*, u32 dlc_id) {
  return trt_SteamAPI_BIsDlcInstalled(dlc_id);
}
const char* TRT_GUEST_ABI trt_SteamAPI_ISteamApps_GetCurrentGameLanguage(void*) {
  return "english";
}
u32 TRT_GUEST_ABI trt_SteamAPI_ISteamUtils_GetAppID(void*) { return g_app_id; }

#define STEAM_IFACE_FN(idx) \
  static void* TRT_GUEST_ABI steam_iface_##idx(void) { return steam_get_iface(idx); }
STEAM_IFACE_FN(1)  STEAM_IFACE_FN(2)  STEAM_IFACE_FN(3)  STEAM_IFACE_FN(5)
STEAM_IFACE_FN(6)  STEAM_IFACE_FN(7)  STEAM_IFACE_FN(8)  STEAM_IFACE_FN(9)
STEAM_IFACE_FN(11) STEAM_IFACE_FN(12) STEAM_IFACE_FN(13) STEAM_IFACE_FN(14)
STEAM_IFACE_FN(15) STEAM_IFACE_FN(16) STEAM_IFACE_FN(17) STEAM_IFACE_FN(18)
STEAM_IFACE_FN(19) STEAM_IFACE_FN(20) STEAM_IFACE_FN(24)
#undef STEAM_IFACE_FN
static void* TRT_GUEST_ABI steam_apps_obj_ptr(void) {
  return &g_steam_apps_obj;
}
static void* TRT_GUEST_ABI steam_user_obj_ptr(void) {
  return &g_steam_user_obj;
}
static void* TRT_GUEST_ABI steam_utils_obj_ptr(void) {
  return &g_steam_utils_obj;
}
static void* TRT_GUEST_ABI steam_friends_obj_ptr(void) {
  return &g_steam_friends_obj;
}
static void* TRT_GUEST_ABI steam_rs_obj_ptr(void) {
  return &g_steam_rs_obj;
}
static void* TRT_GUEST_ABI steam_input_obj_ptr(void) {
  return &g_steam_input_obj;
}
static void* TRT_GUEST_ABI steam_client_obj_ptr(void) {
  return &g_steam_client_obj;
}

static int TRT_GUEST_ABI steam_internal_init(const char* /*ver*/, char* pOutErrMsg) {
  if (pOutErrMsg) pOutErrMsg[0] = '\0';
  g_initialized = 1;
  return 0; // k_ESteamAPIInitResult_OK
}
static void TRT_GUEST_ABI steam_release_current_thread_memory(void) {}

// Non-DRM ownership & subscription checks: always refuse bypass (0 or NoAuth=2)
static int TRT_GUEST_ABI steam_apps_false(void*) { return 0; }
static int TRT_GUEST_ABI steam_apps_false_u32(void*, u32) { return 0; }
static int TRT_GUEST_ABI steam_apps_is_timed_trial(void*, u32* allowed, u32* played) {
  if (allowed) *allowed = 0;
  if (played) *played = 0;
  return 0;
}
static u64 TRT_GUEST_ABI steam_apps_get_app_owner(void*) { return 0; }
static int TRT_GUEST_ABI steam_user_has_license(void*, u64 /*steam_id*/, u32 /*app_id*/) {
  // EUserHasLicenseForAppResult: 0=HasLicense, 1=DoesNotHaveLicense, 2=NoAuth
  return 2;
}
static int TRT_GUEST_ABI steam_user_b_logged_on(void*) {
  return g_initialized ? 1 : 0;
}
static u64 TRT_GUEST_ABI steam_user_get_steam_id(void*) {
  return 0x0110000100000001ULL;
}
static const char* TRT_GUEST_ABI steam_friends_get_persona_name(void*) {
  return "TiramisuPlayer";
}
static int TRT_GUEST_ABI steam_friends_get_persona_state(void*) { return 0; }
static const char* TRT_GUEST_ABI steam_const_english(void*) { return "english"; }
static const char* TRT_GUEST_ABI steam_utils_get_ip_country(void*) { return "US"; }
static unsigned char TRT_GUEST_ABI steam_utils_get_battery(void*) { return 255; }
static u32 TRT_GUEST_ABI steam_utils_get_server_time(void*) { return 1700000000u; }
static int TRT_GUEST_ABI steam_utils_get_universe(void*) { return 1; }
static u32 TRT_GUEST_ABI steam_apps_get_install_dir(void*, u32 /*app_id*/,
                                                    char* pchFolder, u32 cap) {
  if (pchFolder && cap >= 4) {
    pchFolder[0] = 'Z';
    pchFolder[1] = ':';
    pchFolder[2] = '\\';
    pchFolder[3] = '\0';
    return 3;
  }
  if (pchFolder && cap > 0) pchFolder[0] = '\0';
  return 0;
}
static int TRT_GUEST_ABI steam_apps_get_build_id(void*) { return 1; }

struct steam_ret16 { unsigned char b[16]; };
struct steam_ret40 { unsigned char b[40]; };
static steam_ret16 TRT_GUEST_ABI steam_zero16(void) { steam_ret16 r = {}; return r; }
static steam_ret40 TRT_GUEST_ABI steam_zero40(void) { steam_ret40 r = {}; return r; }

static int steam_streq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static const trt_named_fn k_steam_extra_fns[] = {
    {"SteamInternal_SteamAPI_Init", (void*)steam_internal_init},
    {"SteamAPI_ReleaseCurrentThreadMemory", (void*)steam_release_current_thread_memory},
    {"SteamClient", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamClient_v017", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamClient_v018", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamClient_v019", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamClient_v020", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamClient_v021", (void*)steam_client_obj_ptr},
    {"SteamAPI_SteamUser_v019", (void*)steam_user_obj_ptr},
    {"SteamAPI_SteamUser_v020", (void*)steam_user_obj_ptr},
    {"SteamAPI_SteamApps_v007", (void*)steam_apps_obj_ptr},
    {"SteamAPI_SteamApps_v009", (void*)steam_apps_obj_ptr},
    {"SteamAPI_SteamUtils_v008", (void*)steam_utils_obj_ptr},
    {"SteamAPI_SteamUtils_v009", (void*)steam_utils_obj_ptr},
    {"SteamAPI_SteamFriends_v015", (void*)steam_friends_obj_ptr},
    {"SteamAPI_SteamFriends_v017", (void*)steam_friends_obj_ptr},
    {"SteamAPI_SteamFriends_v018", (void*)steam_friends_obj_ptr},
    {"SteamAPI_SteamMatchmaking_v009", (void*)steam_iface_1},
    {"SteamAPI_SteamMatchmakingServers_v002", (void*)steam_iface_2},
    {"SteamAPI_SteamParties_v002", (void*)steam_iface_3},
    {"SteamAPI_SteamRemoteStorage_v014", (void*)steam_rs_obj_ptr},
    {"SteamAPI_SteamRemoteStorage_v016", (void*)steam_rs_obj_ptr},
    {"SteamAPI_SteamUserStats_v011", (void*)steam_iface_5},
    {"SteamAPI_SteamUserStats_v012", (void*)steam_iface_5},
    {"SteamAPI_SteamUserStats_v013", (void*)steam_iface_5},
    {"SteamAPI_SteamNetworking_v006", (void*)steam_iface_6},
    {"SteamAPI_SteamScreenshots_v003", (void*)steam_iface_7},
    {"SteamAPI_SteamMusic_v001", (void*)steam_iface_8},
    {"SteamAPI_SteamMusicRemote_v001", (void*)steam_iface_24},
    {"SteamAPI_SteamHTTP_v003", (void*)steam_iface_9},
    {"SteamAPI_SteamController_v007", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamController_v008", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamInput_v001", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamInput_v002", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamInput_v005", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamInput_v006", (void*)steam_input_obj_ptr},
    {"SteamAPI_SteamUGC_v021", (void*)steam_iface_11},
    {"SteamAPI_SteamHTMLSurface_v005", (void*)steam_iface_12},
    {"SteamAPI_SteamInventory_v003", (void*)steam_iface_13},
    {"SteamAPI_SteamTimeline_v004", (void*)steam_iface_14},
    {"SteamAPI_SteamVideo_v007", (void*)steam_iface_15},
    {"SteamAPI_SteamParentalSettings_v001", (void*)steam_iface_16},
    {"SteamAPI_SteamRemotePlay_v004", (void*)steam_iface_17},
    {"SteamAPI_SteamNetworkingMessages_SteamAPI_v002", (void*)steam_iface_18},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", (void*)steam_iface_19},
    {"SteamAPI_SteamNetworkingUtils_SteamAPI_v004", (void*)steam_iface_20},
    {"SteamAPI_ISteamClient_CreateSteamPipe", (void*)st_client_CreateSteamPipe},
    {"SteamAPI_ISteamClient_BReleaseSteamPipe", (void*)st_client_BReleaseSteamPipe},
    {"SteamAPI_ISteamClient_ConnectToGlobalUser", (void*)st_client_ConnectToGlobalUser},
    {"SteamAPI_ISteamClient_CreateLocalUser", (void*)st_client_CreateLocalUser},
    {"SteamAPI_ISteamClient_ReleaseUser", (void*)st_client_ReleaseUser},
    {"SteamAPI_ISteamClient_GetISteamUser", (void*)st_client_GetISteamUser},
    {"SteamAPI_ISteamClient_GetISteamGameServer", (void*)st_client_GetISteamGameServer},
    {"SteamAPI_ISteamClient_SetLocalIPBinding", (void*)st_client_SetLocalIPBinding},
    {"SteamAPI_ISteamClient_GetISteamFriends", (void*)st_client_GetISteamFriends},
    {"SteamAPI_ISteamClient_GetISteamUtils", (void*)st_client_GetISteamUtils},
    {"SteamAPI_ISteamClient_GetISteamMatchmaking", (void*)st_client_GetISteamMatchmaking},
    {"SteamAPI_ISteamClient_GetISteamMatchmakingServers", (void*)st_client_GetISteamMatchmakingServers},
    {"SteamAPI_ISteamClient_GetISteamGenericInterface", (void*)st_client_GetISteamGenericInterface},
    {"SteamAPI_ISteamClient_GetISteamUserStats", (void*)st_client_GetISteamUserStats},
    {"SteamAPI_ISteamClient_GetISteamGameServerStats", (void*)st_client_GetISteamGameServerStats},
    {"SteamAPI_ISteamClient_GetISteamApps", (void*)st_client_GetISteamApps},
    {"SteamAPI_ISteamClient_GetISteamNetworking", (void*)st_client_GetISteamNetworking},
    {"SteamAPI_ISteamClient_GetISteamRemoteStorage", (void*)st_client_GetISteamRemoteStorage},
    {"SteamAPI_ISteamClient_GetISteamScreenshots", (void*)st_client_GetISteamScreenshots},
    {"SteamAPI_ISteamClient_GetISteamGameSearch", (void*)st_client_GetISteamGameSearch},
    {"SteamAPI_ISteamClient_RunFrame", (void*)st_client_RunFrame},
    {"SteamAPI_ISteamClient_GetIPCCallCount", (void*)st_client_GetIPCCallCount},
    {"SteamAPI_ISteamClient_SetWarningMessageHook", (void*)st_client_SetWarningMessageHook},
    {"SteamAPI_ISteamClient_BShutdownIfAllPipesClosed", (void*)st_client_BShutdownIfAllPipesClosed},
    {"SteamAPI_ISteamClient_GetISteamHTTP", (void*)st_client_GetISteamHTTP},
    {"SteamAPI_ISteamClient_GetISteamController", (void*)st_client_GetISteamController},
    {"SteamAPI_ISteamClient_GetISteamUGC", (void*)st_client_GetISteamUGC},
    {"SteamAPI_ISteamClient_GetISteamAppList", (void*)st_client_GetISteamAppList},
    {"SteamAPI_ISteamClient_GetISteamMusic", (void*)st_client_GetISteamMusic},
    {"SteamAPI_ISteamClient_GetISteamMusicRemote", (void*)st_client_GetISteamMusicRemote},
    {"SteamAPI_ISteamClient_GetISteamHTMLSurface", (void*)st_client_GetISteamHTMLSurface},
    {"SteamAPI_ISteamClient_GetISteamInventory", (void*)st_client_GetISteamInventory},
    {"SteamAPI_ISteamClient_GetISteamVideo", (void*)st_client_GetISteamVideo},
    {"SteamAPI_ISteamClient_GetISteamParentalSettings", (void*)st_client_GetISteamParentalSettings},
    {"SteamAPI_ISteamClient_GetISteamInput", (void*)st_client_GetISteamInput},
    {"SteamAPI_ISteamClient_GetISteamParties", (void*)st_client_GetISteamParties},
    {"SteamAPI_ISteamClient_GetISteamRemotePlay", (void*)st_client_GetISteamRemotePlay},
    {"SteamAPI_ISteamApps_BIsSubscribed", (void*)steam_apps_false},
    {"SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend", (void*)steam_apps_false},
    {"SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing", (void*)steam_apps_false},
    {"SteamAPI_ISteamApps_BIsTimedTrial", (void*)steam_apps_is_timed_trial},
    {"SteamAPI_ISteamApps_BIsAppInstalled", (void*)steam_apps_false_u32},
    {"SteamAPI_ISteamApps_GetAppOwner", (void*)steam_apps_get_app_owner},
    {"SteamAPI_ISteamUser_UserHasLicenseForApp", (void*)steam_user_has_license},
    {"SteamAPI_ISteamUser_BLoggedOn", (void*)steam_user_b_logged_on},
    {"SteamAPI_ISteamUser_GetSteamID", (void*)steam_user_get_steam_id},
    {"SteamAPI_ISteamFriends_GetPersonaName", (void*)steam_friends_get_persona_name},
    {"SteamAPI_ISteamFriends_GetPersonaState", (void*)steam_friends_get_persona_state},
    {"SteamAPI_ISteamApps_GetAvailableGameLanguages", (void*)steam_const_english},
    {"SteamAPI_ISteamUtils_GetSteamUILanguage", (void*)steam_const_english},
    {"SteamAPI_ISteamUtils_GetIPCountry", (void*)steam_utils_get_ip_country},
    {"SteamAPI_ISteamUtils_GetCurrentBatteryPower", (void*)steam_utils_get_battery},
    {"SteamAPI_ISteamUtils_GetServerRealTime", (void*)steam_utils_get_server_time},
    {"SteamAPI_ISteamUtils_GetConnectedUniverse", (void*)steam_utils_get_universe},
    {"SteamAPI_ISteamApps_GetAppInstallDir", (void*)steam_apps_get_install_dir},
    {"SteamAPI_ISteamApps_GetAppBuildId", (void*)steam_apps_get_build_id},
    {"SteamAPI_ISteamRemoteStorage_FileWrite", (void*)st_rs_FileWrite},
    {"SteamAPI_ISteamRemoteStorage_FileRead", (void*)st_rs_FileRead},
    {"SteamAPI_ISteamRemoteStorage_FileForget", (void*)st_rs_FileForget},
    {"SteamAPI_ISteamRemoteStorage_FileDelete", (void*)st_rs_FileDelete},
    {"SteamAPI_ISteamRemoteStorage_FileExists", (void*)st_rs_FileExists},
    {"SteamAPI_ISteamRemoteStorage_FilePersisted", (void*)st_rs_FilePersisted},
    {"SteamAPI_ISteamRemoteStorage_GetFileSize", (void*)st_rs_GetFileSize},
    {"SteamAPI_ISteamRemoteStorage_GetFileTimestamp", (void*)st_rs_GetFileTimestamp},
    {"SteamAPI_ISteamRemoteStorage_GetFileCount", (void*)st_rs_GetFileCount},
    {"SteamAPI_ISteamRemoteStorage_GetFileNameAndSize", (void*)st_rs_GetFileNameAndSize},
    {"SteamAPI_ISteamRemoteStorage_GetQuota", (void*)st_rs_GetQuota},
    {"SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount", (void*)st_rs_IsCloudEnabledForAccount},
    {"SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp", (void*)st_rs_IsCloudEnabledForApp},
    {"SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp", (void*)st_rs_SetCloudEnabledForApp},
    {"SteamAPI_ISteamInput_Init", (void*)st_input_Init},
    {"SteamAPI_ISteamInput_Shutdown", (void*)st_input_Shutdown},
    {"SteamAPI_ISteamInput_RunFrame", (void*)st_input_RunFrame},
    {"SteamAPI_ISteamInput_GetConnectedControllers", (void*)st_input_GetConnectedControllers},
    {"SteamAPI_ISteamInput_GetControllerForGamepadIndex", (void*)st_input_GetControllerForGamepadIndex},
    {"SteamAPI_ISteamInput_GetGamepadIndexForController", (void*)st_input_GetGamepadIndexForController},
    {"SteamAPI_ISteamInput_GetInputTypeForHandle", (void*)st_input_GetInputTypeForHandle},
    {"SteamAPI_ISteamInput_TriggerVibration", (void*)st_input_TriggerVibration},
    {"SteamAPI_ISteamInput_TriggerVibrationExtended", (void*)st_input_TriggerVibrationExtended},
    {"SteamAPI_ISteamInput_GetAnalogActionData", (void*)steam_zero16},
    {"SteamAPI_ISteamInput_GetMotionData", (void*)steam_zero40},
};

static const char* const k_steam_str_exports[] = {
    "SteamAPI_ISteamApps_GetLaunchQueryParam",
    "SteamAPI_ISteamFriends_GetClanName",
    "SteamAPI_ISteamFriends_GetClanTag",
    "SteamAPI_ISteamFriends_GetFriendPersonaName",
    "SteamAPI_ISteamFriends_GetFriendPersonaNameHistory",
    "SteamAPI_ISteamFriends_GetFriendRichPresence",
    "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex",
    "SteamAPI_ISteamFriends_GetFriendsGroupName",
    "SteamAPI_ISteamFriends_GetPlayerNickname",
    "SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy",
    "SteamAPI_ISteamInput_GetGlyphForXboxOrigin",
    "SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin",
    "SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin",
    "SteamAPI_ISteamInput_GetStringForActionOrigin",
    "SteamAPI_ISteamInput_GetStringForAnalogActionName",
    "SteamAPI_ISteamInput_GetStringForDigitalActionName",
    "SteamAPI_ISteamInput_GetStringForXboxOrigin",
    "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute",
    "SteamAPI_ISteamUserStats_GetAchievementName",
    "SteamAPI_ISteamUserStats_GetLeaderboardName",
    "SteamAPI_gameserveritem_t_GetName",
    "SteamAPI_servernetadr_t_GetConnectionAddressString",
    "SteamAPI_servernetadr_t_GetQueryAddressString",
};

static const char* const k_steam_noop_exports[] = {
    "SteamAPI_ISteamApps_BGetDLCDataByIndex",
    "SteamAPI_ISteamApps_BIsCybercafe",
    "SteamAPI_ISteamApps_BIsLowViolence",
    "SteamAPI_ISteamApps_BIsVACBanned",
    "SteamAPI_ISteamApps_GetBetaInfo",
    "SteamAPI_ISteamApps_GetCurrentBetaName",
    "SteamAPI_ISteamApps_GetDLCCount",
    "SteamAPI_ISteamApps_GetDlcDownloadProgress",
    "SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime",
    "SteamAPI_ISteamApps_GetFileDetails",
    "SteamAPI_ISteamApps_GetInstalledDepots",
    "SteamAPI_ISteamApps_GetLaunchCommandLine",
    "SteamAPI_ISteamApps_GetNumBetas",
    "SteamAPI_ISteamApps_InstallDLC",
    "SteamAPI_ISteamApps_MarkContentCorrupt",
    "SteamAPI_ISteamApps_SetActiveBeta",
    "SteamAPI_ISteamApps_SetDlcContext",
    "SteamAPI_ISteamApps_UninstallDLC",
    "SteamAPI_ISteamFriends_ActivateGameOverlay",
    "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog",
    "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString",
    "SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog",
    "SteamAPI_ISteamFriends_ActivateGameOverlayToStore",
    "SteamAPI_ISteamFriends_ActivateGameOverlayToUser",
    "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage",
    "SteamAPI_ISteamFriends_BHasEquippedProfileItem",
    "SteamAPI_ISteamFriends_ClearRichPresence",
    "SteamAPI_ISteamFriends_CloseClanChatWindowInSteam",
    "SteamAPI_ISteamFriends_DownloadClanActivityCounts",
    "SteamAPI_ISteamFriends_EnumerateFollowingList",
    "SteamAPI_ISteamFriends_GetChatMemberByIndex",
    "SteamAPI_ISteamFriends_GetClanActivityCounts",
    "SteamAPI_ISteamFriends_GetClanByIndex",
    "SteamAPI_ISteamFriends_GetClanChatMemberCount",
    "SteamAPI_ISteamFriends_GetClanChatMessage",
    "SteamAPI_ISteamFriends_GetClanCount",
    "SteamAPI_ISteamFriends_GetClanOfficerByIndex",
    "SteamAPI_ISteamFriends_GetClanOfficerCount",
    "SteamAPI_ISteamFriends_GetClanOwner",
    "SteamAPI_ISteamFriends_GetCoplayFriend",
    "SteamAPI_ISteamFriends_GetCoplayFriendCount",
    "SteamAPI_ISteamFriends_GetFollowerCount",
    "SteamAPI_ISteamFriends_GetFriendByIndex",
    "SteamAPI_ISteamFriends_GetFriendCoplayGame",
    "SteamAPI_ISteamFriends_GetFriendCoplayTime",
    "SteamAPI_ISteamFriends_GetFriendCount",
    "SteamAPI_ISteamFriends_GetFriendCountFromSource",
    "SteamAPI_ISteamFriends_GetFriendFromSourceByIndex",
    "SteamAPI_ISteamFriends_GetFriendGamePlayed",
    "SteamAPI_ISteamFriends_GetFriendMessage",
    "SteamAPI_ISteamFriends_GetFriendPersonaState",
    "SteamAPI_ISteamFriends_GetFriendRelationship",
    "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount",
    "SteamAPI_ISteamFriends_GetFriendSteamLevel",
    "SteamAPI_ISteamFriends_GetFriendsGroupCount",
    "SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex",
    "SteamAPI_ISteamFriends_GetFriendsGroupMembersCount",
    "SteamAPI_ISteamFriends_GetFriendsGroupMembersList",
    "SteamAPI_ISteamFriends_GetLargeFriendAvatar",
    "SteamAPI_ISteamFriends_GetMediumFriendAvatar",
    "SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages",
    "SteamAPI_ISteamFriends_GetProfileItemPropertyString",
    "SteamAPI_ISteamFriends_GetProfileItemPropertyUint",
    "SteamAPI_ISteamFriends_GetSmallFriendAvatar",
    "SteamAPI_ISteamFriends_HasFriend",
    "SteamAPI_ISteamFriends_InviteUserToGame",
    "SteamAPI_ISteamFriends_IsClanChatAdmin",
    "SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam",
    "SteamAPI_ISteamFriends_IsClanOfficialGameGroup",
    "SteamAPI_ISteamFriends_IsClanPublic",
    "SteamAPI_ISteamFriends_IsFollowing",
    "SteamAPI_ISteamFriends_IsUserInSource",
    "SteamAPI_ISteamFriends_JoinClanChatRoom",
    "SteamAPI_ISteamFriends_LeaveClanChatRoom",
    "SteamAPI_ISteamFriends_OpenClanChatWindowInSteam",
    "SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser",
    "SteamAPI_ISteamFriends_ReplyToFriendMessage",
    "SteamAPI_ISteamFriends_RequestClanOfficerList",
    "SteamAPI_ISteamFriends_RequestEquippedProfileItems",
    "SteamAPI_ISteamFriends_RequestFriendRichPresence",
    "SteamAPI_ISteamFriends_RequestUserInformation",
    "SteamAPI_ISteamFriends_SendClanChatMessage",
    "SteamAPI_ISteamFriends_SetInGameVoiceSpeaking",
    "SteamAPI_ISteamFriends_SetListenForFriendsMessages",
    "SteamAPI_ISteamFriends_SetPlayedWith",
    "SteamAPI_ISteamFriends_SetRichPresence",
    "SteamAPI_ISteamHTMLSurface_AddHeader",
    "SteamAPI_ISteamHTMLSurface_AllowStartRequest",
    "SteamAPI_ISteamHTMLSurface_CopyToClipboard",
    "SteamAPI_ISteamHTMLSurface_CreateBrowser",
    "SteamAPI_ISteamHTMLSurface_ExecuteJavascript",
    "SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse",
    "SteamAPI_ISteamHTMLSurface_Find",
    "SteamAPI_ISteamHTMLSurface_GetLinkAtPosition",
    "SteamAPI_ISteamHTMLSurface_GoBack",
    "SteamAPI_ISteamHTMLSurface_GoForward",
    "SteamAPI_ISteamHTMLSurface_Init",
    "SteamAPI_ISteamHTMLSurface_JSDialogResponse",
    "SteamAPI_ISteamHTMLSurface_KeyChar",
    "SteamAPI_ISteamHTMLSurface_KeyDown",
    "SteamAPI_ISteamHTMLSurface_KeyUp",
    "SteamAPI_ISteamHTMLSurface_LoadURL",
    "SteamAPI_ISteamHTMLSurface_MouseDoubleClick",
    "SteamAPI_ISteamHTMLSurface_MouseDown",
    "SteamAPI_ISteamHTMLSurface_MouseMove",
    "SteamAPI_ISteamHTMLSurface_MouseUp",
    "SteamAPI_ISteamHTMLSurface_MouseWheel",
    "SteamAPI_ISteamHTMLSurface_OpenDeveloperTools",
    "SteamAPI_ISteamHTMLSurface_PasteFromClipboard",
    "SteamAPI_ISteamHTMLSurface_Reload",
    "SteamAPI_ISteamHTMLSurface_RemoveBrowser",
    "SteamAPI_ISteamHTMLSurface_SetBackgroundMode",
    "SteamAPI_ISteamHTMLSurface_SetCookie",
    "SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor",
    "SteamAPI_ISteamHTMLSurface_SetHorizontalScroll",
    "SteamAPI_ISteamHTMLSurface_SetKeyFocus",
    "SteamAPI_ISteamHTMLSurface_SetPageScaleFactor",
    "SteamAPI_ISteamHTMLSurface_SetSize",
    "SteamAPI_ISteamHTMLSurface_SetVerticalScroll",
    "SteamAPI_ISteamHTMLSurface_Shutdown",
    "SteamAPI_ISteamHTMLSurface_StopFind",
    "SteamAPI_ISteamHTMLSurface_StopLoad",
    "SteamAPI_ISteamHTMLSurface_ViewSource",
    "SteamAPI_ISteamHTTP_CreateCookieContainer",
    "SteamAPI_ISteamHTTP_CreateHTTPRequest",
    "SteamAPI_ISteamHTTP_DeferHTTPRequest",
    "SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct",
    "SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut",
    "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData",
    "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize",
    "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize",
    "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue",
    "SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData",
    "SteamAPI_ISteamHTTP_PrioritizeHTTPRequest",
    "SteamAPI_ISteamHTTP_ReleaseCookieContainer",
    "SteamAPI_ISteamHTTP_ReleaseHTTPRequest",
    "SteamAPI_ISteamHTTP_SendHTTPRequest",
    "SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse",
    "SteamAPI_ISteamHTTP_SetCookie",
    "SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS",
    "SteamAPI_ISteamHTTP_SetHTTPRequestContextValue",
    "SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer",
    "SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter",
    "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue",
    "SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout",
    "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody",
    "SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate",
    "SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo",
    "SteamAPI_ISteamInput_ActivateActionSet",
    "SteamAPI_ISteamInput_ActivateActionSetLayer",
    "SteamAPI_ISteamInput_BNewDataAvailable",
    "SteamAPI_ISteamInput_BWaitForData",
    "SteamAPI_ISteamInput_DeactivateActionSetLayer",
    "SteamAPI_ISteamInput_DeactivateAllActionSetLayers",
    "SteamAPI_ISteamInput_EnableActionEventCallbacks",
    "SteamAPI_ISteamInput_EnableDeviceCallbacks",
    "SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin",
    "SteamAPI_ISteamInput_GetActionSetHandle",
    "SteamAPI_ISteamInput_GetActiveActionSetLayers",
    "SteamAPI_ISteamInput_GetAnalogActionHandle",
    "SteamAPI_ISteamInput_GetAnalogActionOrigins",
    "SteamAPI_ISteamInput_GetConnectedControllers",
    "SteamAPI_ISteamInput_GetControllerForGamepadIndex",
    "SteamAPI_ISteamInput_GetCurrentActionSet",
    "SteamAPI_ISteamInput_GetDeviceBindingRevision",
    "SteamAPI_ISteamInput_GetDigitalActionData",
    "SteamAPI_ISteamInput_GetDigitalActionHandle",
    "SteamAPI_ISteamInput_GetDigitalActionOrigins",
    "SteamAPI_ISteamInput_GetGamepadIndexForController",
    "SteamAPI_ISteamInput_GetInputTypeForHandle",
    "SteamAPI_ISteamInput_GetRemotePlaySessionID",
    "SteamAPI_ISteamInput_GetSessionInputConfigurationSettings",
    "SteamAPI_ISteamInput_Init",
    "SteamAPI_ISteamInput_Legacy_TriggerHapticPulse",
    "SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse",
    "SteamAPI_ISteamInput_RunFrame",
    "SteamAPI_ISteamInput_SetInputActionManifestFilePath",
    "SteamAPI_ISteamInput_SetLEDColor",
    "SteamAPI_ISteamInput_ShowBindingPanel",
    "SteamAPI_ISteamInput_Shutdown",
    "SteamAPI_ISteamInput_StopAnalogActionMomentum",
    "SteamAPI_ISteamInput_TranslateActionOrigin",
    "SteamAPI_ISteamInput_TriggerSimpleHapticEvent",
    "SteamAPI_ISteamInput_TriggerVibration",
    "SteamAPI_ISteamInput_TriggerVibrationExtended",
    "SteamAPI_ISteamInventory_AddPromoItem",
    "SteamAPI_ISteamInventory_AddPromoItems",
    "SteamAPI_ISteamInventory_CheckResultSteamID",
    "SteamAPI_ISteamInventory_ConsumeItem",
    "SteamAPI_ISteamInventory_DeserializeResult",
    "SteamAPI_ISteamInventory_DestroyResult",
    "SteamAPI_ISteamInventory_ExchangeItems",
    "SteamAPI_ISteamInventory_GenerateItems",
    "SteamAPI_ISteamInventory_GetAllItems",
    "SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs",
    "SteamAPI_ISteamInventory_GetItemDefinitionIDs",
    "SteamAPI_ISteamInventory_GetItemDefinitionProperty",
    "SteamAPI_ISteamInventory_GetItemPrice",
    "SteamAPI_ISteamInventory_GetItemsByID",
    "SteamAPI_ISteamInventory_GetItemsWithPrices",
    "SteamAPI_ISteamInventory_GetNumItemsWithPrices",
    "SteamAPI_ISteamInventory_GetResultItemProperty",
    "SteamAPI_ISteamInventory_GetResultItems",
    "SteamAPI_ISteamInventory_GetResultStatus",
    "SteamAPI_ISteamInventory_GetResultTimestamp",
    "SteamAPI_ISteamInventory_GrantPromoItems",
    "SteamAPI_ISteamInventory_LoadItemDefinitions",
    "SteamAPI_ISteamInventory_RemoveProperty",
    "SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs",
    "SteamAPI_ISteamInventory_RequestPrices",
    "SteamAPI_ISteamInventory_SerializeResult",
    "SteamAPI_ISteamInventory_SetPropertyBool",
    "SteamAPI_ISteamInventory_SetPropertyFloat",
    "SteamAPI_ISteamInventory_SetPropertyInt64",
    "SteamAPI_ISteamInventory_SetPropertyString",
    "SteamAPI_ISteamInventory_StartPurchase",
    "SteamAPI_ISteamInventory_StartUpdateProperties",
    "SteamAPI_ISteamInventory_SubmitUpdateProperties",
    "SteamAPI_ISteamInventory_TransferItemQuantity",
    "SteamAPI_ISteamInventory_TriggerItemDrop",
    "SteamAPI_ISteamMatchmakingServers_CancelQuery",
    "SteamAPI_ISteamMatchmakingServers_CancelServerQuery",
    "SteamAPI_ISteamMatchmakingServers_GetServerCount",
    "SteamAPI_ISteamMatchmakingServers_GetServerDetails",
    "SteamAPI_ISteamMatchmakingServers_IsRefreshing",
    "SteamAPI_ISteamMatchmakingServers_PingServer",
    "SteamAPI_ISteamMatchmakingServers_PlayerDetails",
    "SteamAPI_ISteamMatchmakingServers_RefreshQuery",
    "SteamAPI_ISteamMatchmakingServers_RefreshServer",
    "SteamAPI_ISteamMatchmakingServers_ReleaseRequest",
    "SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList",
    "SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList",
    "SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList",
    "SteamAPI_ISteamMatchmakingServers_RequestInternetServerList",
    "SteamAPI_ISteamMatchmakingServers_RequestLANServerList",
    "SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList",
    "SteamAPI_ISteamMatchmakingServers_ServerRules",
    "SteamAPI_ISteamMatchmaking_AddFavoriteGame",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter",
    "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter",
    "SteamAPI_ISteamMatchmaking_CreateLobby",
    "SteamAPI_ISteamMatchmaking_DeleteLobbyData",
    "SteamAPI_ISteamMatchmaking_GetFavoriteGame",
    "SteamAPI_ISteamMatchmaking_GetFavoriteGameCount",
    "SteamAPI_ISteamMatchmaking_GetLobbyByIndex",
    "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry",
    "SteamAPI_ISteamMatchmaking_GetLobbyData",
    "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex",
    "SteamAPI_ISteamMatchmaking_GetLobbyDataCount",
    "SteamAPI_ISteamMatchmaking_GetLobbyGameServer",
    "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex",
    "SteamAPI_ISteamMatchmaking_GetLobbyMemberData",
    "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit",
    "SteamAPI_ISteamMatchmaking_GetLobbyOwner",
    "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
    "SteamAPI_ISteamMatchmaking_InviteUserToLobby",
    "SteamAPI_ISteamMatchmaking_JoinLobby",
    "SteamAPI_ISteamMatchmaking_LeaveLobby",
    "SteamAPI_ISteamMatchmaking_RemoveFavoriteGame",
    "SteamAPI_ISteamMatchmaking_RequestLobbyData",
    "SteamAPI_ISteamMatchmaking_RequestLobbyList",
    "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg",
    "SteamAPI_ISteamMatchmaking_SetLobbyData",
    "SteamAPI_ISteamMatchmaking_SetLobbyGameServer",
    "SteamAPI_ISteamMatchmaking_SetLobbyJoinable",
    "SteamAPI_ISteamMatchmaking_SetLobbyMemberData",
    "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit",
    "SteamAPI_ISteamMatchmaking_SetLobbyOwner",
    "SteamAPI_ISteamMatchmaking_SetLobbyType",
    "SteamAPI_ISteamMusic_BIsEnabled",
    "SteamAPI_ISteamMusic_BIsPlaying",
    "SteamAPI_ISteamMusic_GetPlaybackStatus",
    "SteamAPI_ISteamMusic_GetVolume",
    "SteamAPI_ISteamMusic_Pause",
    "SteamAPI_ISteamMusic_Play",
    "SteamAPI_ISteamMusic_PlayNext",
    "SteamAPI_ISteamMusic_PlayPrevious",
    "SteamAPI_ISteamMusic_SetVolume",
    "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser",
    "SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser",
    "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser",
    "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo",
    "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel",
    "SteamAPI_ISteamNetworkingMessages_SendMessageToUser",
    "SteamAPI_ISteamNetworkingSockets_AcceptConnection",
    "SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP",
    "SteamAPI_ISteamNetworkingSockets_CloseConnection",
    "SteamAPI_ISteamNetworkingSockets_CloseListenSocket",
    "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes",
    "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress",
    "SteamAPI_ISteamNetworkingSockets_ConnectP2P",
    "SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer",
    "SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort",
    "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP",
    "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P",
    "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP",
    "SteamAPI_ISteamNetworkingSockets_CreatePollGroup",
    "SteamAPI_ISteamNetworkingSockets_CreateSocketPair",
    "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup",
    "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection",
    "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus",
    "SteamAPI_ISteamNetworkingSockets_GetCertificateRequest",
    "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo",
    "SteamAPI_ISteamNetworkingSockets_GetConnectionName",
    "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus",
    "SteamAPI_ISteamNetworkingSockets_GetConnectionUserData",
    "SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus",
    "SteamAPI_ISteamNetworkingSockets_GetFakeIP",
    "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID",
    "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort",
    "SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress",
    "SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection",
    "SteamAPI_ISteamNetworkingSockets_InitAuthentication",
    "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection",
    "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup",
    "SteamAPI_ISteamNetworkingSockets_ResetIdentity",
    "SteamAPI_ISteamNetworkingSockets_RunCallbacks",
    "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection",
    "SteamAPI_ISteamNetworkingSockets_SendMessages",
    "SteamAPI_ISteamNetworkingSockets_SetCertificate",
    "SteamAPI_ISteamNetworkingSockets_SetConnectionName",
    "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup",
    "SteamAPI_ISteamNetworkingSockets_SetConnectionUserData",
    "SteamAPI_ISteamNetworkingUtils_AllocateMessage",
    "SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate",
    "SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString",
    "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations",
    "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost",
    "SteamAPI_ISteamNetworkingUtils_GetConfigValue",
    "SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo",
    "SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP",
    "SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType",
    "SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation",
    "SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp",
    "SteamAPI_ISteamNetworkingUtils_GetPOPCount",
    "SteamAPI_ISteamNetworkingUtils_GetPOPList",
    "SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter",
    "SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP",
    "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus",
    "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess",
    "SteamAPI_ISteamNetworkingUtils_IsFakeIPv4",
    "SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues",
    "SteamAPI_ISteamNetworkingUtils_ParsePingLocationString",
    "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat",
    "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32",
    "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32",
    "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString",
    "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser",
    "SteamAPI_ISteamNetworking_AllowP2PPacketRelay",
    "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser",
    "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser",
    "SteamAPI_ISteamNetworking_GetP2PSessionState",
    "SteamAPI_ISteamNetworking_IsP2PPacketAvailable",
    "SteamAPI_ISteamNetworking_ReadP2PPacket",
    "SteamAPI_ISteamNetworking_SendP2PPacket",
    "SteamAPI_ISteamParentalSettings_BIsAppBlocked",
    "SteamAPI_ISteamParentalSettings_BIsAppInBlockList",
    "SteamAPI_ISteamParentalSettings_BIsFeatureBlocked",
    "SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList",
    "SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled",
    "SteamAPI_ISteamParentalSettings_BIsParentalLockLocked",
    "SteamAPI_ISteamParties_CancelReservation",
    "SteamAPI_ISteamParties_ChangeNumOpenSlots",
    "SteamAPI_ISteamParties_CreateBeacon",
    "SteamAPI_ISteamParties_DestroyBeacon",
    "SteamAPI_ISteamParties_GetAvailableBeaconLocations",
    "SteamAPI_ISteamParties_GetBeaconByIndex",
    "SteamAPI_ISteamParties_GetBeaconDetails",
    "SteamAPI_ISteamParties_GetBeaconLocationData",
    "SteamAPI_ISteamParties_GetNumActiveBeacons",
    "SteamAPI_ISteamParties_GetNumAvailableBeaconLocations",
    "SteamAPI_ISteamParties_JoinParty",
    "SteamAPI_ISteamParties_OnReservationCompleted",
    "SteamAPI_ISteamRemotePlay_BEnableRemotePlayTogetherDirectInput",
    "SteamAPI_ISteamRemotePlay_BGetSessionClientResolution",
    "SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite",
    "SteamAPI_ISteamRemotePlay_BSessionRemotePlayTogether",
    "SteamAPI_ISteamRemotePlay_CreateMouseCursor",
    "SteamAPI_ISteamRemotePlay_DisableRemotePlayTogetherDirectInput",
    "SteamAPI_ISteamRemotePlay_GetInput",
    "SteamAPI_ISteamRemotePlay_GetLargeSessionAvatar",
    "SteamAPI_ISteamRemotePlay_GetMediumSessionAvatar",
    "SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor",
    "SteamAPI_ISteamRemotePlay_GetSessionClientName",
    "SteamAPI_ISteamRemotePlay_GetSessionCount",
    "SteamAPI_ISteamRemotePlay_GetSessionGuestID",
    "SteamAPI_ISteamRemotePlay_GetSessionID",
    "SteamAPI_ISteamRemotePlay_GetSessionSteamID",
    "SteamAPI_ISteamRemotePlay_GetSmallSessionAvatar",
    "SteamAPI_ISteamRemotePlay_SetMouseCursor",
    "SteamAPI_ISteamRemotePlay_SetMousePosition",
    "SteamAPI_ISteamRemotePlay_SetMouseVisibility",
    "SteamAPI_ISteamRemotePlay_ShowRemotePlayTogetherUI",
    "SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch",
    "SteamAPI_ISteamRemoteStorage_EndFileWriteBatch",
    "SteamAPI_ISteamRemoteStorage_FileDelete",
    "SteamAPI_ISteamRemoteStorage_FileExists",
    "SteamAPI_ISteamRemoteStorage_FileForget",
    "SteamAPI_ISteamRemoteStorage_FilePersisted",
    "SteamAPI_ISteamRemoteStorage_FileRead",
    "SteamAPI_ISteamRemoteStorage_FileReadAsync",
    "SteamAPI_ISteamRemoteStorage_FileShare",
    "SteamAPI_ISteamRemoteStorage_FileWrite",
    "SteamAPI_ISteamRemoteStorage_FileWriteAsync",
    "SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel",
    "SteamAPI_ISteamRemoteStorage_FileWriteStreamClose",
    "SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen",
    "SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk",
    "SteamAPI_ISteamRemoteStorage_GetCachedUGCCount",
    "SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle",
    "SteamAPI_ISteamRemoteStorage_GetFileCount",
    "SteamAPI_ISteamRemoteStorage_GetFileSize",
    "SteamAPI_ISteamRemoteStorage_GetFileTimestamp",
    "SteamAPI_ISteamRemoteStorage_GetLocalFileChange",
    "SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount",
    "SteamAPI_ISteamRemoteStorage_GetQuota",
    "SteamAPI_ISteamRemoteStorage_GetSyncPlatforms",
    "SteamAPI_ISteamRemoteStorage_GetUGCDetails",
    "SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress",
    "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount",
    "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp",
    "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp",
    "SteamAPI_ISteamRemoteStorage_SetSyncPlatforms",
    "SteamAPI_ISteamRemoteStorage_UGCDownload",
    "SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation",
    "SteamAPI_ISteamRemoteStorage_UGCRead",
    "SteamAPI_ISteamScreenshots_AddScreenshotToLibrary",
    "SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary",
    "SteamAPI_ISteamScreenshots_HookScreenshots",
    "SteamAPI_ISteamScreenshots_IsScreenshotsHooked",
    "SteamAPI_ISteamScreenshots_SetLocation",
    "SteamAPI_ISteamScreenshots_TagPublishedFile",
    "SteamAPI_ISteamScreenshots_TagUser",
    "SteamAPI_ISteamScreenshots_TriggerScreenshot",
    "SteamAPI_ISteamScreenshots_WriteScreenshot",
    "SteamAPI_ISteamTimeline_AddGamePhaseTag",
    "SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent",
    "SteamAPI_ISteamTimeline_AddRangeTimelineEvent",
    "SteamAPI_ISteamTimeline_ClearTimelineTooltip",
    "SteamAPI_ISteamTimeline_DoesEventRecordingExist",
    "SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist",
    "SteamAPI_ISteamTimeline_EndGamePhase",
    "SteamAPI_ISteamTimeline_EndRangeTimelineEvent",
    "SteamAPI_ISteamTimeline_OpenOverlayToGamePhase",
    "SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent",
    "SteamAPI_ISteamTimeline_RemoveTimelineEvent",
    "SteamAPI_ISteamTimeline_SetGamePhaseAttribute",
    "SteamAPI_ISteamTimeline_SetGamePhaseID",
    "SteamAPI_ISteamTimeline_SetTimelineGameMode",
    "SteamAPI_ISteamTimeline_SetTimelineTooltip",
    "SteamAPI_ISteamTimeline_StartGamePhase",
    "SteamAPI_ISteamTimeline_StartRangeTimelineEvent",
    "SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent",
    "SteamAPI_ISteamUGC_AddAppDependency",
    "SteamAPI_ISteamUGC_AddContentDescriptor",
    "SteamAPI_ISteamUGC_AddDependency",
    "SteamAPI_ISteamUGC_AddExcludedTag",
    "SteamAPI_ISteamUGC_AddItemKeyValueTag",
    "SteamAPI_ISteamUGC_AddItemPreviewFile",
    "SteamAPI_ISteamUGC_AddItemPreviewVideo",
    "SteamAPI_ISteamUGC_AddItemToFavorites",
    "SteamAPI_ISteamUGC_AddRequiredKeyValueTag",
    "SteamAPI_ISteamUGC_AddRequiredTag",
    "SteamAPI_ISteamUGC_AddRequiredTagGroup",
    "SteamAPI_ISteamUGC_BInitWorkshopForGameServer",
    "SteamAPI_ISteamUGC_CreateItem",
    "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor",
    "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage",
    "SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest",
    "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest",
    "SteamAPI_ISteamUGC_DeleteItem",
    "SteamAPI_ISteamUGC_DownloadItem",
    "SteamAPI_ISteamUGC_GetAppDependencies",
    "SteamAPI_ISteamUGC_GetDownloadedItems",
    "SteamAPI_ISteamUGC_GetItemDownloadInfo",
    "SteamAPI_ISteamUGC_GetItemInstallInfo",
    "SteamAPI_ISteamUGC_GetItemState",
    "SteamAPI_ISteamUGC_GetItemUpdateProgress",
    "SteamAPI_ISteamUGC_GetNumDownloadedItems",
    "SteamAPI_ISteamUGC_GetNumSubscribedItems",
    "SteamAPI_ISteamUGC_GetNumSupportedGameVersions",
    "SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview",
    "SteamAPI_ISteamUGC_GetQueryUGCChildren",
    "SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors",
    "SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag",
    "SteamAPI_ISteamUGC_GetQueryUGCMetadata",
    "SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews",
    "SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags",
    "SteamAPI_ISteamUGC_GetQueryUGCNumTags",
    "SteamAPI_ISteamUGC_GetQueryUGCPreviewURL",
    "SteamAPI_ISteamUGC_GetQueryUGCResult",
    "SteamAPI_ISteamUGC_GetQueryUGCStatistic",
    "SteamAPI_ISteamUGC_GetQueryUGCTag",
    "SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName",
    "SteamAPI_ISteamUGC_GetSubscribedItems",
    "SteamAPI_ISteamUGC_GetSupportedGameVersionData",
    "SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences",
    "SteamAPI_ISteamUGC_GetUserItemVote",
    "SteamAPI_ISteamUGC_GetWorkshopEULAStatus",
    "SteamAPI_ISteamUGC_MarkDownloadedItemAsUnused",
    "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest",
    "SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags",
    "SteamAPI_ISteamUGC_RemoveAppDependency",
    "SteamAPI_ISteamUGC_RemoveContentDescriptor",
    "SteamAPI_ISteamUGC_RemoveDependency",
    "SteamAPI_ISteamUGC_RemoveItemFromFavorites",
    "SteamAPI_ISteamUGC_RemoveItemKeyValueTags",
    "SteamAPI_ISteamUGC_RemoveItemPreview",
    "SteamAPI_ISteamUGC_SendQueryUGCRequest",
    "SteamAPI_ISteamUGC_SetAdminQuery",
    "SteamAPI_ISteamUGC_SetAllowCachedResponse",
    "SteamAPI_ISteamUGC_SetAllowLegacyUpload",
    "SteamAPI_ISteamUGC_SetCloudFileNameFilter",
    "SteamAPI_ISteamUGC_SetItemContent",
    "SteamAPI_ISteamUGC_SetItemDescription",
    "SteamAPI_ISteamUGC_SetItemMetadata",
    "SteamAPI_ISteamUGC_SetItemPreview",
    "SteamAPI_ISteamUGC_SetItemTags",
    "SteamAPI_ISteamUGC_SetItemTitle",
    "SteamAPI_ISteamUGC_SetItemUpdateLanguage",
    "SteamAPI_ISteamUGC_SetItemVisibility",
    "SteamAPI_ISteamUGC_SetItemsDisabledLocally",
    "SteamAPI_ISteamUGC_SetLanguage",
    "SteamAPI_ISteamUGC_SetMatchAnyTag",
    "SteamAPI_ISteamUGC_SetRankedByTrendDays",
    "SteamAPI_ISteamUGC_SetRequiredGameVersions",
    "SteamAPI_ISteamUGC_SetReturnAdditionalPreviews",
    "SteamAPI_ISteamUGC_SetReturnChildren",
    "SteamAPI_ISteamUGC_SetReturnKeyValueTags",
    "SteamAPI_ISteamUGC_SetReturnLongDescription",
    "SteamAPI_ISteamUGC_SetReturnMetadata",
    "SteamAPI_ISteamUGC_SetReturnOnlyIDs",
    "SteamAPI_ISteamUGC_SetReturnPlaytimeStats",
    "SteamAPI_ISteamUGC_SetReturnTotalOnly",
    "SteamAPI_ISteamUGC_SetSearchText",
    "SteamAPI_ISteamUGC_SetSubscriptionsLoadOrder",
    "SteamAPI_ISteamUGC_SetTimeCreatedDateRange",
    "SteamAPI_ISteamUGC_SetTimeUpdatedDateRange",
    "SteamAPI_ISteamUGC_SetUserItemVote",
    "SteamAPI_ISteamUGC_ShowWorkshopEULA",
    "SteamAPI_ISteamUGC_StartItemUpdate",
    "SteamAPI_ISteamUGC_StartPlaytimeTracking",
    "SteamAPI_ISteamUGC_StopPlaytimeTracking",
    "SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems",
    "SteamAPI_ISteamUGC_SubmitItemUpdate",
    "SteamAPI_ISteamUGC_SubscribeItem",
    "SteamAPI_ISteamUGC_SuspendDownloads",
    "SteamAPI_ISteamUGC_UnsubscribeItem",
    "SteamAPI_ISteamUGC_UpdateItemPreviewFile",
    "SteamAPI_ISteamUGC_UpdateItemPreviewVideo",
    "SteamAPI_ISteamUserStats_AttachLeaderboardUGC",
    "SteamAPI_ISteamUserStats_ClearAchievement",
    "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries",
    "SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers",
    "SteamAPI_ISteamUserStats_FindLeaderboard",
    "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard",
    "SteamAPI_ISteamUserStats_GetAchievement",
    "SteamAPI_ISteamUserStats_GetAchievementAchievedPercent",
    "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime",
    "SteamAPI_ISteamUserStats_GetAchievementIcon",
    "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat",
    "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32",
    "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry",
    "SteamAPI_ISteamUserStats_GetGlobalStatDouble",
    "SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble",
    "SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64",
    "SteamAPI_ISteamUserStats_GetGlobalStatInt64",
    "SteamAPI_ISteamUserStats_GetLeaderboardDisplayType",
    "SteamAPI_ISteamUserStats_GetLeaderboardEntryCount",
    "SteamAPI_ISteamUserStats_GetLeaderboardSortMethod",
    "SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo",
    "SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo",
    "SteamAPI_ISteamUserStats_GetNumAchievements",
    "SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers",
    "SteamAPI_ISteamUserStats_GetStatFloat",
    "SteamAPI_ISteamUserStats_GetStatInt32",
    "SteamAPI_ISteamUserStats_GetUserAchievement",
    "SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime",
    "SteamAPI_ISteamUserStats_GetUserStatFloat",
    "SteamAPI_ISteamUserStats_GetUserStatInt32",
    "SteamAPI_ISteamUserStats_IndicateAchievementProgress",
    "SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages",
    "SteamAPI_ISteamUserStats_RequestGlobalStats",
    "SteamAPI_ISteamUserStats_RequestUserStats",
    "SteamAPI_ISteamUserStats_ResetAllStats",
    "SteamAPI_ISteamUserStats_SetAchievement",
    "SteamAPI_ISteamUserStats_SetStatFloat",
    "SteamAPI_ISteamUserStats_SetStatInt32",
    "SteamAPI_ISteamUserStats_StoreStats",
    "SteamAPI_ISteamUserStats_UpdateAvgRateStat",
    "SteamAPI_ISteamUserStats_UploadLeaderboardScore",
    "SteamAPI_ISteamUser_AdvertiseGame",
    "SteamAPI_ISteamUser_BIsBehindNAT",
    "SteamAPI_ISteamUser_BIsPhoneIdentifying",
    "SteamAPI_ISteamUser_BIsPhoneRequiringVerification",
    "SteamAPI_ISteamUser_BIsPhoneVerified",
    "SteamAPI_ISteamUser_BIsTwoFactorEnabled",
    "SteamAPI_ISteamUser_BSetDurationControlOnlineState",
    "SteamAPI_ISteamUser_BeginAuthSession",
    "SteamAPI_ISteamUser_CancelAuthTicket",
    "SteamAPI_ISteamUser_DecompressVoice",
    "SteamAPI_ISteamUser_EndAuthSession",
    "SteamAPI_ISteamUser_GetAuthSessionTicket",
    "SteamAPI_ISteamUser_GetAuthTicketForWebApi",
    "SteamAPI_ISteamUser_GetAvailableVoice",
    "SteamAPI_ISteamUser_GetDurationControl",
    "SteamAPI_ISteamUser_GetEncryptedAppTicket",
    "SteamAPI_ISteamUser_GetGameBadgeLevel",
    "SteamAPI_ISteamUser_GetMarketEligibility",
    "SteamAPI_ISteamUser_GetPlayerSteamLevel",
    "SteamAPI_ISteamUser_GetVoice",
    "SteamAPI_ISteamUser_GetVoiceOptimalSampleRate",
    "SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED",
    "SteamAPI_ISteamUser_RequestEncryptedAppTicket",
    "SteamAPI_ISteamUser_RequestStoreAuthURL",
    "SteamAPI_ISteamUser_StartVoiceRecording",
    "SteamAPI_ISteamUser_StopVoiceRecording",
    "SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED",
    "SteamAPI_ISteamUtils_BOverlayNeedsPresent",
    "SteamAPI_ISteamUtils_CheckFileSignature",
    "SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput",
    "SteamAPI_ISteamUtils_DismissGamepadTextInput",
    "SteamAPI_ISteamUtils_FilterText",
    "SteamAPI_ISteamUtils_GetAPICallFailureReason",
    "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput",
    "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength",
    "SteamAPI_ISteamUtils_GetIPCCallCount",
    "SteamAPI_ISteamUtils_GetIPv6ConnectivityState",
    "SteamAPI_ISteamUtils_GetImageRGBA",
    "SteamAPI_ISteamUtils_GetImageSize",
    "SteamAPI_ISteamUtils_GetSecondsSinceAppActive",
    "SteamAPI_ISteamUtils_GetSecondsSinceComputerActive",
    "SteamAPI_ISteamUtils_InitFilterText",
    "SteamAPI_ISteamUtils_IsAPICallCompleted",
    "SteamAPI_ISteamUtils_IsOverlayEnabled",
    "SteamAPI_ISteamUtils_IsSteamChinaLauncher",
    "SteamAPI_ISteamUtils_IsSteamInBigPictureMode",
    "SteamAPI_ISteamUtils_IsSteamRunningInVR",
    "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck",
    "SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled",
    "SteamAPI_ISteamUtils_SetGameLauncherMode",
    "SteamAPI_ISteamUtils_SetOverlayNotificationInset",
    "SteamAPI_ISteamUtils_SetOverlayNotificationPosition",
    "SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled",
    "SteamAPI_ISteamUtils_SetWarningMessageHook",
    "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput",
    "SteamAPI_ISteamUtils_ShowGamepadTextInput",
    "SteamAPI_ISteamUtils_StartVRDashboard",
    "SteamAPI_ISteamVideo_GetOPFSettings",
    "SteamAPI_ISteamVideo_GetOPFStringForApp",
    "SteamAPI_ISteamVideo_GetVideoURL",
    "SteamAPI_ISteamVideo_IsBroadcasting",
    "SteamAPI_SteamNetworkingConfigValue_t_SetFloat",
    "SteamAPI_SteamNetworkingConfigValue_t_SetInt32",
    "SteamAPI_SteamNetworkingConfigValue_t_SetInt64",
    "SteamAPI_SteamNetworkingConfigValue_t_SetPtr",
    "SteamAPI_SteamNetworkingConfigValue_t_SetString",
    "SteamAPI_SteamNetworkingIPAddr_Clear",
    "SteamAPI_SteamNetworkingIPAddr_GetFakeIPType",
    "SteamAPI_SteamNetworkingIPAddr_GetIPv4",
    "SteamAPI_SteamNetworkingIPAddr_ParseString",
    "SteamAPI_SteamNetworkingIPAddr_SetIPv4",
    "SteamAPI_SteamNetworkingIPAddr_ToString",
    "SteamAPI_SteamNetworkingIdentity_GetSteamID64",
    "SteamAPI_SteamNetworkingIdentity_SetSteamID64",
    "SteamAPI_SteamNetworkingMessage_t_Release",
};

int trt_bridge_register_steam(void) {
  static const trt_named_fn k_fns[] = {
      {"SteamAPI_Init", (void*)&trt_SteamAPI_Init},
      {"SteamAPI_InitEx", (void*)&trt_SteamAPI_InitEx},
      {"SteamAPI_InitFlat", (void*)&trt_SteamAPI_InitFlat},
      {"SteamAPI_Shutdown", (void*)&trt_SteamAPI_Shutdown},
      {"SteamAPI_RestartAppIfNecessary",
       (void*)&trt_SteamAPI_RestartAppIfNecessary},
      {"SteamAPI_IsSteamRunning", (void*)&trt_SteamAPI_IsSteamRunning},
      {"SteamAPI_GetSteamID", (void*)&trt_SteamAPI_GetSteamID},
      {"SteamAPI_ActivateGameOverlay", (void*)&trt_SteamAPI_ActivateGameOverlay},
      {"SteamAPI_SetRichPresence", (void*)&trt_SteamAPI_SetRichPresence},
      {"SteamAPI_RunCallbacks", (void*)&trt_SteamAPI_RunCallbacks},
      {"SteamAPI_RegisterCallback", (void*)&trt_SteamAPI_RegisterCallback},
      {"SteamAPI_UnregisterCallback", (void*)&trt_SteamAPI_UnregisterCallback},
      {"SteamAPI_RegisterCallResult", (void*)&trt_SteamAPI_RegisterCallResult},
      {"SteamAPI_UnregisterCallResult", (void*)&trt_SteamAPI_UnregisterCallResult},
      {"SteamAPI_BIsSubscribedApp", (void*)&trt_SteamAPI_BIsSubscribedApp},
      {"SteamAPI_BIsDlcInstalled", (void*)&trt_SteamAPI_BIsDlcInstalled},
      {"SteamAPI_GetCurrentGameLanguage",
       (void*)&trt_SteamAPI_GetCurrentGameLanguage},
      {"SteamAPI_GetHSteamUser", (void*)&trt_SteamAPI_GetHSteamUser},
      {"SteamAPI_GetHSteamPipe", (void*)&trt_SteamAPI_GetHSteamPipe},
      {"SteamAPI_ManualDispatch_Init", (void*)&trt_SteamAPI_ManualDispatch_Init},
      {"SteamAPI_ManualDispatch_RunFrame",
       (void*)&trt_SteamAPI_ManualDispatch_RunFrame},
      {"SteamAPI_ManualDispatch_GetNextCallback",
       (void*)&trt_SteamAPI_ManualDispatch_GetNextCallback},
      {"SteamAPI_ManualDispatch_FreeLastCallback",
       (void*)&trt_SteamAPI_ManualDispatch_FreeLastCallback},
      {"SteamInternal_ContextInit", (void*)&trt_SteamInternal_ContextInit},
      {"SteamInternal_CreateInterface",
       (void*)&trt_SteamInternal_CreateInterface},
      {"SteamInternal_FindOrCreateUserInterface",
       (void*)&trt_SteamInternal_FindOrCreateUserInterface},
      {"SteamInternal_FindOrCreateGameServerInterface",
       (void*)&trt_SteamInternal_FindOrCreateGameServerInterface},
      {"SteamAPI_SteamUser_v021", (void*)&trt_SteamAPI_SteamUser_v021},
      {"SteamAPI_SteamUser_v023", (void*)&trt_SteamAPI_SteamUser_v023},
      {"SteamAPI_SteamApps_v008", (void*)&trt_SteamAPI_SteamApps_v008},
      {"SteamAPI_SteamUtils_v010", (void*)&trt_SteamAPI_SteamUtils_v010},
      {"SteamAPI_ISteamApps_BIsSubscribedApp",
       (void*)&trt_SteamAPI_ISteamApps_BIsSubscribedApp},
      {"SteamAPI_ISteamApps_BIsDlcInstalled",
       (void*)&trt_SteamAPI_ISteamApps_BIsDlcInstalled},
      {"SteamAPI_ISteamApps_GetCurrentGameLanguage",
       (void*)&trt_SteamAPI_ISteamApps_GetCurrentGameLanguage},
      {"SteamAPI_ISteamUtils_GetAppID",
       (void*)&trt_SteamAPI_ISteamUtils_GetAppID},
  };
  static trt_named_fn g_all_fns[2048];
  int n = 0;
  auto add_fn = [&](const char* name, void* fn) {
    if (n >= 2048) return;
    for (int i = 0; i < n; ++i) {
      if (steam_streq(g_all_fns[i].name, name)) return;
    }
    g_all_fns[n++] = {name, fn};
  };
  for (int i = 0; i < (int)(sizeof(k_fns) / sizeof(k_fns[0])); ++i) {
    add_fn(k_fns[i].name, k_fns[i].fn);
  }
  for (int i = 0; i < (int)(sizeof(k_steam_extra_fns) / sizeof(k_steam_extra_fns[0])); ++i) {
    add_fn(k_steam_extra_fns[i].name, k_steam_extra_fns[i].fn);
  }
  for (int i = 0; i < (int)(sizeof(k_steam_str_exports) / sizeof(k_steam_str_exports[0])); ++i) {
    add_fn(k_steam_str_exports[i], (void*)steam_empty_str);
  }
  for (int i = 0; i < (int)(sizeof(k_steam_noop_exports) / sizeof(k_steam_noop_exports[0])); ++i) {
    add_fn(k_steam_noop_exports[i], (void*)steam_noop_zero);
  }
  trt_module_register("steam_api64.dll", TRT_ROUTE_BUILTIN, g_all_fns, n);
  trt_module_register("steam_api.dll", TRT_ROUTE_BUILTIN, g_all_fns, n);
  return 0;
}

} // extern "C"
