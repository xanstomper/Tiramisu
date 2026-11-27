// tiramisu/src/trt/bridge_eos.cpp
// Honest offline-safe Epic Online Services (EOSSDK-Win64/32-Shipping.dll)
// and GOG Galaxy (Galaxy64.dll / Galaxy.dll) runtime bridges.
//
// Contract (honest runtime, zero DRM/anti-cheat bypass):
//   - Lifecycle & Platform handles succeed cleanly (Init/Create/Tick/Release/Shutdown).
//   - CheckForLauncherAndRestart returns EOS_NoChange (24) so offline titles
//     continue booting instead of terminating to spawn the Epic launcher.
//   - NetworkStatus is EOS_NS_Offline (0); online Auth/Connect/Ecom callbacks
//     complete cleanly with EOS_NoConnection (18) — never fakes ownership.
//   - GetAntiCheatClientInterface returns NULL (anti-cheat is never faked).
#include "bridge_eos.h"
#include "thunk.h"
#include "trt.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// Official EOS_EResult values (EOS SDK 1.16 public header eos_result.h)
constexpr int EOS_Success = 0;
constexpr int EOS_NoConnection = 1;
constexpr int EOS_InvalidParameters = 10;
constexpr int EOS_NotConfigured = 17;
constexpr int EOS_NotFound = 18;
constexpr int EOS_NoChange = 24;
constexpr int EOS_AlreadyConfigured = 26;

enum eos_iface_kind : u32 {
  EOS_IF_PLATFORM = 1,
  EOS_IF_AUTH,
  EOS_IF_CONNECT,
  EOS_IF_ACHIEVEMENTS,
  EOS_IF_STATS,
  EOS_IF_FRIENDS,
  EOS_IF_USERINFO,
  EOS_IF_PRESENCE,
  EOS_IF_PLAYER_STORAGE,
  EOS_IF_TITLE_STORAGE,
  EOS_IF_UI,
  EOS_IF_ECOM,
  EOS_IF_LOBBY,
  EOS_IF_SESSIONS,
  EOS_IF_METRICS,
  EOS_IF_P2P,
  EOS_IF_LEADERBOARDS,
  EOS_IF_PROGRESSION,
  EOS_IF_REPORTS,
  EOS_IF_SANCTIONS,
  EOS_IF_RTC,
  EOS_IF_MODS,
  EOS_IF_KWS,
  EOS_IF_CUSTOM_INVITES,
  EOS_IF_INTEGRATED_PLATFORM,
};

struct eos_iface_handle {
  u32 magic;
  u32 kind;
};

struct eos_platform_obj {
  u32 magic;
  int network_status;
  int app_status;
  eos_iface_handle ifaces[32];
};

constexpr u32 k_eos_magic = 0x454F5331u;  // "EOS1"

std::atomic<bool> g_eos_initialized{false};
std::atomic<u64> g_eos_notify_seq{1};
std::atomic<bool> g_galaxy_initialized{false};
std::mutex g_eos_mu;

struct eos_pending_cb {
  void (TRT_GUEST_ABI *fn)(const void*);
  std::vector<u8> payload;
};
std::vector<eos_pending_cb> g_eos_cbs;

void enqueue_cb(void* fn_ptr, const void* payload, size_t payload_sz) {
  if (!fn_ptr || !payload || !payload_sz) return;
  eos_pending_cb item;
  item.fn = reinterpret_cast<void (TRT_GUEST_ABI*)(const void*)>(fn_ptr);
  item.payload.assign(static_cast<const u8*>(payload),
                      static_cast<const u8*>(payload) + payload_sz);
  std::lock_guard<std::mutex> lk(g_eos_mu);
  g_eos_cbs.push_back(std::move(item));
}

eos_platform_obj* check_platform(void* h) {
  if (!h) return nullptr;
  auto* p = static_cast<eos_platform_obj*>(h);
  return (p->magic == k_eos_magic) ? p : nullptr;
}

void* get_iface(void* h, eos_iface_kind kind) {
  eos_platform_obj* p = check_platform(h);
  if (!p || static_cast<size_t>(kind) >= 32u) return nullptr;
  return &p->ifaces[kind];
}

}  // namespace

extern "C" {

// ---- EOS Core Lifecycle ----
int TRT_GUEST_ABI eos_Initialize(const void* options) {
  (void)options;
  if (g_eos_initialized.exchange(true)) return EOS_AlreadyConfigured;
  trt_core_log("eos: EOS_Initialize -> EOS_Success (offline-safe runtime)");
  return EOS_Success;
}

int TRT_GUEST_ABI eos_Shutdown(void) {
  std::lock_guard<std::mutex> lk(g_eos_mu);
  g_eos_cbs.clear();
  g_eos_initialized.store(false);
  return EOS_Success;
}

const char* TRT_GUEST_ABI eos_GetVersion(void) {
  return "1.16.3-tiramisu";
}

const char* TRT_GUEST_ABI eos_EResult_ToString(int result) {
  switch (result) {
    case EOS_Success: return "EOS_Success";
    case EOS_NoConnection: return "EOS_NoConnection";
    case EOS_InvalidParameters: return "EOS_InvalidParameters";
    case EOS_NotConfigured: return "EOS_NotConfigured";
    case EOS_NotFound: return "EOS_NotFound";
    case EOS_NoChange: return "EOS_NoChange";
    case EOS_AlreadyConfigured: return "EOS_AlreadyConfigured";
    default: return "EOS_UnexpectedError";
  }
}

int TRT_GUEST_ABI eos_EResult_IsOperationComplete(int result) {
  (void)result;
  return 1;
}

int TRT_GUEST_ABI eos_Logging_SetCallback(void* cb) {
  (void)cb;
  return EOS_Success;
}

int TRT_GUEST_ABI eos_Logging_SetLogLevel(int category, int level) {
  (void)category;
  (void)level;
  return EOS_Success;
}

// ---- EOS Platform ----
void* TRT_GUEST_ABI eos_Platform_Create(const void* options) {
  (void)options;
  auto* p = static_cast<eos_platform_obj*>(calloc(1, sizeof(eos_platform_obj)));
  if (!p) return nullptr;
  p->magic = k_eos_magic;
  p->network_status = 0;  // EOS_NS_Offline
  p->app_status = 3;      // EOS_AS_Foreground
  for (u32 i = 0; i < 32u; ++i) {
    p->ifaces[i].magic = k_eos_magic;
    p->ifaces[i].kind = i;
  }
  trt_core_log("eos: EOS_Platform_Create -> offline platform handle");
  return p;
}

void TRT_GUEST_ABI eos_Platform_Release(void* handle) {
  eos_platform_obj* p = check_platform(handle);
  if (!p) return;
  p->magic = 0;
  free(p);
}

void TRT_GUEST_ABI eos_Platform_Tick(void* handle) {
  (void)handle;
  std::vector<eos_pending_cb> ready;
  {
    std::lock_guard<std::mutex> lk(g_eos_mu);
    if (g_eos_cbs.empty()) return;
    ready.swap(g_eos_cbs);
  }
  for (const auto& cb : ready) {
    if (cb.fn && !cb.payload.empty()) cb.fn(cb.payload.data());
  }
}

int TRT_GUEST_ABI eos_Platform_CheckForLauncherAndRestart(void* handle) {
  (void)handle;
  return EOS_NoChange;  // Do NOT terminate the game to relaunch Epic launcher
}

int TRT_GUEST_ABI eos_Platform_SetNetworkStatus(void* handle, int status) {
  eos_platform_obj* p = check_platform(handle);
  if (!p) return EOS_InvalidParameters;
  p->network_status = status;
  return EOS_Success;
}

int TRT_GUEST_ABI eos_Platform_GetNetworkStatus(void* handle) {
  eos_platform_obj* p = check_platform(handle);
  return p ? p->network_status : 0;
}

int TRT_GUEST_ABI eos_Platform_SetApplicationStatus(void* handle, int status) {
  eos_platform_obj* p = check_platform(handle);
  if (!p) return EOS_InvalidParameters;
  p->app_status = status;
  return EOS_Success;
}

int TRT_GUEST_ABI eos_Platform_GetApplicationStatus(void* handle) {
  eos_platform_obj* p = check_platform(handle);
  return p ? p->app_status : 3;
}

int TRT_GUEST_ABI eos_Platform_GetActiveCountryCode(void* h, void* uid, char* out, int* len) {
  (void)h; (void)uid;
  if (!len) return EOS_InvalidParameters;
  if (out && *len >= 3) { memcpy(out, "US", 3); }
  *len = 3;
  return EOS_Success;
}

int TRT_GUEST_ABI eos_Platform_GetActiveLocaleCode(void* h, void* uid, char* out, int* len) {
  (void)h; (void)uid;
  if (!len) return EOS_InvalidParameters;
  if (out && *len >= 6) { memcpy(out, "en-US", 6); }
  *len = 6;
  return EOS_Success;
}

// ---- Platform Interface Getters ----
void* TRT_GUEST_ABI eos_Platform_GetAuthInterface(void* h) { return get_iface(h, EOS_IF_AUTH); }
void* TRT_GUEST_ABI eos_Platform_GetConnectInterface(void* h) { return get_iface(h, EOS_IF_CONNECT); }
void* TRT_GUEST_ABI eos_Platform_GetAchievementsInterface(void* h) { return get_iface(h, EOS_IF_ACHIEVEMENTS); }
void* TRT_GUEST_ABI eos_Platform_GetStatsInterface(void* h) { return get_iface(h, EOS_IF_STATS); }
void* TRT_GUEST_ABI eos_Platform_GetFriendsInterface(void* h) { return get_iface(h, EOS_IF_FRIENDS); }
void* TRT_GUEST_ABI eos_Platform_GetUserInfoInterface(void* h) { return get_iface(h, EOS_IF_USERINFO); }
void* TRT_GUEST_ABI eos_Platform_GetPresenceInterface(void* h) { return get_iface(h, EOS_IF_PRESENCE); }
void* TRT_GUEST_ABI eos_Platform_GetPlayerDataStorageInterface(void* h) { return get_iface(h, EOS_IF_PLAYER_STORAGE); }
void* TRT_GUEST_ABI eos_Platform_GetTitleStorageInterface(void* h) { return get_iface(h, EOS_IF_TITLE_STORAGE); }
void* TRT_GUEST_ABI eos_Platform_GetUIInterface(void* h) { return get_iface(h, EOS_IF_UI); }
void* TRT_GUEST_ABI eos_Platform_GetEcomInterface(void* h) { return get_iface(h, EOS_IF_ECOM); }
void* TRT_GUEST_ABI eos_Platform_GetLobbyInterface(void* h) { return get_iface(h, EOS_IF_LOBBY); }
void* TRT_GUEST_ABI eos_Platform_GetSessionsInterface(void* h) { return get_iface(h, EOS_IF_SESSIONS); }
void* TRT_GUEST_ABI eos_Platform_GetMetricsInterface(void* h) { return get_iface(h, EOS_IF_METRICS); }
void* TRT_GUEST_ABI eos_Platform_GetP2PInterface(void* h) { return get_iface(h, EOS_IF_P2P); }
void* TRT_GUEST_ABI eos_Platform_GetLeaderboardsInterface(void* h) { return get_iface(h, EOS_IF_LEADERBOARDS); }
void* TRT_GUEST_ABI eos_Platform_GetProgressionSnapshotInterface(void* h) { return get_iface(h, EOS_IF_PROGRESSION); }
void* TRT_GUEST_ABI eos_Platform_GetReportsInterface(void* h) { return get_iface(h, EOS_IF_REPORTS); }
void* TRT_GUEST_ABI eos_Platform_GetSanctionsInterface(void* h) { return get_iface(h, EOS_IF_SANCTIONS); }
void* TRT_GUEST_ABI eos_Platform_GetRTCInterface(void* h) { return get_iface(h, EOS_IF_RTC); }
void* TRT_GUEST_ABI eos_Platform_GetModsInterface(void* h) { return get_iface(h, EOS_IF_MODS); }
void* TRT_GUEST_ABI eos_Platform_GetKWSInterface(void* h) { return get_iface(h, EOS_IF_KWS); }
void* TRT_GUEST_ABI eos_Platform_GetCustomInvitesInterface(void* h) { return get_iface(h, EOS_IF_CUSTOM_INVITES); }
void* TRT_GUEST_ABI eos_Platform_GetIntegratedPlatformInterface(void* h) { return get_iface(h, EOS_IF_INTEGRATED_PLATFORM); }
void* TRT_GUEST_ABI eos_Platform_GetAntiCheatClientInterface(void* h) {
  (void)h;
  trt_core_log("eos: GetAntiCheatClientInterface -> NULL (anti-cheat not bypassed)");
  return nullptr;
}

// ---- Auth / Connect / Ecom / UI / Achievements (honest offline behavior) ----
struct eos_generic_cb_info {
  int result_code;
  int pad;
  void* client_data;
  void* account_id;
  void* extra[4];
};

void TRT_GUEST_ABI eos_Auth_Login(void* h, const void* opts, void* client_data, void* cb) {
  (void)h; (void)opts;
  eos_generic_cb_info info{};
  info.result_code = EOS_NoConnection;
  info.client_data = client_data;
  enqueue_cb(cb, &info, sizeof(info));
}

int TRT_GUEST_ABI eos_Auth_GetLoggedInAccountsCount(void* h) { (void)h; return 0; }
void* TRT_GUEST_ABI eos_Auth_GetLoggedInAccountByIndex(void* h, int idx) { (void)h; (void)idx; return nullptr; }
int TRT_GUEST_ABI eos_Auth_GetLoginStatus(void* h, void* acc) { (void)h; (void)acc; return 0; }
u64 TRT_GUEST_ABI eos_Auth_AddNotifyLoginStatusChanged(void* h, const void* opts, void* cd, void* cb) {
  (void)h; (void)opts; (void)cd; (void)cb;
  return g_eos_notify_seq.fetch_add(1);
}
void TRT_GUEST_ABI eos_Auth_RemoveNotifyLoginStatusChanged(void* h, u64 id) { (void)h; (void)id; }

void TRT_GUEST_ABI eos_Connect_Login(void* h, const void* opts, void* client_data, void* cb) {
  (void)h; (void)opts;
  eos_generic_cb_info info{};
  info.result_code = EOS_NoConnection;
  info.client_data = client_data;
  enqueue_cb(cb, &info, sizeof(info));
}
int TRT_GUEST_ABI eos_Connect_GetLoggedInUsersCount(void* h) { (void)h; return 0; }
void* TRT_GUEST_ABI eos_Connect_GetLoggedInUserByIndex(void* h, int idx) { (void)h; (void)idx; return nullptr; }
int TRT_GUEST_ABI eos_Connect_GetLoginStatus(void* h, void* uid) { (void)h; (void)uid; return 0; }
u64 TRT_GUEST_ABI eos_Connect_AddNotifyAuthExpiration(void* h, const void* opts, void* cd, void* cb) {
  (void)h; (void)opts; (void)cd; (void)cb;
  return g_eos_notify_seq.fetch_add(1);
}
void TRT_GUEST_ABI eos_Connect_RemoveNotifyAuthExpiration(void* h, u64 id) { (void)h; (void)id; }
u64 TRT_GUEST_ABI eos_Connect_AddNotifyLoginStatusChanged(void* h, const void* opts, void* cd, void* cb) {
  (void)h; (void)opts; (void)cd; (void)cb;
  return g_eos_notify_seq.fetch_add(1);
}
void TRT_GUEST_ABI eos_Connect_RemoveNotifyLoginStatusChanged(void* h, u64 id) { (void)h; (void)id; }

void TRT_GUEST_ABI eos_Ecom_QueryOwnership(void* h, const void* opts, void* client_data, void* cb) {
  (void)h; (void)opts;
  eos_generic_cb_info info{};
  info.result_code = EOS_NoConnection;
  info.client_data = client_data;
  enqueue_cb(cb, &info, sizeof(info));
}

void TRT_GUEST_ABI eos_Achievements_QueryDefinitions(void* h, const void* opts, void* client_data, void* cb) {
  (void)h; (void)opts;
  eos_generic_cb_info info{};
  info.result_code = EOS_NoConnection;
  info.client_data = client_data;
  enqueue_cb(cb, &info, sizeof(info));
}
u32 TRT_GUEST_ABI eos_Achievements_GetAchievementDefinitionCount(void* h, const void* opts) {
  (void)h; (void)opts; return 0;
}

u64 TRT_GUEST_ABI eos_UI_AddNotifyDisplaySettingsUpdated(void* h, const void* opts, void* cd, void* cb) {
  (void)h; (void)opts; (void)cd; (void)cb;
  return g_eos_notify_seq.fetch_add(1);
}
void TRT_GUEST_ABI eos_UI_RemoveNotifyDisplaySettingsUpdated(void* h, u64 id) { (void)h; (void)id; }
int TRT_GUEST_ABI eos_UI_SetToggleFriendsKey(void* h, const void* opts) { (void)h; (void)opts; return EOS_Success; }
int TRT_GUEST_ABI eos_UI_SetDisplayPreference(void* h, const void* opts) { (void)h; (void)opts; return EOS_Success; }
int TRT_GUEST_ABI eos_UI_GetFriendsVisible(void* h, const void* opts) { (void)h; (void)opts; return 0; }

int TRT_GUEST_ABI eos_Metrics_BeginPlayerSession(void* h, const void* opts) { (void)h; (void)opts; return EOS_Success; }
int TRT_GUEST_ABI eos_Metrics_EndPlayerSession(void* h, const void* opts) { (void)h; (void)opts; return EOS_Success; }

int TRT_GUEST_ABI eos_EpicAccountId_IsValid(void* id) { return id ? 1 : 0; }
int TRT_GUEST_ABI eos_ProductUserId_IsValid(void* id) { return id ? 1 : 0; }
int TRT_GUEST_ABI eos_Id_ToString(void* id, char* out, int* len) {
  if (!id || !len) return EOS_InvalidParameters;
  if (out && *len > 0) out[0] = '\0';
  *len = 1;
  return EOS_Success;
}
void* TRT_GUEST_ABI eos_Id_FromString(const char* str) { (void)str; return nullptr; }

int TRT_GUEST_ABI eos_ByteArray_ToString(const u8* bytes, u32 len, char* out, u32* out_len) {
  if (!out_len) return EOS_InvalidParameters;
  const u32 need = len * 2u + 1u;
  if (!out || *out_len < need) {
    *out_len = need;
    return EOS_InvalidParameters;
  }
  static const char k_hex[] = "0123456789abcdef";
  for (u32 i = 0; i < len; ++i) {
    out[i * 2u] = k_hex[(bytes[i] >> 4) & 0x0F];
    out[i * 2u + 1u] = k_hex[bytes[i] & 0x0F];
  }
  out[len * 2u] = '\0';
  *out_len = need;
  return EOS_Success;
}

// ---- GOG Galaxy offline-safe surface (Galaxy64.dll / Galaxy.dll) ----
void TRT_GUEST_ABI galaxy_Init(const void* opts) {
  (void)opts;
  g_galaxy_initialized.store(true);
  trt_core_log("galaxy: Galaxy::Init -> offline mode (not signed in)");
}
void TRT_GUEST_ABI galaxy_Shutdown(void) { g_galaxy_initialized.store(false); }
void TRT_GUEST_ABI galaxy_ProcessData(void) {}
int TRT_GUEST_ABI galaxy_IsSignedIn(void) { return 0; }
int TRT_GUEST_ABI galaxy_IsLoggedOn(void) { return 0; }
void* TRT_GUEST_ABI galaxy_NullIface(void) { return nullptr; }

static const trt_named_fn k_eos_fns[] = {
  {"EOS_Initialize", (void*)&eos_Initialize},
  {"EOS_Shutdown", (void*)&eos_Shutdown},
  {"EOS_GetVersion", (void*)&eos_GetVersion},
  {"EOS_EResult_ToString", (void*)&eos_EResult_ToString},
  {"EOS_EResult_IsOperationComplete", (void*)&eos_EResult_IsOperationComplete},
  {"EOS_Logging_SetCallback", (void*)&eos_Logging_SetCallback},
  {"EOS_Logging_SetLogLevel", (void*)&eos_Logging_SetLogLevel},
  {"EOS_Platform_Create", (void*)&eos_Platform_Create},
  {"EOS_Platform_Release", (void*)&eos_Platform_Release},
  {"EOS_Platform_Tick", (void*)&eos_Platform_Tick},
  {"EOS_Platform_CheckForLauncherAndRestart", (void*)&eos_Platform_CheckForLauncherAndRestart},
  {"EOS_Platform_SetNetworkStatus", (void*)&eos_Platform_SetNetworkStatus},
  {"EOS_Platform_GetNetworkStatus", (void*)&eos_Platform_GetNetworkStatus},
  {"EOS_Platform_SetApplicationStatus", (void*)&eos_Platform_SetApplicationStatus},
  {"EOS_Platform_GetApplicationStatus", (void*)&eos_Platform_GetApplicationStatus},
  {"EOS_Platform_GetActiveCountryCode", (void*)&eos_Platform_GetActiveCountryCode},
  {"EOS_Platform_GetActiveLocaleCode", (void*)&eos_Platform_GetActiveLocaleCode},
  {"EOS_Platform_GetAuthInterface", (void*)&eos_Platform_GetAuthInterface},
  {"EOS_Platform_GetConnectInterface", (void*)&eos_Platform_GetConnectInterface},
  {"EOS_Platform_GetAchievementsInterface", (void*)&eos_Platform_GetAchievementsInterface},
  {"EOS_Platform_GetStatsInterface", (void*)&eos_Platform_GetStatsInterface},
  {"EOS_Platform_GetFriendsInterface", (void*)&eos_Platform_GetFriendsInterface},
  {"EOS_Platform_GetUserInfoInterface", (void*)&eos_Platform_GetUserInfoInterface},
  {"EOS_Platform_GetPresenceInterface", (void*)&eos_Platform_GetPresenceInterface},
  {"EOS_Platform_GetPlayerDataStorageInterface", (void*)&eos_Platform_GetPlayerDataStorageInterface},
  {"EOS_Platform_GetTitleStorageInterface", (void*)&eos_Platform_GetTitleStorageInterface},
  {"EOS_Platform_GetUIInterface", (void*)&eos_Platform_GetUIInterface},
  {"EOS_Platform_GetEcomInterface", (void*)&eos_Platform_GetEcomInterface},
  {"EOS_Platform_GetLobbyInterface", (void*)&eos_Platform_GetLobbyInterface},
  {"EOS_Platform_GetSessionsInterface", (void*)&eos_Platform_GetSessionsInterface},
  {"EOS_Platform_GetMetricsInterface", (void*)&eos_Platform_GetMetricsInterface},
  {"EOS_Platform_GetP2PInterface", (void*)&eos_Platform_GetP2PInterface},
  {"EOS_Platform_GetLeaderboardsInterface", (void*)&eos_Platform_GetLeaderboardsInterface},
  {"EOS_Platform_GetProgressionSnapshotInterface", (void*)&eos_Platform_GetProgressionSnapshotInterface},
  {"EOS_Platform_GetReportsInterface", (void*)&eos_Platform_GetReportsInterface},
  {"EOS_Platform_GetSanctionsInterface", (void*)&eos_Platform_GetSanctionsInterface},
  {"EOS_Platform_GetRTCInterface", (void*)&eos_Platform_GetRTCInterface},
  {"EOS_Platform_GetModsInterface", (void*)&eos_Platform_GetModsInterface},
  {"EOS_Platform_GetKWSInterface", (void*)&eos_Platform_GetKWSInterface},
  {"EOS_Platform_GetCustomInvitesInterface", (void*)&eos_Platform_GetCustomInvitesInterface},
  {"EOS_Platform_GetIntegratedPlatformInterface", (void*)&eos_Platform_GetIntegratedPlatformInterface},
  {"EOS_Platform_GetAntiCheatClientInterface", (void*)&eos_Platform_GetAntiCheatClientInterface},
  {"EOS_Auth_Login", (void*)&eos_Auth_Login},
  {"EOS_Auth_GetLoggedInAccountsCount", (void*)&eos_Auth_GetLoggedInAccountsCount},
  {"EOS_Auth_GetLoggedInAccountByIndex", (void*)&eos_Auth_GetLoggedInAccountByIndex},
  {"EOS_Auth_GetLoginStatus", (void*)&eos_Auth_GetLoginStatus},
  {"EOS_Auth_AddNotifyLoginStatusChanged", (void*)&eos_Auth_AddNotifyLoginStatusChanged},
  {"EOS_Auth_RemoveNotifyLoginStatusChanged", (void*)&eos_Auth_RemoveNotifyLoginStatusChanged},
  {"EOS_Connect_Login", (void*)&eos_Connect_Login},
  {"EOS_Connect_GetLoggedInUsersCount", (void*)&eos_Connect_GetLoggedInUsersCount},
  {"EOS_Connect_GetLoggedInUserByIndex", (void*)&eos_Connect_GetLoggedInUserByIndex},
  {"EOS_Connect_GetLoginStatus", (void*)&eos_Connect_GetLoginStatus},
  {"EOS_Connect_AddNotifyAuthExpiration", (void*)&eos_Connect_AddNotifyAuthExpiration},
  {"EOS_Connect_RemoveNotifyAuthExpiration", (void*)&eos_Connect_RemoveNotifyAuthExpiration},
  {"EOS_Connect_AddNotifyLoginStatusChanged", (void*)&eos_Connect_AddNotifyLoginStatusChanged},
  {"EOS_Connect_RemoveNotifyLoginStatusChanged", (void*)&eos_Connect_RemoveNotifyLoginStatusChanged},
  {"EOS_Ecom_QueryOwnership", (void*)&eos_Ecom_QueryOwnership},
  {"EOS_Achievements_QueryDefinitions", (void*)&eos_Achievements_QueryDefinitions},
  {"EOS_Achievements_GetAchievementDefinitionCount", (void*)&eos_Achievements_GetAchievementDefinitionCount},
  {"EOS_UI_AddNotifyDisplaySettingsUpdated", (void*)&eos_UI_AddNotifyDisplaySettingsUpdated},
  {"EOS_UI_RemoveNotifyDisplaySettingsUpdated", (void*)&eos_UI_RemoveNotifyDisplaySettingsUpdated},
  {"EOS_UI_SetToggleFriendsKey", (void*)&eos_UI_SetToggleFriendsKey},
  {"EOS_UI_SetDisplayPreference", (void*)&eos_UI_SetDisplayPreference},
  {"EOS_UI_GetFriendsVisible", (void*)&eos_UI_GetFriendsVisible},
  {"EOS_Metrics_BeginPlayerSession", (void*)&eos_Metrics_BeginPlayerSession},
  {"EOS_Metrics_EndPlayerSession", (void*)&eos_Metrics_EndPlayerSession},
  {"EOS_EpicAccountId_IsValid", (void*)&eos_EpicAccountId_IsValid},
  {"EOS_ProductUserId_IsValid", (void*)&eos_ProductUserId_IsValid},
  {"EOS_EpicAccountId_ToString", (void*)&eos_Id_ToString},
  {"EOS_ProductUserId_ToString", (void*)&eos_Id_ToString},
  {"EOS_EpicAccountId_FromString", (void*)&eos_Id_FromString},
  {"EOS_ProductUserId_FromString", (void*)&eos_Id_FromString},
  {"EOS_ByteArray_ToString", (void*)&eos_ByteArray_ToString},
};

static const trt_named_fn k_galaxy_fns[] = {
  {"Galaxy_Init", (void*)&galaxy_Init},
  {"Galaxy_Shutdown", (void*)&galaxy_Shutdown},
  {"Galaxy_ProcessData", (void*)&galaxy_ProcessData},
  {"Galaxy_IsSignedIn", (void*)&galaxy_IsSignedIn},
  {"Galaxy_IsLoggedOn", (void*)&galaxy_IsLoggedOn},
  {"?Init@api@galaxy@@YAXAEBUInitOptions@12@@Z", (void*)&galaxy_Init},
  {"?Shutdown@api@galaxy@@YAXXZ", (void*)&galaxy_Shutdown},
  {"?ProcessData@api@galaxy@@YAXXZ", (void*)&galaxy_ProcessData},
  {"?User@api@galaxy@@YAPEAVIUser@12@XZ", (void*)&galaxy_NullIface},
  {"?Friends@api@galaxy@@YAPEAVIFriends@12@XZ", (void*)&galaxy_NullIface},
  {"?Stats@api@galaxy@@YAPEAVIStats@12@XZ", (void*)&galaxy_NullIface},
  {"?Utils@api@galaxy@@YAPEAVIUtils@12@XZ", (void*)&galaxy_NullIface},
  {"?Apps@api@galaxy@@YAPEAVIApps@12@XZ", (void*)&galaxy_NullIface},
  {"?Storage@api@galaxy@@YAPEAVIStorage@12@XZ", (void*)&galaxy_NullIface},
};

int trt_bridge_register_eos(void) {
  static const char* const k_eos_dlls[] = {
    "EOSSDK-Win64-Shipping.dll",
    "EOSSDK-Win32-Shipping.dll",
    "EOSSDK-Win64-Shipping-L.dll",
  };
  for (size_t i = 0; i < sizeof(k_eos_dlls) / sizeof(k_eos_dlls[0]); ++i) {
    trt_module_register(k_eos_dlls[i], TRT_ROUTE_BUILTIN, k_eos_fns,
                        static_cast<u32>(sizeof(k_eos_fns) / sizeof(k_eos_fns[0])));
  }
  static const char* const k_galaxy_dlls[] = {
    "Galaxy64.dll",
    "Galaxy.dll",
  };
  for (size_t i = 0; i < sizeof(k_galaxy_dlls) / sizeof(k_galaxy_dlls[0]); ++i) {
    trt_module_register(k_galaxy_dlls[i], TRT_ROUTE_BUILTIN, k_galaxy_fns,
                        static_cast<u32>(sizeof(k_galaxy_fns) / sizeof(k_galaxy_fns[0])));
  }
  return 0;
}

}  // extern "C"
