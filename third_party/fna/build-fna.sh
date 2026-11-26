#!/usr/bin/env bash
# third_party/fna/build-fna.sh — build FNA as Microsoft.Xna.Framework 4.0.0.0
# plus the .Graphics / .Game type-forwarder assemblies (Path B, Celeste).
#
# Recipe (proven by the Path B spike, 2026-11-20, on mineplex):
#   * FNA master 852e4af, classic mcs / .NET 4.x build (upstream `make
#     release`, ~342 files). The netstandard2.0 build does NOT run under
#     mineplex Mono 6.8.0.105 (no netstandard facade installed).
#   * The core is built as Microsoft.Xna.Framework v4.0.0.0: mcs names the
#     assembly after the -out file, and FNA's own AssemblyInfo (26.11.0.0)
#     is swapped for AssemblyInfo.core.cs (4.0.0.0). Mono binds the unsigned
#     substitutes to Celeste.exe's strong-named references with no complaint
#     (verified by the spike and by the probe at the end of this script).
#   * .Graphics (95 types) and .Game (16 types, minus GamerServicesComponent
#     — FNA has no GamerServices namespace and Celeste has 0 references to
#     it) are type-forwarder assemblies: [assembly: TypeForwardedTo] for
#     every top-level public type of the real XNA assemblies.
#   * Tiramisu backend (Path B step 2): FNAPlatform-tiramisu.patch adds a
#     third dispatch branch (TIRAMISU_FNA_PLATFORM=1 or
#     FNA_PLATFORM_BACKEND=TIRAMISU) to a COPY of FNAPlatform.cs in gen/,
#     and Tiramisu_FNAPlatform.cs (host CoreWindow + host input, no SDL)
#     is compiled into the core. SDL2/SDL3 backends are untouched.
#
# Outputs (kept in-tree): out/Microsoft.Xna.Framework{,.Graphics,.Game}.dll
#   + out/Microsoft.Xna.Framework.dll.config (FNA's dllmap for FNA3D/FAudio/
#   SDL2/SDL3, copied from upstream app.config).
# Idempotent: rebuilds only when an input is newer than the outputs, or with
#   --force. The FNA source clone (third_party/fna-src) is never modified.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
SRC="$root/third_party/fna-src"
OUT="$here/out"
GEN="$here/gen"
mkdir -p "$OUT" "$GEN"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

CORE="$OUT/Microsoft.Xna.Framework.dll"
GFX="$OUT/Microsoft.Xna.Framework.Graphics.dll"
GAME="$OUT/Microsoft.Xna.Framework.Game.dll"

needs_build=0
if [[ "$FORCE" == 1 ]]; then needs_build=1; fi
for o in "$CORE" "$GFX" "$GAME"; do [[ -f "$o" ]] || needs_build=1; done
if [[ "$needs_build" == 0 ]]; then
  # Freshness reference: the newest output (the Game forwarder is built
  # last). Generated files under gen/ and out/ are NOT inputs — comparing
  # against them would rebuild on every run.
  newest_out="$GAME"
  for o in "$CORE" "$GFX"; do
    [[ "$o" -nt "$newest_out" ]] && newest_out="$o"
  done
  newer=$(find "$SRC/src" "$SRC/Makefile" "$here" \
    -not -path "$GEN/*" -not -path "$OUT/*" \
    \( -name '*.cs' -o -name 'Makefile' -o -name '*.fxb' -o -name '*.txt' \
       -o -name '*.patch' \) \
    -newer "$newest_out" -print -quit 2>/dev/null || true)
  [[ -n "$newer" ]] && needs_build=1
fi
if [[ "$needs_build" == 0 ]]; then
  echo "fna: outputs up to date, skipping (use --force to rebuild)"
  ls -l "$CORE" "$GFX" "$GAME"
  exit 0
fi

command -v mcs >/dev/null || { echo "ERROR: mcs not found (mono-mcs package)" >&2; exit 2; }
command -v mono >/dev/null || { echo "ERROR: mono not found" >&2; exit 2; }
command -v monodis >/dev/null || { echo "ERROR: monodis not found" >&2; exit 2; }
[[ -d "$SRC/src" ]] || { echo "ERROR: FNA source clone missing at $SRC" >&2; exit 2; }

# --- 1. core: FNA as Microsoft.Xna.Framework 4.0.0.0 ------------------------
cd "$SRC"
# Dry-run the upstream Makefile to get its exact mcs command line (source
# list + -resource flags), then retarget the -out name and swap the
# AssemblyInfo. The clone itself is never modified.
mcs_cmd=$(make --no-print-directory -n release 2>/dev/null | grep '^mcs ' | head -1)
[[ -n "$mcs_cmd" ]] || { echo "ERROR: could not extract mcs command from FNA Makefile" >&2; exit 2; }
src_count=$(printf '%s' "$mcs_cmd" | tr ' ' '\n' | grep -c '\.cs$' || true)
echo "fna: upstream mcs source files: $src_count"
[[ "$src_count" -ge 330 ]] || { echo "ERROR: unexpected source count $src_count (expected ~342)" >&2; exit 2; }
mcs_cmd=$(printf '%s' "$mcs_cmd" | sed "s|-out:bin/Release/FNA.dll|-out:$CORE|")
mcs_cmd=$(printf '%s' "$mcs_cmd" | sed 's|src/Properties/AssemblyInfo.cs||')
mcs_cmd="$mcs_cmd $here/AssemblyInfo.core.cs"
# Tiramisu backend (Path B step 2): upstream FNAPlatform.cs only
# dispatches SDL2/SDL3. Patch a COPY in gen/ (the clone is never
# modified), swap it into the compile, and append the backend itself.
# The dispatch selects Tiramisu when TIRAMISU_FNA_PLATFORM=1 or
# FNA_PLATFORM_BACKEND=TIRAMISU; SDL2/SDL3 are untouched otherwise.
command -v patch >/dev/null || { echo "ERROR: patch not found" >&2; exit 2; }
PATCHED_PLATFORM="$GEN/FNAPlatform.cs"
cp "$SRC/src/FNAPlatform/FNAPlatform.cs" "$PATCHED_PLATFORM"
patch "$PATCHED_PLATFORM" < "$here/FNAPlatform-tiramisu.patch" \
  || { echo "ERROR: FNAPlatform dispatch patch failed to apply" >&2; exit 2; }
grep -q 'Tiramisu_FNAPlatform' "$PATCHED_PLATFORM" \
  || { echo "ERROR: patched FNAPlatform.cs lacks the Tiramisu branch" >&2; exit 2; }
mcs_cmd=$(printf '%s' "$mcs_cmd" | sed "s|src/FNAPlatform/FNAPlatform.cs|$PATCHED_PLATFORM|")
mcs_cmd="$mcs_cmd $here/Tiramisu_FNAPlatform.cs"
echo "fna: building Microsoft.Xna.Framework.dll (+ Tiramisu backend) ..."
# shellcheck disable=SC2086
eval "$mcs_cmd"
ls -l "$CORE"

# --- 2. type forwarders ------------------------------------------------------
gen_forwarders() {
  local list="$1" genfile="$2" title="$3"
  {
    echo "// GENERATED by third_party/fna/build-fna.sh from $(basename "$list")."
    echo "// Every top-level public type of the real $title 4.0.0.0 assembly;"
    echo "// the types live in the FNA core (built as Microsoft.Xna.Framework)."
    echo "using System.Runtime.CompilerServices;"
    while IFS= read -r t; do
      [[ -z "$t" || "$t" == \#* ]] && continue
      echo "[assembly: TypeForwardedTo(typeof($t))]"
    done < "$list"
  } > "$genfile"
  echo "fna: $(grep -c 'TypeForwardedTo' "$genfile") forwarders -> $(basename "$genfile")"
}
gen_forwarders "$here/graphics-types.txt" "$GEN/Forwarders.Graphics.cs" \
  "Microsoft.Xna.Framework.Graphics"
gen_forwarders "$here/game-types.txt" "$GEN/Forwarders.Game.cs" \
  "Microsoft.Xna.Framework.Game"

[[ "$(grep -c 'TypeForwardedTo' "$GEN/Forwarders.Graphics.cs")" == 95 ]] \
  || { echo "ERROR: Graphics forwarder count != 95" >&2; exit 2; }
[[ "$(grep -c 'TypeForwardedTo' "$GEN/Forwarders.Game.cs")" == 16 ]] \
  || { echo "ERROR: Game forwarder count != 16" >&2; exit 2; }

echo "fna: building Microsoft.Xna.Framework.Graphics.dll ..."
mcs -target:library -optimize -out:"$GFX" -r:"$CORE" \
  "$GEN/Forwarders.Graphics.cs" "$here/AssemblyInfo.graphics.cs"
echo "fna: building Microsoft.Xna.Framework.Game.dll ..."
mcs -target:library -optimize -out:"$GAME" -r:"$CORE" \
  "$GEN/Forwarders.Game.cs" "$here/AssemblyInfo.game.cs"

# --- 3. dllmap config + license ----------------------------------------------
cp "$SRC/app.config" "$OUT/Microsoft.Xna.Framework.dll.config"
cp "$SRC/licenses/LICENSE" "$here/LICENSE-MS-PL"
grep -q "Microsoft Public License" "$here/LICENSE-MS-PL" \
  || { echo "ERROR: FNA LICENSE is not MS-PL as expected" >&2; exit 2; }

# --- 4. verify ----------------------------------------------------------------
echo "--- assembly identities ---"
for a in "$CORE" "$GFX" "$GAME"; do
  monodis --assembly "$a" | grep -E "Name:|Version:" | tr '\n' ' '
  echo " <= $(basename "$a")"
done
echo "--- core assembly refs (must be only mscorlib + System 4.0.0.0) ---"
monodis --assemblyref "$CORE"
refs=$(monodis --assemblyref "$CORE" | grep -c "Version=4.0.0.0")
[[ "$refs" == 2 ]] || { echo "ERROR: core has unexpected assembly refs" >&2; exit 2; }

# Binding probe: load the three assemblies EXACTLY as Celeste.exe references
# them (name + version 4.0.0.0 + MS public key token) and resolve one type
# through each forwarder. This is the spike's binding proof, scripted.
cat > "$GEN/probe.cs" <<'EOF'
using System;
using System.Reflection;
class Probe {
  static int Main() {
    string[] names = {
      "Microsoft.Xna.Framework, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
      "Microsoft.Xna.Framework.Graphics, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
      "Microsoft.Xna.Framework.Game, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
    };
    foreach (string n in names) {
      Assembly a = Assembly.Load(n);
      Console.WriteLine("LOAD-OK " + a.FullName);
    }
    Type gd = Type.GetType("Microsoft.Xna.Framework.Graphics.GraphicsDevice, Microsoft.Xna.Framework.Graphics, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553");
    if (gd == null) { Console.WriteLine("FORWARD-FAIL GraphicsDevice"); return 1; }
    Console.WriteLine("FORWARD-OK GraphicsDevice -> " + gd.Assembly.GetName().Name);
    Type game = Type.GetType("Microsoft.Xna.Framework.Game, Microsoft.Xna.Framework.Game, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553");
    if (game == null) { Console.WriteLine("FORWARD-FAIL Game"); return 1; }
    Console.WriteLine("FORWARD-OK Game -> " + game.Assembly.GetName().Name);
    return 0;
  }
}
EOF
mcs -out:"$GEN/probe.exe" "$GEN/probe.cs"
echo "--- binding probe (MONO_PATH=$OUT) ---"
MONO_PATH="$OUT" mono "$GEN/probe.exe"

echo "fna: BUILD OK"
ls -l "$CORE" "$GFX" "$GAME"
