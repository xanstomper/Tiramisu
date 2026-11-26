#!/usr/bin/env bash
# tiramisu/tools/repack-universal.sh — Phase 5: universal-guest round-trip.
#
# Translates a cleared PC game's imports (Phase 3, out/pc2xb) and writes the
# round-trip manifest <out_dir>/universal_game_manifest.json describing the
# universal guest the game is packed for.
#
#   ./tools/repack-universal.sh <game_dir> [out_dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game_dir="${1:?usage: repack-universal.sh <game_dir> [out_dir]}"
out_dir="${2:-}"

pc2xb="$root/tests/out/pc2xb"
[[ -x "$pc2xb" ]] || { echo "repack: run bash tools/build-test.sh first" >&2; exit 1; }
[[ -d "$game_dir" ]] || { echo "repack: missing game dir: $game_dir" >&2; exit 1; }

if [[ -z "$out_dir" ]]; then
  out_dir="$(dirname "$game_dir")/univ_$(basename "$game_dir")"
fi
mkdir -p "$out_dir"
# Copy the cleared game tree (its host-serviced imports travel with it).
cp -a "$game_dir"/. "$out_dir"/
# Find the game's main exe (first top-level *.exe) — crude, fine for repack.
main_exe="$(find "$out_dir" -maxdepth 1 -name '*.exe' | sort | head -1)"
[[ -n "$main_exe" ]] || { echo "repack: no *.exe found in $game_dir" >&2; exit 1; }
base="$(basename "$main_exe")"
# Translate the import surface (writes xb_imports.json / .csv + xb_profile.json).
"$pc2xb" "$main_exe" "$out_dir/xb"
# Round-trip manifest for the universal guest pack.
{
  echo "{"
  echo "  \"profile_flags\": \"TRT_UNIV_CLEAR_PC\","
  echo "  \"guest\": \"universal\","
  echo "  \"main_exe\": \"$base\","
  echo "  \"xb_profile\": \"$out_dir/xb_profile.json\","
  echo "  \"note\": \"Universal guest round-trip: PC imports translated to Xbox equivalents; host-serviced imports are serviced by the Tiramisu universal host at boot.\""
  echo "}"
} > "$out_dir/universal_game_manifest.json"
echo "repack: $game_dir -> $out_dir (universal guest)"
