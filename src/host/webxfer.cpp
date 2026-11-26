// tiramisu/src/host/webxfer.cpp
// Concurrent LAN HTTP game-transfer & management server (WinSock2 on UWP, POSIX
// sockets on Linux offline tests).
// Endpoints:
//   GET  /                                      -> Mobile + Desktop Web Dashboard
//   GET  /games                                 -> JSON array of games + exes + config
//   GET  /api/status                            -> JSON live status & upload progress
//   GET  /api/logs                              -> Tail of tiramisu.log
//   GET  /api/backup-saves                      -> Download .zip of drive_c + profiles + settings
//   POST /api/restore-saves                     -> Restore drive_c + profiles + settings from .zip
//   GET  /api/debug-bundle                      -> Download support .zip (logs + settings + summary)
//   GET  /progress/<id>                         -> Fractional upload progress 0..1
//   PUT  /upload/<name>.zip                     -> Streaming .zip upload -> extract to games\<stem>
//   PUT  /api/upload-file?game=<n>&rel=<path>   -> Streaming unpacked folder file upload
//   GET  /api/list-files?game=<n>              -> JSON array of {rel,size} for a game dir
//   POST /launch/<name> | /api/launch/<name>    -> Launch game on TV
//   POST /api/stop                              -> Request clean stop of active guest game
//   POST /api/rescan                            -> Force immediate game library rescan
//   POST /api/config/<name>                     -> Save exe / appid / args / input preset
//   DELETE /api/game/<name> | POST /api/delete/<name> -> Delete game directory
#include "webxfer.h"
#include "crash_report.h"
#include "web_controls_page.h"
#include "host_log.h"
#include "settings.h"
#ifdef TIRAMISU_UWP_HOST
  #include "cover_art.h"
  #include "../trt/bridge_d3d.h"
  #include "swapchain.h"
  #include "../trt/iso.h"
  #include <memory>
  #include <stdexcept>
  #include "steam_client.h"
  #include "vendor/qrcodegen.h"
  #include <winrt/base.h>
  #include <winrt/Windows.Foundation.h>
  #include <winrt/Windows.Storage.Streams.h>
  #include <winrt/Windows.Web.Http.h>
  #include <winrt/windows.web.http.headers.h>
#endif

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>    // brown-line paired-LAN control pairing RNG
  #include <sys/stat.h>  // _stat64 for upload verification
  #pragma comment(lib, "bcrypt.lib")
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  static const sock_t kBadSock = INVALID_SOCKET;
  #define XFER_SEP '\\'
#else
  #include <arpa/inet.h>
  #include <dirent.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #ifdef __linux__
    #include <ifaddrs.h>
  #endif
  using sock_t = int;
  static const sock_t kBadSock = -1;
  #define XFER_SEP '/'
#endif

// 64-bit file positioning: plain fseek/ftell use 32-bit long on Windows and
// break past 2 GB (tiramisu.log grows past that on long sessions). On LP64
// Linux long is already 64-bit, so plain fseek/ftell are correct there.
#if defined(_WIN32)
  #define xfer_fseek _fseeki64
  #define xfer_ftell _ftelli64
#else
  #define xfer_fseek fseek
  #define xfer_ftell ftell
#endif

#include "vendor/miniz.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace longan {

namespace {

std::mutex g_mu;
webxfer_state g;

// .238 DIAG: throttle for the HTTP instrumentation lines -- at
// most one line per interval per call site, so a polling
// dashboard does not spam tiramisu.log. Returns true when the
// line may be logged (and records the timestamp).
static bool webxfer_throttle(std::atomic<long long>& last_ms,
                             long long interval_ms) {
  const long long now =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  long long prev = last_ms.load(std::memory_order_relaxed);
  if (now - prev < interval_ms) return false;
  // NOTE: prev must be non-const -- compare_exchange_strong
  // takes the expected value by reference and updates it on
  // failure (a const here fails to compile -- caught by the
  // .238 build).
  return last_ms.compare_exchange_strong(prev, now,
                                         std::memory_order_relaxed);
}
static std::atomic<long long> s_http_accept_last_ms{0};
static std::atomic<long long> s_http_worker_last_ms{0};
static std::atomic<long long> s_http_status_last_ms{0};
static std::atomic<long long> s_http_respond_last_ms{0};

// In-flight upload guard (item 8): concurrent upload streams to DIFFERENT
// files are the whole point of parallelism, but two streams writing the
// SAME file would interleave bytes and fail verification. The registry
// keys on game + rel (or "zip:" + filename for the zip path); a duplicate
// in-flight upload gets 409. Entries are erased on every exit path via
// the RAII guard in the handlers.
std::mutex g_upload_mu;
std::set<std::string> g_uploads_inflight;

struct inflight_guard {
  std::string key;
  explicit inflight_guard(std::string k) : key(std::move(k)) {}
  ~inflight_guard() {
    std::lock_guard<std::mutex> lk(g_upload_mu);
    g_uploads_inflight.erase(key);
  }
  inflight_guard(const inflight_guard&) = delete;
  inflight_guard& operator=(const inflight_guard&) = delete;
};
std::mutex g_external_storage_mu;
std::vector<webxfer_storage_volume> g_external_storage;
std::string g_storage_broker_status = "checking";
std::atomic<bool> g_url_active{false};
std::atomic<uint64_t> g_url_received{0}, g_url_expected{0};
std::string g_url_phase;  // guarded by g_mu
std::atomic<bool> g_stop{false};
std::atomic<int> g_workers{0};
sock_t g_listener = kBadSock;

using control_clock = std::chrono::steady_clock;
std::mutex g_control_mu;
std::string g_control_code;
std::string g_control_token;
std::vector<web_control_event> g_control_events;
control_clock::time_point g_control_last{};
control_clock::time_point g_control_retry{};
unsigned g_control_bad_codes = 0;

bool control_random(void* data, size_t size) {
#ifdef _WIN32
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(data),
                         static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
  FILE* source = fopen("/dev/urandom", "rb");
  if (!source) return false;
  const bool ok = fread(data, 1, size, source) == size;
  fclose(source);
  return ok;
#endif
}

void reset_web_controls() {
  unsigned bytes = 0;
  std::lock_guard<std::mutex> lock(g_control_mu);
  g_control_code.clear();
  if (control_random(&bytes, sizeof(bytes))) {
    char code[9];
    snprintf(code, sizeof(code), "%08u", bytes % 100000000u);
    g_control_code = code;
  }
  g_control_token.clear();
  g_control_events.clear();
  g_control_last = {};
  g_control_retry = {};
  g_control_bad_codes = 0;
}

std::string g_local_state_dir;
std::string g_games_dir;
std::string g_package_games_dir;
std::string g_network_games_dir;
std::mutex g_network_root_mu;
std::string g_incoming;

void mkdirs(const std::string& p);  // defined just below (used by autopilot)

// ---- in-console autopilot --------------------------------------------------
// Armed by a transfer, driven by main.cpp's EXISTING launch handoff: the host
// loop consumes launch_requested and reports the outcome through
// webxfer_set_launch_result(). A failed boot is matched against a declared
// signature set, a real repair is written, and the next launch is queued.
// Keeping the machine here means no host source outside webxfer.cpp changes.
struct autopilot_state {
  std::string game;
  int attempt = 0;
  int max_attempts = 5;
  std::string result = "idle";
};
autopilot_state g_ap;

static std::string ap_read_small(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  xfer_fseek(f, 0, SEEK_END);
  long long sz = (long long)xfer_ftell(f);
  if (sz <= 0) { fclose(f); return {}; }
  const long long take = sz > 131072 ? 131072 : sz;  // tail only
  xfer_fseek(f, sz - take, SEEK_SET);
  std::string out((size_t)take, '\0');
  size_t n = fread(&out[0], 1, (size_t)take, f);
  fclose(f);
  out.resize(n);
  return out;
}

static void ap_write_small(const std::string& path, const std::string& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}

static bool ap_has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

static void ap_history(const char* outcome) {
  const std::string dir = g_games_dir + std::string(1, XFER_SEP) + g_ap.game +
                          std::string(1, XFER_SEP) + ".tiramisu";
  mkdirs(dir);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"game\":\"%s\",\"attempt\":%d,\"result\":\"%s\"}\n",
           g_ap.game.c_str(), g_ap.attempt, outcome);
  ap_write_small(dir + std::string(1, XFER_SEP) + "autopilot.json", buf);
}

// Lock must be held. Returns true when a repair was applied and a retry queued.
static bool ap_evaluate_locked() {
  if (g_ap.game.empty()) return false;
  const std::string sep(1, XFER_SEP);
  const std::string gdir = g_games_dir + sep + g_ap.game;
  const std::string log = ap_read_small(
      g_local_state_dir + sep + "tiramisu.log");

  std::string aliases;
  std::string profile;
  int fixes = 0;
  auto alias = [&](const char* from, const char* to) {
    aliases += std::string("  \"") + from + "\": \"" + to + "\",\n";
    ++fixes;
  };
  if (ap_has(log, "msvcp140")) {
    alias("msvcp140.dll", "msvcp140_app.dll");
    alias("msvcp140_1.dll", "msvcp140_1_app.dll");
  }
  if (ap_has(log, "vcruntime140")) {
    alias("vcruntime140.dll", "vcruntime140_app.dll");
  }
  if (ap_has(log, "d3dcompiler_4")) {
    alias("d3dcompiler_43.dll", "d3dcompiler_47.dll");
    alias("d3dcompiler_46.dll", "d3dcompiler_47.dll");
  }
  if (ap_has(log, "xinput1_4") || ap_has(log, "xinput1_3")) {
    alias("xinput1_4.dll", "xinputuap.dll");
    alias("xinput1_3.dll", "xinputuap.dll");
  }
  if (ap_has(log, "SuspendThread loop failed") ||
      ap_has(log, "Fatal error in GC")) {
    profile += "write_watch=1\nmemory_budget_mb=4096\n";
    ++fixes;
  }
  if (ap_has(log, "frame") && ap_has(log, "stall")) {
    profile += "frame_rate_cap=60\nvsync=1\n";
    ++fixes;
  }
  // Real console probe (logs/20261121-203238-tiramisu.log): the GameInput
  // mouse callback returns E_NOTIMPL on Series S. Switch to controller input
  // rather than leaving the title on a callback that cannot succeed.
  if (ap_has(log, "GameInput mouse callback failed hr=0x80004001")) {
    profile += "default_input=controller\n";
    ++fixes;
  }

  if (!fixes) {
    g_ap.result = "no-fix";
    ap_history("no-fix");  // named stopping point, never a silent skip
    return false;
  }

  if (!aliases.empty()) {
    if (aliases.size() >= 2) aliases.resize(aliases.size() - 2);  // trailing ","
    const std::string mdir = gdir + sep + ".tiramisu";
    mkdirs(mdir);
    ap_write_small(mdir + sep + "aliases.json",
                   "{\n" + aliases + "\n}\n");
  }
  if (!profile.empty()) {
    // Merge: preserve keys the title already had (titlebuild writes defaults).
    const std::string ini_path = gdir + sep + "tiramisu.profile.ini";
    std::string merged = ap_read_small(ini_path);
    if (merged.find("write_watch") == std::string::npos)
      merged += profile;
    ap_write_small(ini_path, merged);
  }

  ++g_ap.attempt;
  char out[64];
  if (g_ap.attempt >= g_ap.max_attempts) {
    snprintf(out, sizeof(out), "attempt-limit");
    g_ap.result = out;
    ap_history(out);
    return false;
  }
  g_ap.result = "fix";
  g.launch_name = g_ap.game;    // main.cpp consumes this on its next poll
  g.launch_requested = true;
  ap_history("fix");
  return true;
}

std::atomic<uint64_t> g_upload_total{0};
std::atomic<uint64_t> g_upload_done{0};
std::mutex g_upload_label_mu;
std::string g_upload_label;

void set_upload_label(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_upload_label_mu);
  g_upload_label = s;
}

void sock_close(sock_t s) {
  if (s == kBadSock) return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

struct sock_guard {
  sock_t s;
  ~sock_guard() { sock_close(s); }
};

std::string xfer_lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool xfer_ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
      return false;
    }
  }
  return true;
}

std::string trim_ws(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && (unsigned char)s[a] <= ' ') ++a;
  size_t b = s.size();
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

// ---------- cross-platform filesystem helpers ----------
bool dir_exists(const std::string& p) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '\\' || p[i] == '/') {
      cur = p.substr(0, i);
      if (!cur.empty() && cur != "/" &&
          !(cur.size() == 2 && cur[1] == ':') && !dir_exists(cur)) {
#ifdef _WIN32
        CreateDirectoryA(cur.c_str(), nullptr);
#else
        mkdir(cur.c_str(), 0755);
#endif
      }
    }
  }
}

struct dir_ent {
  std::string name;
  bool is_dir = false;
  uint64_t size = 0;
};

void list_dir_entries(const std::string& dir, std::vector<dir_ent>* out) {
  out->clear();
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string n = fd.cFileName;
    if (n == "." || n == "..") continue;
    dir_ent e;
    e.name = std::move(n);
    e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    out->push_back(std::move(e));
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* ent = readdir(d)) {
    std::string n = ent->d_name;
    if (n == "." || n == "..") continue;
    std::string full = dir + "/" + n;
    struct stat st{};
    if (stat(full.c_str(), &st) != 0) continue;
    dir_ent e;
    e.name = std::move(n);
    e.is_dir = S_ISDIR(st.st_mode);
    e.size = e.is_dir ? 0 : (uint64_t)st.st_size;
    out->push_back(std::move(e));
  }
  closedir(d);
#endif
}

std::vector<std::string> game_roots() {
  std::vector<std::string> roots;
  if (!g_games_dir.empty()) roots.push_back(g_games_dir);
  if (!g_package_games_dir.empty() && g_package_games_dir != g_games_dir)
    roots.push_back(g_package_games_dir);
  std::lock_guard<std::mutex> network_lock(g_network_root_mu);
  if (!g_network_games_dir.empty() &&
      g_network_games_dir != g_games_dir &&
      g_network_games_dir != g_package_games_dir)
    roots.push_back(g_network_games_dir);
  return roots;
}

std::string find_game_dir(const std::string& name) {
  const std::string sep(1, XFER_SEP);
  for (const auto& root : game_roots()) {
    const std::string direct = root + sep + name;
    if (dir_exists(direct)) return direct;
    std::vector<dir_ent> entries;
    list_dir_entries(root, &entries);
    for (const auto& entry : entries) {
      if (entry.is_dir && xfer_ieq(entry.name, name))
        return root + sep + entry.name;
    }
  }
  return {};
}

bool remove_tree(const std::string& dir) {
  if (!dir_exists(dir)) return false;
  std::vector<dir_ent> ents;
  list_dir_entries(dir, &ents);
  for (const auto& e : ents) {
    std::string child = dir + std::string(1, XFER_SEP) + e.name;
    if (e.is_dir) remove_tree(child);
    else {
#ifdef _WIN32
      DeleteFileA(child.c_str());
#else
      unlink(child.c_str());
#endif
    }
  }
#ifdef _WIN32
  return RemoveDirectoryA(dir.c_str()) != 0;
#else
  return rmdir(dir.c_str()) == 0;
#endif
}

std::string read_small_text(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  char buf[2048];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  return trim_ws(buf);
}

bool write_or_clear_text(const std::string& p, const std::string& val) {
  std::string v = trim_ws(val);
  if (v.empty()) {
    remove(p.c_str());
    return true;
  }
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return false;
  fwrite(v.data(), 1, v.size(), f);
  fclose(f);
  return true;
}

std::string read_log_tail(const std::string& p, size_t max_bytes = 16384) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return "(no logs yet)";
  xfer_fseek(f, 0, SEEK_END);
  const int64_t sz = (int64_t)xfer_ftell(f);
  if (sz <= 0) {
    fclose(f);
    return "(log empty)";
  }
  int64_t off = (sz > (int64_t)max_bytes) ? (sz - (int64_t)max_bytes) : 0;
  xfer_fseek(f, off, SEEK_SET);
  std::string out((size_t)(sz - off), '\0');
  size_t n = fread(&out[0], 1, out.size(), f);
  out.resize(n);
  fclose(f);
  if (off > 0) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(nl + 1);
  }
  return out;
}

// ---------- URL decoding & strict path sanitization ----------
int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::string url_decode(const std::string& s, bool plus_is_space = false) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int h = hex_val(s[i + 1]);
      int l = hex_val(s[i + 2]);
      if (h >= 0 && l >= 0) {
        out.push_back((char)((h << 4) | l));
        i += 2;
        continue;
      }
    } else if (plus_is_space && s[i] == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string query_param(const std::string& query, const std::string& key) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string kv = (amp == std::string::npos) ? query.substr(pos)
                                                : query.substr(pos, amp - pos);
    size_t eq = kv.find('=');
    if (eq != std::string::npos && kv.substr(0, eq) == key) {
      return url_decode(kv.substr(eq + 1), true);
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

bool valid_path_component(const std::string& name) {
  if (name.empty() || name.back() == '.' || name.back() == ' ') return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '<' ||
        c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return false;
  }
  const std::string stem = xfer_lower(trim_ws(name.substr(0, name.find('.'))));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
      stem == "conin$" || stem == "conout$") return false;
  if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
      stem[3] >= '0' && stem[3] <= '9') return false;
  return true;
}

bool sanitize_game_name(const std::string& raw, std::string* out) {
  std::string s = trim_ws(raw);
  if (s.empty() || s.size() > 160 || s == "." || s == "..") return false;
  if (s.find("..") != std::string::npos) return false;
  if (!valid_path_component(s)) return false;
  *out = s;
  return true;
}

bool sanitize_rel_path(const std::string& raw, char sep, std::string* out) {
  if (raw.empty() || raw.size() > 512) return false;
  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= raw.size(); ++i) {
    if (i == raw.size() || raw[i] == '/' || raw[i] == '\\') {
      if (!cur.empty()) {
        if (cur == "." || cur == ".." || cur.find("..") != std::string::npos) {
          return false;
        }
        if (!valid_path_component(cur)) return false;
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(raw[i]);
    }
  }
  if (parts.empty()) return false;
  std::string res;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) res.push_back(sep);
    res += parts[i];
  }
  *out = res;
  return true;
}

// ---------- Exe scanner ----------
bool xfer_is_junk_exe(const std::string& fn) {
  const std::string s = xfer_lower(fn);
  return s.find("unitycrashhandler") != std::string::npos ||
         s.find("crashreport") != std::string::npos ||
         s.find("ueprereq") != std::string::npos ||
         s.find("ue4prereq") != std::string::npos ||
         s.find("ue5prereq") != std::string::npos ||
         s.find("vcredist") != std::string::npos ||
         s.find("vc_redist") != std::string::npos ||
         s.find("dxsetup") != std::string::npos ||
         s.find("dotnet") != std::string::npos ||
         s.find("unins0") != std::string::npos ||
         s.find("uninstall") != std::string::npos;
}

struct exe_item {
  std::string rel;
  int score = 0;
  uint64_t size = 0;
};

void collect_exes_rec(const std::string& base_dir, const std::string& rel_dir,
                      const std::string& game_name, int depth,
                      std::vector<exe_item>* out) {
  if (depth > 5) return;
  const std::string sep(1, XFER_SEP);
  const std::string dir = rel_dir.empty() ? base_dir : (base_dir + sep + rel_dir);
  std::vector<dir_ent> ents;
  list_dir_entries(dir, &ents);
  const std::string g_low = xfer_lower(game_name);
  for (const auto& e : ents) {
    if (e.name == "__MACOSX") continue;
    const std::string rel = rel_dir.empty() ? e.name : (rel_dir + sep + e.name);
    if (e.is_dir) {
      collect_exes_rec(base_dir, rel, game_name, depth + 1, out);
      continue;
    }
    const size_t dot = e.name.rfind('.');
    if (dot == std::string::npos || !xfer_ieq(e.name.substr(dot), ".exe")) continue;
    if (xfer_is_junk_exe(e.name)) continue;
    const std::string stem_low = xfer_lower(e.name.substr(0, dot));
    int sc = (depth == 0) ? 60 : (depth == 1 ? 30 : 0);
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") sc += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15) sc += 120;
    out->push_back({rel, sc, e.size});
  }
}

void scan_game_exes(const std::string& game_dir, const std::string& game_name,
                    std::vector<std::string>* out_exes, std::string* selected) {
  out_exes->clear();
  selected->clear();
  std::vector<exe_item> items;
  collect_exes_rec(game_dir, "", game_name, 0, &items);
  std::sort(items.begin(), items.end(), [](const exe_item& a, const exe_item& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.size != b.size) return a.size > b.size;
    return a.rel < b.rel;
  });
  for (const auto& it : items) out_exes->push_back(it.rel);
  if (out_exes->empty()) return;
  *selected = (*out_exes)[0];
  const std::string cfg_exe =
      read_small_text(game_dir + std::string(1, XFER_SEP) + "tiramisu_exe.txt");
  if (!cfg_exe.empty()) {
    for (const auto& ex : *out_exes) {
      std::string a = ex, b = cfg_exe;
      for (char& c : a) if (c == '\\') c = '/';
      for (char& c : b) if (c == '\\') c = '/';
      if (xfer_ieq(a, b)) {
        *selected = ex;
        break;
      }
    }
  }
}

// ---------- recursive file listing (for /api/list-files) ----------
struct file_item {
  std::string rel;
  uint64_t size = 0;
};

void collect_files_rec(const std::string& base_dir, const std::string& rel_dir,
                       int depth, std::vector<file_item>* out) {
  if (depth > 32) return;
  const std::string sep(1, XFER_SEP);
  const std::string dir = rel_dir.empty() ? base_dir : (base_dir + sep + rel_dir);
  std::vector<dir_ent> ents;
  list_dir_entries(dir, &ents);
  for (const auto& e : ents) {
    const std::string rel = rel_dir.empty() ? e.name : (rel_dir + sep + e.name);
    if (e.is_dir) {
      collect_files_rec(base_dir, rel, depth + 1, out);
      continue;
    }
    out->push_back({rel, e.size});
  }
}

// ---------- zip extraction (miniz, smart single-folder unwrap) ----------
bool extract_zip(const std::string& zip_path, const std::string& dest,
                 std::string* note, bool require_complete = false) {
  mz_zip_archive zip{};
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
    *note = "not a valid zip";
    return false;
  }
  const int n = (int)mz_zip_reader_get_num_files(&zip);
#ifdef _WIN32
  ULARGE_INTEGER available{};
  if (require_complete && GetDiskFreeSpaceExA(dest.c_str(), &available, nullptr, nullptr)) {
    uint64_t remaining = available.QuadPart;
    for (int i = 0; i < n; ++i) {
      mz_zip_archive_file_stat st{};
      if (!mz_zip_reader_file_stat(&zip, i, &st) ||
          (!st.m_is_directory && st.m_uncomp_size > remaining)) {
        mz_zip_reader_end(&zip);
        *note = "archive does not fit in available storage or contains an unreadable entry";
        return false;
      }
      if (!st.m_is_directory) remaining -= st.m_uncomp_size;
    }
  }
#endif
  // Detect if every real file lives inside a single top-level wrapper dir
  // (e.g. "MyGame/MyGame.exe"), and unwrap it so it lands at games\MyGame\MyGame.exe
  // rather than games\MyGame\MyGame\MyGame.exe.
  std::string common_top;
  bool can_unwrap = (n > 0);
  for (int i = 0; i < n && can_unwrap; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    std::string rel;
    if (!sanitize_rel_path(st.m_filename, '/', &rel)) continue;
    if (rel.rfind("__MACOSX/", 0) == 0) continue;
    size_t sl = rel.find('/');
    if (sl == std::string::npos) {
      can_unwrap = false;
      break;
    }
    std::string top = rel.substr(0, sl);
    if (common_top.empty()) common_top = top;
    else if (!xfer_ieq(common_top, top)) can_unwrap = false;
  }
  if (common_top.empty()) can_unwrap = false;
  if (can_unwrap) {
    // Only unwrap when the single top folder matches the destination game name
    // (avoids stripping legitimate subdirs like "bin/").
    const size_t ds = dest.find_last_of("/\\");
    const std::string dest_base =
        (ds == std::string::npos) ? dest : dest.substr(ds + 1);
    if (!xfer_ieq(common_top, dest_base)) can_unwrap = false;
  }

  int extracted = 0;
  int failed = 0;
  for (int i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) { ++failed; continue; }
    if (st.m_is_directory) continue;
    std::string rel_raw = st.m_filename;
    if (rel_raw.rfind("__MACOSX/", 0) == 0) continue;
    if (can_unwrap) {
      std::string norm;
      if (sanitize_rel_path(rel_raw, '/', &norm) &&
          norm.size() > common_top.size() + 1 &&
          xfer_ieq(norm.substr(0, common_top.size()), common_top) &&
          norm[common_top.size()] == '/') {
        rel_raw = norm.substr(common_top.size() + 1);
      }
    }
    std::string rel;
    if (!sanitize_rel_path(rel_raw, XFER_SEP, &rel)) { ++failed; continue; }
    const std::string out_path = dest + std::string(1, XFER_SEP) + rel;
    const size_t slash = out_path.rfind(XFER_SEP);
    if (slash != std::string::npos) mkdirs(out_path.substr(0, slash));
    if (mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0)) {
      ++extracted;
    } else {
      ++failed;
    }
    if (require_complete && failed) break;
  }
  mz_zip_reader_end(&zip);
  if (require_complete && failed) {
    *note = "archive contains an unreadable or unsupported entry";
    return false;
  }
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%d files", extracted);
  *note = tmp;
  return extracted > 0;
}

#ifdef TIRAMISU_UWP_HOST
void finish_url_download(const std::string& name, const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_url_phase.clear();
    if (error.empty()) {
      g.last_upload = name + " installed";
      g.last_error.clear();
      g.rescan_requested = true;
      g.rescan_game = name;
    } else {
      g.last_error = error;
    }
  }
  g_url_received = 0;
  g_url_expected = 0;
  g_url_active = false;
}

void download_game_url(std::string url, std::string name, std::string extension,
                       std::string incoming, std::string games) {
  std::string error;
  std::string stage;
  auto set_phase = [](const char* phase) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_url_phase = phase;
  };
  const bool zip = extension == ".zip";
  const std::string destination = games + "\\" + name + (zip ? "" : extension);
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct apartment_guard { ~apartment_guard() { winrt::uninit_apartment(); } } apartment;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    if (GetFileAttributesA(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
      throw std::runtime_error("Game is already installed; choose a different name");
    }
    // Create a private directory on the same volume as the library. Only
    // rename the verified payload into the library after a complete transfer.
    const std::string candidate = incoming + "\\url-" + std::to_string(GetTickCount64());
    if (!CreateDirectoryA(candidate.c_str(), nullptr))
      throw std::runtime_error("Cannot create download staging folder");
    stage = candidate;
    const std::string payload = stage + "\\payload" + extension;
    HttpClient http;
    const auto response = http.GetAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(url)),
                                        HttpCompletionOption::ResponseHeadersRead).get();
    if (!response.IsSuccessStatusCode())
      throw std::runtime_error("Download HTTP " + std::to_string((int)response.StatusCode()));
    uint64_t expected = 0;
    if (const auto length = response.Content().Headers().ContentLength()) expected = length.Value();
    ULARGE_INTEGER available{};
    if (expected && GetDiskFreeSpaceExA(incoming.c_str(), &available, nullptr, nullptr) &&
        expected > available.QuadPart)
      throw std::runtime_error("Insufficient storage for download");
    g_url_expected = expected;
    set_phase("Downloading");
    auto stream = response.Content().ReadAsInputStreamAsync().get();
    Buffer buffer(65536);
    std::vector<uint8_t> bytes(65536);
    std::unique_ptr<FILE, decltype(&fclose)> output(fopen(payload.c_str(), "wb"), fclose);
    if (!output) throw std::runtime_error("Cannot open download staging file");
    uint64_t received = 0;
    while (!g_stop) {
      auto chunk = stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial).get();
      const size_t count = chunk.Length();
      if (!count) break;
      auto reader = DataReader::FromBuffer(chunk);
      reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + count));
      if (fwrite(bytes.data(), 1, count, output.get()) != count)
        throw std::runtime_error("Could not write download; check free storage");
      received += count;
      g_url_received = received;
      if (expected && received > expected)
        throw std::runtime_error("Download exceeded its advertised length");
    }
    if (fclose(output.release()) != 0)
      throw std::runtime_error("Could not finish writing download");
    if (g_stop) throw std::runtime_error("Download cancelled");
    if (received == 0 || (expected && received != expected))
      throw std::runtime_error("Download ended before the full file arrived");

    std::string install_source = payload;
    if (zip) {
      set_phase("Extracting");
      install_source = stage + "\\" + name;
      if (!CreateDirectoryA(install_source.c_str(), nullptr))
        throw std::runtime_error("Cannot create extraction folder");
      std::string note;
      if (!extract_zip(payload, install_source, &note, true))
        throw std::runtime_error("Download extraction failed: " + note);
      std::vector<std::string> exes;
      std::string exe;
      scan_game_exes(install_source, name, &exes, &exe);
      if (exe.empty()) {
        char image[1024] = {};
        if (!trt_iso_find_image_in_dir(install_source.c_str(), image, sizeof(image)))
          throw std::runtime_error("Archive contains no game executable or disc image");
      }
    } else {
      trt_iso* image = trt_iso_open(payload.c_str());
      if (!image) throw std::runtime_error("Download is not a readable disc image");
      trt_iso_close(image);
    }
    if (g_stop) throw std::runtime_error("Download cancelled");
    // Windows rename fails if the destination already exists, including when
    // an upload installed the same game while the download was in progress.
    if (rename(install_source.c_str(), destination.c_str()) != 0)
      throw std::runtime_error("Cannot install game; the destination may already exist");
  } catch (const std::exception& e) {
    error = e.what();
  } catch (const winrt::hresult_error& e) {
    char code[24];
    snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(e.code().value));
    error = std::string("Xbox download error ") + code;
  } catch (...) {
    error = "Xbox download failed";
  }
  if (!stage.empty()) remove_tree(stage);
  finish_url_download(name, error);
}
#endif

// ---------- Pre-launch PE compatibility inspection ----------
struct pe_compat_info {
  std::string arch = "x64";
  std::string badge = "Windows x64";
  bool ok = true;
};

static pe_compat_info inspect_pe_compat(const std::string& gdir,
                                        const std::string& exe_rel) {
  pe_compat_info out;
  const std::string sep(1, XFER_SEP);
  const std::string full = gdir + sep + exe_rel;
  FILE* f = fopen(full.c_str(), "rb");
  if (!f) {
    out.arch = "unknown";
    out.badge = "exe unreadable";
    out.ok = false;
    return out;
  }
  uint8_t hdr[4096] = {};
  const size_t got = fread(hdr, 1, sizeof(hdr), f);
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    fclose(f);
    out.arch = "unknown";
    out.badge = "not a Windows PE";
    out.ok = false;
    return out;
  }
  uint32_t e_lfanew = 0;
  memcpy(&e_lfanew, hdr + 0x3C, 4);
  if (e_lfanew + 26 > got || hdr[e_lfanew] != 'P' || hdr[e_lfanew + 1] != 'E') {
    fclose(f);
    out.arch = "unknown";
    out.badge = "invalid PE header";
    out.ok = false;
    return out;
  }
  const uint16_t machine = (uint16_t)(hdr[e_lfanew + 4] | (hdr[e_lfanew + 5] << 8));
  const uint16_t num_sec = (uint16_t)(hdr[e_lfanew + 6] | (hdr[e_lfanew + 7] << 8));
  const uint16_t opt_size = (uint16_t)(hdr[e_lfanew + 20] | (hdr[e_lfanew + 21] << 8));
  const uint16_t opt_magic = (uint16_t)(hdr[e_lfanew + 24] | (hdr[e_lfanew + 25] << 8));
  if (machine == 0x8664 && opt_magic == 0x20B) {
    out.arch = "x64";
  } else if (machine == 0x014C) {
    // Tiramisu routes PE32 binaries through the x86-32 interpreter/JIT (x86_run_pe32).
    out.arch = "x86";
    out.badge = "x86 32-bit (JIT)";
    out.ok = true;
    fclose(f);
    return out;
  } else if (machine == 0xAA64) {
    fclose(f);
    out.arch = "arm64";
    out.badge = "ARM64 (Unsupported)";
    out.ok = false;
    return out;
  } else {
    fclose(f);
    out.arch = "unknown";
    out.badge = "Unsupported Arch";
    out.ok = false;
    return out;
  }

  uint32_t imp_rva = 0;
  const uint32_t opt_off = e_lfanew + 24;
  if (opt_off + 128 <= got) memcpy(&imp_rva, hdr + opt_off + 120, 4);
  const uint32_t sec_off = opt_off + opt_size;
  auto rva_to_file = [&](uint32_t rva) -> uint64_t {
    const uint32_t secs = num_sec < 96 ? num_sec : 96;
    for (uint32_t s = 0; s < secs; ++s) {
      const uint32_t so = sec_off + s * 40;
      if (so + 40 > got) break;
      uint32_t vaddr = 0, vsize = 0, raw = 0, rawsz = 0;
      memcpy(&vsize, hdr + so + 8, 4);
      memcpy(&vaddr, hdr + so + 12, 4);
      memcpy(&rawsz, hdr + so + 16, 4);
      memcpy(&raw, hdr + so + 20, 4);
      const uint32_t span = vsize > rawsz ? vsize : rawsz;
      if (rva >= vaddr && rva < vaddr + span && (rva - vaddr) < rawsz) {
        return (uint64_t)raw + (uint64_t)(rva - vaddr);
      }
    }
    return 0;
  };

  bool has_d3d11 = false, has_d3d12 = false, has_vk = false;
  bool has_gl = false, has_d3d9 = false, has_unity = false, has_ac = false;
  bool has_gdk = false;
  const uint64_t imp_file = imp_rva ? rva_to_file(imp_rva) : 0;
  if (imp_file > 0) {
    uint8_t desc[2048] = {};
    if (xfer_fseek(f, (long long)imp_file, SEEK_SET) == 0) {
      const size_t dgot = fread(desc, 1, sizeof(desc), f);
      for (uint32_t d = 0; d * 20 + 20 <= dgot; ++d) {
        uint32_t name_rva = 0;
        memcpy(&name_rva, desc + d * 20 + 12, 4);
        if (!name_rva) break;
        const uint64_t nfile = rva_to_file(name_rva);
        if (!nfile || xfer_fseek(f, (long long)nfile, SEEK_SET) != 0) break;
        char nm[64] = {};
        if (fread(nm, 1, sizeof(nm) - 1, f) == 0) continue;
        const std::string low = xfer_lower(nm);
        if (low.rfind("d3d11", 0) == 0) has_d3d11 = true;
        else if (low.rfind("d3d12", 0) == 0) has_d3d12 = true;
        else if (low.rfind("vulkan-1", 0) == 0) has_vk = true;
        else if (low.rfind("opengl32", 0) == 0) has_gl = true;
        else if (low.rfind("d3d9", 0) == 0 || low.rfind("d3d8", 0) == 0 ||
                 low.rfind("ddraw", 0) == 0) has_d3d9 = true;
        else if (low.rfind("unityplayer", 0) == 0) has_unity = true;
        else if (low.find("easyanticheat") != std::string::npos ||
                 low.find("battleye") != std::string::npos) has_ac = true;
        else if (low.rfind("xgameruntime", 0) == 0 ||
                 low.rfind("gamingservices", 0) == 0 ||
                 low.rfind("gameos", 0) == 0 ||
                 low.rfind("libhttpclient.gdk", 0) == 0) has_gdk = true;
      }
    }
  }
  fclose(f);

  // Also check top-level game dir for common kernel anti-cheat launchers/DLLs
  // and Xbox GDK / GameOS console payloads (not Win32 targets).
  std::vector<dir_ent> top;
  list_dir_entries(gdir, &top);
  for (const auto& e : top) {
    const std::string low = xfer_lower(e.name);
    if (low.find("easyanticheat") != std::string::npos ||
        low.find("battleye") != std::string::npos) {
      has_ac = true;
    }
    if (low == "microsoftgame.config" || low == "gameos.xvd" ||
        low.rfind("libhttpclient.gdk", 0) == 0) {
      has_gdk = true;
    }
  }

  if (has_ac) {
    out.badge = "Anti-Cheat Detected (Blocked)";
    out.ok = false;
  } else if (has_gdk) {
    out.badge = "Xbox GDK / GameOS build (Unsupported)";
    out.ok = false;
  } else if (has_d3d12 && !has_d3d11) {
    out.badge = "x64 D3D12 (Experimental)";
  } else if (has_vk) {
    out.badge = "x64 Vulkan Ready";
  } else if (has_unity) {
    out.badge = "x64 Unity Ready";
  } else if (has_d3d11) {
    out.badge = "x64 D3D11 Ready";
  } else if (has_gl && !has_d3d11) {
    out.badge = "x64 OpenGL (Experimental)";
  } else if (has_d3d9) {
    out.badge = "x64 D3D9 Ready";
  } else {
    out.badge = "x64 Windows Ready";
  }
  return out;
}

// ---------- Backup saves / restore / debug bundle zip helpers ----------
void zip_add_dir_capped(mz_zip_archive* zip, const std::string& dir,
                        const std::string& prefix, uint64_t max_file_bytes,
                        uint64_t* total_added, uint64_t max_total_bytes,
                        std::string* manifest) {
  if (!dir_exists(dir)) return;
  std::vector<file_item> files;
  collect_files_rec(dir, "", 0, &files);
  const std::string sep(1, XFER_SEP);
  for (const auto& it : files) {
    std::string rel = it.rel;
    for (char& c : rel) if (c == '\\') c = '/';
    const std::string arc = prefix + "/" + rel;
    if (it.size > max_file_bytes || *total_added + it.size > max_total_bytes) {
      if (manifest) *manifest += "SKIP " + arc + " (" + std::to_string(it.size) + " bytes)\n";
      continue;
    }
    const std::string src = dir + sep + it.rel;
    if (mz_zip_writer_add_file(zip, arc.c_str(), src.c_str(), nullptr, 0,
                               MZ_DEFAULT_COMPRESSION)) {
      *total_added += it.size;
      if (manifest) *manifest += "OK   " + arc + "\n";
    }
  }
}

void zip_add_file_if_present(mz_zip_archive* zip, const std::string& path,
                             const char* arc_name, uint64_t max_tail_bytes) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return;
  xfer_fseek(f, 0, SEEK_END);
  const int64_t sz = (int64_t)xfer_ftell(f);
  if (sz < 0) { fclose(f); return; }
  const int64_t off = (max_tail_bytes > 0 && sz > (int64_t)max_tail_bytes)
                          ? (sz - (int64_t)max_tail_bytes)
                          : 0;
  xfer_fseek(f, off, SEEK_SET);
  std::string data((size_t)(sz - off), '\0');
  const size_t n = data.empty() ? 0 : fread(&data[0], 1, data.size(), f);
  fclose(f);
  data.resize(n);
  mz_zip_writer_add_mem(zip, arc_name, data.data(), data.size(),
                        MZ_DEFAULT_COMPRESSION);
}

bool restore_saves_zip(const std::string& zip_path, const std::string& root,
                       int* out_restored, std::string* out_err) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
    *out_err = "not a valid backup zip";
    return false;
  }
  const int n = (int)mz_zip_reader_get_num_files(&zip);
  const std::string sep(1, XFER_SEP);
  int count = 0;
  for (int i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
    std::string norm;
    if (!sanitize_rel_path(st.m_filename, '/', &norm)) continue;
    const bool allowed = (norm == "settings.ini") ||
                         (norm.rfind("drive_c/", 0) == 0) ||
                         (norm.rfind("profiles/", 0) == 0);
    if (!allowed) continue;
    std::string os_rel;
    if (!sanitize_rel_path(norm, XFER_SEP, &os_rel)) continue;
    const std::string dest = root + sep + os_rel;
    const size_t sl = dest.rfind(XFER_SEP);
    if (sl != std::string::npos) mkdirs(dest.substr(0, sl));
    if (mz_zip_reader_extract_to_file(&zip, i, dest.c_str(), 0)) ++count;
  }
  mz_zip_reader_end(&zip);
  *out_restored = count;
  return true;
}

void send_all(sock_t s, const char* data, size_t len);

void send_temp_zip_download(sock_t s, const std::string& zip_path,
                            const char* dl_name) {
  FILE* f = fopen(zip_path.c_str(), "rb");
  if (!f) {
    remove(zip_path.c_str());
    return;
  }
  xfer_fseek(f, 0, SEEK_END);
  const int64_t sz = (int64_t)xfer_ftell(f);
  xfer_fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    remove(zip_path.c_str());
    return;
  }
  char hdr[512];
  int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/zip\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Content-Length: %lld\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    dl_name, (long long)sz);
  if (hl > 0) send_all(s, hdr, (size_t)hl);
  char buf[65536];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    send_all(s, buf, n);
  }
  fclose(f);
  remove(zip_path.c_str());
}

// ---------- JSON + lightweight config parsing ----------
std::string json_escape(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if ((uint8_t)c < 0x20) r += ' ';
    else r += c;
  }
  return r;
}

bool parse_json_str_field(const std::string& json, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t col = json.find(':', k + needle.size());
  if (col == std::string::npos) return false;
  size_t q1 = json.find('"', col + 1);
  if (q1 == std::string::npos) return false;
  std::string val;
  for (size_t i = q1 + 1; i < json.size(); ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      ++i;
      if (json[i] == 'n') val.push_back('\n');
      else if (json[i] == 'r') val.push_back('\r');
      else val.push_back(json[i]);
    } else if (json[i] == '"') {
      *out = val;
      return true;
    } else {
      val.push_back(json[i]);
    }
  }
  return false;
}

// One list drives both dashboard reads and writes, so a visible setting
// cannot quietly disappear from one side of the HTTP API.
template <typename T>
struct dashboard_setting {
  const char* key;
  T host_settings::*member;
};
constexpr dashboard_setting<bool> dashboard_flags[] = {
    {"pad_vibration", &host_settings::pad_vibration},
    {"win11_personality", &host_settings::win11_personality},
    {"write_watch", &host_settings::write_watch},
    {"steam_auth", &host_settings::steam_auth},
    {"show_qr", &host_settings::show_qr},
    {"show_fps", &host_settings::show_fps},
    {"show_sysinfo", &host_settings::show_sysinfo},
    {"quick_menu_enabled", &host_settings::quick_menu_enabled},
    {"vsync", &host_settings::vsync},
    {"shader_cache", &host_settings::shader_cache},
    {"fsb_decode", &host_settings::fsb_decode},
};
constexpr dashboard_setting<int> dashboard_numbers[] = {
    {"mouse_speed", &host_settings::mouse_speed},
    {"wheel_speed", &host_settings::wheel_speed},
    {"os_major", &host_settings::os_major},
    {"os_minor", &host_settings::os_minor},
    {"os_build", &host_settings::os_build},
    {"log_lines", &host_settings::log_lines},
    {"frame_rate_cap", &host_settings::frame_rate_cap},
    {"resolution_scale", &host_settings::resolution_scale},
    {"memory_budget_mb", &host_settings::memory_budget_mb},
    {"input_deadzone", &host_settings::input_deadzone},
    {"audio_buffer_ms", &host_settings::audio_buffer_ms},
    {"texture_quality", &host_settings::texture_quality},
};
constexpr dashboard_setting<std::string> dashboard_text[] = {
    {"default_input", &host_settings::default_input},
    {"network_games_root", &host_settings::network_games_root},
};

std::string dashboard_settings_json(const host_settings& settings) {
  std::string json = "{";
  auto add = [&](const char* key, const std::string& encoded) {
    if (json.size() > 1) json += ',';
    json += '"';
    json += key;
    json += "\":";
    json += encoded;
  };
  for (const auto& entry : dashboard_flags)
    add(entry.key, settings.*entry.member ? "true" : "false");
  for (const auto& entry : dashboard_numbers)
    add(entry.key, std::to_string(settings.*entry.member));
  for (const auto& entry : dashboard_text)
    add(entry.key, '"' + json_escape(settings.*entry.member) + '"');
  return json + '}';
}

void apply_dashboard_settings(const std::string& body, host_settings& settings) {
  auto value = [&](const char* key) -> const char* {
    const std::string name = std::string("\"") + key + '"';
    const auto pos = body.find(name);
    if (pos == std::string::npos) return "";
    const auto colon = body.find_first_not_of(" \t\r\n", pos + name.size());
    if (colon == std::string::npos || body[colon] != ':') return "";
    const auto start = body.find_first_not_of(" \t\r\n", colon + 1);
    return start == std::string::npos ? "" : body.c_str() + start;
  };
  auto ends_value = [](char c) {
    return c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  for (const auto& entry : dashboard_flags) {
    const char* raw = value(entry.key);
    if (strncmp(raw, "true", 4) == 0 && ends_value(raw[4])) settings.*entry.member = true;
    else if (strncmp(raw, "false", 5) == 0 && ends_value(raw[5])) settings.*entry.member = false;
  }
  for (const auto& entry : dashboard_numbers) {
    const char* raw = value(entry.key);
    char* end = nullptr;
    const long parsed = strtol(raw, &end, 10);
    if (end != raw && ends_value(*end))
      settings.*entry.member = static_cast<int>(std::clamp(parsed, -100000L, 100000L));
  }
  for (const auto& entry : dashboard_text)
    parse_json_str_field(body, entry.key, &(settings.*entry.member));
}


std::string games_json() {
  const auto roots = game_roots();
  const std::string sep(1, XFER_SEP);
  std::set<std::string> seen;
  std::string j = "[";
  bool first = true;
  for (const auto& root : roots) {
    std::vector<dir_ent> ents;
    list_dir_entries(root, &ents);
    std::sort(ents.begin(), ents.end(), [](const dir_ent& a, const dir_ent& b) {
      return a.name < b.name;
    });
    for (const auto& d : ents) {
      if (!d.is_dir || !seen.insert(xfer_lower(d.name)).second) continue;
      const std::string gdir = root + sep + d.name;
      std::vector<std::string> exes;
      std::string exe;
      scan_game_exes(gdir, d.name, &exes, &exe);
      if (exe.empty()) continue;
      const std::string appid = read_small_text(gdir + sep + "steam_appid.txt");
      const std::string args = read_small_text(gdir + sep + "tiramisu_args.txt");
      std::string input = read_small_text(gdir + sep + "tiramisu_input.txt");
      if (input.empty()) input = "auto";
      const pe_compat_info compat = inspect_pe_compat(gdir, exe);
      if (!first) j += ",";
      first = false;
      j += "{\"name\":\"" + json_escape(d.name) + "\",\"exe\":\"" +
           json_escape(exe) + "\",\"appid\":\"" + json_escape(appid) +
           "\",\"args\":\"" + json_escape(args) + "\",\"input\":\"" +
           json_escape(input) + "\",\"arch\":\"" + json_escape(compat.arch) +
           "\",\"compat\":\"" + json_escape(compat.badge) + "\",\"compat_ok\":" +
           (compat.ok ? "true" : "false") + ",\"exes\":[";
      for (size_t i = 0; i < exes.size(); ++i) {
        if (i) j += ",";
        j += "\"" + json_escape(exes[i]) + "\"";
      }
      j += "]}";
    }
  }
  return j + "]";
}

const char* k_upload_page =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<meta name=theme-color content='#08070a'><title>Tiramisu — Xbox LAN Dashboard</title><style>"
  "*{box-sizing:border-box}"
  "button:focus-visible,a:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid #cbbab5;outline-offset:2px}"
  "body{background:#08070a;color:#f6f3f2;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
  "max-width:860px;margin:0 auto;padding:20px;min-height:100vh;"
  "background-image:radial-gradient(circle at 12% 18%,rgba(246,192,202,.16),transparent 42%),radial-gradient(circle at 88% 92%,rgba(247,188,199,.14),transparent 44%)}"
  "header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;margin-bottom:18px}"
  "h1{color:#f6f3f2;font-size:24px;margin:0;letter-spacing:-.02em}h2{font-size:15px;color:#cbbab5;margin:0 0 12px;letter-spacing:.02em;text-transform:uppercase}"
  ".card{background:linear-gradient(180deg,rgba(28,26,31,.88),rgba(19,18,22,.94));border:1px solid rgba(255,255,255,.10);border-radius:20px;padding:18px;margin-bottom:16px;box-shadow:0 16px 40px rgba(0,0,0,.55)}"
  ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
  ".btn{padding:10px 16px;min-height:42px;border:0;border-radius:999px;background:linear-gradient(180deg,#cbbab5,#bdaaa4);color:#161316;font-weight:700;cursor:pointer;font-size:14px}"
  ".btn:hover{filter:brightness(1.06)}.btn:disabled{opacity:.45;cursor:not-allowed}"
  ".btn-sec{background:rgba(255,255,255,.09);color:#f6f3f2;border:1px solid rgba(255,255,255,.12)}.btn-danger{background:#7f2230;color:#ffe2e7}"
  "input,select{background:#121116;color:#f6f3f2;border:1px solid rgba(255,255,255,.14);border-radius:12px;padding:9px 12px;font-size:14px}"
  ".drop{border:2px dashed rgba(203,186,181,.45);border-radius:16px;padding:24px 16px;text-align:center;color:#d5cecc;background:rgba(255,255,255,.03);margin-bottom:12px;transition:all .15s}"
  ".drop.over{border-color:#f6f3f2;background:rgba(203,186,181,.12)}"
  "#bar{height:10px;background:rgba(255,255,255,.12);border-radius:999px;overflow:hidden;margin:12px 0}"
  "#fill{height:100%;width:0;background:linear-gradient(90deg,#cbbab5,#f6f3f2);transition:width .15s}"
  ".game{background:rgba(255,255,255,.035);border:1px solid rgba(255,255,255,.09);border-radius:16px;padding:14px;margin-bottom:10px}"
  ".game-top{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}"
  ".game-head{display:flex;align-items:center;gap:12px;min-width:0}"
  ".cover-thumb{width:84px;height:48px;border-radius:10px;object-fit:cover;background:#16141a;border:1px solid rgba(255,255,255,.12);flex-shrink:0}"
  ".game-cfg{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}"
  "@media(max-width:560px){.game-cfg{grid-template-columns:1fr}}"
  "pre{background:#0e0d11;border:1px solid rgba(255,255,255,.09);border-radius:12px;padding:12px;max-height:260px;overflow:auto;font-size:12px;color:#d5cecc;white-space:pre-wrap}"
  ".pill{display:inline-block;font-size:12px;padding:4px 10px;border-radius:999px;background:rgba(203,186,181,.16);color:#cbbab5;border:1px solid rgba(203,186,181,.3)}"
  ".connect{display:flex;gap:18px;align-items:center;flex-wrap:wrap}"
  "#qrx img{width:136px;height:136px;border-radius:12px;background:#fff;padding:6px;display:block}"
  ".conn-info{flex:1;min-width:220px}"
  ".conn-url{font-size:18px;font-weight:700;color:#cbbab5;word-break:break-all}"
  ".conn-sub{font-size:13px;color:#9e9699;margin-top:6px}"
  ".setrow{display:grid;grid-template-columns:1fr 220px;gap:10px;align-items:center;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.08)}"
  ".setrow label{font-size:14px;color:#f6f3f2}.setrow input,.setrow select{width:100%}"
  "@media(max-width:560px){.setrow{grid-template-columns:1fr}}"
  "nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}"
  "nav a{color:#d5cecc;background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.10);padding:9px 15px;border-radius:999px;text-decoration:none;font-size:13px;font-weight:600}"
  "@media(max-width:560px){body{padding:12px}.card{padding:14px}input,select{font-size:16px}}"
  "</style></head><body>"
  "<header><div><h1>Tiramisu</h1><span class=pill id=runpill role=status>Connecting…</span></div>"
  "<div class=row><button class='btn btn-sec' onclick=rescan()>Rescan</button>"
  "<button class='btn btn-danger' onclick=stopGame()>Stop Game</button></div></header>"
  "<nav aria-label=Companion><a href='#transfer'>Transfer</a><a href='#games'>Games</a>"
  "<a href='#steam'>Steam</a><a href='#support'>Saves &amp; Support</a><a href='#settings'>Settings</a><a href='#console-logs'>Logs</a></nav>"
  "<div class=card><h2>Console connection &amp; QR</h2>"
  "<div class=connect><div id=qrx><img src='/api/qr.svg' alt='LAN QR Code' onerror=\"this.style.display='none'\"></div>"
  "<div class=conn-info><div class=conn-url id=connurl>—</div>"
  "<div class=conn-sub>Scan this QR on any phone or open the URL on your PC over the same Wi-Fi / LAN.</div>"
  "<div class=conn-sub>Remote Keyboard &amp; Mouse: <a id=ctrlLink href='/controls' style='color:#cbbab5;font-weight:700'>Open Remote KBM / Trackpad →</a></div>"
  "</div></div></div>"
  "<div class=card id=transfer><h2>Transfer Games to Xbox</h2>"
  "<div class=drop id=dz>Drag &amp; drop a <b>game folder</b>, <b>.zip archive</b>, or <b>files</b> here</div>"
  "<div class=row style='margin-bottom:10px'>"
  "<input id=gname placeholder='Game Name (auto from folder/zip)' style='flex:1;min-width:180px'>"
  "<button class=btn onclick=\"E('fzip').click()\">Upload .zip</button>"
  "<button class='btn btn-sec' onclick=openDirPicker()>Upload Folder</button>"
  "<button class='btn btn-sec' onclick=\"E('ffiles').click()\">Pick Files (Phone)</button>"
  "<input type=file id=fdir webkitdirectory directory multiple style='display:none'>"
  "<input type=file id=fzip accept='.zip,application/zip' style='display:none'>"
  "<input type=file id=ffiles multiple style='display:none'></div>"
  "<div id=dirhint style='font-size:12px;color:#9e9699;margin:-2px 0 10px'>Tip: iPhone/mobile browsers cannot upload raw folders — zip the folder first (<b>Upload .zip</b>) or use an Xbox USB drive.</div>"
  "<div class=row style='margin-bottom:6px'>"
  "<input id=dlurl placeholder='Direct .zip URL for Xbox to download itself (https://...)' style='flex:1;min-width:200px'>"
  "<button class='btn btn-sec' onclick=downloadFromUrl()>Download on Xbox</button></div>"
  "<div id=externalStorage class=conn-sub>Checking external storage...</div>"
  "<div id=bar><div id=fill></div></div>"
  "<div id=log role=status style='font-size:13px;color:#d5cecc;min-height:18px'>Ready over LAN</div></div>"
  "<dialog id=profileEdit class=card><form method=dialog><h2 id=profileTitle>Game settings</h2>"
  "<p>Settings in this file apply on the next launch for this game.</p>"
  "<textarea id=profileText rows=12 cols=46 aria-label='Per-game settings'></textarea>"
  "<div class=row><button type=button class=btn onclick=saveGameProfile()>Save</button>"
  "<button class='btn btn-sec'>Close</button></div><p id=profileResult role=status></p></form></dialog>"
  "<div class=card id=games><h2>Installed Games on Xbox</h2><div id=list><span style='color:#9e9699'>loading...</span></div></div>"
  "<div class=card id=steam><div class=game-top><h2 style='margin:0'>Steam Account &amp; Direct Downloads</h2>"
  "<div class=row><button class=btn onclick=startSteamQr()>Sign in with Steam QR</button>"
  "<button class='btn btn-sec' onclick=refreshSteam()>Refresh Steam</button>"
  "<button class='btn btn-danger' onclick=steamSignOut()>Sign Out</button></div></div>"
  "<div id=steaminfo style='margin:10px 0;font-size:13px;color:#d5cecc'>Checking Steam account...</div>"
  "<div id=steamqrwrap style='display:none;margin:10px 0'><img id=steamqrimg style='width:180px;height:180px;background:#fff;padding:8px;border-radius:14px' alt='Steam QR'></div>"
  "<label for=steamsearch>Find a Steam game</label> "
  "<input id=steamsearch type=search placeholder=\"Title or AppID\" oninput=\"renderSteam()\">"
  "<div id=steamlist></div></div>"
  "<div class=card id=support><h2>Save Backups &amp; Support Bundle</h2>"
  "<div style='font-size:13px;color:#d5cecc;margin-bottom:10px'>Dev Mode uninstalls wipe <code>LocalState</code>. Back up saves before updating Tiramisu, or download a debug zip for bug reports.</div>"
  "<div class=row>"
  "<a class=btn href='/api/backup-saves' download='tiramisu-saves-backup.zip' style='text-decoration:none;display:inline-flex;align-items:center'>Backup Saves (.zip)</a>"
  "<button class='btn btn-sec' onclick=\"E('frestore').click()\">Restore Saves (.zip)</button>"
  "<a class='btn btn-sec' href='/api/debug-bundle' download='tiramisu-debug-bundle.zip' style='text-decoration:none;display:inline-flex;align-items:center'>Download Debug Bundle (.zip)</a>"
  "<input type=file id=frestore accept='.zip,application/zip' style='display:none'></div>"
  "<div id=supstat style='margin-top:8px;font-size:13px;color:#cbbab5'></div></div>"
  "<div class=card id=settings><h2>Console Settings</h2>"
  "<div id=setwrap><span style='color:#9e9699'>loading settings...</span></div>"
  "<div class=row style='margin-top:12px'><button class=btn onclick=saveSettings()>Save Settings</button>"
  "<span id=setstat style='color:#cbbab5;font-size:13px'></span></div></div>"
  "<div class=card id=console-logs><div class=game-top><h2 style='margin:0'>Live Console Logs</h2>"
  "<div class=row><a class='btn btn-sec' href='/api/debug-bundle' download='tiramisu-debug-bundle.zip' style='text-decoration:none;display:inline-flex;align-items:center'>Debug Bundle (.zip)</a>"
  "<button class='btn btn-sec' onclick=loadLogs()>Refresh Logs</button></div></div>"
  "<pre id=logs style='margin-top:10px'>Loading logs...</pre></div>"
  "<script>"
  "const E=id=>document.getElementById(id);"
  "function setLog(s){E('log').textContent=s;}"
  "function safeRel(p){return (p||'').replace(/\\\\/g,'/').split('/').filter(Boolean).join('/');}"
  "function xhrPut(url,blob,onProg){return new Promise((res,rej)=>{const x=new XMLHttpRequest();"
  "x.open('PUT',url,true);x.timeout=0;"
  "if(x.upload&&onProg)x.upload.onprogress=e=>{if(e.lengthComputable)onProg(e.loaded,e.total);};"
  "x.onload=()=>res({ok:x.status>=200&&x.status<300,status:x.status,text:x.responseText});"
  "x.onerror=()=>rej(new Error('network error'));x.ontimeout=()=>rej(new Error('upload timeout'));"
  "x.send(blob);});}"
  "async function putWithRetry(url,blob,onProg){let lastErr;"
  "for(let a=0;a<3;a++){try{const r=await xhrPut(url,blob,onProg);if(r.ok)return r;lastErr=new Error('HTTP '+r.status+': '+r.text);}catch(e){lastErr=e;}"
  "await new Promise(r=>setTimeout(r,400*(a+1)));}throw lastErr;}"
  "async function uploadFiles(items,forcedRoot){"
  "if(!items.length)return;"
  "let g=(E('gname').value||'').trim();"
  "if(!g){g=forcedRoot||(items[0].rel.split('/')[0]||'UploadedGame');g=g.replace(/\\.zip$/i,'');E('gname').value=g;}"
  "let total=0,done=0,count=0;const inflight=new Map();const t0=Date.now();items.forEach(x=>total+=x.file.size);"
  "const paint=()=>{let curBytes=done;inflight.forEach(v=>curBytes+=v);"
  "const sec=Math.max(0.2,(Date.now()-t0)/1000);const mbps=(curBytes/1048576)/sec;"
  "const pct=total?Math.min(99,(curBytes/total)*100):Math.min(99,(count/items.length)*100);"
  "E('fill').style.width=pct.toFixed(1)+'%';"
  "setLog('Uploading '+count+'/'+items.length+' ('+(curBytes/1048576).toFixed(1)+' / '+(total/1048576).toFixed(1)+' MB @ '+mbps.toFixed(1)+' MB/s) -> '+g);};"
  "setLog('Uploading '+items.length+' file(s) ('+(total/1048576).toFixed(1)+' MB) -> '+g+'...');"
  "E('fill').style.width='1%';"
  "let idx=0;"
  "async function worker(){while(idx<items.length){const myId=idx++;const cur=items[myId];"
  "let rel=safeRel(cur.rel);const p0=rel.indexOf('/');"
  "if(!forcedRoot&&p0>0&&rel.slice(0,p0)===g)rel=rel.slice(p0+1);"
  "if(cur.stripFirst&&rel.includes('/'))rel=rel.slice(rel.indexOf('/')+1);"
  "const url='/api/upload-file?game='+encodeURIComponent(g)+'&rel='+encodeURIComponent(rel)+'&batch=1';"
  "await putWithRetry(url,cur.file,(ld)=>{inflight.set(myId,ld);paint();});"
  "inflight.delete(myId);done+=cur.file.size;count++;paint();}}"
  "try{await Promise.all([worker(),worker(),worker()]);"
  "await fetch('/api/upload-complete?game='+encodeURIComponent(g),{method:'POST'});"
  "E('fill').style.width='100%';"
  "setLog('Uploaded '+items.length+' file(s) ('+(total/1048576).toFixed(1)+' MB) to '+g+' — ready on Xbox!');refresh();}catch(e){setLog('Upload error: '+e.message);}}"
  "async function uploadZip(f){if(!f)return;"
  "let name=(E('gname').value||'').trim();"
  "let fname=name?(name.replace(/\\.zip$/i,'')+'.zip'):f.name;"
  "setLog('Uploading '+fname+' ('+(f.size/1048576).toFixed(1)+' MB)...');E('fill').style.width='2%';"
  "try{const r=await putWithRetry('/upload/'+encodeURIComponent(fname),f,(ld,tot)=>{"
  "const pct=tot?Math.min(95,(ld/tot)*95):50;E('fill').style.width=pct.toFixed(1)+'%';"
  "setLog(pct>=94?('Extracting '+fname+' on Xbox...'):('Uploading '+fname+': '+(ld/1048576).toFixed(1)+' / '+(tot/1048576).toFixed(1)+' MB'));});"
  "E('fill').style.width='100%';setLog(r.text||'Extracted cleanly!');refresh();}catch(e){setLog('Upload failed: '+e.message);}}"
  "async function downloadFromUrl(){const u=(E('dlurl').value||'').trim();if(!u)return;"
  "let g=(E('gname').value||'').trim();"
  "setLog('Starting direct Xbox download...');E('fill').style.width='5%';"
  "try{const r=await fetch('/api/download-url',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url:u,game:g})});"
  "setLog(await r.text());if(r.ok)hadDownload=true;else E('fill').style.width='0%';}"
  "catch(e){E('fill').style.width='0%';setLog('Could not reach Xbox: '+e.message);}}"
  "function openDirPicker(){"
  "const inp=E('fdir');"
  "const ios=/iPhone|iPad|iPod/i.test(navigator.userAgent);"
  "if(ios||!('webkitdirectory' in inp)){"
  "setLog('Folder upload is not supported in this browser — compress the game folder as a .zip and tap Upload .zip, or use Pick Files (Phone).');"
  "E('dirhint').style.color='#f3dbc4';return;}"
  "if(/Android|Mobile/i.test(navigator.userAgent)||(navigator.userAgentData&&navigator.userAgentData.mobile)){"
  "setLog('Opening the mobile folder picker. If it selects no files, use Upload .zip or Pick Files (Phone) instead.');}"
  "inp.click();}"
  "if(/Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent)||(navigator.userAgentData&&navigator.userAgentData.mobile)){"
  "E('dirhint').innerHTML='Mobile browser detected: folder upload may be blocked — <b>Upload .zip</b> or <b>Pick Files (Phone)</b> is the reliable path. Large games: use an Xbox USB drive.';"
  "E('dirhint').style.color='#f3dbc4';}"
  "E('fzip').onchange=e=>{if(e.target.files[0])uploadZip(e.target.files[0]);e.target.value='';};"
  "E('fdir').onchange=e=>{const fs=[...e.target.files];if(!fs.length){"
  "setLog('No files found in selected folder — zip the folder and use Upload .zip instead.');return;}"
  "const root=(fs[0].webkitRelativePath||fs[0].name).split('/')[0];"
  "uploadFiles(fs.map(f=>({file:f,rel:f.webkitRelativePath||f.name,stripFirst:true})),root);e.target.value='';};"
  "E('ffiles').onchange=e=>{const fs=[...e.target.files];if(!fs.length)return;"
  "if(fs.length===1&&/\\.zip$/i.test(fs[0].name)){uploadZip(fs[0]);}else{"
  "uploadFiles(fs.map(f=>({file:f,rel:f.name,stripFirst:false})),'');}e.target.value='';};"
  "E('frestore').onchange=async e=>{const f=e.target.files[0];e.target.value='';if(!f)return;"
  "E('supstat').textContent='Restoring saves from '+f.name+'...';"
  "try{const r=await putWithRetry('/api/restore-saves',f);E('supstat').textContent='Restore complete: '+r.text;loadSettings();}catch(err){E('supstat').textContent='Restore failed: '+err.message;}};"
  "async function walkEntry(ent,prefix,out){if(ent.isFile){"
  "const f=await new Promise(res=>ent.file(res));out.push({file:f,rel:prefix+f.name,stripFirst:false});"
  "}else if(ent.isDirectory){const rdr=ent.createReader();"
  "const all=[];while(true){const b=await new Promise(res=>rdr.readEntries(res));if(!b.length)break;all.push(...b);}"
  "for(const c of all)await walkEntry(c,prefix+ent.name+'/',out);}}"
  "const dz=E('dz');"
  "dz.ondragover=e=>{e.preventDefault();dz.classList.add('over');};"
  "dz.ondragleave=()=>dz.classList.remove('over');"
  "dz.ondrop=async e=>{e.preventDefault();dz.classList.remove('over');"
  "const items=[...(e.dataTransfer.items||[])];"
  "const ents=items.map(i=>i.webkitGetAsEntry?i.webkitGetAsEntry():null).filter(Boolean);"
  "if(ents.length===1&&ents[0].isFile&&/\\.zip$/i.test(ents[0].name)){const f=await new Promise(r=>ents[0].file(r));return uploadZip(f);}"
  "if(ents.length){const out=[];for(const en of ents)await walkEntry(en,'',out);"
  "const root=ents[0].isDirectory?ents[0].name:'';await uploadFiles(out.map(x=>({...x,stripFirst:ents[0].isDirectory})),root);}"
  "else if(e.dataTransfer.files.length){const f=e.dataTransfer.files[0];if(/\\.zip$/i.test(f.name))uploadZip(f);else uploadFiles([...e.dataTransfer.files].map(x=>({file:x,rel:x.name,stripFirst:false})),'');}};"
  "async function uploadCover(name,inp){const f=inp.files[0];if(!f)return;"
  "setLog('Uploading cover art for '+name+'...');"
  "const r=await fetch('/api/cover/'+encodeURIComponent(name),{method:'PUT',body:f});"
  "setLog(r.ok?('Updated cover art for '+name):('Cover upload failed: HTTP '+r.status));refresh();}"
  "async function launch(n,ok,badge){if(ok===false&&!confirm(n+' flagged: '+(badge||'unsupported')+'\\nTry launching anyway?'))return;setLog('Launching '+n+' — look at TV');await fetch('/launch/'+encodeURIComponent(n),{method:'POST'});refreshStatus();}"
  "async function stopGame(){setLog('Stopping active game...');await fetch('/api/stop',{method:'POST'});refreshStatus();}"
  "async function rescan(){await fetch('/api/rescan',{method:'POST'});refresh();}"
  "async function delGame(n){if(!confirm('Delete '+n+'?'))return;await fetch('/api/game/'+encodeURIComponent(n),{method:'DELETE'});refresh();}"
  "let profileGame='';"
  "async function editGameProfile(name){try{const r=await fetch('/api/game-profile/'+encodeURIComponent(name));"
  "if(!r.ok)throw Error(await r.text());profileGame=name;E('profileTitle').textContent=name+' settings';"
  "E('profileText').value=await r.text();E('profileResult').textContent='';E('profileEdit').showModal();}catch(e){setLog(e.message);}}"
  "async function saveGameProfile(){try{const r=await fetch('/api/game-profile/'+encodeURIComponent(profileGame),{method:'PUT',body:E('profileText').value});"
  "E('profileResult').textContent=await r.text();}catch(e){E('profileResult').textContent=e.message;}}"
  "async function saveCfg(idx,name){const p={exe:E('exe'+idx).value,input:E('inp'+idx).value,appid:E('app'+idx).value,args:E('arg'+idx).value};"
  "await fetch('/api/config/'+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
  "setLog('Saved config for '+name);refresh();}"
  "function esc(s){return (s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
  "async function refresh(){const g=await(await fetch('/games')).json();const el=E('list');"
  "if(!g.length){el.innerHTML='<span style=\"color:#9e9699\">No games yet — drag a game folder or .zip above</span>';return;}"
  "el.innerHTML=g.map((x,i)=>{const opts=(x.exes||[x.exe]).map(e=>'<option value=\"'+esc(e)+'\"'+(e===x.exe?' selected':'')+'>'+esc(e)+'</option>').join('');"
  "const inps=['auto','controller','kbm','pad2kbm'].map(m=>'<option value=\"'+m+'\"'+(m===x.input?' selected':'')+'>Input: '+m+'</option>').join('');"
  "const ccol=(x.compat_ok===false)?'#ff9aa8':'#b6e3c6';"
  "const cbadge=x.compat?('<span class=pill style=\"color:'+ccol+';border-color:rgba(255,255,255,.16)\">'+esc(x.compat)+'</span>'):'';"
  "return '<div class=game><div class=game-top><div class=game-head>'"
  "+'<img class=cover-thumb src=\"/api/cover/'+encodeURIComponent(x.name)+'?t='+Date.now()+'\" onerror=\"this.style.opacity=0.2\">'"
  "+'<b>'+esc(x.name)+'</b> '+cbadge+'</div><div class=row>'"
  "+'<button class=btn onclick=\"launch(\\''+esc(x.name)+'\\','+(x.compat_ok!==false)+',\\''+esc(x.compat||'')+'\\')\">Launch ▶</button>'"
  "+'<button class=\"btn btn-sec\" onclick=\"E(\\'cov'+i+'\\').click()\">Cover</button>'"
  "+'<input type=file id=cov'+i+' accept=\"image/*\" style=\"display:none\" onchange=\"uploadCover(\\''+esc(x.name)+'\\',this)\">'"
  "+'<button class=\"btn btn-sec\" data-game=\"'+esc(x.name)+'\" onclick=\"editGameProfile(this.dataset.game)\">Game settings</button>'"
  "+'<button class=\"btn btn-sec\" onclick=\"saveCfg('+i+',\\''+esc(x.name)+'\\')\">Save</button>'"
  "+'<button class=\"btn btn-danger\" onclick=\"delGame(\\''+esc(x.name)+'\\')\">Delete</button></div></div>'"
  "+'<div class=game-cfg><select id=exe'+i+'>'+opts+'</select><select id=inp'+i+'>'+inps+'</select>'"
  "+'<input id=app'+i+' placeholder=\"Steam AppID (optional)\" value=\"'+esc(x.appid)+'\">'"
  "+'<input id=arg'+i+' placeholder=\"Launch args (e.g. -windowed)\" value=\"'+esc(x.args)+'\"></div></div>';}).join('');}"
  "async function refreshStorage(){try{const r=await fetch('/api/storage');"
  "if(!r.ok)throw Error(r.status);const s=await r.json(),v=s.volumes;"
  "E('externalStorage').textContent=!v.length?(s.broker_status==='checking'?'Checking external storage...':"
  "s.broker_status==='unavailable'?'Windows removable storage unavailable; no game drive paths found':"
  "'No external game drives detected'):"
  "v.map(d=>d.name+(d.game_roots.length?' — '+d.game_roots.join(', '):"
  "' — no accessible Games or TiramisuGames folder')).join(' · ');"
  "}catch{E('externalStorage').textContent='External storage status unavailable';}}"
  "let hadDownload=false;"
  "async function refreshStatus(){try{const r=await fetch('/api/status');if(!r.ok)throw Error(r.status);const s=await r.json();"
  "E('runpill').textContent=s.running_game?('Running: '+s.running_game):'Xbox Connected';"
  "if(s.control_code&&E('ctrlLink'))E('ctrlLink').href='/controls?pin='+encodeURIComponent(s.control_code);"
  "if(s.download_phase){const pct=s.download_total?Math.min(99,s.download_done/s.download_total*100):5;"
  "E('fill').style.width=pct+'%';setLog(s.download_phase+' · '+(s.download_done/1048576).toFixed(1)+' MB');}"
  "else if(hadDownload){E('fill').style.width=s.last_error?'0%':'100%';setLog(s.last_error||s.last_upload||'Download finished');refresh();}"
  "else if(s.upload_total>0&&s.upload_done<s.upload_total){E('fill').style.width=Math.min(99,(s.upload_done/s.upload_total)*100)+'%';}"
  "hadDownload=!!s.download_phase;}catch{E('runpill').textContent='Xbox Unavailable';}}"
  "let steamLibrary=[],steamSignedIn=false;"
  "async function loadSteam(){try{const st=await(await fetch('/api/steam/status')).json();"
  "E('steaminfo').textContent=(st.signed_in?('Signed in as '+(st.persona||st.steamid64)+' · '):'')+(st.status||'Ready');"
  "if(st.qr_url){E('steamqrwrap').style.display='block';E('steamqrimg').src='/api/steam/qr.svg?t='+Date.now();}else{E('steamqrwrap').style.display='none';}"
  "steamSignedIn=st.signed_in;steamLibrary=await(await fetch('/api/steam/library')).json();renderSteam();"
  "}catch{E('steaminfo').textContent='Steam status unavailable';}}"
  "function renderSteam(){const q=E('steamsearch').value.trim().toLowerCase();"
  "const items=steamLibrary.filter(x=>x.title.toLowerCase().includes(q)||String(x.appid).includes(q));"
  "E('steamlist').innerHTML=items.length?items.map(x=>'<div class=setrow><div><b>'+esc(x.title)+'</b> <span style=\"color:#9e9699;font-size:12px\">(AppID '+x.appid+' · '+esc(x.status||x.size)+')</span></div>'"
  "+(x.installed?'<span class=pill>Installed</span>':(x.downloading||x.queued?'<button class=\"btn btn-sec\" onclick=\"cancelSteam('+x.appid+')\">'+(x.queued?'Queued':'Cancel '+Math.round((x.pct||0)*100)+'%')+'</button>':'<button class=\"btn btn-sec\" onclick=\"dlSteam('+x.appid+')\">Download to Xbox</button>'))+'</div>').join(''):(steamSignedIn?(q?'No matching Steam games.':'No Steam games loaded.'):'Sign in with Steam QR to view your library.');"
  "}"
  "async function startSteamQr(){await fetch('/api/steam/qr',{method:'POST'});setTimeout(loadSteam,1200);}"
  "async function refreshSteam(){await fetch('/api/steam/refresh',{method:'POST'});setTimeout(loadSteam,1200);}"
  "async function steamSignOut(){await fetch('/api/steam/signout',{method:'POST'});loadSteam();}"
  "async function steamJob(id,action){try{const r=await fetch('/api/steam/'+action+'/'+id,{method:'POST'});setLog(await r.text());loadSteam();}catch(e){setLog(e.message);}}"
  "function dlSteam(id){steamJob(id,'download');}"
  "function cancelSteam(id){steamJob(id,'cancel');}"
  "async function loadLogs(){try{E('logs').textContent=await(await fetch('/api/logs')).text();}catch{}}"
  "const SET_DEFS=["
  "['default_input','Default input mode','choice',['auto','controller','kbm','pad2kbm']],"
  "['mouse_speed','Pad-as-mouse speed (1-100)','int',null],"
  "['wheel_speed','Pad wheel speed (1-100)','int',null],"
  "['pad_vibration','Pad vibration (guest rumble)','bool',null],"
  "['os_major','OS reported to games — major','int',null],"
  "['os_minor','OS minor','int',null],"
  "['os_build','OS build (19045=Win10 22H2, 22000+=Win11)','int',null],"
  "['win11_personality','Win11 personality','bool',null],"
  "['write_watch','Write-watch GC (GetWriteWatch)','bool',null],"
  "['steam_auth','Steam QR link (own account)','bool',null],"
  "['show_qr','Show QR / LAN link on TV','bool',null],"
  "['show_fps','Show FPS counter','bool',null],"
  "['show_sysinfo','Show system info','bool',null],"
  "['quick_menu_enabled','Enable in-game quick menu','bool',null],"
  "['fsb_decode','Decode FSB audio in the console runtime','bool',null],"
  "['vsync','VSync','bool',null],"
  "['frame_rate_cap','Frame rate cap (0=off, 30, 60, 120)','int',null],"
  "['resolution_scale','Resolution scale % (50-200)','int',null],"
  "['memory_budget_mb','Memory budget MB (1024-8192)','int',null],"
  "['input_deadzone','Controller deadzone % (0-50)','int',null],"
  "['audio_buffer_ms','Audio buffer ms (10-50)','int',null],"
  "['shader_cache','Shader compile cache','bool',null],"
  "['texture_quality','Texture quality (0=Low, 3=Ultra)','int',null],"
  "['network_games_root','Network games folder','text',null],"
  "['log_lines','Dashboard log tail lines (4-40)','int',null]];"
  "async function loadSettings(){"
  "let s;try{s=await(await fetch('/api/settings')).json();}catch{E('setwrap').innerHTML='<span style=\"color:#a66\">settings unavailable</span>';return;}"
  "E('setwrap').innerHTML=SET_DEFS.map(d=>{const [k,lab,ty,chs]=d;const v=s[k];"
  "if(ty==='bool')return '<div class=setrow><label>'+lab+'</label><select id=set_'+k+'>'"
  "+'<option value=\"1\"'+(v?' selected':'')+'>ON</option><option value=\"0\"'+(!v?' selected':'')+'>OFF</option></select></div>';"
  "if(ty==='choice')return '<div class=setrow><label>'+lab+'</label><select id=set_'+k+'>'"
  "+chs.map(c=>'<option value=\"'+c+'\"'+(c===v?' selected':'')+'>'+c+'</option>').join('')+'</select></div>';"
  "return '<div class=setrow><label for=set_'+k+'>'+lab+'</label><input id=set_'+k+' type='+(ty==='int'?'number':'text')+' value=\"'+esc(String(v==null?'':v))+'\"></div>';}).join('');}"
  "async function saveSettings(){const p={};SET_DEFS.forEach(d=>{const k=d[0],ty=d[2];const el=E('set_'+k);if(!el)return;"
  "if(ty==='bool')p[k]=el.value==='1';else if(ty==='int'){if(el.value!=='')p[k]=Number(el.value);}else p[k]=el.value;});"
  "try{const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
  "E('setstat').textContent=r.ok?'Saved. Launch settings apply to the next game.':'Save failed (HTTP '+r.status+')';if(r.ok)loadSettings();"
  "}catch(e){E('setstat').textContent='Save failed: '+e.message;}}"
  "E('connurl').textContent=location.href.split('?')[0];"
  "loadSettings();refresh();refreshStorage();setInterval(refreshStorage,7000);refreshStatus();loadSteam();loadLogs();setInterval(refreshStatus,2000);setInterval(loadSteam,4000);setInterval(loadLogs,5000);</script></body></html>";

bool recv_exact_all(sock_t s, const char* pre, size_t pre_len, uint64_t len,
                    std::string* out) {
  out->clear();
  if (len > 1024ull * 1024ull) return false;
  out->reserve((size_t)len);
  size_t take_pre = std::min<size_t>(pre_len, (size_t)len);
  if (take_pre) out->append(pre, take_pre);
  uint64_t got = take_pre;
  char buf[4096];
  while (got < len && !g_stop) {
    int want = (int)std::min<uint64_t>(len - got, sizeof(buf));
    int n = recv(s, buf, want, 0);
    if (n <= 0) break;
    out->append(buf, (size_t)n);
    got += (uint64_t)n;
  }
  return got == len;
}

uint64_t stream_to_file(sock_t s, const char* pre, size_t pre_len,
                        const char* path, uint64_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  uint64_t got = 0;
  bool write_ok = true;
  if (pre && pre_len) {
    size_t wr = std::min<size_t>(pre_len, (size_t)std::min<uint64_t>(len, pre_len));
    if (wr) {
      if (fwrite(pre, 1, wr, f) != wr) {
        write_ok = false;  // short write (disk full): never count it
      } else {
        got += wr;
        g_upload_done += wr;
      }
    }
  }
  char buf[65536];
  while (write_ok && got < len && !g_stop) {
    const int want = (int)std::min<uint64_t>(len - got, sizeof(buf));
    const int n = recv(s, buf, want, 0);
    if (n <= 0) break;
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
      write_ok = false;
      break;
    }
    got += (uint64_t)n;
    g_upload_done += (uint64_t)n;
  }
  if (fclose(f) != 0) write_ok = false;  // buffered data lost on flush
  if (!write_ok) {
    char line[320];
    snprintf(line, sizeof(line),
             "webxfer: upload write failed for %s (disk full or I/O error)",
             path);
    log_line(line);
    return 0;  // caller removes the partial file + 500s
  }
  if (got != len) return 0;  // short read (client disconnect); caller cleans up
  // verify the bytes actually landed on disk before reporting success: a
  // truncated filesystem write must never come back ok:true
#if defined(_WIN32)
  struct _stat64 st {};
  const bool stat_ok = _stat64(path, &st) == 0;
#else
  struct stat st {};
  const bool stat_ok = stat(path, &st) == 0;
#endif
  const uint64_t disk_size = stat_ok ? (uint64_t)st.st_size : 0;
  if (!stat_ok || disk_size != len) {
    char line[384];
    snprintf(line, sizeof(line),
             "webxfer: upload verify failed for %s (on-disk %llu bytes, "
             "expected %llu) — removed",
             path, (unsigned long long)disk_size,
             (unsigned long long)len);
    log_line(line);
    remove(path);
    return 0;
  }
  return got;
}

bool stream_chunked_to_file(sock_t s, const char* pre, size_t pre_len,
                            const char* path, uint64_t* out_written) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  std::string buf(pre ? pre : "", pre_len);
  auto ensure = [&](size_t need) -> bool {
    char tmp[65536];
    while (buf.size() < need && !g_stop) {
      int n = recv(s, tmp, sizeof(tmp), 0);
      if (n <= 0) return false;
      buf.append(tmp, (size_t)n);
    }
    return buf.size() >= need;
  };
  uint64_t written = 0;
  bool ok = true;
  while (ok && !g_stop) {
    size_t crlf = buf.find("\r\n");
    while (crlf == std::string::npos) {
      char tmp[4096];
      int n = recv(s, tmp, sizeof(tmp), 0);
      if (n <= 0) { ok = false; break; }
      buf.append(tmp, (size_t)n);
      crlf = buf.find("\r\n");
      if (buf.size() > 65536 && crlf == std::string::npos) { ok = false; break; }
    }
    if (!ok) break;
    std::string line = buf.substr(0, crlf);
    buf.erase(0, crlf + 2);
    size_t semi = line.find(';');
    if (semi != std::string::npos) line.resize(semi);
    uint64_t csz = strtoull(line.c_str(), nullptr, 16);
    if (csz == 0) break;
    uint64_t rem = csz;
    while (rem > 0) {
      if (buf.empty() && !ensure(1)) { ok = false; break; }
      size_t take = (size_t)std::min<uint64_t>(rem, buf.size());
      if (fwrite(buf.data(), 1, take, f) != take) { ok = false; break; }
      buf.erase(0, take);
      rem -= take;
      written += take;
      g_upload_done += take;
    }
    if (!ok || !ensure(2)) { ok = false; break; }
    buf.erase(0, 2); // trailing CRLF
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) remove(path);
  if (out_written) *out_written = written;
  return ok;
}

void set_upload_timeout(sock_t s) {
#ifdef _WIN32
  const DWORD to_ms = 120000;  // 120s for slow Wi-Fi uploads
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_ms, sizeof(to_ms));
#else
  struct timeval to_tv = {120, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_tv, sizeof(to_tv));
#endif
}

void send_all(sock_t s, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = send(s, data + sent, (int)(len - sent), 0);
    if (n <= 0) break;
    sent += (size_t)n;
  }
}

void respond(sock_t s, const char* code, const char* ctype,
             const std::string& body) {
  // .239 DIAG: bracket send_all -- the .238 Sekiro anomaly
  // (GAP-HTTP-STATUS fired but the t+45s poll timed out,
  // handler ran, wait_ms=0) points at/after respond() /
  // send_all(). BEFORE without AFTER names a block inside
  // send_all; AFTER with ms~=30000 names the SO_SNDTIMEO
  // expiry. Throttled to one pair per 30s so /api/logs and
  // polling dashboards do not spam the log. Does NOT change
  // /api/stop behavior; does NOT touch g_mu.
  const auto resp_t0 = std::chrono::steady_clock::now();
  const bool resp_log = webxfer_throttle(s_http_respond_last_ms, 30000);
  if (resp_log) {
    char rb[192];
    snprintf(rb, sizeof(rb),
             "webxfer: GAP-HTTP-RESPOND-BEFORE code=%s bytes=%zu",
             code, body.size());
    log_line(rb);
  }
  char hdr[384];
  int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                    "Connection: close\r\n\r\n", code, ctype, body.size());
  if (hl > 0) send_all(s, hdr, (size_t)hl);
  if (!body.empty()) send_all(s, body.data(), body.size());
  if (resp_log) {
    const long long resp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - resp_t0)
            .count();
    char ra[192];
    snprintf(ra, sizeof(ra),
             "webxfer: GAP-HTTP-RESPOND-AFTER ms=%lld code=%s",
             resp_ms, code);
    log_line(ra);
  }
}

// Send one HTTP/1.1 chunked-transfer chunk (hex length, data, CRLF).
void send_chunk(sock_t s, const std::string& data) {
  if (data.empty()) return;
  char hdr[32];
  int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", data.size());
  if (hl > 0) send_all(s, hdr, (size_t)hl);
  send_all(s, data.data(), data.size());
  send_all(s, "\r\n", 2);
}

bool parse_control_events(const std::string& body, std::vector<web_control_event>* events) {
  std::istringstream input(body);
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream row(line);
    web_control_event e{};
    std::string extra;
    if (!(row >> e.kind >> e.first >> e.second) || (row >> extra) ||
        events->size() == 128) return false;
    const bool down = e.second == 0 || e.second == 1;
    switch (e.kind) {
      case 'a':
        if (e.first < 0 || e.first > 1000 || e.second < 0 || e.second > 1000) return false;
        break;
      case 'm':
        if (e.first < -2048 || e.first > 2048 || e.second < -2048 || e.second > 2048) return false;
        break;
      case 'b': if (e.first < 0 || e.first > 4 || !down) return false; break;
      case 'k': if (e.first < 8 || e.first > 254 || !down) return false; break;
      case 'w': if (e.first != 120 && e.first != -120) return false; break;
      case 't': if (e.first < 32 || e.first > 65535) return false; break;
      case 'r': break;
      default: return false;
    }
    events->push_back(e);
  }
  return true;
}

void handle_controls(sock_t s, const std::string& path, const std::string& auth,
                     const std::string& body) {
  std::string output;
  const char* status = "200 OK";
  {
    std::lock_guard<std::mutex> lock(g_control_mu);
    const auto now = control_clock::now();
    if (path == "/api/controls/pair") {
      if (now < g_control_retry) {
        status = "429 Too Many Requests";
      } else if (g_control_code.empty() || body != g_control_code) {
        status = "401 Unauthorized";
        if (++g_control_bad_codes >= 5) {
          g_control_bad_codes = 0;
          g_control_retry = now + std::chrono::minutes(1);
        }
      } else {
        unsigned char random[16]{};
        if (!control_random(random, sizeof(random))) {
          status = "503 Service Unavailable";
        } else {
          constexpr char hex[] = "0123456789abcdef";
          for (unsigned char byte : random) {
            output += hex[byte >> 4];
            output += hex[byte & 15];
          }
          g_control_token = "Bearer " + output;
          g_control_events = {{'r', 0, 0}};
          g_control_last = now;
          g_control_bad_codes = 0;
        }
      }
    } else if (g_control_token.empty() || auth != g_control_token) {
      status = "401 Unauthorized";
    } else if (path == "/api/controls/leave") {
      g_control_token.clear();
      g_control_events = {{'r', 0, 0}};
      g_control_last = {};
    } else if (path == "/api/controls/input") {
      std::vector<web_control_event> events;
      if (!parse_control_events(body, &events)) {
        status = "400 Bad Request";
      } else if (g_control_events.size() + events.size() > 512) {
        g_control_events = {{'r', 0, 0}};
        status = "429 Too Many Requests";
      } else {
        g_control_events.insert(g_control_events.end(), events.begin(), events.end());
        g_control_last = now;
      }
    } else {
      status = "404 Not Found";
    }
  }
  respond(s, status, "text/plain; charset=utf-8", output);
}

std::string read_control_body(sock_t s, const char* pre, size_t pre_len,
                              size_t size) {
  std::string body(pre, std::min(size, pre_len));
  while (body.size() < size) {
    char buf[512];
    const int count = recv(s, buf, static_cast<int>(
        std::min(sizeof(buf), size - body.size())), 0);
    if (count <= 0) break;
    body.append(buf, count);
  }
  return body;
}

void handle_client(sock_t s) {
  sock_guard sg{s};
  char req[16384];
  int rl = 0;
  int hdr_end = -1;
  while (rl < (int)sizeof(req) - 1) {
    int n = recv(s, req + rl, (int)sizeof(req) - 1 - rl, 0);
    if (n <= 0) break;
    rl += n;
    req[rl] = 0;
    const char* end = strstr(req, "\r\n\r\n");
    if (end) {
      hdr_end = (int)(end - req) + 4;
      break;
    }
  }
  if (hdr_end < 0) return;

  const char* pre = req + hdr_end;
  const size_t pre_len = (size_t)(rl - hdr_end);

  char method[16] = {}, raw_target[2048] = {};
  if (sscanf(req, "%15s %2047s", method, raw_target) != 2) {
    respond(s, "400 Bad Request", "text/plain", "malformed request");
    return;
  }

  uint64_t content_len = 0;
  bool expect_100 = false;
  bool is_chunked = false;
  std::string hdrs(req, (size_t)hdr_end);
  std::string hdrs_low = xfer_lower(hdrs);
  size_t clp = hdrs_low.find("\r\ncontent-length:");
  if (clp != std::string::npos) {
    content_len = strtoull(hdrs_low.c_str() + clp + 17, nullptr, 10);
  }
  if (hdrs_low.find("\r\nexpect: 100-continue") != std::string::npos) {
    expect_100 = true;
  }
  if (hdrs_low.find("\r\ntransfer-encoding:") != std::string::npos &&
      hdrs_low.find("chunked") != std::string::npos) {
    is_chunked = true;
  }

  if (strcmp(method, "OPTIONS") == 0) {
    respond(s, "204 No Content", "text/plain", "");
    return;
  }

  std::string target = raw_target;
  std::string url_path = target;
  std::string query;
  size_t qm = target.find('?');
  if (qm != std::string::npos) {
    url_path = target.substr(0, qm);
    query = target.substr(qm + 1);
  }

  if (strcmp(method, "GET") == 0 && url_path == "/api/qr.svg") {
#ifdef TIRAMISU_UWP_HOST
    std::string u;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      u = g.url;
    }
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!u.empty() && qrcodegen_encodeText(u.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM,
                                           qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                           qrcodegen_Mask_AUTO, true)) {
      const int sz = qrcodegen_getSize(qr);
      const int brd = 3;
      const int tot = sz + brd * 2;
      std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " +
                        std::to_string(tot) + " " + std::to_string(tot) +
                        "' shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/><path fill='#08070a' d='";
      for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
          if (qrcodegen_getModule(qr, x, y)) {
            svg += "M" + std::to_string(x + brd) + "," + std::to_string(y + brd) + "h1v1h-1z";
          }
        }
      }
      svg += "'/></svg>";
      respond(s, "200 OK", "image/svg+xml", svg);
      return;
    }
#endif
    respond(s, "404 Not Found", "text/plain", "qr unavailable");
    return;
  }

  if (strcmp(method, "GET") == 0 && url_path.rfind("/api/cover/", 0) == 0) {
#ifdef TIRAMISU_UWP_HOST
    std::string name;
    if (sanitize_game_name(url_decode(url_path.substr(11), false), &name)) {
      std::string p = cover_art_disk_path(name);
      if (!p.empty()) {
        FILE* cf = fopen(p.c_str(), "rb");
        if (cf) {
          xfer_fseek(cf, 0, SEEK_END);
          long long sz = (long long)xfer_ftell(cf);
          xfer_fseek(cf, 0, SEEK_SET);
          if (sz > 0 && sz < 8 * 1024 * 1024) {
            std::string body((size_t)sz, '\0');
            size_t n = fread(&body[0], 1, (size_t)sz, cf);
            body.resize(n);
            fclose(cf);
            const bool is_png = (n >= 4 && (unsigned char)body[0] == 0x89 && body[1] == 'P');
            respond(s, "200 OK", is_png ? "image/png" : "image/jpeg", body);
            return;
          }
          fclose(cf);
        }
      }
    }
#endif
    respond(s, "404 Not Found", "text/plain", "no cover");
    return;
  }

  if ((strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0) &&
      url_path.rfind("/api/cover/", 0) == 0) {
#ifdef TIRAMISU_UWP_HOST
    std::string name;
    if (!sanitize_game_name(url_decode(url_path.substr(11), false), &name) ||
        content_len == 0 || content_len > 8 * 1024 * 1024) {
      respond(s, "400 Bad Request", "text/plain", "bad cover request");
      return;
    }
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    std::string body;
    body.reserve((size_t)content_len);
    size_t take_pre = std::min<size_t>(pre_len, (size_t)content_len);
    if (take_pre) body.append(pre, take_pre);
    char btmp[8192];
    while (body.size() < content_len && !g_stop) {
      int n = recv(s, btmp, (int)std::min<uint64_t>(content_len - body.size(), sizeof(btmp)), 0);
      if (n <= 0) break;
      body.append(btmp, (size_t)n);
    }
    if (body.size() == content_len &&
        cover_art_save_bytes(name, (const uint8_t*)body.data(), body.size())) {
      respond(s, "200 OK", "application/json", "{\"ok\":true}");
      return;
    }
#endif
    respond(s, "400 Bad Request", "text/plain", "invalid cover image");
    return;
  }

  if (strcmp(method, "POST") == 0 && url_path == "/api/upload-complete") {
    std::string game;
    if (!sanitize_game_name(query_param(query, "game"), &game)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game");
      return;
    }
    g_upload_total = 0;
    g_upload_done = 0;
    set_upload_label("");
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.last_upload = game + " (uploaded)";
      g.last_error.clear();
      g.rescan_requested = true;
      g.rescan_game = game;
    }
    webxfer_autopilot_begin(game);
    respond(s, "200 OK", "application/json", "{\"ok\":true}");
    return;
  }

  if (strcmp(method, "POST") == 0 && url_path == "/api/download-url") {
    const auto get_header = [&](const char* name) {
      const std::string field = std::string("\r\n") + name + ":";
      const size_t start = hdrs_low.find(field);
      if (start == std::string::npos) return std::string{};
      const size_t value = start + field.size();
      return trim_ws(hdrs_low.substr(value, hdrs_low.find("\r\n", value) - value));
    };
    const std::string origin = get_header("origin");
    if (!origin.empty() && origin != "http://" + get_header("host")) {
      respond(s, "403 Forbidden", "text/plain", "Use the Tiramisu dashboard to start downloads");
      return;
    }
    std::string body;
    if (content_len == 0 || content_len > 16384 ||
        !recv_exact_all(s, pre, pre_len, content_len, &body)) {
      respond(s, "400 Bad Request", "text/plain", "Invalid download request");
      return;
    }
    std::string url, name;
    if (!parse_json_str_field(body, "url", &url) ||
        (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
      respond(s, "400 Bad Request", "text/plain", "Provide an HTTP or HTTPS URL");
      return;
    }
    parse_json_str_field(body, "game", &name);
#ifdef TIRAMISU_UWP_HOST
    std::string extension;
    try {
      const winrt::Windows::Foundation::Uri uri(winrt::to_hstring(url));
      if (uri.Host().empty() || !uri.UserName().empty() || !uri.Password().empty()) {
        respond(s, "400 Bad Request", "text/plain", "Invalid download URL");
        return;
      }
      const std::string path = winrt::to_string(uri.Path());
      const size_t slash = path.rfind('/');
      std::string filename = url_decode(path.substr(slash == std::string::npos ? 0 : slash + 1));
      const size_t dot = filename.rfind('.');
      if (dot != std::string::npos) {
        extension = xfer_lower(filename.substr(dot));
        filename.resize(dot);
      }
      if (extension != ".iso" && extension != ".img" && extension != ".bin") {
        extension = ".zip";
      }
      if (trim_ws(name).empty()) name = filename;
    } catch (...) {
      respond(s, "400 Bad Request", "text/plain", "Invalid download URL");
      return;
    }
    if (!sanitize_game_name(name, &name) || name.front() == '.' ||
        name.back() == '.') {
      respond(s, "400 Bad Request", "text/plain", "Enter a valid game name");
      return;
    }
    if (g_url_active.exchange(true)) {
      respond(s, "409 Conflict", "text/plain", "Another URL download is active");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g.last_error.clear();
      g.last_upload.clear();
      g_url_phase = "Connecting";
    }
    try {
      std::thread(download_game_url, url, name, extension, g_incoming,
                  g_games_dir).detach();
    } catch (...) {
      finish_url_download({}, "Could not start download worker");
      respond(s, "503 Service Unavailable", "text/plain", "Could not start download");
      return;
    }
    respond(s, "202 Accepted", "text/plain", "Downloading " + name + " on Xbox");
#else
    respond(s, "501 Not Implemented", "text/plain", "URL download requires Xbox UWP host");
#endif
    return;
  }

  if (strcmp(method, "GET") == 0 && url_path == "/controls") {
    respond(s, "200 OK", "text/html; charset=utf-8", k_controls_page);
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path.rfind("/api/controls/", 0) == 0) {
    // Do not allow cross-origin browser requests to operate the app.
    const auto origin = hdrs_low.find("\r\norigin:");
    const auto host = hdrs_low.find("\r\nhost:");
    if (origin != std::string::npos && host != std::string::npos) {
      const auto value = [&](size_t at, size_t field) {
        const size_t start = at + field;
        return trim_ws(hdrs_low.substr(start, hdrs_low.find("\r\n", start) - start));
      };
      if (value(origin, 9) != "http://" + value(host, 7)) {
        respond(s, "403 Forbidden", "text/plain", "");
        return;
      }
    }
    if (content_len > 2048) {
      respond(s, "413 Payload Too Large", "text/plain", "");
      return;
    }
    const auto at = hdrs_low.find("\r\nauthorization:");
    std::string auth;
    if (at != std::string::npos) {
      const size_t start = at + 16;
      auth = trim_ws(hdrs.substr(start, hdrs.find("\r\n", start) - start));
    }
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const auto body = read_control_body(s, pre, pre_len, static_cast<size_t>(content_len));
    if (body.size() != content_len) {
      respond(s, "400 Bad Request", "text/plain", "");
      return;
    }
    handle_controls(s, url_path, auth, body);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/") {
    respond(s, "200 OK", "text/html; charset=utf-8", k_upload_page);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/games") {
    // games_json() only touches g_games_dir + the filesystem — it never
    // reads g, so holding g_mu across the (slow, disk-bound) scan and the
    // socket write blocked every other handler. Build + respond lock-free.
    respond(s, "200 OK", "application/json", games_json());
    return;
  }
#ifdef TIRAMISU_UWP_HOST
  if (strcmp(method, "GET") == 0 && url_path == "/api/steam/status") {
    const auto acct = steam_client_account();
    const auto items = steam_client_items();
    char sb[1024];
    snprintf(sb, sizeof(sb),
             "{\"signed_in\":%s,\"steamid64\":\"%llu\",\"persona\":\"%s\","
             "\"status\":\"%s\",\"qr_url\":\"%s\",\"busy\":%s,\"count\":%zu}",
             acct.signed_in ? "true" : "false",
             (unsigned long long)acct.steamid64,
             json_escape(acct.persona.empty() ? acct.account_name : acct.persona).c_str(),
             json_escape(acct.status_line).c_str(),
             json_escape(acct.qr_url).c_str(),
             acct.busy ? "true" : "false", items.size());
    respond(s, "200 OK", "application/json", sb);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/steam/qr.svg") {
    const auto acct = steam_client_account();
    if (acct.qr_url.empty()) {
      respond(s, "404 Not Found", "text/plain", "no active steam qr");
      return;
    }
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(acct.qr_url.c_str(), tmp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
      respond(s, "500 Internal Server Error", "text/plain", "qr encode error");
      return;
    }
    const int sz = qrcodegen_getSize(qr);
    const int quiet = 2;
    const int vb = sz + quiet * 2;
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " +
                      std::to_string(vb) + " " + std::to_string(vb) +
                      "' shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/>"
                      "<path fill='#121011' d='";
    for (int y = 0; y < sz; ++y) {
      for (int x = 0; x < sz; ++x) {
        if (qrcodegen_getModule(qr, x, y)) {
          svg += "M" + std::to_string(x + quiet) + "," + std::to_string(y + quiet) + "h1v1h-1z";
        }
      }
    }
    svg += "'/></svg>";
    respond(s, "200 OK", "image/svg+xml", svg);
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/steam/qr") {
    steam_client_begin_qr([]() {
      std::lock_guard<std::mutex> lk(g_mu);
      g.rescan_requested = true;
      g.rescan_game.clear();
    });
    respond(s, "202 Accepted", "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/steam/refresh") {
    steam_client_refresh_remote([]() {
      std::lock_guard<std::mutex> lk(g_mu);
      g.rescan_requested = true;
      g.rescan_game.clear();
    });
    respond(s, "202 Accepted", "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/steam/signout") {
    steam_client_sign_out();
    respond(s, "200 OK", "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/steam/library") {
    const auto items = steam_client_items();
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
      if (i) out += ",";
      const auto& item = items[i];
      out += "{\"appid\":" + std::to_string(item.app_id) +
             ",\"title\":\"" + json_escape(item.title) +
             "\",\"size\":\"" + json_escape(item.size_str) +
             "\",\"installed\":" + (item.installed ? "true" : "false") +
             ",\"downloading\":" + (item.downloading ? "true" : "false") +
             ",\"queued\":" + (item.queued ? "true" : "false") +
             ",\"pct\":" + std::to_string(item.download_pct) +
             ",\"status\":\"" + json_escape(item.status_text) + "\"}";
    }
    out += "]";
    respond(s, "200 OK", "application/json", out);
    return;
  }
  if (strcmp(method, "POST") == 0 &&
      (url_path.rfind("/api/steam/download/", 0) == 0 ||
       url_path.rfind("/api/steam/cancel/", 0) == 0)) {
    const bool cancel = url_path.rfind("/api/steam/cancel/", 0) == 0;
    const std::string id = url_path.substr(cancel ? 18 : 20);
    uint64_t value = 0;
    if (id.empty() || id.size() > 10) {
      respond(s, "400 Bad Request", "text/plain", "Invalid Steam App ID");
      return;
    }
    for (const char digit : id) {
      if (digit < '0' || digit > '9') {
        respond(s, "400 Bad Request", "text/plain", "Invalid Steam App ID");
        return;
      }
      value = value * 10 + (digit - '0');
    }
    if (value == 0 || value > UINT32_MAX) {
      respond(s, "400 Bad Request", "text/plain", "Invalid Steam App ID");
      return;
    }
    const auto appid = static_cast<uint32_t>(value);
    const bool accepted = cancel ? steam_client_cancel_download(appid)
        : steam_client_download_app(appid, []() {
            std::lock_guard<std::mutex> lock(g_mu);
            g.rescan_requested = true;
            g.rescan_game.clear();
          });
    respond(s, accepted ? "202 Accepted" : "409 Conflict", "text/plain",
            accepted ? (cancel ? "Cancellation requested" : "Install requested")
                     : (cancel ? "No cancellable download" : "Could not queue install"));
    return;
  }
#endif
#ifdef TIRAMISU_UWP_HOST
  if (strcmp(method, "GET") == 0 && url_path == "/api/launch-worker-fault") {
    launch_checkpoint fault{};
    const bool available = launch_worker_fault_read(g_local_state_dir, &fault) &&
                           fault.outcome == launch_outcome::guest_fault;
    char body[128];
    snprintf(body, sizeof(body), "{\"available\":%s,\"phase\":%u,\"code\":%u}",
             available ? "true" : "false", static_cast<unsigned>(fault.phase), fault.code);
    respond(s, "200 OK", "application/json", body);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/launch-checkpoint") {
    launch_checkpoint checkpoint{};
    if (!launch_checkpoint_read(g_local_state_dir, &checkpoint)) {
      respond(s, "200 OK", "application/json", "{\"available\":false}");
      return;
    }
    char body[128];
    snprintf(body, sizeof(body),
             "{\"available\":true,\"phase\":%u,\"outcome\":%u,\"code\":%u}",
             static_cast<unsigned>(checkpoint.phase),
             static_cast<unsigned>(checkpoint.outcome), checkpoint.code);
    respond(s, "200 OK", "application/json", body);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/launch-api-tail") {
    trt_launch_api_entry entries[12]{};
    u64 calls = 0;
    const u32 count = gap_trace_launch_tail(entries, 12, &calls);
    std::string body = "{\"observed_calls\":" + std::to_string(calls) + ",\"newest_first\":[";
    for (u32 i = 0; i < count; ++i) {
      if (i) body += ',';
      body += "{\"api\":\"" + json_escape(entries[i].name) +
              "\",\"repeats\":" + std::to_string(entries[i].repeats) + "}";
    }
    body += "]}";
    respond(s, "200 OK", "application/json", body);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/render-status") {
    trt_gpu_render_stats stats{};
    trt_gpu_get_render_stats(&stats);
    trt_gpu_resize_stats resize{};
    trt_gpu_get_resize_stats(&resize);
    trt_sync_launch_stats sync{};
    trt_sync_get_launch_stats(&sync);
    const auto draw = swapchain_guest_draw_probe();
    const auto shader_input = swapchain_guest_shader_input_probe();
    char buf[2304];
    snprintf(buf, sizeof(buf),
             "{\"d3d11_device_calls\":%u,\"d3d11_swapchains\":%u,"
             "\"d3d11_get_buffer_calls\":%u,\"d3d11_present_calls\":%u,"
             "\"d3d11_successful_presents\":%u,\"d3d11_last_present_result\":%d,"
             "\"d3d11_swap_effect\":%u,\"d3d11_buffer_count\":%u,\"d3d11_stable_backbuffer\":%u,"
             "\"d3d11_width\":%u,\"d3d11_height\":%u,"
             "\"d3d11_resize_target_calls\":%u,\"d3d11_resize_target_width\":%u,"
             "\"d3d11_resize_target_height\":%u,\"d3d11_resize_target_result\":%d,"
             "\"d3d11_resize_buffer_calls\":%u,\"d3d11_resize_buffer_requested_width\":%u,"
             "\"d3d11_resize_buffer_requested_height\":%u,\"d3d11_resize_buffer_client_width\":%u,"
             "\"d3d11_resize_buffer_client_height\":%u,\"d3d11_resize_buffer_present_count\":%u,"
             "\"d3d11_resize_buffer_result\":%d,"
             "\"draw_probe_state\":%u,\"draw_probe_samples\":%u,\"draw_probe_vertices\":%llu,"
             "\"draw_probe_pixel_invocations\":%llu,\"draw_probe_result\":%d,"
             "\"surface_samples\":%u,\"guest_lit_pixels\":%u,\"host_lit_pixels\":%u,"
             "\"surface_probe_result\":%d,\"draw_probe_nonempty_samples\":%u,"
             "\"surface_guest_rtv_samples\":%u,\"surface_other_rtv_samples\":%u,"
             "\"surface_other_samples\":%u,\"surface_other_lit_pixels\":%u,"
             "\"surface_other_width\":%u,\"surface_other_height\":%u,"
             "\"surface_other_format\":%u,\"surface_other_result\":%d,"
             "\"shader_input_snapshots\":%u,\"shader_input_bound_views\":%u,"
             "\"shader_input_samples\":%u,\"shader_input_lit_pixels\":%u,"
             "\"shader_input_slot\":%u,\"shader_input_width\":%u,\"shader_input_height\":%u,"
             "\"shader_input_format\":%u,\"shader_input_result\":%d,"
             "\"sync_wait_completed\":%u,\"sync_wait_timeout\":%u,\"sync_wait_failed\":%u,"
             "\"sync_setevent_signaled\":%u,\"sync_setevent_failed\":%u,"
             "\"guest_commit_mib\":%llu,\"guest_commit_budget_mib\":%u}",
             stats.d3d11_device_calls, stats.d3d11_swapchains, stats.d3d11_get_buffer_calls,
             stats.d3d11_present_calls, stats.d3d11_successful_presents,
             stats.d3d11_last_present_result, stats.d3d11_swap_effect,
             stats.d3d11_buffer_count, stats.d3d11_stable_backbuffer, stats.d3d11_width,
             stats.d3d11_height, resize.target_calls, resize.target_width, resize.target_height,
             resize.target_result, resize.buffer_calls, resize.buffer_requested_width,
             resize.buffer_requested_height, resize.buffer_client_width,
             resize.buffer_client_height, resize.buffer_present_count, resize.buffer_result,
             draw.state, draw.samples,
             draw.vertices, draw.pixel_invocations, draw.result, draw.surface_samples,
             draw.guest_lit_pixels, draw.host_lit_pixels, draw.surface_result,
             draw.nonempty_draw_samples, draw.guest_rtv_samples, draw.other_rtv_samples,
             draw.other_surface_samples, draw.other_lit_pixels,
             draw.other_width, draw.other_height, draw.other_format, draw.other_surface_result,
             shader_input.snapshots, shader_input.bound_views, shader_input.samples,
             shader_input.lit_pixels, shader_input.slot, shader_input.width,
             shader_input.height, shader_input.format, shader_input.result,
             sync.wait_completed, sync.wait_timed_out, sync.wait_failed,
             sync.event_signaled, sync.event_failed,
             static_cast<unsigned long long>(trt_mem_budget_committed() / 1048576ULL),
             trt_mem_budget_get_mb());
    respond(s, "200 OK", "application/json", buf);
    return;
  }
#endif
  if (strcmp(method, "GET") == 0 && url_path == "/api/storage") {
    std::vector<webxfer_storage_volume> volumes;
    std::string broker_status;
    {
      std::lock_guard<std::mutex> lock(g_external_storage_mu);
      volumes = g_external_storage;
      broker_status = g_storage_broker_status;
    }
    std::string json = "{\"broker_status\":\"" + json_escape(broker_status) + "\",\"volumes\":[";
    bool first = true;
    for (const auto& volume : volumes) {
      if (!first) json += ',';
      first = false;
      json += "{\"name\":\"" + json_escape(volume.name) + "\",\"path\":\"" +
              json_escape(volume.path) + "\",\"game_roots\":[";
      for (size_t i = 0; i < volume.game_roots.size(); ++i) {
        if (i) json += ',';
        json += "\"" + json_escape(volume.game_roots[i]) + "\"";
      }
      json += "]}";
    }
    respond(s, "200 OK", "application/json", json + "]}");
    return;
  }

  if (strcmp(method, "GET") == 0 && url_path == "/api/status") {
    // .238 FIX: serve /api/status WITHOUT holding g_mu across
    // the response -- the .237 priority fix did NOT cure the
    // :9127 deafness during guest runs, and the status handler
    // was the prime g_mu suspect (it held the lock across the
    // whole snprintf+respond). Snapshot the fields under a
    // brief scoped lock, release it, then format + respond
    // lock-free. Also measures the g_mu wait: if acquiring
    // the lock ever takes >20ms, that names g_mu contention
    // as the deafness cause (GAP-STATUS-GMUWAIT). Does NOT
    // change /api/stop behavior.
    const auto t0 = std::chrono::steady_clock::now();
    bool running = false;
    std::string running_game, url, last_upload, last_error, launch_name,
        last_launch_result, last_launch_name, ap_game, ap_result;
    std::string download_phase;
    bool launch_requested = false;
    int ap_attempt = 0;
    unsigned long long upload_done = 0, upload_total = 0;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      running = g.running;
      running_game = g.running_game;
      url = g.url;
      last_upload = g.last_upload;
      last_error = g.last_error;
      download_phase = g_url_phase;
      launch_requested = g.launch_requested;
      launch_name = g.launch_name;
      last_launch_result = g.last_launch_result;
      last_launch_name = g.last_launch_name;
      ap_game = g_ap.game;
      ap_attempt = g_ap.attempt;
      ap_result = g_ap.result;
      upload_done = g_upload_done.load();
      upload_total = g_upload_total.load();
    }
    const long long wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    if (wait_ms > 20) {
      char wl[128];
      snprintf(wl, sizeof(wl),
               "webxfer: GAP-STATUS-GMUWAIT ms=%lld guest=%s", wait_ms,
               running_game.empty() ? "0" : "1");
      log_line(wl);
    }
    // .238 DIAG: prove the worker REACHED the handler during a
    // guest run (throttled) -- ACCEPT without WORKERSTART means
    // thread spawn/start is blocked; WORKERSTART without this
    // line means recv/parse is blocked; this line without a
    // response means respond/send is blocked.
    if (!running_game.empty() &&
        webxfer_throttle(s_http_status_last_ms, 30000)) {
      char hl[160];
      snprintf(hl, sizeof(hl),
               "webxfer: GAP-HTTP-STATUS guest=1 game=%s wait_ms=%lld",
               running_game.c_str(), wait_ms);
      log_line(hl);
    }
    const std::string ctrl_code = webxfer_control_code();
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"running\":%s,\"guest_running\":%s,\"running_game\":\"%s\",\"url\":\"%s\","
             "\"last_upload\":\"%s\",\"last_error\":\"%s\","
             "\"launch_pending\":%s,\"launch_name\":\"%s\","
             "\"last_launch_result\":\"%s\",\"last_launch_name\":\"%s\","
             "\"autopilot_game\":\"%s\",\"autopilot_attempt\":%d,"
             "\"autopilot_result\":\"%s\","
             "\"control_code\":\"%s\","
             "\"upload_done\":%llu,\"upload_total\":%llu,"
             "\"download_phase\":\"%s\",\"download_done\":%llu,\"download_total\":%llu}",
             running ? "true" : "false",
             // running = the server is listening; guest_running = a guest is
             // actually launched (running_game is set on launch, cleared on exit)
             !running_game.empty() ? "true" : "false",
             json_escape(running_game).c_str(), json_escape(url).c_str(),
             json_escape(last_upload).c_str(),
             json_escape(last_error).c_str(),
             launch_requested ? "true" : "false",
             json_escape(launch_name).c_str(),
             json_escape(last_launch_result).c_str(),
             json_escape(last_launch_name).c_str(),
             json_escape(ap_game).c_str(), ap_attempt, ap_result.c_str(),
             json_escape(ctrl_code).c_str(),
             upload_done, upload_total,
             json_escape(download_phase).c_str(),
             static_cast<unsigned long long>(g_url_received.load()),
             static_cast<unsigned long long>(g_url_expected.load()));
    respond(s, "200 OK", "application/json", buf);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/logs") {
    const std::string logp =
        g_local_state_dir + std::string(1, XFER_SEP) + "tiramisu.log";
    respond(s, "200 OK", "text/plain; charset=utf-8", read_log_tail(logp));
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/backup-saves") {
    const std::string sep(1, XFER_SEP);
    mkdirs(g_incoming);
    const std::string tmp_zip = g_incoming + sep + "backup-saves-" +
                                std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()) +
                                ".zip";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmp_zip.c_str(), 0)) {
      respond(s, "500 Server Error", "text/plain", "cannot create backup zip");
      return;
    }
    std::string manifest = "Tiramisu save backup\n";
    uint64_t total_added = 0;
    const uint64_t kMaxFile = 64ull * 1024 * 1024;
    const uint64_t kMaxTotal = 256ull * 1024 * 1024;
    zip_add_dir_capped(&zip, g_local_state_dir + sep + "drive_c", "drive_c",
                       kMaxFile, &total_added, kMaxTotal, &manifest);
    zip_add_dir_capped(&zip, g_local_state_dir + sep + "profiles", "profiles",
                       kMaxFile, &total_added, kMaxTotal, &manifest);
    zip_add_file_if_present(&zip, g_local_state_dir + sep + "settings.ini",
                            "settings.ini", 0);
    mz_zip_writer_add_mem(&zip, "backup-manifest.txt", manifest.data(),
                          manifest.size(), MZ_DEFAULT_COMPRESSION);
    const bool ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok) {
      remove(tmp_zip.c_str());
      respond(s, "500 Server Error", "text/plain", "backup finalize failed");
      return;
    }
    send_temp_zip_download(s, tmp_zip, "tiramisu-saves-backup.zip");
    return;
  }
  if ((strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) &&
      url_path == "/api/restore-saves") {
    set_upload_timeout(s);
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const std::string sep(1, XFER_SEP);
    mkdirs(g_incoming);
    const std::string tmp_zip = g_incoming + sep + "restore-saves-" +
                                std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()) +
                                ".zip";
    uint64_t got = 0;
    bool upload_ok = false;
    if (is_chunked) {
      upload_ok = stream_chunked_to_file(s, pre, pre_len, tmp_zip.c_str(), &got);
    } else {
      got = stream_to_file(s, pre, pre_len, tmp_zip.c_str(), content_len);
      upload_ok = (got == content_len && got > 0);
    }
    if (!upload_ok) {
      remove(tmp_zip.c_str());
      respond(s, "400 Bad Request", "text/plain", "incomplete backup upload");
      return;
    }
    int restored = 0;
    std::string err;
    const bool ok = restore_saves_zip(tmp_zip, g_local_state_dir, &restored, &err);
    remove(tmp_zip.c_str());
    if (!ok) {
      respond(s, "400 Bad Request", "text/plain", err);
      return;
    }
    settings_load(g_local_state_dir);
    char rb[96];
    snprintf(rb, sizeof(rb), "{\"ok\":true,\"restored\":%d}", restored);
    respond(s, "200 OK", "application/json", rb);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/debug-bundle") {
    const std::string sep(1, XFER_SEP);
    mkdirs(g_incoming);
    const std::string tmp_zip = g_incoming + sep + "debug-bundle-" +
                                std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()) +
                                ".zip";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmp_zip.c_str(), 0)) {
      respond(s, "500 Server Error", "text/plain", "cannot create debug zip");
      return;
    }
    zip_add_file_if_present(&zip, g_local_state_dir + sep + "tiramisu.log",
                            "tiramisu.log", 512ull * 1024);
    zip_add_file_if_present(&zip, g_local_state_dir + sep + "settings.ini",
                            "settings.ini", 64ull * 1024);
    std::vector<dir_ent> root_ents;
    list_dir_entries(g_local_state_dir, &root_ents);
    for (const auto& e : root_ents) {
      if (e.is_dir) continue;
      const std::string low = xfer_lower(e.name);
      if (low.find("crash") != std::string::npos) {
        zip_add_file_if_present(&zip, g_local_state_dir + sep + e.name,
                                e.name.c_str(), 256ull * 1024);
      }
    }
    std::string run_game, last_res, last_err;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      run_game = g.running_game;
      last_res = g.last_launch_result;
      last_err = g.last_error;
    }
    const std::string summary =
        std::string("{\"running_game\":\"") + json_escape(run_game) +
        "\",\"last_launch_result\":\"" + json_escape(last_res) +
        "\",\"last_error\":\"" + json_escape(last_err) +
        "\",\"games\":" + games_json() + "}";
    mz_zip_writer_add_mem(&zip, "bundle-summary.json", summary.data(),
                          summary.size(), MZ_DEFAULT_COMPRESSION);
    const bool ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok) {
      remove(tmp_zip.c_str());
      respond(s, "500 Server Error", "text/plain", "debug finalize failed");
      return;
    }
    send_temp_zip_download(s, tmp_zip, "tiramisu-debug-bundle.zip");
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/list-files") {
    std::string game;
    if (!sanitize_game_name(query_param(query, "game"), &game)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game name");
      return;
    }
    const std::string gdir = find_game_dir(game);
    if (gdir.empty()) {
      respond(s, "404 Not Found", "text/plain", "game not found");
      return;
    }
    std::vector<file_item> files;
    collect_files_rec(gdir, "", 0, &files);
    std::sort(files.begin(), files.end(),
              [](const file_item& a, const file_item& b) { return a.rel < b.rel; });
    // Stream the JSON array with chunked transfer encoding so a 33k-file
    // tree never needs a multi-MB contiguous body buffer.
    const char* hdr =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    send_all(s, hdr, strlen(hdr));
    std::string chunk;
    chunk.reserve(65536);
    auto flush_chunk = [&]() {
      if (!chunk.empty()) {
        send_chunk(s, chunk);
        chunk.clear();
      }
    };
    chunk += "[";
    bool first = true;
    char szbuf[32];
    for (const auto& f : files) {
      std::string rel = f.rel;
      for (char& c : rel) if (c == '\\') c = '/';
      if (!first) chunk += ",";
      first = false;
      snprintf(szbuf, sizeof(szbuf), "%llu", (unsigned long long)f.size);
      chunk += "{\"rel\":\"";
      chunk += json_escape(rel);
      chunk += "\",\"size\":";
      chunk += szbuf;
      chunk += "}";
      if (chunk.size() >= 32768) flush_chunk();
    }
    chunk += "]";
    flush_chunk();
    send_all(s, "0\r\n\r\n", 5);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path.rfind("/progress/", 0) == 0) {
    char line[64];
    uint64_t tot = g_upload_total.load();
    uint64_t dn = g_upload_done.load();
    snprintf(line, sizeof(line), "%f", tot ? (double)dn / (double)tot : 0.0);
    respond(s, "200 OK", "text/plain", line);
    return;
  }
  if (strcmp(method, "POST") == 0 &&
      (url_path.rfind("/launch/", 0) == 0 || url_path.rfind("/api/launch/", 0) == 0)) {
    const size_t off = (url_path.rfind("/api/launch/", 0) == 0) ? 12 : 8;
    std::string raw_name = url_decode(url_path.substr(off), false);
    std::string name;
    if (!sanitize_game_name(raw_name, &name)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game name");
      return;
    }
    const std::string gdir = find_game_dir(name);
    if (gdir.empty()) {
      respond(s, "404 Not Found", "text/plain", "game not found");
      return;
    }
    // Launch honesty (item 9): the old code returned 200 "launching" the
    // moment the flag was set, before anything ran — and silently
    // overwrote a pending launch or a running game. A guest already
    // running or a launch already queued is a 409; a fresh queue is 202
    // Accepted (queued, not started — /api/status reports the outcome via
    // last_launch_result once the host loop acts on it).
    std::string conflict;
    std::string conflict_name;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      if (!g.running_game.empty()) {
        conflict = "already running";
        conflict_name = g.running_game;
      } else if (g.launch_requested) {
        conflict = "launch already queued";
        conflict_name = g.launch_name;
      } else {
        g.launch_name = name;
        g.launch_requested = true;
      }
    }
    if (!conflict.empty()) {
      respond(s, "409 Conflict", "application/json",
              "{\"error\":\"" + conflict + "\",\"name\":\"" +
                  json_escape(conflict_name) + "\"}");
      return;
    }
    respond(s, "202 Accepted", "application/json",
            "{\"queued\":true,\"name\":\"" + json_escape(name) + "\"}");
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/stop") {
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.stop_requested = true;
    }
    respond(s, "200 OK", "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/rescan") {
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.rescan_requested = true;
      g.rescan_game.clear();  // manual rescan is always a full rescan
    }
    respond(s, "200 OK", "application/json", "{\"ok\":true}");
    return;
  }
  if ((strcmp(method, "DELETE") == 0 && url_path.rfind("/api/game/", 0) == 0) ||
      (strcmp(method, "POST") == 0 && url_path.rfind("/api/delete/", 0) == 0)) {
    const size_t off = (strcmp(method, "DELETE") == 0) ? 10 : 12;
    std::string name;
    if (!sanitize_game_name(url_decode(url_path.substr(off), false), &name)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game name");
      return;
    }
    const std::string gdir = g_games_dir + std::string(1, XFER_SEP) + name;
    if (!dir_exists(gdir)) {
      respond(s, "404 Not Found", "text/plain", "not found");
      return;
    }
    if (!remove_tree(gdir)) {
      respond(s, "500 Server Error", "text/plain", "delete failed");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.rescan_requested = true;
      g.rescan_game = name;  // scoped: only this game's entries vanish
    }
    respond(s, "200 OK", "application/json", "{\"deleted\":true}");
    return;
  }
  if ((strcmp(method, "GET") == 0 || strcmp(method, "PUT") == 0) &&
      url_path.rfind("/api/game-profile/", 0) == 0) {
    std::string name;
    if (!sanitize_game_name(url_decode(url_path.substr(18), false), &name)) {
      respond(s, "400 Bad Request", "text/plain", "Invalid game name");
      return;
    }
    const auto game = find_game_dir(name);
    if (game.empty()) {
      respond(s, "404 Not Found", "text/plain", "Game not found");
      return;
    }
    const auto profile = settings_title_profile_dir(g_local_state_dir, game);
    if (strcmp(method, "GET") == 0) {
      respond(s, "200 OK", "text/plain; charset=utf-8", settings_read_user_profile(profile));
      return;
    }
    std::string text;
    if (content_len > 64 * 1024 || is_chunked ||
        !recv_exact_all(s, pre, pre_len, content_len, &text) ||
        text.find('\0') != std::string::npos) {
      respond(s, "400 Bad Request", "text/plain", "Invalid or oversized settings file");
      return;
    }
    const bool saved = settings_write_user_profile(profile, text);
    respond(s, saved ? "200 OK" : "500 Server Error", "text/plain",
            saved ? "Saved. Settings apply on the next launch." : "Could not write game settings");
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path.rfind("/api/config/", 0) == 0) {
    std::string name;
    if (!sanitize_game_name(url_decode(url_path.substr(12), false), &name)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game name");
      return;
    }
    const std::string gdir = g_games_dir + std::string(1, XFER_SEP) + name;
    if (!dir_exists(gdir)) {
      respond(s, "404 Not Found", "text/plain", "game not found");
      return;
    }
    std::string body;
    if (!recv_exact_all(s, pre, pre_len, content_len, &body)) {
      respond(s, "400 Bad Request", "text/plain", "bad body");
      return;
    }
    std::string exe_v, app_v, arg_v, inp_v;
    bool has_exe = parse_json_str_field(body, "exe", &exe_v);
    bool has_app = parse_json_str_field(body, "appid", &app_v);
    bool has_arg = parse_json_str_field(body, "args", &arg_v);
    bool has_inp = parse_json_str_field(body, "input", &inp_v);
    const std::string sep(1, XFER_SEP);
    if (has_exe) {
      std::string safe_exe;
      if (!exe_v.empty() && !sanitize_rel_path(exe_v, '\\', &safe_exe)) {
        respond(s, "400 Bad Request", "text/plain", "invalid exe path");
        return;
      }
      write_or_clear_text(gdir + sep + "tiramisu_exe.txt", safe_exe);
    }
    if (has_app) {
      std::string digits;
      for (char c : app_v) if (c >= '0' && c <= '9') digits.push_back(c);
      write_or_clear_text(gdir + sep + "steam_appid.txt", digits);
    }
    if (has_arg) {
      for (char& c : arg_v) if (c == '\r' || c == '\n') c = ' ';
      write_or_clear_text(gdir + sep + "tiramisu_args.txt", arg_v);
    }
    if (has_inp) {
      std::string mode = xfer_lower(trim_ws(inp_v));
      if (mode != "controller" && mode != "kbm" && mode != "pad2kbm") mode.clear();
      write_or_clear_text(gdir + sep + "tiramisu_input.txt", mode);
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.rescan_requested = true;
      g.rescan_game = name;  // scoped: only this game's config changed
    }
    respond(s, "200 OK", "application/json", "{\"saved\":true}");
    return;
  }
  if ((strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0) &&
      url_path == "/api/upload-file") {
    std::string game_raw = query_param(query, "game");
    std::string rel_raw = query_param(query, "rel");
    std::string game, rel;
    if (!sanitize_game_name(game_raw, &game) ||
        !sanitize_rel_path(rel_raw, XFER_SEP, &rel)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game or rel path");
      return;
    }
    // Two streams writing the SAME file would interleave bytes and fail
    // verification — refuse the duplicate (409) instead of corrupting the
    // file. Different files stream concurrently (the whole point of the
    // parallel uploader). The guard erases the key on every exit path.
    {
      std::lock_guard<std::mutex> lk(g_upload_mu);
      if (!g_uploads_inflight.insert(game + "\x1f" + rel).second) {
        respond(s, "409 Conflict", "text/plain", "already uploading");
        return;
      }
    }
    inflight_guard inflight(game + "\x1f" + rel);
    set_upload_timeout(s);
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const std::string sep(1, XFER_SEP);
    const std::string gdir = g_games_dir + sep + game;
    const std::string out_path = gdir + sep + rel;
    const size_t sl = out_path.rfind(XFER_SEP);
    if (sl != std::string::npos) mkdirs(out_path.substr(0, sl));
    set_upload_label(game);
    uint64_t got = 0;
    bool upload_ok = false;
    if (is_chunked) {
      upload_ok = stream_chunked_to_file(s, pre, pre_len, out_path.c_str(), &got);
    } else {
      g_upload_total += content_len;
      got = stream_to_file(s, pre, pre_len, out_path.c_str(), content_len);
      upload_ok = (got == content_len);
    }
    if (!upload_ok) {
      remove(out_path.c_str());
      respond(s, "500 Server Error", "text/plain", "incomplete file");
      return;
    }
    const bool is_batch = (query_param(query, "batch") == "1");
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.last_upload = game + " -> " + rel;
      g.last_error.clear();
      if (!is_batch) {
        g.rescan_requested = true;
        g.rescan_game = game;
      }
    }
    if (!is_batch) {
      webxfer_autopilot_begin(game);
    }
    {
      // got == content_len here AND the on-disk size was verified equal
      char okj[64];
      snprintf(okj, sizeof(okj), "{\"ok\":true,\"size\":%llu}",
               (unsigned long long)got);
      respond(s, "200 OK", "application/json", okj);
    }
    return;
  }
  if ((strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0) &&
      url_path.rfind("/upload/", 0) == 0) {
    std::string fname = url_decode(url_path.substr(8), false);
    for (auto& c : fname) if (c == '/' || c == '\\' || c == ':') c = '_';
    if (fname.find("..") != std::string::npos || fname.empty()) {
      respond(s, "400 Bad Request", "text/plain", "bad name");
      return;
    }
    // Same in-flight guard as /api/upload-file: two zips of the same name
    // would share one incoming temp file.
    {
      std::lock_guard<std::mutex> lk(g_upload_mu);
      if (!g_uploads_inflight.insert("zip:" + fname).second) {
        respond(s, "409 Conflict", "text/plain", "already uploading");
        return;
      }
    }
    inflight_guard inflight("zip:" + fname);
    set_upload_timeout(s);
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const std::string sep(1, XFER_SEP);
    const std::string tmp = g_incoming + sep + fname;
    g_upload_total = content_len;
    g_upload_done = 0;
    set_upload_label(fname);
    uint64_t got = 0;
    bool upload_ok = false;
    if (is_chunked) {
      upload_ok = stream_chunked_to_file(s, pre, pre_len, tmp.c_str(), &got);
    } else {
      got = stream_to_file(s, pre, pre_len, tmp.c_str(), content_len);
      upload_ok = (got == content_len);
    }
    if (!upload_ok) {
      remove(tmp.c_str());
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g.last_error = "upload incomplete";
      }
      respond(s, "500 Server Error", "text/plain", "incomplete");
      g_upload_total = 0;
      g_upload_done = 0;
      set_upload_label("");
      return;
    }
    char mb[32];
    snprintf(mb, sizeof(mb), "%.1f MB", (double)got / 1048576.0);
    if (fname.size() > 4 && xfer_ieq(fname.substr(fname.size() - 4), ".zip")) {
      const size_t stem = fname.rfind('.');
      std::string game_name;
      if (!sanitize_game_name(fname.substr(0, stem), &game_name)) {
        remove(tmp.c_str());
        respond(s, "400 Bad Request", "text/plain", "bad zip name");
        g_upload_total = 0;
        g_upload_done = 0;
        return;
      }
      const std::string dest = g_games_dir + sep + game_name;
      mkdirs(dest);
      std::string note;
      const bool ok = extract_zip(tmp, dest, &note);
      remove(tmp.c_str());
      if (ok) {
        {
          std::lock_guard<std::mutex> lk(g_mu);
          g.last_upload = game_name + " (" + mb + ", " + note + ")";
          g.last_error.clear();
          g.rescan_requested = true;
          g.rescan_game = game_name;  // scoped: only this game's dir changed
        }
        webxfer_autopilot_begin(game_name);  // arm the in-console fix loop
        respond(s, "200 OK", "text/plain",
                "extracted: " + game_name + " — " + note +
                ". Hit Launch, then look at the TV.");
      } else {
        {
          std::lock_guard<std::mutex> lk(g_mu);
          g.last_error = "zip extract failed: " + note;
        }
        respond(s, "500 Server Error", "text/plain", "extract failed");
      }
    } else {
      remove(tmp.c_str());
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g.last_error = "use .zip or folder upload (/api/upload-file)";
      }
      respond(s, "400 Bad Request", "text/plain",
              "drop a game folder or upload a .zip");
    }
    g_upload_total = 0;
    g_upload_done = 0;
    return;
  }
  if (url_path == "/api/settings" && strcmp(method, "GET") == 0) {
    respond(s, "200 OK", "application/json", dashboard_settings_json(settings_get_global()));
    return;
  }
  if (url_path == "/api/settings" && strcmp(method, "POST") == 0) {
    std::string body;
    if (is_chunked || content_len > 8192 ||
        !recv_exact_all(s, pre, pre_len, content_len, &body)) {
      respond(s, "400 Bad Request", "text/plain", "Invalid settings request");
      return;
    }
    auto st = settings_get_global();
    const auto previous_network_root = st.network_games_root;
    apply_dashboard_settings(body, st);
    if (!settings_store(st, g_local_state_dir)) {
      respond(s, "500 Server Error", "text/plain", "Could not save settings");
      return;
    }
    const auto network_root = settings_get_global().network_games_root;
    webxfer_set_network_games_dir(network_root);
    if (network_root != previous_network_root) {
      std::lock_guard<std::mutex> lock(g_mu);
      g.rescan_requested = true;
      g.rescan_game.clear();
    }
    log_line("settings: saved from the web dashboard");
    respond(s, "200 OK", "application/json", "{\"saved\":true}");
    return;
  }
  respond(s, "404 Not Found", "text/plain", "not found");
}

// The HTTP listener and its workers run beside a guest that spins at ~100% CPU,
// so they ask for one notch above normal. Only the Windows host can raise its
// own priority without privileges; elsewhere this is a documented no-op rather
// than an unguarded Win32 call that fails to compile on the Linux harness.
inline void boost_current_thread() {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

void listener_thread() {
  // .237 FIX: the app server goes deaf during guest runs (both
  // games) -- the listener runs at normal priority and starves
  // while the guest spins at ~100% CPU (the watchdog shows the
  // same starvation: 10ms cadence ticks at ~100ms gaps). One
  // notch up so /api/status + /api/stop stay reachable during a
  // run; the server does milliseconds of work per request, so
  // the guest is unaffected. If :9127 still goes deaf in .237,
  // the cause is a lock (g_mu), not scheduling, and .238
  // investigates. Does NOT change /api/stop behavior.
  boost_current_thread();
  while (!g_stop) {
    sock_t c = accept(g_listener, nullptr, nullptr);
    // .238 DIAG: prove the listener ACCEPTED during a guest run
    // (throttled) -- accept without a later WORKERSTART means
    // the worker thread never started; no ACCEPT at all means
    // the listener itself is stuck.
    if (c != kBadSock && webxfer_throttle(s_http_accept_last_ms, 10000)) {
      char al[96];
      snprintf(al, sizeof(al), "webxfer: GAP-HTTP-ACCEPT workers=%d",
               (int)g_workers.load());
      log_line(al);
    }
    if (c == kBadSock) {
      if (g_stop) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    // SOCKET TIMEOUTS: a half-open client (a timed-out poller, a phone that
    // walked away) must not park a worker in recv() forever. Measured death
    // spiral: stuck workers piled up, the overflow path ran handle_client
    // INLINE on the listener, and one blocked request killed the whole
    // dashboard while the UI kept rendering. handle_client's sock_guard
    // closes the socket, so the worker only unregisters.
#ifdef _WIN32
    const DWORD to_ms = 30000;  // 30s
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_ms, sizeof(to_ms));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to_ms, sizeof(to_ms));
#else
    struct timeval to_tv = {30, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_tv, sizeof(to_tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to_tv, sizeof(to_tv));
#endif
    if (g_workers.load() < 16) {
      g_workers.fetch_add(1);
      std::thread([c]() {
        // .237 FIX: same starvation as the listener -- a worker
        // that never gets scheduled leaves its request unanswered
        // and holds a worker slot (16 cap), so a starved pool
        // refuses new connections. One notch up.
        boost_current_thread();
        // .238 DIAG: prove the worker thread STARTED during a
        // guest run (throttled) -- ACCEPT without this line
        // means thread creation/startup is blocked (loader
        // lock / heap / scheduler).
        if (webxfer_throttle(s_http_worker_last_ms, 10000)) {
          log_line("webxfer: GAP-HTTP-WORKERSTART");
        }
        handle_client(c);  // sock_guard closes c
        g_workers.fetch_sub(1);
      }).detach();
    } else {
      sock_close(c);  // busy: refuse instead of blocking the listener
    }
  }
}

std::string pick_best_lan_ip() {
  std::string best_ip = "127.0.0.1";
  int best_score = 0;

  // 1. OS outbound-route query via UDP connect (sends zero packets on wire)
  sock_t probe = socket(AF_INET, SOCK_DGRAM, 0);
  if (probe != kBadSock) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
    if (connect(probe, (sockaddr*)&dst, sizeof(dst)) == 0) {
      sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(probe, (sockaddr*)&local, &len) == 0) {
        char buf[64] = {};
        if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
          int sc = webxfer_score_ipv4(buf);
          if (sc > best_score) {
            best_score = sc;
            best_ip = buf;
          }
        }
      }
    }
    sock_close(probe);
  }

  // 2. Hostname getaddrinfo sweep for LAN interfaces
  char host[128] = {};
  if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
      for (addrinfo* p = res; p; p = p->ai_next) {
        if (!p->ai_addr || p->ai_family != AF_INET) continue;
        sockaddr_in* a = (sockaddr_in*)p->ai_addr;
        char buf[64] = {};
        if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
          int sc = webxfer_score_ipv4(buf);
          if (sc > best_score) {
            best_score = sc;
            best_ip = buf;
          }
        }
      }
      freeaddrinfo(res);
    }
  }

#if !defined(_WIN32) && defined(__linux__)
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0 && ifa) {
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
      sockaddr_in* a = (sockaddr_in*)p->ifa_addr;
      char buf[64] = {};
      if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
        int sc = webxfer_score_ipv4(buf);
        if (sc > best_score) {
          best_score = sc;
          best_ip = buf;
        }
      }
    }
    freeifaddrs(ifa);
  }
#endif

  return best_ip;
}

} // namespace

int webxfer_score_ipv4(const char* dotted) {
  if (!dotted || !dotted[0]) return 0;
  uint32_t a = 0, b = 0, c = 0, d = 0;
  if (sscanf(dotted, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
      a > 255 || b > 255 || c > 255 || d > 255) {
    return 0;
  }
  if (a == 127u || (a == 169u && b == 254u) || a == 0u) return 0;
  if (a == 192u && b == 168u) return 300;
  if (a == 10u) return 250;
  if (a == 172u && b >= 16u && b <= 31u) return 200;
  return 100;
}

void webxfer_set_external_storage(const std::vector<webxfer_storage_volume>& volumes,
                                  const std::string& broker_status) {
  std::lock_guard<std::mutex> lock(g_external_storage_mu);
  g_external_storage = volumes;
  g_storage_broker_status = broker_status;
}

void webxfer_set_package_games_dir(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_package_games_dir = path;
}

void webxfer_set_network_games_dir(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(g_network_root_mu);
    if (g_network_games_dir == path) return;
    g_network_games_dir = path;
  }
#ifdef _WIN32
  if (!path.empty()) {
    SetLastError(ERROR_SUCCESS);
    const DWORD attr = GetFileAttributesA(path.c_str());
    const DWORD attr_error =
        attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const std::string glob = path + "\\*";
    WIN32_FIND_DATAA fd{};
    SetLastError(ERROR_SUCCESS);
    HANDLE h = FindFirstFileA(glob.c_str(), &fd);
    const DWORD find_error =
        h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    unsigned entries = 0;
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
          ++entries;
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
    char msg[256];
    snprintf(msg, sizeof(msg),
             "network games probe: attr=%s attr_err=%lu find_err=%lu entries=%u",
             attr == INVALID_FILE_ATTRIBUTES ? "missing" : "ok",
             (unsigned long)attr_error, (unsigned long)find_error, entries);
    log_line(msg);
  }
#endif
}

std::string webxfer_start_port(const std::string& local_state, unsigned short port) {
  webxfer_shutdown();
#ifdef _WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::lock_guard<std::mutex> lk(g_mu);
    g.last_error = "WSAStartup failed";
    return {};
  }
#endif
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g = webxfer_state{};
    reset_web_controls();
    g_local_state_dir = local_state;
    const std::string sep(1, XFER_SEP);
    g_games_dir = local_state + sep + "games";
    g_incoming = local_state + sep + "incoming";
    mkdirs(g_games_dir);
    mkdirs(g_incoming);
  }

  g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listener == kBadSock) {
    std::lock_guard<std::mutex> lk(g_mu);
    g.last_error = "socket() failed";
    return {};
  }
  int yes = 1;
  setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(g_listener, (sockaddr*)&addr, sizeof(addr)) != 0 ||
      listen(g_listener, 64) != 0) {  // 64-deep backlog: 6 parallel upload
                                      // workers + dashboard + launch all
                                      // connect at once; 16 dropped SYNs
    sock_close(g_listener);
    g_listener = kBadSock;
    std::lock_guard<std::mutex> lk(g_mu);
    g.last_error = "bind/listen failed (port taken?)";
    return {};
  }

  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  unsigned short real_port = port;
  if (getsockname(g_listener, (sockaddr*)&bound, &blen) == 0) {
    real_port = ntohs(bound.sin_port);
  }

  g_stop = false;
  std::thread(listener_thread).detach();

  const std::string ip = pick_best_lan_ip();
  std::string url;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g.port = real_port;
    g.running = true;
    g.url = "http://" + ip + ":" + std::to_string(real_port);
    url = g.url;
  }

  char line[256];
  snprintf(line, sizeof(line), "webxfer: serving %s -> %s", url.c_str(),
           g_games_dir.c_str());
  log_line(line);
  return url;
}

std::string webxfer_start(const std::string& local_state, int display_w) {
  (void)display_w;
  return webxfer_start_port(local_state, 9127);
}

bool webxfer_find_game(const char* name, char* out_exe, size_t cap,
                       std::string* out_game_dir) {
  if (!name || !name[0] || !out_exe || cap == 0) return false;
  out_exe[0] = 0;
  std::string safe_name;
  if (!sanitize_game_name(name, &safe_name)) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string gdir = find_game_dir(safe_name);
  if (gdir.empty()) return false;
  std::vector<std::string> exes;
  std::string exe;
  scan_game_exes(gdir, safe_name, &exes, &exe);
  if (exe.empty()) return false;
  const size_t need = exe.size() + 1;
  if (need > cap) return false;
  memcpy(out_exe, exe.c_str(), need);
  if (out_game_dir) *out_game_dir = gdir;
  return true;
}

bool webxfer_poll_launch(std::string* out_name) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g.launch_requested) return false;
  g.launch_requested = false;
  if (out_name) *out_name = g.launch_name;
  return true;
}

bool webxfer_poll_stop() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g.stop_requested) return false;
  g.stop_requested = false;
  return true;
}

bool webxfer_poll_rescan(std::string* out_game) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g.rescan_requested) return false;
  g.rescan_requested = false;
  if (out_game) *out_game = g.rescan_game;
  g.rescan_game.clear();
  return true;
}

bool webxfer_poll_rescan() { return webxfer_poll_rescan(nullptr); }

void webxfer_set_running_game(const std::string& name) {
  std::lock_guard<std::mutex> lk(g_mu);
  g.running_game = name;
}

void webxfer_autopilot_begin(const std::string& game) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_ap.game = game;
  g_ap.attempt = 0;
  g_ap.result = "armed";
  ap_history("armed");
}

void webxfer_autopilot_state(std::string* game, int* attempt,
                             std::string* result) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (game) *game = g_ap.game;
  if (attempt) *attempt = g_ap.attempt;
  if (result) *result = g_ap.result;
}

void webxfer_set_launch_result(const std::string& name, const std::string& result) {
  bool evaluate = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g.last_launch_name = name;
    g.last_launch_result = result;
    // A "started" outcome means the guest was launched; the next log pull
    // (or the next host-loop tick) carries the boot evidence to match.
    if (result == "started" && !g_ap.game.empty() && name == g_ap.game)
      evaluate = true;
  }
  if (evaluate) {
    std::lock_guard<std::mutex> lk(g_mu);
    ap_evaluate_locked();
  }
}

void webxfer_shutdown() {
  g_stop = true;
  if (g_listener != kBadSock) {
    sock_close(g_listener);
    g_listener = kBadSock;
  }
  for (int i = 0; i < 50 && g_workers.load() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#ifdef _WIN32
  WSACleanup();
#endif
  std::lock_guard<std::mutex> lk(g_mu);
  g.running = false;
}

std::string webxfer_status_line() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_url_phase.empty()) {
    return g_url_phase + " · " + std::to_string(g_url_received.load() / 1048576) +
           " MB  |  " + g.url;
  }
  if (!g.last_error.empty()) return "xfer ERR: " + g.last_error + "  |  " + g.url;
  if (!g.last_upload.empty()) return "xfer: " + g.last_upload + "  |  " + g.url;
  return g.running ? ("LAN Dashboard: " + g.url) : "transfer: offline";
}

void webxfer_upload_progress(uint64_t* out_done, uint64_t* out_total,
                             std::string* out_label) {
  if (out_done) *out_done = g_upload_done.load();
  if (out_total) *out_total = g_upload_total.load();
  if (out_label) {
    std::lock_guard<std::mutex> lk(g_upload_label_mu);
    *out_label = g_upload_label;
  }
}

std::string webxfer_url() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.url;
}

unsigned short webxfer_port() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.port;
}

std::vector<web_control_event> webxfer_control_events() {
  std::lock_guard<std::mutex> lock(g_control_mu);
  if (g_control_last != control_clock::time_point{} &&
      control_clock::now() - g_control_last > std::chrono::seconds(2)) {
    g_control_last = {};
    g_control_events = {{'r', 0, 0}};
  }
  std::vector<web_control_event> events;
  events.swap(g_control_events);
  return events;
}
std::string webxfer_control_code() {
  std::lock_guard<std::mutex> lock(g_control_mu);
  return g_control_code;
}

} // namespace longan
