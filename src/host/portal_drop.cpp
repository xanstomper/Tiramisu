#include "portal_drop.h"

#include "game_library.h"
#include "host_log.h"
#include "usb_storage.h"
#include "webxfer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <map>
#include <thread>

namespace longan {
namespace {

struct archive_stamp {
  uint64_t size;
  uint64_t modified;
  bool operator==(const archive_stamp&) const = default;
};

std::atomic<bool> scanning{false};
std::map<std::string, archive_stamp> observed;
std::map<std::string, archive_stamp> attempted;

uint64_t combine_words(DWORD high, DWORD low) {
  return (static_cast<uint64_t>(high) << 32) | low;
}

void scan_portal_drop(const std::string& local_state, const std::string& drop_dir) {
  WIN32_FIND_DATAW entry{};
  HANDLE list = FindFirstFileW((host_u82ws(drop_dir) + L"\\*").c_str(), &entry);
  if (list == INVALID_HANDLE_VALUE) return;

  std::map<std::string, archive_stamp> current;
  do {
    if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      continue;
    const std::string name = host_ws2u8(entry.cFileName);
    if (name.size() <= 4 || lower_ascii(name.substr(name.size() - 4)) != ".zip")
      continue;
    const archive_stamp stamp{
        combine_words(entry.nFileSizeHigh, entry.nFileSizeLow),
        combine_words(entry.ftLastWriteTime.dwHighDateTime, entry.ftLastWriteTime.dwLowDateTime)};
    current.emplace(name, stamp);
  } while (FindNextFileW(list, &entry));
  FindClose(list);

  // Let uploads settle across two scans. Hold a read handle during extraction
  // so a writer cannot modify the ZIP between validation and publication.
  for (const auto& [name, stamp] : current) {
    const auto previous = observed.find(name);
    const auto done = attempted.find(name);
    if (previous == observed.end() || previous->second != stamp ||
        (done != attempted.end() && done->second == stamp)) continue;
    const auto path = host_u82ws(drop_dir + "\\" + name);
    HANDLE source = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                OPEN_EXISTING, nullptr);
    if (source == INVALID_HANDLE_VALUE) continue;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool still_stable = GetFileInformationByHandle(source, &info) &&
        stamp == archive_stamp{combine_words(info.nFileSizeHigh, info.nFileSizeLow),
                               combine_words(info.ftLastWriteTime.dwHighDateTime,
                                             info.ftLastWriteTime.dwLowDateTime)};
    bool installed = false;
    std::string note;
    if (still_stable) {
      attempted[name] = stamp;
      installed = webxfer_import_portal_zip(local_state, name, &note);
    }
    CloseHandle(source);
    if (still_stable) {
      if (installed) {
        const auto archived = path + L".imported";
        if (!MoveFileExW(path.c_str(), archived.c_str(), 0))
          log_line(("portal: installed " + name + " and kept the ZIP in the drop folder").c_str());
        else
          log_line(("portal: installed " + name).c_str());
      } else {
        log_line(("portal: could not install " + name + " (" + note + ")").c_str());
      }
    }
  }
  observed.swap(current);
  for (auto it = attempted.begin(); it != attempted.end();) {
    if (observed.count(it->first)) ++it;
    else it = attempted.erase(it);
  }
}

}  // namespace

void portal_drop_poll(const std::string& local_state, const std::string& drop_dir) {
  static ULONGLONG next_scan = 0;
  const ULONGLONG now = GetTickCount64();
  if (drop_dir.empty() || now < next_scan || scanning.exchange(true)) return;
  next_scan = now + 5000;
  try {
    std::thread([local_state, drop_dir] {
      try {
        scan_portal_drop(local_state, drop_dir);
      } catch (...) {
        log_line("portal: drop folder scan failed");
      }
      scanning.store(false);
    }).detach();
  } catch (...) {
    scanning.store(false);
  }
}

}  // namespace longan
