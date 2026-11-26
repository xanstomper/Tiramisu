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
//
// PLUS: a UNIVERSAL path for "one engine for everything." When no engine
// evidence is found, instead of the generic Custom Native bucket, we emit a
// UNIVERSAL profile whose import surface is empty-by-default: the universal
// guest *accepts anything*, and the dynamic API synthesizer fills unknowns
// at boot. This is the default for anything that can't be classified —
// PC games included.
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
  // Universal: no engine evidence. The universal guest accepts any game,
  // the import surface is "fill unknowns dynamically", and PC games are
  // cleared onto Xbox through the API-translation layer (Phase 3+).
  TRT_ENGINE_UNIVERSAL
};

enum trt_universal_flag {
  // 0 = plain universal. 1 = "clear" mode: PC games may be cleared to the
  // Xbox universal path (dump imports, translate PC imports to Xbox
  // equivalents, pack as universal guest).
  TRT_UNIV_CLEAR_PC = 1,
  // 2 = force universal even when evidence exists (manual override).
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

  trt_engine_type engine_type = TRT_ENGINE_UNKNOWN;
  trt_graphics_api preferred_gfx = TRT_GFX_D3D11;
  trt_audio_middleware preferred_audio = TRT_AUDIO_UNKNOWN;
  uint32_t universal_flags = 0;      // OR of trt_universal_flag (0 = plain)

  uint32_t steam_appid = 480;
  bool has_steam = false;

  std::vector<std::string> cmdline_args;
  // Bridge modules Tiramisu will service for this title, "module:bridge".
  std::vector<std::string> synthetic_shims;
  // Lower-case module names imported (directly or delay-loaded) by the exe.
  std::vector<std::string> imported_modules;
  std::string summary;
};

#ifdef __cplusplus
extern "C" {
#endif

// Synthesize engine profile for a game directory.
// Automatically chooses the optimal executable, generates .tiramisu/profile.json
// and .tiramisu/shims.txt, sets up steam_appid.txt if needed, and returns true
// on success.
bool trt_synthesize_engine(const char* game_dir, const char* target_exe_hint,
                           trt_engine_profile* out_profile);

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
