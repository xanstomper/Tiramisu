// tiramisu/src/host/cover_art.cpp
// Automatic local + Steam CDN game cover art loader (premultiplied BGRA for D2D).
#include "cover_art.h"
#include "host_log.h"
#include "vendor/stb_image.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

namespace longan {

namespace {

struct cover_entry {
  std::vector<uint8_t> bgra;
  unsigned w = 0;
  unsigned h = 0;
  uint64_t version = 0;
  bool attempted_net = false;
  std::string disk_path;
};

struct fetch_job {
  std::string game_name;
  std::string game_dir;
  uint32_t appid = 0;
};

std::mutex g_mu;
std::string g_covers_dir;
std::unordered_map<std::string, cover_entry> g_cache;
std::unordered_map<std::string, cover_entry> g_hero_cache;
std::set<std::string> g_queued;
std::deque<fetch_job> g_jobs;
std::atomic<bool> g_worker_started{false};

std::string lower_key(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back((char)std::tolower(c));
  return out;
}

std::string sanitize_key(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_') out.push_back((char)std::tolower(c));
    else if (c == ' ') out.push_back('_');
  }
  return out.empty() ? "game" : out;
}

std::wstring u8to16(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (n <= 0) return std::wstring(s.begin(), s.end());
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

std::string u16to8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 16 * 1024 * 1024) {
    fclose(f);
    return false;
  }
  out->resize((size_t)sz);
  size_t n = fread(out->data(), 1, (size_t)sz, f);
  fclose(f);
  out->resize(n);
  return n > 16;
}

bool decode_to_bgra(const uint8_t* data, size_t len,
                    std::vector<uint8_t>* out_bgra, unsigned* out_w,
                    unsigned* out_h) {
  int w = 0, h = 0, ch = 0;
  unsigned char* rgba =
      stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
  if (!rgba || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    if (rgba) stbi_image_free(rgba);
    return false;
  }
  const size_t px_count = (size_t)w * (size_t)h;
  out_bgra->resize(px_count * 4);
  uint8_t* dst = out_bgra->data();
  for (size_t i = 0; i < px_count; ++i) {
    const uint8_t r = rgba[i * 4 + 0];
    const uint8_t g = rgba[i * 4 + 1];
    const uint8_t b = rgba[i * 4 + 2];
    const uint8_t a = rgba[i * 4 + 3];
    dst[i * 4 + 0] = (uint8_t)((b * a) / 255);
    dst[i * 4 + 1] = (uint8_t)((g * a) / 255);
    dst[i * 4 + 2] = (uint8_t)((r * a) / 255);
    dst[i * 4 + 3] = a;
  }
  stbi_image_free(rgba);
  *out_w = (unsigned)w;
  *out_h = (unsigned)h;
  return true;
}

bool http_get_bytes(const std::string& url, std::vector<uint8_t>* out) {
  try {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    HttpClient http;
    auto resp = http.GetAsync(winrt::Windows::Foundation::Uri(u8to16(url))).get();
    if (!resp.IsSuccessStatusCode()) return false;
    IBuffer buf = resp.Content().ReadAsBufferAsync().get();
    if (buf.Length() < 128 || buf.Length() > 12 * 1024 * 1024) return false;
    DataReader dr = DataReader::FromBuffer(buf);
    out->resize(buf.Length());
    dr.ReadBytes(*out);
    return true;
  } catch (...) {
    return false;
  }
}

bool http_get_text(const std::string& url, std::string* out) {
  try {
    using namespace winrt::Windows::Web::Http;
    HttpClient http;
    auto resp = http.GetAsync(winrt::Windows::Foundation::Uri(u8to16(url))).get();
    if (!resp.IsSuccessStatusCode()) return false;
    *out = u16to8(std::wstring(resp.Content().ReadAsStringAsync().get().c_str()));
    return !out->empty();
  } catch (...) {
    return false;
  }
}

// A store search for a generic folder name ("hermes") returns whatever Steam
// ranks first (Hello Kitty Island Adventure, in the case that motivated this).
// Only accept a candidate whose name shares a word of 3+ characters with the
// query, so sideloaded/unknown folders keep a placeholder instead of wrong art.
static bool cover_name_matches(const std::string& query,
                               const std::string& candidate) {
  auto lower = [](std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
  };
  auto alnum = [](char c) { return isalnum((unsigned char)c) != 0; };
  const std::string q = lower(query), c = lower(candidate);
  size_t i = 0;
  while (i < q.size()) {
    while (i < q.size() && !alnum(q[i])) ++i;
    const size_t s = i;
    while (i < q.size() && alnum(q[i])) ++i;
    if (i - s < 3) continue;
    const std::string tok = q.substr(s, i - s);
    size_t p = 0;
    while ((p = c.find(tok, p)) != std::string::npos) {
      const bool left_ok = p == 0 || !alnum(c[p - 1]);
      const size_t e = p + tok.size();
      const bool right_ok = e >= c.size() || !alnum(c[e]);
      if (left_ok && right_ok) return true;
      p = e;
    }
  }
  return false;
}

std::string clean_search_name(const std::string& raw) {
  std::string s = raw;
  // Strip staging prefixes like "stage-" or "test-"
  std::string low = lower_key(s);
  if (low.rfind("stage-", 0) == 0 && s.size() > 6) s = s.substr(6);
  else if (low.rfind("stage_", 0) == 0 && s.size() > 6) s = s.substr(6);
  for (char& c : s) {
    if (c == '_' || c == '-') c = ' ';
  }
  return s;
}

void store_decoded_cover(const std::string& game_name, const uint8_t* raw,
                         size_t raw_len, const std::string& disk_path) {
  std::vector<uint8_t> bgra;
  unsigned w = 0, h = 0;
  if (!decode_to_bgra(raw, raw_len, &bgra, &w, &h)) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto& e = g_cache[lower_key(game_name)];
  e.bgra = std::move(bgra);
  e.w = w;
  e.h = h;
  e.version++;
  if (!disk_path.empty()) e.disk_path = disk_path;
}

void store_decoded_hero(const std::string& game_name, const uint8_t* raw,
                        size_t raw_len, const std::string& disk_path) {
  std::vector<uint8_t> bgra;
  unsigned w = 0, h = 0;
  if (!decode_to_bgra(raw, raw_len, &bgra, &w, &h)) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto& e = g_hero_cache[lower_key(game_name)];
  e.bgra = std::move(bgra);
  e.w = w;
  e.h = h;
  e.version++;
  if (!disk_path.empty()) e.disk_path = disk_path;
}

bool try_load_local_cover(const std::string& game_name,
                          const std::string& game_dir) {
  std::string covers_dir;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    covers_dir = g_covers_dir;
  }
  const std::string sk = sanitize_key(game_name);
  std::vector<std::string> candidates;
  if (!game_dir.empty()) {
    candidates.push_back(game_dir + "\\cover.jpg");
    candidates.push_back(game_dir + "\\cover.png");
    candidates.push_back(game_dir + "\\header.jpg");
    candidates.push_back(game_dir + "\\.tiramisu\\cover.jpg");
    candidates.push_back(game_dir + "\\.tiramisu\\cover.png");
  }
  if (!covers_dir.empty()) {
    candidates.push_back(covers_dir + "\\" + sk + ".jpg");
    candidates.push_back(covers_dir + "\\" + sk + ".png");
  }
  for (const auto& p : candidates) {
    std::vector<uint8_t> bytes;
    if (read_file_bytes(p, &bytes)) {
      store_decoded_cover(game_name, bytes.data(), bytes.size(), p);
      return true;
    }
  }
  return false;
}

bool try_load_local_hero(const std::string& game_name,
                         const std::string& game_dir) {
  std::string covers_dir;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    covers_dir = g_covers_dir;
  }
  const std::string sk = sanitize_key(game_name);
  std::vector<std::string> candidates;
  if (!game_dir.empty()) {
    candidates.push_back(game_dir + "\\hero.jpg");
    candidates.push_back(game_dir + "\\hero.png");
    candidates.push_back(game_dir + "\\library_hero.jpg");
    candidates.push_back(game_dir + "\\.tiramisu\\hero.jpg");
  }
  if (!covers_dir.empty()) {
    candidates.push_back(covers_dir + "\\" + sk + "_hero.jpg");
    candidates.push_back(covers_dir + "\\" + sk + "_hero.png");
  }
  for (const auto& p : candidates) {
    std::vector<uint8_t> bytes;
    if (read_file_bytes(p, &bytes)) {
      store_decoded_hero(game_name, bytes.data(), bytes.size(), p);
      return true;
    }
  }
  return false;
}

void cover_worker_loop() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  while (true) {
    fetch_job job;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      if (g_jobs.empty()) {
        g_worker_started = false;
        return;
      }
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }

    const bool have_cover = try_load_local_cover(job.game_name, job.game_dir);
    const bool have_hero = try_load_local_hero(job.game_name, job.game_dir);
    if (have_cover && have_hero) continue;

    // Resolve Steam AppID (ignore 0 and Spacewar 480 placeholder)
    uint32_t resolved_id = (job.appid > 0 && job.appid != 480) ? job.appid : 0;
    std::string fallback_img_url;
    if (resolved_id == 0) {
      std::string term = clean_search_name(job.game_name);
      std::string escaped;
      try {
        escaped = u16to8(std::wstring(
            winrt::Windows::Foundation::Uri::EscapeComponent(u8to16(term))
                .c_str()));
      } catch (...) {
        escaped = term;
      }
      std::string resp;
      if (http_get_text(
              "https://store.steampowered.com/api/storesearch/?term=" +
                  escaped + "&l=english&cc=US",
              &resp)) {
        try {
          auto j = winrt::Windows::Data::Json::JsonObject::Parse(
              winrt::hstring(u8to16(resp)));
          if (j.HasKey(L"items")) {
            auto arr = j.GetNamedArray(L"items");
            const uint32_t limit = arr.Size() < 5 ? arr.Size() : 5;
            for (uint32_t ai = 0; ai < limit; ++ai) {
              auto it0 = arr.GetObjectAt(ai);
              std::string cand;
              if (it0.HasKey(L"name")) {
                cand = u16to8(
                    std::wstring(it0.GetNamedString(L"name").c_str()));
              }
              if (!cover_name_matches(job.game_name, cand)) continue;
              if (it0.HasKey(L"id")) {
                resolved_id = (uint32_t)it0.GetNamedNumber(L"id");
              }
              if (it0.HasKey(L"tiny_image")) {
                fallback_img_url = u16to8(
                    std::wstring(it0.GetNamedString(L"tiny_image").c_str()));
              }
              break;
            }
          }
        } catch (...) {
        }
      }
    }

    // Persist resolved real Steam AppID (replacing missing or 480 placeholder)
    // so the Steam bridge, overlay, and web dashboard see the title's true AppID.
    if (resolved_id > 0 && resolved_id != 480 && !job.game_dir.empty()) {
      const std::string appid_path = job.game_dir + "\\steam_appid.txt";
      uint32_t cur_id = 0;
      if (FILE* rf = fopen(appid_path.c_str(), "rb")) {
        char buf[32] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        fclose(rf);
        buf[n] = 0;
        cur_id = (uint32_t)strtoul(buf, nullptr, 10);
      }
      if (cur_id == 0 || cur_id == 480) {
        if (FILE* wf = fopen(appid_path.c_str(), "wb")) {
          fprintf(wf, "%u\n", resolved_id);
          fclose(wf);
          char amsg[160];
          snprintf(amsg, sizeof(amsg),
                   "cover_art: persisted Steam AppID %u for %s",
                   resolved_id, job.game_name.c_str());
          log_line(amsg);
        }
      }
    }

    // Fetch full Steam appdetails once if needed (gives hashed header, HD background_raw, 1080p screenshots)
    std::string ad_header, ad_bg_raw, ad_ss0;
    auto query_appdetails = [&]() {
      if (!ad_header.empty() || !ad_bg_raw.empty() || !ad_ss0.empty() || resolved_id == 0) return;
      std::string ad_resp;
      const std::string id_s = std::to_string(resolved_id);
      if (http_get_text(
              "https://store.steampowered.com/api/appdetails?appids=" + id_s,
              &ad_resp)) {
        try {
          auto root = winrt::Windows::Data::Json::JsonObject::Parse(
              winrt::hstring(u8to16(ad_resp)));
          auto w_id = winrt::hstring(u8to16(id_s));
          if (root.HasKey(w_id)) {
            auto entry = root.GetNamedObject(w_id);
            if (entry.HasKey(L"data")) {
              auto data = entry.GetNamedObject(L"data");
              if (data.HasKey(L"header_image")) {
                ad_header = u16to8(std::wstring(data.GetNamedString(L"header_image").c_str()));
              }
              if (data.HasKey(L"background_raw")) {
                ad_bg_raw = u16to8(std::wstring(data.GetNamedString(L"background_raw").c_str()));
              }
              if (data.HasKey(L"screenshots")) {
                auto ss = data.GetNamedArray(L"screenshots");
                if (ss.Size() > 0) {
                  auto s0 = ss.GetObjectAt(0);
                  if (s0.HasKey(L"path_full")) {
                    ad_ss0 = u16to8(std::wstring(s0.GetNamedString(L"path_full").c_str()));
                  }
                }
              }
            }
          }
        } catch (...) {
        }
      }
    };

    // 1) Card cover art (prefer crisp 616x353 capsule -> 460x215 header)
    if (!have_cover) {
      std::vector<std::string> urls;
      if (resolved_id > 0) {
        const std::string id_s = std::to_string(resolved_id);
        urls.push_back(
            "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" +
            id_s + "/capsule_616x353.jpg");
        urls.push_back(
            "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" +
            id_s + "/header.jpg");
        urls.push_back(
            "https://cdn.akamai.steamstatic.com/steam/apps/" + id_s +
            "/header.jpg");
      }
      if (!fallback_img_url.empty()) urls.push_back(fallback_img_url);

      std::vector<uint8_t> img_bytes;
      for (const auto& u : urls) {
        if (http_get_bytes(u, &img_bytes)) break;
      }
      if (img_bytes.empty() && resolved_id > 0) {
        query_appdetails();
        if (!ad_header.empty()) http_get_bytes(ad_header, &img_bytes);
      }
      if (!img_bytes.empty()) {
        std::string save_path;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          if (!g_covers_dir.empty()) {
            save_path = g_covers_dir + "\\" + sanitize_key(job.game_name) + ".jpg";
          }
        }
        if (!save_path.empty()) {
          FILE* f = fopen(save_path.c_str(), "wb");
          if (f) {
            fwrite(img_bytes.data(), 1, img_bytes.size(), f);
            fclose(f);
          }
        }
        store_decoded_cover(job.game_name, img_bytes.data(), img_bytes.size(),
                            save_path);
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "cover_art: fetched card %s (appid=%u, %zu bytes)",
                 job.game_name.c_str(), resolved_id, img_bytes.size());
        log_line(msg);
      }
    }

    // 2) Ultra-HD Hero Banner art (1920x620 library_hero.jpg -> 1920x1080 screenshot -> page_bg_raw)
    if (!have_hero && resolved_id > 0) {
      const std::string id_s = std::to_string(resolved_id);
      std::vector<std::string> hurls = {
          "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" +
              id_s + "/library_hero.jpg",
          "https://cdn.akamai.steamstatic.com/steam/apps/" + id_s +
              "/library_hero.jpg",
      };
      std::vector<uint8_t> hero_bytes;
      for (const auto& u : hurls) {
        if (http_get_bytes(u, &hero_bytes)) break;
      }
      if (hero_bytes.empty()) {
        query_appdetails();
        if (!ad_ss0.empty()) http_get_bytes(ad_ss0, &hero_bytes);
        if (hero_bytes.empty() && !ad_bg_raw.empty())
          http_get_bytes(ad_bg_raw, &hero_bytes);
        if (hero_bytes.empty() && !ad_header.empty())
          http_get_bytes(ad_header, &hero_bytes);
      }
      if (!hero_bytes.empty()) {
        std::string save_path;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          if (!g_covers_dir.empty()) {
            save_path = g_covers_dir + "\\" + sanitize_key(job.game_name) + "_hero.jpg";
          }
        }
        if (!save_path.empty()) {
          FILE* f = fopen(save_path.c_str(), "wb");
          if (f) {
            fwrite(hero_bytes.data(), 1, hero_bytes.size(), f);
            fclose(f);
          }
        }
        store_decoded_hero(job.game_name, hero_bytes.data(), hero_bytes.size(),
                           save_path);
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "cover_art: fetched HD hero %s (appid=%u, %zu bytes)",
                 job.game_name.c_str(), resolved_id, hero_bytes.size());
        log_line(msg);
      }
    }
  }
}

} // namespace

void cover_art_reset_queue() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_queued.clear();
  g_cache.clear();
  g_hero_cache.clear();
}

void cover_art_init(const std::string& state_dir) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_covers_dir = state_dir + "\\covers";
  CreateDirectoryA(g_covers_dir.c_str(), nullptr);
}

void cover_art_request(const std::string& game_name,
                       const std::string& game_dir, uint32_t steam_appid) {
  if (game_name.empty()) return;
  const std::string key = lower_key(game_name);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(key);
    auto hit = g_hero_cache.find(key);
    if (it != g_cache.end() && !it->second.bgra.empty() &&
        hit != g_hero_cache.end() && !hit->second.bgra.empty())
      return;
    if (g_queued.count(key)) return;
    g_queued.insert(key);
  }
  // Fast synchronous disk check for both card & HD hero
  const bool local_cov = try_load_local_cover(game_name, game_dir);
  const bool local_hero = try_load_local_hero(game_name, game_dir);
  if (local_cov && local_hero) return;

  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_jobs.push_back({game_name, game_dir, steam_appid});
  }
  if (!g_worker_started.exchange(true)) {
    std::thread(cover_worker_loop).detach();
  }
}

bool cover_art_save_bytes(const std::string& game_name, const uint8_t* data,
                          size_t len) {
  if (game_name.empty() || !data || len < 16) return false;
  std::vector<uint8_t> bgra;
  unsigned w = 0, h = 0;
  if (!decode_to_bgra(data, len, &bgra, &w, &h)) return false;

  std::string save_path;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_covers_dir.empty()) {
      const bool is_png = (len >= 4 && data[0] == 0x89 && data[1] == 'P');
      save_path = g_covers_dir + "\\" + sanitize_key(game_name) +
                  (is_png ? ".png" : ".jpg");
    }
  }
  if (!save_path.empty()) {
    FILE* f = fopen(save_path.c_str(), "wb");
    if (f) {
      fwrite(data, 1, len, f);
      fclose(f);
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto& e = g_cache[lower_key(game_name)];
    e.bgra = std::move(bgra);
    e.w = w;
    e.h = h;
    e.version++;
    if (!save_path.empty()) e.disk_path = save_path;
  }
  return true;
}

bool cover_art_get_bgra(const std::string& game_name,
                        std::vector<uint8_t>* out_bgra, unsigned* out_w,
                        unsigned* out_h, uint64_t* out_version) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(lower_key(game_name));
  if (it == g_cache.end() || it->second.bgra.empty()) return false;
  if (out_version) *out_version = it->second.version;
  if (out_w) *out_w = it->second.w;
  if (out_h) *out_h = it->second.h;
  if (out_bgra) *out_bgra = it->second.bgra;
  return true;
}

bool cover_art_get_hero_bgra(const std::string& game_name,
                             std::vector<uint8_t>* out_bgra, unsigned* out_w,
                             unsigned* out_h, uint64_t* out_version) {
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string key = lower_key(game_name);
  auto hit = g_hero_cache.find(key);
  if (hit != g_hero_cache.end() && !hit->second.bgra.empty()) {
    if (out_version) *out_version = hit->second.version;
    if (out_w) *out_w = hit->second.w;
    if (out_h) *out_h = hit->second.h;
    if (out_bgra) *out_bgra = hit->second.bgra;
    return true;
  }
  auto it = g_cache.find(key);
  if (it == g_cache.end() || it->second.bgra.empty()) return false;
  if (out_version) *out_version = it->second.version;
  if (out_w) *out_w = it->second.w;
  if (out_h) *out_h = it->second.h;
  if (out_bgra) *out_bgra = it->second.bgra;
  return true;
}

std::string cover_art_disk_path(const std::string& game_name) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(lower_key(game_name));
  if (it != g_cache.end() && !it->second.disk_path.empty()) {
    return it->second.disk_path;
  }
  if (!g_covers_dir.empty()) {
    std::string jpg = g_covers_dir + "\\" + sanitize_key(game_name) + ".jpg";
    FILE* f = fopen(jpg.c_str(), "rb");
    if (f) { fclose(f); return jpg; }
    std::string png = g_covers_dir + "\\" + sanitize_key(game_name) + ".png";
    f = fopen(png.c_str(), "rb");
    if (f) { fclose(f); return png; }
  }
  return {};
}

} // namespace longan
