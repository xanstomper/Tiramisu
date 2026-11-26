// tiramisu/src/trt/mono_host.h
// Mono embedding for managed (.NET) games — Path B (Celeste).
//
// A managed exe (CLR header present, e.g. Celeste.exe: ILONLY +
// 32BITREQUIRED) cannot run through the native PE64 loader: its entry
// point is mscoree!_CorExeMain and its code is IL. Instead the host embeds
// the Mono runtime (MIT) and invokes the managed Main directly. The game
// runs as 64-bit managed code; its XNA references bind to the FNA
// substitutes (third_party/fna) and its native P/Invokes bind through
// dllmaps installed here (offline: the real FMOD/Steam bridges in
// third_party/mono-bridges).
//
// PLATFORM: the embedding is real on Linux (offline harness), where
// libmono-2.0 is dlopen'd from third_party/mono (see its README), and
// on Windows/UWP, where the statically linked Mono built by
// third_party/mono-uwp is embedded directly (same 6.8.0.105 runtime;
// interpreter mode forced on UWP — the AppContainer blocks JIT code
// generation). Runtime Mono-on-UWP execution is console-verification-
// pending: it can only be proven by a console test (no UWP runtime
// exists on Linux).
//
// FNA PLATFORM: managed XNA games pick their windowing/input backend
// through trt_mono_config.fna_platform ("tiramisu" = the host surface +
// host input, "sdl2" = SDL2 windowing for the offline harness). The
// selection is applied as environment variables before Main runs, and
// the Tiramisu backend's native internal calls (src/trt/fna_platform.cpp)
// are registered with the runtime on every run, so a game that selects
// the Tiramisu backend always finds its native bridge.
//
// One run per process: Mono does not support re-initialisation, and
// trt_mono_run blocks until Main returns.
#ifndef TIRAMISU_MONO_HOST_H
#define TIRAMISU_MONO_HOST_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

// A P/Invoke module redirect: managed code P/Invoking `module` loads
// `target` instead. `target` is an absolute path to a .so (or a bare
// soname resolved by the system loader) on Linux; on UWP it may also be
// "__Internal" (resolve from the exe's own exports, e.g. FNA3D).
// Installed into EVERY image the runtime loads (via the assembly-load
// hook), so it covers the exe, its managed DLLs, and lazily-loaded
// assemblies alike.
typedef struct trt_mono_dllmap {
  const char* module;  // e.g. "fmod"
  const char* target;  // e.g. "/abs/path/libfmod-real.so"
} trt_mono_dllmap;

typedef struct trt_mono_config {
  const char* exe_path;         // managed exe to run (required)
  const char* libmono_path;     // explicit libmono-2.0.so.1 (NULL = search:
                                // libmono-2.0.so.1, libmonosgen-2.0.so.1,
                                // libmonoboehm-2.0.so.1 via the loader)
  const char* framework_dir;    // Mono framework root, e.g. "/usr/lib"
                                // (NULL = "/usr/lib"); mscorlib lives in
                                // <framework_dir>/mono/4.5/
  const char* config_dir;       // Mono config dir, e.g. "/etc" (NULL = "/etc")
  const char* assemblies_path;  // extra assembly search dirs, ':'-separated
                                // (NULL = none). The FNA out dir goes here.
  const trt_mono_dllmap* dllmaps;
  int dllmap_count;
  int interpreter;              // 1 = Mono interpreter mode (no JIT).
                                // 0 = JIT (default; proven by the spike).
                                // Interpreter mode is the UWP candidate:
                                // it sidesteps JIT/W^X concerns entirely
                                // for a game this light.
  const char* domain_name;      // NULL = "tiramisu-mono"
  const char* fna_platform;     // FNA windowing/input backend for XNA
                                // games: "tiramisu" (host CoreWindow +
                                // host input; the console backend),
                                // "sdl2" (SDL2 windowing; the offline
                                // harness default), NULL/"" = leave the
                                // environment alone (whatever FNA picks
                                // from FNA_PLATFORM_BACKEND, default
                                // SDL3). Applied as env vars before Main
                                // runs; an unknown value fails the run
                                // with TRT_ERR_MONO (fail-visible).
  void (*log)(const char* line);  // NULL = stderr with a "mono: " prefix
} trt_mono_config;

// Returns 1 if the PE file image has a CLR (COM descriptor) directory —
// i.e. it is a managed assembly that must run under Mono, not the native
// PE loader. Handles PE32 and PE32+. Bounds-checked; returns 0 for
// malformed or truncated inputs. Platform-independent (pure parsing).
int trt_pe_is_managed(const u8* file, size_t file_size);

// Run a managed exe's Main under an embedded Mono runtime. Blocks until
// Main returns (a game loop runs until the process is stopped). Returns
// TRT_OK when Main returned normally; TRT_ERR_MONO on any embedding
// failure (detail in trt_mono_last_error()); if Main returns int, the
// value is written to *exit_code_out (NULL to ignore). An unhandled
// managed exception is printed in full to stderr (via
// mono_print_unhandled_exception) and reported as TRT_ERR_MONO.
// Call from the thread that should host the game; the runtime attaches to
// the calling thread.
int trt_mono_run(const trt_mono_config* cfg, const char* const* args,
                 int nargs, int* exit_code_out);

// Human-readable detail of the last embedding failure ("" when none).
const char* trt_mono_last_error(void);

// 1 when this build embeds a managed runtime (Mono), 0 when it was compiled
// without one. The launch preflight uses it to name a managed title as
// unrunnable up front instead of letting mscoree!<Flavor>Main fail later.
int trt_mono_host_present(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_MONO_HOST_H
