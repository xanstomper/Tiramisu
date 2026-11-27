// tiramisu/src/host/game_library.h
// Game shelf data model, PE/ISO executable scoring, static compatibility
// scanning, engine label inference, and shelf ordering.
#ifndef TIRAMISU_GAME_LIBRARY_H
#define TIRAMISU_GAME_LIBRARY_H

#include "../trt/iso.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace longan {

struct game_entry {
  std::string name;
  std::string dir;    // full root directory of this game (LocalState or USB)
  std::string exe;    // selected relative path inside game_dir (or the image)
  std::string image;  // disc image (.iso/.bin/.img) when the game ships on one
  std::vector<std::string> exes;
  int exe_idx = 0;
  std::string compat;      // static PE analysis, shown before launch
  std::string input_mode;  // auto | controller | kbm | pad2kbm
  std::string engine_name;
  std::string gfx_name;
  std::string audio_name;
  bool synthesized = false;
  bool is_external = false;
  bool win32_readable = true;
  std::string usb_id;
  std::string source_label;  // e.g. "USB E: · NTFS"
};

struct exe_cand {
  std::string rel;
  uint64_t size;
  int score;
  int depth;
};

std::string lower_ascii(std::string s);
bool is_junk_exe(const std::string& fname);

void collect_exes_rec(const std::string& base_dir, const std::string& rel_dir,
                      const std::string& game_name, int depth,
                      std::vector<exe_cand>* out);
void collect_iso_exes_rec(trt_iso* iso, const std::string& rel_dir,
                          const std::string& game_name, int depth,
                          std::vector<exe_cand>* out);

void scan_compat(const std::string& path, char* out, unsigned cap);
void scan_compat_iso(trt_iso* iso, const std::string& rel, char* out, unsigned cap);

void scan_game_entry(
    std::vector<game_entry>& games,
    const std::string& root, const std::string& n, bool is_dir,
    char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice);
void scan_games_root(
    std::vector<game_entry>& games,
    const std::string& root, char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice);

std::vector<std::pair<std::string, std::string>> snapshot_prev_choice(
    const std::vector<game_entry>& games);
std::wstring infer_engine_label(const game_entry& ge);
void sort_games_shelf(std::vector<game_entry>& games);

}  // namespace longan

#endif
