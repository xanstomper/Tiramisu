// tiramisu/src/trt/shell_link.h
// MS-SHLLINK (.lnk) reader: lets the runtime launch applications from Windows
// shortcuts (Start-menu / desktop entries) the way Explorer would — target,
// arguments, working directory, icon, show command.
//
// Attribution: structure + hardening rules (size cap, class-id check, bounded
// string reads, LinkInfo local-path assembly, StringData order) are mapped
// from Win32Bridge/Bridge/ShellLinkReader.h in
//   https://github.com/Pedro1234-code/DesktopMode_uwp   (MIT, Pdawg11239 /
//   Pedro1234-code). Rewritten for Tiramisu (UTF-16 = u16, no windows.h, VFS
//   file access). See THIRD_PARTY_NOTICES.md.
//
// Not covered (named gaps): LinkTargetIDList-only shortcuts (shell items with
// no LinkInfo path, e.g. UWP/AppX shortcuts) are reported with target empty
// and `has_idlist` set; the caller decides how to handle them.
#ifndef TIRAMISU_SHELL_LINK_H
#define TIRAMISU_SHELL_LINK_H

#include "trt.h"

#include <stddef.h>
#include <string>

struct trt_shelllink {
  std::u16string target;            // LinkInfo local base path + suffix
  std::u16string relative_path;
  std::u16string description;       // NAME_STRING
  std::u16string working_dir;
  std::u16string arguments;
  std::u16string icon_path;
  int icon_index = 0;
  u32 show_command = 1;             // SW_SHOWNORMAL
  u16 hot_key = 0;
  bool has_idlist = false;
};

constexpr size_t kTrtShellLinkMaxSize = 1024 * 1024;

// Parse an in-memory .lnk. Returns false on any structural violation.
bool trt_shelllink_parse(const u8* data, size_t len, trt_shelllink* out);

// Read + parse a .lnk through the guest VFS.
bool trt_shelllink_load(const u16* guest_path, trt_shelllink* out);

// Best launch target: absolute `target`, else `relative_path` resolved against
// the shortcut's own directory. Empty if the link carries neither.
std::u16string trt_shelllink_launch_target(const trt_shelllink& link,
                                           const std::u16string& link_path);

#endif
