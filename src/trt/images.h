// tiramisu/src/trt/images.h
// Guest image registry + on-demand loading of the DLLs a game ships.
//
// A shipped PC game is rarely one file: it brings engine DLLs, helper DLLs
// and often its own version.dll / winmm.dll shims. Without LoadLibrary the
// game cannot start, so the runtime loads guest DLLs the way Windows does:
// search the game's own directory, map, bind imports (recursively), seal,
// run DllMain, and hand out a module handle.
#ifndef TIRAMISU_IMAGES_H
#define TIRAMISU_IMAGES_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// The runtime context: backend, resolver, and where the game's own DLLs
// live on the host filesystem.
void trt_runtime_init(const trt_backend* be, const char* search_dir);
// Secondary game DLL directory when the executable lives below the mount root.
void trt_runtime_set_content_root(const char* host_dir);
const trt_backend* trt_runtime_backend(void);
// The resolver that knows about guest images (and falls back to the module
// table for system modules). Use it for the exe AND for every guest DLL.
trt_resolver* trt_runtime_resolver(void);
const trt_resolver* trt_runtime_export_resolver(void);
// Where the game's own DLLs live on the host filesystem.
const char* trt_runtime_search_dir(void);
// Dynamic DLL search directories (SetDllDirectoryW / AddDllDirectory / loader
// subdir recursion). Host filesystem paths; nullptr clears SetDllDirectory.
void trt_runtime_set_dll_dir(const char* host_dir);
int  trt_runtime_add_dll_dir(const char* host_dir);
int  trt_runtime_remove_dll_dir(int slot);
int  trt_runtime_find_dll(const char* name_or_rel, char* out, size_t cap);
// The resolver itself (for callers that need to resolve a symbol).
void* trt_resolve_full(void* user, const char* module, const char* symbol,
                       u16 ordinal);

// ---- registry ----
trt_image* trt_images_find(const char* name);      // case-insensitive
trt_image* trt_images_by_handle(void* handle);
trt_image* trt_images_first(void);                  // the main guest image
                                                 // (the exe); falls back to
                                                 // the first loaded image
                                                 // when no main registered
// ---- enumeration (the psapi bridge exposes this as the REAL guest module
// list: K32EnumProcessModules / EnumProcesses / MiniDumpWriteDump module
// table all read from here) ----
int trt_images_count(void);
trt_image* trt_images_nth(int idx);
const char* trt_images_nth_name(int idx);   // "engine.dll"
const char* trt_images_nth_path(int idx);   // host path it was loaded from
void* trt_images_handle(trt_image* img);
const char* trt_images_name(void* handle);
// STAGE 7: synthetic handles for system modules that exist as no host file
// (kernel32, api-set contracts, ...): LoadLibraryW/GetModuleHandleW hand
// these out so the CRT startup's module probes succeed like real Windows.
void* trt_images_synthetic_handle(const char* module_name);
int trt_images_is_synthetic(void* handle);
const char* trt_images_synth_name(void* handle);
// Register the main guest image (the exe mapped directly via trt_map_pe, not
// through trt_load_pe) so the psapi/dbghelp module list includes it, and so
// trt_images_first() (GetModuleHandle(NULL), FindResource(NULL)) answers
// with the exe rather than whichever image registered first.
void trt_images_register_main(trt_image* img, const char* guest_name);
// Register an alias for an image (e.g. launcher name when redirected to shipping exe).
void trt_images_register_alias(trt_image* img, const char* alias_name);

// ---- loading ----
// Map a PE from a host path, register it, seal it. If `run_entry` is set,
// the entry point is called with (handle, 1, 0) - DllMain semantics.
trt_image* trt_load_pe(const char* host_path, const char* guest_name,
                       int run_entry);

// Call a guest function with the Win64 ABI, whatever the host ABI is.
// (On the console host this is a plain call; in the Linux test harness the
// argument registers differ, so we marshal them.)
u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d);
// STAGE 7: same, but with the image's guest TEB installed as the guest gs
// base for the call (host gs restored after) — needed by any image with a
// TLS directory whose code reads gs:[0x58].
u64 trt_call_guest_img(void* fn, trt_image* img, u64 a, u64 b, u64 c, u64 d);

// Load-only mode toggle: 0 = map/bind/seal without running TLS callbacks or
// DllMain (corpus dry-run), 1 (default) = full boot semantics.
void trt_set_image_code_run(int enable);

// Escaped-guest-exception landing pad (Linux harness): armed by the innermost
// active host→guest call boundary; the terminal EH path longjmps here when a
// guest C++ exception escapes the guest images (UnityPlayer native
// wrapper-catch emulation — the boundary reports a synthetic error return).
// Per-thread: each worker thread arms its own pad around its guest call.
#include <setjmp.h>
struct eh_escape_state {
  unsigned char active;
  jmp_buf jb;
};
extern thread_local eh_escape_state g_eh_escape;

// Per-thread host→guest call nesting depth. Incremented by trt_call_guest /
// trt_call_guest_img on entry, decremented on return. EH machinery tags
// per-call state with this depth and retires it when the matching call ends.
extern thread_local unsigned g_guest_call_depth;

// Disable/invoke DLL_THREAD_ATTACH (2) and DLL_THREAD_DETACH (3) notifications
// for loaded guest images and TLS callbacks.
int  trt_images_disable_thread_calls(void* handle);
void trt_images_notify_thread(int reason);

// Drop every registered guest image (between corpus PEs: a stale image from
// a previous game directory must never satisfy the next game's imports).
void trt_images_reset(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_IMAGES_H
