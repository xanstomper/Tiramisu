#!/usr/bin/env bash
# third_party/fna3d/build-fna3d.sh — build FNA3D (FNA's 3D renderer) from
# source as libFNA3D.so.0, SDL2 backend + bundled MojoShader (the graphics
# backend FNA uses at runtime; the Path B spike ran it under OpenGL/llvmpipe).
# Source clone: ../fna3d-src/ (FNA3D master d01538e, MojoShader submodule
# included). Idempotent; --force rebuilds.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
SRC="$root/third_party/fna3d-src"
OUT="$here/out"
BUILD="$here/build"
mkdir -p "$OUT"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

LIB="$OUT/libFNA3D.so.0"
needs_build=0
[[ "$FORCE" == 1 ]] && needs_build=1
[[ -f "$LIB" ]] || needs_build=1
if [[ "$needs_build" == 0 ]]; then
  newer=$(find "$SRC/src" "$SRC/MojoShader" "$SRC/CMakeLists.txt" \
    -newer "$LIB" -print -quit 2>/dev/null || true)
  [[ -n "$newer" ]] && needs_build=1
fi
if [[ "$needs_build" == 0 ]]; then
  echo "fna3d: $LIB up to date, skipping (use --force to rebuild)"
  exit 0
fi

command -v cmake >/dev/null || { echo "ERROR: cmake not found" >&2; exit 2; }
[[ -d "$SRC/src" ]] || { echo "ERROR: FNA3D source clone missing at $SRC" >&2; exit 2; }
[[ -d "$SRC/MojoShader" ]] || { echo "ERROR: MojoShader submodule missing in $SRC" >&2; exit 2; }

# SDL2, not SDL3: mineplex has SDL2 2.30.0 dev + runtime, no SDL3.
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SDL3=OFF -DBUILD_SHARED_LIBS=ON
cmake --build "$BUILD" -j"$(nproc)"

cp -f "$BUILD/libFNA3D.so.0" "$BUILD/libFNA3D.so" "$OUT/" 2>/dev/null \
  || cp -f "$BUILD"/libFNA3D.so* "$OUT/"
cp "$SRC/LICENSE" "$here/LICENSE-zlib"
grep -qi "zlib" "$here/LICENSE-zlib" || echo "NOTE: FNA3D LICENSE text differs from zlib template; kept as-is"

echo "--- libFNA3D.so.0 linkage ---"
ldd "$LIB" | grep -E "SDL|not found" || true
if ldd "$LIB" | grep -q "not found"; then
  echo "ERROR: libFNA3D.so.0 has unresolved dependencies" >&2; exit 2
fi
ls -l "$OUT/"
echo "fna3d: BUILD OK"
