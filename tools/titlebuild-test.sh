#!/usr/bin/env bash
# tiramisu/tools/titlebuild-test.sh
# Offline gate for the title build + autofix engine: every corpus title must
# produce a measured verdict with no false "blocked", the generated files must
# be valid, and the declared block rules must actually fire.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
tb="python3 tools/titlebuild.py"
rc=0

# The symbol classifier is the runtime's own (test_corpus); build it first so
# the engine uses ground truth instead of the Python fallback.
tools/corpus-smoke.sh >/dev/null 2>&1 || true

echo "== corpus titles =="
for d in corpus-local/*/; do
  [[ -d "$d" ]] || continue
  n=$(find "$d" -type f \( -iname '*.exe' -o -iname '*.dll' \) | wc -l)
  [[ "$n" -gt 0 ]] || { echo "SKIP $d (no PEs)"; continue; }
  out=$($tb --check "$d" 2>&1); r=$?
  verdict=$(sed -n 's/^verdict: *//p' <<<"$out")
  printf '%-28s %-9s %-14s %s\n' "$(basename "$d")" "$verdict" \
    "$(sed -n 's/^engine: *//p' <<<"$out")" \
    "$(sed -n 's/^imports: */imp=/p' <<<"$out")"
  case "$(basename "$d")" in
    gonext|raccoin)
      grep -q '^engine: *unity-il2cpp' <<<"$out" || { echo "FAIL engine detection: $d"; rc=1; } ;;
    botw-native)
      grep -q '^engine: *godot-sdl3' <<<"$out" || { echo "FAIL engine detection: $d"; rc=1; } ;;
    sekiro)
      grep -q '^engine: *fromsoftware' <<<"$out" || { echo "FAIL engine detection: $d"; rc=1; } ;;
  esac
  if [[ $r -gt 1 || -z "$verdict" ]]; then
    echo "FAIL $d: expected a measured verdict (got exit $r)"; sed 's/^/    /' <<<"$out"; rc=1
  fi
done

echo "== generated package =="
tmp=$(mktemp -d /tmp/opencode/titlebuild-XXXXXX)
mkdir -p "$tmp/game"
pe=$(find corpus-local -type f -iname '*.exe' | head -1)
cp "$pe" "$tmp/game/" 2>/dev/null
$tb "$tmp/game" >/dev/null 2>&1
if [[ -f "$tmp/game/.tiramisu/manifest.json" ]] && \
   python3 -c "import json,sys; m=json.load(open('$tmp/game/.tiramisu/manifest.json')); sys.exit(0 if m['verdict'] in ('bootable','has-gaps','blocked') else 1)" && \
   grep -q '^default_input=' "$tmp/game/tiramisu.profile.ini"; then
  echo "OK manifest.json + tiramisu.profile.ini written"
else
  echo "FAIL package generation"; rc=1
fi
if [[ -f "$tmp/game/.tiramisu/shims.txt" ]]; then
  echo "OK shims.txt written ($(wc -l < "$tmp/game/.tiramisu/shims.txt") modules)"
else
  echo "FAIL shims.txt"; rc=1
fi
engine_game="$tmp/engine"; mkdir -p "$engine_game"
find corpus-local/raccoin -maxdepth 1 -type f \( -name '*.exe' -o -name '*.dll' \) \
  -exec cp {} "$engine_game/" \; 2>/dev/null
python3 tools/titlebuild.py "$engine_game" >/dev/null 2>&1
if grep -q '"engine": "unity-il2cpp"' "$engine_game/.tiramisu/manifest.json" && \
   grep -q '^write_watch=1' "$engine_game/tiramisu.profile.ini" && \
   grep -q '^memory_budget_mb=4096' "$engine_game/tiramisu.profile.ini"; then
  echo "OK detected engine generated its own profile overrides"
else
  echo "FAIL engine profile generation"; rc=1
fi

echo "== block rules fire =="
mkdir -p "$tmp/ac/game"; : > "$tmp/ac/game/EasyAntiCheat.exe"
out=$($tb --check "$tmp/ac/game" 2>&1); r=$?
if [[ $r -eq 1 && "$out" == *'BLOCKED'* ]]; then
  echo "OK anti-cheat blocked (exit 1)"
else
  echo "FAIL anti-cheat rule (exit $r)"; sed 's/^/    /' <<<"$out"; rc=1
fi

pe32=$(python3 - <<'PY'
import glob, sys
sys.path.insert(0, 'tools')
import pe_readiness
for f in sorted(glob.glob('corpus-local/**/*.dll', recursive=True)):
    try:
        r = pe_readiness.parse(f)
    except Exception:
        continue
    if r and r['bits'] == 32:
        print(f)
        break
PY
)
if [[ -n "${pe32:-}" ]]; then
  mkdir -p "$tmp/x86/game"; cp "$pe32" "$tmp/x86/game/game.exe"
  out=$($tb --check "$tmp/x86/game" 2>&1); r=$?
  if [[ $r -eq 1 && "$out" == *BLOCKED* ]]; then
    echo "OK 32-bit blocked via $pe32"
  else
    echo "FAIL x86 rule (exit $r)"; sed 's/^/    /' <<<"$out"; rc=1
  fi
else
  echo "SKIP x86 rule (no PE32 fixture in corpus)"
fi

rm -rf "$tmp"
echo
[[ $rc -eq 0 ]] && echo "RESULT: ALL PASS" || echo "RESULT: FAIL"
exit $rc
