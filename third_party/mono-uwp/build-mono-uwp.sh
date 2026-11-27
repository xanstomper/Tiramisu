#!/bin/bash
# tiramisu/third_party/mono-uwp/build-mono-uwp.sh — build stock Mono 6.8.0.105
# (SGen GC, interpreter-capable) as a static library for the UWP/WinRT target.
#
# Why stock Mono builds for UWP at all: Mono 6.8's Windows code is written
# against the WINAPI-family abstraction (msvc/msvc-win32-support.h):
# WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) selects the UWP code paths
# in 29 runtime files (LoadPackagedLibrary instead of LoadLibrary, BCrypt
# instead of CryptoAPI, no console/registry/toolhelp use). The build opts
# in with -DHAVE_EXTERN_DEFINED_WINAPI_SUPPORT — the header #errors without
# it under the APP partition. There is no separate "UWP Mono": this is the
# same 6.8.0.105 runtime the Linux harness embeds, compiled for WinRT.
#
# What is built: the SGen static libmono (eglib + libmini + libmonoruntime
# + libmonoutils + libgcmonosgen + zlib), 294 C sources — the same set the
# MSVC solution (msvc/mono.sln, Release|x64, SGen) builds, plus
# console-null.c (the UWP console driver, which the MSVC build omits) —
# plus asm/win64.S, the GNU as translation of Mono's mono/utils/win64.asm
# (MASM, git only — not in the release tarball), which defines
# mono_context_get_current and copy_stack_data_internal_win32_wrapper
# (the cooperative-GC-suspend register capture; cannot be written in C).
# See mono-sources.txt (derived by dev/parse_targets.py from the stock
# 6.8 .targets files at tag mono-6.8.0.105). The class libraries are NOT
# rebuilt: the 4.5 profile framework assemblies are platform-independent
# IL, and the UWP build reuses the same 6.8.0.105 assemblies the Linux path
# uses (packaged by tools/build-host.sh from /usr/lib/mono/4.5/).
#
# The interpreter is part of libmini (mono/mini/interp/); it is selected
# at runtime with --interpreter before mono_jit_init (see mono_host.cpp).
# No JIT code is executed on the console: the UWP branch forces
# interpreter mode.
#
# Layout: the tarball + extracted source + objects live in
# $MONO_UWP_CACHE (default ~/.cache/tiramisu/mono-uwp); the library lands
# in <out>/libmonosgen-uwp.lib (<out> defaults to $MONO_UWP_CACHE/out).
# The build is skipped when the outputs are up to date (stamp covers the
# script, the source list, the vendored msvc headers, and the patches);
# MONO_UWP_REBUILD=1 forces a rebuild. Progress goes to stderr; the only
# stdout line is the library path, so callers can capture it:
#   MONO_LIB="$(third_party/mono-uwp/build-mono-uwp.sh)"
#
# Console-verification-pending: this produces a genuinely buildable and
# linkable UWP Mono. Whether Celeste runs under it on the Series S can
# only be proven by a console test (no UWP runtime exists on Linux).
set -euo pipefail
# Build tools can write progress to stdout. Reserve stdout for the archive path.
exec 3>&1 >&2
export PATH="$HOME/.local/bin:$PATH"
here="$(cd "$(dirname "$0")/../.." && pwd)"
UWP_DIR="$here/third_party/mono-uwp"

MONO_VERSION="6.8.0.105"
TARBALL="mono-6.8.0.105.tar.xz"
TARBALL_URL="https://download.mono-project.com/sources/mono/$TARBALL"
# Pinned digest of the release tarball (downloaded 2026-11-20; size
# 243441900 bytes, matching the server's Content-Length).
TARBALL_SHA256="578799c44c3c86a9eb5daf6dec6c60a24341940fd376371956d4a46cf8612178"

CACHE="${MONO_UWP_CACHE:-$HOME/.cache/tiramisu/mono-uwp}"
SRC="$CACHE/src/mono-6.8.0.105"
OUT="${1:-$CACHE/out}"
LIB="$OUT/libmonosgen-uwp.lib"

log() { echo "mono-uwp: $*" >&2; }
die() { log "ERROR: $*"; exit 1; }

command -v curl >/dev/null || die "curl not found"
command -v python3 >/dev/null || die "python3 not found"
command -v sha256sum >/dev/null || die "sha256sum not found"
command -v llvm-nm >/dev/null || die "llvm-nm not found (ships with LLVM)"

CB="${UWP_CROSSBUILD:-$here/../gow-uwp/ref/uwp-crossbuild}"
[[ -x "$CB/scripts/build.sh" ]] || CB="$HOME/gow-uwp/ref/uwp-crossbuild"
[[ -x "$CB/scripts/build.sh" ]] || die "uwp-crossbuild wrapper not found (set UWP_CROSSBUILD)"
[[ -d "$HOME/.cache/uwp-crossbuild/xwin/sdk" ]] || die "no xwin SDK — run: xwin --accept-license --arch x86_64 splat --output ~/.cache/uwp-crossbuild/xwin"

# --- stamp: rebuild when any build input changes ---------------------------
stamp_inputs=("$UWP_DIR/build-mono-uwp.sh" "$UWP_DIR/mono-sources.txt"
              "$UWP_DIR/msvc/msvc-win32-support.h"
              "$UWP_DIR/msvc/msvc-disabled-warnings.h"
              "$UWP_DIR/asm/win64.S")
for p in "$UWP_DIR"/patches/*.patch "$UWP_DIR"/include/*.h; do
  [[ -f "$p" ]] && stamp_inputs+=("$p")
done
current_stamp="$(cat "${stamp_inputs[@]}" 2>/dev/null | sha256sum | cut -d' ' -f1)-$MONO_VERSION"

if [[ -z "${MONO_UWP_REBUILD:-}" && -f "$LIB" && -f "$OUT/.mono-uwp-stamp" && \
      "$(cat "$OUT/.mono-uwp-stamp")" == "$current_stamp" ]]; then
  log "up to date: $LIB"
  printf '%s\n' "$LIB" >&3
  exit 0
fi

mkdir -p "$CACHE/src" "$OUT" "$CACHE/obj"

# --- 1. fetch the release tarball (pinned URL + digest) ---------------------
if [[ ! -f "$CACHE/$TARBALL" ]]; then
  log "downloading $TARBALL_URL"
  curl -sSL --fail -o "$CACHE/$TARBALL.part" "$TARBALL_URL" \
    || die "download failed: $TARBALL_URL"
  mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
fi
echo "$TARBALL_SHA256  $CACHE/$TARBALL" | sha256sum -c - >/dev/null \
  || die "tarball digest mismatch: $CACHE/$TARBALL (delete it and retry)"

# --- 2. extract the runtime subset + stage generated files ------------------
# The release tarball's msvc/ lacks the .targets files and the two
# headers below (they live in git, not in `make dist`); the headers are
# vendored in this directory (same tag, MIT — see README.md) and the
# source list is vendored as mono-sources.txt, so no git fetch is needed.
if [[ ! -f "$SRC/.tiramisu-staged" || "$(cat "$SRC/.tiramisu-staged" 2>/dev/null)" != "$current_stamp" ]]; then
  log "extracting $MONO_VERSION runtime subset to $SRC"
  rm -rf "$SRC"
  mkdir -p "$SRC"
  tar -xf "$CACHE/$TARBALL" -C "$CACHE/src" \
    "mono-6.8.0.105/mono" "mono-6.8.0.105/support" "mono-6.8.0.105/msvc" \
    "mono-6.8.0.105/winconfig.h" "mono-6.8.0.105/configure.ac" \
    "mono-6.8.0.105/LICENSE" \
    || die "tar extraction failed"

  cp "$UWP_DIR/msvc/msvc-win32-support.h" "$UWP_DIR/msvc/msvc-disabled-warnings.h" \
    "$SRC/msvc/" || die "vendored msvc headers missing"

  # UWP patches (if any): applied to the fresh extraction, in sorted
  # order, BEFORE config.h is generated — patch 03 modifies winconfig.h,
  # the config.h template, so it must land first.
  for p in "$UWP_DIR"/patches/*.patch; do
    [[ -f "$p" ]] || continue
    log "applying patch $(basename "$p")"
    patch -p1 -d "$SRC" --forward < "$p" || die "patch failed: $p"
  done

  # config.h: replicate mono.winconfig.targets (RunWinConfigSetup) — the
  # MSVC template winconfig.h with the version placeholders substituted
  # from configure.ac. The @*_DEFINES@ lines are comments expanding to
  # nothing without a cygconfig.h, so only the versions need replacing.
  mono_ver="$(sed -n 's/^AC_INIT(mono, \[\([0-9.]*\)\].*/\1/p' "$SRC/configure.ac" | head -1)"
  corlib_ver="$(sed -n 's/^MONO_CORLIB_VERSION=//p' "$SRC/configure.ac" | head -1)"
  [[ -n "$mono_ver" && -n "$corlib_ver" ]] || die "could not read versions from configure.ac"
  sed -e "s/#MONO_VERSION#/$mono_ver/g" -e "s/#MONO_CORLIB_VERSION#/$corlib_ver/g" \
    "$SRC/winconfig.h" > "$SRC/config.h"

  # version.h: the MSVC build writes only FULL_VERSION.
  printf '#define FULL_VERSION "tiramisu-uwp/%s"\n' "$mono_ver" > "$SRC/mono/mini/version.h"

  # cpu-amd64.h: the JIT machine description, generated by genmdesc.py
  # (host Python) from cpu-amd64.md + mini-ops.h — the same rule as
  # mono/mini/Makefile.am. Needed even for interpreter-only use: mini.c
  # includes it unconditionally.
  (cd "$SRC/mono/mini" && python3 genmdesc.py TARGET_AMD64 . cpu-amd64.h \
    amd64_desc ./cpu-amd64.md) || die "genmdesc.py failed"

  echo "$current_stamp" > "$SRC/.tiramisu-staged"
fi

# --- 3. compile the runtime via the UWP crossbuild wrapper ------------------
# Defines mirror msvc/mono.props (MONO_PREPROCESSOR_DEFINITIONS) +
# the per-project Release|x64 defines, with one deliberate deviation:
# WINVER/_WIN32_WINNT 0x0A00 instead of 0x0601: UWP is Win10+, and
# LoadPackagedLibrary needs _WIN32_WINNT >= 0x0602 to be declared.
# HAVE_EXTERN_DEFINED_WINAPI_SUPPORT is Mono's own opt-in for the APP
# partition (msvc-win32-support.h #errors without it under UWP).
#
# The runtime is built per library, exactly like the MSVC solution
# (eglib, libmini, libmonoruntime, libmonoutils, libgcmonosgen), then
# merged into one archive with llvm-lib. This matters: the SGen GC
# defines (HAVE_SGEN_GC, HAVE_MOVING_COLLECTOR, HAVE_WRITE_BARRIERS,
# HAVE_CONC_GC_AS_DEFAULT) go to libmonoruntime AND libgcmonosgen in
# the MSVC build (libmonoruntime via $(GC_DEFINES), libgcmonosgen via
# $(SGEN_DEFINES) — same four defines) — sgen-gc.c is entirely gated
# on HAVE_SGEN_GC, so without them the GC library compiles empty.
# They must NOT leak into libmini: mono/mini/mini.h #errors if they
# do, because the JIT must not depend on GC internals.
common_defs=(/MT /O2 /W0
  /D__default_codegen__ /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE
  /DHAVE_CONFIG_H /DGC_NOT_DLL /DWIN32_THREADS
  /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0501
  /D_UNICODE /DUNICODE /DFD_SETSIZE=1024 /DNVALGRIND
  /DWIN32 /DWIN32_LEAN_AND_MEAN /DWIN64 /DNDEBUG /D_LIB
  /DHAVE_EXTERN_DEFINED_WINAPI_SUPPORT
  /DHAVE_API_SUPPORT_WIN32_GET_MODULE_HANDLE=1
  /DHAVE_API_SUPPORT_WIN32_LOAD_PACKAGED_LIBRARY=1)
gc_defs=(/DHAVE_SGEN_GC /DHAVE_MOVING_COLLECTOR /DHAVE_WRITE_BARRIERS
         /DHAVE_CONC_GC_AS_DEFAULT)

mkdir -p "$OUT/.parts"
part_libs=()
for lib in eglib libmini libmonoruntime libmonoutils libgcmonosgen; do
  mapfile -t lib_srcs < <(awk -v lib="$lib" -F'\t' '$1==lib {print $2}' \
    "$UWP_DIR/mono-sources.txt" | sed "s|^|$SRC/|")
  [[ "${#lib_srcs[@]}" -gt 0 ]] || die "no sources for $lib in mono-sources.txt"
  log "compiling $lib (${#lib_srcs[@]} sources)"
  extra_defs=()
  if [[ "$lib" == libmonoruntime || "$lib" == libgcmonosgen ]]; then
    extra_defs=("${gc_defs[@]}")
  fi
  # -I include/ first: the SDK-header case shims (SDKDDKVer.h,
  # MSWSock.h) must win over the SDK's lowercased filenames (see
  # include/README.md). Per-lib object dirs: object names are
  # basename-derived and could collide across libraries.
  UWP_OBJ_DIR="$CACHE/obj-$lib" "$CB/scripts/build.sh" --uwp --static-lib \
    --out "$OUT/.parts/$lib.lib" --jobs "${TRT_JOBS:-2}" \
    -I "$UWP_DIR/include" -I "$SRC" -I "$SRC/mono" -I "$SRC/mono/eglib" \
    "${lib_srcs[@]}" \
    -- "${common_defs[@]}" "${extra_defs[@]}" \
    || die "compilation failed: $lib"
  part_libs+=("$OUT/.parts/$lib.lib")
done

# --- 3b. assemble win64.S (mono_context_get_current +
# copy_stack_data_internal_win32_wrapper) ----------------------------------
# Mono keeps these two x64 functions in mono/utils/win64.asm (MASM, git
# only — not in the release tarball). clang-cl cannot assemble MASM, so
# asm/win64.S is a mechanical GNU as translation (see its header); it
# assembles to the same instruction stream (verified by disassembly).
log "assembling asm/win64.S"
clang-cl -target x86_64-pc-windows-msvc -c "$UWP_DIR/asm/win64.S" \
  -o "$CACHE/obj/win64.obj" || die "win64.S assembly failed"

log "merging ${#part_libs[@]} per-library archives + win64.obj into $LIB"
llvm-lib "/out:$LIB" "${part_libs[@]}" "$CACHE/obj/win64.obj" || die "llvm-lib merge failed"

# --- 4. verify the embedding entry points are in the archive -----------------
# NB: llvm-nm's output is captured to a file first: piping it to `grep -q`
# makes grep exit on the first match, SIGPIPE-ing llvm-nm, which under
# `set -o pipefail` fails the check even when the symbol is present
# (llvm-nm exits 74 on the broken pipe). Capturing once is also faster
# (one nm invocation instead of eight).
nm_out="$OUT/.mono-uwp-symbols.txt"
llvm-nm --defined-only "$LIB" > "$nm_out" 2>/dev/null \
  || die "llvm-nm failed on $LIB"
for sym in mono_jit_init mono_jit_exec mono_config_parse mono_set_dirs \
           mono_domain_assembly_open mono_add_internal_call \
           mono_install_assembly_load_hook mono_jit_cleanup \
           mono_context_get_current copy_stack_data_internal_win32_wrapper \
           mono_mmap_map MonoLoadImage; do
  grep -q " T $sym$" "$nm_out" \
    || die "symbol missing from $LIB: $sym"
done
log "symbols present: mono_jit_init, mono_jit_exec, +10 more"

echo "$current_stamp" > "$OUT/.mono-uwp-stamp"
log "built $LIB ($(du -h "$LIB" | cut -f1))"
printf '%s\n' "$LIB" >&3
