#!/usr/bin/env bash
# tiramisu/third_party/mono-bridges/build-real-bridges.sh
# Build the REAL FMOD + Steamworks dllmap .so files for the Mono harness:
#   out/libfmod-real.so          (fmod + fmodstudio dllmaps)
#   out/libcsteamworks-real.so   (CSteamworks + sdkencryptedappticket dllmaps)
#
# Each .so is self-contained: the bridge sources it needs, the door
# wrappers, and mono_dllmap_shim.cpp (host services the bridges expect).
# Linked -Bsymbolic so internal references bind to the .so's own
# definitions (the test binary links libtrt.a, which has its own copies
# of trt_audio_* etc.). -Wl,--no-undefined fails the build on any
# missing shim symbol.
#
# The build regenerates gen/gen_studio_doors.inc and the two
# gen_*_failvisible.inc files from the implmaps, then verifies every
# implmap native name is exported (Mono throws EntryPointNotFound
# otherwise).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
generate_bridge_includes() {
  mkdir -p "$here/gen"
  python3 "$here/gen-failvisible.py"
  python3 "$here/door-gen.py"
}

if [[ "${1:-}" == "--generate-only" ]]; then
  generate_bridge_includes
  exit 0
fi

out="$here/out"
mkdir -p "$out"

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
CXXFLAGS="-std=c++20 -O2 -Wall -fPIC -Isrc/trt -Ithird_party/mono/usr/include/mono-2.0"
CFLAGS="-O2 -Wall -fPIC"

cd "$root"

# Up-to-date check: skip the rebuild when both .so files exist and are
# newer than every input (the gate builds the FNA stack twice). Pass
# --force to rebuild unconditionally.
if [[ "${1:-}" != "--force" ]] && [[ -f "$out/libfmod-real.so" ]] &&
   [[ -f "$out/libcsteamworks-real.so" ]] &&
   [[ -f "$here/gen/gen_studio_doors.inc" ]] &&
   [[ -f "$here/gen/gen_fmod_failvisible.inc" ]] &&
   [[ -f "$here/gen/gen_steam_failvisible.inc" ]]; then
  stale=0
  for src in src/trt/bridge_fmod.cpp src/trt/bridge_fmod_studio.cpp \
             src/trt/audio_codecs.cpp src/trt/audio_codecs.h \
             src/trt/trt_trace.cpp src/trt/trt_trace.h \
             src/trt/bridge_fmod_studio.h src/trt/bridge_steam.cpp \
             src/trt/bridge_steam.h src/trt/vendor_stb_vorbis.c \
             third_party/mono-bridges/mono_fmod_door.cpp \
             third_party/mono-bridges/mono_steam_door.cpp \
             third_party/mono-bridges/mono_dllmap_shim.cpp \
             third_party/mono-bridges/gen-failvisible.py \
             third_party/mono-bridges/door-gen.py \
             third_party/mono-bridges/celeste.implmap \
             third_party/mono-bridges/steamworks.implmap \
             third_party/mono-bridges/real-fmod.txt \
             third_party/mono-bridges/real-steam.txt; do
    if [[ "$src" -nt "$out/libfmod-real.so" ]] ||
       [[ "$src" -nt "$out/libcsteamworks-real.so" ]]; then
      stale=1
      break
    fi
  done
  if [[ "$stale" == 0 ]]; then
    echo "real bridges up to date: $out/libfmod-real.so $out/libcsteamworks-real.so"
    exit 0
  fi
fi

# 1. Generate the door + fail-visible includes from the implmaps.
generate_bridge_includes

# 2. Compile the bridge sources (with -fPIC; libtrt.a objects are
#    -fno-pie and cannot go into a shared object).
objs="$out/objs"
mkdir -p "$objs"
$CXX $CXXFLAGS -c src/trt/bridge_fmod.cpp -o "$objs/bridge_fmod.o"
$CXX $CXXFLAGS -c src/trt/bridge_fmod_studio.cpp -o "$objs/bridge_fmod_studio.o"
$CXX $CXXFLAGS -c src/trt/audio_codecs.cpp -o "$objs/audio_codecs.o"
$CXX $CXXFLAGS -c src/trt/trt_trace.cpp -o "$objs/trt_trace.o"
$CXX $CXXFLAGS -c src/trt/bridge_steam.cpp -o "$objs/bridge_steam.o"
$CXX $CXXFLAGS -c src/trt/steam_auth.cpp -o "$objs/steam_auth.o"
$CXX $CXXFLAGS -c src/trt/steam_library.cpp -o "$objs/steam_library.o"
$CC $CFLAGS -DSTB_VORBIS_NO_STDIO -c src/trt/vendor_stb_vorbis.c \
  -o "$objs/vendor_stb_vorbis.o"
$CC $CFLAGS -c src/trt/vendor_dr_mp3.c -o "$objs/vendor_dr_mp3.o"
$CXX $CXXFLAGS -c third_party/mono-bridges/mono_dllmap_shim.cpp \
  -o "$objs/mono_dllmap_shim.o"
$CXX $CXXFLAGS -c third_party/mono-bridges/mono_fmod_door.cpp \
  -o "$objs/mono_fmod_door.o"
$CXX $CXXFLAGS -c third_party/mono-bridges/mono_steam_door.cpp \
  -o "$objs/mono_steam_door.o"

# 3. Link.
$CXX -shared -fPIC -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,--no-undefined \
  "$objs/bridge_fmod.o" "$objs/bridge_fmod_studio.o" "$objs/audio_codecs.o" \
  "$objs/trt_trace.o" \
  "$objs/vendor_stb_vorbis.o" "$objs/vendor_dr_mp3.o" "$objs/mono_dllmap_shim.o" \
  "$objs/mono_fmod_door.o" -lpthread -lm \
  -o "$out/libfmod-real.so"
$CXX -shared -fPIC -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,--no-undefined \
  "$objs/bridge_steam.o" "$objs/steam_auth.o" "$objs/steam_library.o" \
  "$objs/mono_dllmap_shim.o" \
  "$objs/mono_steam_door.o" -lpthread -lm \
  -o "$out/libcsteamworks-real.so"

# 4. Verify: every implmap native name must be exported.
python3 - "$here/celeste.implmap" "$here/steamworks.implmap" \
  "$out/libfmod-real.so" "$out/libcsteamworks-real.so" <<'PYEOF'
import re, subprocess, sys
def names(path):
    out = set()
    for line in open(path):
        m = re.search(r'\((\S+) (?:fmod|fmodstudio|CSteamworks|sdkencryptedappticket)\)', line)
        if m: out.add(m.group(1))
    return out
def exports(lib):
    r = subprocess.run(["nm", "-D", "--defined-only", lib], capture_output=True, text=True)
    return {l.split()[-1] for l in r.stdout.splitlines() if len(l.split()) >= 3}
fmod, steam = names(sys.argv[1]), names(sys.argv[2])
ef, es = exports(sys.argv[3]), exports(sys.argv[4])
miss_f = sorted(fmod - ef)
miss_s = sorted(steam - es)
print(f"fmod: {len(fmod)} implmap names, {len(ef)} exports, missing {len(miss_f)}")
print(f"steam: {len(steam)} implmap names, {len(es)} exports, missing {len(miss_s)}")
if miss_f: print("missing fmod:", *miss_f[:20])
if miss_s: print("missing steam:", *miss_s[:20])
sys.exit(1 if (miss_f or miss_s) else 0)
PYEOF

echo "built $out/libfmod-real.so $out/libcsteamworks-real.so"
