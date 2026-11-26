#!/usr/bin/env bash
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

# Use the same DLL names but remove the symbol that the back edge imports.
mkdir "$tmpdir/missing"
sed 's/int cycle_a(void)/int another_export(void)/' "$tmpdir/a.c" > "$tmpdir/missing/a.c"
"$clang_bin" --target=x86_64-pc-windows-msvc -c "$tmpdir/missing/a.c" -o "$tmpdir/missing/a.obj"
"$lld_bin" /dll /entry:DllMain /nodefaultlib /dynamicbase \
  /out:"$tmpdir/missing/cycle_a.dll" "$tmpdir/missing/a.obj" "$tmpdir/b.lib"
cp "$tmpdir/cycle_b.dll" "$tmpdir/missing/cycle_b.dll"

clang++ -std=c++20 -O2 -Wall -no-pie tests/test_cycle_loader.cpp tests/out/libtrt.a -o "$tmpdir/check"
timeout 10 "$tmpdir/check" "$tmpdir"
