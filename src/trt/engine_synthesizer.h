// tiramisu/src/trt/engine_synthesizer.h
// Autonomous Engine Synthesizer & Adaptive Execution Harness.
//
// Automatically inspects transferred or local game folders, discovers
// executables, classifies engine architectures (Unity IL2CPP/Mono, Unreal,
// Godot, GameMaker, FNA/XNA, Source, Custom Native), determines PE32 vs PE64
// and managed vs native, synthesizes graphics, audio, Steam sandboxes,
// and produces a complete runnable profile.
//
// Classification is EVIDENCE-BASED: the chosen executable's real PE import
// table (and delay-import table) is parsed, so a game is only called Unity
// when it actually imports UnityPlayer.dll / mono, FNA when it imports FNA.dll,
// D3D9 when it imports d3d9.dll, and so on. File names on disk are used as a
// secondary signal only where the engine is loaded by name at runtime.
#ifndef TIRAMISU_ENGINE_SYNTHESIZER_H
#define TIRAMISU_ENGINE_SYNTHESIZER_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

enum trt_engine_type {
  TRT_ENGINE_UNKNOWN = 0,
  TRT_ENGINE_UNITY_IL2CPP,
  TRT_ENGINE_UNITY_MONO,
  TRT_ENGINE_UNREAL,
  TRT_ENGINE_GODOT,
  TRT_ENGINE_GAMEMAKER,
  TRT_ENGINE_FNA_XNA,
  TRT_ENGINE_SOURCE,
  TRT_ENGINE_CUSTOM_NATIVE,
  // Managed (.NET) image with no engine evidence: runs on the Mono host but is
  // not an FNA/XNA title. Appended so the existing enumerators keep their values.
  TRT_ENGINE_MANAGED_DOTNET,
  // Universal: no engine evidence. The universal guest accepts any game, the
  // import surface is "fill unknowns dynamically", and PC games can be cleared
  // onto Xbox through the API-translation layer (Phase 3+). This is the
  // default when a game cannot be classified as any specific engine.
  TRT_ENGINE_UNIVERSAL
};

// Flags on trt_engine_profile::universal_flags (OR of these; 0 = plain).
enum trt_universal_flag {
  // TRT_UNIV_CLEAR_PC: PC games may be cleared to the Xbox universal path —
  // dump the PC import surface, translate PC imports to Xbox equivalents,
  // pack as universal guest. (Phase 3.)
  TRT_UNIV_CLEAR_PC = 1,
  // TRT_UNIV_FORCE: force universal even when engine evidence exists
  // (manual override).
  TRT_UNIV_FORCE = 2
};

enum trt_graphics_api {
  TRT_GFX_UNKNOWN = 0,
  TRT_GFX_D3D11,
  TRT_GFX_D3D12,
  TRT_GFX_D3D9,
  TRT_GFX_VULKAN,
  TRT_GFX_OPENGL
};

enum trt_audio_middleware {
  TRT_AUDIO_UNKNOWN = 0,
  TRT_AUDIO_FMOD,
  TRT_AUDIO_XAUDIO2,
  TRT_AUDIO_WASAPI,
  TRT_AUDIO_OPENAL,
  TRT_AUDIO_WWISE
};

struct trt_engine_profile {
  std::string game_name;
  std::string game_dir;
  std::string main_exe_rel;
  std::string main_exe_abs;
  bool is_pe32 = false;
  bool is_pe64 = false;
  bool is_managed = false; // .NET / CLR (Mono)
  // False when the chosen exe could not be read as a PE. The synthesizer then
  // refuses by name instead of guessing PE64 (a guess used to reach the loader
  // as a wrong-arch launch).
  bool pe_scanned = false;
  // Engine payload closure: the exe alone is classified, not the engine's data.
  // An exe copied without *_Data/Managed, GameAssembly.dll, *.pak or Content\
  // still classifies as Unity/UE, then dies in "Failed to load mono".
  bool payload_complete = true;
  std::vector<std::string> missing_payload;
  // Screen the launch asks for. The console budget (5 GB Dev Mode, 1080p-class
  // output) is the default; TRT_SCREEN=WxH overrides it. Engines that ignore
  // command-line size still get their own settings file (see
  // trt_engine_write_user_settings).
  int screen_width = 1280;
  int screen_height = 720;
  bool fullscreen = false;

  trt_engine_type engine_type = TRT_ENGINE_UNKNOWN;
  trt_graphics_api preferred_gfx = TRT_GFX_D3D11;
  trt_audio_middleware preferred_audio = TRT_AUDIO_UNKNOWN;

  uint32_t steam_appid = 0;
  uint32_t universal_flags = 0;      // OR of trt_universal_flag (0 = plain)
  bool has_steam = false;

  std::vector<std::string> cmdline_args;
  // Bridge modules Tiramisu will service for this title, "module:bridge".
  std::vector<std::string> synthetic_shims;
  // Lower-case imports from the executable and its reachable bundled DLLs.
  std::vector<std::string> imported_modules;
  std::string summary;
};

struct trt_backend;

// Host capability probes for the launch preflight. Filled by the caller (host
// main, offline harness test) so this module stays free of host/bridge deps.
struct trt_host_caps {
  int d3d11_device = 0;  // 1 = host ID3D11Device+Context pair installed
  int d3d12_device = 0;  // 1 = host D3D12 device (0 = recorded only)
  int mono_host = 0;     // 1 = this build links a managed (Mono) host
  int compositor = 0;    // 1 = frames reach a window (swapchain present)
  int audio_device = 0;  // 1 = an audio endpoint exists
  int opengl_renderer = 0; // 1 = the WGL bridge has rendering and present hooks
};

// Why a launch is expected to stop, named. OR of these in the report.
enum trt_preflight_blocker {
  TRT_PRE_NOT_A_PE = 1u << 0,         // exe unreadable / not a PE
  TRT_PRE_PAYLOAD_MISSING = 1u << 1,  // engine data absent (Unity/UE/XNA)
  TRT_PRE_MANAGED_HOST = 1u << 2,     // managed image, no Mono host linked
  TRT_PRE_NO_GFX_DEVICE = 1u << 3,    // chosen API has no host device
  TRT_PRE_GFX_NOT_EXECUTED = 1u << 4, // API recorded/trapped, never drawn
  TRT_PRE_NO_COMPOSITOR = 1u << 5,    // pixels cannot reach a window
  TRT_PRE_ANTI_CHEAT = 1u << 6,       // EAC / BattlEye imported
  TRT_PRE_STUB_MODULES = 1u << 7      // graphics/audio imports serviced by stubs
};

struct trt_preflight_report {
  unsigned blockers = 0;              // OR of trt_preflight_blocker
  std::vector<std::string> lines;     // one "code: text" line per blocker
  std::vector<std::string> notes;     // non-blocking facts (appid guess, audio)
  bool runnable = false;              // blockers == 0
};

#ifdef __cplusplus
extern "C" {
#endif

// Read explicit install metadata. Zero means no usable identity.
uint32_t trt_steam_read_appid(const char* game_dir);

// Synthesize engine profile for a game directory.
// Automatically chooses the optimal executable, generates .tiramisu/profile.json
// and .tiramisu/shims.txt, and returns true
// on success.
bool trt_synthesize_engine(const char* game_dir, const char* target_exe_hint,
                           trt_engine_profile* out_profile);

// Synthesize engine profile directly for a single executable file.
// Inspects headers, imports, delay-imports, and engine byte signatures without
// requiring a directory tree walk.
bool trt_synthesize_engine_for_file(const char* exe_path,
                                    trt_engine_profile* out_profile);

// Autonomous Runtime Provisioning:
// Universal runtime wiring that provisions graphics bridges (D3D11, D3D12, Vulkan,
// OpenGL, D3D9/8), audio middleware (XAudio2, FMOD, Wwise, OpenAL, DirectSound),
// storage & video middleware (Bink, CriWare, Miles, Oodle, DStorage), and blanket-thunks
// any uncataloged or third-party imported DLLs so games execute without per-game hacks.
int trt_autonomous_provision_runtime(const trt_backend* be, const trt_engine_profile* prof);

// Launch preflight: resolve the synthesized profile against real host
// capability BEFORE the first import binds, so the wall is logged once instead
// of being discovered 400 log lines later. Writes the verdict into
// <game_dir>/.tiramisu/profile.json ("preflight") and emits one summary line
// through trt_core_log. Returns the number of blockers (0 = nothing known to
// stop it). `caps` may be NULL: every capability then counts as absent.
int trt_engine_launch_preflight(const trt_engine_profile* prof,
                                const trt_host_caps* caps,
                                trt_preflight_report* out);

// Names the profile's chosen graphics API is expected to fail on this build.
const char* trt_preflight_blocker_name(unsigned flag);

// Write the engine's own settings file with the synthesized screen, so a title
// does not boot at its 4K / 12 GB defaults inside a 5 GB Dev-Mode budget.
// Implemented for Unreal (GameUserSettings.ini under the title's
// Saved/Config/<platform>), which is the engine that reads it back on start.
// `drive_c` is the host directory mounted as the guest's C:. Returns 1 when a
// file was written (its host path lands in out_path), 0 when this engine has no
// such file.
int trt_engine_write_user_settings(const trt_engine_profile* prof,
                                   const char* drive_c,
                                   char* out_path, size_t out_path_cap);

// Parse the import + delay-import module names of any PE file.
// Writes a lower-case comma-separated list into out_csv (NUL terminated) and
// returns the number of modules, or -1 if the file is not a readable PE.
int trt_pe_scan_import_modules(const char* path, char* out_csv, size_t cap);

const char* trt_engine_type_name(trt_engine_type t);
const char* trt_graphics_api_name(trt_graphics_api g);
const char* trt_audio_middleware_name(trt_audio_middleware a);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_ENGINE_SYNTHESIZER_H
