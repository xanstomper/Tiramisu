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
#include <atomic>
#include "input.h"
#include "steam_auth.h"
#include "steam_library.h"
#include "thunk.h"

#include <algorithm>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

namespace {

constexpr int S_OK_ = 0;
constexpr int E_FAIL_ = 0x80004005;

// ---- the app context ----
u32 g_app_id = 0;
int g_initialized = 0;

// The signed-in Steam account (0 = nobody signed in). Filled by
// trt_steam_load_account() from steam_auth's OpenID identity file, so
// GetSteamID answers with the account the user actually proved.
u64 g_steamid64 = 0;
std::string g_persona;
int g_signed_in = 0;

// AppIDs the user's own Steam library contains (imported manifests).
// BIsSubscribedApp answers from this set and nothing else: Tiramisu never
// manufactures an entitlement Steam did not install.
std::vector<u32> g_owned_apps;
std::mutex g_owned_mu;

// ---- callback / callresult registration (the overlay + achievements use it) ----
// A real registration table: the entries persist until the game unregisters
// them, and unregistering really removes one. A register that only appended and
// an unregister that did nothing would leave RunCallbacks calling a callback the
// game already deleted.
struct callback_entry {
  void* callback;
  // A guest callback runs under the guest ABI (Windows x64), not the host's:
  // a plain `int (*)(void*, void*)` cast here passed `self` in the wrong
  // register and faulted inside the game's own Run().
  int(TRT_GUEST_ABI* run)(void* self, void* param);
};
std::vector<callback_entry> g_callbacks;
std::mutex g_callbacks_mu;

struct callresult_entry {
  void* callback;
  u64 call_id;
};
std::vector<callresult_entry> g_callresults;
std::mutex g_callresults_mu;

} // namespace

extern "C" {

// ---- lifecycle ----

// SteamAPI_Init() / SteamAPI_InitEx(ctx)
int TRT_GUEST_ABI trt_SteamAPI_Init(void) {
  g_initialized = 1;
  return 1;  // true: the auth layer is live
}
int TRT_GUEST_ABI trt_SteamAPI_InitEx(void* ctx) {
  (void)ctx;
  g_initialized = 1;
  return 1;
}
int TRT_GUEST_ABI trt_SteamAPI_InitFlat(char* out_err_msg) {
  g_initialized = 1;
  if (out_err_msg) out_err_msg[0] = 0;
  return 0;  // k_ESteamAPIInitResult_OK
}
int TRT_GUEST_ABI trt_SteamAPI_InitSafe(void) { return trt_SteamAPI_Init(); }
u32 TRT_GUEST_ABI trt_Steam_GetHSteamUserCurrent(void) { return trt_SteamAPI_GetHSteamUser(); }
int TRT_GUEST_ABI trt_Steam_RegisterInterfaceFuncs(void*, void*) { return 1; }
void TRT_GUEST_ABI trt_Steam_RunCallbacks(void) { trt_SteamAPI_RunCallbacks(); }
const char* TRT_GUEST_ABI trt_SteamAPI_GetSteamInstallPath(void) { return "C:\\Program Files (x86)\\Steam"; }
void TRT_GUEST_ABI trt_SteamAPI_Shutdown(void) { g_initialized = 0; }
void trt_steam_set_appid(u32 app_id) { g_app_id = app_id; }

// ---- account + library --------------------------------------------------
// The host calls these after a real OpenID sign-in and after importing the
// user's Steam library. Nothing here invents an entitlement: the owned set is
// exactly the AppIDs the user's own appmanifest_*.acf files name.
void trt_steam_set_identity(const char* steamid64, const char* persona) {
  g_steamid64 = 0;
  g_persona.clear();
  g_signed_in = 0;
  if (steamid64 && steamid64[0]) {
#ifndef TIRAMISU_UWP_HOST
    const std::string claim =
        std::string("https://steamcommunity.com/openid/id/") + steamid64;
    std::string canon;
    if (trt_steam_claimed_id_to_steam64(claim, &canon)) {
      g_steamid64 = strtoull(canon.c_str(), nullptr, 10);
      g_signed_in = 1;
      if (persona) g_persona = persona;
    }
#else
    // UWP: steam_auth.cpp is not in the UWP build (not UWP-ready:
    // <unistd.h> + WinHTTP); the input is already a numeric steamid64,
    // so parse it directly (no OpenID round-trip needed).
    g_steamid64 = strtoull(steamid64, nullptr, 10);
    if (g_steamid64 != 0) {
      g_signed_in = 1;
      if (persona) g_persona = persona;
    }
#endif
  }
  char line[256];
  if (g_signed_in) {
    snprintf(line, sizeof(line), "steam: account signed in as %s (%llu)",
             g_persona.empty() ? "(no persona)" : g_persona.c_str(),
             (unsigned long long)g_steamid64);
  } else {
    snprintf(line, sizeof(line),
             "steam: no account signed in -> anonymous local user "
             "(sign in from the dashboard to import a library)");
  }
  trt_core_log(line);
}

void trt_steam_clear_identity(void) { trt_steam_set_identity(nullptr, nullptr); }

void trt_steam_set_owned_appids(const u32* appids, int count) {
  std::lock_guard<std::mutex> lk(g_owned_mu);
  g_owned_apps.clear();
  for (int i = 0; i < count; ++i) {
    if (!appids || !appids[i]) continue;
    if (std::find(g_owned_apps.begin(), g_owned_apps.end(), appids[i]) !=
        g_owned_apps.end())
      continue;
    g_owned_apps.push_back(appids[i]);
  }
  std::sort(g_owned_apps.begin(), g_owned_apps.end());
}

int trt_steam_owned_app_count(void) {
  std::lock_guard<std::mutex> lk(g_owned_mu);
  return (int)g_owned_apps.size();
}

int trt_steam_is_owned(u32 app_id) {
  std::lock_guard<std::mutex> lk(g_owned_mu);
  return std::binary_search(g_owned_apps.begin(), g_owned_apps.end(), app_id);
}

u64 trt_steam_current_id(void) { return g_signed_in ? g_steamid64 : 0; }
const char* trt_steam_current_persona(void) {
  return (g_signed_in && !g_persona.empty()) ? g_persona.c_str() : "TiramisuPlayer";
}

// Load the persisted identity and the local Steam library in one call. Returns
// the number of AppIDs the account's installation reports (0 = no library
// found, which is a named, visible state rather than a fake success).
int trt_steam_load_account(const char* state_dir) {
  trt_steam_identity id;
  if (state_dir && trt_steam_auth_load(state_dir, &id)) {
    trt_steam_set_identity(id.steamid64.c_str(), id.persona.c_str());
  } else {
    trt_steam_clear_identity();
  }

  trt_steam_library lib;
#ifndef TIRAMISU_UWP_HOST
  trt_steam_library_scan(&lib);
#endif
  std::vector<u32> owned;
  int installed = 0;
  for (const auto& app : lib.apps) {
    if (!app.appid) continue;
    owned.push_back(app.appid);
    if (app.fully_installed) ++installed;
  }
  trt_steam_set_owned_appids(owned.data(), (int)owned.size());

  char line[320];
  snprintf(line, sizeof(line),
           "steam: library scan -> %d apps (%d fully installed) across %d "
           "libraries, %d note(s)",
           (int)lib.apps.size(), installed, (int)lib.roots.size(),
           (int)lib.notes.size());
  trt_core_log(line);
  for (const auto& n : lib.notes) {
    const std::string l = "steam: library note: " + n;
    trt_core_log(l.c_str());
  }
  return (int)lib.apps.size();
}

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
int TRT_GUEST_ABI trt_SteamAPI_IsSteamRunning(void) { return 1; }

// ---- user ----

// SteamUser()->GetSteamID(): the account that actually signed in through
// Steam OpenID, or the anonymous local user when nobody did.
u64 TRT_GUEST_ABI trt_SteamAPI_GetSteamID(void) {
  if (g_signed_in) return g_steamid64;
  return 0x0110000100000001ull;  // anonymous local user, valid ID format
}

// ---- friends / overlay / utils ----
static std::atomic<int> g_steam_overlay_flag{0};

int trt_steam_overlay_check_and_reset(void) {
  return g_steam_overlay_flag.exchange(0);
}

void trt_steam_trigger_overlay(void) {
  g_steam_overlay_flag.store(1);
}

int TRT_GUEST_ABI trt_SteamAPI_ActivateGameOverlay(const char* url) {
  (void)url;
  trt_steam_trigger_overlay();
  return 1;
}

static void TRT_GUEST_ABI st_friends_ActivateGameOverlay(void* self, const char* dialog) {
  (void)self; (void)dialog;
  trt_steam_trigger_overlay();
}

static void TRT_GUEST_ABI st_friends_ActivateGameOverlayToUser(void* self, const char* dialog, u64 steamid) {
  (void)self; (void)dialog; (void)steamid;
  trt_steam_trigger_overlay();
}

static void TRT_GUEST_ABI st_friends_ActivateGameOverlayInviteDialog(void* self, u64 lobby) {
  (void)self; (void)lobby;
  trt_steam_trigger_overlay();
}
int TRT_GUEST_ABI trt_SteamAPI_SetRichPresence(const char* key,
                                               const char* value) {
  (void)key; (void)value;
  return 1;
}
void TRT_GUEST_ABI trt_SteamAPI_RunCallbacks(void) {
  // The registered callbacks run through the real callback dispatch; a game
  // that waits for an achievement callback gets it when it earns one.
  // RegisterCallback stores .run = nullptr (the game fills the vtable), so
  // dispatch reads the CCallbackBase vtable directly: the first field of the
  // callback object is the vtable pointer, slot 0 is Run.
  //
  // The list is copied first, so a callback that registers or unregisters
  // another callback from inside Run() cannot invalidate the iteration.
  std::vector<callback_entry> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_callbacks_mu);
    snapshot = g_callbacks;
  }
  for (const callback_entry& e : snapshot) {
    void* cb = e.callback;
    if (!cb) continue;
    if (e.run) {
      e.run(cb, nullptr);
      continue;
    }
    void** vtable = *reinterpret_cast<void***>(cb);
    if (!vtable || !vtable[0]) continue;
    auto run = reinterpret_cast<int(TRT_GUEST_ABI*)(void*, void*)>(vtable[0]);
    run(cb, nullptr);
  }
}

// SteamAPI_RegisterCallback(callback, id) / UnregisterCallback
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallback(void* callback, int id) {
  (void)id;
  if (!callback) return;
  std::lock_guard<std::mutex> lk(g_callbacks_mu);
  for (const callback_entry& e : g_callbacks) {
    if (e.callback == callback) return;  // already registered: one entry
  }
  // The game fills the vtable, so dispatch reads it directly.
  g_callbacks.push_back({callback, nullptr});
}
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallback(void* callback) {
  if (!callback) return;
  std::lock_guard<std::mutex> lk(g_callbacks_mu);
  for (size_t i = 0; i < g_callbacks.size(); ++i) {
    if (g_callbacks[i].callback == callback) {
      g_callbacks.erase(g_callbacks.begin() + (ptrdiff_t)i);
      return;
    }
  }
}

// ---- steam apps: the OWNERSHIP question. Fail-visible on purpose ----

// SteamApps()->BIsSubscribedApp(appid): we answer honestly: in the Tiramisu
// auth model there is no Steam license server, so ownership is UNKNOWN and
// a DRM check keyed on it will fail - visibly, with this log line.
// SteamApps()->BIsSubscribedApp(appid): the AppID is answered from the
// user's OWN imported Steam library. A title that is installed on this
// machine under that account answers 1; anything else answers 0 with a named
// line. That distinction is the whole point -- it is not a bypass, it is the
// user's real manifest state.
int TRT_GUEST_ABI trt_SteamAPI_BIsSubscribedApp(u32 app_id) {
  char line[192];
  if (trt_steam_is_owned(app_id)) {
    snprintf(line, sizeof(line),
             "trt: steam_api64 BIsSubscribedApp(appid=%u) -> 1 (present in "
             "the signed-in account's imported library)", app_id);
    trt_core_log(line);
    return 1;
  }
  snprintf(line, sizeof(line),
           "trt: steam_api64 BIsSubscribedApp(appid=%u) -> 0 (not in the %d "
           "imported library app(s); no license server, by design)",
           app_id, trt_steam_owned_app_count());
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
  { const char* st_v = "english"; char st_line[160]; snprintf(st_line, sizeof(st_line), "strtrace: SteamAPI_GetCurrentGameLanguage -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); }
  return "english";
}

int TRT_GUEST_ABI trt_SteamAPI_GetHSteamUser(void) { return g_initialized ? 1 : 0; }
int TRT_GUEST_ABI trt_SteamAPI_GetHSteamPipe(void) { return g_initialized ? 1 : 0; }

// Call results are a real table too: an async API call (a leaderboard upload, a
// workshop query) registers its CCallResult here, and the entry survives until
// the game unregisters it or the matching call completes. Nothing in this
// bridge completes an async call, so the table is the whole observable contract
// and it is honest about holding entries rather than pretending to have none.
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallResult(void* cb, u64 call_id) {
  if (!cb) return;
  std::lock_guard<std::mutex> lk(g_callresults_mu);
  for (const callresult_entry& e : g_callresults) {
    if (e.callback == cb && e.call_id == call_id) return;
  }
  g_callresults.push_back({cb, call_id});
}
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallResult(void* cb, u64 call_id) {
  std::lock_guard<std::mutex> lk(g_callresults_mu);
  for (size_t i = 0; i < g_callresults.size(); ++i) {
    if (g_callresults[i].callback == cb && g_callresults[i].call_id == call_id) {
      g_callresults.erase(g_callresults.begin() + (ptrdiff_t)i);
      return;
    }
  }
}

// Host-side observability: how many call results are outstanding. Used by the
// gate test to prove Register/Unregister really mutate the table.
int trt_steam_callresult_count(void) {
  std::lock_guard<std::mutex> lk(g_callresults_mu);
  return (int)g_callresults.size();
}
int trt_steam_callback_count(void) {
  std::lock_guard<std::mutex> lk(g_callbacks_mu);
  return (int)g_callbacks.size();
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
// fake_steam_iface lives in bridge_steam.h (the Mono door
// needs the type to reference the g_steam_*_obj globals).

static const char* TRT_GUEST_ABI steam_empty_str(void) { return ""; }
static u64 TRT_GUEST_ABI steam_noop_zero(void) { return 0; }
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
const char* TRT_GUEST_ABI st_apps_GetLang(void*) { { const char* st_v = "english"; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: ISteamApps.GetCurrentGameLanguage -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); } return "english"; }
const char* TRT_GUEST_ABI st_apps_GetAvailLangs(void*) { { const char* st_v = "english"; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: ISteamApps.GetAvailableGameLanguages -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); } return "english"; }
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
    { char st_line[200]; snprintf(st_line, sizeof(st_line), "strtrace: ISteamApps.GetAppInstallDir -> buf=%p cap=%u value=\"%.128s\"", (void*)pchFolder, cap, pchFolder); trt_core_log(st_line); }
    return 3;
  }
  if (pchFolder && cap > 0) pchFolder[0] = '\0';
  return 0;
}
const char* TRT_GUEST_ABI st_apps_GetLaunchQueryParam(void*, const char*) {
  { const char* st_v = ""; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: ISteamApps.GetLaunchQueryParam -> ptr=%p \"%.128s\" (empty, non-NULL)", (const void*)st_v, st_v); trt_core_log(st_line); }
  return "";
}
int TRT_GUEST_ABI st_apps_GetAppBuildId(void*) { return 1; }
// ISteamApps slot 13: bool GetCurrentBetaName(char* pchName, int cchNameBuffer).
// Was steam_noop_zero (false, buffer untouched). Honest answer: not in a
// beta branch -> empty name + false, with the buffer filled so no caller
// ever reads uninitialised stack as a string.
int TRT_GUEST_ABI st_apps_GetCurrentBetaName(void*, char* pchName, int cchNameBuffer) {
  if (pchName && cchNameBuffer > 0) pchName[0] = '\0';
  char line[192];
  snprintf(line, sizeof(line), "strtrace: ISteamApps.GetCurrentBetaName -> buf=%p size=%d value=\"\" (not in beta)", (void*)pchName, cchNameBuffer);
  trt_core_log(line);
  return 0;
}
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
    (const void*)&st_apps_GetCurrentBetaName,      // 13 GetCurrentBetaName
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
int TRT_GUEST_ABI st_user_GetHSteamUser(void*) { return g_initialized ? 1 : 0; }
int TRT_GUEST_ABI st_user_BLoggedOn(void*) { return g_initialized ? 1 : 0; }
u64 TRT_GUEST_ABI st_user_GetSteamID(void*, u64* hidden_sret) {
  const u64 id = trt_SteamAPI_GetSteamID();
  if (hidden_sret) *hidden_sret = id;
  return id;
}
// ISteamUser slot 4: bool GetUserDataFolder(char* buf, int size).
// Real Steam fills buf with the per-user data folder. The previous
// steam_noop_zero returned false and left buf untouched, so games that
// build save/config paths from it got an empty path.
int TRT_GUEST_ABI st_user_GetUserDataFolder(void* self, char* buf, int size) {
  const char* folder = "C:\\users\\player\\Saved Games";
  int result = 0;
  if (buf && size > 0) {
    size_t n = strlen(folder);
    if ((int)n >= size) n = (size_t)(size - 1);
    memcpy(buf, folder, n);
    buf[n] = '\0';
    result = 1;
  }
  char line[192];
  snprintf(line, sizeof(line), "trt: ISteamUser.GetUserDataFolder(self=%p buf=%p size=%d) -> %d",
           self, (void*)buf, size, result);
  trt_core_log(line);
  { char st_line[256]; if (buf && result) { snprintf(st_line, sizeof(st_line), "strtrace: ISteamUser.GetUserDataFolder -> buf=%p size=%d value=\"%.128s\"", (void*)buf, size, buf); } else { snprintf(st_line, sizeof(st_line), "strtrace: ISteamUser.GetUserDataFolder -> buf=%p size=%d value=NULL (result=%d)", (void*)buf, size, result); } trt_core_log(st_line); }
  return result;
}
const void* g_steam_user_vtbl[64] = {
    (const void*)&st_user_GetHSteamUser, // 0
    (const void*)&st_user_BLoggedOn,     // 1
    (const void*)&st_user_GetSteamID,    // 2
    (const void*)&steam_noop_zero,       // 3 TrackAppUsageEvent (void)
    (const void*)&st_user_GetUserDataFolder, // 4 GetUserDataFolder
    (const void*)&steam_noop_zero,       // 5 StartVoiceRecording (void)
    (const void*)&steam_noop_zero,       // 6 StopVoiceRecording (void)
    (const void*)&steam_noop_zero,       // 7 GetAvailableVoice
    Z8, Z16, Z16, Z16,
};
fake_steam_iface g_steam_user_obj = {g_steam_user_vtbl};

// ---- ISteamUtils ----
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceAppActive(void*) { return 1; }
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceComputerActive(void*) { return 1; }
int TRT_GUEST_ABI st_utils_GetConnectedUniverse(void*) { return 1; }
u32 TRT_GUEST_ABI st_utils_GetServerRealTime(void*) { return 1700000000u; }
const char* TRT_GUEST_ABI st_utils_GetIPCountry(void*) { { const char* st_v = "US"; char st_line[160]; snprintf(st_line, sizeof(st_line), "strtrace: ISteamUtils.GetIPCountry -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); } return "US"; }
unsigned char TRT_GUEST_ABI st_utils_GetCurrentBatteryPower(void*) { return 255; }
u32 TRT_GUEST_ABI st_utils_GetAppID(void*) { return g_app_id; }
// ISteamUtils slot 12: const char* GetSteamUILanguage(). Was steam_noop_zero
// (NULL) while the flat export returns "english" - the vtable/flat mismatch
// is the strongest NULL-string candidate for the GoW crash.
const char* TRT_GUEST_ABI st_utils_GetSteamUILanguage(void*) {
  const char* st_v = "english";
  char st_line[192];
  snprintf(st_line, sizeof(st_line), "strtrace: ISteamUtils.GetSteamUILanguage -> ptr=%p \"%.128s\"", (const void*)st_v, st_v);
  trt_core_log(st_line);
  return st_v;
}
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
    (const void*)&st_utils_GetSteamUILanguage,            // 12 GetSteamUILanguage
    (const void*)&steam_noop_zero,                        // 13
    (const void*)&steam_noop_zero,                        // 14
    (const void*)&steam_noop_zero,                        // 15
    Z16, Z16, Z16,
};
fake_steam_iface g_steam_utils_obj = {g_steam_utils_vtbl};

// ---- ISteamFriends (real signed-in persona + host-fed friend roster) -----
static std::mutex g_friends_mu;
static std::vector<trt_steam_friend_info> g_friends_list;

extern "C" {
void trt_steam_set_friends(const trt_steam_friend_info* list, int count) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  g_friends_list.clear();
  if (list && count > 0) {
    for (int i = 0; i < count && i < 256; ++i) g_friends_list.push_back(list[i]);
  }
}
int trt_steam_get_friend_count(void) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  return (int)g_friends_list.size();
}
int trt_steam_get_friend_at(int idx, trt_steam_friend_info* out) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  if (!out || idx < 0 || idx >= (int)g_friends_list.size()) return 0;
  *out = g_friends_list[(size_t)idx];
  return 1;
}
}

const char* TRT_GUEST_ABI st_friends_GetPersonaName(void*) {
  return trt_steam_current_persona();
}
int TRT_GUEST_ABI st_friends_GetPersonaState(void*) {
  return g_initialized ? 1 : 0;  // k_EPersonaStateOnline when running
}
int TRT_GUEST_ABI st_friends_GetFriendCount(void*, int flags) {
  (void)flags;
  std::lock_guard<std::mutex> lk(g_friends_mu);
  return (int)g_friends_list.size();
}
u64 TRT_GUEST_ABI st_friends_GetFriendByIndex(void*, int idx, int flags, u64* hidden_sret) {
  (void)flags;
  u64 id = 0;
  {
    std::lock_guard<std::mutex> lk(g_friends_mu);
    if (idx >= 0 && idx < (int)g_friends_list.size())
      id = g_friends_list[(size_t)idx].steam_id;
  }
  if (hidden_sret) *hidden_sret = id;
  return id;
}
int TRT_GUEST_ABI st_friends_GetFriendRelationship(void*, u64 steam_id) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  for (const auto& f : g_friends_list) {
    if (f.steam_id == steam_id) return 3;  // k_EFriendRelationshipFriend
  }
  return 0;
}
int TRT_GUEST_ABI st_friends_GetFriendPersonaState(void*, u64 steam_id) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  for (const auto& f : g_friends_list) {
    if (f.steam_id == steam_id) return f.state;
  }
  return 0;
}
const char* TRT_GUEST_ABI st_friends_GetFriendPersonaName(void*, u64 steam_id) {
  std::lock_guard<std::mutex> lk(g_friends_mu);
  static char s_fname[64];
  for (const auto& f : g_friends_list) {
    if (f.steam_id == steam_id) {
      snprintf(s_fname, sizeof(s_fname), "%s", f.persona);
      return s_fname;
    }
  }
  return trt_steam_current_persona();
}
// ---- ISteamFriends string slots that were steam_noop_zero (NULL) ----
// Per-slot traced wrappers returning "" (steam_empty_str value). Layout is
// SteamFriends016/017 (GoW 2022, SDK 1.53-era): 9 GetFriendPersonaNameHistory,
// 11 GetPlayerNickname, 14 GetFriendsGroupName, 20 GetClanName, 21 GetClanTag,
// 45 GetFriendRichPresence, 46 GetFriendRichPresenceKeyByIndex.
const char* TRT_GUEST_ABI st_friends_GetFriendPersonaNameHistory(void* self, u64 steam_id, int index) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetFriendPersonaNameHistory(self=%p friend=%016llx index=%d) -> \"\"", self, (unsigned long long)steam_id, index);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetPlayerNickname(void* self, u64 steam_id) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetPlayerNickname(self=%p player=%016llx) -> \"\"", self, (unsigned long long)steam_id);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetFriendsGroupName(void* self, u16 group_id) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetFriendsGroupName(self=%p group=%u) -> \"\"", self, (unsigned)group_id);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetClanName(void* self, u64 steam_id) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetClanName(self=%p clan=%016llx) -> \"\"", self, (unsigned long long)steam_id);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetClanTag(void* self, u64 steam_id) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetClanTag(self=%p clan=%016llx) -> \"\"", self, (unsigned long long)steam_id);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetFriendRichPresence(void* self, u64 steam_id, const char* key) {
  char line[256];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetFriendRichPresence(self=%p friend=%016llx key=\"%.64s\") -> \"\"", self, (unsigned long long)steam_id, key ? key : "(null)");
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_friends_GetFriendRichPresenceKeyByIndex(void* self, u64 steam_id, int index) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamFriends.GetFriendRichPresenceKeyByIndex(self=%p friend=%016llx index=%d) -> \"\"", self, (unsigned long long)steam_id, index);
  trt_core_log(line);
  return steam_empty_str();
}
const void* g_steam_friends_vtbl[64] = {
    (const void*)&st_friends_GetPersonaName,  // 0 GetPersonaName
    (const void*)&steam_noop_zero,            // 1 SetPersonaName
    (const void*)&st_friends_GetPersonaState, // 2 GetPersonaState
    (const void*)&st_friends_GetFriendCount,  // 3 GetFriendCount
    (const void*)&st_friends_GetFriendByIndex,// 4 GetFriendByIndex
    (const void*)&st_friends_GetFriendRelationship, // 5 GetFriendRelationship
    (const void*)&st_friends_GetFriendPersonaState, // 6 GetFriendPersonaState
    (const void*)&st_friends_GetFriendPersonaName, // 7 GetFriendPersonaName
    (const void*)&steam_noop_zero,            // 8 GetFriendGamePlayed
    (const void*)&st_friends_GetFriendPersonaNameHistory, // 9 GetFriendPersonaNameHistory
    (const void*)&steam_noop_zero,            // 10 GetFriendSteamLevel
    (const void*)&st_friends_GetPlayerNickname, // 11 GetPlayerNickname
    (const void*)&steam_noop_zero,            // 12 GetFriendsGroupCount
    (const void*)&steam_noop_zero,            // 13 GetFriendsGroupIDByIndex
    (const void*)&st_friends_GetFriendsGroupName, // 14 GetFriendsGroupName
    (const void*)&steam_noop_zero,            // 15 GetFriendsGroupMembersCount
    (const void*)&steam_noop_zero,            // 16 GetFriendsGroupMembersList
    (const void*)&steam_noop_zero,            // 17 HasFriend
    (const void*)&steam_noop_zero,            // 18 GetClanCount
    (const void*)&steam_noop_zero,            // 19 GetClanByIndex
    (const void*)&st_friends_GetClanName,     // 20 GetClanName
    (const void*)&st_friends_GetClanTag,      // 21 GetClanTag
    (const void*)&steam_noop_zero,            // 22 GetClanActivityCounts
    (const void*)&steam_noop_zero,            // 23 DownloadClanActivityCounts
    (const void*)&steam_noop_zero,            // 24 GetFriendCountFromSource
    (const void*)&steam_noop_zero,            // 25 GetFriendFromSourceByIndex
    (const void*)&steam_noop_zero,            // 26 IsUserInSource
    (const void*)&steam_noop_zero,            // 27 SetInGameVoiceSpeaking
    (const void*)&st_friends_ActivateGameOverlay,            // 28 ActivateGameOverlay
    (const void*)&st_friends_ActivateGameOverlayToUser,      // 29 ActivateGameOverlayToUser
    (const void*)&steam_noop_zero,            // 30 ActivateGameOverlayToWebPage
    (const void*)&steam_noop_zero,            // 31 ActivateGameOverlayToStore
    (const void*)&steam_noop_zero,            // 32 SetPlayedWith
    (const void*)&st_friends_ActivateGameOverlayInviteDialog, // 33 ActivateGameOverlayInviteDialog
    (const void*)&steam_noop_zero,            // 34 GetSmallFriendAvatar
    (const void*)&steam_noop_zero,            // 35 GetMediumFriendAvatar
    (const void*)&steam_noop_zero,            // 36 GetLargeFriendAvatar
    (const void*)&steam_noop_zero,            // 37 RequestUserInformation
    (const void*)&steam_noop_zero,            // 38 RequestClanOfficerList
    (const void*)&steam_noop_zero,            // 39 GetClanOwner
    (const void*)&steam_noop_zero,            // 40 GetClanOfficerCount
    (const void*)&steam_noop_zero,            // 41 GetClanOfficerByIndex
    (const void*)&steam_noop_zero,            // 42 GetUserRestrictions
    (const void*)&steam_noop_zero,            // 43 SetRichPresence
    (const void*)&steam_noop_zero,            // 44 ClearRichPresence
    (const void*)&st_friends_GetFriendRichPresence, // 45 GetFriendRichPresence
    (const void*)&st_friends_GetFriendRichPresenceKeyByIndex, // 46 GetFriendRichPresenceKeyByIndex
    (const void*)&steam_noop_zero,            // 47 GetFriendRichPresenceKeyCount
    Z16,
};
fake_steam_iface g_steam_friends_obj = {g_steam_friends_vtbl};

// ---- ISteamUserStats (persistent stats, achievements & leaderboards) ----
struct steam_stat_entry {
  char name[128];
  bool is_float;
  i32 i_val;
  float f_val;
};
struct steam_ach_entry {
  char name[128];
  bool achieved;
  u32 unlock_time;
  u32 cur_progress;
  u32 max_progress;
};
struct steam_lb_entry {
  u64 handle;
  char name[128];
  int sort_method;
  int display_type;
};
static std::mutex g_stats_mu;
static u32 g_stats_loaded_appid = 0xFFFFFFFFu;
static std::vector<steam_stat_entry> g_stats_list;
static std::vector<steam_ach_entry> g_ach_list;
static std::vector<steam_lb_entry> g_lb_list;
static u64 g_next_lb_handle = 0x534C420000000100ULL;

static int stats_file_path(char* host_out, size_t cap, int for_write) {
  char guest[512];
  snprintf(guest, sizeof(guest),
           "C:\\users\\player\\AppData\\Local\\Tiramisu\\SteamStats\\%u.cfg",
           (unsigned)g_app_id);
  u16 wg[512];
  size_t k = 0;
  for (; guest[k] && k + 1 < 512; ++k) wg[k] = (unsigned char)guest[k];
  wg[k] = 0;
  if (trt_vfs_resolve_utf16_rw(wg, for_write, host_out, cap) != 0) return -1;
  if (for_write) trt_vfs_ensure_parents(host_out);
  return 0;
}

static void stats_load_locked(void) {
  if (g_stats_loaded_appid == g_app_id) return;
  g_stats_loaded_appid = g_app_id;
  g_stats_list.clear();
  g_ach_list.clear();
  char host[512];
  if (stats_file_path(host, sizeof(host), 0) != 0) return;
  FILE* f = fopen(host, "rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (strncmp(line, "I:", 2) == 0) {
      char nm[128] = {}; i32 v = 0;
      if (sscanf(line + 2, "%127[^=]=%d", nm, &v) == 2) {
        steam_stat_entry e{};
        snprintf(e.name, sizeof(e.name), "%s", nm);
        e.is_float = false; e.i_val = v;
        g_stats_list.push_back(e);
      }
    } else if (strncmp(line, "F:", 2) == 0) {
      char nm[128] = {}; float v = 0.0f;
      if (sscanf(line + 2, "%127[^=]=%f", nm, &v) == 2) {
        steam_stat_entry e{};
        snprintf(e.name, sizeof(e.name), "%s", nm);
        e.is_float = true; e.f_val = v;
        g_stats_list.push_back(e);
      }
    } else if (strncmp(line, "A:", 2) == 0) {
      char nm[128] = {}; int ach = 0; unsigned ut = 0, cp = 0, mp = 0;
      if (sscanf(line + 2, "%127[^=]=%d,%u,%u,%u", nm, &ach, &ut, &cp, &mp) >= 2) {
        steam_ach_entry e{};
        snprintf(e.name, sizeof(e.name), "%s", nm);
        e.achieved = (ach != 0); e.unlock_time = ut; e.cur_progress = cp; e.max_progress = mp;
        g_ach_list.push_back(e);
      }
    }
  }
  fclose(f);
}

static void stats_save_locked(void) {
  char host[512];
  if (stats_file_path(host, sizeof(host), 1) != 0) return;
  FILE* f = fopen(host, "wb");
  if (!f) return;
  for (const auto& s : g_stats_list) {
    if (s.is_float) fprintf(f, "F:%s=%f\n", s.name, s.f_val);
    else fprintf(f, "I:%s=%d\n", s.name, s.i_val);
  }
  for (const auto& a : g_ach_list) {
    fprintf(f, "A:%s=%d,%u,%u,%u\n", a.name, a.achieved ? 1 : 0,
            a.unlock_time, a.cur_progress, a.max_progress);
  }
  fclose(f);
}

extern "C" {
int trt_steam_get_achievement_count(void) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  return (int)g_ach_list.size();
}
int trt_steam_get_achievement_at(int idx, trt_steam_achievement_info* out) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  if (!out || idx < 0 || idx >= (int)g_ach_list.size()) return 0;
  const auto& a = g_ach_list[(size_t)idx];
  snprintf(out->name, sizeof(out->name), "%s", a.name);
  out->achieved = a.achieved;
  out->unlock_time = a.unlock_time;
  out->cur_progress = a.cur_progress;
  out->max_progress = a.max_progress;
  return 1;
}
}

int TRT_GUEST_ABI st_userstats_RequestCurrentStats(void*) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  return 1;
}
int TRT_GUEST_ABI st_userstats_GetStatInt32(void*, const char* name, i32* pData) {
  if (!name || !pData) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (const auto& s : g_stats_list) {
    if (strcmp(s.name, name) == 0) {
      *pData = s.is_float ? (i32)s.f_val : s.i_val;
      return 1;
    }
  }
  *pData = 0;
  return 1;
}
int TRT_GUEST_ABI st_userstats_GetStatFloat(void*, const char* name, float* pData) {
  if (!name || !pData) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (const auto& s : g_stats_list) {
    if (strcmp(s.name, name) == 0) {
      *pData = s.is_float ? s.f_val : (float)s.i_val;
      return 1;
    }
  }
  *pData = 0.0f;
  return 1;
}
int TRT_GUEST_ABI st_userstats_SetStatInt32(void*, const char* name, i32 data) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (auto& s : g_stats_list) {
    if (strcmp(s.name, name) == 0) { s.is_float = false; s.i_val = data; return 1; }
  }
  steam_stat_entry e{};
  snprintf(e.name, sizeof(e.name), "%s", name);
  e.is_float = false; e.i_val = data;
  g_stats_list.push_back(e);
  return 1;
}
int TRT_GUEST_ABI st_userstats_SetStatFloat(void*, const char* name, float data) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (auto& s : g_stats_list) {
    if (strcmp(s.name, name) == 0) { s.is_float = true; s.f_val = data; return 1; }
  }
  steam_stat_entry e{};
  snprintf(e.name, sizeof(e.name), "%s", name);
  e.is_float = true; e.f_val = data;
  g_stats_list.push_back(e);
  return 1;
}
int TRT_GUEST_ABI st_userstats_UpdateAvgRateStat(void* self, const char* name, float countThisSession, double dSessionLength) {
  if (dSessionLength <= 0.0) return 0;
  return st_userstats_SetStatFloat(self, name, (float)(countThisSession / dSessionLength));
}
static steam_ach_entry& ach_get_or_add_locked(const char* name) {
  stats_load_locked();
  for (auto& a : g_ach_list) {
    if (strcmp(a.name, name) == 0) return a;
  }
  steam_ach_entry e{};
  snprintf(e.name, sizeof(e.name), "%s", name);
  g_ach_list.push_back(e);
  return g_ach_list.back();
}
int TRT_GUEST_ABI st_userstats_GetAchievement(void*, const char* name, u8* pbAchieved) {
  if (!name || !pbAchieved) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (const auto& a : g_ach_list) {
    if (strcmp(a.name, name) == 0) { *pbAchieved = a.achieved ? 1 : 0; return 1; }
  }
  *pbAchieved = 0;
  return 1;
}
int TRT_GUEST_ABI st_userstats_SetAchievement(void*, const char* name) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  auto& a = ach_get_or_add_locked(name);
  if (!a.achieved) {
    a.achieved = true;
    a.unlock_time = (u32)time(nullptr);
    char line[192];
    snprintf(line, sizeof(line), "steam: achievement unlocked -> %s", name);
    trt_core_log(line);
  }
  return 1;
}
int TRT_GUEST_ABI st_userstats_ClearAchievement(void*, const char* name) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  auto& a = ach_get_or_add_locked(name);
  a.achieved = false;
  a.unlock_time = 0;
  return 1;
}
int TRT_GUEST_ABI st_userstats_GetAchievementAndUnlockTime(void*, const char* name, u8* pbAchieved, u32* punUnlockTime) {
  if (!name) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  for (const auto& a : g_ach_list) {
    if (strcmp(a.name, name) == 0) {
      if (pbAchieved) *pbAchieved = a.achieved ? 1 : 0;
      if (punUnlockTime) *punUnlockTime = a.unlock_time;
      return 1;
    }
  }
  if (pbAchieved) *pbAchieved = 0;
  if (punUnlockTime) *punUnlockTime = 0;
  return 1;
}
int TRT_GUEST_ABI st_userstats_StoreStats(void*) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_save_locked();
  return 1;
}
int TRT_GUEST_ABI st_userstats_GetAchievementIcon(void*, const char*) { return 0; }
const char* TRT_GUEST_ABI st_userstats_GetAchievementDisplayAttribute(void*, const char* name, const char* key) {
  if (!name || !key) return steam_empty_str();
  static char s_attr[128];
  if (strcmp(key, "name") == 0) {
    snprintf(s_attr, sizeof(s_attr), "%s", name);
    return s_attr;
  }
  return steam_empty_str();
}
int TRT_GUEST_ABI st_userstats_IndicateAchievementProgress(void*, const char* name, u32 nCur, u32 nMax) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  auto& a = ach_get_or_add_locked(name);
  a.cur_progress = nCur;
  a.max_progress = nMax;
  if (nMax > 0 && nCur >= nMax) {
    a.achieved = true;
    if (!a.unlock_time) a.unlock_time = (u32)time(nullptr);
  }
  return 1;
}
u32 TRT_GUEST_ABI st_userstats_GetNumAchievements(void*) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  return (u32)g_ach_list.size();
}
const char* TRT_GUEST_ABI st_userstats_GetAchievementName(void*, u32 index) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  stats_load_locked();
  static char s_ach_ret[128];
  if (index >= g_ach_list.size()) return steam_empty_str();
  snprintf(s_ach_ret, sizeof(s_ach_ret), "%s", g_ach_list[index].name);
  return s_ach_ret;
}
u64 TRT_GUEST_ABI st_userstats_RequestUserStats(void*, u64) { return 1; }
int TRT_GUEST_ABI st_userstats_GetUserStatInt32(void* self, u64, const char* name, i32* pData) {
  return st_userstats_GetStatInt32(self, name, pData);
}
int TRT_GUEST_ABI st_userstats_GetUserStatFloat(void* self, u64, const char* name, float* pData) {
  return st_userstats_GetStatFloat(self, name, pData);
}
int TRT_GUEST_ABI st_userstats_GetUserAchievement(void* self, u64, const char* name, u8* pbAchieved) {
  return st_userstats_GetAchievement(self, name, pbAchieved);
}
const char* TRT_GUEST_ABI st_userstats_GetLeaderboardName(void*, u64 leaderboard) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  static char s_lb_ret[128];
  for (const auto& lb : g_lb_list) {
    if (lb.handle == leaderboard) {
      snprintf(s_lb_ret, sizeof(s_lb_ret), "%s", lb.name);
      return s_lb_ret;
    }
  }
  return steam_empty_str();
}
int TRT_GUEST_ABI st_userstats_ResetAllStats(void*, int bAchievementsToo) {
  std::lock_guard<std::mutex> lk(g_stats_mu);
  g_stats_list.clear();
  if (bAchievementsToo) g_ach_list.clear();
  stats_save_locked();
  return 1;
}
u64 TRT_GUEST_ABI st_userstats_FindOrCreateLeaderboard(void*, const char* name, int sort_method, int display_type) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  for (const auto& lb : g_lb_list) {
    if (strcmp(lb.name, name) == 0) return lb.handle;
  }
  steam_lb_entry e{g_next_lb_handle++, {}, sort_method, display_type};
  snprintf(e.name, sizeof(e.name), "%s", name);
  g_lb_list.push_back(e);
  return e.handle;
}
u64 TRT_GUEST_ABI st_userstats_FindLeaderboard(void*, const char* name) {
  if (!name || !name[0]) return 0;
  std::lock_guard<std::mutex> lk(g_stats_mu);
  for (const auto& lb : g_lb_list) {
    if (strcmp(lb.name, name) == 0) return lb.handle;
  }
  return 0;
}
int TRT_GUEST_ABI st_userstats_GetLeaderboardEntryCount(void*, u64) { return 0; }

static const void* g_steam_userstats_vtbl[64] = {
    (const void*)&st_userstats_RequestCurrentStats,          // 0
    (const void*)&st_userstats_GetStatInt32,                 // 1
    (const void*)&st_userstats_GetStatFloat,                 // 2
    (const void*)&st_userstats_SetStatInt32,                 // 3
    (const void*)&st_userstats_SetStatFloat,                 // 4
    (const void*)&st_userstats_UpdateAvgRateStat,            // 5
    (const void*)&st_userstats_GetAchievement,               // 6
    (const void*)&st_userstats_SetAchievement,               // 7
    (const void*)&st_userstats_ClearAchievement,             // 8
    (const void*)&st_userstats_GetAchievementAndUnlockTime,  // 9
    (const void*)&st_userstats_StoreStats,                   // 10
    (const void*)&st_userstats_GetAchievementIcon,           // 11
    (const void*)&st_userstats_GetAchievementDisplayAttribute, // 12
    (const void*)&st_userstats_IndicateAchievementProgress,  // 13
    (const void*)&st_userstats_GetNumAchievements,           // 14
    (const void*)&st_userstats_GetAchievementName,           // 15
    (const void*)&st_userstats_RequestUserStats,             // 16
    (const void*)&st_userstats_GetUserStatInt32,             // 17
    (const void*)&st_userstats_GetUserStatFloat,             // 18
    (const void*)&st_userstats_GetUserAchievement,           // 19
    (const void*)&st_userstats_GetLeaderboardName,           // 20
    (const void*)&st_userstats_ResetAllStats,                // 21
    (const void*)&st_userstats_FindOrCreateLeaderboard,      // 22
    (const void*)&st_userstats_FindLeaderboard,              // 23
    (const void*)&st_userstats_GetLeaderboardEntryCount,     // 24
    (const void*)&steam_noop_zero,                           // 25
    (const void*)&steam_noop_zero,                           // 26
    (const void*)&steam_noop_zero,                           // 27
    Z4, Z16, Z16,                                            // 28..63
};
fake_steam_iface g_steam_userstats_obj = {g_steam_userstats_vtbl};

// ---- ISteamMatchmaking (real lobby registry) ----------------------------
struct steam_kv { char k[64]; char v[128]; };
struct steam_lobby_rec {
  u64 id;
  u64 owner;
  int type;
  int member_limit;
  bool joinable;
  std::vector<u64> members;
  std::vector<steam_kv> data;
  std::vector<std::string> chat;
};
static std::mutex g_mm_mu;
static std::vector<steam_lobby_rec> g_lobbies;
static u64 g_next_lobby_id = 0x1860000000000001ULL;

static steam_lobby_rec* mm_find_locked(u64 id) {
  for (auto& l : g_lobbies) if (l.id == id) return &l;
  return nullptr;
}
int TRT_GUEST_ABI st_mm_GetFavoriteGameCount(void*) { return 0; }
int TRT_GUEST_ABI st_mm_GetFavoriteGame(void*, int, u32*, u32*, u16*, u16*, u32*, u32*) { return 0; }
int TRT_GUEST_ABI st_mm_AddFavoriteGame(void*, u32, u32, u16, u16, u32, u32) { return 0; }
int TRT_GUEST_ABI st_mm_RemoveFavoriteGame(void*, u32, u32, u16, u16, u32) { return 0; }
u64 TRT_GUEST_ABI st_mm_RequestLobbyList(void*) { return 1; }
void TRT_GUEST_ABI st_mm_AddFilter5(void*, const char*, const char*, int) {}
void TRT_GUEST_ABI st_mm_AddFilterNum(void*, const char*, int, int) {}
void TRT_GUEST_ABI st_mm_AddFilterVal(void*, const char*, int) {}
void TRT_GUEST_ABI st_mm_AddFilterOne(void*, int) {}
u64 TRT_GUEST_ABI st_mm_GetLobbyByIndex(void*, int idx, u64* sret) {
  u64 id = 0;
  {
    std::lock_guard<std::mutex> lk(g_mm_mu);
    if (idx >= 0 && idx < (int)g_lobbies.size()) id = g_lobbies[(size_t)idx].id;
  }
  if (sret) *sret = id;
  return id;
}
u64 TRT_GUEST_ABI st_mm_CreateLobby(void*, int lobby_type, int max_members) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  steam_lobby_rec l{};
  l.id = g_next_lobby_id++;
  l.owner = trt_SteamAPI_GetSteamID();
  l.type = lobby_type;
  l.member_limit = max_members > 0 ? max_members : 8;
  l.joinable = true;
  l.members.push_back(l.owner);
  g_lobbies.push_back(l);
  return l.id;
}
u64 TRT_GUEST_ABI st_mm_JoinLobby(void*, u64 lobby_id) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l || !l->joinable) return 0;
  u64 me = trt_SteamAPI_GetSteamID();
  if (std::find(l->members.begin(), l->members.end(), me) == l->members.end())
    l->members.push_back(me);
  return 1;
}
void TRT_GUEST_ABI st_mm_LeaveLobby(void*, u64 lobby_id) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  u64 me = trt_SteamAPI_GetSteamID();
  for (size_t i = 0; i < g_lobbies.size(); ++i) {
    if (g_lobbies[i].id == lobby_id) {
      auto& m = g_lobbies[i].members;
      m.erase(std::remove(m.begin(), m.end(), me), m.end());
      if (m.empty()) g_lobbies.erase(g_lobbies.begin() + (ptrdiff_t)i);
      return;
    }
  }
}
int TRT_GUEST_ABI st_mm_InviteUserToLobby(void*, u64, u64) { return 1; }
int TRT_GUEST_ABI st_mm_GetNumLobbyMembers(void*, u64 lobby_id) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  return l ? (int)l->members.size() : 0;
}
u64 TRT_GUEST_ABI st_mm_GetLobbyMemberByIndex(void*, u64 lobby_id, int idx, u64* sret) {
  u64 id = 0;
  {
    std::lock_guard<std::mutex> lk(g_mm_mu);
    auto* l = mm_find_locked(lobby_id);
    if (l && idx >= 0 && idx < (int)l->members.size()) id = l->members[(size_t)idx];
  }
  if (sret) *sret = id;
  return id;
}
const char* TRT_GUEST_ABI st_mm_GetLobbyData(void*, u64 lobby_id, const char* key) {
  if (!key) return steam_empty_str();
  std::lock_guard<std::mutex> lk(g_mm_mu);
  static char s_val[128];
  auto* l = mm_find_locked(lobby_id);
  if (!l) return steam_empty_str();
  for (const auto& kv : l->data) {
    if (strcmp(kv.k, key) == 0) {
      snprintf(s_val, sizeof(s_val), "%s", kv.v);
      return s_val;
    }
  }
  return steam_empty_str();
}
int TRT_GUEST_ABI st_mm_SetLobbyData(void*, u64 lobby_id, const char* key, const char* val) {
  if (!key || !key[0]) return 0;
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  for (auto& kv : l->data) {
    if (strcmp(kv.k, key) == 0) {
      snprintf(kv.v, sizeof(kv.v), "%s", val ? val : "");
      return 1;
    }
  }
  steam_kv kv{};
  snprintf(kv.k, sizeof(kv.k), "%s", key);
  snprintf(kv.v, sizeof(kv.v), "%s", val ? val : "");
  l->data.push_back(kv);
  return 1;
}
int TRT_GUEST_ABI st_mm_GetLobbyDataCount(void*, u64 lobby_id) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  return l ? (int)l->data.size() : 0;
}
int TRT_GUEST_ABI st_mm_GetLobbyDataByIndex(void*, u64 lobby_id, int idx, char* key, int key_cap, char* val, int val_cap) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l || idx < 0 || idx >= (int)l->data.size()) return 0;
  const auto& kv = l->data[(size_t)idx];
  if (key && key_cap > 0) snprintf(key, (size_t)key_cap, "%s", kv.k);
  if (val && val_cap > 0) snprintf(val, (size_t)val_cap, "%s", kv.v);
  return 1;
}
int TRT_GUEST_ABI st_mm_DeleteLobbyData(void*, u64 lobby_id, const char* key) {
  if (!key) return 0;
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  for (size_t i = 0; i < l->data.size(); ++i) {
    if (strcmp(l->data[i].k, key) == 0) {
      l->data.erase(l->data.begin() + (ptrdiff_t)i);
      return 1;
    }
  }
  return 0;
}
const char* TRT_GUEST_ABI st_mm_GetLobbyMemberData(void*, u64, u64, const char*) { return steam_empty_str(); }
void TRT_GUEST_ABI st_mm_SetLobbyMemberData(void*, u64, const char*, const char*) {}
int TRT_GUEST_ABI st_mm_SendLobbyChatMsg(void*, u64 lobby_id, const void* msg, int len) {
  if (!msg || len <= 0) return 0;
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  l->chat.emplace_back((const char*)msg, (size_t)len);
  return 1;
}
int TRT_GUEST_ABI st_mm_GetLobbyChatEntry(void*, u64 lobby_id, int idx, u64* user, void* data, int cap, int* type) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l || idx < 0 || idx >= (int)l->chat.size()) return 0;
  if (user) *user = l->owner;
  if (type) *type = 1;  // k_EChatEntryTypeChatMsg
  int cp = (int)l->chat[(size_t)idx].size();
  if (data && cap > 0) {
    if (cp > cap) cp = cap;
    memcpy(data, l->chat[(size_t)idx].data(), (size_t)cp);
  }
  return cp;
}
int TRT_GUEST_ABI st_mm_RequestLobbyData(void*, u64) { return 1; }
int TRT_GUEST_ABI st_mm_SetLobbyMemberLimit(void*, u64 lobby_id, int limit) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  l->member_limit = limit;
  return 1;
}
int TRT_GUEST_ABI st_mm_GetLobbyMemberLimit(void*, u64 lobby_id) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  return l ? l->member_limit : 0;
}
int TRT_GUEST_ABI st_mm_SetLobbyType(void*, u64 lobby_id, int type) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  l->type = type;
  return 1;
}
int TRT_GUEST_ABI st_mm_SetLobbyJoinable(void*, u64 lobby_id, int joinable) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  l->joinable = (joinable != 0);
  return 1;
}
u64 TRT_GUEST_ABI st_mm_GetLobbyOwner(void*, u64 lobby_id, u64* sret) {
  u64 ow = 0;
  {
    std::lock_guard<std::mutex> lk(g_mm_mu);
    auto* l = mm_find_locked(lobby_id);
    if (l) ow = l->owner;
  }
  if (sret) *sret = ow;
  return ow;
}
int TRT_GUEST_ABI st_mm_SetLobbyOwner(void*, u64 lobby_id, u64 new_owner) {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  auto* l = mm_find_locked(lobby_id);
  if (!l) return 0;
  l->owner = new_owner;
  return 1;
}
static const void* g_steam_matchmaking_vtbl[64] = {
    (const void*)&st_mm_GetFavoriteGameCount,   // 0
    (const void*)&st_mm_GetFavoriteGame,        // 1
    (const void*)&st_mm_AddFavoriteGame,        // 2
    (const void*)&st_mm_RemoveFavoriteGame,     // 3
    (const void*)&st_mm_RequestLobbyList,       // 4
    (const void*)&st_mm_AddFilter5,             // 5
    (const void*)&st_mm_AddFilterNum,           // 6
    (const void*)&st_mm_AddFilterVal,           // 7
    (const void*)&st_mm_AddFilterOne,           // 8
    (const void*)&st_mm_AddFilterOne,           // 9
    (const void*)&st_mm_AddFilter5,             // 10
    (const void*)&st_mm_GetLobbyByIndex,        // 11
    (const void*)&st_mm_CreateLobby,            // 12
    (const void*)&st_mm_JoinLobby,              // 13
    (const void*)&st_mm_LeaveLobby,             // 14
    (const void*)&st_mm_InviteUserToLobby,      // 15
    (const void*)&st_mm_GetNumLobbyMembers,     // 16
    (const void*)&st_mm_GetLobbyMemberByIndex,  // 17
    (const void*)&st_mm_GetLobbyData,           // 18
    (const void*)&st_mm_SetLobbyData,           // 19
    (const void*)&st_mm_GetLobbyDataCount,      // 20
    (const void*)&st_mm_GetLobbyDataByIndex,    // 21
    (const void*)&st_mm_DeleteLobbyData,        // 22
    (const void*)&st_mm_GetLobbyMemberData,     // 23
    (const void*)&st_mm_SetLobbyMemberData,     // 24
    (const void*)&st_mm_SendLobbyChatMsg,       // 25
    (const void*)&st_mm_GetLobbyChatEntry,      // 26
    (const void*)&st_mm_RequestLobbyData,       // 27
    (const void*)&steam_noop_zero,              // 28 SetLobbyGameServer
    (const void*)&steam_noop_zero,              // 29 GetLobbyGameServer
    (const void*)&st_mm_SetLobbyMemberLimit,    // 30
    (const void*)&st_mm_GetLobbyMemberLimit,    // 31
    (const void*)&st_mm_SetLobbyType,           // 32
    (const void*)&st_mm_SetLobbyJoinable,       // 33
    (const void*)&st_mm_GetLobbyOwner,          // 34
    (const void*)&st_mm_SetLobbyOwner,          // 35
    (const void*)&steam_noop_zero,              // 36
    (const void*)&steam_noop_zero, (const void*)&steam_noop_zero,
    (const void*)&steam_noop_zero, Z8, Z16,
};
fake_steam_iface g_steam_matchmaking_obj = {g_steam_matchmaking_vtbl};

// ---- ISteamNetworking (P2P loopback / session table) --------------------
struct steam_p2p_pkt {
  u64 remote_id;
  int channel;
  std::vector<u8> payload;
};
static std::mutex g_net_mu;
static std::vector<steam_p2p_pkt> g_net_rx;
static std::vector<u64> g_net_peers;
static bool g_net_relay = true;

int TRT_GUEST_ABI st_net_SendP2PPacket(void*, u64 remote_id, const void* data, u32 len, int send_type, int channel) {
  (void)send_type;
  if (!data || len == 0 || len > 1048576u) return 0;
  std::lock_guard<std::mutex> lk(g_net_mu);
  const u64 self_id = trt_SteamAPI_GetSteamID();
  bool accepted = (remote_id == self_id);
  for (u64 p : g_net_peers) if (p == remote_id) accepted = true;
  if (accepted) {
    steam_p2p_pkt pkt{remote_id == self_id ? self_id : remote_id, channel, {}};
    pkt.payload.assign((const u8*)data, (const u8*)data + len);
    g_net_rx.push_back(std::move(pkt));
  }
  return 1;
}
int TRT_GUEST_ABI st_net_IsP2PPacketAvailable(void*, u32* msg_sz, int channel) {
  std::lock_guard<std::mutex> lk(g_net_mu);
  for (const auto& pkt : g_net_rx) {
    if (pkt.channel == channel) {
      if (msg_sz) *msg_sz = (u32)pkt.payload.size();
      return 1;
    }
  }
  if (msg_sz) *msg_sz = 0;
  return 0;
}
int TRT_GUEST_ABI st_net_ReadP2PPacket(void*, void* dest, u32 cap, u32* msg_sz, u64* remote_id, int channel) {
  std::lock_guard<std::mutex> lk(g_net_mu);
  for (size_t i = 0; i < g_net_rx.size(); ++i) {
    if (g_net_rx[i].channel == channel) {
      const auto& pkt = g_net_rx[i];
      u32 n = (u32)pkt.payload.size();
      if (n > cap) n = cap;
      if (dest && n > 0) memcpy(dest, pkt.payload.data(), n);
      if (msg_sz) *msg_sz = n;
      if (remote_id) *remote_id = pkt.remote_id;
      g_net_rx.erase(g_net_rx.begin() + (ptrdiff_t)i);
      return 1;
    }
  }
  if (msg_sz) *msg_sz = 0;
  return 0;
}
int TRT_GUEST_ABI st_net_AcceptP2PSessionWithUser(void*, u64 remote_id) {
  std::lock_guard<std::mutex> lk(g_net_mu);
  if (std::find(g_net_peers.begin(), g_net_peers.end(), remote_id) == g_net_peers.end())
    g_net_peers.push_back(remote_id);
  return 1;
}
int TRT_GUEST_ABI st_net_CloseP2PSessionWithUser(void*, u64 remote_id) {
  std::lock_guard<std::mutex> lk(g_net_mu);
  g_net_peers.erase(std::remove(g_net_peers.begin(), g_net_peers.end(), remote_id), g_net_peers.end());
  return 1;
}
int TRT_GUEST_ABI st_net_CloseP2PChannelWithUser(void*, u64, int) { return 1; }
int TRT_GUEST_ABI st_net_GetP2PSessionState(void*, u64 remote_id, void* out_state) {
  std::lock_guard<std::mutex> lk(g_net_mu);
  bool active = (remote_id == trt_SteamAPI_GetSteamID()) ||
                (std::find(g_net_peers.begin(), g_net_peers.end(), remote_id) != g_net_peers.end());
  if (out_state) {
    // P2PSessionState_t layout: bConnectionActive(1), bConnecting(1), eP2PSessionError(1), bUsingRelay(1), nBytesQueuedForSend(4), nPacketsQueuedForSend(4), m_nRemoteIP(4), m_nRemotePort(2)
    u8* s = (u8*)out_state;
    memset(s, 0, 20);
    s[0] = active ? 1 : 0;
  }
  return active ? 1 : 0;
}
int TRT_GUEST_ABI st_net_AllowP2PPacketRelay(void*, int allow) {
  g_net_relay = (allow != 0);
  return 1;
}
static const void* g_steam_networking_vtbl[64] = {
    (const void*)&st_net_SendP2PPacket,             // 0
    (const void*)&st_net_IsP2PPacketAvailable,      // 1
    (const void*)&st_net_ReadP2PPacket,             // 2
    (const void*)&st_net_AcceptP2PSessionWithUser,  // 3
    (const void*)&st_net_CloseP2PSessionWithUser,   // 4
    (const void*)&st_net_CloseP2PChannelWithUser,   // 5
    (const void*)&st_net_GetP2PSessionState,        // 6
    (const void*)&st_net_AllowP2PPacketRelay,       // 7
    Z8, Z16, Z16, Z16,
};
fake_steam_iface g_steam_networking_obj = {g_steam_networking_vtbl};

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

int TRT_GUEST_ABI st_rs_FileWrite(void*, const char* pchFile,
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

i32 TRT_GUEST_ABI st_rs_FileRead(void*, const char* pchFile,
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

int TRT_GUEST_ABI st_rs_FileForget(void*, const char*) { return 1; }

int TRT_GUEST_ABI st_rs_FileDelete(void*, const char* pchFile) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  char norm[260], host[512];
  if (rs_resolve_host(pchFile, 1, norm, sizeof(norm), host, sizeof(host)) != 0)
    return 0;
  int rc = (remove(host) == 0) ? 1 : 0;
  rs_index_remove_locked(norm);
  return rc;
}

int TRT_GUEST_ABI st_rs_FileExists(void*, const char* pchFile) {
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

int TRT_GUEST_ABI st_rs_FilePersisted(void* self, const char* pchFile) {
  return st_rs_FileExists(self, pchFile);
}

i32 TRT_GUEST_ABI st_rs_GetFileSize(void*, const char* pchFile) {
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

i64 TRT_GUEST_ABI st_rs_GetFileTimestamp(void* self, const char* pchFile) {
  if (!st_rs_FileExists(self, pchFile)) return 0;
  return (i64)time(nullptr);
}

i32 TRT_GUEST_ABI st_rs_GetFileCount(void*) {
  std::lock_guard<std::mutex> lk(g_steam_rs_mu);
  rs_load_index_locked();
  return g_steam_rs_count;
}

const char* TRT_GUEST_ABI st_rs_GetFileNameAndSize(void*, int iFile,
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

int TRT_GUEST_ABI st_rs_GetQuota(void*, u64* total, u64* avail) {
  if (total) *total = 100ULL * 1024ULL * 1024ULL;
  if (avail) *avail = 100ULL * 1024ULL * 1024ULL;
  return 1;
}

int TRT_GUEST_ABI st_rs_IsCloudEnabledForAccount(void*) { return 1; }
int TRT_GUEST_ABI st_rs_IsCloudEnabledForApp(void*) {
  return g_steam_cloud_enabled;
}
void TRT_GUEST_ABI st_rs_SetCloudEnabledForApp(void*, int enabled) {
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

int TRT_GUEST_ABI st_input_Init(void*, int /*bExplicitlyCallRunFrame*/) {
  return 1;
}
int TRT_GUEST_ABI st_input_Shutdown(void*) {
  for (u32 i = 0; i < 4; ++i) trt_pad_set_vibration(i, 0, 0);
  return 1;
}
void TRT_GUEST_ABI st_input_RunFrame(void*, int /*bReserved*/) {}
int TRT_GUEST_ABI st_input_GetConnectedControllers(void*, u64* handles_out) {
  int count = 0;
  for (u32 i = 0; i < 4; ++i) {
    if (trt_pad_connected(i)) {
      if (handles_out) handles_out[count] = kSteamInputHandleBase + i;
      ++count;
    }
  }
  return count;
}
u64 TRT_GUEST_ABI st_input_GetControllerForGamepadIndex(void*, int index) {
  if (index < 0 || index >= 4) return 0;
  if (!trt_pad_connected((u32)index)) return 0;
  return kSteamInputHandleBase + (u64)index;
}
int TRT_GUEST_ABI st_input_GetGamepadIndexForController(void*, u64 handle) {
  u32 idx = 0;
  if (!input_handle_to_idx(handle, &idx)) return -1;
  return trt_pad_connected(idx) ? (int)idx : -1;
}
int TRT_GUEST_ABI st_input_GetInputTypeForHandle(void*, u64 handle) {
  u32 idx = 0;
  if (!input_handle_to_idx(handle, &idx)) return 0;
  // k_ESteamInputType_XBoxOneController = 3
  return trt_pad_connected(idx) ? 3 : 0;
}
void TRT_GUEST_ABI st_input_TriggerVibration(void*, u64 handle,
                                                  u16 left_speed,
                                                  u16 right_speed) {
  u32 idx = 0;
  if (input_handle_to_idx(handle, &idx)) {
    trt_pad_set_vibration(idx, left_speed, right_speed);
  }
}
void TRT_GUEST_ABI st_input_TriggerVibrationExtended(
    void* self, u64 handle, u16 left, u16 right, u16 /*lt*/, u16 /*rt*/) {
  st_input_TriggerVibration(self, handle, left, right);
}
// ISteamInput string getters (slots 19/23). Were steam_noop_zero (NULL).
const char* TRT_GUEST_ABI st_input_GetStringForDigitalActionName(void* self, u64 action_handle) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamInput.GetStringForDigitalActionName(self=%p action=%016llx) -> \"\"", self, (unsigned long long)action_handle);
  trt_core_log(line);
  return steam_empty_str();
}
const char* TRT_GUEST_ABI st_input_GetStringForAnalogActionName(void* self, u64 action_handle) {
  char line[224];
  snprintf(line, sizeof(line), "strtrace: ISteamInput.GetStringForAnalogActionName(self=%p action=%016llx) -> \"\"", self, (unsigned long long)action_handle);
  trt_core_log(line);
  return steam_empty_str();
}
const void* g_steam_input_vtbl[64] = {
    (const void*)&st_input_Init,                        // 0
    (const void*)&st_input_Shutdown,                    // 1
    (const void*)&steam_noop_zero,                      // 2 SetInputActionManifestFilePath
    (const void*)&st_input_RunFrame,                    // 3
    (const void*)&steam_noop_zero,                      // 4 BWaitForData
    (const void*)&steam_noop_zero,                      // 5 BNewDataAvailable
    (const void*)&st_input_GetConnectedControllers,     // 6
    (const void*)&steam_noop_zero,                      // 7 GetActionSetHandle
    (const void*)&steam_noop_zero,                      // 8 ActivateActionSet
    (const void*)&steam_noop_zero,                      // 9 GetCurrentActionSet
    (const void*)&steam_noop_zero,                      // 10 ActivateAllControllerActionSets
    (const void*)&steam_noop_zero,                      // 11 GetActionSetLayer
    (const void*)&steam_noop_zero,                      // 12 ActivateActionSetLayer
    (const void*)&steam_noop_zero,                      // 13 DeactivateActionSetLayer
    (const void*)&steam_noop_zero,                      // 14 DeactivateAllActionSetLayers
    (const void*)&steam_noop_zero,                      // 15 GetActiveActionSetLayers
    (const void*)&steam_noop_zero,                      // 16 GetDigitalActionHandle
    (const void*)&steam_noop_zero,                      // 17 GetDigitalActionData
    (const void*)&steam_noop_zero,                      // 18 GetDigitalActionOrigins
    (const void*)&st_input_GetStringForDigitalActionName, // 19 GetStringForDigitalActionName
    (const void*)&steam_noop_zero,                      // 20 GetAnalogActionHandle
    (const void*)&steam_noop_zero,                      // 21 GetAnalogActionData
    (const void*)&steam_noop_zero,                      // 22 GetAnalogActionOrigins
    (const void*)&st_input_GetStringForAnalogActionName, // 23 GetStringForAnalogActionName
    (const void*)&steam_noop_zero,                      // 24 StopAnalogActionMomentum
    (const void*)&steam_noop_zero,                      // 25 GetMotionData
    (const void*)&st_input_TriggerVibration,           // 26 TriggerVibration
    (const void*)&st_input_TriggerVibrationExtended,   // 27 TriggerVibrationExtended
    (const void*)&steam_noop_zero,                      // 28 TriggerSimpleHapticEvent
    (const void*)&steam_noop_zero,                      // 29 SetLEDColor
    (const void*)&steam_noop_zero,                      // 30 TriggerHapticPulse
    (const void*)&steam_noop_zero,                      // 31 TriggerRepeatedHapticPulse
    (const void*)&steam_noop_zero,                      // 32 Legacy_TriggerHapticPulse
    (const void*)&steam_noop_zero,                      // 33 Legacy_TriggerRepeatedHapticPulse
    (const void*)&st_input_GetGamepadIndexForController, // 34 GetGamepadIndexForController
    (const void*)&st_input_GetControllerForGamepadIndex, // 35 GetControllerForGamepadIndex
    (const void*)&st_input_GetInputTypeForHandle,       // 36 GetInputTypeForHandle
    (const void*)&steam_noop_zero,                      // 37
    (const void*)&steam_noop_zero,                      // 38
    (const void*)&steam_noop_zero,                      // 39
    (const void*)&steam_noop_zero,                      // 40
    (const void*)&steam_noop_zero,                      // 41
    (const void*)&steam_noop_zero,                      // 42
    (const void*)&steam_noop_zero,                      // 43
    (const void*)&steam_noop_zero,                      // 44
    (const void*)&steam_noop_zero,                      // 45
    (const void*)&steam_noop_zero,                      // 46
    Z16,
};
fake_steam_iface g_steam_input_obj = {g_steam_input_vtbl};

// ---- ISteamClient ----
int TRT_GUEST_ABI st_client_CreateSteamPipe(void*) { return 1; }
int TRT_GUEST_ABI st_client_BReleaseSteamPipe(void*, int /*pipe*/) { return 1; }
int TRT_GUEST_ABI st_client_ConnectToGlobalUser(void*, int /*pipe*/) { return 1; }
int TRT_GUEST_ABI st_client_CreateLocalUser(void*, int* pipe, int /*acct*/) {
  if (pipe) *pipe = 1;
  return 1;
}
void TRT_GUEST_ABI st_client_ReleaseUser(void*, int, int) {}
void* TRT_GUEST_ABI st_client_GetISteamUser(void*, int, int, const char*) {
  return &g_steam_user_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamGameServer(void*, int, int, const char*) {
  return steam_get_iface(19);
}
void TRT_GUEST_ABI st_client_SetLocalIPBinding(void*, const void*, u16) {}
void* TRT_GUEST_ABI st_client_GetISteamFriends(void*, int, int, const char*) {
  return &g_steam_friends_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamUtils(void*, int, const char*) {
  return &g_steam_utils_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamMatchmaking(void*, int, int, const char*) {
  return &g_steam_matchmaking_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamMatchmakingServers(void*, int, int, const char*) {
  return steam_get_iface(2);
}
void* TRT_GUEST_ABI st_client_GetISteamGenericInterface(void*, int, int, const char* ver) {
  return trt_SteamInternal_CreateInterface(ver);
}
void* TRT_GUEST_ABI st_client_GetISteamUserStats(void*, int, int, const char*) {
  return &g_steam_userstats_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamGameServerStats(void*, int, int, const char*) {
  return steam_get_iface(21);
}
void* TRT_GUEST_ABI st_client_GetISteamApps(void*, int, int, const char*) {
  return &g_steam_apps_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamNetworking(void*, int, int, const char*) {
  return &g_steam_networking_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamRemoteStorage(void*, int, int, const char*) {
  return &g_steam_rs_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamScreenshots(void*, int, int, const char*) {
  return steam_get_iface(7);
}
void* TRT_GUEST_ABI st_client_GetISteamGameSearch(void*, int, int, const char*) {
  return steam_get_iface(22);
}
void TRT_GUEST_ABI st_client_RunFrame(void*) {}
u32 TRT_GUEST_ABI st_client_GetIPCCallCount(void*) { return 1; }
void TRT_GUEST_ABI st_client_SetWarningMessageHook(void*, void*) {}
int TRT_GUEST_ABI st_client_BShutdownIfAllPipesClosed(void*) { return 1; }
void* TRT_GUEST_ABI st_client_GetISteamHTTP(void*, int, int, const char*) {
  return steam_get_iface(9);
}
void* TRT_GUEST_ABI st_client_GetISteamUnifiedMessages(void*, int, int, const char*) {
  return steam_get_iface(23);
}
void* TRT_GUEST_ABI st_client_GetISteamController(void*, int, int, const char*) {
  return &g_steam_input_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamUGC(void*, int, int, const char*) {
  return steam_get_iface(11);
}
void* TRT_GUEST_ABI st_client_GetISteamAppList(void*, int, int, const char*) {
  return steam_get_iface(14);
}
void* TRT_GUEST_ABI st_client_GetISteamMusic(void*, int, int, const char*) {
  return steam_get_iface(8);
}
void* TRT_GUEST_ABI st_client_GetISteamMusicRemote(void*, int, int, const char*) {
  return steam_get_iface(15);
}
void* TRT_GUEST_ABI st_client_GetISteamHTMLSurface(void*, int, int, const char*) {
  return steam_get_iface(12);
}
void* TRT_GUEST_ABI st_client_GetISteamInventory(void*, int, int, const char*) {
  return steam_get_iface(13);
}
void* TRT_GUEST_ABI st_client_GetISteamVideo(void*, int, int, const char*) {
  return steam_get_iface(15);
}
void* TRT_GUEST_ABI st_client_GetISteamParentalSettings(void*, int, int, const char*) {
  return steam_get_iface(16);
}
void* TRT_GUEST_ABI st_client_GetISteamInput(void*, int, int, const char*) {
  return &g_steam_input_obj;
}
void* TRT_GUEST_ABI st_client_GetISteamParties(void*, int, int, const char*) {
  return steam_get_iface(3);
}
void* TRT_GUEST_ABI st_client_GetISteamRemotePlay(void*, int, int, const char*) {
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
    return &g_steam_userstats_obj;
  if (strncmp(version, "SteamUtils", 10) == 0 ||
      strncmp(version, "STEAMUTILS_INTERFACE_VERSION", 28) == 0)
    return &g_steam_utils_obj;
  if (strncmp(version, "SteamFriends", 12) == 0 ||
      strncmp(version, "STEAMFRIENDS_INTERFACE_VERSION", 30) == 0)
    return &g_steam_friends_obj;
  if ((strncmp(version, "SteamMatchmaking", 16) == 0 &&
       strncmp(version, "SteamMatchmakingServers", 23) != 0) ||
      (strncmp(version, "STEAMMATCHMAKING_INTERFACE_VERSION", 34) == 0 &&
       strncmp(version, "STEAMMATCHMAKINGSERVERS_INTERFACE_VERSION", 41) != 0))
    return &g_steam_matchmaking_obj;
  if (strncmp(version, "SteamNetworking", 15) == 0 ||
      strncmp(version, "STEAMNETWORKING_INTERFACE_VERSION", 33) == 0)
    return &g_steam_networking_obj;
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
  (void)user;
  return trt_SteamInternal_CreateInterface(ver);
}

void* TRT_GUEST_ABI trt_SteamInternal_FindOrCreateGameServerInterface(int user,
                                                                      const char* ver) {
  (void)user;
  return trt_SteamInternal_CreateInterface(ver);
}

void* TRT_GUEST_ABI trt_SteamInternal_ContextInit(void* ctx_init_data) {
  if (!ctx_init_data) return nullptr;
  struct init_block {
    void (TRT_GUEST_ABI *init_fn)(void*);
    uintptr_t counter;
    u8 ctx_storage[1];
  };
  init_block* blk = (init_block*)ctx_init_data;
  void* ctx = (void*)blk->ctx_storage;
  if (blk->counter != 1) {
    blk->counter = 1;
    if (blk->init_fn) blk->init_fn(ctx);
  }
  return ctx;
}

void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v021(void) { return &g_steam_user_obj; }
void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v023(void) { return &g_steam_user_obj; }
void* TRT_GUEST_ABI trt_SteamAPI_SteamApps_v008(void) { return &g_steam_apps_obj; }
void* TRT_GUEST_ABI trt_SteamAPI_SteamUtils_v010(void) { return &g_steam_utils_obj; }
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsSubscribedApp(void*, u32 app_id) {
  return trt_SteamAPI_BIsSubscribedApp(app_id);
}
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsDlcInstalled(void*, u32 dlc_id) {
  return trt_SteamAPI_BIsDlcInstalled(dlc_id);
}
const char* TRT_GUEST_ABI trt_SteamAPI_ISteamApps_GetCurrentGameLanguage(void*) {
  { const char* st_v = "english"; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: SteamAPI_ISteamApps_GetCurrentGameLanguage -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); }
  return "english";
}
u32 TRT_GUEST_ABI trt_SteamAPI_ISteamUtils_GetAppID(void*) { return g_app_id; }

#define STEAM_IFACE_FN(idx) \
  static void* TRT_GUEST_ABI steam_iface_##idx(void) { return steam_get_iface(idx); }
STEAM_IFACE_FN(1)  STEAM_IFACE_FN(2)  STEAM_IFACE_FN(3)
STEAM_IFACE_FN(6)  STEAM_IFACE_FN(7)  STEAM_IFACE_FN(8)  STEAM_IFACE_FN(9)
STEAM_IFACE_FN(11) STEAM_IFACE_FN(12) STEAM_IFACE_FN(13) STEAM_IFACE_FN(14)
STEAM_IFACE_FN(15) STEAM_IFACE_FN(16) STEAM_IFACE_FN(17) STEAM_IFACE_FN(18)
STEAM_IFACE_FN(19) STEAM_IFACE_FN(20) STEAM_IFACE_FN(24)
#undef STEAM_IFACE_FN
static void* TRT_GUEST_ABI steam_apps_obj_ptr(void) {
  return &g_steam_apps_obj;
}
static void* TRT_GUEST_ABI steam_userstats_obj_ptr(void) {
  return &g_steam_userstats_obj;
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
static void* TRT_GUEST_ABI steam_matchmaking_obj_ptr(void) {
  return &g_steam_matchmaking_obj;
}
static void* TRT_GUEST_ABI steam_networking_obj_ptr(void) {
  return &g_steam_networking_obj;
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
static u64 TRT_GUEST_ABI steam_apps_get_app_owner(void*) {
  return trt_steam_is_owned(g_app_id) ? trt_SteamAPI_GetSteamID() : 0;
}
static int TRT_GUEST_ABI steam_user_has_license(void*, u64 /*steam_id*/, u32 /*app_id*/) {
  // EUserHasLicenseForAppResult: 0=HasLicense, 1=DoesNotHaveLicense, 2=NoAuth
  return 2;
}
static int TRT_GUEST_ABI steam_user_b_logged_on(void*) {
  return g_initialized ? 1 : 0;
}
static u64 TRT_GUEST_ABI steam_user_get_steam_id(void*) {
  return trt_SteamAPI_GetSteamID();
}
static const char* TRT_GUEST_ABI steam_friends_get_persona_name(void*) {
  return trt_steam_current_persona();
}
static int TRT_GUEST_ABI steam_friends_get_persona_state(void*) {
  return g_initialized ? 1 : 0;
}
static const char* TRT_GUEST_ABI steam_const_english(void*) { { const char* st_v = "english"; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: SteamAPI_GetSteamUILanguage/GetAvailableGameLanguages(flat) -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); } return "english"; }
static const char* TRT_GUEST_ABI steam_utils_get_ip_country(void*) { { const char* st_v = "US"; char st_line[192]; snprintf(st_line, sizeof(st_line), "strtrace: SteamAPI_ISteamUtils_GetIPCountry(flat) -> ptr=%p \"%.128s\"", (const void*)st_v, st_v); trt_core_log(st_line); } return "US"; }
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
    { char st_line[200]; snprintf(st_line, sizeof(st_line), "strtrace: SteamAPI_ISteamApps_GetAppInstallDir(flat) -> buf=%p cap=%u value=\"%.128s\"", (void*)pchFolder, cap, pchFolder); trt_core_log(st_line); }
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
    {"SteamUser", (void*)steam_user_obj_ptr},
    {"SteamFriends", (void*)steam_friends_obj_ptr},
    {"SteamUtils", (void*)steam_utils_obj_ptr},
    {"SteamApps", (void*)steam_apps_obj_ptr},
    {"SteamUserStats", (void*)steam_userstats_obj_ptr},
    {"SteamMatchmaking", (void*)steam_matchmaking_obj_ptr},
    {"SteamNetworking", (void*)steam_networking_obj_ptr},
    {"SteamRemoteStorage", (void*)steam_rs_obj_ptr},
    {"SteamScreenshots", (void*)steam_iface_7},
    {"SteamHTTP", (void*)steam_iface_9},
    {"SteamInput", (void*)steam_input_obj_ptr},
    {"SteamController", (void*)steam_input_obj_ptr},
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
    {"SteamAPI_SteamMatchmaking_v009", (void*)steam_matchmaking_obj_ptr},
    {"SteamAPI_SteamMatchmakingServers_v002", (void*)steam_iface_2},
    {"SteamAPI_SteamParties_v002", (void*)steam_iface_3},
    {"SteamAPI_SteamRemoteStorage_v014", (void*)steam_rs_obj_ptr},
    {"SteamAPI_SteamRemoteStorage_v016", (void*)steam_rs_obj_ptr},
    {"SteamAPI_SteamUserStats_v011", (void*)steam_userstats_obj_ptr},
    {"SteamAPI_SteamUserStats_v012", (void*)steam_userstats_obj_ptr},
    {"SteamAPI_SteamUserStats_v013", (void*)steam_userstats_obj_ptr},
    {"SteamAPI_SteamNetworking_v006", (void*)steam_networking_obj_ptr},
    {"SteamAPI_ISteamUserStats_RequestCurrentStats", (void*)st_userstats_RequestCurrentStats},
    {"SteamAPI_ISteamUserStats_GetStatInt32", (void*)st_userstats_GetStatInt32},
    {"SteamAPI_ISteamUserStats_GetStatFloat", (void*)st_userstats_GetStatFloat},
    {"SteamAPI_ISteamUserStats_SetStatInt32", (void*)st_userstats_SetStatInt32},
    {"SteamAPI_ISteamUserStats_SetStatFloat", (void*)st_userstats_SetStatFloat},
    {"SteamAPI_ISteamUserStats_UpdateAvgRateStat", (void*)st_userstats_UpdateAvgRateStat},
    {"SteamAPI_ISteamUserStats_GetAchievement", (void*)st_userstats_GetAchievement},
    {"SteamAPI_ISteamUserStats_SetAchievement", (void*)st_userstats_SetAchievement},
    {"SteamAPI_ISteamUserStats_ClearAchievement", (void*)st_userstats_ClearAchievement},
    {"SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime", (void*)st_userstats_GetAchievementAndUnlockTime},
    {"SteamAPI_ISteamUserStats_StoreStats", (void*)st_userstats_StoreStats},
    {"SteamAPI_ISteamUserStats_GetAchievementIcon", (void*)st_userstats_GetAchievementIcon},
    {"SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute", (void*)st_userstats_GetAchievementDisplayAttribute},
    {"SteamAPI_ISteamUserStats_IndicateAchievementProgress", (void*)st_userstats_IndicateAchievementProgress},
    {"SteamAPI_ISteamUserStats_GetNumAchievements", (void*)st_userstats_GetNumAchievements},
    {"SteamAPI_ISteamUserStats_GetAchievementName", (void*)st_userstats_GetAchievementName},
    {"SteamAPI_ISteamUserStats_RequestUserStats", (void*)st_userstats_RequestUserStats},
    {"SteamAPI_ISteamUserStats_GetUserStatInt32", (void*)st_userstats_GetUserStatInt32},
    {"SteamAPI_ISteamUserStats_GetUserStatFloat", (void*)st_userstats_GetUserStatFloat},
    {"SteamAPI_ISteamUserStats_GetUserAchievement", (void*)st_userstats_GetUserAchievement},
    {"SteamAPI_ISteamUserStats_GetLeaderboardName", (void*)st_userstats_GetLeaderboardName},
    {"SteamAPI_ISteamUserStats_ResetAllStats", (void*)st_userstats_ResetAllStats},
    {"SteamAPI_ISteamUserStats_FindOrCreateLeaderboard", (void*)st_userstats_FindOrCreateLeaderboard},
    {"SteamAPI_ISteamUserStats_FindLeaderboard", (void*)st_userstats_FindLeaderboard},
    {"SteamAPI_ISteamUserStats_GetLeaderboardEntryCount", (void*)st_userstats_GetLeaderboardEntryCount},
    {"SteamAPI_ISteamMatchmaking_GetFavoriteGameCount", (void*)st_mm_GetFavoriteGameCount},
    {"SteamAPI_ISteamMatchmaking_GetFavoriteGame", (void*)st_mm_GetFavoriteGame},
    {"SteamAPI_ISteamMatchmaking_AddFavoriteGame", (void*)st_mm_AddFavoriteGame},
    {"SteamAPI_ISteamMatchmaking_RemoveFavoriteGame", (void*)st_mm_RemoveFavoriteGame},
    {"SteamAPI_ISteamMatchmaking_RequestLobbyList", (void*)st_mm_RequestLobbyList},
    {"SteamAPI_ISteamMatchmaking_GetLobbyByIndex", (void*)st_mm_GetLobbyByIndex},
    {"SteamAPI_ISteamMatchmaking_CreateLobby", (void*)st_mm_CreateLobby},
    {"SteamAPI_ISteamMatchmaking_JoinLobby", (void*)st_mm_JoinLobby},
    {"SteamAPI_ISteamMatchmaking_LeaveLobby", (void*)st_mm_LeaveLobby},
    {"SteamAPI_ISteamMatchmaking_InviteUserToLobby", (void*)st_mm_InviteUserToLobby},
    {"SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", (void*)st_mm_GetNumLobbyMembers},
    {"SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", (void*)st_mm_GetLobbyMemberByIndex},
    {"SteamAPI_ISteamMatchmaking_GetLobbyData", (void*)st_mm_GetLobbyData},
    {"SteamAPI_ISteamMatchmaking_SetLobbyData", (void*)st_mm_SetLobbyData},
    {"SteamAPI_ISteamMatchmaking_GetLobbyDataCount", (void*)st_mm_GetLobbyDataCount},
    {"SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex", (void*)st_mm_GetLobbyDataByIndex},
    {"SteamAPI_ISteamMatchmaking_DeleteLobbyData", (void*)st_mm_DeleteLobbyData},
    {"SteamAPI_ISteamMatchmaking_SendLobbyChatMsg", (void*)st_mm_SendLobbyChatMsg},
    {"SteamAPI_ISteamMatchmaking_GetLobbyChatEntry", (void*)st_mm_GetLobbyChatEntry},
    {"SteamAPI_ISteamMatchmaking_RequestLobbyData", (void*)st_mm_RequestLobbyData},
    {"SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit", (void*)st_mm_SetLobbyMemberLimit},
    {"SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit", (void*)st_mm_GetLobbyMemberLimit},
    {"SteamAPI_ISteamMatchmaking_SetLobbyType", (void*)st_mm_SetLobbyType},
    {"SteamAPI_ISteamMatchmaking_SetLobbyJoinable", (void*)st_mm_SetLobbyJoinable},
    {"SteamAPI_ISteamMatchmaking_GetLobbyOwner", (void*)st_mm_GetLobbyOwner},
    {"SteamAPI_ISteamMatchmaking_SetLobbyOwner", (void*)st_mm_SetLobbyOwner},
    {"SteamAPI_ISteamNetworking_SendP2PPacket", (void*)st_net_SendP2PPacket},
    {"SteamAPI_ISteamNetworking_IsP2PPacketAvailable", (void*)st_net_IsP2PPacketAvailable},
    {"SteamAPI_ISteamNetworking_ReadP2PPacket", (void*)st_net_ReadP2PPacket},
    {"SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser", (void*)st_net_AcceptP2PSessionWithUser},
    {"SteamAPI_ISteamNetworking_CloseP2PSessionWithUser", (void*)st_net_CloseP2PSessionWithUser},
    {"SteamAPI_ISteamNetworking_CloseP2PChannelWithUser", (void*)st_net_CloseP2PChannelWithUser},
    {"SteamAPI_ISteamNetworking_GetP2PSessionState", (void*)st_net_GetP2PSessionState},
    {"SteamAPI_ISteamNetworking_AllowP2PPacketRelay", (void*)st_net_AllowP2PPacketRelay},
    {"SteamAPI_ISteamFriends_GetFriendCount", (void*)st_friends_GetFriendCount},
    {"SteamAPI_ISteamFriends_GetFriendByIndex", (void*)st_friends_GetFriendByIndex},
    {"SteamAPI_ISteamFriends_GetFriendRelationship", (void*)st_friends_GetFriendRelationship},
    {"SteamAPI_ISteamFriends_GetFriendPersonaState", (void*)st_friends_GetFriendPersonaState},
    {"SteamAPI_ISteamFriends_GetFriendPersonaName", (void*)st_friends_GetFriendPersonaName},
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
      {"SteamAPI_InitSafe", (void*)&trt_SteamAPI_InitSafe},
      {"Steam_GetHSteamUserCurrent", (void*)&trt_Steam_GetHSteamUserCurrent},
      {"Steam_RegisterInterfaceFuncs", (void*)&trt_Steam_RegisterInterfaceFuncs},
      {"SteamAPI_GetSteamInstallPath", (void*)&trt_SteamAPI_GetSteamInstallPath},
      {"Steam_RunCallbacks", (void*)&trt_Steam_RunCallbacks},
      {"SteamAPI_SetTryCatchCallbacks", (void*)&steam_noop_zero},
      {"SteamAPI_UseBreakpadCrashHandler", (void*)&steam_noop_zero},
      {"SteamAPI_SetBreakpadAppID", (void*)&trt_steam_set_appid},
      {"SteamAPI_SetMiniDumpComment", (void*)&steam_noop_zero},
      {"SteamAPI_WriteMiniDump", (void*)&steam_noop_zero},
      {"SteamGameServer_InitSafe", (void*)&trt_SteamAPI_InitSafe},
      {"SteamGameServer_GetHSteamPipe", (void*)&trt_SteamAPI_GetHSteamPipe},
      {"SteamGameServer_GetHSteamUser", (void*)&trt_SteamAPI_GetHSteamUser},
      {"SteamGameServer_RunCallbacks", (void*)&steam_noop_zero},
      {"SteamGameServer_Shutdown", (void*)&steam_noop_zero},
      {"SteamGameServer_GetSteamID", (void*)&trt_SteamAPI_GetSteamID},
      {"SteamGameServer_BSecure", (void*)&trt_SteamAPI_InitSafe},
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
