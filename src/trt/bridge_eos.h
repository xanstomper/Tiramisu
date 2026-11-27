// tiramisu/src/trt/bridge_eos.h
// Honest offline-safe Epic Online Services (EOSSDK) and GOG Galaxy bridges.
// Never bypasses DRM or anti-cheat: AntiCheatClient returns NULL, online
// auth/ecom queries report EOS_NoConnection / offline cleanly so games reach
// offline/single-player menus without crashing on missing imports.
#ifndef TIRAMISU_BRIDGE_EOS_H
#define TIRAMISU_BRIDGE_EOS_H

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_eos(void);

#ifdef __cplusplus
}
#endif

#endif
