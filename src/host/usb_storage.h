// tiramisu/src/host/usb_storage.h
// NTFS / exFAT / FAT32 USB & external drive auto-detection, WinRT broker
// fallback, Xbox FolderPicker integration, and background copy/extract installer.
#ifndef TIRAMISU_USB_STORAGE_H
#define TIRAMISU_USB_STORAGE_H

#include "webxfer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace longan {

// Shared UTF-8 <-> UTF-16 host conversion & recursive directory cleanup helpers.
std::string host_ws2u8(const wchar_t* w);
std::wstring host_u82ws(const std::string& s);
bool host_delete_dir_recursive(const std::string& path);

namespace usb_storage {

// Configure LocalState / Games / Package paths before polling.
void init(const std::string& local_state, const std::string& games_dir,
          const std::string& package_dir);

// Non-blocking external storage discovery poll (background worker thread).
// Returns true whenever the discovered volume/item list changed.
bool poll(bool force = false);

// Xbox UWP FolderPicker (non-blocking async op polled on the UI thread).
void begin_folder_picker();
void poll_folder_picker();

// Start copying / extracting an external USB item (folder, .zip, or .iso)
// into LocalState\games on a background worker thread.
bool start_install_by_id(const std::string& item_id, const std::string& custom_name);

// Consumes completion flag from the background USB installer worker.
bool poll_install_finished(std::string* out_toast);

// Live transfer progress for the Xbox TV UI & LAN dashboard.
void get_copy_progress(bool* out_active, uint64_t* out_done,
                       uint64_t* out_total, std::string* out_label);

const std::vector<webxfer_storage_volume>& volumes();
bool find_item_at_index(int index, std::string* out_id, std::string* out_name);

}  // namespace usb_storage
}  // namespace longan

#endif
