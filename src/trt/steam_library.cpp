// tiramisu/src/trt/steam_library.cpp
// Real Steam library discovery. See steam_library.h for the why.
//
// Security note: every file this module reads is Steam's own plain-text
// metadata (libraryfolders.vdf, appmanifest_*.acf). No credential, token or
// password is ever read from, or written to, the Steam installation.
#include "steam_library.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

// ---- small filesystem helpers ---------------------------------------------

bool is_dir(const std::string& path) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool is_file(const std::string& path) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string read_file(const std::string& path, size_t cap = 4u << 20) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
    if (out.size() > cap) break;  // a manifest is ~2 KB; a VDF is smaller
  }
  fclose(f);
  return out;
}

std::string join(const std::string& a, const char* b) {
  if (a.empty()) return b;
  const char last = a[a.size() - 1];
  if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string home_dir() {
#ifdef _WIN32
  const char* p = getenv("USERPROFILE");
  return p ? p : "";
#else
  const char* p = getenv("HOME");
  return p ? p : "";
#endif
}

std::string env_dir(const char* name) {
  const char* p = getenv(name);
  return p ? p : "";
}

// Directory listing. Returns entries (names only, no separators) whose name
// ends with `suffix` (case-insensitive) when suffix is non-empty.
std::vector<std::string> list_dir(const std::string& path,
                                  const std::string& suffix) {
  std::vector<std::string> out;
#ifdef _WIN32
  std::string pattern = join(path, "*");
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    out.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(path.c_str());
  if (!d) return out;
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (!is_file(join(path, e->d_name))) continue;
    out.push_back(e->d_name);
  }
  closedir(d);
#endif
  if (!suffix.empty()) {
    const std::string tail = lower(suffix);
    std::vector<std::string> filtered;
    for (auto& n : out) {
      if (n.size() < tail.size()) continue;
      if (lower(n.substr(n.size() - tail.size())) == tail) filtered.push_back(n);
    }
    out.swap(filtered);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---- VDF tokenizer / parser ------------------------------------------------

struct vdf_tok {
  std::string text;
  bool brace_open = false;
  bool brace_close = false;
};

// Tokenize Steam's KeyValues text. Handles "quoted strings" with \\ \" escapes,
// bare words, braces and // comments. Conditional tags ("[$WIN32]") are dropped
// with the key they follow, which is correct for the blocks we read.
bool vdf_tokenize(const std::string& text, std::vector<vdf_tok>* out) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
        c == '\v') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n') ++i;
      continue;
    }
    if (c == '{') {
      vdf_tok t;
      t.brace_open = true;
      out->push_back(std::move(t));
      ++i;
      continue;
    }
    if (c == '}') {
      vdf_tok t;
      t.brace_close = true;
      out->push_back(std::move(t));
      ++i;
      continue;
    }
    if (c == '"') {
      ++i;
      vdf_tok t;
      while (i < n) {
        char ch = text[i++];
        if (ch == '\\' && i < n) {
          char e = text[i++];
          switch (e) {
            case 'n': t.text.push_back('\n'); break;
            case 't': t.text.push_back('\t'); break;
            case '\\': t.text.push_back('\\'); break;
            case '"': t.text.push_back('"'); break;
            default:
              t.text.push_back('\\');
              t.text.push_back(e);
              break;
          }
          continue;
        }
        if (ch == '"') break;
        t.text.push_back(ch);
      }
      out->push_back(std::move(t));
      continue;
    }
    // Bare word (numbers, unquoted values, conditional tags).
    size_t start = i;
    while (i < n && !isspace((unsigned char)text[i]) && text[i] != '{' &&
           text[i] != '}' && text[i] != '"') {
      ++i;
    }
    if (i == start) {  // defensive: never spin on an unexpected byte
      ++i;
      continue;
    }
    vdf_tok t;
    t.text.assign(text, start, i - start);
    out->push_back(std::move(t));
  }
  return !out->empty();
}

struct vdf_node {
  std::string key;
  std::string value;
  bool has_value = false;
  std::vector<vdf_node> kids;
};

bool vdf_parse_block(const std::vector<vdf_tok>& toks, size_t* i,
                     std::vector<vdf_node>* out, unsigned depth = 0) {
  if (depth > 64) return false;
  while (*i < toks.size()) {
    const vdf_tok& t = toks[*i];
    if (t.brace_close) {
      ++*i;
      return true;  // caller closes the block
    }
    if (t.brace_open) {  // stray brace: skip it rather than desync the walk
      ++*i;
      continue;
    }
    vdf_node node;
    node.key = t.text;
    ++*i;
    if (*i < toks.size() && toks[*i].brace_open) {
      ++*i;
      if (!vdf_parse_block(toks, i, &node.kids, depth + 1)) return false;
      node.has_value = false;
    } else if (*i < toks.size() && !toks[*i].brace_close) {
      node.value = toks[*i].text;
      node.has_value = true;
      ++*i;
    }
    out->push_back(std::move(node));
  }
  return true;
}

bool vdf_parse_document(const std::string& text, std::vector<vdf_node>* out) {
  std::vector<vdf_tok> toks;
  if (!vdf_tokenize(text, &toks)) return false;
  size_t i = 0;
  return vdf_parse_block(toks, &i, out);
}

const vdf_node* find_child(const std::vector<vdf_node>& nodes, const char* key) {
  for (const auto& n : nodes) {
    if (lower(n.key) == lower(key)) return &n;
  }
  return nullptr;
}

// The root node we care about: "libraryfolders" or "AppState" (case-insensitive).
const vdf_node* find_root(const std::vector<vdf_node>& nodes, const char* key) {
  for (const auto& n : nodes) {
    if (lower(n.key) == lower(key)) return &n;
  }
  // Legacy files wrap everything in one extra block ("LibraryFolders" or a
  // bare "libraryfolders" without children).
  for (const auto& n : nodes) {
    if (const vdf_node* deeper = find_child(n.kids, key)) return deeper;
  }
  return nullptr;
}

const vdf_node* child(const vdf_node* parent, const char* key) {
  return parent ? find_child(parent->kids, key) : nullptr;
}

std::string value(const vdf_node* node) {
  return node && node->has_value ? node->value : std::string();
}

template <class Number>
Number decimal(const std::string& text) {
  Number number = 0;
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, number);
  return result.ec == std::errc{} && result.ptr == end ? number : 0;
}

bool app_nodes(const std::string& text, std::vector<vdf_node>* roots) {
  // Some CM responses prefix the quoted KeyValues text with binary metadata.
  const size_t begin = text.find('"');
  if (begin == std::string::npos) return false;
  const size_t end = text.find_last_not_of('\0');
  return vdf_parse_document(text.substr(begin, end - begin + 1), roots);
}

bool config_allows(std::string list, const char* desired) {
  if (list.empty()) return true;
  list = lower(std::move(list));
  list.erase(std::remove_if(list.begin(), list.end(), [](unsigned char c) {
    return std::isspace(c);
  }), list.end());
  return ("," + list + ",").find(std::string(",") + desired + ",") != std::string::npos;
}

trt_steam_depot public_depot(const vdf_node& node) {
  trt_steam_depot depot{};
  depot.id = decimal<uint32_t>(node.key);
  depot.source_app = decimal<uint32_t>(value(child(&node, "depotfromapp")));
  const vdf_node* config = child(&node, "config");
  depot.optional_dlc = decimal<uint32_t>(value(child(&node, "dlcappid"))) != 0 ||
                       decimal<uint32_t>(value(child(config, "dlcappid"))) != 0;
  const vdf_node* pub = child(child(&node, "manifests"), "public");
  depot.manifest = decimal<uint64_t>(value(pub));
  if (depot.manifest == 0) depot.manifest = decimal<uint64_t>(value(child(pub, "gid")));
  depot.size = decimal<uint64_t>(value(child(pub, "size")));
  return depot;
}

// A path is a library path when it is non-empty and not Steam's bookkeeping
// (contentstatsid, time*). The modern manifest also carries "totalsize".
bool looks_like_path(const std::string& s) {
  if (s.size() < 2) return false;
  if (s.find('/') != std::string::npos) return true;
  if (s.find('\\') != std::string::npos) return true;
  if (s.size() >= 2 && s[0] == '/' ) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------

bool trt_steam_parse_app_content(const std::string& text, trt_steam_app_content* out) {
  if (!out) return false;
  *out = {};
  std::vector<vdf_node> roots;
  if (!app_nodes(text, &roots)) return false;
  const vdf_node* depots = find_root(roots, "depots");
  if (!depots) return false;
  const vdf_node* launch = child(find_root(roots, "config"), "launch");

  struct choice {
    trt_steam_depot depot;
    std::string arch;
    bool windows_specific;
    bool english_specific;
  };
  std::vector<choice> candidates;
  bool has_32 = false;
  bool has_64 = false;
  for (const auto& node : depots->kids) {
    const auto depot = public_depot(node);
    if (depot.id < 10 || depot.id == 228980 || depot.id == 228990 ||
        (!depot.manifest && !depot.source_app)) continue;
    const auto* config = child(&node, "config");
    const auto os = value(child(config, "oslist"));
    const auto language = value(child(config, "language"));
    if (!config_allows(os, "windows") || !config_allows(language, "english")) continue;
    const auto arch = value(child(config, "osarch"));
    if (!arch.empty() && !depot.optional_dlc) {
      has_32 |= config_allows(arch, "32");
      has_64 |= config_allows(arch, "64");
    }
    candidates.push_back({depot, arch, !os.empty(), !language.empty()});
  }

  const vdf_node* selected_launch = nullptr;
  std::string launch_arch;
  if (launch) {
    for (const auto& entry : launch->kids) {
      const auto* config = child(&entry, "config");
      const auto executable = lower(value(child(&entry, "executable")));
      if (executable.size() < 4 || executable.compare(executable.size() - 4, 4, ".exe") != 0 ||
          !config_allows(value(child(config, "oslist")), "windows")) continue;
      const auto arch = value(child(config, "osarch"));
      if (!config_allows(arch, "64") && !config_allows(arch, "32")) continue;
      if ((has_32 || has_64) && !arch.empty() &&
          !(has_32 && config_allows(arch, "32")) &&
          !(has_64 && config_allows(arch, "64"))) continue;
      const auto type = lower(value(child(&entry, "type")));
      if (selected_launch && !type.empty() && type != "default") continue;
      selected_launch = &entry;
      launch_arch = arch;
      if (type.empty() || type == "default") break;
    }
  }

  const char* arch = (!config_allows(launch_arch, "64") ||
                      (launch_arch.empty() && has_32 && !has_64)) ? "32" : "64";
  std::stable_sort(candidates.begin(), candidates.end(), [](const choice& a, const choice& b) {
    // A platform or language overlay takes precedence over the shared files.
    const int a_rank = a.windows_specific + 2 * a.english_specific + 4 * a.depot.optional_dlc;
    const int b_rank = b.windows_specific + 2 * b.english_specific + 4 * b.depot.optional_dlc;
    return a_rank < b_rank;
  });
  for (const auto& candidate : candidates) {
    if (config_allows(candidate.arch, arch)) out->depots.push_back(candidate.depot);
  }
  if (selected_launch) {
    out->executable = value(child(selected_launch, "executable"));
    out->arguments = value(child(selected_launch, "arguments"));
    out->working_dir = value(child(selected_launch, "workingdir"));
  }
  return !out->depots.empty();
}

bool trt_steam_find_public_depot(const std::string& text, uint32_t depot_id,
                                trt_steam_depot* out) {
  if (!out) return false;
  std::vector<vdf_node> roots;
  if (!app_nodes(text, &roots)) return false;
  const auto* node = child(find_root(roots, "depots"), std::to_string(depot_id).c_str());
  if (!node) return false;
  *out = public_depot(*node);
  return out->manifest || out->source_app;
}

bool trt_steam_normalize_content_path(const std::string& path, std::string* out) {
  if (!out || path.empty() || path.front() == '/' || path.front() == '\\') return false;
  std::string normalized;
  for (size_t start = 0; start < path.size();) {
    const size_t end = path.find_first_of("/\\", start);
    const std::string part = path.substr(start, end - start);
    if (!part.empty() && part != ".") {
      if (part.back() == '.' || part.back() == ' ' ||
          part.find_first_of(":*?\"<>|") != std::string::npos ||
          std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }))
        return false;
      std::string stem = lower(part.substr(0, part.find('.')));
      while (!stem.empty() && stem.back() == ' ') stem.pop_back();
      if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
          (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 ||
                                stem.compare(0, 3, "lpt") == 0) &&
           stem[3] >= '0' && stem[3] <= '9')) return false;
      if (!normalized.empty()) normalized += '\\';
      normalized += part;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (normalized.empty()) return false;
  *out = std::move(normalized);
  return true;
}

std::string trt_steam_depot_path_key(std::string path) {
  for (char& c : path)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return path;
}

bool trt_steam_add_depot_path(trt_steam_depot_layout& layout, const std::string& path,
                             trt_steam_depot_path_owner owner) {
  auto key = trt_steam_depot_path_key(path);
  const auto old = layout.find(key);
  if (old != layout.end() && old->second.directory != owner.directory) return false;
  layout.insert_or_assign(std::move(key), owner);
  return true;
}

bool trt_steam_validate_depot_layout(const trt_steam_depot_layout& layout,
                                    std::string* conflict) {
  for (const auto& entry : layout) {
    const auto& key = entry.first;
    for (auto slash = key.find('\\'); slash != std::string::npos;
         slash = key.find('\\', slash + 1)) {
      const auto parent = layout.find(key.substr(0, slash));
      if (parent != layout.end() && !parent->second.directory) {
        if (conflict) *conflict = parent->first;
        return false;
      }
    }
  }
  return true;
}

std::vector<std::string> trt_steam_default_roots() {
  std::vector<std::string> out;
  const std::string home = home_dir();
#ifdef _WIN32
  const std::string pf = env_dir("ProgramFiles(x86)");
  const std::string pf64 = env_dir("ProgramFiles");
  if (!pf.empty()) out.push_back(join(pf, "Steam"));
  if (!pf64.empty()) out.push_back(join(pf64, "Steam"));
  if (!env_dir("STEAM_PATH").empty()) out.push_back(env_dir("STEAM_PATH"));
  out.push_back("C:\\Program Files (x86)\\Steam");
  out.push_back("C:\\Program Files\\Steam");
#else
  if (!home.empty()) {
    out.push_back(join(home, ".local/share/Steam"));
    out.push_back(join(home, ".steam/steam"));
    out.push_back(join(home, ".steam/root"));
    out.push_back(join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam"));
  }
  out.push_back("/usr/local/share/Steam");
  if (!env_dir("STEAM_PATH").empty()) out.push_back(env_dir("STEAM_PATH"));
#endif
  // Dedupe, keep order.
  std::vector<std::string> uniq;
  for (auto& p : out) {
    if (p.empty()) continue;
    if (std::find(uniq.begin(), uniq.end(), p) == uniq.end()) uniq.push_back(p);
  }
  return uniq;
}

std::string trt_steam_libraryfolders_path(const std::string& root) {
  return join(join(root, "steamapps"), "libraryfolders.vdf");
}

int trt_steam_parse_library_paths(const std::string& text,
                                  std::vector<std::string>* out) {
  if (!out) return -1;
  std::vector<vdf_node> roots;
  if (!vdf_parse_document(text, &roots) || roots.empty()) return -1;
  const vdf_node* lf = find_root(roots, "libraryfolders");
  if (!lf) return -1;
  int added = 0;
  for (const auto& entry : lf->kids) {
    // Legacy: the numeric key's VALUE is the path.
    if (entry.has_value) {
      if (looks_like_path(entry.value)) {
        out->push_back(entry.value);
        ++added;
      }
      continue;
    }
    // Modern: the numeric key's block holds "path".
    if (const vdf_node* path = find_child(entry.kids, "path")) {
      if (path->has_value && looks_like_path(path->value)) {
        out->push_back(path->value);
        ++added;
      }
    }
  }
  return added;
}

int trt_steam_parse_appmanifest(const std::string& text, trt_steam_app* out) {
  if (!out) return -1;
  std::vector<vdf_node> roots;
  if (!vdf_parse_document(text, &roots) || roots.empty()) return -1;
  const vdf_node* state = find_root(roots, "AppState");
  if (!state) return -1;
  trt_steam_app app;
  bool have_appid = false;
  for (const auto& kv : state->kids) {
    if (!kv.has_value) continue;
    const std::string k = lower(kv.key);
    if (k == "appid") {
      app.appid = (unsigned)strtoul(kv.value.c_str(), nullptr, 10);
      have_appid = app.appid != 0;
    } else if (k == "name") {
      app.name = kv.value;
    } else if (k == "installdir") {
      app.installdir = kv.value;
    } else if (k == "stateflags") {
      app.state_flags = (unsigned)strtoul(kv.value.c_str(), nullptr, 10);
      app.fully_installed = (app.state_flags & 4u) != 0;
    } else if (k == "sizeondisk") {
      app.size_on_disk = strtoull(kv.value.c_str(), nullptr, 10);
    }
  }
  if (!have_appid) return -1;
  *out = std::move(app);
  return 0;
}

std::string trt_steam_find_exe(const std::string& installdir) {
  if (installdir.empty() || !is_dir(installdir)) return "";
  const auto exes = list_dir(installdir, ".exe");
  if (exes.empty()) return "";

  // Prefer an exe whose name contains the install dir's own name (the usual
  // "<GameName>/<GameName>.exe" layout), then the largest file.
  std::string dirname = installdir;
  while (!dirname.empty() && (dirname.back() == '/' || dirname.back() == '\\'))
    dirname.pop_back();
  if (const size_t slash = dirname.find_last_of("/\\"); slash != std::string::npos)
    dirname = dirname.substr(slash + 1);

  std::string best;
  unsigned long long best_size = 0;
  bool best_named = false;
  const std::string want = lower(dirname);
  for (const auto& name : exes) {
    const std::string full = join(installdir, name.c_str());
    unsigned long long size = 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fd)) {
      size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
#else
    struct stat st;
    if (stat(full.c_str(), &st) == 0) size = (unsigned long long)st.st_size;
#endif
    const std::string stem = lower(name.substr(0, name.size() - 4));
    const bool named = !want.empty() && stem.find(want) != std::string::npos;
    if (best.empty() || (named && !best_named) ||
        (named == best_named && size > best_size)) {
      best = name;
      best_size = size;
      best_named = named;
    }
  }
  return best;
}

int trt_steam_library_scan(trt_steam_library* lib) {
  return trt_steam_library_scan_roots(trt_steam_default_roots(), lib);
}

int trt_steam_library_scan_roots(const std::vector<std::string>& roots,
                                 trt_steam_library* lib) {
  trt_steam_library local;
  trt_steam_library& L = lib ? *lib : local;
  L.roots.clear();
  L.apps.clear();
  L.notes.clear();

  std::vector<std::string> libraries;
  auto add_library = [&](const std::string& p) {
    if (p.empty()) return;
    if (!is_dir(join(p, "steamapps"))) return;
    if (std::find(libraries.begin(), libraries.end(), p) != libraries.end()) return;
    libraries.push_back(p);
  };

  for (const auto& root : roots) {
    if (!is_dir(root)) continue;
    add_library(root);  // the root is itself a library
    const std::string vdf = trt_steam_libraryfolders_path(root);
    if (!is_file(vdf)) continue;
    const std::string text = read_file(vdf);
    if (text.empty()) {
      L.notes.push_back("unreadable libraryfolders.vdf at " + vdf);
      continue;
    }
    std::vector<std::string> extra;
    if (trt_steam_parse_library_paths(text, &extra) < 0) {
      L.notes.push_back("not a libraryfolders.vdf: " + vdf);
      continue;
    }
    for (auto& p : extra) add_library(p);
  }

  L.roots = libraries;
  for (const auto& library : libraries) {
    const std::string apps_dir = join(library, "steamapps");
    const auto manifests = list_dir(apps_dir, ".acf");
    for (const auto& m : manifests) {
      if (m.rfind("appmanifest_", 0) != 0) continue;  // librarycache etc.
      const std::string full = join(apps_dir, m.c_str());
      const std::string text = read_file(full);
      if (text.empty()) {
        L.notes.push_back("unreadable manifest " + m);
        continue;
      }
      trt_steam_app app;
      if (trt_steam_parse_appmanifest(text, &app) != 0) {
        L.notes.push_back("unparsed manifest " + m);
        continue;
      }
      app.library_path = library;
      L.apps.push_back(std::move(app));
    }
  }

  std::sort(L.apps.begin(), L.apps.end(),
            [](const trt_steam_app& a, const trt_steam_app& b) {
              return lower(a.name) < lower(b.name);
            });
  return (int)L.apps.size();
}
