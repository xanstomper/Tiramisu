// tiramisu/src/trt/engine_synthesizer.cpp
// Autonomous Engine Synthesizer & Adaptive Execution Harness.
//
// Detection order, highest evidence first:
//   1. The chosen exe's PE headers   (PE32 vs PE64, CLR/managed bit)
//   2. Its real import + delay-import module table (read on demand and bounds-checked)
//   3. Runtime-loaded engine DLLs sitting next to it (Unity/Godot/FNA)
//   4. Data markers on disk (data.win, .pck, .pak, .vpk)
// Nothing is classified from a bare filename guess like "options.ini".
#include "engine_synthesizer.h"
#include "pe.h"
#include "trt.h"
#include "thunk.h"
#include "dynamic_api.h"
#include "bridge_d3d.h"
#include "bridge_d3d12.h"
#include "bridge_d3d9.h"
#include "bridge_d3dx9.h"
#include "bridge_d3d8.h"
#include "bridge_ddraw.h"
#include "bridge_vulkan.h"
#include "bridge_opengl32.h"
#include "bridge_audio.h"
#include "bridge_dsound.h"
#include "bridge_openal.h"
#include "bridge_wasapi.h"
#include "bridge_fmod.h"
#include "bridge_wwise.h"
#include "bridge_bink.h"
#include "bridge_criware.h"
#include "bridge_miles.h"
#include "bridge_oodle.h"
#include "bridge_dstorage.h"
#include "bridge_gameinput.h"
#include "bridge_xinput.h"
#include "bridge_hid.h"
#include "bridge_winrt.h"
#include "bridge_steam.h"
#include "bridge_nvapi.h"
#include "bridge_d3dx.h"
#include "bridge_gdiplus.h"
#include "bridge_mmedia.h"
#include "bridge_mf.h"
#include "bridge_dinput.h"
#include "bridge_comctl.h"
#include "bridge_ktmw32.h"
#include "bridge_wldap32.h"
#include "bridge_sys_extras.h"
#include "bridge_psapi.h"
#include "bridge_ws2.h"
#include "bridge_ole.h"
#include "bridge_shell.h"
#include "bridge_shlwapi.h"
#include "bridge_actctx.h"
#include "bridge_version.h"
#include "bridge_imm32.h"
#include "bridge_wininet.h"
#include "bridge_iphlpapi.h"
#include "bridge_crypt32.h"
#include "bridge_dbghelp.h"
#include "bridge_winmm.h"
#include "bridge_gdi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

char lower_char(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

std::string lower_str(std::string s) {
  for (char& c : s) c = lower_char(c);
  return s;
}

std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  char sep = '/';
#ifdef _WIN32
  sep = '\\';
#endif
  if (a.back() == '/' || a.back() == '\\') return a + b;
  return a + sep + b;
}

std::string normalize_exe_hint(std::string path) {
  if (path.find_first_of(":\r\n") != std::string::npos ||
      path.find('\0') != std::string::npos) return {};
  std::replace(path.begin(), path.end(), '\\', '/');
  if (path.empty() || path.front() == '/') return {};
  std::string normalized;
  for (size_t start = 0; start < path.size();) {
    const size_t end = path.find('/', start);
    const std::string part = path.substr(start, end - start);
    if (part == "..") return {};
    if (part != "." && !part.empty()) {
      if (part.back() == '.' || part.back() == ' ') return {};
      normalized = path_join(normalized, part);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return normalized;
}

bool file_exists(const std::string& p) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool dir_exists(const std::string& p) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_directory(const std::string& p) {
  if (p.empty() || dir_exists(p)) return true;
#ifdef _WIN32
  return CreateDirectoryA(p.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
  return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

// Child directory names (not paths) of one host directory. Used by the engine
// payload closure check, which needs directories (Cuphead_Data, Content) rather
// than the file list the classifier walks.
std::vector<std::string> list_child_dirs(const std::string& dir) {
  std::vector<std::string> out;
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
    out.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* de;
  while ((de = readdir(d)) != nullptr) {
    if (de->d_name[0] == '.' && (de->d_name[1] == 0 ||
                                 (de->d_name[1] == '.' && de->d_name[2] == 0)))
      continue;
    std::string p = path_join(dir, de->d_name);
    if (dir_exists(p)) out.push_back(de->d_name);
  }
  closedir(d);
#endif
  return out;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) return false;
  return lower_str(s.substr(s.size() - suffix.size())) == lower_str(suffix);
}

// One profile.json writer: the synthesizer calls it once, the launch preflight
// calls it again with the verdict attached, so the file the user reads always
// matches the profile the launch used.
bool write_profile_json(const std::string& base_dir,
                        const trt_engine_profile* p,
                        const trt_preflight_report* rep) {
  if (base_dir.empty()) return false;
  const std::string meta_dir = path_join(base_dir, ".tiramisu");
  if (!dir_exists(meta_dir) && !make_directory(meta_dir)) return false;
  const std::string path = path_join(meta_dir, "profile.json");
  FILE* pf = fopen(path.c_str(), "w");
  if (!pf) return false;
  fprintf(pf, "{\n");
  fprintf(pf, "  \"game\": \"%s\",\n", json_escape(p->game_name).c_str());
  fprintf(pf, "  \"engine\": \"%s\",\n",
          json_escape(trt_engine_type_name(p->engine_type)).c_str());
  fprintf(pf, "  \"architecture\": \"%s\",\n",
          p->is_managed ? "Managed (.NET)"
                        : (p->is_pe32 ? "PE32 (x86)"
                                      : (p->is_pe64 ? "PE64 (x64)" : "unknown")));
  fprintf(pf, "  \"pe_scanned\": %s,\n", p->pe_scanned ? "true" : "false");
  fprintf(pf, "  \"main_exe\": \"%s\",\n", json_escape(p->main_exe_rel).c_str());
  fprintf(pf, "  \"graphics\": \"%s\",\n",
          json_escape(trt_graphics_api_name(p->preferred_gfx)).c_str());
  fprintf(pf, "  \"audio\": \"%s\",\n",
          json_escape(trt_audio_middleware_name(p->preferred_audio)).c_str());
  fprintf(pf, "  \"steam_appid\": %u,\n", p->steam_appid);
  fprintf(pf, "  \"steam_appid_synthesized\": false,\n");
  fprintf(pf, "  \"has_steam\": %s,\n", p->has_steam ? "true" : "false");
  fprintf(pf, "  \"universal_flags\": %u,\n", p->universal_flags);
  fprintf(pf, "  \"screen\": { \"width\": %d, \"height\": %d, \"fullscreen\": %s },\n",
          p->screen_width, p->screen_height, p->fullscreen ? "true" : "false");
  fprintf(pf, "  \"payload_complete\": %s,\n", p->payload_complete ? "true" : "false");
  fprintf(pf, "  \"missing_payload\": [");
  for (size_t i = 0; i < p->missing_payload.size(); ++i) {
    fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(p->missing_payload[i]).c_str());
  }
  fprintf(pf, "],\n");
  fprintf(pf, "  \"imports\": [");
  for (size_t i = 0; i < p->imported_modules.size(); ++i) {
    fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(p->imported_modules[i]).c_str());
  }
  fprintf(pf, "],\n");
  fprintf(pf, "  \"shims\": [");
  for (size_t i = 0; i < p->synthetic_shims.size(); ++i) {
    fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(p->synthetic_shims[i]).c_str());
  }
  fprintf(pf, "],\n");
  fprintf(pf, "  \"cmdline\": [");
  for (size_t i = 0; i < p->cmdline_args.size(); ++i) {
    fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(p->cmdline_args[i]).c_str());
  }
  fprintf(pf, "],\n");
  if (!rep) {
    fprintf(pf, "  \"preflight\": null\n");
  } else {
    fprintf(pf, "  \"preflight\": {\n");
    fprintf(pf, "    \"runnable\": %s,\n", rep->runnable ? "true" : "false");
    fprintf(pf, "    \"blockers\": %u,\n", rep->blockers);
    fprintf(pf, "    \"codes\": [");
    bool first = true;
    for (unsigned b = 1; b; b <<= 1) {
      if (!(rep->blockers & b)) continue;
      fprintf(pf, "%s\"%s\"", first ? "" : ", ",
              json_escape(trt_preflight_blocker_name(b)).c_str());
      first = false;
    }
    fprintf(pf, "],\n");
    fprintf(pf, "    \"lines\": [");
    for (size_t i = 0; i < rep->lines.size(); ++i) {
      fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(rep->lines[i]).c_str());
    }
    fprintf(pf, "],\n");
    fprintf(pf, "    \"notes\": [");
    for (size_t i = 0; i < rep->notes.size(); ++i) {
      fprintf(pf, "%s\"%s\"", i ? ", " : "", json_escape(rep->notes[i]).c_str());
    }
    fprintf(pf, "]\n  }\n");
  }
  fprintf(pf, "}\n");
  fclose(pf);
  return true;
}

// Recursive directory creation for a host path (`drive_c` may not exist yet on
// a first launch). Separators are the host's own.
bool make_dirs(const std::string& path) {
  if (path.empty()) return false;
  if (dir_exists(path)) return true;
  std::string cur;
  cur.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    const char c = path[i];
    cur += c;
    const bool at_sep = (c == '/' || c == '\\');
    if (!at_sep && i + 1 != path.size()) continue;
    std::string prefix = cur;
    while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
      prefix.pop_back();
    if (prefix.empty()) continue;
    if (prefix.size() == 2 && prefix[1] == ':') continue;  // drive root
    if (!dir_exists(prefix)) make_directory(prefix);
  }
  return dir_exists(path);
}

bool is_junk_executable(const std::string& name) {
  std::string s = lower_str(name);
  return s.find("unitycrashhandler") != std::string::npos ||
         s.find("crashreport") != std::string::npos ||
         s.find("crashpad") != std::string::npos ||
         s.find("ueprereq") != std::string::npos ||
         s.find("ue4prereq") != std::string::npos ||
         s.find("ue5prereq") != std::string::npos ||
         s.find("vcredist") != std::string::npos ||
         s.find("vc_redist") != std::string::npos ||
         s.find("dxsetup") != std::string::npos ||
         s.find("dxwebsetup") != std::string::npos ||
         s.find("dotnet") != std::string::npos ||
         s.find("setup") != std::string::npos ||      // installers, not games
         s.find("install") != std::string::npos ||
         s.find("unins0") != std::string::npos ||
         s.find("uninstall") != std::string::npos ||
         s.find("easyanticheat") != std::string::npos ||
         s.find("battleye") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Read-only whole-file mapping: a 500 MB game exe is inspected without copying
// it, and every access is bounds-checked so a truncated/hostile PE cannot read
// past the mapping.
// ---------------------------------------------------------------------------
// Reads only the requested file ranges. PE file offsets can lie beyond the
// signature scan windows, even when the corresponding RVAs are small.
class scan_file {
 public:
  explicit scan_file(const std::string& path)
      : stream_(path, std::ios::binary | std::ios::ate) {
    const auto end = stream_.tellg();
    if (end > 0) size_ = static_cast<uint64_t>(end);
  }

  uint64_t size() const { return size_; }

  bool read(uint64_t offset, void* out, size_t length) {
    if (!length) return true;
    if (offset > size_ || length > size_ - offset) return false;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(stream_.read(static_cast<char*>(out),
                                          static_cast<std::streamsize>(length)));
  }

 private:
  std::ifstream stream_;
  uint64_t size_ = 0;
};

struct pe_scan {
  bool ok = false;
  bool is_pe32 = false;
  bool is_pe64 = false;
  bool is_managed = false;
  bool is_guest_exe = false;
  uint16_t subsystem = 0;
  uint16_t machine = 0;
  std::vector<std::string> modules;  // lower-case, unique, sorted
};

struct file_range {
  uint64_t offset = 0;
  uint64_t length = 0;
};

file_range pe_file_range(const std::vector<trt_section_header>& sections,
                         uint32_t headers_size, uint64_t file_size,
                         uint32_t rva) {
  if (rva < headers_size && rva < file_size)
    return {rva, std::min<uint64_t>(headers_size, file_size) - rva};
  for (const auto& section : sections) {
    if (rva < section.virtual_address) continue;
    const uint32_t delta = rva - section.virtual_address;
    if (delta >= section.size_of_raw_data) continue;
    const uint64_t offset = uint64_t(section.pointer_to_raw_data) + delta;
    if (offset >= file_size) continue;
    return {offset, std::min<uint64_t>(section.size_of_raw_data - delta,
                                       file_size - offset)};
  }
  return {};
}

bool scan_pe_file(const std::string& path, pe_scan* out) {
  scan_file file(path);
  trt_dos_header dos{};
  if (!file.read(0, &dos, sizeof(dos)) || dos.e_magic != 0x5a4d) return false;
  const uint64_t nt = dos.e_lfanew;
  uint32_t signature = 0;
  trt_file_header header{};
  if (!file.read(nt, &signature, sizeof(signature)) || signature != 0x00004550u ||
      !file.read(nt + sizeof(signature), &header, sizeof(header))) return false;

  const uint64_t opt_offset = nt + sizeof(signature) + sizeof(header);
  uint16_t magic = 0;
  if (header.size_of_optional_header < sizeof(magic) ||
      !file.read(opt_offset, &magic, sizeof(magic))) return false;
  const bool pe32 = magic == TRT_PE_OPT32_MAGIC;
  const bool pe64 = magic == 0x020b;
  if (!pe32 && !pe64) return false;
  const size_t dirs_offset = pe32 ? TRT_PE32_OPT_DIRS_OFF : TRT_PE64_OPT_DIRS_OFF;
  if (header.size_of_optional_header < dirs_offset) return false;

  uint32_t headers_size = 0, dir_count = 0;
  if (!file.read(opt_offset + 60, &headers_size, sizeof(headers_size)) ||
      !file.read(opt_offset + dirs_offset - sizeof(dir_count), &dir_count,
                 sizeof(dir_count)) ||
      !file.read(opt_offset + 68, &out->subsystem, sizeof(out->subsystem)))
    return false;
  const size_t known_count = std::min<size_t>(dir_count, TRT_PE_NUM_DIRECTORIES);
  if (known_count > (header.size_of_optional_header - dirs_offset) /
                        sizeof(trt_data_directory)) return false;
  trt_data_directory dirs[TRT_PE_NUM_DIRECTORIES]{};
  if (!file.read(opt_offset + dirs_offset, dirs, known_count * sizeof(dirs[0])))
    return false;

  const uint64_t section_offset = opt_offset + header.size_of_optional_header;
  // The Windows image loader limits PE images to 96 sections.
  if (header.number_of_sections > 96 || section_offset > file.size()) return false;
  std::vector<trt_section_header> sections(header.number_of_sections);
  if (!file.read(section_offset, sections.data(),
                 sections.size() * sizeof(trt_section_header))) return false;

  out->machine = header.machine;
  out->is_pe32 = pe32;
  out->is_pe64 = pe64;
  out->is_guest_exe = !(header.characteristics & 0x2000u) &&
                      header.machine == (pe32 ? TRT_PE_MACHINE_I386 : TRT_PE_MACHINE_X64);
  out->is_managed = dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0;

  auto range_for = [&](uint32_t rva) {
    return pe_file_range(sections, headers_size, file.size(), rva);
  };
  auto add_module = [&](uint32_t rva) {
    if (!rva) return;
    const file_range name = range_for(rva);
    char bytes[512];
    const size_t length = std::min<uint64_t>(name.length, sizeof(bytes));
    if (length && file.read(name.offset, bytes, length) && bytes[0] &&
        memchr(bytes, 0, length)) out->modules.push_back(lower_str(bytes));
  };
  auto directory = [&](size_t index) {
    const auto& dir = dirs[index];
    if (!dir.virtual_address) return file_range{};
    file_range range = range_for(dir.virtual_address);
    if (dir.size) range.length = std::min<uint64_t>(range.length, dir.size);
    return range;
  };

  const file_range imports = directory(TRT_PE_DIR_IMPORT);
  const size_t import_count = std::min<uint64_t>(512,
      imports.length / sizeof(trt_import_descriptor));
  for (size_t i = 0; i < import_count; ++i) {
    trt_import_descriptor desc{};
    if (!file.read(imports.offset + i * sizeof(desc), &desc, sizeof(desc))) return false;
    if (!desc.name && !desc.original_first_thunk && !desc.first_thunk) break;
    add_module(desc.name);
  }
  const file_range delays = directory(TRT_PE_DIR_DELAY_IMPORT);
  const size_t delay_count = std::min<uint64_t>(256,
      delays.length / sizeof(trt_delayload_descriptor));
  for (size_t i = 0; i < delay_count; ++i) {
    trt_delayload_descriptor desc{};
    if (!file.read(delays.offset + i * sizeof(desc), &desc, sizeof(desc))) return false;
    if (!desc.name_rva && !desc.iat_rva && !desc.int_rva) break;
    if (desc.attributes & 1) add_module(desc.name_rva);
  }

  std::sort(out->modules.begin(), out->modules.end());
  out->modules.erase(std::unique(out->modules.begin(), out->modules.end()),
                     out->modules.end());
  out->ok = true;
  return true;
}

// ---------------------------------------------------------------------------
// Engine byte signatures. A binary that delay-loads its engine, or whose
// import names were stripped, still carries the engine's own strings. Only
// unambiguous markers are used (an asset path scheme, a build tag, a product
// name), never a bare "engine" word that would match half the industry.
// ---------------------------------------------------------------------------
struct pe_signatures {
  bool unreal = false;
  bool unity = false;
  bool godot = false;
  bool gamemaker = false;
  bool source = false;
  bool xna = false;
  bool cryengine = false;
  bool re_engine = false;
  bool idtech = false;
  bool decima = false;
  bool defold = false;
  bool cocos = false;
  bool love2d = false;
};

bool contains_bytes(const u8* d, size_t total, const char* needle) {
  const size_t nlen = strlen(needle);
  if (!nlen || total < nlen) return false;
  const u8* p = d;
  const u8* end = d + total - nlen + 1;
  const unsigned char first = (unsigned char)needle[0];
  while (p < end) {
    const void* hit = memchr(p, first, (size_t)(end - p));
    if (!hit) return false;
    const u8* h = (const u8*)hit;
    if (memcmp(h, needle, nlen) == 0) return true;
    p = h + 1;
  }
  return false;
}

// Bounded scan: the head and tail of a huge exe carry the build tags and
// asset paths; the middle of a 500 MB recompile adds minutes for no evidence.
bool scan_signatures(const std::string& path, pe_signatures* out) {
  static const char* const kUnreal[] = {"++UE4+Release", "++UE5+Release",
                                        "/Script/Engine", ".uasset", ".umap"};
  static const char* const kUnity[] = {"UnityPlayer.dll", "Unity IL2CPP",
                                       "Assembly-CSharp.dll"};
  static const char* const kGodot[] = {"GodotEngine", "GodotSharp"};
  static const char* const kGm[] = {"YoYo Games", "data.win"};
  static const char* const kSource[] = {"Source Engine", "vstdlib.dll"};
  static const char* const kXna[] = {"XNA Framework", "Microsoft.Xna.Framework"};
  static const char* const kCry[] = {"CrySystem", "CryEngine", "CryRender"};
  static const char* const kRe[] = {"REEngine", "re_chunk", "re2_", "re7."};
  static const char* const kId[] = {"idRenderSystem", "idDeclManager", "idtech"};
  static const char* const kDecima[] = {"Decima", "Core::Platform::"};
  static const char* const kDefold[] = {"dmengine", "defold", "game.projectc"};
  static const char* const kCocos[] = {"cocos2d", "libcocos"};
  static const char* const kLove[] = {"love2d", "game.love", "lua51.dll"};

  scan_file file(path);
  if (!file.size()) return false;
  constexpr uint64_t head_limit = 64u << 20;
  constexpr uint64_t tail_limit = 16u << 20;
  constexpr size_t chunk_size = 1u << 20;
  // Overlap adjacent reads so a marker split across chunks is still visible.
  constexpr size_t overlap = 64;
  std::vector<u8> buffer(chunk_size + overlap);
  auto scan_range = [&](uint64_t start, uint64_t end) {
    for (uint64_t offset = start; offset < end; offset += chunk_size) {
      const size_t length = std::min<uint64_t>(end - offset, buffer.size());
      if (!file.read(offset, buffer.data(), length)) return false;
      auto any = [&](const auto& markers) {
        for (const char* marker : markers) {
          if (contains_bytes(buffer.data(), length, marker)) return true;
        }
        return false;
      };
      out->unreal = out->unreal || any(kUnreal);
      out->unity = out->unity || any(kUnity);
      out->godot = out->godot || any(kGodot);
      out->gamemaker = out->gamemaker || any(kGm);
      out->source = out->source || any(kSource);
      out->xna = out->xna || any(kXna);
      out->cryengine = out->cryengine || any(kCry);
      out->re_engine = out->re_engine || any(kRe);
      out->idtech = out->idtech || any(kId);
      out->decima = out->decima || any(kDecima);
      out->defold = out->defold || any(kDefold);
      out->cocos = out->cocos || any(kCocos);
      out->love2d = out->love2d || any(kLove);
    }
    return true;
  };

  const uint64_t head_end = std::min(file.size(), head_limit);
  if (!scan_range(0, head_end)) return false;
  if (file.size() > head_end) {
    const uint64_t tail_start = file.size() - std::min(file.size(), tail_limit);
    if (!scan_range(std::max(head_end - overlap, tail_start), file.size()))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Directory walk
// ---------------------------------------------------------------------------
struct exe_candidate {
  std::string rel_path;
  std::string full_path;
  uint64_t size = 0;
  int depth = 0;
  int score = 0;
};

void scan_dir_recursive(const std::string& base_dir, const std::string& rel_dir,
                        int depth, int max_depth,
                        std::vector<exe_candidate>* out_exes,
                        std::vector<std::string>* out_all_files) {
  if (depth > max_depth) return;
  std::string cur_full = rel_dir.empty() ? base_dir : path_join(base_dir, rel_dir);

#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(path_join(cur_full, "*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string fn = fd.cFileName;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    std::string child_rel = rel_dir.empty() ? fn : path_join(rel_dir, fn);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      std::string fn_low = lower_str(fn);
      if (fn_low != "thirdparty" && fn_low != "prereqs" && fn_low != "extras") {
        scan_dir_recursive(base_dir, child_rel, depth + 1, max_depth,
                           out_exes, out_all_files);
      }
    } else {
      if (out_all_files) out_all_files->push_back(child_rel);
      std::string fn_low = lower_str(fn);
      if (fn_low.size() >= 4 && fn_low.substr(fn_low.size() - 4) == ".exe") {
        if (!is_junk_executable(fn)) {
          exe_candidate cand;
          cand.rel_path = child_rel;
          cand.full_path = path_join(base_dir, child_rel);
          cand.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
          cand.depth = depth;
          out_exes->push_back(cand);
        }
      }
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(cur_full.c_str());
  if (!d) return;
  struct dirent* de;
  while ((de = readdir(d)) != nullptr) {
    std::string fn = de->d_name;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    std::string child_rel = rel_dir.empty() ? fn : path_join(rel_dir, fn);
    std::string child_full = path_join(base_dir, child_rel);

    struct stat st;
    if (stat(child_full.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      std::string fn_low = lower_str(fn);
      if (fn_low != "thirdparty" && fn_low != "prereqs" && fn_low != "extras") {
        scan_dir_recursive(base_dir, child_rel, depth + 1, max_depth,
                           out_exes, out_all_files);
      }
    } else if (S_ISREG(st.st_mode)) {
      if (out_all_files) out_all_files->push_back(child_rel);
      std::string fn_low = lower_str(fn);
      if (fn_low.size() >= 4 && fn_low.substr(fn_low.size() - 4) == ".exe") {
        if (!is_junk_executable(fn)) {
          exe_candidate cand;
          cand.rel_path = child_rel;
          cand.full_path = child_full;
          cand.size = (uint64_t)st.st_size;
          cand.depth = depth;
          out_exes->push_back(cand);
        }
      }
    }
  }
  closedir(d);
#endif
}

std::string parent_directory(const std::string& path) {
  const auto separator = path.find_last_of("/\\");
  return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

std::vector<std::string> collect_launch_imports(const std::string& game_dir,
                                                const std::string& executable,
                                                const pe_scan& main,
                                                const std::vector<std::string>& files) {
  const auto exe_dir = lower_str(parent_directory(executable));
  std::unordered_map<std::string, const std::string*> dll_paths;
  for (const auto& file : files) {
    if (!ends_with_ci(file, ".dll")) continue;
    const auto dir = lower_str(parent_directory(file));
    if (!dir.empty() && dir != exe_dir) continue;
    auto [existing, added] = dll_paths.emplace(lower_str(file), &file);
    if (!added && file < *existing->second) existing->second = &file;
  }

  std::unordered_set<std::string> modules;
  using pending_imports = std::pair<std::vector<std::string>, std::string>;
  std::deque<pending_imports> pending{{main.modules, exe_dir}};
  size_t remaining = 64;
  // Follow the runtime's import search: current DLL directory, then the
  // executable directory, then the mounted game root. Bound disk work and
  // let the nearest dependencies take priority when a title has many DLLs.
  while (!pending.empty()) {
    auto [imports, importer_dir] = std::move(pending.front());
    pending.pop_front();
    for (const auto& module : imports) {
      if (!modules.insert(module).second || !remaining ||
          module.find_first_of("/\\:") != std::string::npos ||
          module == "steam_api.dll" || module == "steam_api64.dll") continue;
      auto candidate = dll_paths.find(path_join(importer_dir, module));
      if (candidate == dll_paths.end())
        candidate = dll_paths.find(path_join(exe_dir, module));
      if (candidate == dll_paths.end()) candidate = dll_paths.find(module);
      if (candidate == dll_paths.end()) continue;
      --remaining;

      pe_scan dependency;
      const auto& relative = *candidate->second;
      if (scan_pe_file(path_join(game_dir, relative), &dependency) &&
          dependency.machine == main.machine && dependency.is_pe32 == main.is_pe32) {
        pending.emplace_back(std::move(dependency.modules),
                             lower_str(parent_directory(relative)));
      }
    }
  }
  std::vector<std::string> result(modules.begin(), modules.end());
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace

extern "C" {

const char* trt_engine_type_name(trt_engine_type t) {
  switch (t) {
    case TRT_ENGINE_UNITY_IL2CPP: return "Unity (IL2CPP)";
    case TRT_ENGINE_UNITY_MONO: return "Unity (Mono)";
    case TRT_ENGINE_UNREAL: return "Unreal Engine";
    case TRT_ENGINE_GODOT: return "Godot Engine";
    case TRT_ENGINE_GAMEMAKER: return "GameMaker";
    case TRT_ENGINE_FNA_XNA: return "FNA / XNA (Mono Host)";
    case TRT_ENGINE_SOURCE: return "Source Engine";
    case TRT_ENGINE_CUSTOM_NATIVE: return "Custom Native C/C++";
    case TRT_ENGINE_MANAGED_DOTNET: return "Managed (.NET / Mono Host)";
    case TRT_ENGINE_UNIVERSAL: return "Universal (One Engine For Everything)";
    default: return "Unknown Engine";
}

  }
const char* trt_graphics_api_name(trt_graphics_api g) {
  switch (g) {
    case TRT_GFX_D3D11: return "Direct3D 11 (Passthrough)";
    case TRT_GFX_D3D12: return "Direct3D 12 (Recording/Submit)";
    case TRT_GFX_D3D9: return "Direct3D 9 (d3d9to11 Bridge)";
    case TRT_GFX_VULKAN: return "Vulkan (Tiramisu ICD)";
    case TRT_GFX_OPENGL: return "OpenGL (WGL Bridge)";
    default: return "Auto / Default";
}

  }
const char* trt_audio_middleware_name(trt_audio_middleware a) {
  switch (a) {
    case TRT_AUDIO_FMOD: return "FMOD Studio / LowLevel";
    case TRT_AUDIO_XAUDIO2: return "XAudio2";
    case TRT_AUDIO_WASAPI: return "WASAPI Core";
    case TRT_AUDIO_OPENAL: return "OpenAL Soft";
    case TRT_AUDIO_WWISE: return "Wwise (AkSoundEngine)";
    default: return "DirectSound / System";
}

  }
uint32_t trt_steam_read_appid(const char* game_dir) {
  if (!game_dir || !game_dir[0]) return 0;
  std::ifstream file(path_join(game_dir, "steam_appid.txt"), std::ios::binary);
  char bytes[64];
  file.read(bytes, sizeof(bytes));
  if (!file.eof() || file.bad()) return 0;
  const auto length = static_cast<size_t>(file.gcount());
  size_t start = 0;
  size_t end = length;
  while (start < end && std::isspace(static_cast<unsigned char>(bytes[start]))) ++start;
  while (end > start && std::isspace(static_cast<unsigned char>(bytes[end - 1]))) --end;
  if (end == start || end - start > 10) return 0;
  uint64_t value = 0;
  while (start < end) {
    const char digit = bytes[start++];
    if (digit < '0' || digit > '9') return 0;
    value = value * 10 + static_cast<unsigned>(digit - '0');
  }

  // Older releases wrote Spacewar's AppID for unrelated games. A legacy
  // sidecar alone cannot distinguish that placeholder from a real install.
  constexpr uint32_t kLegacyPlaceholderAppId = 480;
  if (value == kLegacyPlaceholderAppId || value > UINT32_MAX) return 0;
  return static_cast<uint32_t>(value);
}

int trt_pe_scan_import_modules(const char* path, char* out_csv, size_t cap) {
  if (out_csv && cap) out_csv[0] = 0;
  if (!path || !path[0]) return -1;
  pe_scan scan;
  if (!scan_pe_file(path, &scan)) return -1;
  if (out_csv && cap) {
    size_t used = 0;
    for (size_t i = 0; i < scan.modules.size(); ++i) {
      const std::string& m = scan.modules[i];
      const size_t need = m.size() + (i ? 1 : 0) + 1;
      if (used + need > cap) break;
      if (i) out_csv[used++] = ',';
      memcpy(out_csv + used, m.data(), m.size());
      used += m.size();
      out_csv[used] = 0;
    }
  }
  return (int)scan.modules.size();
}

static bool synthesize_engine(const char* game_dir, const char* target_exe_hint,
                              trt_engine_profile* out_profile, bool exact) {
  if (!game_dir || !game_dir[0] || !out_profile) return false;
  *out_profile = {};

  std::string base_dir = game_dir;
  // Clean trailing separator
  while (base_dir.size() > 1 && (base_dir.back() == '/' || base_dir.back() == '\\')) {
    base_dir.pop_back();

  // Extract base game name
  }
  std::string game_name;
  size_t last_slash = base_dir.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    game_name = base_dir.substr(last_slash + 1);
  } else {
    game_name = base_dir;

  }
  out_profile->game_name = game_name;
  out_profile->game_dir = base_dir;

  const std::string hint = normalize_exe_hint(target_exe_hint ? target_exe_hint : "");
  const std::string hint_lower = lower_str(hint);

  // Directory launches search the bundle for candidates. An exact file
  // only needs its sibling DLLs and the engine payload checks below.
  std::vector<exe_candidate> exes;
  std::vector<std::string> all_files;
  scan_dir_recursive(base_dir, "", 0, exact ? 0 : 5, &exes, &all_files);

  // If a target executable hint was provided, ensure it is considered even if
  // directory recursion missed it or truncated early.
  if (!hint.empty() && file_exists(path_join(base_dir, hint))) {
    const auto found = std::find_if(exes.begin(), exes.end(), [&](const exe_candidate& c) {
      return c.rel_path == hint;
    });
    if (found == exes.end()) {
      exe_candidate cand{};
      cand.rel_path = hint;
      cand.full_path = path_join(base_dir, hint);
      exes.push_back(cand);
    }
  }

  // 2. Score executable candidates to identify the true primary binary
  std::string g_low = lower_str(game_name);
  for (auto& cand : exes) {
    cand.score = 100;
    std::string fn = cand.rel_path;
    size_t s_pos = fn.find_last_of("/\\");
    if (s_pos != std::string::npos) fn = fn.substr(s_pos + 1);
    std::string stem = fn;
    if (stem.size() >= 4 && stem.substr(stem.size() - 4) == ".exe") {
      stem = stem.substr(0, stem.size() - 4);
    }
    std::string stem_low = lower_str(stem);

    // Exact name match or shipping name match
    if (stem_low == g_low) cand.score += 300;
    if (stem_low == g_low + "-win64-shipping") cand.score += 350;
    if (stem_low.find(g_low) != std::string::npos) cand.score += 150;

    // Prefer root or 1-level deep binaries
    if (cand.depth == 0) cand.score += 80;
    else if (cand.depth == 1) cand.score += 40;
    else cand.score -= cand.depth * 25;

    // Prefer substantial file sizes
    if (cand.size > 20000000) cand.score += 60; // > 20MB
    else if (cand.size > 2000000) cand.score += 30; // > 2MB
    else if (cand.size < 100000) cand.score -= 50; // tiny helper stub

  }
  std::sort(exes.begin(), exes.end(), [&](const exe_candidate& a, const exe_candidate& b) {
    const bool a_hint = !hint.empty() && lower_str(a.rel_path) == hint_lower;
    const bool b_hint = !hint.empty() && lower_str(b.rel_path) == hint_lower;
    if (a_hint != b_hint) return a_hint;
    if (a_hint && (a.rel_path == hint) != (b.rel_path == hint))
      return a.rel_path == hint;
    if (a.score != b.score) return a.score > b.score;
    return a.rel_path < b.rel_path;
  });

  // 3. Inspect candidates in score order until one is a readable PE. A
  // launcher, an uninstaller, or a truncated copy at the top of the ranking
  // must not hide the real game binary behind it, and an exe that is not a PE
  // at all is refused by name instead of being guessed as PE64 (that guess
  // used to reach the loader as a wrong-arch launch).
  pe_scan scan;
  exe_candidate chosen;
  bool scanned = false;
  std::string rejected;
  for (const auto& cand : exes) {
    if (exact && cand.rel_path != hint) continue;
    pe_scan s;
    if (scan_pe_file(cand.full_path, &s) && s.is_guest_exe) {
      chosen = cand;
      scan = s;
      scanned = true;
      break;
    }
    rejected += (rejected.empty() ? "" : ", ") + cand.rel_path;
  }
  if (!scanned) {
    char line[900];
    if (rejected.empty())
      snprintf(line, sizeof(line),
               "synthesizer: REFUSED %s - no executable found to synthesize",
               game_name.c_str());
    else
      snprintf(line, sizeof(line),
               "synthesizer: REFUSED %s - no supported x86/x64 PE executable (%s); "
               "architecture is never guessed",
               game_name.c_str(), rejected.c_str());
    trt_core_log(line);
    return false;
  }
  out_profile->main_exe_rel = chosen.rel_path;
  out_profile->main_exe_abs = chosen.full_path;
  out_profile->pe_scanned = true;
  out_profile->is_pe32 = scan.is_pe32;
  out_profile->is_pe64 = scan.is_pe64;
  out_profile->is_managed = scan.is_managed;
  // An explicit launch hint can select an executable outside the bounded
  // discovery walk. Inventory its directory too, without opening up a second
  // recursive search, so the import scan sees the DLLs the loader can use.
  if (std::find(all_files.begin(), all_files.end(), chosen.rel_path) ==
      all_files.end()) {
    const auto launch_dir = parent_directory(chosen.rel_path);
    if (!launch_dir.empty()) {
      std::vector<exe_candidate> ignored;
      scan_dir_recursive(base_dir, launch_dir, 0, 0, &ignored, &all_files);
    }
  }
  out_profile->imported_modules =
      collect_launch_imports(base_dir, chosen.rel_path, scan, all_files);
  auto mod_is = [&](const char* name) {
    const std::string want = lower_str(name);
    return std::binary_search(out_profile->imported_modules.begin(),
                              out_profile->imported_modules.end(), want);
  };
  auto mod_prefix = [&](const char* prefix) {
    const std::string p = lower_str(prefix);
    for (const auto& m : out_profile->imported_modules) {
      if (m.compare(0, p.size(), p) == 0) return true;
    }
    return false;
  };
  auto file_suffix = [&](const char* suffix) {
    const std::string s = lower_str(suffix);
    for (const auto& f : all_files) {
      if (f.size() >= s.size() &&
          std::equal(s.rbegin(), s.rend(), f.rbegin(),
                     [](char lhs, char rhs) { return lhs == lower_char(rhs); })) {
        return true;
      }
    }
    return false;
  };

  // 4. Engine classification - imports first, disk contents second.
  const bool is_managed = out_profile->is_managed;
  // Universal mode: no engine evidence + no is_managed -> TRT_ENGINE_UNIVERSAL.
  // PC games are also universal-first (the universal guest is the "one engine
  // for everything"): classify every unmanaged, no-evidence game as universal
  // unless a higher-evidence engine is found below.
  const bool no_engine_evidence = !is_managed &&
      out_profile->engine_type == TRT_ENGINE_UNKNOWN;
  const bool imp_unity_player = mod_is("unityplayer.dll");
  const bool imp_il2cpp = mod_is("gameassembly.dll");
  const bool imp_mono = mod_prefix("mono-2.0") || mod_is("mono.dll");
  const bool imp_fna = mod_is("fna.dll");
  const bool imp_monogame = mod_prefix("monogame.framework");
  const bool imp_xna = mod_prefix("microsoft.xna.framework");

  pe_signatures sig;
  const bool have_sig = scan_signatures(chosen.full_path, &sig);

  const bool disk_unity_player = file_suffix("unityplayer.dll");
  const bool disk_il2cpp = file_suffix("gameassembly.dll");
  const bool disk_mono =
      file_suffix("mono-2.0.dll") || file_suffix("assembly-csharp.dll") ||
      file_suffix("monobleedingedge.dll");
  const bool disk_fna = file_suffix("fna.dll") || file_suffix("monogame.framework.dll");

  const bool is_unity_il2cpp = imp_il2cpp || disk_il2cpp;
  const bool is_unity_mono =
      imp_unity_player || imp_mono || disk_unity_player || disk_mono ||
      (have_sig && sig.unity);
  const bool is_fna = imp_fna || imp_monogame || imp_xna || disk_fna ||
                      (have_sig && sig.xna);

  // Unreal: monolithic shipping exe, .pak containers, or explicit UE modules.
  const bool is_unreal =
      mod_prefix("ue4game") || mod_prefix("ue5game") || mod_prefix("unrealengine") ||
      file_suffix("-win64-shipping.exe") || file_suffix("-win32-shipping.exe") ||
      file_suffix(".pak") || file_suffix(".utoc") || file_suffix(".ucas") ||
      (have_sig && sig.unreal);
  // Godot: .pck container, a godot runtime module, or the engine's own strings.
  const bool is_godot = mod_prefix("godot") || file_suffix(".pck") ||
                        file_suffix("godotsharp.dll") || (have_sig && sig.godot);
  // GameMaker: the compiled game is data.win / game.unx; nothing else counts.
  const bool is_gamemaker = file_suffix("data.win") || file_suffix("game.unx") ||
                            file_suffix("game.ios") || (have_sig && sig.gamemaker);
  // Source: VPK containers, the tier0/vstdlib pair, or the engine's banner.
  const bool is_source = mod_is("tier0.dll") || mod_is("vstdlib.dll") ||
                         file_suffix(".vpk") || file_suffix("engine.dll") ||
                         (have_sig && sig.source);

  const bool is_cry = mod_prefix("crysystem") || mod_prefix("cryrender") ||
                      file_suffix("crysystem.dll") || (have_sig && sig.cryengine);
  const bool is_re = (have_sig && sig.re_engine) || file_suffix("re_chunk_000.pak");
  const bool is_id = (have_sig && sig.idtech);
  const bool is_decima = (have_sig && sig.decima);
  const bool is_defold = (have_sig && sig.defold) || file_suffix("game.projectc");
  const bool is_cocos = (have_sig && sig.cocos) || mod_prefix("cocos2d") || file_suffix("cocos2d.dll");
  const bool is_love = (have_sig && sig.love2d) || file_suffix("game.love");

  if (is_managed && is_fna) {
    out_profile->engine_type = TRT_ENGINE_FNA_XNA;
  } else if (is_managed && is_unity_mono) {
    out_profile->engine_type = TRT_ENGINE_UNITY_MONO;
  } else if (is_managed) {
    out_profile->engine_type = TRT_ENGINE_MANAGED_DOTNET;
  } else if (is_unity_il2cpp) {
    out_profile->engine_type = TRT_ENGINE_UNITY_IL2CPP;
  } else if (is_unity_mono) {
    out_profile->engine_type = TRT_ENGINE_UNITY_MONO;
  } else if (is_fna) {
    out_profile->engine_type = TRT_ENGINE_FNA_XNA;
  } else if (is_unreal) {
    out_profile->engine_type = TRT_ENGINE_UNREAL;
  } else if (is_godot) {
    out_profile->engine_type = TRT_ENGINE_GODOT;
  } else if (is_gamemaker) {
    out_profile->engine_type = TRT_ENGINE_GAMEMAKER;
  } else if (is_source) {
    out_profile->engine_type = TRT_ENGINE_SOURCE;
  } else if (is_cry || is_re || is_id || is_decima || is_defold || is_cocos || is_love) {
    out_profile->engine_type = TRT_ENGINE_CUSTOM_NATIVE;
  } else if (is_managed) {
    // Managed with no engine evidence — already covered above, but keep a
    // defensive fallback for future expansion.
    out_profile->engine_type = TRT_ENGINE_MANAGED_DOTNET;
  } else if (no_engine_evidence) {
    out_profile->engine_type = TRT_ENGINE_UNIVERSAL;
  } else {
    out_profile->engine_type = TRT_ENGINE_CUSTOM_NATIVE;
  }
  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    out_profile->universal_flags |= TRT_UNIV_CLEAR_PC;  // PC games: universal-first
  }

  // 5. Graphics API from the real imports. Engines whose mature Tiramisu path
  //    is D3D11 keep it when the binary imports d3d11, even if d3d12 is also
  //    linked in for a SM6 path. DXGI is shared by both renderers.
  const bool imp_d12 = mod_is("d3d12.dll");
  const bool imp_d11 = mod_is("d3d11.dll") || mod_is("d3d10_1.dll") ||
                       mod_is("d3d10.dll");
  const bool imp_d9 = mod_is("d3d9.dll") || mod_is("d3d8.dll") || mod_is("ddraw.dll");
  const bool imp_vk = mod_is("vulkan-1.dll");
  const bool imp_gl = mod_is("opengl32.dll");

  const bool prefer_d3d11 = out_profile->engine_type == TRT_ENGINE_UNITY_IL2CPP ||
                            out_profile->engine_type == TRT_ENGINE_UNITY_MONO ||
                            out_profile->engine_type == TRT_ENGINE_UNREAL ||
                            out_profile->engine_type == TRT_ENGINE_FNA_XNA;

  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    // Universal guest: prefer a GPU-backed API when the game's imports hint
    // at one (the universal host/guest translates PC imports to Xbox
    // equivalents; this picks the right GPU path to translate into).
    if (imp_d11) out_profile->preferred_gfx = TRT_GFX_D3D11;
    else if (imp_d12) out_profile->preferred_gfx = TRT_GFX_D3D12;
    else if (imp_vk) out_profile->preferred_gfx = TRT_GFX_VULKAN;
    else if (imp_d9) out_profile->preferred_gfx = TRT_GFX_D3D9;
    else if (imp_gl) out_profile->preferred_gfx = TRT_GFX_OPENGL;
  } else if (out_profile->engine_type == TRT_ENGINE_GODOT && imp_vk) {
    out_profile->preferred_gfx = TRT_GFX_VULKAN;
  } else if (prefer_d3d11 && imp_d11) {
    out_profile->preferred_gfx = TRT_GFX_D3D11;
  } else if (imp_d12) {
    out_profile->preferred_gfx = TRT_GFX_D3D12;
  } else if (imp_d11) {
    out_profile->preferred_gfx = TRT_GFX_D3D11;
  } else if (imp_vk) {
    out_profile->preferred_gfx = TRT_GFX_VULKAN;
  } else if (imp_d9) {
    out_profile->preferred_gfx = TRT_GFX_D3D9;
  } else if (imp_gl) {
    out_profile->preferred_gfx = TRT_GFX_OPENGL;
  } else {
    out_profile->preferred_gfx = TRT_GFX_D3D11; // bridge default
  }

  // 5b. Universal guest prefers its own abstracted graphics path by default
  //     (the universal guest is the "one engine"; PC imports translate in the
  //     host/guest at launch, not in the engine profile).
  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    if (imp_d11) out_profile->preferred_gfx = TRT_GFX_D3D11;
    else if (imp_d12) out_profile->preferred_gfx = TRT_GFX_D3D12;
    else if (imp_vk) out_profile->preferred_gfx = TRT_GFX_VULKAN;
    else if (imp_d9) out_profile->preferred_gfx = TRT_GFX_D3D9;
    else if (imp_gl) out_profile->preferred_gfx = TRT_GFX_OPENGL;
  }

  // 6. Audio middleware from the real imports.
  const bool imp_fmod = mod_is("fmod.dll") || mod_is("fmod64.dll") ||
                        mod_is("fmodstudio.dll") || mod_is("fmodstudio64.dll") ||
                        mod_prefix("fmod");
  const bool imp_wwise = mod_prefix("aksoundengine") || mod_prefix("akaudio");
  const bool imp_xaudio = mod_prefix("xaudio2") || mod_is("xaudio2.dll");
  const bool imp_openal = mod_is("openal32.dll");

  if (imp_fmod) out_profile->preferred_audio = TRT_AUDIO_FMOD;
  else if (imp_wwise) out_profile->preferred_audio = TRT_AUDIO_WWISE;
  else if (imp_xaudio) out_profile->preferred_audio = TRT_AUDIO_XAUDIO2;
  else if (imp_openal) out_profile->preferred_audio = TRT_AUDIO_OPENAL;
  else out_profile->preferred_audio = TRT_AUDIO_WASAPI;

  // 7. Engine-specific command line (only arguments that engine actually parses).
  //    Screen size is part of it: the console renders into a 1080p-class
  //    compositor inside a 5 GB Dev-Mode budget, so a title left on its
  //    4K/high-VRAM defaults spends that budget on buffers it cannot show.
  {
    const char* env_screen = getenv("TRT_SCREEN");
    int sw = 0, sh = 0;
    if (env_screen && sscanf(env_screen, "%dx%d", &sw, &sh) == 2 && sw >= 320 &&
        sh >= 240 && sw <= 3840 && sh <= 2160) {
      out_profile->screen_width = sw;
      out_profile->screen_height = sh;
    }
  }
  const std::string sw = std::to_string(out_profile->screen_width);
  const std::string sh = std::to_string(out_profile->screen_height);
  switch (out_profile->engine_type) {
    case TRT_ENGINE_UNITY_IL2CPP:
    case TRT_ENGINE_UNITY_MONO:
      if (out_profile->preferred_gfx == TRT_GFX_D3D11)
        out_profile->cmdline_args.push_back("-force-d3d11");
      out_profile->cmdline_args.push_back("-screen-width");
      out_profile->cmdline_args.push_back(sw);
      out_profile->cmdline_args.push_back("-screen-height");
      out_profile->cmdline_args.push_back(sh);
      out_profile->cmdline_args.push_back("-screen-fullscreen");
      out_profile->cmdline_args.push_back(out_profile->fullscreen ? "1" : "0");
      out_profile->cmdline_args.push_back("-logFile");
      out_profile->cmdline_args.push_back("C:\\Temp\\unity.log");
      break;
    case TRT_ENGINE_UNREAL:
      out_profile->cmdline_args.push_back("-windowed");
      out_profile->cmdline_args.push_back("-ResX=" + sw);
      out_profile->cmdline_args.push_back("-ResY=" + sh);
      out_profile->cmdline_args.push_back("-NoVerifyGC");
      if (out_profile->preferred_gfx == TRT_GFX_D3D12) {
        out_profile->cmdline_args.push_back("-d3d12");
      } else if (out_profile->preferred_gfx == TRT_GFX_VULKAN) {
        out_profile->cmdline_args.push_back("-vulkan");
      } else {
        out_profile->cmdline_args.push_back("-dx11");
      }
      break;
    default:
      break;

  // 8. Steam: a real steam_api import (or the DLL on disk) needs steam_appid.txt.
  }
  const bool has_steam_api = mod_is("steam_api64.dll") || mod_is("steam_api.dll") ||
                             file_suffix("steam_api64.dll") || file_suffix("steam_api.dll");
  out_profile->has_steam = has_steam_api;
  out_profile->steam_appid = trt_steam_read_appid(base_dir.c_str());
  // TRT_SYNTH_DRY_RUN permits classification without writing a profile.
  const bool dry_run = getenv("TRT_SYNTH_DRY_RUN") != nullptr;

  // 8b. Payload closure: step 4 classified the exe; this proves the engine
  //     data it loads next is actually here. An exe copied without its engine
  //     payload still classifies as Unity/UE and then dies in its own loader
  //     ("Failed to load mono"), so every missing piece is named now.
  {
    const size_t exe_sl2 = chosen.rel_path.find_last_of("/\\");
    std::string exe_fn = (exe_sl2 == std::string::npos)
                             ? chosen.rel_path
                             : chosen.rel_path.substr(exe_sl2 + 1);
    std::string stem = exe_fn;
    if (ends_with_ci(stem, ".exe")) stem = stem.substr(0, stem.size() - 4);
    const size_t parent_end = chosen.full_path.find_last_of("/\\");
    const std::string exe_dir = parent_end == std::string::npos
                                    ? base_dir : chosen.full_path.substr(0, parent_end);
    auto find_dir = [](const std::vector<std::string>& children,
                       const std::string& want) {
      for (const auto& k : children) {
        if (lower_str(k) == lower_str(want)) return k;
      }
      return std::string();
    };
    auto add_missing = [&](const std::string& what) {
      out_profile->missing_payload.push_back(what);
    };

    if (out_profile->engine_type == TRT_ENGINE_UNITY_IL2CPP ||
        out_profile->engine_type == TRT_ENGINE_UNITY_MONO) {
      const auto exe_children = list_child_dirs(exe_dir);
      const std::string data = find_dir(exe_children, stem + "_Data");
      if (data.empty()) {
        add_missing(stem + "_Data (Unity engine data: scenes, assets, managed code)");
      } else {
        const std::string data_dir = path_join(exe_dir, data);
        const std::vector<std::string> data_kids = list_child_dirs(data_dir);
        auto data_has = [&](const std::string& want) {
          for (const auto& k : data_kids)
            if (lower_str(k) == lower_str(want)) return true;
          return false;
        };
        if (out_profile->is_managed ||
            out_profile->engine_type == TRT_ENGINE_UNITY_MONO) {
          const bool managed = data_has("Managed");
          const bool embedded_mono = data_has("Mono");
          const bool bleeding =
              !find_dir(exe_children, "MonoBleedingEdge").empty() || data_has("MonoBleedingEdge");
          if (!managed && !embedded_mono && !bleeding) {
            add_missing(data + "/Managed (scripts) or " + data +
                        "/Mono/EmbedRuntime (runtime)");
          }
        }
        if (out_profile->engine_type == TRT_ENGINE_UNITY_IL2CPP) {
          if (!data_has("il2cpp_data") && !file_exists(path_join(exe_dir, "GameAssembly.dll"))) {
            add_missing(data + "/il2cpp_data and GameAssembly.dll (IL2CPP code)");
          }
        }
      }
    } else if (out_profile->engine_type == TRT_ENGINE_UNREAL) {
      if (!file_suffix(".pak") && !file_suffix(".utoc") && !file_suffix(".ucas") &&
          find_dir(list_child_dirs(base_dir), "Content").empty()) {
        add_missing("Content/Paks/*.pak (Unreal assets)");
      }
    } else if (out_profile->engine_type == TRT_ENGINE_FNA_XNA) {
      if (!file_suffix(".xnb") && find_dir(list_child_dirs(base_dir), "Content").empty()) {
        add_missing("Content/ (*.xnb assets)");
      }
    } else if (out_profile->engine_type == TRT_ENGINE_GODOT &&
               !file_suffix(".pck")) {
      trt_core_log("synthesizer: godot payload assumed embedded in the exe "
                   "(no .pck beside it)");
    }
    out_profile->payload_complete = out_profile->missing_payload.empty();
    if (!out_profile->payload_complete) {
      std::string joined;
      for (const auto& m : out_profile->missing_payload)
        joined += (joined.empty() ? "" : " | ") + m;
      char line[900];
      snprintf(line, sizeof(line),
               "synthesizer: PAYLOAD INCOMPLETE for %s - missing %s",
               out_profile->main_exe_rel.c_str(), joined.c_str());
      trt_core_log(line);
    }
  }

  // 9. Shims: the bridge modules that will service this title's imports.
  auto add_shim = [&](const char* module, const char* bridge) {
    std::string s = std::string(module) + ":" + bridge;
    if (std::find(out_profile->synthetic_shims.begin(),
                  out_profile->synthetic_shims.end(), s) ==
        out_profile->synthetic_shims.end()) {
      out_profile->synthetic_shims.push_back(s);
    }
  };
  switch (out_profile->preferred_gfx) {
    case TRT_GFX_D3D12: add_shim("d3d12.dll", "bridge_d3d12"); break;
    case TRT_GFX_D3D9: add_shim("d3d9.dll", "bridge_d3d9"); break;
    case TRT_GFX_VULKAN: add_shim("vulkan-1.dll", "bridge_vulkan"); break;
    case TRT_GFX_OPENGL: add_shim("opengl32.dll", "bridge_opengl32"); break;
    default: add_shim("d3d11.dll", "bridge_d3d"); break;
  }
  if (imp_d9 && out_profile->preferred_gfx != TRT_GFX_D3D9) {
    add_shim("d3d9.dll", "bridge_d3d9"); // a D3D9 fallback path still runs
  }
  switch (out_profile->preferred_audio) {
    case TRT_AUDIO_FMOD:
      add_shim("fmod.dll", "bridge_fmod");
      add_shim("fmodstudio.dll", "bridge_fmod_studio");
      break;
    case TRT_AUDIO_WWISE: add_shim("aksoundengine.dll", "bridge_wwise"); break;
    case TRT_AUDIO_OPENAL: add_shim("openal32.dll", "bridge_openal"); break;
    case TRT_AUDIO_XAUDIO2: add_shim("xaudio2.dll", "bridge_audio"); break;
    default: add_shim("wasapi", "bridge_wasapi"); break;
  }
  if (has_steam_api) add_shim("steam_api64.dll", "bridge_steam");
  if (is_managed) add_shim("mscoree.dll", "mono_host");

  // 10. Persist .tiramisu/profile.json (valid JSON: every string escaped) and
  //     .tiramisu/shims.txt. The preflight rewrites the same file with its
  //     verdict under "preflight".
  std::string meta_dir = path_join(base_dir, ".tiramisu");
  if (!dry_run) {
    make_directory(meta_dir);
    if (!write_profile_json(base_dir, out_profile, nullptr)) {
      trt_core_log("synthesizer: profile.json could not be written (game folder readonly?)");
    }

    std::string shims_path = path_join(meta_dir, "shims.txt");
    FILE* sf = fopen(shims_path.c_str(), "w");
    if (sf) {
      fprintf(sf, "# tiramisu shims for %s\n", game_name.c_str());
      for (const auto& s : out_profile->synthetic_shims) {
        fprintf(sf, "%s\n", s.c_str());
      }
      fclose(sf);
    }
  }

  // Build user-visible summary string
  char sum[512];
  snprintf(sum, sizeof(sum),
           "synthesizer: auto-configured %s -> %s [%s] gfx=%s audio=%s steam=%u imports=%zu sig=%s",
           game_name.c_str(), out_profile->main_exe_rel.c_str(),
           trt_engine_type_name(out_profile->engine_type),
           trt_graphics_api_name(out_profile->preferred_gfx),
           trt_audio_middleware_name(out_profile->preferred_audio),
           out_profile->steam_appid, out_profile->imported_modules.size(),
           !have_sig ? "none"
                     : (sig.unreal ? "unreal"
                                   : (sig.unity ? "unity"
                                                : (sig.godot ? "godot"
                                                             : (sig.gamemaker
                                                                    ? "gamemaker"
                                                                    : (sig.source
                                                                           ? "source"
                                                                           : (sig.xna ? "xna" : "none")))))));
  out_profile->summary = sum;
  trt_core_log(sum);

  return true;
}

bool trt_synthesize_engine(const char* game_dir, const char* target_exe_hint,
                           trt_engine_profile* out_profile) {
  return synthesize_engine(game_dir, target_exe_hint, out_profile, false);
}

bool trt_synthesize_engine_for_file(const char* exe_path,
                                    trt_engine_profile* out_profile) {
  if (!exe_path || !exe_path[0] || !out_profile) return false;
  std::string full_path = exe_path;
  size_t last_slash = full_path.find_last_of("/\\");
  std::string dir = last_slash == std::string::npos
                        ? "." : full_path.substr(0, std::max<size_t>(1, last_slash));
  std::string fn = (last_slash != std::string::npos) ? full_path.substr(last_slash + 1) : full_path;
  return synthesize_engine(dir.c_str(), fn.c_str(), out_profile, true);
}

int trt_autonomous_provision_runtime(const trt_backend* be, const trt_engine_profile* prof) {
  if (be) {
    trt_core_set_backend(be);
  }

  // 1. Ensure all graphics backends & translation layers are provisioned
  trt_bridge_register_d3d();
  trt_bridge_register_d3d12();
  trt_bridge_register_d3d9();
  trt_bridge_register_d3dx9();
  trt_bridge_register_d3d8();
  trt_bridge_register_ddraw();
  trt_bridge_register_vulkan();
  trt_bridge_register_opengl32();
  trt_bridge_register_nvapi();
  trt_bridge_register_d3dx();
  trt_bridge_register_gdiplus();

  // 2. Ensure audio middleware is provisioned
  trt_bridge_register_audio();
  trt_bridge_register_dsound();
  trt_bridge_register_openal();
  trt_bridge_register_wasapi();
  trt_bridge_register_fmod();
  trt_bridge_register_wwise();
  trt_bridge_register_mmedia();

  // 3. Ensure game & video middleware are provisioned
  trt_bridge_register_bink();
  trt_bridge_register_criware();
  trt_bridge_register_miles();
  trt_bridge_register_oodle();
  trt_bridge_register_dstorage();
  trt_bridge_register_mf();

  // 4. Ensure input & platform bridges are provisioned
  trt_bridge_register_gameinput();
  trt_bridge_register_xinput();
  trt_bridge_register_dinput();
  trt_bridge_register_hid();
  trt_bridge_register_winrt();
  trt_bridge_register_steam();
  trt_bridge_register_comctl();
  trt_bridge_register_ktmw32();
  trt_bridge_register_wldap32();
  trt_bridge_register_sys_extras();

  // 5. Ensure core OS subsystems & network/security bridges are provisioned
  trt_bridge_register_psapi();
  trt_bridge_register_ws2();
  trt_bridge_register_ole();
  trt_bridge_register_shell();
  trt_bridge_register_shlwapi();
  trt_bridge_register_actctx();
  trt_bridge_register_imm32();
  trt_bridge_register_wininet();
  trt_bridge_register_iphlpapi();
  trt_bridge_register_crypt32();
  trt_bridge_register_prng();
  trt_bridge_register_dbghelp();
  trt_bridge_register_winmm();
  trt_bridge_register_gdi();
  trt_bridge_register_version();

  // 6. Pre-register ubiquitous third-party gaming middleware as STUB
  //    so LoadLibrary succeeds with a synthetic handle and GetProcAddress
  //    resolves via trt_dynamic_synthesize_api (never crashes at runtime)
  static const char* const k_gaming_middleware[] = {
      "discord_game_sdk.dll",
      "discord-rpc.dll",
      "eossdk-win64-shipping.dll",
      "eossdk-win32-shipping.dll",
      "eossdk.dll",
      "galaxy64.dll",
      "galaxy.dll",
      "gfsdk_aftermath_lib.x64.dll",
      "gfsdk_ssao_d3d11.win64.dll",
      "gfsdk_txaa_alpharesolve.win64.dll",
      "gfsdk_godrays_dx11.x64.dll",
      "anselsdk64.dll",
      "sl.interposer.dll",
      "sl.dlss.dll",
      "sl.common.dll",
      "sl.reflex.dll",
      "nvngx_dlss.dll",
      "nvngx.dll",
      "ffx_fsr2_api_dx12_x64.dll",
      "ffx_fsr2_api_x64.dll",
      "ffx_backend_dx12_x64.dll",
      "libxess.dll",
      "rzchromasdk64.dll",
      "rzchromasdk.dll",
      "cuesdk.x64_2015.dll",
      "cuesdk_2015.dll",
      "logitechgkey.dll",
      "logitechled.dll",
      "openvr_api.dll",
      "openxr_loader.dll",
      "sentry.dll",
      "bugsplat64.dll",
      "bugsplat.dll",
      "breakpad_client.dll",
      "vivoxcore.dll",
      "ortp.dll",
      "speex.dll",
      "easyanticheat_x64.dll",
      "beclient_x64.dll",
  };
  for (size_t i = 0; i < sizeof(k_gaming_middleware) / sizeof(k_gaming_middleware[0]); ++i) {
    if (trt_module_route(k_gaming_middleware[i]) == TRT_ROUTE_MISSING) {
      if (prof && !prof->game_dir.empty()) {
        std::string p = path_join(prof->game_dir, k_gaming_middleware[i]);
        if (file_exists(p)) continue;
      }
      trt_module_register(k_gaming_middleware[i], TRT_ROUTE_STUB, nullptr, 0);
    }
  }

  if (!prof) return 0;

  // 6. Blanket unknown imported modules as STUB for fail-visible dynamic synthesis
  for (const auto& mod : prof->imported_modules) {
    if (mod.empty()) continue;
    trt_route rt = trt_module_route(mod.c_str());
    if (rt == TRT_ROUTE_MISSING) {
      // Check if this module physically exists on disk in the game directory or exe dir.
      // Game-shipped binaries (UnityPlayer.dll, GameAssembly.dll, mono-2.0-bdwgc.dll, etc.)
      // must NEVER be blanketed as STUB, or their exports (UnityMain) will resolve to
      // dummy no-ops that return 0 immediately instead of executing the game.
      if (!prof->game_dir.empty()) {
        std::string p1 = path_join(prof->game_dir, mod);
        if (file_exists(p1) || file_exists(p1 + ".dll")) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "trt: [AUTONOMOUS SYNTHESIS] preserving native game DLL %s (found in game dir)",
                   mod.c_str());
          trt_core_log(msg);
          continue;
        }
      }
      if (!prof->main_exe_abs.empty()) {
        size_t sl = prof->main_exe_abs.find_last_of("/\\");
        if (sl != std::string::npos) {
          std::string exe_parent = prof->main_exe_abs.substr(0, sl);
          std::string p2 = path_join(exe_parent, mod);
          if (file_exists(p2) || file_exists(p2 + ".dll")) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "trt: [AUTONOMOUS SYNTHESIS] preserving native game DLL %s (found in exe dir)",
                     mod.c_str());
            trt_core_log(msg);
            continue;
          }
        }
      }
#ifndef _WIN32
      if (!prof->game_dir.empty()) {
        std::string m_low = lower_str(mod);
        std::string m_low_dll = (m_low.size() >= 4 && m_low.substr(m_low.size() - 4) == ".dll")
                                    ? m_low
                                    : (m_low + ".dll");
        DIR* d = opendir(prof->game_dir.c_str());
        if (d) {
          bool found = false;
          struct dirent* de;
          while ((de = readdir(d)) != nullptr) {
            std::string fn = lower_str(de->d_name);
            if (fn == m_low || fn == m_low_dll) {
              found = true;
              break;
            }
          }
          closedir(d);
          if (found) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "trt: [AUTONOMOUS SYNTHESIS] preserving native game DLL %s (found in dir ci)",
                     mod.c_str());
            trt_core_log(msg);
            continue;
          }
        }
      }
#endif
      trt_module_register(mod.c_str(), TRT_ROUTE_STUB, nullptr, 0);
      char msg[256];
      snprintf(msg, sizeof(msg),
               "trt: [AUTONOMOUS SYNTHESIS] auto-provisioned blanket stub module %s",
               mod.c_str());
      trt_core_log(msg);
    }
  }

  char log_buf[512];
  snprintf(log_buf, sizeof(log_buf),
           "trt: [AUTONOMOUS SYNTHESIS] provisioned runtime for %s (engine=%s, gfx=%s, audio=%s)",
           prof->game_name.empty() ? "<unknown>" : prof->game_name.c_str(),
           trt_engine_type_name(prof->engine_type),
           trt_graphics_api_name(prof->preferred_gfx),
           trt_audio_middleware_name(prof->preferred_audio));
  trt_core_log(log_buf);

  return 0;
}

// Engine-owned settings file: the place an engine reads its screen from on
// start, independent of the command line it was given. Unreal reads
// Saved/Config/<platform>/GameUserSettings.ini; without it the title boots on
// its own defaults (4K, high VRAM) whatever -ResX was passed.
extern "C" int trt_engine_write_user_settings(const trt_engine_profile* prof,
                                              const char* drive_c,
                                              char* out_path,
                                              size_t out_path_cap) {
  if (!prof || !drive_c || !drive_c[0]) return 0;
  if (prof->engine_type != TRT_ENGINE_UNREAL) return 0;
  std::string name = prof->game_name.empty() ? std::string("Game") : prof->game_name;
  for (char& c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      c = '_';
  }
  const char* const k_platforms[] = {"WindowsNoEditor", "Windows"};
  int wrote = 0;
  for (const char* plat : k_platforms) {
    std::string dir = drive_c;
    dir = path_join(dir, "Users");
    dir = path_join(dir, "Player");
    dir = path_join(dir, "AppData");
    dir = path_join(dir, "Local");
    dir = path_join(dir, name);
    dir = path_join(dir, "Saved");
    dir = path_join(dir, "Config");
    dir = path_join(dir, plat);
    if (!make_dirs(dir)) continue;
    const std::string file = path_join(dir, "GameUserSettings.ini");
    FILE* f = fopen(file.c_str(), "w");
    if (!f) continue;
    const int w = prof->screen_width;
    const int h = prof->screen_height;
    const int mode = prof->fullscreen ? 0 : 2;  // 0 = fullscreen, 2 = windowed
    fprintf(f, "[/Script/Engine.GameUserSettings]\n");
    fprintf(f, "bUseVSync=False\n");
    fprintf(f, "ResolutionSizeX=%d\n", w);
    fprintf(f, "ResolutionSizeY=%d\n", h);
    fprintf(f, "LastUserConfirmedResolutionSizeX=%d\n", w);
    fprintf(f, "LastUserConfirmedResolutionSizeY=%d\n", h);
    fprintf(f, "WindowPosX=-1\nWindowPosY=-1\n");
    fprintf(f, "FullscreenMode=%d\nLastConfirmedFullscreenMode=%d\n", mode, mode);
    fprintf(f, "PreferredFullscreenMode=%d\nVersion=5\n", mode);
    fclose(f);
    if (out_path && out_path_cap) snprintf(out_path, out_path_cap, "%s", file.c_str());
    ++wrote;
  }
  if (wrote) {
    char line[600];
    snprintf(line, sizeof(line),
             "synthesizer: wrote %dx%d %s user settings for %s (%d platform file%s)",
             prof->screen_width, prof->screen_height,
             prof->fullscreen ? "fullscreen" : "windowed", name.c_str(), wrote,
             wrote == 1 ? "" : "s");
    trt_core_log(line);
  }
  return wrote ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Launch preflight
// ---------------------------------------------------------------------------
static int preflight_core_module(const char* m) {
  // Modules whose absence changes what the game does (vs. a cosmetic feature).
  static const char* const k_core[] = {
      "d3d8.dll",      "d3d9.dll",       "d3d10.dll",
      "d3d10_1.dll",   "d3d11.dll",      "d3d12.dll",
      "dxgi.dll",      "ddraw.dll",      "opengl32.dll",
      "vulkan-1.dll",  "xinput1_3.dll",  "xinput1_4.dll",
      "xinput9_1_0.dll", "gameinput.dll", "dsound.dll",
      "xaudio2_6.dll", "xaudio2_7.dll",  "xaudio2_8.dll",
      "xaudio2_9.dll", "fmod.dll",       "fmod64.dll",
      "fmodstudio.dll", "fmodstudio64.dll", "aksoundengine.dll",
      "openal32.dll",  "mfplat.dll",     "mfreadwrite.dll",
      "bink2w64.dll",  "binkw64.dll",    "cri_ware_unity.dll",
      "dstorage.dll",
  };
  for (const char* c : k_core)
    if (strcmp(c, m) == 0) return 1;
  return 0;
}

extern "C" const char* trt_preflight_blocker_name(unsigned flag) {
  switch (flag) {
    case TRT_PRE_NOT_A_PE: return "not_a_pe";
    case TRT_PRE_PAYLOAD_MISSING: return "payload_missing";
    case TRT_PRE_MANAGED_HOST: return "no_managed_host";
    case TRT_PRE_NO_GFX_DEVICE: return "no_gfx_device";
    case TRT_PRE_GFX_NOT_EXECUTED: return "gfx_not_executed";
    case TRT_PRE_NO_COMPOSITOR: return "no_compositor";
    case TRT_PRE_ANTI_CHEAT: return "anti_cheat";
    case TRT_PRE_STUB_MODULES: return "stub_modules";
    default: return "unknown";
  }
}

extern "C" int trt_engine_launch_preflight(const trt_engine_profile* prof,
                                           const trt_host_caps* caps,
                                           trt_preflight_report* out) {
  if (!out) return 0;
  out->blockers = 0;
  out->lines.clear();
  out->notes.clear();
  out->runnable = false;
  if (!prof) return 0;
  static const trt_host_caps k_no_caps{};
  const trt_host_caps* c = caps ? caps : &k_no_caps;
  const std::string name = prof->game_name.empty() ? prof->main_exe_rel
                                                   : prof->game_name;

  auto block = [&](unsigned flag, const std::string& text) {
    out->blockers |= flag;
    out->lines.push_back(std::string(trt_preflight_blocker_name(flag)) + ": " + text);
  };
  auto note = [&](const std::string& text) { out->notes.push_back(text); };
  auto join = [](const std::vector<std::string>& v, size_t max) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i)
      s += (i ? ", " : "") + v[i];
    if (v.size() > max) {
      char tail[32];
      snprintf(tail, sizeof(tail), " (+%zu more)", v.size() - max);
      s += tail;
    }
    return s;
  };

  // 1. Can this binary be executed at all?
  if (!prof->pe_scanned) {
    block(TRT_PRE_NOT_A_PE,
          "the main executable was never read as a PE (" + prof->main_exe_rel + ")");
  } else if (!prof->is_pe32 && !prof->is_pe64 && !prof->is_managed) {
    block(TRT_PRE_NOT_A_PE,
          "not an x86/x64 image (ARM64 or unrecognised machine): no engine "
          "in this build executes it");
  }

  // 2. Is the engine's own data beside the exe?
  if (!prof->payload_complete) {
    if (prof->missing_payload.empty()) {
      block(TRT_PRE_PAYLOAD_MISSING, "engine payload is incomplete");
    } else {
      for (const auto& m : prof->missing_payload)
        block(TRT_PRE_PAYLOAD_MISSING, "missing " + m);
    }
  }

  // 3. Managed code needs the Mono host.
  if (prof->is_managed && !c->mono_host) {
    block(TRT_PRE_MANAGED_HOST,
          "managed (.NET) image but this build links no Mono host: the entry "
          "point is mscoree!<Flavor>Main and cannot run natively");
  } else if (prof->is_managed) {
    note("managed image: Main runs under the Mono host");
  }

  // 4. Is the chosen graphics API executed or only recorded?
  switch (prof->preferred_gfx) {
    case TRT_GFX_D3D11:
      if (!c->d3d11_device)
        block(TRT_PRE_NO_GFX_DEVICE,
              "D3D11 selected but no host device exists: D3D11CreateDevice "
              "returns DXGI_ERROR_UNSUPPORTED at the first call");
      break;
    case TRT_GFX_D3D9:
      if (!c->d3d11_device)
        block(TRT_PRE_NO_GFX_DEVICE,
              "D3D9 selected: it draws through the D3D11 host, which has no device");
      break;
    case TRT_GFX_VULKAN:
      if (!c->d3d11_device)
        block(TRT_PRE_NO_GFX_DEVICE,
              "Vulkan selected: it is translated onto D3D11, which has no host device");
      break;
    case TRT_GFX_D3D12:
      if (!c->d3d12_device)
        block(TRT_PRE_GFX_NOT_EXECUTED,
              "D3D12 selected: command lists are recorded, not submitted - "
              "nothing is drawn");
      break;
    case TRT_GFX_OPENGL:
      if (!c->opengl_renderer)
        block(TRT_PRE_GFX_NOT_EXECUTED,
              "OpenGL selected but the WGL bridge has no active render backend");
      break;
    default:
      block(TRT_PRE_NO_GFX_DEVICE, "no graphics API could be resolved for this title");
      break;
  }
  if (!c->compositor) {
    block(TRT_PRE_NO_COMPOSITOR,
          "no compositor: frames are drawn offscreen and no window appears");
  }
  if (!c->audio_device) note("no audio endpoint: the title runs silent (not fatal)");
  if (prof->is_pe32)
    note("PE32: runs on the x86 engine, whose 3D surface is the translated path only");

  // 5. Anti-cheat and stub-serviced imports. Only an explicitly registered
  //    STUB counts: a route that is merely MISSING is registered later in the
  //    launch (or is the game's own DLL), and counting it produced false
  //    "stub" notes for unityplayer.dll and system modules.
  std::vector<std::string> ac, stub_core;
  auto on_disk = [&](const std::string& m) {
    for (const std::string* dir : {&prof->game_dir}) {
      if (!dir || dir->empty()) continue;
      const std::string p = path_join(*dir, m);
      if (file_exists(p) || file_exists(p + ".dll")) return true;
    }
    if (!prof->main_exe_abs.empty()) {
      const size_t sl = prof->main_exe_abs.find_last_of("/\\");
      const std::string p = sl == std::string::npos
                                ? path_join(std::string("."), m)
                                : path_join(prof->main_exe_abs.substr(0, sl), m);
      if (file_exists(p) || file_exists(p + ".dll")) return true;
    }
    return false;
  };
  for (const auto& m : prof->imported_modules) {
    if (m.empty()) continue;
    if (m.find("easyanticheat") != std::string::npos ||
        m.find("battleye") != std::string::npos ||
        m.find("beclient") != std::string::npos) {
      ac.push_back(m);
    }
    if (trt_module_route(m.c_str()) != TRT_ROUTE_STUB) continue;
    if (on_disk(m)) continue;  // the game's own binary, never a stub
    if (preflight_core_module(m.c_str())) stub_core.push_back(m);
  }
  if (!ac.empty()) {
    block(TRT_PRE_ANTI_CHEAT,
          "anti-cheat imported (" + join(ac, 3) +
              "): the kernel-level driver cannot exist here, the title refuses to start");
  }
  if (!stub_core.empty()) {
    block(TRT_PRE_STUB_MODULES,
          "graphics/audio imports bind to stubs (" + join(stub_core, 4) +
              "): their calls answer zero instead of drawing or playing");
  }
  // Non-graphics modules registered as stubs are deliberately not listed: the
  // OS surface (advapi32/user32/ws2_32) is registered by the host's own bridge
  // pass, and naming it here would blame a stub for a module that is served.

  // 6. Advisory facts that change online behaviour, not launches.
  if (prof->has_steam && !prof->steam_appid) {
    note("No usable Steam AppID in install metadata. Steam services stay disabled.");
  }

  out->runnable = (out->blockers == 0);
  if (getenv("TRT_SYNTH_DRY_RUN") == nullptr) {
    if (!write_profile_json(prof->game_dir, prof, out)) {
      out->notes.push_back("profile.json could not be written (game folder read-only?)");
    }
  }

  int nblock = 0;
  for (unsigned b = out->blockers; b; b &= (b - 1)) ++nblock;
  for (const auto& line : out->lines) {
    trt_core_log(("trt: [PREFLIGHT] " + name + " " + line).c_str());
  }
  char summary[512];
  if (nblock == 0) {
    snprintf(summary, sizeof(summary),
             "trt: [PREFLIGHT] %s: launchable on this host (gfx=%s, audio=%s, %zu note(s))",
             name.c_str(), trt_graphics_api_name(prof->preferred_gfx),
             trt_audio_middleware_name(prof->preferred_audio), out->notes.size());
  } else {
    snprintf(summary, sizeof(summary),
             "trt: [PREFLIGHT] %s: %d blocker(s), %zu note(s) - %s",
             name.c_str(), nblock, out->notes.size(),
             out->lines.empty() ? "" : out->lines[0].c_str());
  }
  trt_core_log(summary);
  return nblock;
}

} // extern "C"

