// tiramisu/src/host/usb_volume_filter.h
// Pure, cross-platform classification logic that distinguishes genuine external
// USB sticks and external USB SSDs (NTFS / exFAT / FAT32) from internal Xbox
// Dev Mode / Windows OS fixed partitions (such as Xbox's internal D:\
// DevelopmentFiles partition and ghost C:/Q:/S:/T:/U:/X: system volumes).
#ifndef TIRAMISU_USB_VOLUME_FILTER_H
#define TIRAMISU_USB_VOLUME_FILTER_H

#include <cctype>
#include <cstdint>
#include <string>

namespace longan {

struct usb_drive_probe_info {
  char letter = 0;                 // 'A'..'Z'
  bool is_removable = false;       // DRIVE_REMOVABLE (USB thumb drive / SD)
  bool is_fixed = false;           // DRIVE_FIXED (internal SSD OR external USB SSD)
  bool from_os_usb_api = false;    // returned by KnownFolders::RemovableDevices / StorageDevice / FolderPicker
  bool win32_readable = false;
  uint64_t total_bytes = 0;
  std::string fs;                  // e.g. "NTFS", "exFAT", "FAT32"
  std::string label;               // volume label from GetVolumeInformationW
  bool has_internal_os_dirs = false;  // root contains DevelopmentFiles / Windows / WindowsApps / Users / etc.
  char os_drive = 'C';
  char local_drive = 0;
  char pkg_drive = 0;
};

inline std::string usb_filter_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool is_internal_xbox_dir_name(const std::string& name) {
  const std::string low = usb_filter_lower(name);
  return low == "developmentfiles" || low == "windows" || low == "windowsapps" ||
         low == "users" || low == "program files" || low == "program files (x86)" ||
         low == "programdata" || low == "systemdata" || low == "xboxos" ||
         low == "tempcontent" || low == "systemsupport" || low == "recovery" ||
         low == "wpapps" || low == "mutablebackup";
}

inline bool is_internal_xbox_volume_label(const std::string& label) {
  const std::string low = usb_filter_lower(label);
  if (low.empty()) return false;
  return low == "development" || low == "developmentfiles" ||
         low == "user content" || low == "system support" ||
         low == "temp content" || low == "localstate" ||
         low == "os" || low == "system" || low == "boot" ||
         low == "recovery" || low == "update" || low == "internal" ||
         low == "xboxos" || low == "scratch" || low == "title scratch" ||
         low == "windows" || low == "system reserved" ||
         low.find("developmentfiles") != std::string::npos ||
         low.find("system support") != std::string::npos ||
         low.find("temp content") != std::string::npos;
}

// Returns true iff this drive should be surfaced as an external USB / SSD volume.
// Populates *out_reason with a short diagnostic string for tiramisu.log.
inline bool classify_external_usb_volume(const usb_drive_probe_info& info,
                                         std::string* out_reason = nullptr) {
  auto set_reason = [&](const char* r) {
    if (out_reason) *out_reason = r;
  };
  const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(info.letter)));
  const char os_up = static_cast<char>(std::toupper(static_cast<unsigned char>(info.os_drive)));
  const char loc_up = static_cast<char>(std::toupper(static_cast<unsigned char>(info.local_drive)));
  const char pkg_up = static_cast<char>(std::toupper(static_cast<unsigned char>(info.pkg_drive)));

  // 1. OS RemovableDevices / Portable StorageDevice / User FolderPicker are authoritative.
  if (info.from_os_usb_api) {
    set_reason("os-usb-broker");
    return true;
  }

  // 2. Genuine DRIVE_REMOVABLE (USB flash drives, SD readers) are always external.
  if (info.is_removable) {
    set_reason("drive-removable");
    return true;
  }

  // 3. Only DRIVE_FIXED remains (could be an external USB SSD OR an internal Xbox partition).
  if (!info.is_fixed) {
    set_reason("skip:unsupported-drive-type");
    return false;
  }

  // 4. Exclude known host/app internal drive letters.
  if (up == 'C' || (os_up && up == os_up) || (loc_up && up == loc_up) ||
      (pkg_up && up == pkg_up)) {
    set_reason("skip:internal-os-or-app-drive");
    return false;
  }

  // 5. On Xbox Dev Mode, D:\ is ALWAYS the internal DevelopmentFiles / scratch NTFS
  //    partition (DRIVE_FIXED); external USB sticks/SSDs mount at E:\, F:\, G:\...
  //    Only allow D: if the OS USB API or DRIVE_REMOVABLE explicitly reported it above.
  if (up == 'D') {
    set_reason("skip:internal-xbox-d-drive");
    return false;
  }

  // 6. Reject internal Xbox / Windows partitions by root directory or volume label markers.
  if (info.has_internal_os_dirs) {
    set_reason("skip:internal-os-dir-markers");
    return false;
  }
  if (is_internal_xbox_volume_label(info.label)) {
    set_reason("skip:internal-xbox-volume-label");
    return false;
  }

  // 7. Reject ghost/inaccessible internal Xbox system partition letters (e.g. Q:/S:/T:/U:/X:/M:)
  //    that appear in GetLogicalDrives() with zero accessible metadata and no readability.
  if (!info.win32_readable && info.total_bytes == 0 && info.fs.empty()) {
    set_reason("skip:inaccessible-fixed-partition");
    return false;
  }

  // 8. Accept genuine external fixed volumes (external USB SSDs / HDDs on E:, F:, G:, ...).
  set_reason("external-fixed-ssd");
  return true;
}

}  // namespace longan

#endif
