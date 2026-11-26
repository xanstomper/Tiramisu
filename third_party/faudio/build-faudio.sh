#!/usr/bin/env bash
# third_party/faudio/build-faudio.sh — build FAudio (FNA's XAudio2
# reimplementation) from source as libFAudio.so.0, SDL2 backend.
# Celeste uses FMOD for all its audio, so FAudio is only FNA's audio-device
# dependency; it is built so the FNA stack is complete and self-contained.
# Source clone: ../faudio-src/ (FAudio master 0c42375). Idempotent;
# --force rebuilds.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
SRC="$root/third_party/faudio-src"
OUT="$here/out"
BUILD="$here/build"
mkdir -p "$OUT"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

LIB="$OUT/libFAudio.so.0"
needs_build=0
[[ "$FORCE" == 1 ]] && needs_build=1
[[ -f "$LIB" ]] || needs_build=1
if [[ "$needs_build" == 0 ]]; then
  newer=$(find "$SRC/src" "$SRC/CMakeLists.txt" \
    -newer "$LIB" -print -quit 2>/dev/null || true)
  [[ -n "$newer" ]] && needs_build=1
fi
if [[ "$needs_build" == 0 ]]; then
  echo "faudio: $LIB up to date, skipping (use --force to rebuild)"
  exit 0
fi

command -v cmake >/dev/null || { echo "ERROR: cmake not found" >&2; exit 2; }
[[ -d "$SRC/src" ]] || { echo "ERROR: FAudio source clone missing at $SRC" >&2; exit 2; }

# SDL2, not SDL3: mineplex has SDL2 2.30.0 dev + runtime, no SDL3.
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SDL3=OFF -DBUILD_SHARED_LIBS=ON
cmake --build "$BUILD" -j"$(nproc)"

cp -f "$BUILD"/libFAudio.so* "$OUT/"
cp "$SRC/LICENSE" "$here/LICENSE-zlib" 2>/dev/null || cp "$SRC/LICENSE.txt" "$here/LICENSE-zlib"
grep -qi "zlib" "$here/LICENSE-zlib" || echo "NOTE: FAudio LICENSE text differs from zlib template; kept as-is"

echo "--- libFAudio.so.0 linkage ---"
ldd "$LIB" | grep -E "SDL|not found" || true
if ldd "$LIB" | grep -q "not found"; then
  echo "ERROR: libFAudio.so.0 has unresolved dependencies" >&2; exit 2
fi
ls -l "$OUT/"
echo "faudio: BUILD OK"
