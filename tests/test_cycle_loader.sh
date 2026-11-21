#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
clang_bin="/usr/lib/llvm-19/bin/clang"
lld_bin="$HOME/.local/bin/lld-link"
cat > "$tmpdir/a.c" <<'C'
__declspec(dllimport) int cycle_b(void);
__declspec(dllexport) int cycle_a(void) { return 7; }
__declspec(dllexport) int cycle_a_calls_b(void) { return cycle_b(); }
int DllMain(void* module, unsigned long reason, void* reserved) {
  (void)module; (void)reserved;
  return reason != 1 || cycle_b() == 5;
}
C
cat > "$tmpdir/b.c" <<'C'
__declspec(dllimport) int cycle_a(void);
__declspec(dllexport) int cycle_b(void) { return 5; }
int DllMain(void* module, unsigned long reason, void* reserved) {
  (void)module; (void)reserved;
  return reason != 1 || cycle_a() == 7;
}
C
printf 'LIBRARY cycle_a.dll\nEXPORTS\n cycle_a\n' > "$tmpdir/a.def"
printf 'LIBRARY cycle_b.dll\nEXPORTS\n cycle_b\n' > "$tmpdir/b.def"
"$lld_bin" /lib /def:"$tmpdir/a.def" /out:"$tmpdir/a.lib" /machine:x64
"$lld_bin" /lib /def:"$tmpdir/b.def" /out:"$tmpdir/b.lib" /machine:x64
"$clang_bin" --target=x86_64-pc-windows-msvc -c "$tmpdir/a.c" -o "$tmpdir/a.obj"
"$clang_bin" --target=x86_64-pc-windows-msvc -c "$tmpdir/b.c" -o "$tmpdir/b.obj"
"$lld_bin" /dll /entry:DllMain /nodefaultlib /dynamicbase /out:"$tmpdir/cycle_a.dll" "$tmpdir/a.obj" "$tmpdir/b.lib"
"$lld_bin" /dll /entry:DllMain /nodefaultlib /dynamicbase /out:"$tmpdir/cycle_b.dll" "$tmpdir/b.obj" "$tmpdir/a.lib"

cat > "$tmpdir/bad_a.c" <<'C'
__declspec(dllimport) int cycle_bad_b(void);
__declspec(dllimport) int absent_fn(void);
__declspec(dllexport) int cycle_bad_a(void) { return cycle_bad_b() + absent_fn(); }
C
cat > "$tmpdir/bad_b.c" <<'C'
__declspec(dllimport) int cycle_bad_a(void);
__declspec(dllexport) int cycle_bad_b(void) { return cycle_bad_a(); }
C
printf 'LIBRARY cycle_bad_a.dll\nEXPORTS\n cycle_bad_a\n' > "$tmpdir/bad_a.def"
printf 'LIBRARY cycle_bad_b.dll\nEXPORTS\n cycle_bad_b\n' > "$tmpdir/bad_b.def"
printf 'LIBRARY zmissing.dll\nEXPORTS\n absent_fn\n' > "$tmpdir/missing.def"
"$lld_bin" /lib /def:"$tmpdir/bad_a.def" /out:"$tmpdir/bad_a.lib" /machine:x64
"$lld_bin" /lib /def:"$tmpdir/bad_b.def" /out:"$tmpdir/bad_b.lib" /machine:x64
"$lld_bin" /lib /def:"$tmpdir/missing.def" /out:"$tmpdir/missing.lib" /machine:x64
"$clang_bin" --target=x86_64-pc-windows-msvc -c "$tmpdir/bad_a.c" -o "$tmpdir/bad_a.obj"
"$clang_bin" --target=x86_64-pc-windows-msvc -c "$tmpdir/bad_b.c" -o "$tmpdir/bad_b.obj"
"$lld_bin" /dll /noentry /nodefaultlib /dynamicbase /out:"$tmpdir/cycle_bad_a.dll" "$tmpdir/bad_a.obj" "$tmpdir/bad_b.lib" "$tmpdir/missing.lib"
"$lld_bin" /dll /noentry /nodefaultlib /dynamicbase /out:"$tmpdir/cycle_bad_b.dll" "$tmpdir/bad_b.obj" "$tmpdir/bad_a.lib"

clang++ -std=c++20 -O2 -Wall -no-pie tests/test_cycle_loader.cpp tests/out/libtrt.a -o "$tmpdir/check"
timeout 10 "$tmpdir/check" "$tmpdir/cycle_a.dll" "$tmpdir"
