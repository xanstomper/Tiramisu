// tiramisu/src/trt/bridge_version.h
// version.dll / api-ms-win-core-version-l1-1-* bridge.
//
// Many PC apps and installers query their own (or a companion's) file
// version via GetFileVersionInfo* / VerQueryValue*. On UWP there is no
// version resource loader, so without this bridge the call either fails to
// resolve at load time or returns nothing and the app refuses to start.
//
// This bridge reads the target file through the guest VFS, parses its PE
// .rsrc directory for an RT_VERSION (type 16) resource, and decodes the
// standard VS_VERSION_INFO / StringFileInfo / VarFileInfo tree that
// VerQueryValue walks.
//
// Attribution: resource-extraction + version-block decoding model mapped from
// Win32Bridge/Bridge/VersionShims.cpp in
//   https://github.com/Pedro1234-code/DesktopMode_uwp   (MIT).
// Clean-room rewrite onto Tiramisu's VFS + u16 conventions.
#ifndef TIRAMISU_BRIDGE_VERSION_H
#define TIRAMISU_BRIDGE_VERSION_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_version(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_VERSION_H
