// tiramisu/src/host/steam_client.h
// Native Xbox UWP Steam Client:
//   * Steam QR account sign-in (BeginAuthSessionViaQR -> PollAuthSessionStatus)
//   * Persistent credentials & trt_steam_identity sync
//   * Steam WebAPI account profile, owned games (GetOwnedGames), and friends
//   * Native CM WebSocket + Protobuf + BCrypt AES-256/SHA-1 + CDN depot downloader
#ifndef TIRAMISU_HOST_STEAM_CLIENT_H
#define TIRAMISU_HOST_STEAM_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace longan {

inline constexpr char kSteamInstallPending[] = "\\.tiramisu\\steam-install-pending";
inline constexpr char kSteamLaunchExe[] = "\\.tiramisu\\steam-launch-exe.txt";

struct steam_library_item {
  uint32_t app_id = 0;
  std::string title;
  uint64_t size_bytes = 0;
  std::string size_str;
  bool installed = false;
  bool downloading = false;
  bool queued = false;
  float download_pct = 0.0f;
  std::string status_text;
  std::string install_dir;
  std::string main_exe;
};

struct steam_account_snapshot {
  bool signed_in = false;
  uint64_t steamid64 = 0;
  std::string account_name;
  std::string persona;
  std::string status_line;
  std::string qr_url;
  bool busy = false;
};

// Initialize Steam client state from persisted token/identity + local library.
void steam_client_init(const std::string& state_dir, const std::string& games_dir);

// Sign out and wipe cached credentials/identity.
void steam_client_sign_out();

// Request QR login via Steam IAuthenticationService. Runs asynchronously.
void steam_client_begin_qr(std::function<void()> on_library_updated = nullptr);

// Refresh owned games + friends from Steam WebAPI asynchronously (when signed in).
void steam_client_refresh_remote(std::function<void()> on_library_updated = nullptr);

// Re-scan local games_dir against known Steam items.
void steam_client_rescan_local();

// Download and install an owned Steam game natively onto the console.
// Fails visibly with status_text if signed out, unowned, or unsupported codec.
bool steam_client_download_app(uint32_t app_id,
                               std::function<void()> on_complete = nullptr);
bool steam_client_cancel_download(uint32_t app_id);

// Snapshot current account + library items for UI/main loop (thread-safe).
steam_account_snapshot steam_client_account();
std::vector<steam_library_item> steam_client_items();

} // namespace longan

#endif // TIRAMISU_HOST_STEAM_CLIENT_H
