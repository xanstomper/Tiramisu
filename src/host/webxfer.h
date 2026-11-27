// tiramisu/src/host/webxfer.h
// LAN/QR game transfer & dashboard HTTP server inside the UWP app.
// Launch the Xbox UWP app, scan the TV QR (or type the LAN URL), drag & drop
// any unpacked PC/Steam game folder or .zip, configure exe/appid/args/input,
// launch/stop games, and watch live console logs.
#ifndef TIRAMISU_WEBXFER_H
#define TIRAMISU_WEBXFER_H

#include <cstdint>
#include <string>
#include <vector>

namespace longan {

inline constexpr char kPortalGamesFolder[] = "TiramisuGames";

struct webxfer_usb_item {
  std::string id;
  std::string name;
  std::string volume_path;
  std::string fs;
  std::string path;
  std::string kind;  // "folder" | "zip" | "iso"
  std::string exe;
  uint64_t size_bytes = 0;
  bool win32_readable = true;
  bool installed_internal = false;
  bool operator==(const webxfer_usb_item&) const = default;
};

struct webxfer_storage_volume {
  std::string name;
  std::string path;
  std::vector<std::string> game_roots;
  std::string label;
  std::string fs;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  bool win32_readable = true;
  bool broker_readable = false;
  std::vector<webxfer_usb_item> items;
  bool operator==(const webxfer_storage_volume&) const = default;
};

void webxfer_set_external_storage(const std::vector<webxfer_storage_volume>& volumes,
                                  const std::string& broker_status);
bool webxfer_poll_usb_install(std::string* out_item_id, std::string* out_name);
void webxfer_set_usb_install_status(bool active, const std::string& phase,
                                    uint64_t done_bytes, uint64_t total_bytes,
                                    const std::string& error,
                                    const std::string& completed_note = {});
bool webxfer_extract_zip_to_dir(const std::string& zip_path, const std::string& dest_dir,
                                std::string* out_note, bool require_complete = true);
bool webxfer_sanitize_game_name(const std::string& raw, std::string* out);
// Extract one completed Device Portal ZIP and publish it without replacing a game.
bool webxfer_import_portal_zip(const std::string& local_state,
                               const std::string& filename, std::string* note);

struct webxfer_state {
  bool running = false;
  unsigned short port = 9127;
  std::string url;            // http://<ip>:<port>
  // handoffs: worker threads set name+flag; host loop consumes
  std::string launch_name;
  bool launch_requested = false;
  bool stop_requested = false;
  bool rescan_requested = false;
  // Scoped rescan (item 4): the game dir that changed, or empty for a full
  // rescan (manual /api/rescan, or a change that spans games). Set by the
  // upload/config/delete handlers; consumed by webxfer_poll_rescan.
  std::string rescan_game;
  std::string running_game;
  // Launch honesty (item 9): the outcome of the last launch the host loop
  // acted on ("started", "not found", ...), with the game it was for.
  // /api/launch itself only queues (202) — this is where the result lands.
  std::string last_launch_result;
  std::string last_launch_name;
  // status for the TV overlay
  std::string last_upload;    // "name (123 MB, 42 files)"
  std::string last_error;
};

// Score a dotted-quad IPv4 for LAN reachability (higher = better; 0 = loopback/APIPA).
int webxfer_score_ipv4(const char* dotted);

// Start the listener on default port 9127; returns http://<lan-ip>:9127.
std::string webxfer_start(const std::string& local_state, int display_w);

// Start listener on a specific port (0 = OS ephemeral port for tests).
std::string webxfer_start_port(const std::string& local_state, unsigned short port);

// Poll from the host loop: consume launch / stop / rescan requests.
bool webxfer_poll_launch(std::string* out_name);
// Find an uploaded game by folder name using the webxfer's OWN enumeration
// (the one /games serves). Returns true and fills out_exe with the exe path
// RELATIVE to the games root ("raccoin\RACCOIN.exe"). The shelf scan and
// this must agree; the shelf can come up empty on the console while /games
// works, so the launcher falls back to this.
void webxfer_set_package_games_dir(const std::string& path);
void webxfer_set_network_games_dir(const std::string& path);
bool webxfer_find_game(const char* name, char* out_exe, size_t cap,
                       std::string* out_game_dir = nullptr);
bool webxfer_poll_stop();
// Consume a pending rescan request. The overload also returns the scoped
// game dir (empty = full rescan). The no-arg form keeps the old contract
// (used by tests and any caller that always rescans fully).
bool webxfer_poll_rescan(std::string* out_game);
bool webxfer_poll_rescan();
void webxfer_set_running_game(const std::string& name);
// Record the outcome of a launch the host loop acted on ("started",
// "not found", ...). Surfaced in /api/status as last_launch_result.
void webxfer_set_launch_result(const std::string& name, const std::string& result);
// In-console autopilot: a transfer arms it; the host loop's existing launch
// outcome callback drives a match -> repair -> retry loop. Surfaced in
// /api/status as autopilot_game / autopilot_attempt / autopilot_result.
void webxfer_autopilot_begin(const std::string& game);
void webxfer_autopilot_state(std::string* game, int* attempt,
                             std::string* result);

// One-line status for the TV overlay ("transfer: http://...", last upload, err).
std::string webxfer_status_line();

// Live upload/download progress for the Xbox TV Transfer UI.
void webxfer_upload_progress(uint64_t* out_done, uint64_t* out_total,
                             std::string* out_label);

// Current transfer URL shown in the QR (empty until started).
std::string webxfer_url();
unsigned short webxfer_port();

// Paired LAN input. The UI thread applies these to Tiramisu's input bridge.
struct web_control_event { char kind; int first; int second; };
std::vector<web_control_event> webxfer_control_events();
std::string webxfer_control_code();

void webxfer_shutdown();

} // namespace longan

#endif
