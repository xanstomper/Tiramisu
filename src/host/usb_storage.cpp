// tiramisu/src/host/usb_storage.cpp
// NTFS / exFAT / FAT32 USB & external drive auto-detection, WinRT broker
// fallback, Xbox FolderPicker integration, and background copy/extract installer.
#include "usb_storage.h"

#include "game_library.h"
#include "host_log.h"
#include "ui.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace longan {

std::string host_ws2u8(const wchar_t* w) {
  if (!w || !w[0]) return {};
  const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return {};
  std::string s((size_t)(need - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), need, nullptr, nullptr);
  return s;
}

std::wstring host_u82ws(const std::string& s) {
  if (s.empty()) return {};
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (need <= 0) return {};
  std::wstring w((size_t)need, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), need);
  return w;
}

bool host_delete_dir_recursive(const std::string& path) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((host_u82ws(path) + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
        continue;
      const std::string child = path + "\\" + host_ws2u8(fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        host_delete_dir_recursive(child);
      } else {
        const std::wstring wchild = host_u82ws(child);
        SetFileAttributesW(wchild.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(wchild.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  const std::wstring wpath = host_u82ws(path);
  SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_NORMAL);
  return RemoveDirectoryW(wpath.c_str()) != 0;
}

namespace usb_storage {
namespace {

std::string g_local_state;
std::string g_games_dir;
std::string g_package_dir;

std::vector<webxfer_storage_volume> g_external_volumes;
std::vector<std::string> g_external_direct_game_dirs;
std::mutex g_usb_broker_mu;
std::map<std::string, winrt::Windows::Storage::StorageFolder> g_usb_broker_folders;
std::map<std::string, winrt::Windows::Storage::StorageFile> g_usb_broker_files;
std::vector<winrt::Windows::Storage::StorageFolder> g_user_picked_folders;
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFolder>
    g_folder_picker_op{nullptr};

std::atomic<bool> g_usb_scan_running{false};
std::atomic<bool> g_usb_scan_ready{false};
std::mutex g_usb_scan_mu;
std::vector<webxfer_storage_volume> g_usb_scan_vols;
std::vector<std::string> g_usb_scan_direct_dirs;
std::map<std::string, winrt::Windows::Storage::StorageFolder> g_usb_scan_folders;
std::map<std::string, winrt::Windows::Storage::StorageFile> g_usb_scan_files;
std::string g_usb_scan_broker_status = "checking";

std::atomic<bool> g_usb_copy_active{false};
std::atomic<uint64_t> g_usb_copy_done{0}, g_usb_copy_total{0};
std::mutex g_usb_copy_mu;
std::string g_usb_copy_label;
std::atomic<bool> g_usb_copy_needs_rescan{false};
std::string g_usb_copy_toast;

std::string volume_key(std::string path) {
  while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
    path.pop_back();
  return lower_ascii(std::move(path));
}

bool is_reserved_usb_root_dir(const std::string& name) {
  const std::string low = lower_ascii(name);
  return low == "." || low == ".." || low == "$recycle.bin" ||
         low == "system volume information" || low == "windowsapps" ||
         low == "xbox" || low == "xboxgames" || low == "programdata" ||
         low == "games" || low == "tiramisugames" || low == "tiramisu" ||
         low == "pc games" || low == "pcgames" || low == "downloads" ||
         low == "steamlibrary" || low == "__macosx";
}

bool is_game_archive_ext(const std::string& low_name, std::string* out_kind) {
  auto ends = [&](const char* ext) {
    const size_t n = strlen(ext);
    return low_name.size() > n &&
           low_name.compare(low_name.size() - n, n, ext) == 0;
  };
  if (ends(".zip")) { if (out_kind) *out_kind = "zip"; return true; }
  if (ends(".iso") || ends(".img") || ends(".bin") || ends(".cue")) {
    if (out_kind) *out_kind = "iso";
    return true;
  }
  return false;
}

bool win32_dir_readable_w(const std::string& path) {
  if (path.empty()) return false;
  std::wstring w = host_u82ws(path);
  while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
  w += L"\\*";
  WIN32_FIND_DATAW fd{};
  SetLastError(ERROR_SUCCESS);
  HANDLE h = FindFirstFileW(w.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
  return GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool win32_dir_has_game_w(const std::string& dir, int depth, std::string* first_exe,
                          uint64_t* approx_bytes) {
  if (depth > 2) return false;
  std::wstring w = host_u82ws(dir);
  while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
  w += L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(w.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  uint64_t bytes = 0;
  do {
    const std::string fn = host_ws2u8(fd.cFileName);
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    const std::string full = dir + "\\" + fn;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      const std::string low = lower_ascii(fn);
      if (low == "thirdparty" || low == "extras" || low == "prereqs") continue;
      if (win32_dir_has_game_w(full, depth + 1, first_exe, &bytes)) {
        found = true;
      }
    } else {
      const uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      bytes += sz;
      const std::string low = lower_ascii(fn);
      std::string kind;
      if (low.size() > 4 && low.compare(low.size() - 4, 4, ".exe") == 0 &&
          !is_junk_exe(fn)) {
        if (first_exe && first_exe->empty()) *first_exe = fn;
        found = true;
      } else if (is_game_archive_ext(low, &kind) && kind == "iso") {
        found = true;
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (approx_bytes) *approx_bytes += bytes;
  return found;
}

uint64_t calc_dir_size_w(const std::string& dir, int depth = 0) {
  if (depth > 8) return 0;
  std::wstring w = host_u82ws(dir);
  while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
  w += L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(w.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  uint64_t total = 0;
  do {
    const std::string fn = host_ws2u8(fd.cFileName);
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      total += calc_dir_size_w(dir + "\\" + fn, depth + 1);
    } else {
      total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return total;
}

std::string sanitize_usb_item_name(const std::string& raw, const std::string& kind) {
  std::string stem = raw;
  if (kind == "zip" || kind == "iso") {
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
  }
  std::string out;
  if (webxfer_sanitize_game_name(stem, &out)) return out;
  return "USBGame";
}

void run_external_storage_scan_worker() {
  struct guard_flag { ~guard_flag() { g_usb_scan_running.store(false); } } gf;
  try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}

  struct raw_vol {
    std::string name;
    std::string path;
    winrt::Windows::Storage::StorageFolder folder{nullptr};
    bool is_removable = false;
    bool from_broker = false;
  };
  std::vector<raw_vol> candidates;
  std::string broker_status = "unavailable";

  // 1. WinRT RemovableDevices broker (detects NTFS, exFAT, and FAT32 USB drives
  //    on Xbox Dev Mode even when raw Win32 ACLs restrict direct directory reads).
  try {
    using winrt::Windows::Storage::KnownFolders;
    auto folders = KnownFolders::RemovableDevices().GetFoldersAsync().get();
    for (const auto& folder : folders) {
      std::string name = host_ws2u8(folder.Name().c_str());
      std::string path;
      try { path = host_ws2u8(folder.Path().c_str()); } catch (...) {}
      candidates.push_back({std::move(name), std::move(path), folder, true, true});
    }
    broker_status = "ready";
  } catch (...) {
    broker_status = "unavailable";
  }

  // 2. Include user-picked USB folders (from Xbox FolderPicker).
  {
    std::lock_guard<std::mutex> lk(g_usb_broker_mu);
    for (const auto& pf : g_user_picked_folders) {
      if (!pf) continue;
      std::string name = host_ws2u8(pf.Name().c_str());
      std::string path;
      try { path = host_ws2u8(pf.Path().c_str()); } catch (...) {}
      const auto key = path.empty() ? std::string() : volume_key(path);
      bool dup = false;
      for (auto& c : candidates) {
        if (!key.empty() && !c.path.empty() && volume_key(c.path) == key) {
          if (!c.folder) c.folder = pf;
          c.from_broker = true;
          dup = true;
          break;
        }
      }
      if (!dup) candidates.push_back({std::move(name), std::move(path), pf, true, true});
    }
  }

  // 3. Probe logical drive letters D..Z (catches external USB SSDs/sticks that
  //    enumerate as DRIVE_REMOVABLE or external DRIVE_FIXED with NTFS/exFAT).
  wchar_t sys_dir[MAX_PATH] = {};
  GetSystemDirectoryW(sys_dir, MAX_PATH);
  const char os_drive = sys_dir[0] ? (char)std::toupper((unsigned char)sys_dir[0]) : 'C';
  const char local_drive = g_local_state.size() > 1 && g_local_state[1] == ':'
                               ? (char)std::toupper((unsigned char)g_local_state[0]) : 0;
  const char pkg_drive = g_package_dir.size() > 1 && g_package_dir[1] == ':'
                             ? (char)std::toupper((unsigned char)g_package_dir[0]) : 0;

  const DWORD mask = GetLogicalDrives();
  for (char letter = 'D'; letter <= 'Z'; ++letter) {
    if (mask != 0 && !(mask & (1u << (letter - 'A')))) continue;
    const wchar_t wroot[4] = {(wchar_t)letter, L':', L'\\', 0};
    const UINT dtype = GetDriveTypeW(wroot);
    if (dtype != DRIVE_REMOVABLE && dtype != DRIVE_FIXED) continue;
    const std::string root = std::string(1, letter) + ":\\";
    const auto key = volume_key(root);
    bool exists = false;
    for (auto& c : candidates) {
      if (!c.path.empty() && volume_key(c.path) == key) {
        c.is_removable = c.is_removable || (dtype == DRIVE_REMOVABLE);
        exists = true;
        break;
      }
    }
    if (exists) continue;
    if (dtype == DRIVE_FIXED && (letter == os_drive || letter == local_drive || letter == pkg_drive)) {
      continue;
    }
    candidates.push_back({std::string(1, letter) + ":", root, nullptr,
                          dtype == DRIVE_REMOVABLE, false});
  }

  std::set<std::string> internal_names;
  {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((host_u82ws(g_games_dir) + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::string fn = host_ws2u8(fd.cFileName);
        if (fn != "." && fn != "..") internal_names.insert(lower_ascii(fn));
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }

  std::vector<webxfer_storage_volume> out_vols;
  std::vector<std::string> out_direct_dirs;
  std::map<std::string, winrt::Windows::Storage::StorageFolder> out_folders;
  std::map<std::string, winrt::Windows::Storage::StorageFile> out_files;
  int next_id = 1;

  for (const auto& cand : candidates) {
    webxfer_storage_volume vol;
    vol.name = cand.name;
    vol.path = cand.path;
    vol.broker_readable = (cand.folder != nullptr);

    if (!vol.path.empty()) {
      std::wstring wroot = host_u82ws(vol.path);
      if (wroot.size() >= 2 && wroot[1] == L':' && wroot.back() != L'\\') wroot += L'\\';
      wchar_t vlabel[128] = {};
      wchar_t fsname[32] = {};
      if (GetVolumeInformationW(wroot.c_str(), vlabel, 128, nullptr, nullptr, nullptr, fsname, 32)) {
        vol.label = host_ws2u8(vlabel);
        vol.fs = host_ws2u8(fsname);
      }
      ULARGE_INTEGER avail{}, total{}, free_b{};
      if (GetDiskFreeSpaceExW(wroot.c_str(), &avail, &total, &free_b)) {
        vol.total_bytes = total.QuadPart;
        vol.free_bytes = avail.QuadPart;
      }
      if (!vol.label.empty() && vol.name.find(vol.label) == std::string::npos) {
        if (vol.path.size() >= 2 && vol.path[1] == ':')
          vol.name = vol.path.substr(0, 2) + " " + vol.label;
        else
          vol.name = vol.label;
      } else if (vol.name.empty() && vol.path.size() >= 2) {
        vol.name = vol.path.substr(0, 2);
      }
    }

    vol.win32_readable = win32_dir_readable_w(vol.path);
    std::set<std::string> seen_items;

    auto add_item = [&](const std::string& disp_name, const std::string& full_path,
                        const std::string& kind, const std::string& exe_rel,
                        uint64_t sz, bool w32_ok,
                        winrt::Windows::Storage::StorageFolder bfolder,
                        winrt::Windows::Storage::StorageFile bfile) {
      const std::string ik = lower_ascii(disp_name + "|" + kind + "|" + full_path);
      if (!seen_items.insert(ik).second) return;
      webxfer_usb_item item;
      item.id = "usb-" + std::to_string(next_id++);
      item.name = disp_name;
      item.volume_path = vol.path.empty() ? vol.name : vol.path;
      item.fs = vol.fs;
      item.path = full_path;
      item.kind = kind;
      item.exe = exe_rel;
      item.size_bytes = sz;
      item.win32_readable = w32_ok;
      item.installed_internal = (internal_names.count(lower_ascii(disp_name)) > 0);
      if (bfolder) out_folders.emplace(item.id, std::move(bfolder));
      if (bfile) out_files.emplace(item.id, std::move(bfile));
      vol.items.push_back(std::move(item));
    };

    if (vol.win32_readable && !vol.path.empty()) {
      std::string base = vol.path;
      if (base.back() != '\\' && base.back() != '/') base += '\\';

      if (cand.from_broker && base.size() > 3) {
        std::string first_exe;
        uint64_t sz = 0;
        if (win32_dir_has_game_w(vol.path, 0, &first_exe, &sz)) {
          out_direct_dirs.push_back(vol.path);
          add_item(sanitize_usb_item_name(cand.name, "folder"), vol.path, "folder",
                   first_exe, sz, true, cand.folder, nullptr);
        }
      }

      const char* containers[] = {"Games", "TiramisuGames", "Tiramisu",
                                  "PC Games", "PCGames", "Downloads"};
      std::vector<std::string> candidate_containers;
      for (const char* sub : containers) candidate_containers.push_back(base + sub);
      candidate_containers.push_back(base + "SteamLibrary\\steamapps\\common");

      for (const auto& cpath : candidate_containers) {
        if (!win32_dir_readable_w(cpath)) continue;
        vol.game_roots.push_back(cpath);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((host_u82ws(cpath) + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
          do {
            const std::string fn = host_ws2u8(fd.cFileName);
            if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
            const std::string ep = cpath + "\\" + fn;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
              std::string first_exe;
              uint64_t sz = 0;
              if (win32_dir_has_game_w(ep, 0, &first_exe, &sz)) {
                add_item(sanitize_usb_item_name(fn, "folder"), ep, "folder",
                         first_exe, sz, true, nullptr, nullptr);
              }
            } else {
              std::string kind;
              if (is_game_archive_ext(lower_ascii(fn), &kind)) {
                const uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                add_item(sanitize_usb_item_name(fn, kind), ep, kind, "", sz, true,
                         nullptr, nullptr);
              }
            }
          } while (FindNextFileW(h, &fd));
          FindClose(h);
        }
      }

      WIN32_FIND_DATAW rfd{};
      HANDLE rh = FindFirstFileW((host_u82ws(base) + L"*").c_str(), &rfd);
      if (rh != INVALID_HANDLE_VALUE) {
        do {
          const std::string fn = host_ws2u8(rfd.cFileName);
          const std::string full = base + fn;
          if (rfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (is_reserved_usb_root_dir(fn)) continue;
            std::string first_exe;
            uint64_t sz = 0;
            if (win32_dir_has_game_w(full, 0, &first_exe, &sz)) {
              out_direct_dirs.push_back(full);
              add_item(sanitize_usb_item_name(fn, "folder"), full, "folder",
                       first_exe, sz, true, nullptr, nullptr);
            }
          } else {
            std::string kind;
            if (is_game_archive_ext(lower_ascii(fn), &kind)) {
              const uint64_t sz = ((uint64_t)rfd.nFileSizeHigh << 32) | rfd.nFileSizeLow;
              add_item(sanitize_usb_item_name(fn, kind), full, kind, "", sz, true,
                       nullptr, nullptr);
            }
          }
        } while (FindNextFileW(rh, &rfd));
        FindClose(rh);
      }
    }

    if (cand.folder && (vol.items.empty() || !vol.win32_readable)) {
      auto inspect_broker_game = [&](const winrt::Windows::Storage::StorageFolder& gf) {
        const std::string gname = host_ws2u8(gf.Name().c_str());
        if (is_reserved_usb_root_dir(gname)) return;
        try {
          std::string exe;
          auto files = gf.GetFilesAsync().get();
          for (const auto& f : files) {
            const std::string fn = host_ws2u8(f.Name().c_str());
            const std::string low = lower_ascii(fn);
            if (low.size() > 4 && low.compare(low.size() - 4, 4, ".exe") == 0 && !is_junk_exe(fn)) {
              exe = fn;
              break;
            }
          }
          if (exe.empty()) {
            auto subs = gf.GetFoldersAsync().get();
            for (const auto& sf : subs) {
              auto sfiles = sf.GetFilesAsync().get();
              for (const auto& f : sfiles) {
                const std::string fn = host_ws2u8(f.Name().c_str());
                const std::string low = lower_ascii(fn);
                if (low.size() > 4 && low.compare(low.size() - 4, 4, ".exe") == 0 && !is_junk_exe(fn)) {
                  exe = host_ws2u8(sf.Name().c_str()) + "\\" + fn;
                  break;
                }
              }
              if (!exe.empty()) break;
            }
          }
          if (!exe.empty()) {
            std::string fpath;
            try { fpath = host_ws2u8(gf.Path().c_str()); } catch (...) {}
            const bool r32 = !fpath.empty() && win32_dir_readable_w(fpath);
            add_item(sanitize_usb_item_name(gname, "folder"),
                     fpath.empty() ? (vol.name + "\\" + gname) : fpath,
                     "folder", exe, 0, r32, gf, nullptr);
          }
        } catch (...) {}
      };

      try {
        auto root_files = cand.folder.GetFilesAsync().get();
        for (const auto& f : root_files) {
          const std::string fn = host_ws2u8(f.Name().c_str());
          std::string kind;
          if (!is_game_archive_ext(lower_ascii(fn), &kind)) continue;
          std::string fpath;
          try { fpath = host_ws2u8(f.Path().c_str()); } catch (...) {}
          add_item(sanitize_usb_item_name(fn, kind),
                   fpath.empty() ? (vol.name + "\\" + fn) : fpath,
                   kind, "", 0, vol.win32_readable, nullptr, f);
        }
        auto root_folders = cand.folder.GetFoldersAsync().get();
        for (const auto& rf : root_folders) {
          const std::string rname = host_ws2u8(rf.Name().c_str());
          const std::string low = lower_ascii(rname);
          if (low == "games" || low == "tiramisugames" || low == "tiramisu" ||
              low == "pc games" || low == "pcgames" || low == "downloads") {
            auto sub_files = rf.GetFilesAsync().get();
            for (const auto& sf : sub_files) {
              const std::string sfn = host_ws2u8(sf.Name().c_str());
              std::string kind;
              if (!is_game_archive_ext(lower_ascii(sfn), &kind)) continue;
              std::string spath;
              try { spath = host_ws2u8(sf.Path().c_str()); } catch (...) {}
              add_item(sanitize_usb_item_name(sfn, kind),
                       spath.empty() ? (vol.name + "\\" + rname + "\\" + sfn) : spath,
                       kind, "", 0, vol.win32_readable, nullptr, sf);
            }
            auto sub_folders = rf.GetFoldersAsync().get();
            for (const auto& gf : sub_folders) inspect_broker_game(gf);
          } else {
            inspect_broker_game(rf);
          }
        }
      } catch (...) {}
    }

    if (cand.is_removable || cand.from_broker || !vol.game_roots.empty() || !vol.items.empty()) {
      if (vol.fs.empty() && cand.from_broker) vol.fs = "USB";
      out_vols.push_back(std::move(vol));
    }
  }

  std::sort(out_vols.begin(), out_vols.end(), [](const auto& a, const auto& b) {
    if (a.path != b.path) return a.path < b.path;
    return a.name < b.name;
  });

  {
    std::lock_guard<std::mutex> lk(g_usb_scan_mu);
    g_usb_scan_vols = std::move(out_vols);
    g_usb_scan_direct_dirs = std::move(out_direct_dirs);
    g_usb_scan_folders = std::move(out_folders);
    g_usb_scan_files = std::move(out_files);
    g_usb_scan_broker_status = broker_status;
  }
  g_usb_scan_ready.store(true);
}

bool win32_copy_file_progress(const std::string& src, const std::string& dst,
                              uint64_t total_bytes, uint64_t* done_bytes,
                              const std::string& phase_label, const std::string& item_label,
                              std::string* err) {
  FILE* in = _wfopen(host_u82ws(src).c_str(), L"rb");
  if (!in) {
    if (err) *err = "Read error on USB file: " + item_label;
    return false;
  }
  FILE* out = _wfopen(host_u82ws(dst).c_str(), L"wb");
  if (!out) {
    fclose(in);
    if (err) *err = "Write error on Xbox storage: " + item_label;
    return false;
  }
  std::vector<char> buf(262144);
  bool ok = true;
  while (true) {
    const size_t rd = fread(buf.data(), 1, buf.size(), in);
    if (rd == 0) {
      if (ferror(in)) {
        ok = false;
        if (err) *err = "Read error on USB file: " + item_label;
      }
      break;
    }
    if (fwrite(buf.data(), 1, rd, out) != rd) {
      ok = false;
      if (err) *err = "Disk full or write error writing " + item_label;
      break;
    }
    *done_bytes += rd;
    g_usb_copy_done.store(*done_bytes);
    webxfer_set_usb_install_status(true, phase_label, *done_bytes, total_bytes, "");
  }
  if (fclose(out) != 0 && ok) {
    ok = false;
    if (err) *err = "Disk full flushing " + item_label;
  }
  fclose(in);
  return ok;
}

bool win32_copy_tree_progress(const std::string& src, const std::string& dst,
                              uint64_t total_bytes, uint64_t* done_bytes,
                              const std::string& label, std::string* err) {
  CreateDirectoryW(host_u82ws(dst).c_str(), nullptr);
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((host_u82ws(src) + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = "Cannot read source USB directory";
    return false;
  }
  bool ok = true;
  do {
    const std::string fn = host_ws2u8(fd.cFileName);
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    const std::string s_path = src + "\\" + fn;
    const std::string d_path = dst + "\\" + fn;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!win32_copy_tree_progress(s_path, d_path, total_bytes, done_bytes, label, err)) {
        ok = false;
        break;
      }
    } else if (!win32_copy_file_progress(s_path, d_path, total_bytes, done_bytes,
                                         "Copying " + label, fn, err)) {
      ok = false;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return ok;
}

bool broker_copy_file_stream(const winrt::Windows::Storage::StorageFile& file,
                             const std::string& dst_path, uint64_t total_bytes,
                             uint64_t* done_bytes, const std::string& label,
                             std::string* err) {
  try {
    using namespace winrt::Windows::Storage::Streams;
    auto stream = file.OpenReadAsync().get();
    Buffer rbuf(262144);
    std::vector<uint8_t> bytes(262144);
    FILE* out = _wfopen(host_u82ws(dst_path).c_str(), L"wb");
    if (!out) {
      if (err) *err = "Cannot create Xbox destination file";
      return false;
    }
    bool ok = true;
    while (true) {
      auto chunk = stream.ReadAsync(rbuf, rbuf.Capacity(), InputStreamOptions::Partial).get();
      const size_t n = chunk.Length();
      if (!n) break;
      auto reader = DataReader::FromBuffer(chunk);
      reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + n));
      if (fwrite(bytes.data(), 1, n, out) != n) {
        ok = false;
        if (err) *err = "Insufficient Xbox storage during copy";
        break;
      }
      *done_bytes += n;
      g_usb_copy_done.store(*done_bytes);
      webxfer_set_usb_install_status(true, "Downloading " + label,
                                     *done_bytes, total_bytes, "");
    }
    if (fclose(out) != 0 && ok) {
      ok = false;
      if (err) *err = "Insufficient Xbox storage finishing copy";
    }
    return ok;
  } catch (...) {
    if (err) *err = "WinRT broker read failed for " + label;
    return false;
  }
}

bool broker_copy_folder_tree(const winrt::Windows::Storage::StorageFolder& folder,
                             const std::string& dst_dir, uint64_t total_bytes,
                             uint64_t* done_bytes, const std::string& label,
                             std::string* err) {
  try {
    CreateDirectoryW(host_u82ws(dst_dir).c_str(), nullptr);
    for (const auto& f : folder.GetFilesAsync().get()) {
      const std::string fn = host_ws2u8(f.Name().c_str());
      if (!broker_copy_file_stream(f, dst_dir + "\\" + fn, total_bytes, done_bytes, label, err))
        return false;
    }
    for (const auto& sub : folder.GetFoldersAsync().get()) {
      const std::string sn = host_ws2u8(sub.Name().c_str());
      if (sn == "__MACOSX") continue;
      if (!broker_copy_folder_tree(sub, dst_dir + "\\" + sn, total_bytes, done_bytes, label, err))
        return false;
    }
    return true;
  } catch (...) {
    if (err) *err = "WinRT broker folder traversal failed";
    return false;
  }
}

void usb_install_worker(webxfer_usb_item item, std::string target_name,
                        winrt::Windows::Storage::StorageFolder bfolder,
                        winrt::Windows::Storage::StorageFile bfile) {
  try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
  const std::string dst_base = g_games_dir + "\\" + target_name;
  const std::string stage = g_local_state + "\\incoming\\usb-" + std::to_string(GetTickCount64());
  CreateDirectoryW(host_u82ws(stage).c_str(), nullptr);
  std::string err;
  uint64_t done = 0;
  uint64_t total = item.size_bytes;
  if (total == 0 && item.kind == "folder" && item.win32_readable && !item.path.empty()) {
    total = calc_dir_size_w(item.path);
  }
  g_usb_copy_total.store(total);
  g_usb_copy_done.store(0);
  {
    std::lock_guard<std::mutex> lk(g_usb_copy_mu);
    g_usb_copy_label = target_name;
  }
  webxfer_set_usb_install_status(true, "Downloading " + target_name + " from USB", 0, total, "");

  bool ok = false;
  if (item.kind == "folder") {
    const std::string stage_game = stage + "\\" + target_name;
    if (item.win32_readable && !item.path.empty()) {
      ok = win32_copy_tree_progress(item.path, stage_game, total, &done, target_name, &err);
    }
    if (!ok && bfolder) {
      done = 0;
      ok = broker_copy_folder_tree(bfolder, stage_game, total, &done, target_name, &err);
    }
    if (ok) {
      if (GetFileAttributesW(host_u82ws(dst_base).c_str()) != INVALID_FILE_ATTRIBUTES) {
        host_delete_dir_recursive(dst_base);
      }
      if (!MoveFileExW(host_u82ws(stage_game).c_str(), host_u82ws(dst_base).c_str(),
                       MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
        err = "Cannot finalize install folder in LocalState\\games";
      }
    }
  } else if (item.kind == "zip") {
    const std::string local_zip = stage + "\\payload.zip";
    std::string zip_src = item.path;
    if (!item.win32_readable && bfile) {
      ok = broker_copy_file_stream(bfile, local_zip, total, &done, target_name, &err);
      zip_src = local_zip;
    } else if (!item.path.empty()) {
      ok = true;
    }
    if (ok) {
      const std::string stage_game = stage + "\\" + target_name;
      CreateDirectoryW(host_u82ws(stage_game).c_str(), nullptr);
      webxfer_set_usb_install_status(true, "Extracting " + target_name, done, total, "");
      std::string znote;
      if (!webxfer_extract_zip_to_dir(zip_src, stage_game, &znote, true)) {
        ok = false;
        err = "ZIP extract failed: " + znote;
      } else {
        if (GetFileAttributesW(host_u82ws(dst_base).c_str()) != INVALID_FILE_ATTRIBUTES) {
          host_delete_dir_recursive(dst_base);
        }
        if (!MoveFileExW(host_u82ws(stage_game).c_str(), host_u82ws(dst_base).c_str(),
                         MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
          ok = false;
          err = "Cannot move extracted game to Xbox Library";
        }
      }
    }
  } else if (item.kind == "iso") {
    std::string ext = ".iso";
    const size_t dot = item.path.rfind('.');
    if (dot != std::string::npos) ext = lower_ascii(item.path.substr(dot));
    const std::string stage_iso = stage + "\\" + target_name + ext;
    const std::string dst_iso = g_games_dir + "\\" + target_name + ext;
    if (bfile && !item.win32_readable) {
      ok = broker_copy_file_stream(bfile, stage_iso, total, &done, target_name, &err);
    } else if (!item.path.empty()) {
      ok = win32_copy_file_progress(item.path, stage_iso, total, &done,
                                    "Copying " + target_name, target_name + ext, &err);
    }
    if (ok) {
      if (!MoveFileExW(host_u82ws(stage_iso).c_str(), host_u82ws(dst_iso).c_str(),
                       MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
        err = "Cannot save disc image to Xbox Library";
      }
    }
  }

  host_delete_dir_recursive(stage);
  {
    std::lock_guard<std::mutex> lk(g_usb_copy_mu);
    g_usb_copy_toast = ok ? ("Installed from USB: " + target_name)
                          : ("USB install failed: " + (err.empty() ? "unknown error" : err));
  }
  webxfer_set_usb_install_status(false, "", done, total, err,
                                 ok ? ("USB -> " + target_name) : "");
  g_usb_copy_active.store(false);
  g_usb_copy_needs_rescan.store(true);
}

}  // namespace

void init(const std::string& local_state, const std::string& games_dir,
          const std::string& package_dir) {
  g_local_state = local_state;
  g_games_dir = games_dir;
  g_package_dir = package_dir;
}

bool poll(bool force) {
  static ULONGLONG next_probe = 0;
  const ULONGLONG now = GetTickCount64();

  if (g_usb_scan_ready.exchange(false)) {
    std::vector<webxfer_storage_volume> vols;
    std::vector<std::string> direct_dirs;
    std::string broker_status;
    {
      std::lock_guard<std::mutex> lk(g_usb_scan_mu);
      vols = std::move(g_usb_scan_vols);
      direct_dirs = std::move(g_usb_scan_direct_dirs);
      broker_status = g_usb_scan_broker_status;
      std::lock_guard<std::mutex> blk(g_usb_broker_mu);
      g_usb_broker_folders = std::move(g_usb_scan_folders);
      g_usb_broker_files = std::move(g_usb_scan_files);
    }
    const bool changed = (vols != g_external_volumes) || (direct_dirs != g_external_direct_game_dirs);
    const size_t prev_cnt = g_external_volumes.size();
    g_external_volumes = std::move(vols);
    g_external_direct_game_dirs = std::move(direct_dirs);
    webxfer_set_external_storage(g_external_volumes, broker_status);
    if (changed) {
      log_line(("storage: USB/external discovery (" + broker_status +
                "), volumes=" + std::to_string(g_external_volumes.size())).c_str());
      if (g_external_volumes.size() > prev_cnt && !g_external_volumes.empty()) {
        const auto& v = g_external_volumes.back();
        ui_toast(host_u82ws("USB Drive detected: " + v.name +
                            (v.fs.empty() ? "" : " [" + v.fs + "]")), 3.2f);
      }
    }
    return changed;
  }

  if ((force || now >= next_probe) && !g_usb_scan_running.exchange(true)) {
    next_probe = now + 3000ull;
    try {
      std::thread(run_external_storage_scan_worker).detach();
    } catch (...) {
      g_usb_scan_running.store(false);
    }
  }
  return false;
}

void begin_folder_picker() {
  if (g_folder_picker_op) return;
  try {
    using namespace winrt::Windows::Storage::Pickers;
    FolderPicker picker;
    picker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
    picker.FileTypeFilter().Append(L"*");
    g_folder_picker_op = picker.PickSingleFolderAsync();
  } catch (...) {
    g_folder_picker_op = nullptr;
    ui_toast(L"Xbox Folder Picker unavailable on this view", 2.8f);
  }
}

void poll_folder_picker() {
  using winrt::Windows::Foundation::AsyncStatus;
  if (!g_folder_picker_op || g_folder_picker_op.Status() == AsyncStatus::Started) return;
  try {
    if (g_folder_picker_op.Status() == AsyncStatus::Completed) {
      auto folder = g_folder_picker_op.GetResults();
      if (folder) {
        std::string pname = host_ws2u8(folder.Name().c_str());
        std::string ppath;
        try { ppath = host_ws2u8(folder.Path().c_str()); } catch (...) {}
        {
          std::lock_guard<std::mutex> lk(g_usb_broker_mu);
          g_user_picked_folders.push_back(folder);
        }
        log_line(("storage: user picked USB folder: " + pname +
                  (ppath.empty() ? "" : (" (" + ppath + ")"))).c_str());
        ui_toast(host_u82ws("Attached USB folder: " + pname), 3.0f);
        poll(true);
      }
    }
  } catch (...) {}
  g_folder_picker_op = nullptr;
}

bool start_install_by_id(const std::string& item_id, const std::string& custom_name) {
  if (g_usb_copy_active.exchange(true)) {
    ui_toast(L"A USB download is already in progress", 2.5f);
    return false;
  }
  webxfer_usb_item found{};
  bool has = false;
  for (const auto& vol : g_external_volumes) {
    for (const auto& it : vol.items) {
      if (it.id == item_id) {
        found = it;
        has = true;
        break;
      }
    }
    if (has) break;
  }
  if (!has) {
    g_usb_copy_active.store(false);
    ui_toast(L"USB item no longer available — rescanning", 2.5f);
    return false;
  }
  std::string target = custom_name.empty() ? found.name : custom_name;
  if (!webxfer_sanitize_game_name(target, &target)) target = found.name;

  winrt::Windows::Storage::StorageFolder bfolder{nullptr};
  winrt::Windows::Storage::StorageFile bfile{nullptr};
  {
    std::lock_guard<std::mutex> lk(g_usb_broker_mu);
    auto itf = g_usb_broker_folders.find(item_id);
    if (itf != g_usb_broker_folders.end()) bfolder = itf->second;
    auto itfi = g_usb_broker_files.find(item_id);
    if (itfi != g_usb_broker_files.end()) bfile = itfi->second;
  }

  ui_toast(host_u82ws("Downloading " + target + " from USB to Xbox SSD…"), 3.0f);
  try {
    std::thread(usb_install_worker, found, target, bfolder, bfile).detach();
    return true;
  } catch (...) {
    g_usb_copy_active.store(false);
    ui_toast(L"Failed to start USB installer thread", 2.5f);
    return false;
  }
}

bool poll_install_finished(std::string* out_toast) {
  if (!g_usb_copy_needs_rescan.exchange(false)) return false;
  if (out_toast) {
    std::lock_guard<std::mutex> lk(g_usb_copy_mu);
    out_toast->swap(g_usb_copy_toast);
  }
  return true;
}

void get_copy_progress(bool* out_active, uint64_t* out_done,
                       uint64_t* out_total, std::string* out_label) {
  if (out_active) *out_active = g_usb_copy_active.load();
  if (out_done) *out_done = g_usb_copy_done.load();
  if (out_total) *out_total = g_usb_copy_total.load();
  if (out_label) {
    std::lock_guard<std::mutex> lk(g_usb_copy_mu);
    *out_label = g_usb_copy_label;
  }
}

const std::vector<webxfer_storage_volume>& volumes() { return g_external_volumes; }

bool find_item_at_index(int index, std::string* out_id, std::string* out_name) {
  int i = 0;
  for (const auto& vol : g_external_volumes) {
    for (const auto& it : vol.items) {
      if (i++ == index) {
        if (out_id) *out_id = it.id;
        if (out_name) *out_name = it.name;
        return true;
      }
    }
  }
  return false;
}

}  // namespace usb_storage
}  // namespace longan
