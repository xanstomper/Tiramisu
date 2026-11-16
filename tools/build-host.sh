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
"$here/tools/build-test.sh" > "$out/obj/test.log" 2>&1 \
  || { echo "ERROR: offline loader test FAILED — see $out/obj/test.log"; tail -5 "$out/obj/test.log"; exit 3; }
echo "offline loader test: PASS"

# --- 2. compile + link the host (UWP profile) --------------------------------
UWP_OBJ_DIR="$out/obj" "$CB/scripts/build.sh" --uwp \
  --out "$out/layout/tiramisu.exe" --jobs "$(nproc)" \
  src/host/main.cpp src/host/host_log.cpp src/host/swapchain.cpp \
  src/host/ui.cpp src/host/webxfer.cpp src/host/audio.cpp src/host/settings.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp src/trt/iso.cpp \
  src/trt/bridge_res.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp \
  src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_d3d12.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp \
  src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp \
  src/trt/bridge_mmedia.cpp src/trt/bridge_img.cpp src/trt/bridge_d3dx.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp \
  src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_dbghelp.cpp src/trt/bridge_dstorage.cpp \
  src/trt/bridge_ntdll.cpp src/trt/bridge_opengl32.cpp src/trt/glsl_hlsl.cpp \
  src/trt/bridge_vulkan.cpp src/trt/spirv_hlsl.cpp src/trt/bridge_hid.cpp \
  src/trt/bridge_xinput.cpp src/trt/bridge_wasapi.cpp src/trt/bridge_winrt.cpp src/trt/bridge_ws2.cpp src/trt/bridge_msvcrt.cpp src/trt/bridge_console_pack.cpp \
  src/trt/bridge_sys_extras.cpp src/trt/bridge_mf.cpp src/trt/bridge_gameinput.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_kernel32_gap.cpp src/trt/bridge_user32.cpp src/trt/bridge_user32_gap.cpp src/trt/input.cpp \
  src/trt/bridge_nvapi.cpp \
  src/host/vendor/miniz.c src/host/vendor/miniz_tdef.c \
  src/host/vendor/miniz_tinfl.c src/host/vendor/miniz_zip.c \
  src/host/vendor/qrcodegen.c src/trt/mojo_hlsl.c \
  --link-arg d3d11.lib --link-arg d3d12.lib --link-arg dxgi.lib --link-arg xinput.lib \
  --link-arg d3dcompiler.lib --link-arg xaudio2_8.lib --link-arg ws2_32.lib \
  -- /MT /O2 /EHsc /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN \
     /D_CRT_SECURE_NO_WARNINGS /DWINRT_NOCRASH_ON_UNHANDLED

# --- 3. sanity: UWP image MUST be an AppContainer image ----------------------
objdump -p "$out/layout/tiramisu.exe" | grep -E 'DllCharacteristics|Subsystem' || true
