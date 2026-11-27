// tiramisu/src/host/game_library.cpp
// Game shelf scanning, executable scoring, static PE/ISO compatibility
// analysis, engine label inference, and shelf ordering.
#include "game_library.h"

#include "cover_art.h"
#include "host_log.h"
#include "steam_install.h"
#include "usb_storage.h"
#include "../trt/engine_synthesizer.h"
#include "../trt/trt.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace longan {

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool is_junk_exe(const std::string& fname) {
  const std::string s = lower_ascii(fname);
  return s.find("unitycrashhandler") != std::string::npos ||
         s.find("crashreport") != std::string::npos ||
         s.find("setup") != std::string::npos ||
         s.find("install") != std::string::npos ||
         s.find("ueprereq") != std::string::npos ||
         s.find("ue4prereq") != std::string::npos ||
         s.find("ue5prereq") != std::string::npos ||
         s.find("vcredist") != std::string::npos ||
         s.find("vc_redist") != std::string::npos ||
         s.find("dxsetup") != std::string::npos ||
         s.find("dotnet") != std::string::npos ||
         s.find("unins0") != std::string::npos ||
         s.find("uninstall") != std::string::npos ||
         s.find("easyanticheat") != std::string::npos ||
         s.find("battleye") != std::string::npos;
}

void collect_exes_rec(const std::string& base_dir, const std::string& rel_dir,
                      const std::string& game_name, int depth,
                      std::vector<exe_cand>* out) {
  if (depth > 5) return;
  const std::string dir = rel_dir.empty() ? base_dir : (base_dir + "\\" + rel_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  const std::string g_low = lower_ascii(game_name);
  do {
    const std::string fn = fd.cFileName;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    const std::string rel = rel_dir.empty() ? fn : (rel_dir + "\\" + fn);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      const std::string fn_low = lower_ascii(fn);
      if (fn_low == "thirdparty" || fn_low == "extras" || fn_low == "prereqs") {
        continue;
      }
      collect_exes_rec(base_dir, rel, game_name, depth + 1, out);
      continue;
    }
    const size_t dot = fn.rfind('.');
    if (dot == std::string::npos || _stricmp(fn.c_str() + dot, ".exe") != 0) {
      continue;
    }
    if (is_junk_exe(fn)) continue;
    const uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    const std::string stem_low = lower_ascii(fn.substr(0, dot));
    const std::string rel_low = lower_ascii(rel);
    int score = 0;
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") score += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15) {
      score += 120;
    }
    if (stem_low.find("-win64-shipping") != std::string::npos ||
        stem_low.find("-shipping") != std::string::npos ||
        rel_low.find("binaries\\win64") != std::string::npos ||
        rel_low.find("binaries/win64") != std::string::npos ||
        rel_low.find("bin\\win64") != std::string::npos ||
        rel_low.find("bin/win64") != std::string::npos) {
      score += 2000;
    }
    if (depth == 0) score += 60;
    else if (depth == 1) score += 30;
    out->push_back({rel, sz, score, depth});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

void collect_iso_exes_rec(trt_iso* iso, const std::string& rel_dir,
                          const std::string& game_name, int depth,
                          std::vector<exe_cand>* out) {
  if (!iso || depth > 5) return;
  struct rec {
    std::vector<std::string> names;
    std::vector<int> dirs;
    std::vector<uint64_t> sizes;
  } r;
  trt_iso_enum(iso, rel_dir.c_str(), [](void* user, const char* name, int is_dir,
                                        uint64_t size) {
    rec* rr = (rec*)user;
    rr->names.push_back(name);
    rr->dirs.push_back(is_dir);
    rr->sizes.push_back(size);
  }, &r);
  const std::string g_low = lower_ascii(game_name);
  for (size_t i = 0; i < r.names.size(); ++i) {
    const std::string& fn = r.names[i];
    const std::string rel = rel_dir.empty() ? fn : (rel_dir + "\\" + fn);
    if (r.dirs[i]) {
      const std::string fn_low = lower_ascii(fn);
      if (fn_low == "thirdparty" || fn_low == "extras" || fn_low == "prereqs" ||
          fn_low == "directx" || fn_low == "redist")
        continue;
      collect_iso_exes_rec(iso, rel, game_name, depth + 1, out);
      continue;
    }
    const size_t dot = fn.rfind('.');
    if (dot == std::string::npos || _stricmp(fn.c_str() + dot, ".exe") != 0)
      continue;
    if (is_junk_exe(fn)) continue;
    const std::string stem_low = lower_ascii(fn.substr(0, dot));
    int score = 0;
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") score += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15)
      score += 120;
    if (depth == 0) score += 60;
    else if (depth == 1) score += 30;
    out->push_back({rel, r.sizes[i], score, depth});
  }
}

namespace {

void scan_compat_stream(std::istream& f, char* out, unsigned cap) {
  uint8_t hdr[4096] = {};
  f.read((char*)hdr, sizeof(hdr));
  const size_t got = (size_t)f.gcount();
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    snprintf(out, cap, "compat: not a PE"); return;
  }
  uint32_t e_lfanew = 0;
  memcpy(&e_lfanew, hdr + 0x3C, 4);
  if (e_lfanew + 24 > got || hdr[e_lfanew] != 'P' || hdr[e_lfanew + 1] != 'E') {
    snprintf(out, cap, "compat: PE signature missing"); return;
  }
  const uint16_t machine = (uint16_t)(hdr[e_lfanew + 4] | (hdr[e_lfanew + 5] << 8));
  const uint16_t num_sec = (uint16_t)(hdr[e_lfanew + 6] | (hdr[e_lfanew + 7] << 8));
  const uint16_t opt_size = (uint16_t)(hdr[e_lfanew + 20] | (hdr[e_lfanew + 21] << 8));
  const uint16_t opt_magic = (uint16_t)(hdr[e_lfanew + 24] | (hdr[e_lfanew + 25] << 8));
  const char* arch;
  if (machine == 0x8664 && opt_magic == 0x20B) arch = "x64";
  else if (machine == 0x014C) arch = "x86 32-bit (JIT)";
  else if (machine == 0xAA64) arch = "ARM64 (unsupported)";
  else arch = "unknown arch";

  uint32_t imp_rva = 0, imp_sz = 0;
  const uint32_t opt_off = e_lfanew + 24;
  if (opt_magic == 0x20B && opt_off + 128 <= got) {
    memcpy(&imp_rva, hdr + opt_off + 120, 4);
    memcpy(&imp_sz, hdr + opt_off + 124, 4);
  } else if (opt_magic == 0x10B && opt_off + 112 <= got) {
    memcpy(&imp_rva, hdr + opt_off + 104, 4);
    memcpy(&imp_sz, hdr + opt_off + 108, 4);
  }
  (void)imp_sz;

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

  char mods[256] = "";
  const uint64_t imp_file = imp_rva ? rva_to_file(imp_rva) : 0;
  if (imp_file > 0) {
    uint8_t desc[2048] = {};
    f.clear();
    f.seekg((std::streamoff)imp_file, std::ios::beg);
    f.read((char*)desc, sizeof(desc));
    const size_t dgot = (size_t)f.gcount();
    size_t o = 0;
    for (uint32_t d = 0; d * 20 + 20 <= dgot && o + 40 < sizeof(mods); ++d) {
      uint32_t name_rva = 0;
      memcpy(&name_rva, desc + d * 20 + 12, 4);
      if (!name_rva) break;
      const uint64_t nfile = rva_to_file(name_rva);
      if (!nfile) break;
      char nm[64] = {};
      f.clear();
      f.seekg((std::streamoff)nfile, std::ios::beg);
      f.read(nm, sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      if (!nm[0]) continue;
      const char* mark = "";
      if (!_strnicmp(nm, "steam_api", 9)) mark = "[steam-auth]";
      else if (!_strnicmp(nm, "eossdk", 6)) mark = "[eos OK]";
      else if (!_strnicmp(nm, "galaxy", 6)) mark = "[galaxy OK]";
      else if (!_strnicmp(nm, "dstorage", 8)) mark = "[dstorage OK]";
      else if (!_strnicmp(nm, "d3d12", 5)) mark = "[d3d12 OK]";
      else if (!_strnicmp(nm, "d3d9", 4)) mark = "[d3d9 OK]";
      else if (!_strnicmp(nm, "d3d8", 4)) mark = "[d3d8 OK]";
      else if (!_strnicmp(nm, "d3dx9", 5)) mark = "[d3dx9 OK]";
      else if (!_strnicmp(nm, "dsound", 6)) mark = "[dsound OK]";
      else if (!_strnicmp(nm, "ddraw", 5)) mark = "[ddraw OK]";
      else if (!_strnicmp(nm, "d3d11", 5)) mark = "[d3d11 OK]";
      else if (!_strnicmp(nm, "xaudio", 6)) mark = "[audio OK]";
      else if (!_strnicmp(nm, "openal", 6)) mark = "[openal OK]";
      else if (!_strnicmp(nm, "xinput", 6)) mark = "[pad OK]";
      else if (!_strnicmp(nm, "gameinput", 9)) mark = "[gameinput OK]";
      else if (!_strnicmp(nm, "winhttp", 7)) mark = "[winhttp OK]";
      else if (!_strnicmp(nm, "vulkan-1", 8)) mark = "[VK->D3D OK]";
      else if (!_strnicmp(nm, "opengl32", 8)) mark = "[GL->D3D OK]";
      else if (!_strnicmp(nm, "unityplayer", 11)) mark = "[unity OK]";
      else if (!_strnicmp(nm, "easyanticheat", 13) ||
               !_strnicmp(nm, "battleye", 8) ||
               !_strnicmp(nm, "beclient", 8)) mark = "[anti-cheat BLOCKED]";
      o += (size_t)snprintf(mods + o, sizeof(mods) - o, " %s%s", nm, mark);
    }
  }
  snprintf(out, cap, "compat: %s;%s", arch, mods[0] ? mods : " no imports");
}

}  // namespace

void scan_compat(const std::string& path, char* out, unsigned cap) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { snprintf(out, cap, "compat: exe unreadable"); return; }
  scan_compat_stream(f, out, cap);
}

void scan_compat_iso(trt_iso* iso, const std::string& rel, char* out,
                     unsigned cap) {
  int is_dir = 0;
  uint64_t size = 0;
  if (!iso || trt_iso_stat(iso, rel.c_str(), &is_dir, &size) != 0 || is_dir) {
    snprintf(out, cap, "compat: exe unreadable");
    return;
  }
  const uint64_t cap_bytes = 96ull * 1024 * 1024;
  const uint64_t want = size < cap_bytes ? size : cap_bytes;
  std::string buf((size_t)want, '\0');
  const uint32_t got = trt_iso_read(iso, rel.c_str(), 0, buf.data(), (uint32_t)want);
  buf.resize(got);
  std::istringstream is(buf, std::ios::binary);
  scan_compat_stream(is, out, cap);
}

void scan_game_entry(
    std::vector<game_entry>& games,
    const std::string& root, const std::string& n, bool is_dir,
    char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice) {
  if (n == "." || n == "..") return;
  if (!is_dir) {
    const size_t dot = n.rfind('.');
    if (dot == std::string::npos) return;
    const std::string ext = lower_ascii(n.substr(dot));
    if (ext != ".iso" && ext != ".bin" && ext != ".img" && ext != ".cue") return;
    std::string img_path = root + "\\" + n;
    if (ext == ".cue") {
      char track[1200] = {};
      if (!trt_iso_cue_track(img_path.c_str(), track, sizeof(track))) {
        char line[700];
        snprintf(line, sizeof(line),
                 "shelf: %s is a .cue whose FILE track could not be read "
                 "(skipped, named)",
                 img_path.c_str());
        log_line(line);
        return;
      }
      char line[700];
      snprintf(line, sizeof(line), "shelf: %s -> track %s",
               (root + "\\" + n).c_str(), track);
      log_line(line);
      img_path = track;
    }
    bool dup_img = false;
    for (const auto& existing : games)
      if (!_stricmp(existing.image.c_str(), img_path.c_str())) dup_img = true;
    if (dup_img) return;
    trt_iso* iso = trt_iso_open(img_path.c_str());
    if (!iso) {
      char line[600];
      snprintf(line, sizeof(line),
               "shelf: %s is not an ISO9660/UDF image (skipped, named)",
               img_path.c_str());
      log_line(line);
      return;
    }
    std::vector<exe_cand> icands;
    const std::string stem = n.substr(0, dot);
    collect_iso_exes_rec(iso, "", stem, 0, &icands);
    if (icands.empty()) {
      char line[600];
      snprintf(line, sizeof(line),
               "shelf: %s mounted but holds no runnable exe", img_path.c_str());
      log_line(line);
      trt_iso_close(iso);
      return;
    }
    std::sort(icands.begin(), icands.end(),
              [](const exe_cand& a, const exe_cand& b) {
      if (a.score != b.score) return a.score > b.score;
      if (a.size != b.size) return a.size > b.size;
      return a.rel < b.rel;
    });
    game_entry ge;
    ge.name = stem;
    ge.dir = root;
    ge.image = img_path;
    for (const auto& c : icands) ge.exes.push_back(c.rel);
    ge.exe = ge.exes[0];
    char compat[280];
    scan_compat_iso(iso, ge.exe, compat, (unsigned)sizeof(compat));
    ge.compat = compat;
    trt_iso_close(iso);
    cover_art_request(ge.name, ge.dir, 0);
    games.push_back(std::move(ge));
    return;
  }
  const std::string gdir = root + "\\" + n;
  const auto steam_install = inspect_steam_install(gdir);
  if (steam_install.state == steam_install_state::incomplete ||
      steam_install.state == steam_install_state::missing_executable) return;
  std::string disp = n;
  bool dup = false;
  for (const auto& existing : games) {
    if (!_stricmp(existing.dir.c_str(), gdir.c_str())) {
      dup = true;
      break;
    }
    if (existing.name == disp && drive_tag) {
      disp = n + " [" + std::string(1, drive_tag) + ":]";
    }
  }
  if (dup) return;
  std::vector<exe_cand> cands;
  collect_exes_rec(gdir, "", n, 0, &cands);
  std::string image_path;
  if (steam_install.state != steam_install_state::ready) {
    char img[1024] = {};
    if (trt_iso_find_image_in_dir(gdir.c_str(), img, sizeof(img))) {
      std::vector<exe_cand> icands;
      trt_iso* iso = trt_iso_open(img);
      if (iso) {
        collect_iso_exes_rec(iso, "", n, 0, &icands);
        trt_iso_close(iso);
      }
      if (!icands.empty()) {
        std::sort(icands.begin(), icands.end(),
                  [](const exe_cand& a, const exe_cand& b) {
          if (a.score != b.score) return a.score > b.score;
          if (a.size != b.size) return a.size > b.size;
          return a.rel < b.rel;
        });
        int folder_score = -1;
        for (const auto& c : cands)
          if (c.score > folder_score) folder_score = c.score;
        if (icands[0].score > folder_score) {
          cands = icands;
          image_path = img;
          char line[800];
          snprintf(line, sizeof(line),
                   "shelf: %s mounts disc image %s (best exe inside: %s)",
                   n.c_str(), img, icands[0].rel.c_str());
          log_line(line);
        } else {
          char line[800];
          snprintf(line, sizeof(line),
                   "shelf: %s runs from its folder exe over disc image %s "
                   "(the image is not mounted)",
                   n.c_str(), img);
          log_line(line);
        }
      } else {
        char line[700];
        snprintf(line, sizeof(line),
                 "shelf: %s holds a disc image but no runnable exe inside it",
                 img);
        log_line(line);
      }
    }
  }
  std::string steam_exe;
  if (steam_install.state == steam_install_state::ready) {
    steam_exe = steam_install.executable;
    if (std::none_of(cands.begin(), cands.end(), [&](const exe_cand& candidate) {
          return _stricmp(candidate.rel.c_str(), steam_exe.c_str()) == 0;
        })) {
      exe_cand selected{};
      selected.rel = steam_exe;
      cands.push_back(std::move(selected));
    }
  }
  if (cands.empty()) return;
  std::sort(cands.begin(), cands.end(), [](const exe_cand& a, const exe_cand& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.size != b.size) return a.size > b.size;
    if (a.depth != b.depth) return a.depth < b.depth;
    return a.rel < b.rel;
  });
  game_entry ge;
  ge.name = disp;
  ge.dir = gdir;
  for (const auto& c : cands) ge.exes.push_back(c.rel);
  ge.exe_idx = 0;
  for (size_t k = 0; k < ge.exes.size() && !steam_exe.empty(); ++k) {
    if (_stricmp(ge.exes[k].c_str(), steam_exe.c_str()) == 0) {
      ge.exe_idx = static_cast<int>(k);
      break;
    }
  }
  {
    std::ifstream ef(gdir + "\\tiramisu_exe.txt");
    std::string cfg_exe;
    if (ef && std::getline(ef, cfg_exe)) {
      while (!cfg_exe.empty() && ((unsigned char)cfg_exe.back() <= ' '))
        cfg_exe.pop_back();
      for (size_t k = 0; k < ge.exes.size(); ++k) {
        if (!_stricmp(ge.exes[k].c_str(), cfg_exe.c_str())) {
          ge.exe_idx = (int)k;
          break;
        }
      }
    }
    std::ifstream inf(gdir + "\\tiramisu_input.txt");
    std::string im;
    if (inf && std::getline(inf, im)) {
      while (!im.empty() && ((unsigned char)im.back() <= ' ')) im.pop_back();
      ge.input_mode = lower_ascii(im);
    }
  }
  for (const auto& p : prev_choice) {
    if (p.first == (ge.image.empty() ? gdir : ge.image)) {
      for (size_t k = 0; k < ge.exes.size(); ++k) {
        if (ge.exes[k] == p.second) {
          ge.exe_idx = (int)k;
          break;
        }
      }
      break;
    }
  }
  ge.exe = ge.exes[ge.exe_idx];
  ge.image = image_path;
  char compat[280];
  if (!ge.image.empty()) {
    trt_iso* iso = trt_iso_open(ge.image.c_str());
    scan_compat_iso(iso, ge.exe, compat, (unsigned)sizeof(compat));
    trt_iso_close(iso);
  } else {
    scan_compat(gdir + "\\" + ge.exe, compat, (unsigned)sizeof(compat));
  }
  ge.compat = compat;
  if (ge.image.empty() && !ge.dir.empty() && !ge.exe.empty()) {
    trt_engine_profile prof;
    if (trt_synthesize_engine(ge.dir.c_str(), ge.exe.c_str(), &prof)) {
      ge.engine_name = trt_engine_type_name(prof.engine_type);
      ge.gfx_name = trt_graphics_api_name(prof.preferred_gfx);
      ge.audio_name = trt_audio_middleware_name(prof.preferred_audio);
      ge.synthesized = true;
      if (!prof.main_exe_rel.empty() && prof.main_exe_rel != ge.exe) {
        for (size_t k = 0; k < ge.exes.size(); ++k) {
          if (!_stricmp(ge.exes[k].c_str(), prof.main_exe_rel.c_str())) {
            ge.exe_idx = (int)k;
            ge.exe = ge.exes[k];
            break;
          }
        }
      }
    }
  }
  {
    uint32_t appid = 0;
    std::ifstream sf(gdir + "\\steam_appid.txt");
    if (sf) {
      unsigned long v = 0;
      if (sf >> v) appid = static_cast<uint32_t>(v);
    }
    cover_art_request(ge.name, ge.dir, appid);
  }
  games.push_back(std::move(ge));
}

void scan_games_root(
    std::vector<game_entry>& games,
    const std::string& root, char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    scan_game_entry(games, root, fd.cFileName,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    drive_tag, prev_choice);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

std::vector<std::pair<std::string, std::string>> snapshot_prev_choice(
    const std::vector<game_entry>& games) {
  std::vector<std::pair<std::string, std::string>> prev_choice;
  for (const auto& ge : games) {
    prev_choice.push_back({!ge.image.empty() ? ge.image
                            : (ge.dir.empty() ? ge.name : ge.dir),
                           ge.exe});
  }
  return prev_choice;
}

std::wstring infer_engine_label(const game_entry& ge) {
  const std::string c = lower_ascii(ge.compat);
  if (c.find("anti-cheat") != std::string::npos) return L"Anti-Cheat (Blocked)";
  if (ge.synthesized && !ge.engine_name.empty()) {
    std::string label = ge.engine_name;
    if (!ge.gfx_name.empty() && ge.gfx_name != "Direct3D 11") {
      label += " (" + ge.gfx_name + ")";
    }
    return host_u82ws(label);
  }
  if (c.find("32-bit") != std::string::npos) return L"x86 32-bit (JIT)";
  if (c.find("arm64") != std::string::npos) return L"ARM64 (Unsupported)";
  if (c.find("d3d12") != std::string::npos) return L"Direct3D 12";
  if (c.find("vulkan") != std::string::npos) return L"Vulkan -> D3D11";
  if (c.find("unity") != std::string::npos) return L"Unity Engine";
  if (c.find("d3d11") != std::string::npos) return L"Direct3D 11";
  if (c.find("opengl") != std::string::npos) return L"OpenGL -> D3D11";
  if (c.find("d3d9") != std::string::npos) return L"Direct3D 9";
  if (!ge.image.empty()) return L"Disc Image";
  return L"Windows x64";
}

void sort_games_shelf(std::vector<game_entry>& games) {
  games.erase(std::remove_if(games.begin(), games.end(),
                             [](const game_entry& ge) {
                               const std::string low = lower_ascii(ge.name);
                               return low == "d3d12triangle" ||
                                      low == "selftest" ||
                                      low == "guest_hello";
                             }),
              games.end());
  auto rank = [](const game_entry& ge) -> int {
    const std::string low = lower_ascii(ge.name);
    if (low == "celeste") return 0;
    if (low == "sekiro") return 1;
    if (low == "ultrapool") return 2;
    if (low.rfind("stage-", 0) == 0 || low.rfind("stage_", 0) == 0) return 4;
    return 3;
  };
  std::sort(games.begin(), games.end(),
            [&](const game_entry& a, const game_entry& b) {
              const int ra = rank(a), rb = rank(b);
              if (ra != rb) return ra < rb;
              return lower_ascii(a.name) < lower_ascii(b.name);
            });
}

}  // namespace longan
