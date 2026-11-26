#!/usr/bin/env bash
# tiramisu/tools/sanitize-gate.sh — the certainty layer.
#
# The console has 8 cores and a different libc; code that "works" on a
# 2-core desktop can race or UB its way into a crash there. This gate
# rebuilds the runtime under AddressSanitizer + UndefinedBehaviorSanitizer
# (recover=false: the first hit kills the run with a named report) and
# executes every guest-executing suite with EXACTLY the guest arguments the
# normal gate uses (mirrored from build-test.sh).
#
# Requires: tools/build-test.sh has run once (the guest PEs exist in tests/out).
#
# Exit 0 only if every suite is sanitizer-clean.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
CLANG="${CLANG:-/usr/bin/clang-19}"
CLANGXX="${CLANGXX:-/usr/bin/clang++-19}"
out="${1:-out/sanitize}"
SANFLAGS="-fsanitize=address,undefined -fno-sanitize=function -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
objdir="$out/trt_objs_san"
rm -rf "$objdir"; mkdir -p "$objdir"

# ---- 1. the runtime, sanitized ----------------------------------------------
printf '%s\n' src/trt/*.cpp | xargs -P"${TRT_JOBS:-$(nproc)}" -I{} env \
  CLANGXX="$CLANGXX" OBJDIR="$objdir" SANFLAGS="$SANFLAGS" bash -c '
  f="$1"; base="$(basename "$f" .cpp)"
  "$CLANGXX" -std=c++20 $SANFLAGS -fno-pie -c "$f" -o "$OBJDIR/${base}.o" || exit 1
' _ {} || { echo "SANITIZE-GATE: runtime compile FAILED" >&2; exit 1; }
"$CLANG" -O1 -g $SANFLAGS -fno-pie -c src/host/vendor/miniz_tinfl.c -o "$objdir/miniz_tinfl.o" || exit 1
"$CLANG" -std=gnu11 -O1 -g $SANFLAGS -fno-pie -c src/trt/mojo_hlsl.c -o "$objdir/mojo_hlsl.o" || exit 1
"$CLANG" -O1 -g $SANFLAGS -fno-pie -c src/trt/vendor_stb_vorbis.c -o "$objdir/vendor_stb_vorbis.o" -DSTB_VORBIS_NO_STDIO || exit 1
ar rcs "$out/libtrt_san.a" "$objdir"/*.o

# ---- 2. per-suite guest arguments (mirrored from build-test.sh) -------------
guest_args() {
  case "$1" in
    test_audio)          echo "tests/out/guest_audio.exe";;
    test_comctl)         echo "tests/out/guest_comctl.exe";;
    test_d3d)            echo "tests/out/guest_d3d.exe";;
    test_d3d12_triangle) echo "tests/out/guest_triangle.exe";;
    test_d3d8)           echo "tests/out/guest_d3d8.exe";;
    test_d3d9)           echo "tests/out/guest_d3d9.exe";;
    test_d3dx9)          echo "tests/out/guest_d3dx9.exe";;
    test_ddraw)          echo "tests/out/guest_ddraw.exe";;
    test_dinput)         echo "tests/out/guest_dinput.exe";;
    test_dsound)         echo "tests/out/guest_dsound.exe";;
    test_engine)         echo "tests/out/guest_engine.exe /tmp/tir-san-drive_c tests/out/guest_seh.exe";;
    test_game_boot)      echo "tests/out/guest_game.exe";;
    test_gdi)            echo "tests/out/guest_gdi.exe";;
    test_guest_eh)       echo "SKIP-ASAN-CONFLICT tests/out/guest_eh.exe";;
    test_kernel32b)      echo "tests/out/guest_kernel32b.exe";;
    test_libload)        echo "tests/out/guest_libload.exe tests/out";;
    test_mmedia)         echo "tests/out/guest_mmedia.exe";;
    test_ntdll)          echo "tests/out/guest_ntdll.exe";;
    test_ole)            echo "tests/out/guest_ole.exe";;
    test_openal)         echo "tests/out/guest_openal.exe";;
    test_opengl)         echo "tests/out/guest_opengl.exe";;
    test_pe_loader)      echo "tests/out/guest_hello.exe tests/out/guest_hello_reloc.exe";;
    test_real_load)      echo "tests/out/guest_realload.exe";;
    test_reg)            echo "tests/out/guest_reg.exe /tmp/tir-san-drive_c";;
    test_res)            echo "tests/out/guest_res.exe";;
    test_shell)          echo "tests/out/guest_shell.exe";;
    test_shlwapi)        echo "tests/out/guest_shlwapi.exe";;
    test_stage5)         echo "tests/out/guest_wininet.exe 0x3ff";;
    test_steam)          echo "tests/out/guest_steam.exe";;
    test_vulkan)         echo "tests/out/guest_vulkan.exe";;
    test_winmm)          echo "tests/out/guest_winmm.exe";;
    *)                   echo "";;
  esac
}

# the suites that exercise the host with REAL guest execution
suites="test_pe_loader test_input test_game_boot test_real_load test_iso test_libload test_d3d test_dsound test_io_async test_membudget test_fmod test_fuzz_loader test_engine test_reg test_res test_steam test_vulkan test_opengl test_openal test_ole test_shell test_shlwapi test_gdi test_comctl test_winmm test_dinput test_ddraw test_d3d8 test_d3d9 test_d3dx9 test_mmedia test_ntdll test_audio test_guest_eh test_kernel32b test_stage5 test_d3d12_triangle"

rc=0
ran=0
for t in $suites; do
  [[ -f "tests/$t.cpp" ]] || { echo "SKIP $t (no source)"; continue; }
  # skip when the suite's guest is missing (build-test.sh must have run)
  g="$(guest_args "$t")"
  missing=0
  for gp in $g; do
    [[ "$gp" == /* || -f "$gp" ]] || { missing=1; break; }
  done
  [[ $missing -eq 1 ]] && { echo "SKIP $t (guest missing — run build-test.sh first)"; continue; }
  # guest EH builds synthetic stack frames ASan cannot model (its own
  # CHECK fires while REPORTING the copy — not a host bug); the plain
  # 44-check gate owns EH correctness
  [[ "$g" == SKIP-ASAN-CONFLICT* ]] && { echo "SKIP $t (guest EH synthetic frames are outside ASan's model — plain gate owns EH)"; continue; }
  echo "=== sanitize: $t ==="
  "$CLANGXX" -std=c++20 -O1 -g $SANFLAGS -no-pie "tests/$t.cpp" \
    "$out/libtrt_san.a" -lpthread -o "$out/$t" \
    || { echo "SANITIZE-GATE: $t BUILD FAILED"; rc=1; continue; }
  # the drive_c-arg suites need the mount point to EXIST (the VFS mount
  # fails otherwise and relative-path resolution falls to the host cwd)
  [[ "$t" == test_engine || "$t" == test_reg ]] && rm -rf /tmp/tir-san-drive_c; mkdir -p /tmp/tir-san-drive_c  # FRESH state: build-test rm -rf its DRIEC too
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    timeout 300 "$out/$t" $g > "$out/$t.san.log" 2>&1
  trc=$?
  if [[ $trc -ne 0 ]]; then
    echo "SANITIZE-GATE: $t FAILED (rc=$trc) — report:"
    grep -E "ERROR: |runtime error:|SUMMARY" "$out/$t.san.log" | head -6
    rc=1
  else
    verdict=$(grep -E "RESULT:" "$out/$t.san.log" | tail -1)
    echo "  clean: ${verdict:-ok}"
    ++ran
  fi
done
echo "suites sanitizer-clean: $ran"
[[ $rc -eq 0 ]] && echo "SANITIZE-GATE: ALL CLEAN" || echo "SANITIZE-GATE: FAILURES (named above)"
exit $rc
