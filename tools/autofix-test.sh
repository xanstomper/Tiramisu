#!/usr/bin/env bash
# tiramisu/tools/autofix-test.sh
# Offline gate for the fix DB, fix planner and retry loop. No console needed:
# real failure logs are replayed from tests/fixtures/logs/.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
py="python3"
rc=0

echo "== signature matching + applied repairs =="
tmp=$(mktemp -d /tmp/opencode/autofix-XXXXXX)
game="$tmp/game"; mkdir -p "$game"
# raccoin has real named gaps, so the retry loop has something to work on.
find corpus-local/raccoin -maxdepth 1 -type f \( -name '*.exe' -o -name '*.dll' \) \
  -exec cp {} "$game/" \; 2>/dev/null

out=$($py tools/autofix.py "$game" tests/fixtures/logs/log.1 --apply --attempt 1)
echo "$out" | grep -q 'redist-msvcp140' && echo "OK msvcp140 rule matched" || { echo "FAIL msvcp140 rule"; rc=1; }
echo "$out" | grep -q 'redist-vcruntime140' && echo "OK vcruntime140 rule matched" || { echo "FAIL vcruntime140 rule"; rc=1; }
echo "$out" | grep -q 'shader-compiler-forward' && echo "OK d3dcompiler rule matched" || { echo "FAIL d3dcompiler rule"; rc=1; }

if grep -q 'msvcp140_app.dll' "$game/.tiramisu/aliases.json" && \
   grep -q '^shader_cache=1' "$game/tiramisu.profile.ini" && \
   grep -q '"attempt": 1' "$game/.tiramisu/autofix.json"; then
  echo "OK aliases.json + profile.ini + autofix.json written"
else
  echo "FAIL repair artifacts"; rc=1
fi

echo "== repairs survive a rebuild =="
$py tools/titlebuild.py "$game" >/dev/null 2>&1
if grep -q 'msvcp140_app.dll' "$game/.tiramisu/manifest.json" && \
   grep -q '^shader_cache=1' "$game/tiramisu.profile.ini"; then
  echo "OK alias active in manifest; profile override preserved"
else
  echo "FAIL rebuild lost repairs"; rc=1
fi

echo "== real captured logs (not synthetic) =="
real_game="$tmp/real"; mkdir -p "$real_game"
out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-console-probe.log --apply)
echo "$out" | grep -q 'input-mouse-callback-failed' && \
  echo "OK real console probe -> GameInput repair matched" || { echo "FAIL real console rule"; rc=1; }
echo "$out" | grep -q 'dstorage-missing-named' && \
  echo "OK real DStorage gap recorded as named (not hidden)" || { echo "FAIL named DStorage"; rc=1; }
if grep -q '^default_input=controller' "$real_game/tiramisu.profile.ini"; then
  echo "OK real repair applied to profile"
else
  echo "FAIL real repair not applied"; rc=1
fi
out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-corpus-failure.log)
echo "$out" | grep -q 'guest-fault-named' && \
  echo "OK real SIGSEGV/RUN FAILED recorded as named work item" || { echo "FAIL real crash rule"; rc=1; }

# Real failed boot of a NON-booting TITLE (Go Next demo), captured from
# tools/corpus-run.sh on this tree — not a synthetic fixture.
out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-title-boot-failure.log)
echo "$out" | grep -q 'mscoree-stub-named' && \
  echo "OK real non-booting title (Go Next) failure matched + named" || { echo "FAIL real title rule"; rc=1; }
out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-title-boot-after-mscoree-fix.log)
echo "$out" | grep -q 'unitymain2-probe-named' && \
  echo "OK post-mscoree real failure (UnityMain2 probe) matched + named" || { echo "FAIL post-mscoree rule"; rc=1; }
out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-console-sekiro-failure.log)
echo "$out" | grep -q 'missing-game-data-named' && \
  echo "OK real console Sekiro failure (missing pack.dat) matched + named" || { echo "FAIL console Sekiro rule"; rc=1; }

out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-console-ultrapool-boot.log)
echo "$out" | grep -q 'vulkan-present-stall-named' && \
  echo "OK real console ultrapool boot + present stall matched + named" || { echo "FAIL console ultrapool rule"; rc=1; }

out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-console-seh-contained.log)
echo "$out" | grep -q 'sekiro-invalid-pointer-named' && \
  echo "OK real console SEH-contained fault matched + named" || { echo "FAIL SEH rule"; rc=1; }

out=$($py tools/autofix.py "$real_game" tests/fixtures/logs/real-console-spirv-failure.log)
echo "$out" | grep -q 'spirv-shader-parse-named' && \
  echo "OK real console SPIR-V shader failures matched + named" || { echo "FAIL SPIR-V rule"; rc=1; }

echo "== retry loop =="
$py tools/autopilot.py "$game" --dry-run tests/fixtures/logs --max-attempts 5 >"$tmp/loop.txt" 2>&1
cat "$tmp/loop.txt" | sed 's/^/    /'
if grep -q 'SUCCESS' "$tmp/loop.txt" || grep -q 'no declared repair' "$tmp/loop.txt"; then
  echo "OK loop stopped on a measured condition"
else
  echo "FAIL loop did not converge"; rc=1
fi
if $py -c "import json;h=json.load(open('$game/.tiramisu/autopilot.json'))['history'];assert any(x['result']=='fix' for x in h);assert any(x['result']=='no-fix' for x in h)" 2>/dev/null; then
  echo "OK attempt history records fixes and the named stopping point"
else
  echo "FAIL attempt history"; rc=1
fi

echo "== terminal block =="
anti="$tmp/anti"; mkdir -p "$anti"; : > "$anti/EasyAntiCheat.exe"
$py tools/autopilot.py "$anti" --dry-run tests/fixtures/logs --max-attempts 3 >"$tmp/anti.txt" 2>&1
if grep -q 'terminal exclusion' "$tmp/anti.txt"; then
  echo "OK anti-cheat is terminal (no fake repair)"
else
  echo "FAIL terminal block"; sed 's/^/    /' "$tmp/anti.txt"; rc=1
fi

rm -rf "$tmp"
echo
[[ $rc -eq 0 ]] && echo "RESULT: ALL PASS" || echo "RESULT: FAIL"
exit $rc
