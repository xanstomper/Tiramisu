#!/bin/bash
# tiramisu/tools/build-host.sh — cross-build the Tiramisu UWP host on Linux.
#   clang-cl (MSVC ABI, LLVM 19) -> lld-link -> out/layout/tiramisu.exe
# The host is a UWP image (--uwp): AppContainer + WINAPI_FAMILY_APP.
# Guest exes are separate normal desktop PEs — never compiled with --uwp.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
out="${1:-out}"; mkdir -p "$out/layout" "$out/obj"

CB="${UWP_CROSSBUILD:-$here/../gow-uwp/ref/uwp-crossbuild}"
[[ -x "$CB/scripts/build.sh" ]] || CB="$HOME/gow-uwp/ref/uwp-crossbuild"
[[ -x "$CB/scripts/build.sh" ]] || { echo "ERROR: uwp-crossbuild wrapper not found (set UWP_CROSSBUILD)" >&2; exit 2; }
[[ -d "$HOME/.cache/uwp-crossbuild/xwin/sdk" ]] || { echo "ERROR: no SDK — run: xwin --accept-license --arch x86_64 splat --output ~/.cache/uwp-crossbuild/xwin" >&2; exit 2; }
command -v makemsix >/dev/null || { echo "ERROR: makemsix not in PATH" >&2; exit 2; }

# --- 1. build the offline test first: loader core must be green on host ------
if [[ -n "${SKIP_GATE:-}" ]]; then
  echo "offline loader test: SKIPPED (SKIP_GATE)"
else
"$here/tools/build-test.sh" > "$out/obj/test.log" 2>&1 \
  || { echo "ERROR: offline loader test FAILED — see $out/obj/test.log"; tail -5 "$out/obj/test.log"; exit 3; }
echo "offline loader test: PASS"
fi

# --- 2a. build Mono for UWP (Path B step 5): stock 6.8.0.105, SGen,
#         interpreter-capable, statically linked. Cached; a no-op when
#         up to date (see third_party/mono-uwp/build-mono-uwp.sh). The
#         script prints ONLY the library path on stdout (progress goes
#         to stderr), so the path is captured here. The link below also
#         adds bcrypt.lib: Mono's Windows crypto (mono-security-windows)
#         uses BCryptGenRandom under UWP (BCrypt instead of CryptoAPI).
MONO_LIB="$("$here/third_party/mono-uwp/build-mono-uwp.sh")" \
  || { echo "ERROR: Mono for UWP build failed" >&2; exit 4; }
[[ -f "$MONO_LIB" ]] || { echo "ERROR: Mono UWP lib missing: $MONO_LIB" >&2; exit 4; }
echo "Mono for UWP: $MONO_LIB"

# --- 2. compile + link the host (UWP profile) --------------------------------
# FNA3D D3D11 (Path B step 3): the D3D11 driver + FNA3D core are compiled
# into the host so a managed game renders on the host's CoreWindow
# swapchain. MojoShader is NOT compiled separately here: it already
# lives in the host via src/trt/mojo_hlsl.c (unity build, extended with
# the effects + d3d11 backend for FNA3D) -- compiling it twice would
# duplicate every MOJOSHADER_* symbol. The SDL shim
# (third_party/fna3d-src/sdl-shim) stands in for SDL2, which does not
# exist on the console; see its README. The Linux gate keeps using
# FNA3D's OpenGL driver (llvmpipe) via third_party/fna3d/build-fna3d.sh.
# FAudio (Path B step 6): FNA's audio (SoundEffect/Song/3D) goes through
# FAudio, so the FAudio core + the Tiramisu UWP platform layer
# (third_party/faudio/uwp/FAudio_platform_uwp.c, XAudio2 output) are
# compiled into the host the same way, and FNA resolves the P/Invokes
# via the FAudio -> __Internal dllmap (mono_host.cpp). The vendored
# FAudio_platform_win32.c is NOT compiled: its IMMDeviceEnumerator-based
# WASAPI backend is DESKTOP-partition-only and cannot compile under
# WINAPI_FAMILY_APP (see uwp/README.md). XNA_Song.c is NOT compiled
# either: the UWP layer provides the MF song functions, and compiling
# both would duplicate every XNA_Song* symbol. The wmadec file IS
# compiled, without HAVE_WMADEC, for its upstream fallback decode_wma
# (logs + silence; wmadmod.dll does not exist on Xbox).
# Defines: FAUDIO_WIN32_PLATFORM selects FAudio's Win32 CRT section
# (malloc/string macros, FAudio_Log decl); FAUDIO_UWP_PLATFORM enables
# the UWP platform file; FAUDIO_DISABLE_DEBUGCONFIGURATION matches the
# Linux Release build and removes FAudio's getenv calls (getenv is
# desktop-gated in the UCRT). The /FI compat header declares lstrcpynA
# (DESKTOP-only in the SDK; FACT.c needs it); the definition is a real
# implementation in the UWP platform file. All three defines + the /FI
# apply to every TU of this build (the wrapper has no per-file flags);
# they are inert for TUs that do not use them. MF libs: the song path
# (MFStartup/SourceReader) is APP-partition; mfuuid carries the MF GUIDs.
UWP_OBJ_DIR="$out/obj" "$CB/scripts/build.sh" --uwp \
  --out "$out/layout/tiramisu.exe" --jobs "${TRT_JOBS:-2}" \
  -I third_party/fna3d-src/include \
  -I third_party/fna3d-src/MojoShader \
  -I third_party/fna3d-src/sdl-shim \
  -I third_party/mono/usr/include/mono-2.0 \
  -I third_party/faudio-src/include \
  -I third_party/faudio-src/src \
  src/host/main.cpp src/host/host_log.cpp src/host/swapchain.cpp \
  src/host/ui.cpp src/host/webxfer.cpp src/host/audio.cpp src/host/settings.cpp \
  src/host/crash_report.cpp src/host/steam_client.cpp src/host/cover_art.cpp \
  src/host/boot_anim.cpp src/host/console_detect.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp src/trt/iso.cpp \
  src/trt/dynamic_api.cpp src/trt/engine_synthesizer.cpp \
  src/trt/bridge_res.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp \
  src/trt/x86_32.cpp src/trt/x86_image.cpp \
  src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_d3d12.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp \
  src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp \
  src/trt/bridge_mmedia.cpp src/trt/acm_codec.cpp src/trt/bridge_img.cpp src/trt/bridge_d3dx.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_actctx.cpp src/trt/shell_link.cpp src/trt/bridge_version.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp src/trt/gdi_raster.cpp \
  src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_prng.cpp src/trt/bridge_dbghelp.cpp \
  src/trt/bridge_ntdll.cpp src/trt/bridge_opengl32.cpp src/trt/glsl_hlsl.cpp \
  src/trt/bridge_vulkan.cpp src/trt/spirv_hlsl.cpp src/trt/pc2xb_translator.cpp src/trt/spirv_cache.cpp src/trt/steam_auth.cpp src/trt/steam_library.cpp src/trt/universal_guest.cpp src/trt/bridge_hid.cpp \
  src/trt/bridge_xinput.cpp src/trt/bridge_wasapi.cpp src/trt/bridge_winrt.cpp src/trt/bridge_ws2.cpp src/trt/bridge_msvcrt.cpp src/trt/bridge_console_pack.cpp \
  src/trt/bridge_sys_extras.cpp src/trt/bridge_mf.cpp src/trt/bridge_gameinput.cpp \
  src/trt/bridge_dstorage.cpp src/trt/bridge_bink.cpp src/trt/bridge_oodle.cpp src/trt/bridge_wwise.cpp \
  src/trt/bridge_criware.cpp src/trt/bridge_miles.cpp \
  src/trt/bridge_heap.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_kernel32_gap.cpp src/trt/bridge_user32.cpp src/trt/bridge_user32_gap.cpp src/trt/input.cpp \
  src/trt/bridge_nvapi.cpp src/trt/bridge_gdiplus.cpp src/trt/bridge_fmod.cpp \
  src/trt/bridge_wldap32.cpp src/trt/bridge_ktmw32.cpp \
  src/trt/bridge_fmod_studio.cpp \
  src/trt/x86_exec.cpp src/trt/x86_bridge.cpp src/trt/x86_image.cpp src/trt/x86_32.cpp src/trt/x86_jit.cpp src/trt/x86_heap.cpp \
  src/trt/bridge_membudget.cpp src/trt/bridge_esram.cpp src/trt/mono_host.cpp src/trt/fna_platform.cpp \
  src/trt/trt_trace.cpp \
  third_party/mono-bridges/mono_steam_door.cpp \
  src/trt/bridge_system_native.cpp \
  third_party/mono-bridges/mono_fmod_door.cpp \
  src/trt/fna3d_host.cpp \
  third_party/fna3d-src/src/FNA3D.c \
  third_party/fna3d-src/src/FNA3D_Driver_D3D11.c \
  third_party/fna3d-src/src/FNA3D_PipelineCache.c \
  third_party/fna3d-src/sdl-shim/sdl_shim.c \
  third_party/faudio-src/src/F3DAudio.c \
  third_party/faudio-src/src/FACT3D.c third_party/faudio-src/src/FACT.c \
  third_party/faudio-src/src/FACT_internal.c \
  third_party/faudio-src/src/FAPOBase.c third_party/faudio-src/src/FAPOFX.c \
  third_party/faudio-src/src/FAPOFX_echo.c third_party/faudio-src/src/FAPOFX_eq.c \
  third_party/faudio-src/src/FAPOFX_masteringlimiter.c third_party/faudio-src/src/FAPOFX_reverb.c \
  third_party/faudio-src/src/FAudio.c \
  third_party/faudio-src/src/FAudioFX_reverb.c third_party/faudio-src/src/FAudioFX_collector.c \
  third_party/faudio-src/src/FAudioFX_volumemeter.c \
  third_party/faudio-src/src/FAudio_internal.c third_party/faudio-src/src/FAudio_internal_simd.c \
  third_party/faudio-src/src/FAudio_operationset.c \
  third_party/faudio-src/src/FAudio_platform_win32_wmadec.c \
  third_party/faudio/uwp/FAudio_platform_uwp.c \
  src/host/vendor/miniz.c src/host/vendor/miniz_tdef.c \
  src/host/vendor/miniz_tinfl.c src/host/vendor/miniz_zip.c \
  src/host/vendor/qrcodegen.c src/trt/mojo_hlsl.c src/trt/vendor_stb_vorbis.c \
  src/trt/vendor_dr_mp3.c third_party/zstddeclib.c \
  --link-arg d3d11.lib --link-arg d3d12.lib --link-arg dxgi.lib --link-arg xinput.lib \
  --link-arg d3dcompiler.lib --link-arg xaudio2_8.lib --link-arg ws2_32.lib \
  --link-arg mfplat.lib --link-arg mfreadwrite.lib --link-arg mfuuid.lib \
  --link-arg bcrypt.lib --link-arg "$MONO_LIB" \
  --link-arg /STACK:0x800000 \
  --link-arg "/DEF:$PWD/third_party/mono-bridges/host-bridges.def" \
  -- /MT /O2 /EHsc /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN \
     /D_CRT_SECURE_NO_WARNINGS /DWINRT_NOCRASH_ON_UNHANDLED \
     /DTIRAMISU_UWP_HOST \
     /DFNA3D_DRIVER_D3D11 /DWINAPI_FAMILY_WINRT=1 \
     /DMOJOSHADER_NO_VERSION_INCLUDE /DMOJOSHADER_EFFECT_SUPPORT \
     /DFAUDIO_WIN32_PLATFORM /DFAUDIO_UWP_PLATFORM /DFAUDIO_DISABLE_DEBUGCONFIGURATION \
     "/FI$here/third_party/faudio/uwp/faudio_uwp_compat.h"

# --- 2b. explicit check: the FNA3D D3D11 driver object built under the
#         UWP toolchain (compile-level gate for Path B step 3). The link
#         above already proves the objects exist and resolve (fna3d_host
#         references FNA3D_D3D11_SetHostDevice; the driver references the
#         MojoShader d3d11 backend from mojo_hlsl.o); this names the
#         objects so a missing driver file fails loudly instead of
#         silently dropping the D3D11 path.
for fna_obj in "$out/obj/"*FNA3D_Driver_D3D11*.obj \
               "$out/obj/"*FNA3D*.obj \
               "$out/obj/"*fna3d_host*.obj; do
  [[ -f "$fna_obj" ]] || { echo "ERROR: FNA3D UWP object missing: $fna_obj" >&2; exit 4; }
done
echo "FNA3D D3D11 driver objects present in $out/obj"

# --- 2c. explicit check: Mono is in the UWP link (Path B step 5). The
#         link above already proves the symbols resolve (mono_host.o
#         references mono_jit_init/mono_jit_exec; lld-link would fail
#         on an unresolved symbol); this names them so a missing Mono
#         lib fails loudly instead of silently dropping the embedding.
#         The exe does not export the Mono symbols (statically linked,
#         internal), so the check is on the linked objects + the lib.
#         FNA3D IS exported from the exe (__declspec(dllexport)) — the
#         __Internal dllmap target — so its exports are checked in the
#         exe's export table.
#         NB: each tool's output is captured to a file before grepping:
#         piping to `grep -q` makes grep exit on the first match,
#         SIGPIPE-ing the producer, which under `set -o pipefail` fails
#         the check even when the symbol is present.
llvm-nm --defined-only "$MONO_LIB" > "$out/obj/mono-symbols.txt" 2>/dev/null \
  || { echo "ERROR: llvm-nm failed on $MONO_LIB" >&2; exit 5; }
for sym in mono_jit_init mono_jit_exec mono_config_parse mono_set_dirs \
           mono_domain_assembly_open mono_add_internal_call \
           mono_install_assembly_load_hook mono_jit_cleanup; do
  grep -q " T $sym$" "$out/obj/mono-symbols.txt" \
    || { echo "ERROR: Mono symbol missing from $MONO_LIB: $sym" >&2; exit 5; }
done
mono_host_obj="$(ls "$out/obj/"*mono_host*.obj 2>/dev/null | head -1)"
[[ -f "$mono_host_obj" ]] \
  || { echo "ERROR: mono_host UWP object missing in $out/obj" >&2; exit 5; }
llvm-nm --undefined-only "$mono_host_obj" > "$out/obj/mono_host-undef.txt" 2>/dev/null \
  || { echo "ERROR: llvm-nm failed on $mono_host_obj" >&2; exit 5; }
for sym in mono_jit_init mono_jit_exec; do
  grep -q " U $sym$" "$out/obj/mono_host-undef.txt" \
    || { echo "ERROR: mono_host.o does not reference $sym (UWP Mono embedding not wired?)" >&2; exit 5; }
done
echo "Mono symbols present in UWP link: mono_jit_init, mono_jit_exec, +6 more (lib: $MONO_LIB)"
objdump -p "$out/layout/tiramisu.exe" > "$out/obj/exe-headers.txt" \
  || { echo "ERROR: objdump failed on $out/layout/tiramisu.exe" >&2; exit 5; }
grep -q "FNA3D_" "$out/obj/exe-headers.txt" \
  || { echo "ERROR: no FNA3D_* exports in $out/layout/tiramisu.exe" >&2; exit 5; }
echo "FNA3D exports present in tiramisu.exe"

# --- 2d. package the Mono framework assemblies + machine config into
#         the app layout (Path B step 5). The UWP branch of mono_host
#         derives framework_dir/config_dir from the install dir:
#         <install>/mono/4.5/*.dll + <install>/mono/gac/... and
#         <install>/etc/mono/config, mirroring Linux's /usr/lib + /etc
#         split. The assemblies are the same 6.8.0.105 IL the Linux
#         path uses (platform-independent; not rebuilt). tools/pack.sh
#         includes $out/layout in the MSIX.
MONO_FW="${MONO_FRAMEWORK_DIR:-/usr/lib/mono}"
[[ -d "$MONO_FW/4.5" && -f "$MONO_FW/4.5/mscorlib.dll" ]] \
  || { echo "ERROR: Mono framework assemblies not found: $MONO_FW/4.5" >&2; exit 6; }
rm -rf "$out/layout/mono"
mkdir -p "$out/layout/mono" "$out/layout/etc/mono"
cp -r "$MONO_FW/4.5" "$out/layout/mono/"
[[ -d "$MONO_FW/gac" ]] && cp -r "$MONO_FW/gac" "$out/layout/mono/"
[[ -f /etc/mono/config ]] && cp /etc/mono/config "$out/layout/etc/mono/config"
# FNA substitutes for XNA 4.0 (Celeste and other managed XNA titles). The
# offline gate sets mono_host.cfg.assemblies_path to third_party/fna/out;
# the host falls back to <install>/fna, so ship the same DLLs there.
FNA_OUT="$here/third_party/fna/out"
[[ -f "$FNA_OUT/Microsoft.Xna.Framework.dll" &&
   -f "$FNA_OUT/Microsoft.Xna.Framework.Game.dll" &&
   -f "$FNA_OUT/Microsoft.Xna.Framework.Graphics.dll" ]] \
  || { echo "ERROR: FNA assemblies missing in $FNA_OUT (run third_party/build-fna-stack.sh)" >&2; exit 6; }
rm -rf "$out/layout/fna"
mkdir -p "$out/layout/fna"
cp "$FNA_OUT"/Microsoft.Xna.Framework*.dll "$FNA_OUT/Microsoft.Xna.Framework.dll.config" "$out/layout/fna/"
echo "FNA XNA assemblies packaged in $out/layout/fna"
# Tiramisu UWP: map every managed P/Invoke library to the host exe's
# exports. Programmatic dllmaps (assembly_load_hook) do not apply to all
# resolution paths on this Mono build; the config file is read at init.
python3 - "$out/layout/etc/mono/config" <<'PYCFG'
import sys
p = sys.argv[1]
try:
    txt = open(p).read()
except FileNotFoundError:
    txt = "<configuration>\n</configuration>\n"
maps = ["FNA3D", "FAudio", "fmod", "fmodstudio", "CSteamworks",
        "sdkencryptedappticket", "System.Native"]
add = "".join('  <dllmap dll="%s" target="__Internal"/>\n' % m for m in maps)
if "XanTiramisuInternal" not in txt:
    txt = txt.replace("</configuration>",
        "  <!-- XanTiramisuInternal: host exports -->\n" + add + "</configuration>")
open(p, "w").write(txt)
PYCFG
echo "Mono framework packaged: $(du -sh "$out/layout/mono" | cut -f1) assemblies + config"

# --- 2e. explicit check: FAudio is in the UWP link (Path B step 6). The
#         link above already proves the objects exist and resolve
#         (FAudio.c references the FAudio_Platform* functions from the
#         UWP layer; FACT.c references FAudio_fopen/memopen; lld-link
#         would fail on an unresolved symbol); this names the objects and
#         the exports so a missing FAudio file fails loudly instead of
#         silently dropping FNA audio on the console. FAudio IS exported
#         from the exe (FAUDIOAPI __declspec(dllexport) under _WIN32) --
#         the __Internal dllmap target -- so its exports are checked in
#         the exe's export table, and the FAudio dllmap string is checked
#         in mono_host.o. The win32-platform object must NOT exist: that
#         file's WASAPI backend is DESKTOP-partition-only and its symbols
#         would duplicate the UWP layer's.
#         NB: exe-headers.txt was dumped by section 2c; re-dump it here
#         so this check stands alone if sections are reordered.
for faudio_obj in "$out/obj/"*FAudio_platform_uwp*.obj \
                 "$out/obj/"*FAudio*.obj \
                 "$out/obj/"*FACT*.obj \
                 "$out/obj/"*FAPO*.obj \
                 "$out/obj/"*F3DAudio*.obj; do
  [[ -f "$faudio_obj" ]] || { echo "ERROR: FAudio UWP object missing: $faudio_obj" >&2; exit 5; }
done
if ls "$out/obj/"*FAudio_platform_win32.[0-9]*.obj >/dev/null 2>&1; then
  echo "ERROR: FAudio win32-platform object present in $out/obj (DESKTOP-only backend must not be compiled for UWP)" >&2; exit 5
fi
echo "FAudio UWP objects present in $out/obj (UWP layer + core + FACT/FAPO/F3DAudio)"
objdump -p "$out/layout/tiramisu.exe" > "$out/obj/exe-headers.txt" \
  || { echo "ERROR: objdump failed on $out/layout/tiramisu.exe" >&2; exit 5; }
for sym in FAudioCreate FAudio_GetDeviceCount FAudio_CreateMasteringVoice \
           FAudio_CreateSourceVoice XNA_SongInit XNA_PlaySong \
           FAudio_fopen FAudio_memopen FACT3DInitialize FAudioLinkedVersion; do
  grep -q "$sym" "$out/obj/exe-headers.txt" \
    || { echo "ERROR: FAudio export missing from $out/layout/tiramisu.exe: $sym" >&2; exit 5; }
done
echo "FAudio exports present in tiramisu.exe: FAudioCreate, FAudio_GetDeviceCount, +8 more"
mono_host_obj_2e="$(ls "$out/obj/"*mono_host*.obj 2>/dev/null | head -1)"
[[ -f "$mono_host_obj_2e" ]] \
  || { echo "ERROR: mono_host UWP object missing in $out/obj" >&2; exit 5; }
strings "$out/obj/"*mono_host*.obj | grep -x "FAudio" > /dev/null \
  || { echo "ERROR: mono_host.o lacks the FAudio dllmap string (FAudio -> __Internal not wired?)" >&2; exit 5; }
echo "FAudio dllmap string present in mono_host.o (FAudio -> __Internal)"

# --- 3. sanity: UWP image MUST be an AppContainer image ----------------------
objdump -p "$out/layout/tiramisu.exe" | grep -E 'DllCharacteristics|Subsystem' || true
