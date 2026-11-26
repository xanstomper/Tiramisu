#!/bin/bash
# tiramisu/tools/corpus-smoke.sh
# Hardware-free verification of the real-game corpus:
# builds tests/test_corpus.cpp with the same clang++ flag style build-test.sh
# uses for test_real_load, then runs it on every PE found under
# ~/tiramisu/corpus-local/ (depth 2), tee-ing everything to
# tests/out/corpus-report.md.
#
# Exit 0 = no LOAD FAILED. Corrupted/MZ-less/unsupported-arch inputs report
# SKIP or LOAD FAILED individually — a SKIP never fails the gate.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=tests/out
mkdir -p "$out"

# --- same toolchain pick as build-test.sh ---
LLVM_BIN="/usr/lib/llvm-19/bin"
CLANG="$LLVM_BIN/clang"
[[ -x "$CLANG" ]] || CLANG="$HOME/.local/bin/clang-19"
[[ -x "$CLANG" ]] || { echo "ERROR: clang 19 not found" >&2; exit 1; }

# --- build test_corpus (compile C translation units with C compiler first) ---
cobj="$out/smoke_c_objs"
mkdir -p "$cobj"
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz_tinfl.c -o "$cobj/miniz_tinfl.o" || exit 1
"$CLANG" -std=gnu11 -O2 -fno-pie -c src/trt/mojo_hlsl.c -o "$cobj/mojo_hlsl.o" || exit 1
"$CLANG" -O2 -Wall -fno-pie -c src/trt/vendor_stb_vorbis.c -o "$cobj/vendor_stb_vorbis.o" -DSTB_VORBIS_NO_STDIO || exit 1
"$CLANG" -O2 -Wall -fno-pie -c src/trt/vendor_dr_mp3.c -o "$cobj/vendor_dr_mp3.o" || exit 1

clang++ -std=c++17 -O2 -Wall -no-pie -Ithird_party/mono/usr/include/mono-2.0 \
  tests/test_corpus.cpp \
  src/trt/dynamic_api.cpp src/trt/engine_synthesizer.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/vfs.cpp src/trt/iso.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_d3d12.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_membudget.cpp src/trt/bridge_esram.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp src/trt/bridge_mmedia.cpp src/trt/acm_codec.cpp src/trt/bridge_img.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_actctx.cpp src/trt/shell_link.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp src/trt/gdi_raster.cpp src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_dbghelp.cpp src/trt/bridge_d3dx.cpp src/trt/input.cpp src/trt/trt_trace.cpp \
  src/trt/bridge_heap.cpp src/trt/bridge_kernel32.cpp src/trt/bridge_kernel32_gap.cpp src/trt/bridge_user32.cpp src/trt/bridge_user32_gap.cpp \
  src/trt/bridge_ntdll.cpp src/trt/bridge_opengl32.cpp src/trt/glsl_hlsl.cpp \
  src/trt/bridge_vulkan.cpp src/trt/spirv_hlsl.cpp src/trt/bridge_fmod_studio.cpp src/trt/bridge_system_native.cpp src/trt/fna3d_host.cpp src/trt/fna_platform.cpp src/trt/mono_host.cpp src/trt/pc2xb_translator.cpp src/trt/spirv_cache.cpp src/trt/universal_guest.cpp src/trt/x86_bridge.cpp src/trt/x86_exec.cpp src/trt/x86_heap.cpp src/trt/x86_jit.cpp src/trt/bridge_hid.cpp src/trt/bridge_xinput.cpp \
  src/trt/bridge_wasapi.cpp src/trt/bridge_winrt.cpp src/trt/bridge_ws2.cpp src/trt/bridge_msvcrt.cpp src/trt/bridge_version.cpp src/trt/bridge_prng.cpp src/trt/bridge_console_pack.cpp \
  src/trt/bridge_sys_extras.cpp src/trt/bridge_mf.cpp src/trt/bridge_gameinput.cpp \
  src/trt/bridge_gdiplus.cpp src/trt/bridge_nvapi.cpp src/trt/bridge_res.cpp src/trt/bridge_fmod.cpp src/trt/bridge_wldap32.cpp src/trt/bridge_ktmw32.cpp src/trt/steam_auth.cpp src/trt/steam_library.cpp src/trt/x86_32.cpp src/trt/x86_image.cpp \
  src/trt/bridge_dstorage.cpp src/trt/bridge_bink.cpp src/trt/bridge_oodle.cpp src/trt/bridge_wwise.cpp src/trt/bridge_criware.cpp src/trt/bridge_miles.cpp \
  "$cobj/miniz_tinfl.o" "$cobj/mojo_hlsl.o" "$cobj/vendor_stb_vorbis.o" "$cobj/vendor_dr_mp3.o" \
  -o "$out/test_corpus" || exit 1

# --- collect every PE under corpus-local/ (whole tree) ---
# Unity titles keep their native plugins in <Title>_Data/Plugins/x86_64/, which
# sits at depth 4. A depth-2 walk silently skipped 7 real PEs (Wwise,
# sqlite3, TapTap, Burst, steam_api64), so the scan is unbounded.
CORPUS_DIR="corpus-local"
mapfile -t PES < <(find "$CORPUS_DIR" -type f 2>/dev/null | \
  grep -iE '\.(exe|dll)$' | sort)

if [[ ${#PES[@]} -eq 0 ]]; then
  echo "corpus-smoke: no PE files found under $CORPUS_DIR"
  exit 0
fi

printf 'corpus-smoke: %d PE file(s): %s\n\n' "${#PES[@]}" "${PES[*]}"

# --- run; tee to tests/out/corpus-report.md; exit nonzero on LOAD FAILED ---
rc=0
"$out/test_corpus" "${PES[@]}" 2>&1 | tee "$out/corpus-report.md"
# PIPESTATUS: the tee pipeline must not swallow the test's exit code
[[ ${PIPESTATUS[0]} -eq 0 ]] || rc=1

exit $rc
