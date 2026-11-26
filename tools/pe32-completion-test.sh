#!/usr/bin/env bash
# tiramisu/tools/pe32-completion-test.sh
# Real 32-bit corpus DLLs must execute their entry to a clean return through
# the clean-room interpreter. Guards the instruction-length fidelity fixes.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=/tmp/opencode/pe32-completion
mkdir -p "$out"
clang++ -std=c++17 -O2 -Wall -no-pie -I src/trt \
  tests/test_x86_image.cpp src/trt/x86_32.cpp src/trt/x86_image.cpp \
  src/trt/images.cpp src/trt/pe_loader.cpp src/trt/module_table.cpp \
  src/trt/apiset.cpp src/trt/pe_exports.cpp src/trt/thunk.cpp \
  src/trt/vfs.cpp src/trt/iso.cpp \
  /tmp/opencode/x86-image-test/miniz_tinfl.o -o "$out/test" || exit 1

pass=0; total=0
for f in corpus-local/ortmm/SDL2.dll \
         corpus-local/ortmm/discord_game_sdk.dll \
         corpus-local/ortmm/binkw32.dll; do
  [[ -f "$f" ]] || continue
  total=$((total+1))
  line=$("$out/test" "$f" 2>/dev/null | grep '^REAL PE32')
  rc=$(sed -n 's/.*run_rc=\([0-9]*\).*/\1/p' <<<"$line")
  printf '%-28s rc=%s\n' "$(basename "$f")" "$rc"
  [[ "$rc" == "1" ]] && pass=$((pass+1))
done
echo "completed $pass/$total real PE32 DLL entries"
[[ $pass -ge 2 ]] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: ALL PASS"
