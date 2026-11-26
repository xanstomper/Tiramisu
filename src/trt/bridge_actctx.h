// tiramisu/src/trt/bridge_actctx.h
// Side-by-side activation contexts (kernel32 CreateActCtx family).
//
// Attribution: the activation-context model (manifest parsing of
// assemblyIdentity / dependentAssembly / requestedExecutionLevel /
// supportedOS / WindowsSettings, per-thread activation stack with cookies,
// process-default context, Query*ActCtx* information classes) is mapped from
// Win32Bridge/Bridge/ActivationContext.cpp in
//   https://github.com/Pedro1234-code/DesktopMode_uwp   (MIT, Pdawg11239 /
//   Pedro1234-code). This file is a clean-room rewrite onto Tiramisu's
//   VFS + resource bridge + u16 string conventions; see THIRD_PARTY_NOTICES.md.
#ifndef TIRAMISU_BRIDGE_ACTCTX_H
#define TIRAMISU_BRIDGE_ACTCTX_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the rest of the runtime cares about from the CURRENT (thread-active,
// else process-default) activation context.
typedef struct trt_actctx_info {
  int valid;              // a context is active
  int common_controls_v6; // depends on Microsoft.Windows.Common-Controls 6.x
  int long_path_aware;
  int ui_access;
  int run_level;          // 0 unspecified, 1 asInvoker, 2 highestAvailable,
                          // 3 requireAdministrator
  int dpi_aware;          // 0 unset/false, 1 true/system, 2 per-monitor
  int dependency_count;
} trt_actctx_info;

// Merge CreateActCtx* & friends onto kernel32/kernelbase (call after the
// kernel32 core registration; merge semantics, safe to call twice).
int trt_bridge_register_actctx(void);

// Query the current context (returns 1 if one is active).
int trt_actctx_current_info(trt_actctx_info* out);

// Load RT_MANIFEST (type 24, id 1 = CREATEPROCESS_MANIFEST_RESOURCE_ID) from
// a loaded module and install it as the process-default context — exactly
// what the Windows loader does for the main executable. Returns the context
// handle or NULL if the module carries no manifest.
void* trt_actctx_install_process_default(void* hmod);

// Drop every context (test isolation / process reset).
void trt_actctx_reset(void);

#ifdef __cplusplus
}
#endif

#endif
