// tiramisu/src/trt/steam_library.h
// Read the user's REAL Steam installation: libraryfolders.vdf -> library
// roots, then each root's steamapps/appmanifest_*.acf -> installed apps.
//
// Why parse instead of asking Steam: Tiramisu never talks to a Steam client
// or account for library data. The files Steam itself maintains are the
// source of truth, they are plain text, and reading them needs no secret.
// Nothing here authenticates or claims ownership; ownership comes from
// steam_auth (OpenID) and the app manifests Steam wrote for installed titles.
#ifndef TIRAMISU_STEAM_LIBRARY_H
#define TIRAMISU_STEAM_LIBRARY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// One installed (or partially installed) Steam app.
struct trt_steam_app {
  unsigned appid = 0;
  std::string name;
  std::string installdir;
  std::string library_path;  // the library root that owns it
  unsigned long long size_on_disk = 0;
  unsigned state_flags = 0;  // Steam's EAppState bits; 4 = fully installed
  bool fully_installed = false;
};

// A Steam library root plus every app manifest found under it.
struct trt_steam_library {
  std::vector<std::string> roots;
  std::vector<trt_steam_app> apps;
  // Named gaps: a root or manifest that could not be read/parsed. Never a
  // silent skip -- the setup page shows these verbatim.
  std::vector<std::string> notes;
};

// Candidate Steam install roots for this host. Missing ones are simply not
// reported; every candidate that exists is returned.
std::vector<std::string> trt_steam_default_roots();

// Path to a library's libraryfolders.vdf ("" if absent).
std::string trt_steam_libraryfolders_path(const std::string& root);

// ---- VDF text parsing (exposed so it can be tested without a Steam install)
// Parse libraryfolders.vdf text into the library paths it lists. Accepts both
// the modern form ("0" { "path" "D:\\SteamLibrary" }) and the legacy form
// ("1" "D:\\SteamLibrary"), and ignores non-path keys such as "contentstatsid".
// Returns the number of paths appended, or -1 when the text is not VDF.
int trt_steam_parse_library_paths(const std::string& text,
                                  std::vector<std::string>* out);

// Parse one appmanifest_<id>.acf. Returns 0 on success, -1 when the text has no
// AppState block or no usable appid.
int trt_steam_parse_appmanifest(const std::string& text, trt_steam_app* out);

struct trt_steam_depot {
  uint32_t id = 0;
  uint32_t source_app = 0; // nonzero for a depot supplied by another app
  uint64_t manifest = 0;   // zero if looked up in source_app
  uint64_t size = 0;
  bool optional_dlc = false;
};

struct trt_steam_app_content {
  std::vector<trt_steam_depot> depots; // overlay order, later depots take precedence
  std::string executable;
  std::string arguments;
  std::string working_dir;
};

// Parse the public Windows content in a Steam PICS response. Selects the
// default launch entry, neutral and English files, and the applicable PC arch.
bool trt_steam_parse_app_content(const std::string& text, trt_steam_app_content* out);

// Look up one depot's public manifest in its source app, without applying
// the source app's platform or language selection to the requesting app.
bool trt_steam_find_public_depot(const std::string& text, uint32_t depot_id,
                                trt_steam_depot* out);

// Convert a depot path to a relative Windows path with canonical separators.
bool trt_steam_normalize_content_path(const std::string& path, std::string* out);

struct trt_steam_depot_path_owner {
  size_t depot;
  size_t file;
  bool directory;
};
using trt_steam_depot_layout = std::unordered_map<std::string, trt_steam_depot_path_owner>;

// Layout keys are normalized Windows paths, compared case insensitively for ASCII.
std::string trt_steam_depot_path_key(std::string path);
bool trt_steam_add_depot_path(trt_steam_depot_layout& layout, const std::string& path,
                             trt_steam_depot_path_owner owner);
bool trt_steam_validate_depot_layout(const trt_steam_depot_layout& layout,
                                    std::string* conflict);

// Pick the launcher exe inside an install dir: the largest *.exe in the top
// level of installdir (Steam games keep their launcher there; shipping
// binaries live in subdirectories). Returns "" when none is found.
std::string trt_steam_find_exe(const std::string& installdir);

// Scan every existing default root. Returns apps found. `lib` receives roots,
// apps and notes; `lib` may be null.
int trt_steam_library_scan(trt_steam_library* lib);

// Scan an explicit list of roots (tests, and a user-supplied extra root).
int trt_steam_library_scan_roots(const std::vector<std::string>& roots,
                                 trt_steam_library* lib);

#endif  // TIRAMISU_STEAM_LIBRARY_H
