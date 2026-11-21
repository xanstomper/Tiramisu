// tiramisu/src/host/webxfer.cpp
// Concurrent LAN HTTP game-transfer & management server (WinSock2 on UWP, POSIX
// sockets on Linux offline tests).
// Endpoints:
//   GET  /                                      -> Mobile + Desktop Web Dashboard
//   GET  /games                                 -> JSON array of games + exes + config
//   GET  /api/status                            -> JSON live status & upload progress
//   GET  /api/logs                              -> Tail of tiramisu.log
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
#include "web_controls_page.h"
#include "host_log.h"
#include "settings.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>
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

#include "vendor/miniz.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace longan {

namespace {

std::mutex g_mu;
webxfer_state g;
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
std::string g_incoming;

std::atomic<uint64_t> g_upload_total{0};
std::atomic<uint64_t> g_upload_done{0};

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
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return "(log empty)";
  }
  long off = (sz > (long)max_bytes) ? (sz - (long)max_bytes) : 0;
  fseek(f, off, SEEK_SET);
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

bool sanitize_game_name(const std::string& raw, std::string* out) {
  std::string s = trim_ws(raw);
  if (s.empty() || s.size() > 160 || s == "." || s == "..") return false;
  if (s.find("..") != std::string::npos) return false;
  for (unsigned char c : s) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '\0' ||
        c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
      return false;
    }
  }
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
        for (unsigned char c : cur) {
          if (c < 0x20 || c == ':') return false;
        }
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

// ---------- zip extraction (miniz) ----------
bool extract_zip(const std::string& zip_path, const std::string& dest,
                 std::string* note) {
  mz_zip_archive zip{};
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
    *note = "not a valid zip";
    return false;
  }
  const int n = (int)mz_zip_reader_get_num_files(&zip);
  int extracted = 0;
  for (int i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    std::string rel;
    if (!sanitize_rel_path(st.m_filename, XFER_SEP, &rel)) continue;
    const std::string out_path = dest + std::string(1, XFER_SEP) + rel;
    const size_t slash = out_path.rfind(XFER_SEP);
    if (slash != std::string::npos) mkdirs(out_path.substr(0, slash));
    if (mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0)) {
      ++extracted;
    }
  }
  mz_zip_reader_end(&zip);
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%d files", extracted);
  *note = tmp;
  return extracted > 0;
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

std::string games_json() {
  std::vector<dir_ent> ents;
  list_dir_entries(g_games_dir, &ents);
  std::sort(ents.begin(), ents.end(), [](const dir_ent& a, const dir_ent& b) {
    return a.name < b.name;
  });
  const std::string sep(1, XFER_SEP);
  std::string j = "[";
  bool first = true;
  for (const auto& d : ents) {
    if (!d.is_dir) continue;
    const std::string gdir = g_games_dir + sep + d.name;
    std::vector<std::string> exes;
    std::string exe;
    scan_game_exes(gdir, d.name, &exes, &exe);
    if (exe.empty()) continue;
    const std::string appid = read_small_text(gdir + sep + "steam_appid.txt");
    const std::string args = read_small_text(gdir + sep + "tiramisu_args.txt");
    std::string input = read_small_text(gdir + sep + "tiramisu_input.txt");
    if (input.empty()) input = "auto";
    if (!first) j += ",";
    first = false;
    j += "{\"name\":\"" + json_escape(d.name) + "\",\"exe\":\"" +
         json_escape(exe) + "\",\"appid\":\"" + json_escape(appid) +
         "\",\"args\":\"" + json_escape(args) + "\",\"input\":\"" +
         json_escape(input) + "\",\"exes\":[";
    for (size_t i = 0; i < exes.size(); ++i) {
      if (i) j += ",";
      j += "\"" + json_escape(exes[i]) + "\"";
    }
    j += "]}";
  }
  return j + "]";
}

const char* k_upload_page =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<meta name=theme-color content='#1b120e'><title>Tiramisu Companion</title><style>"
  "*{box-sizing:border-box}button:focus-visible,a:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid #f1b477;outline-offset:2px}"
  "body{background:#1b120e;color:#fff0dc;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;"
  "max-width:780px;margin:0 auto;padding:20px}"
  "header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;margin-bottom:16px}"
  "h1{color:#e5a568;font-size:24px;margin:0}h2{font-size:15px;color:#d7b89f;margin:0 0 10px}"
  ".card{background:#2b1e17;border:1px solid #513829;border-radius:12px;padding:16px;margin-bottom:16px}"
  ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
  ".btn{padding:10px 14px;min-height:44px;border:0;border-radius:8px;background:#e5a568;color:#241812;font-weight:700;cursor:pointer;font-size:14px}"
  ".btn:hover{background:#f1b477}.btn:disabled{background:#2a3a3e;color:#789}"
  ".btn-sec{background:#423024;color:#f8e7d3}.btn-danger{background:#8b2635;color:#ffd6dc}"
  "input,select{background:#211612;color:#fff0dc;border:1px solid #614534;border-radius:8px;padding:8px 10px;font-size:14px}"
  ".drop{border:2px dashed #b47340;border-radius:10px;padding:20px;text-align:center;color:#d7b89f;background:#211612;margin-bottom:10px}"
  ".drop.over{border-color:#f1b477;background:#3c281c}"
  "#bar{height:8px;background:#483222;border-radius:4px;overflow:hidden;margin:10px 0}"
  "#fill{height:100%;width:0;background:#e5a568;transition:width .2s}"
  ".game{background:#241812;border:1px solid #553a2b;border-radius:10px;padding:12px;margin-bottom:10px}"
  ".game-top{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}"
  ".game-cfg{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}"
  "@media(max-width:560px){.game-cfg{grid-template-columns:1fr}}"
  "pre{background:#1f1510;border:1px solid #503728;border-radius:8px;padding:10px;max-height:240px;overflow:auto;font-size:12px;color:#e4c5ac;white-space:pre-wrap}"
  ".pill{font-size:12px;padding:4px 9px;border-radius:999px;background:#3f2a1d;color:#f1b477}"
  ".connect{display:flex;gap:16px;align-items:center;flex-wrap:wrap}"
  "#qrx canvas{border:8px solid #fff;border-radius:8px;background:#fff}"
  ".conn-info{flex:1;min-width:220px}"
  ".conn-url{font-size:18px;font-weight:600;color:#f1b477;word-break:break-all}"
  ".conn-sub{font-size:13px;color:#c9a88f;margin-top:6px}"
  ".setrow{display:grid;grid-template-columns:1fr 220px;gap:10px;align-items:center;padding:7px 0;border-bottom:1px solid #4b3426}"
  ".setrow label{font-size:14px;color:#f8e7d3}"
  ".setrow input,.setrow select{width:100%}"
  "@media(max-width:560px){.setrow{grid-template-columns:1fr}}"
  "nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}"
  "nav a{color:#f1b477;background:#3f2a1d;padding:10px 14px;border-radius:8px;text-decoration:none}"
  "@media(max-width:560px){body{padding:12px}.card{padding:12px}input,select{font-size:16px}}"
  "</style></head><body>"
  "<header><div><h1>Tiramisu</h1><span class=pill id=runpill role=status>Connecting…</span></div>"
  "<div class=row><button class='btn btn-sec' onclick=rescan()>Rescan</button>"
  "<button class='btn btn-danger' onclick=stopGame()>Stop Game</button></div></header>"
  "<nav aria-label=Companion><a href='#transfer'>Transfer</a><a href='#games'>Games</a>"
  "<a href='#settings'>Settings</a><a href='#console-logs'>Logs</a></nav>"
  "<div class=card><h2>Console connection</h2>"
  "<div class=connect><div id=qrx></div><div class=conn-info>"
  "<div class=conn-url id=connurl>—</div>"
  "<div class=conn-sub>Keep this device and Xbox on the same network.</div>"
  "<div class=conn-sub style='color:#c9a88f'>Open Setup on the Xbox to find its QR code.</div>"
  "</div></div></div>"
  "<div class=card id=transfer><h2>Transfer a game</h2>"
  "<div class=drop id=dz>Drag &amp; drop a <b>game folder</b> or <b>.zip</b> here, or use the buttons below</div>"
  "<div class=row style='margin-bottom:10px'>"
  "<input id=gname placeholder='Game Name (auto from folder/zip)' style='flex:1;min-width:180px'>"
  "<button class=btn onclick=\"document.getElementById('fdir').click()\">Pick Folder</button>"
  "<button class='btn btn-sec' onclick=\"document.getElementById('fzip').click()\">Pick .zip</button>"
  "<input type=file id=fdir webkitdirectory directory multiple style='display:none'>"
  "<input type=file id=fzip accept='.zip' style='display:none'></div>"
  "<div id=bar><div id=fill></div></div>"
  "<div id=log role=status style='font-size:13px;color:#c9a88f;min-height:18px'>Ready over LAN</div></div>"
  "<div class=card id=games><h2>Installed games</h2><div id=list><span style='color:#c9a88f'>loading...</span></div></div>"
  "<div class=card id=settings><h2>Console settings</h2>"
  "<div id=setwrap><span style='color:#c9a88f'>loading settings...</span></div>"
  "<div class=row style='margin-top:10px'><button class=btn onclick=saveSettings()>Save Settings</button>"
  "<span id=setstat style='color:#c9a88f;font-size:13px'></span></div></div>"
  "<div class=card id=console-logs><div class=game-top><h2 style='margin:0'>Live Console Logs</h2>"
  "<button class='btn btn-sec' onclick=loadLogs()>Refresh Logs</button></div>"
  "<pre id=logs style='margin-top:10px'>Click Refresh Logs or wait for auto-refresh...</pre></div>"
  "<script>"
  "const E=id=>document.getElementById(id);"
  "function setLog(s){E('log').textContent=s;}"
  "function safeRel(p){return p.split('/').filter(Boolean).join('/');}"
  "async function uploadFiles(items,forcedRoot){"
  "if(!items.length)return;"
  "let g=(E('gname').value||'').trim();"
  "if(!g){g=forcedRoot||(items[0].rel.split('/')[0]||'UploadedGame');g=g.replace(/\\.zip$/i,'');E('gname').value=g;}"
  "let total=0,done=0;items.forEach(x=>total+=x.file.size);"
  "setLog('Uploading '+items.length+' files ('+(total/1048576).toFixed(1)+' MB) -> '+g+'...');"
  "let idx=0;"
  "async function worker(){while(idx<items.length){const cur=items[idx++];"
  "let rel=safeRel(cur.rel);const p0=rel.indexOf('/');"
  "if(!forcedRoot&&p0>0&&rel.slice(0,p0)===g)rel=rel.slice(p0+1);"
  "if(cur.stripFirst&&rel.includes('/'))rel=rel.slice(rel.indexOf('/')+1);"
  "const url='/api/upload-file?game='+encodeURIComponent(g)+'&rel='+encodeURIComponent(rel);"
  "const r=await fetch(url,{method:'PUT',body:cur.file});"
  "if(!r.ok)throw new Error('HTTP '+r.status+' on '+rel);"
  "done+=cur.file.size;E('fill').style.width=(total?Math.min(100,done/total*100):100)+'%';}}"
  "try{await Promise.all([worker(),worker(),worker()]);"
  "setLog('Uploaded '+items.length+' files to '+g+' — ready to Launch!');refresh();}catch(e){setLog('Upload error: '+e.message);}}"
  "async function uploadZip(f){if(!f)return;"
  "setLog('Uploading '+f.name+'...');E('fill').style.width='5%';"
  "const t=setInterval(()=>{fetch('/progress/'+encodeURIComponent(f.name)).then(r=>r.text()).then(v=>{"
  "E('fill').style.width=Math.min(99,parseFloat(v)*100)+'%';});},600);"
  "try{const r=await fetch('/upload/'+encodeURIComponent(f.name),{method:'PUT',body:f});"
  "clearInterval(t);E('fill').style.width='100%';setLog(await r.text());refresh();}"
  "catch(e){clearInterval(t);setLog('Error: '+e.message);}}"
  "E('fzip').onchange=e=>{if(e.target.files[0])uploadZip(e.target.files[0]);};"
  "E('fdir').onchange=e=>{const fs=[...e.target.files];if(!fs.length)return;"
  "const root=(fs[0].webkitRelativePath||fs[0].name).split('/')[0];"
  "uploadFiles(fs.map(f=>({file:f,rel:f.webkitRelativePath||f.name,stripFirst:true})),root);};"
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
  "else if(e.dataTransfer.files.length){uploadZip(e.dataTransfer.files[0]);}};"
  "async function launch(n){setLog('Launching '+n+' — look at TV');await fetch('/launch/'+encodeURIComponent(n),{method:'POST'});refreshStatus();}"
  "async function stopGame(){setLog('Stopping active game...');await fetch('/api/stop',{method:'POST'});refreshStatus();}"
  "async function rescan(){await fetch('/api/rescan',{method:'POST'});refresh();}"
  "async function delGame(n){if(!confirm('Delete '+n+'?'))return;await fetch('/api/game/'+encodeURIComponent(n),{method:'DELETE'});refresh();}"
  "async function saveCfg(idx,name){const p={exe:E('exe'+idx).value,input:E('inp'+idx).value,appid:E('app'+idx).value,args:E('arg'+idx).value};"
  "await fetch('/api/config/'+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
  "setLog('Saved config for '+name);refresh();}"
  "function esc(s){return (s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
  "async function refresh(){const g=await(await fetch('/games')).json();const el=E('list');"
  "if(!g.length){el.innerHTML='<span style=\"color:#678\">No games yet — drag a game folder or .zip above</span>';return;}"
  "el.innerHTML=g.map((x,i)=>{const opts=(x.exes||[x.exe]).map(e=>'<option value=\"'+esc(e)+'\"'+(e===x.exe?' selected':'')+'>'+esc(e)+'</option>').join('');"
  "const inps=['auto','controller','kbm','pad2kbm'].map(m=>'<option value=\"'+m+'\"'+(m===x.input?' selected':'')+'>Input: '+m+'</option>').join('');"
  "return '<div class=game><div class=game-top><b>'+esc(x.name)+'</b><div class=row>'"
  "+'<button class=btn onclick=\"launch(\\''+esc(x.name)+'\\')\">Launch ▶</button>'"
  "+'<button class=\"btn btn-sec\" onclick=\"saveCfg('+i+',\\''+esc(x.name)+'\\')\">Save</button>'"
  "+'<button class=\"btn btn-danger\" onclick=\"delGame(\\''+esc(x.name)+'\\')\">Delete</button></div></div>'"
  "+'<div class=game-cfg><select id=exe'+i+'>'+opts+'</select><select id=inp'+i+'>'+inps+'</select>'"
  "+'<input id=app'+i+' placeholder=\"Steam AppID (optional)\" value=\"'+esc(x.appid)+'\">'"
  "+'<input id=arg'+i+' placeholder=\"Launch args (e.g. -windowed)\" value=\"'+esc(x.args)+'\"></div></div>';}).join('');}"
  "async function refreshStatus(){try{const r=await fetch('/api/status');if(!r.ok)throw Error(r.status);const s=await r.json();"
  "E('runpill').textContent=s.running_game?('Running: '+s.running_game):'Xbox connected';}catch{E('runpill').textContent='Xbox unavailable';}}"
  "async function loadLogs(){E('logs').textContent=await(await fetch('/api/logs')).text();}"
  // ---- settings editor ----
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
  "['log_lines','Dashboard log tail lines (4-40)','int',null]];"
  "async function loadSettings(){"
  "let s;try{s=await(await fetch('/api/settings')).json();}catch{E('setwrap').innerHTML='<span style=\"color:#a66\">settings unavailable</span>';return;}"
  "E('setwrap').innerHTML=SET_DEFS.map(d=>{const [k,lab,ty,chs]=d;const v=s[k];"
  "if(ty==='bool')return '<div class=setrow><label>'+lab+'</label><select id=set_'+k+'>'"
  "+'<option value=\"1\"'+(v?' selected':'')+'>ON</option><option value=\"0\"'+(!v?' selected':'')+'>OFF</option></select></div>';"
  "if(ty==='choice')return '<div class=setrow><label>'+lab+'</label><select id=set_'+k+'>"
  "+'+chs.map(c=>'<option value=\"'+c+'\"'+(c===v?' selected':'')+'>'+c+'</option>').join('')+'</select></div>';"
  "return '<div class=setrow><label>'+lab+'</label><input id=set_'+k+' type=number value=\"'+v+'\"></div>';}).join('');}"
  "async function saveSettings(){const p={};SET_DEFS.forEach(d=>{const k=d[0],ty=d[2];const el=E('set_'+k);if(!el)return;"
  "if(ty==='bool')p[k]=el.value==='1';else if(ty==='int')p[k]=parseInt(el.value||'0',10);else p[k]=el.value;});"
  "const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
  "E('setstat').textContent=r.ok?'Saved — active immediately':'Save failed (HTTP '+r.status+')';if(r.ok)setTimeout(()=>E('setstat').textContent='',4000);}"
  // ---- connect card: derive this page's own URL as the LAN link ----
  "E('connurl').textContent=location.href.split('?')[0];"
  "loadSettings();"
  "refresh();refreshStatus();setInterval(refreshStatus,2000);setInterval(loadLogs,4000);</script></body></html>";

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

// On-disk size, 64-bit on both platforms (plain stat truncates at 2 GiB
// under MSVC, and GoW texpacks exceed that).
uint64_t file_size_disk(const char* path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
  return ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
#else
  struct stat st{};
  if (stat(path, &st) != 0 || st.st_size < 0) return 0;
  return (uint64_t)st.st_size;
#endif
}

uint64_t stream_to_file(sock_t s, const char* pre, size_t pre_len,
                        const char* path, uint64_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  uint64_t got = 0;      // bytes received from the socket
  uint64_t written = 0;  // bytes fwrite actually accepted
  bool write_ok = true;
  if (pre && pre_len) {
    size_t wr = std::min<size_t>(pre_len, (size_t)std::min<uint64_t>(len, pre_len));
    if (wr) {
      size_t nwr = fwrite(pre, 1, wr, f);
      written += nwr;
      if (nwr != wr) write_ok = false;
      got += wr;
      g_upload_done += nwr;
    }
  }
  char buf[65536];
  while (got < len && !g_stop) {
    const int want = (int)std::min<uint64_t>(len - got, sizeof(buf));
    const int n = recv(s, buf, want, 0);
    if (n < 0) {
      char line[128];
      snprintf(line, sizeof(line), "trt: upload recv error got=%llu/%llu",
               (unsigned long long)got, (unsigned long long)len);
      log_line(line);
      break;
    }
    if (n == 0) break;  // orderly EOF: client went away; short read fails below
    size_t nwr = fwrite(buf, 1, (size_t)n, f);
    written += nwr;
    if (nwr != (size_t)n) write_ok = false;
    got += (uint64_t)n;
    g_upload_done += nwr;
    if (!write_ok) break;  // disk full/quota: stop reading, report honestly
  }
  if (fclose(f) != 0) write_ok = false;  // flush failure (e.g. disk full)
  // Ground truth: the on-disk size. Never trust the byte counters alone --
  // .102 reported ok:true for truncated files (fwrite/fclose ignored).
  const uint64_t on_disk = file_size_disk(path);
  if (!write_ok || got != len || on_disk != len) {
    char line[220];
    snprintf(line, sizeof(line),
             "trt: upload SHORT len=%llu got=%llu wr=%llu disk=%llu %s",
             (unsigned long long)len, (unsigned long long)got,
             (unsigned long long)written, (unsigned long long)on_disk, path);
    log_line(line);
    return on_disk;  // caller treats != len as failure (500 + removes partial)
  }
  return got;
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
  char hdr[256];
  int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n", code, ctype, body.size());
  if (hl > 0) send_all(s, hdr, (size_t)hl);
  if (!body.empty()) send_all(s, body.data(), body.size());
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
  std::string hdrs(req, (size_t)hdr_end);
  std::string hdrs_low = xfer_lower(hdrs);
  size_t clp = hdrs_low.find("\r\ncontent-length:");
  if (clp != std::string::npos) {
    content_len = strtoull(hdrs_low.c_str() + clp + 17, nullptr, 10);
  }
  if (hdrs_low.find("\r\nexpect: 100-continue") != std::string::npos) {
    expect_100 = true;
  }

  std::string target = raw_target;
  std::string url_path = target;
  std::string query;
  size_t qm = target.find('?');
  if (qm != std::string::npos) {
    url_path = target.substr(0, qm);
    query = target.substr(qm + 1);
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
    std::lock_guard<std::mutex> lk(g_mu);
    respond(s, "200 OK", "application/json", games_json());
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/status") {
    std::lock_guard<std::mutex> lk(g_mu);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"running\":%s,\"running_game\":\"%s\",\"url\":\"%s\","
             "\"last_upload\":\"%s\",\"last_error\":\"%s\","
             "\"upload_done\":%llu,\"upload_total\":%llu}",
             g.running ? "true" : "false",
             json_escape(g.running_game).c_str(),
             json_escape(g.url).c_str(),
             json_escape(g.last_upload).c_str(),
             json_escape(g.last_error).c_str(),
             (unsigned long long)g_upload_done.load(),
             (unsigned long long)g_upload_total.load());
    respond(s, "200 OK", "application/json", buf);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/logs") {
    const std::string logp =
        g_local_state_dir + std::string(1, XFER_SEP) + "tiramisu.log";
    respond(s, "200 OK", "text/plain; charset=utf-8", read_log_tail(logp));
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/api/list-files") {
    std::string game;
    if (!sanitize_game_name(query_param(query, "game"), &game)) {
      respond(s, "400 Bad Request", "text/plain", "invalid game name");
      return;
    }
    const std::string gdir = g_games_dir + std::string(1, XFER_SEP) + game;
    if (!dir_exists(gdir)) {
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
    const std::string gdir = g_games_dir + std::string(1, XFER_SEP) + name;
    if (!dir_exists(gdir)) {
      respond(s, "404 Not Found", "text/plain", "game not found");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.launch_name = name;
      g.launch_requested = true;
    }
    respond(s, "200 OK", "text/plain", "launching " + name);
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
    }
    respond(s, "200 OK", "application/json", "{\"deleted\":true}");
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
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const std::string sep(1, XFER_SEP);
    const std::string gdir = g_games_dir + sep + game;
    const std::string out_path = gdir + sep + rel;
    const size_t sl = out_path.rfind(XFER_SEP);
    if (sl != std::string::npos) mkdirs(out_path.substr(0, sl));
    g_upload_total += content_len;
    uint64_t got = stream_to_file(s, pre, pre_len, out_path.c_str(), content_len);
    if (got != content_len) {
      remove(out_path.c_str());
      respond(s, "500 Server Error", "text/plain", "incomplete file");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g.last_upload = game + " -> " + rel;
      g.last_error.clear();
      g.rescan_requested = true;
    }
    respond(s, "200 OK", "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(method, "PUT") == 0 && url_path.rfind("/upload/", 0) == 0) {
    std::string fname = url_decode(url_path.substr(8), false);
    for (auto& c : fname) if (c == '/' || c == '\\' || c == ':') c = '_';
    if (fname.find("..") != std::string::npos || fname.empty()) {
      respond(s, "400 Bad Request", "text/plain", "bad name");
      return;
    }
    if (expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const std::string sep(1, XFER_SEP);
    const std::string tmp = g_incoming + sep + fname;
    g_upload_total = content_len;
    g_upload_done = 0;
    const uint64_t got = stream_to_file(s, pre, pre_len, tmp.c_str(), content_len);
    if (got != content_len) {
      remove(tmp.c_str());
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g.last_error = "upload incomplete";
      }
      respond(s, "500 Server Error", "text/plain", "incomplete");
      g_upload_total = 0;
      g_upload_done = 0;
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
        }
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
  if (strcmp(method, "GET") == 0 && url_path == "/api/settings") {
    // settings as JSON for the dashboard editor
    const auto& s_set = settings_get();
    std::string j = "{";
    char sbuf[256];
    snprintf(sbuf, sizeof(sbuf),
             "\"default_input\":\"%s\",\"mouse_speed\":%d,"
             "\"wheel_speed\":%d,\"pad_vibration\":%s,",
             s_set.default_input.c_str(), s_set.mouse_speed, s_set.wheel_speed,
             s_set.pad_vibration ? "true" : "false");
    j += sbuf;
    snprintf(sbuf, sizeof(sbuf),
             "\"os_major\":%d,\"os_minor\":%d,\"os_build\":%d,"
             "\"win11_personality\":%s,",
             s_set.os_major, s_set.os_minor, s_set.os_build,
             s_set.win11_personality ? "true" : "false");
    j += sbuf;
    snprintf(sbuf, sizeof(sbuf),
             "\"write_watch\":%s,\"steam_auth\":%s,\"show_qr\":%s,"
             "\"log_lines\":%d}",
             s_set.write_watch ? "true" : "false",
             s_set.steam_auth ? "true" : "false",
             s_set.show_qr ? "true" : "false", s_set.log_lines);
    j += sbuf;
    respond(s, "200 OK", "application/json", j);
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path == "/api/settings") {
    // body: JSON with any subset of the setting keys; unknown keys ignored.
    // `pre` already holds bytes read with the header — drain exactly.
    std::string body;
    if (!recv_exact_all(s, pre, pre_len, content_len, &body)) {
      respond(s, "400 Bad Request", "text/plain", "bad body");
      return;
    }
    if (body.size() > 1800) {
      respond(s, "400 Bad Request", "text/plain", "body too large");
      return;
    }
    host_settings st = settings_get();
    auto get_bool = [&](const char* k, bool* v) {
      const std::string pt = std::string("\"") + k + "\":true";
      const std::string pf = std::string("\"") + k + "\":false";
      if (body.find(pt) != std::string::npos) *v = true;
      else if (body.find(pf) != std::string::npos) *v = false;
    };
    auto get_int = [&](const char* k, int* v) {
      const std::string pt = std::string("\"") + k + "\":";
      const size_t p = body.find(pt);
      if (p == std::string::npos) return;
      *v = atoi(body.c_str() + p + pt.size());
    };
    get_bool("pad_vibration", &st.pad_vibration);
    get_bool("win11_personality", &st.win11_personality);
    get_bool("write_watch", &st.write_watch);
    get_bool("steam_auth", &st.steam_auth);
    get_bool("show_qr", &st.show_qr);
    get_int("mouse_speed", &st.mouse_speed);
    get_int("wheel_speed", &st.wheel_speed);
    get_int("os_major", &st.os_major);
    get_int("os_minor", &st.os_minor);
    get_int("os_build", &st.os_build);
    get_int("log_lines", &st.log_lines);
    const std::string dip = "\"default_input\":\"";
    const size_t dp = body.find(dip);
    if (dp != std::string::npos) {
      const size_t e = body.find('"', dp + dip.size());
      if (e != std::string::npos)
        st.default_input = body.substr(dp + dip.size(), e - dp - dip.size());
    }
    settings_store(st, g_local_state_dir);
    log_line("settings: saved from the web dashboard");
    respond(s, "200 OK", "application/json", "{\"saved\":true}");
    return;
  }
  respond(s, "404 Not Found", "text/plain", "not found");
}

void listener_thread() {
  while (!g_stop) {
    sock_t c = accept(g_listener, nullptr, nullptr);
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
      listen(g_listener, 16) != 0) {
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
  std::string url = webxfer_start_port(local_state, 9127);
  if (!url.empty()) return url;
  // Keep side-by-side Tiramisu diagnostic apps from stealing the main app's
  // listener; the next fixed port is a predictable fallback for API clients.
  url = webxfer_start_port(local_state, 9128);
  return url;
}

bool webxfer_find_game(const char* name, char* out_exe, size_t cap) {
  if (!name || !name[0] || !out_exe || cap == 0) return false;
  out_exe[0] = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<dir_ent> ents;
  list_dir_entries(g_games_dir, &ents);
  const std::string sep(1, XFER_SEP);
  for (const auto& d : ents) {
    if (!d.is_dir || d.name != name) continue;
    const std::string gdir = g_games_dir + sep + d.name;
    std::vector<std::string> exes;
    std::string exe;
    scan_game_exes(gdir, d.name, &exes, &exe);
    if (exe.empty()) return false;
    const size_t need = exe.size() + 1;
    if (need > cap) return false;
    memcpy(out_exe, exe.c_str(), need);
    return true;
  }
  return false;
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

bool webxfer_poll_rescan() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g.rescan_requested) return false;
  g.rescan_requested = false;
  return true;
}

void webxfer_set_running_game(const std::string& name) {
  std::lock_guard<std::mutex> lk(g_mu);
  g.running_game = name;
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
  if (!g.last_upload.empty()) return "xfer: " + g.last_upload + "  |  " + g.url;
  if (!g.last_error.empty()) return "xfer ERR: " + g.last_error + "  |  " + g.url;
  return g.running ? ("LAN Dashboard: " + g.url) : "transfer: offline";
}

std::string webxfer_url() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.url;
}

unsigned short webxfer_port() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.port;
}

} // namespace longan
