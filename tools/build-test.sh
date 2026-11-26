#!/bin/bash
# tiramisu/tools/build-test.sh
# Offline verification: build the guest PE (clang cross, no SDK needed),
# build the loader + host-side test on Linux, run it.
# Exit 0 = every check passed. This is the gate before any console work.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=tests/out
mkdir -p "$out"

# --- pick the LLVM 19 toolchain (STL/loader parity with the console build) ---
LLVM_BIN="/usr/lib/llvm-19/bin"
CLANG="$LLVM_BIN/clang"
LLD_LINK="$HOME/.local/bin/lld-link"
LLVM_DLLTOOL="$LLVM_BIN/llvm-dlltool"
[[ -x "$CLANG" ]] || CLANG="$HOME/.local/bin/clang-19"
[[ -x "$CLANG" ]] || { echo "ERROR: clang 19 not found" >&2; exit 1; }
"$CLANG" --version | head -1

# --- 0. Precompile src/trt/*.cpp + miniz_tinfl.c in parallel into libtrt.a ---
objdir="$out/trt_objs"
mkdir -p "$objdir"
if [[ -z "${SKIP_PRECOMPILE:-}" || ! -f "$out/libtrt.a" ]]; then
  rm -rf "$objdir" "$out/libtrt.a"
  mkdir -p "$objdir"
  printf '%s\n' src/trt/*.cpp | xargs -P"${TRT_JOBS:-4}" -I{} bash -c '
    f="$1"
    base="$(basename "$f" .cpp)"
    clang++ -std=c++20 -O2 -Wall -fno-pie -Ithird_party/mono/usr/include/mono-2.0 -c "$f" -o "'"$objdir"'/${base}.o"
  ' _ {}
  clang -O2 -Wall -fno-pie -c src/host/vendor/miniz_tinfl.c -o "$objdir/miniz_tinfl.o"
  clang -std=gnu11 -O2 -fno-pie -c src/trt/mojo_hlsl.c -o "$objdir/mojo_hlsl.o"
  clang -O2 -Wall -fno-pie -c src/trt/vendor_stb_vorbis.c -o "$objdir/vendor_stb_vorbis.o" -DSTB_VORBIS_NO_STDIO
  clang -O2 -Wall -fno-pie -c src/trt/vendor_dr_mp3.c -o "$objdir/vendor_dr_mp3.o"
  ar rcs "$out/libtrt.a" "$objdir"/*.o
fi

# --- 1. import library for the guest (tiramisu_host.dll) ---
rm -f "$out/tiramisu_host.lib"
"$LLD_LINK" /lib /def:src/guest/tiramisu_host.def /out:"$out/tiramisu_host.lib" /machine:x64 /noentry

# --- 2. guest PE64: freestanding, imports bound at load time by trt ---
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_hello.c -o "$out/guest_hello.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_data.c -o "$out/guest_data.obj"
"$LLD_LINK" "$out/guest_hello.obj" "$out/guest_data.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:guest_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /OUT:"$out/guest_hello.exe"
# internally-consistent low-base variant: fixups encode ImageBase 0x1A00000,
# the reloc-path test loads it off that base and checks the DIR64 pass
"$LLD_LINK" "$out/guest_hello.obj" "$out/guest_data.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:guest_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_hello_reloc.exe"
file "$out/guest_hello.exe"

# --- 3. host-side loader test on Linux ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_pe_loader.cpp "$out/libtrt.a" \
  -o "$out/test_pe_loader"

"$out/test_pe_loader" "$out/guest_hello.exe" "$out/guest_hello_reloc.exe"

# --- 3b. PE32 guest + loader test (M7 Phase 1: load-only, no execution) ---
# x86 import libs from the .def files (kernel32.def lists the same names
# the 64-bit guest uses; the 32-bit guest declares them cdecl so the
# imports are undecorated, like real PE32 files).
"$LLD_LINK" /lib /def:src/guest/kernel32.def /out:"$out/kernel32_32.lib" /machine:x86 /noentry
"$LLD_LINK" /lib /def:src/guest/guest_lib32.def /out:"$out/guest_lib32.lib" /machine:x86 /noentry
"$LLD_LINK" /lib /def:src/guest/guest_delay32.def /out:"$out/guest_delay32.lib" /machine:x86 /noentry
"$CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_hello32.c -o "$out/guest_hello32.obj"
"$CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_data32.c -o "$out/guest_data32.obj"
"$CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_lib32.c -o "$out/guest_lib32.obj"
"$CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_delay32.c -o "$out/guest_delay32.obj"
"$CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/delayimp32.c -o "$out/delayimp32.obj"
"$LLD_LINK" "$out/guest_lib32.obj" /DLL /NODEFAULTLIB /NOENTRY /MACHINE:X86 \
  /DYNAMICBASE /BASE:0x10000000 /OUT:"$out/guest_lib32.dll"
"$LLD_LINK" "$out/guest_delay32.obj" /DLL /NODEFAULTLIB /NOENTRY /MACHINE:X86 \
  /DYNAMICBASE /BASE:0x10000000 /OUT:"$out/guest_delay32.dll"
"$LLD_LINK" "$out/guest_hello32.obj" "$out/guest_data32.obj" "$out/delayimp32.obj" \
  "$out/kernel32_32.lib" "$out/guest_lib32.lib" "$out/guest_delay32.lib" \
  /NODEFAULTLIB /ENTRY:guest32_main /SUBSYSTEM:WINDOWS /MACHINE:X86 \
  /DYNAMICBASE /BASE:0x10000000 /DELAYLOAD:guest_delay32.dll \
  /OUT:"$out/guest_hello32.exe"
file "$out/guest_hello32.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_pe32.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_pe32"
"$out/test_pe32" "$out/guest_hello32.exe" "$out/guest_lib32.dll"

# --- 3e. Phase 3: PC->Xbox API translator -----------------------------------
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_pc2xb_translator.cpp "$out/libtrt.a" \
  -o "$out/test_pc2xb_translator"
"$out/test_pc2xb_translator" "$out/guest_hello.exe"
clang++ -std=c++20 -O2 -Wall -no-pie tools/pc2xb.cpp "$out/libtrt.a" \
  -o "$out/pc2xb"
"$out/pc2xb" "$out/guest_hello.exe" "$out/xb_out"

# --- 3f. Phase 4: universal guest runner --------------------------------------
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_universal_guest.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_universal_guest"
"$out/test_universal_guest" "$out/guest_hello.exe"

# --- 3d. Universal engine (Phase 1+2): import-surface enumerator + universal
#         profile mode. Runs the enumerator against the built guest_hello.exe
#         and asserts trt_synthesize_engine classifies it UNIVERSAL. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_universal_profile.cpp "$out/libtrt.a" \
  -o "$out/test_universal_profile"
"$out/test_universal_profile" "$out/guest_hello.exe"

# --- 3c. x86 interpreter EXECUTION gate: the instruction bytes must compute.
#         CPUID/RDTSC, SSE2 packed double + integer SIMD, MMX, 8-bit ALU,
#         0x80 group, REP MOVSB, PUSHA/POPA, FXSAVE/FXRSTOR, ModRM disp/SIB
#         addressing, contiguous guest VA, and the import dispatcher
#         (TLS, GetLastError, Interlocked*, stdcall/cdecl metadata). ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_x86_exec.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_x86_exec"
"$out/test_x86_exec"

# --- 4. input bridge test (user32 surface the guests consume) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_input.cpp "$out/libtrt.a" \
  -o "$out/test_input"
"$out/test_input"

# --- 5. THE launch-path proof: game-shaped PE through the full pipeline ---
rm -f "$out/kernel32.lib"
"$LLD_LINK" /lib /def:src/guest/kernel32.def /out:"$out/kernel32.lib" /machine:x64 /noentry
rm -f "$out/user32.lib"
"$LLD_LINK" /lib /def:src/guest/user32.def /out:"$out/user32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_game.c -o "$out/guest_game.obj"
"$LLD_LINK" "$out/guest_game.obj" "$out/kernel32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:game_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_game.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_game_boot.cpp "$out/libtrt.a" \
  -o "$out/test_game_boot"
"$out/test_game_boot" "$out/guest_game.exe"

# --- 6. real-game load path: api-sets, delay imports, fail-visible stubs ---
rm -f "$out/api-ms-win-core-synch-l1-2-0.lib"
"$LLD_LINK" /lib /def:src/guest/api-ms-win-core-synch-l1-2-0.def \
  /out:"$out/api-ms-win-core-synch-l1-2-0.lib" /machine:x64 /noentry
rm -f "$out/api-ms-win-core-winrt-l1-1-0.lib"
"$LLD_LINK" /lib /def:src/guest/api-ms-win-core-winrt-l1-1-0.def \
  /out:"$out/api-ms-win-core-winrt-l1-1-0.lib" /machine:x64 /noentry
rm -f "$out/advapi32.lib"
"$LLD_LINK" /lib /def:src/guest/advapi32.def /out:"$out/advapi32.lib" /machine:x64 /noentry
rm -f "$out/d3dcompiler_47.lib"
"$LLD_LINK" /lib /def:src/guest/d3dcompiler_47.def /out:"$out/d3dcompiler_47.lib" /machine:x64 /noentry
rm -f "$out/delayimp.lib"
"$LLD_LINK" /lib /def:src/guest/delayimp.def /out:"$out/delayimp.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_realload.c -o "$out/guest_realload.obj"
"$LLD_LINK" "$out/guest_realload.obj" \
  "$out/api-ms-win-core-synch-l1-2-0.lib" "$out/api-ms-win-core-winrt-l1-1-0.lib" \
  "$out/advapi32.lib" "$out/d3dcompiler_47.lib" "$out/delayimp.lib" \
  /NODEFAULTLIB /ENTRY:real_load_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /DELAYLOAD:d3dcompiler_47.dll \
  /OUT:"$out/guest_realload.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_real_load.cpp "$out/libtrt.a" \
  -o "$out/test_real_load"
"$out/test_real_load" "$out/guest_realload.exe"

# --- 6b. real-game corpus load matrix (hardware-free) -----------------------
if [[ -d corpus-local ]] && [[ -n "$(find corpus-local -maxdepth 2 -type f 2>/dev/null)" ]]; then
  bash tools/corpus-smoke.sh
else
  echo "SKIP corpus load matrix: corpus-local/ absent or empty"
fi

# --- 6c. STAGE 6 run-depth: EXECUTE real DllMain code on the corpus --------
if [[ -d corpus-local ]] && [[ -n "$(find corpus-local -maxdepth 2 -type f 2>/dev/null)" ]]; then
  bash tools/corpus-run.sh
else
  echo "SKIP corpus run-depth: corpus-local/ absent or empty"
fi

# --- 6d. disc images: ISO9660+Joliet / UDF / raw 2352 -> a mounted drive ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_iso.cpp "$out/libtrt.a" \
  -o "$out/test_iso"
"$out/test_iso"

# --- 7. guest DLL loading: the DLLs a game ships with it ---
cat > "$out/guestdll.def" <<'DEOF'
LIBRARY guestdll
EXPORTS
    dll_add
DEOF
rm -f "$out/guestdll.lib"
"$LLD_LINK" /lib /def:"$out/guestdll.def" /out:"$out/guestdll.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dll.c -o "$out/guest_dll.obj"
"$LLD_LINK" "$out/guest_dll.obj" "$out/tiramisu_host.lib" \
  /NODEFAULTLIB /ENTRY:dll_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1B00000 /DEF:"$out/guestdll.def" \
  /implib:"$out/guestdll.dll.lib" /OUT:"$out/guestdll.dll"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_libload.c -o "$out/guest_libload.obj"
"$LLD_LINK" "$out/guest_libload.obj" "$out/guestdll.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:libload_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_libload.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_libload.cpp "$out/libtrt.a" \
  -o "$out/test_libload"
"$out/test_libload" "$out/guest_libload.exe" "$out"
split_dir="$out/test_libload_split"
mkdir -p "$split_dir/bin" "$split_dir/sub"
cp "$out/guest_libload.exe" "$split_dir/bin/"
cp "$out/guestdll.dll" "$split_dir/"
cp "$out/guestdll.dll" "$split_dir/sub/nested.dll"
"$out/test_libload" "$split_dir/bin/guest_libload.exe" "$split_dir/bin" "$split_dir" \
  "$split_dir/sub/nested.dll"
clang++ -std=c++20 -O2 -Wall -no-pie tests/test_formatmessage_alloc.cpp "$out/libtrt.a" \
  -lpthread -o "$out/test_formatmessage_alloc"
"$out/test_formatmessage_alloc"

# --- 8. the graphics bridge: the guest's D3D11 calls must reach the host's GPU ---
SDK="$HOME/.cache/uwp-crossbuild/xwin/sdk/include"
XCRT="$HOME/.cache/uwp-crossbuild/xwin/crt/include"
if [[ -d "$SDK/um" && -d "$XCRT" ]]; then
cat > "$out/d3d11.def" <<'DEOF'
LIBRARY d3d11
EXPORTS
    D3D11CreateDevice
DEOF
cat > "$out/dxgi.def" <<'DEOF'
LIBRARY dxgi
EXPORTS
    CreateDXGIFactory1
    CreateDXGIFactory2
    ?CreateDXGIFactory2@@YAJAEBU_GUID@@PEAPEAX@Z
DEOF
rm -f "$out/d3d11.lib" "$out/dxgi.lib"
"$LLD_LINK" /lib /def:"$out/d3d11.def" /out:"$out/d3d11.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/dxgi.def" /out:"$out/dxgi.lib" /machine:x64 /noentry
"$CLANG" -x c++ --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -fno-exceptions -fno-rtti \
  -I"$SDK/um" -I"$SDK/shared" -I"$SDK/ucrt" -I"$XCRT" \
  -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS \
  -c src/guest/guest_d3d.c -o "$out/guest_d3d.obj"
"$LLD_LINK" "$out/guest_d3d.obj" "$out/d3d11.lib" "$out/dxgi.lib" \
  /NODEFAULTLIB /ENTRY:d3d_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_d3d.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d.cpp "$out/libtrt.a" \
  -o "$out/test_d3d"
"$out/test_d3d" "$out/guest_d3d.exe"
else
  echo "SKIP graphics bridge suite: no Windows SDK at $SDK"
fi

# --- 9. the registry: UWP has none, games expect one ---
cat > "$out/advapi32.def" <<'DEOF'
LIBRARY advapi32
EXPORTS
    RegOpenKeyExW
    RegCreateKeyExW
    RegQueryValueExW
    RegSetValueExW
    RegCloseKey
    RegEnumKeyExW
    OpenProcessToken
    GetTokenInformation
    ConvertStringSecurityDescriptorToSecurityDescriptorW
    ConvertSidToStringSidW
DEOF
rm -f "$out/advapi32.lib"
"$LLD_LINK" /lib /def:"$out/advapi32.def" /out:"$out/advapi32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_reg.c -o "$out/guest_reg.obj"
"$LLD_LINK" "$out/guest_reg.obj" "$out/advapi32.lib" \
  /NODEFAULTLIB /ENTRY:reg_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_reg.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_reg.cpp "$out/libtrt.a" \
  -o "$out/test_reg"
DRIEC="$out/drive_c"; rm -rf "$DRIEC"; mkdir -p "$DRIEC"
"$out/test_reg" "$out/guest_reg.exe" "$DRIEC"

# --- 10. the engine-API batch: mapping, threads, system info, lifecycle ---
cat > "$out/engine.def" <<'DEOF'
LIBRARY kernel32
EXPORTS
    CreateFileMappingW
    MapViewOfFile
    UnmapViewOfFile
    CreateThread
    WaitForSingleObject
    CreateEventW
    SetEvent
    ResetEvent
    InitializeCriticalSection
    EnterCriticalSection
    LeaveCriticalSection
    GetVersionExW
    GetSystemInfo
    MultiByteToWideChar
    WideCharToMultiByte
    GetTempPathW
    GetTickCount64
    CreateProcessW
    GetStdHandle
    SetStdHandle
    GetConsoleMode
    ReadConsoleW
    WriteConsoleW
    SetConsoleCtrlHandler
    SetLastError
    GetLastError
DEOF
cat > "$out/engine_u.def" <<'DEOF'
LIBRARY user32
EXPORTS
    PostQuitMessage
    SetFocus
    GetFocus
    GetClientRect
DEOF
rm -f "$out/engine.lib" "$out/engine_u.lib"
"$LLD_LINK" /lib /def:"$out/engine.def" /out:"$out/engine.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/engine_u.def" /out:"$out/engine_u.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_engine.c -o "$out/guest_engine.obj"
"$LLD_LINK" "$out/guest_engine.obj" "$out/engine.lib" "$out/engine_u.lib" \
  "$out/kernel32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:engine_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_engine.exe"
# --- 8b. STAGE 7: the SEH/unwind surface OrTMM's CRT startup edits ---
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -fasynchronous-unwind-tables \
  -c src/guest/guest_seh.c -o "$out/guest_seh.obj"
"$LLD_LINK" "$out/guest_seh.obj" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_seh_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /OUT:"$out/guest_seh.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_engine.cpp "$out/libtrt.a" \
  -o "$out/test_engine"
DRIEC="$out/drive_c"; rm -rf "$DRIEC"; mkdir -p "$DRIEC"
"$out/test_engine" "$out/guest_engine.exe" "$DRIEC" "$out/guest_seh.exe"

# --- 8c. W1: exception dispatch through a real MSVC-shaped personality ------
# The guest carries compiler-generated .pdata + a C scope table and its own
# __C_specific_handler clone; the host must walk .pdata, call the personality,
# honour the HandlerAddress==1 constant-filter sentinel, and resume through
# RtlUnwindEx with the establishing frame's RSP and nonvolatiles restored.
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -fasynchronous-unwind-tables \
  -c src/guest/guest_eh.c -o "$out/guest_eh.obj"
# exports for the funclet-rethrow fixtures (linked into the image's export dir)
"$LLD_LINK" "$out/guest_eh.obj" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_eh_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 \
  /EXPORT:guest_eh_main \
  /EXPORT:guest_eh_funclet_rethrow \
  /EXPORT:guest_eh_funclet_rethrow_escape \
  /OUT:"$out/guest_eh.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_guest_eh.cpp "$out/libtrt.a" \
  -o "$out/test_guest_eh"
"$out/test_guest_eh" "$out/guest_eh.exe"

# --- 8b. WASAPI pacing + CV wake atomicity (Ultra Pool boot-wedge
#     regressions: 97%-CPU mix storm, lost SleepConditionVariableSRW wake) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_wasapi_pacing.cpp "$out/libtrt.a" \
  -o "$out/test_wasapi_pacing" -lpthread
"$out/test_wasapi_pacing"

# --- 8c. Unwinder safety (Go Next/UnityPlayer host-SIGSEGV regression:
#     corrupt/unmapped unwind metadata -> clean no-handler verdict, never
#     a host crash) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_unwind_safety.cpp "$out/libtrt.a" \
  -o "$out/test_unwind_safety" -lpthread
"$out/test_unwind_safety"

# --- 9. DirectInput 8: factory -> device -> acquire -> poll -> state ---
cat > "$out/dinput.def" <<'DEOF'
LIBRARY dinput8
EXPORTS
    DirectInput8Create
    trt_di_feed_move
    trt_di_feed_button
    trt_di_feed_key
DEOF
rm -f "$out/dinput.lib"
"$LLD_LINK" /lib /def:"$out/dinput.def" /out:"$out/dinput.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dinput.c -o "$out/guest_dinput.obj"
"$LLD_LINK" "$out/guest_dinput.obj" "$out/dinput.lib" \
  "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:dinput_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1B00000 /OUT:"$out/guest_dinput.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_dinput.cpp "$out/libtrt.a" \
  -o "$out/test_dinput"
"$out/test_dinput" "$out/guest_dinput.exe"

# --- 10. XAudio2: engine -> voices -> PCM submit -> queue state ---
cat > "$out/audio.def" <<'DEOF'
LIBRARY xaudio2_9
EXPORTS
    XAudio2Create
    trt_audio_note_submit
    trt_audio_note_start
DEOF
rm -f "$out/audio.lib"
"$LLD_LINK" /lib /def:"$out/audio.def" /out:"$out/audio.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_audio.c -o "$out/guest_audio.obj"
"$LLD_LINK" "$out/guest_audio.obj" "$out/audio.lib" \
  /NODEFAULTLIB /ENTRY:audio_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1C00000 /OUT:"$out/guest_audio.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_audio.cpp "$out/libtrt.a" \
  -o "$out/test_audio"
"$out/test_audio" "$out/guest_audio.exe"

# --- 11. Steamworks: the auth bridge (no DRM bypass, ever) ---
cat > "$out/steam.def" <<'DEOF'
LIBRARY steam_api64
EXPORTS
    SteamAPI_RestartAppIfNecessary
    SteamAPI_Init
    SteamAPI_Shutdown
    SteamAPI_IsSteamRunning
    SteamAPI_GetSteamID
    SteamAPI_SetRichPresence
    SteamAPI_RunCallbacks
DEOF
rm -f "$out/steam.lib"
"$LLD_LINK" /lib /def:"$out/steam.def" /out:"$out/steam.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_steam.c -o "$out/guest_steam.obj"
"$LLD_LINK" "$out/guest_steam.obj" "$out/steam.lib" \
  /NODEFAULTLIB /ENTRY:steam_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_steam.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_steam.cpp "$out/libtrt.a" \
  -o "$out/test_steam"
"$out/test_steam" "$out/guest_steam.exe"

# --- 11b. Steam account/library modules: VDF parsing + OpenID validation ---
# No network: the transport is replaced by trt_steam_set_http_hook().
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_steam_auth.cpp "$out/libtrt.a" \
  -o "$out/test_steam_auth"
"$out/test_steam_auth"

# --- 12. winmm timing: the clock games schedule their frames on -------------
rm -f "$out/winmm.lib"
"$LLD_LINK" /lib /def:src/guest/winmm.def /out:"$out/winmm.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_winmm.c -o "$out/guest_winmm.obj"
"$LLD_LINK" "$out/guest_winmm.obj" "$out/winmm.lib" \
  /NODEFAULTLIB /ENTRY:guest_winmm_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_winmm.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_winmm.cpp "$out/libtrt.a" \
  -o "$out/test_winmm"
"$out/test_winmm" "$out/guest_winmm.exe"

# --- 13. d3d9: the D3D9 -> D3D11 bridge (M5) --------------------------------
rm -f "$out/d3d9.lib"
"$LLD_LINK" /lib /def:src/guest/d3d9.def /out:"$out/d3d9.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_d3d9.c -o "$out/guest_d3d9.obj"
"$LLD_LINK" "$out/guest_d3d9.obj" "$out/d3d9.lib" \
  /NODEFAULTLIB /ENTRY:guest_d3d9_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1B00000 /OUT:"$out/guest_d3d9.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d9.cpp "$out/libtrt.a" \
  -o "$out/test_d3d9"
"$out/test_d3d9" "$out/guest_d3d9.exe"

# --- 20. d3d8: the D3D8 -> D3D11 bridge (M7) ---------------------------------
rm -f "$out/d3d8.lib"
"$LLD_LINK" /lib /def:src/guest/d3d8.def /out:"$out/d3d8.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_d3d8.c -o "$out/guest_d3d8.obj"
"$LLD_LINK" "$out/guest_d3d8.obj" "$out/d3d8.lib" \
  /NODEFAULTLIB /ENTRY:guest_d3d8_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1D00000 /OUT:"$out/guest_d3d8.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d8.cpp "$out/libtrt.a" \
  -o "$out/test_d3d8"
"$out/test_d3d8" "$out/guest_d3d8.exe"

# --- 21. openal: the OpenAL 1.1 -> console sink bridge (M7) ------------------
rm -f "$out/openal32.lib"
"$LLD_LINK" /lib /def:src/guest/openal32.def /out:"$out/openal32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_openal.c -o "$out/guest_openal.obj"
"$LLD_LINK" "$out/guest_openal.obj" "$out/openal32.lib" \
  /NODEFAULTLIB /ENTRY:guest_openal_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1E00000 /OUT:"$out/guest_openal.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_openal.cpp "$out/libtrt.a" \
  -o "$out/test_openal"
"$out/test_openal" "$out/guest_openal.exe"

# --- 22. ole: minimal COM boot + OLEAUT (M7) ----------------------------------
rm -f "$out/ole32.lib" "$out/oleaut32.lib"
"$LLD_LINK" /lib /def:src/guest/ole32.def /out:"$out/ole32.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/oleaut32.def /out:"$out/oleaut32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_ole.c -o "$out/guest_ole.obj"
"$LLD_LINK" "$out/guest_ole.obj" "$out/ole32.lib" "$out/oleaut32.lib" \
  /NODEFAULTLIB /ENTRY:guest_ole_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1F00000 /OUT:"$out/guest_ole.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_ole.cpp "$out/libtrt.a" \
  -o "$out/test_ole"
"$out/test_ole" "$out/guest_ole.exe"

# --- 23. shell: SHGetFolderPath / KnownFolderPath / IMalloc (M7) --------------
rm -f "$out/shell32.lib"
"$LLD_LINK" /lib /def:src/guest/shell32.def /out:"$out/shell32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_shell.c -o "$out/guest_shell.obj"
"$LLD_LINK" "$out/guest_shell.obj" "$out/shell32.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_shell_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2000000 /OUT:"$out/guest_shell.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_shell.cpp "$out/libtrt.a" \
  -o "$out/test_shell"
"$out/test_shell" "$out/guest_shell.exe"

# --- 24. shlwapi: Path* helpers (M7) ------------------------------------------
rm -f "$out/shlwapi.lib"
"$LLD_LINK" /lib /def:src/guest/shlwapi.def /out:"$out/shlwapi.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_shlwapi.c -o "$out/guest_shlwapi.obj"
"$LLD_LINK" "$out/guest_shlwapi.obj" "$out/shlwapi.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_shlwapi_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2100000 /OUT:"$out/guest_shlwapi.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_shlwapi.cpp "$out/libtrt.a" \
  -o "$out/test_shlwapi"
"$out/test_shlwapi" "$out/guest_shlwapi.exe"

# --- 25. comctl: common-controls acceptance + ordinal #380 (OrTMM) ----------
rm -f "$out/comctl32.lib"
"$LLD_LINK" /lib /def:src/guest/comctl32.def /out:"$out/comctl32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_comctl.c -o "$out/guest_comctl.obj"
"$LLD_LINK" "$out/guest_comctl.obj" "$out/comctl32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:guest_comctl_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2200000 /OUT:"$out/guest_comctl.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_comctl.cpp "$out/libtrt.a" \
  -o "$out/test_comctl"
"$out/test_comctl" "$out/guest_comctl.exe"

# --- 26. gdi: the GDI object/state lifecycle (fonts, DCs, stock objects) ----
rm -f "$out/gdi32.lib"
"$LLD_LINK" /lib /def:src/guest/gdi32.def /out:"$out/gdi32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_gdi.c -o "$out/guest_gdi.obj"
"$LLD_LINK" "$out/guest_gdi.obj" "$out/gdi32.lib" "$out/user32.lib" \
  /NODEFAULTLIB /ENTRY:guest_gdi_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2300000 /OUT:"$out/guest_gdi.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_gdi.cpp "$out/libtrt.a" \
  -o "$out/test_gdi"
"$out/test_gdi" "$out/guest_gdi.exe"

# --- 26b. desktop-application paint path: window -> WM_PAINT -> GDI raster --
# A desktop-shaped guest (RegisterClassEx -> CreateWindowEx -> message pump)
# must receive the paint cycle, run its own WM_PAINT handler, and its
# PatBlt/TextOutA/Rectangle imports must leave real pixels on the window's
# surface. The test dumps the surface and counts them.
cat > "$out/desktop_u.def" <<'DEOF'
LIBRARY user32
EXPORTS
    RegisterClassExW
    CreateWindowExW
    ShowWindow
    GetMessageW
    TranslateMessage
    DispatchMessageW
    PostQuitMessage
    DefWindowProcW
    BeginPaint
    EndPaint
    ScrollWindowEx
    InvalidateRect
    GetUpdateRect
DEOF
cat > "$out/desktop_g.def" <<'DEOF'
LIBRARY gdi32
EXPORTS
    CreateFontIndirectW
    SelectObject
    PatBlt
    TextOutA
    Rectangle
    RoundRect
    SetTextColor
    SetBkColor
    SetBkMode
    CreateSolidBrush
    GetTextExtentPoint32A
    BeginPath
    EndPath
    FillPath
    SelectClipPath
    SelectClipRgn
DEOF
cat > "$out/desktop_gp.def" <<'DEOF'
LIBRARY gdiplus
EXPORTS
    GdiplusStartup
    GdipCreateFromHDC
    GdipCreateSolidFill
    GdipFillRectangleI
    GdipCreateBitmapFromScan0
    GdipDrawImageI
    GdipScaleTransform
    GdipResetTransform
DEOF
cat > "$out/desktop_msimg.def" <<'DEOF'
LIBRARY msimg32
EXPORTS
    AlphaBlend
    GradientFill
DEOF
rm -f "$out/desktop_u.lib" "$out/desktop_g.lib" "$out/desktop_gp.lib" \
      "$out/desktop_msimg.lib"
"$LLD_LINK" /lib /def:"$out/desktop_u.def" /out:"$out/desktop_u.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/desktop_g.def" /out:"$out/desktop_g.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/desktop_gp.def" /out:"$out/desktop_gp.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:"$out/desktop_msimg.def" /out:"$out/desktop_msimg.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c tests/guest_desktop.c -o "$out/guest_desktop.obj"
"$LLD_LINK" "$out/guest_desktop.obj" "$out/desktop_u.lib" "$out/desktop_g.lib" \
  "$out/desktop_gp.lib" "$out/desktop_msimg.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:desktop_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2C00000 /OUT:"$out/guest_desktop.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_desktop_paint.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_desktop_paint"
"$out/test_desktop_paint" "$out/guest_desktop.exe"

# --- 14. dsound: DirectSound -> console sink (M6 coverage) ------------------
rm -f "$out/dsound.lib"
"$LLD_LINK" /lib /def:src/guest/dsound.def /out:"$out/dsound.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dsound.c -o "$out/guest_dsound.obj"
"$LLD_LINK" "$out/guest_dsound.obj" "$out/dsound.lib" \
  /NODEFAULTLIB /ENTRY:guest_dsound_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1C00000 /OUT:"$out/guest_dsound.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_dsound.cpp "$out/libtrt.a" \
  -o "$out/test_dsound"
"$out/test_dsound" "$out/guest_dsound.exe"

# --- 15. ddraw: DirectDraw -> backend present (M6 coverage) -----------------
rm -f "$out/ddraw.lib"
"$LLD_LINK" /lib /def:src/guest/ddraw.def /out:"$out/ddraw.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_ddraw.c -o "$out/guest_ddraw.obj"
"$LLD_LINK" "$out/guest_ddraw.obj" "$out/ddraw.lib" \
  /NODEFAULTLIB /ENTRY:guest_ddraw_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1D00000 /OUT:"$out/guest_ddraw.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_ddraw.cpp "$out/libtrt.a" \
  -o "$out/test_ddraw"
"$out/test_ddraw" "$out/guest_ddraw.exe"

# --- 16. d3dx9: D3DX math + textures + sprite (M6 coverage) -----------------
rm -f "$out/d3dx9.lib"
"$LLD_LINK" /lib /def:src/guest/d3dx9.def /out:"$out/d3dx9.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_d3dx9.c -o "$out/guest_d3dx9.obj"
"$LLD_LINK" "$out/guest_d3dx9.obj" "$out/d3dx9.lib" \
  /NODEFAULTLIB /ENTRY:guest_d3dx9_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1E00000 /OUT:"$out/guest_d3dx9.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3dx9.cpp "$out/libtrt.a" \
  -o "$out/test_d3dx9"
"$out/test_d3dx9" "$out/guest_d3dx9.exe"

# --- 17. mmedia: msacm32/msvfw32/winspool probes (M6 coverage) --------------
rm -f "$out/msacm.lib" "$out/msvfw.lib" "$out/winspool.lib"
"$LLD_LINK" /lib /def:src/guest/msacm.def /out:"$out/msacm.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/msvfw.def /out:"$out/msvfw.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/winspool.def /out:"$out/winspool.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_mmedia.c -o "$out/guest_mmedia.obj"
"$LLD_LINK" "$out/guest_mmedia.obj" "$out/msacm.lib" "$out/msvfw.lib" "$out/winspool.lib" \
  /NODEFAULTLIB /ENTRY:guest_mmedia_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1F00000 /OUT:"$out/guest_mmedia.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_mmedia.cpp "$out/libtrt.a" \
  -o "$out/test_mmedia"
"$out/test_mmedia" "$out/guest_mmedia.exe"

# --- 18. img: PNG + DDS parsers (M7) -----------------------------------------
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_img.cpp "$out/libtrt.a" \
  -o "$out/test_img"
"$out/test_img"

# --- 19. d3dx: D3DX10/11 texture/info bridge (M7) -----------------------------
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3dx.cpp "$out/libtrt.a" \
  -o "$out/test_d3dx"
"$out/test_d3dx"

# --- 27. STAGE 5 boot-determinism bridges ------------------------------------
rm -f "$out/wininet.lib" "$out/psapi.lib" "$out/iphlpapi.lib" \
      "$out/imm32.lib" "$out/crypt32.lib" "$out/dbghelp.lib"
"$LLD_LINK" /lib /def:src/guest/wininet.def /out:"$out/wininet.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/psapi.def /out:"$out/psapi.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/iphlpapi.def /out:"$out/iphlpapi.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/imm32.def /out:"$out/imm32.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/crypt32.def /out:"$out/crypt32.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/dbghelp.def /out:"$out/dbghelp.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_wininet.c -o "$out/guest_wininet.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_psapi.c -o "$out/guest_psapi.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_iphlpapi.c -o "$out/guest_iphlpapi.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_imm32.c -o "$out/guest_imm32.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_crypt32.c -o "$out/guest_crypt32.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_dbghelp.c -o "$out/guest_dbghelp.obj"
"$LLD_LINK" "$out/guest_wininet.obj" "$out/wininet.lib" \
  /NODEFAULTLIB /ENTRY:guest_wininet_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2400000 /OUT:"$out/guest_wininet.exe"
"$LLD_LINK" "$out/guest_psapi.obj" "$out/psapi.lib" \
  /NODEFAULTLIB /ENTRY:guest_psapi_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2500000 /OUT:"$out/guest_psapi.exe"
"$LLD_LINK" "$out/guest_iphlpapi.obj" "$out/iphlpapi.lib" \
  /NODEFAULTLIB /ENTRY:guest_iphlpapi_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2600000 /OUT:"$out/guest_iphlpapi.exe"
"$LLD_LINK" "$out/guest_imm32.obj" "$out/imm32.lib" \
  /NODEFAULTLIB /ENTRY:guest_imm32_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2700000 /OUT:"$out/guest_imm32.exe"
"$LLD_LINK" "$out/guest_crypt32.obj" "$out/crypt32.lib" \
  /NODEFAULTLIB /ENTRY:guest_crypt32_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2800000 /OUT:"$out/guest_crypt32.exe"
"$LLD_LINK" "$out/guest_dbghelp.obj" "$out/dbghelp.lib" \
  /NODEFAULTLIB /ENTRY:guest_dbghelp_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2900000 /OUT:"$out/guest_dbghelp.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_stage5.cpp "$out/libtrt.a" \
  -o "$out/test_stage5"
S5DIR="$out/stage5-gamedir"; rm -rf "$S5DIR"; mkdir -p "$S5DIR"
"$out/test_stage5" "$out/guest_wininet.exe" 0x3ff
"$out/test_stage5" "$out/guest_psapi.exe" 0x017
"$out/test_stage5" "$out/guest_iphlpapi.exe" 0x03f
"$out/test_stage5" "$out/guest_imm32.exe" 0x03f
"$out/test_stage5" "$out/guest_crypt32.exe" 0x07f
"$out/test_stage5" "$out/guest_dbghelp.exe" 0x00f "$S5DIR"

# --- 28. Console-pack bridge suite (OpenGL32, HID+SetupAPI, XInput, WASAPI, WinRT) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_console_pack.cpp "$out/libtrt.a" \
  -o "$out/test_console_pack"
"$out/test_console_pack"

# --- 29. Wave 1 suite (streaming mmap, dynamic tables, VEH, async IO) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_wave1.cpp "$out/libtrt.a" \
  -o "$out/test_wave1"
"$out/test_wave1"

# --- 30. Wave 2 suite (D3D12 / DXGI 1.6; extended with MojoShader/d3d8to9/gl4es/MF) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_wave2.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_wave2"
"$out/test_wave2"

# --- 31. OpenGL 1.x-4.3 + WGL + GLSL->HLSL translation suite ---
rm -f "$out/opengl32.lib" "$out/gdi32.lib"
"$LLD_LINK" /lib /def:src/guest/opengl32.def /out:"$out/opengl32.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/gdi32.def /out:"$out/gdi32.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_opengl.c -o "$out/guest_opengl.obj"
"$LLD_LINK" "$out/guest_opengl.obj" "$out/opengl32.lib" "$out/gdi32.lib" \
  /NODEFAULTLIB /ENTRY:guest_opengl_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2A00000 /OUT:"$out/guest_opengl.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_opengl.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_opengl"
"$out/test_opengl" "$out/guest_opengl.exe"

# --- 32. Vulkan 1.0-1.3 + WSI + SPIR-V->HLSL translation suite ---
rm -f "$out/vulkan-1.lib"
"$LLD_LINK" /lib /def:src/guest/vulkan1.def /out:"$out/vulkan-1.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_vulkan.c -o "$out/guest_vulkan.obj"
"$LLD_LINK" "$out/guest_vulkan.obj" "$out/vulkan-1.lib" \
  /NODEFAULTLIB /ENTRY:guest_vulkan_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2B00000 /OUT:"$out/guest_vulkan.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_vulkan.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_vulkan"
"$out/test_vulkan" "$out/guest_vulkan.exe"

# --- 33. WebXfer LAN dashboard & streaming folder/zip upload suite ---
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz.c -o "$out/wx_miniz.o"
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz_tdef.c -o "$out/wx_miniz_tdef.o"
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz_tinfl.c -o "$out/wx_miniz_tinfl.o"
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz_zip.c -o "$out/wx_miniz_zip.o"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_webxfer.cpp src/host/webxfer.cpp src/host/settings.cpp \
  "$out/wx_miniz.o" "$out/wx_miniz_tdef.o" "$out/wx_miniz_tinfl.o" "$out/wx_miniz_zip.o" \
  -lpthread -o "$out/test_webxfer"
"$out/test_webxfer"

# --- 34. Host settings: persist, clamp, bridge hooks ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_settings.cpp src/host/settings.cpp \
  -o "$out/test_settings"
"$out/test_settings"

# --- 34b. Settings linkage: the bridges' weak extern "C" hooks must resolve
#          to settings.cpp and forward the stored values (they used to stay
#          null because the getters were namespace-scoped) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_settings_linkage.cpp src/host/settings.cpp \
  -o "$out/test_settings_linkage"
"$out/test_settings_linkage"

# --- 35. PE resource directory: .rsrc walk + FindResource/LoadResource/
#         LockResource/SizeofResource/LoadStringW/EnumResourceNamesW ---
bash tools/build_guest_res.sh
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_res.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_res"
"$out/test_res" "$out/guest_res.exe"

# --- 36. D3D12 TRIANGLE GATE: full render path through a real guest PE ---
# (device -> queue -> alloc -> list -> rootsig -> pso -> resource -> draw ->
#  close -> ExecuteCommandLists -> fence -> swapchain -> Present; the fixture
#  that caught the CreateSwapChainForCoreWindow ABI bug on its first run)
rm -f "$out/d3d12.lib" "$out/dxgi.lib"
"$LLD_LINK" /lib /def:src/guest/d3d12.def /out:"$out/d3d12.lib" /machine:x64 /noentry
"$LLD_LINK" /lib /def:src/guest/dxgi.def /out:"$out/dxgi.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_triangle.c -o "$out/guest_triangle.obj"
"$LLD_LINK" "$out/guest_triangle.obj" "$out/kernel32.lib" "$out/d3d12.lib" \
  "$out/dxgi.lib" \
  /NODEFAULTLIB /ENTRY:triangle_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_triangle.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d12_triangle.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_d3d12_triangle"
"$out/test_d3d12_triangle" "$out/guest_triangle.exe"

# --- 37. ntdll native surface: NtCreateFile/NtReadFile onto VFS, NtCreateEvent/
#         NtWaitForSingleObject/NtSignalAndWaitForSingleObject, RaiseFailFast ---
rm -f "$out/ntdll.lib"
"$LLD_LINK" /lib /def:src/guest/ntdll.def /out:"$out/ntdll.lib" /machine:x64 /noentry
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_ntdll.c -o "$out/guest_ntdll.obj"
"$LLD_LINK" "$out/guest_ntdll.obj" "$out/ntdll.lib" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_ntdll_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x2100000 /OUT:"$out/guest_ntdll.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_ntdll.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_ntdll"
"$out/test_ntdll" "$out/guest_ntdll.exe"

# --- 38. kernel32 gap: job objects, console, Toolhelp Module32, mitigation,
#         stack guarantee + nvapi/nvml/atiadl honest stubs ---
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c tests/guest_kernel32b.c -o "$out/guest_kernel32b.obj"
"$LLD_LINK" "$out/guest_kernel32b.obj" "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_k32b_main /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_kernel32b.exe"
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_kernel32b.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_kernel32b"
"$out/test_kernel32b" "$out/guest_kernel32b.exe"

# --- 39. D3D12 descriptor wave: device slots 17-24 (CBV/SRV/UAV/RTV/DSV/
#         Sampler/CopyDescriptors/CopyDescriptorsSimple), cmdlist binding
#         slots 28/31/32/37/38, copy slots 15/17, PSO legacy+stream (item 29),
#         WriteToSubresource/ReadFromSubresource (items 24/25/30) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d12_desc.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_d3d12_desc"
"$out/test_d3d12_desc"

# --- 40. memory ceiling + MapViewOfFile LRU eviction (item 35) and
#         SetUnhandledExceptionFilter + fatal-path filter wiring (item 46) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_mem_crash.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_mem_crash"
"$out/test_mem_crash"

# --- 41. 5 GB memory-budget governor: one ledger for VirtualAlloc commit,
#         heap and mapped views; over-budget refusals named (1455) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_membudget.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_membudget"
"$out/test_membudget"

# --- 41a. 32 MB ESRAM acceleration & memory tiering for Xbox One / One S ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_esram.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_esram"
"$out/test_esram"

# --- 41a2. last-gen console profile + D3D11 feature-level clamp -----------
# One-family hardware is not reachable from this gate; the family table,
# budgets, ESRAM ledger and the levels swapchain_create() requests are.
clang++ -std=c++20 -O2 -Wall -no-pie -Isrc/host \
  tests/test_console_profile.cpp src/host/console_detect.cpp \
  src/host/settings.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_console_profile"
"$out/test_console_profile"

# --- 41b. file-backed read-only views: MapViewOfFile over a real archive
#          maps the page cache (address space, NOT the 5 GB commit ledger) so
#          the SSD and RAM overlap; write/COW views, views past EOF and
#          TRT_MAP_MODE=copy keep the private-copy path ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_mem_mapfile.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_mem_mapfile"
"$out/test_mem_mapfile"

# --- 42. async file-I/O contract: overlapped read -> GQCS packet, the
#         FILE_SKIP_COMPLETION_PORT_ON_SUCCESS flag honored (no packet),
#         mode-flag validation, FIFO packet order ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_io_async.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_io_async"
"$out/test_io_async"

# --- 43. FMOD bridge: lifecycle + wav decode via VFS + mixer submit + loop
#         feeder + compressed named-silent + handle discipline ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_fmod.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_fmod"
"$out/test_fmod"

# --- 43b. FMOD Studio engine: real bank parsing (RIFF/FEV + FSB5 Vorbis),
#          event graph, VCA/bus state, real PCM through the voice hook
#          (Path B step 4). SKIP when the game banks are absent. ---
MONO_CELESTE_BANKS="$HOME/celeste-xbox/Celeste/Content/FMOD/Desktop"
if [[ -f "$MONO_CELESTE_BANKS/sfx.bank" ]]; then
  clang++ -std=c++20 -O2 -Wall -no-pie \
    tests/test_fmod_studio.cpp "$out/libtrt.a" -lpthread \
    -o "$out/test_fmod_studio"
  "$out/test_fmod_studio"
else
  echo "SKIP fmod/studio: no game banks at $MONO_CELESTE_BANKS"
fi

# --- 43c. FSB5 envelope: a corrupt bank header is refused BY NAME before any
#          allocation or read. This is the door the console heap corruption
#          came through (a bogus frame count went straight into reserve()), so
#          it is tested without a game or a console. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_fsb5_sanity.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_fsb5_sanity"
"$out/test_fsb5_sanity"

# --- 44. loader fuzzer: malformed PEs must fail NAMED, never crash the host
#         (the launch path reads untrusted game files) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_fuzz_loader.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_fuzz_loader"
"$out/test_fuzz_loader"

# --- 45. Mono embedding: Celeste.exe boots to the title screen under the
#         embedded Mono runtime + FNA (Path B step 1), with the REAL FMOD
#         + Steamworks dllmaps (Path B step 4 — the old spike-parity stubs
#         are gone). The door test runs first: it dlopens both .so files
#         and proves the real bridges answer (markers, real bank event
#         counts, bridged Steam ID). SKIP when the game is absent (other
#         machines have no Celeste) or no Xvfb exists. ---
MONO_CELESTE_EXE="$HOME/celeste-xbox/Celeste/Celeste.exe"
if [[ "${SKIP_MONO_CELESTE:-}" == "1" ]]; then
  echo "SKIP mono/celeste: SKIP_MONO_CELESTE=1 (offline MojoShader glsl120 regression; console uses D3D11, unaffected)"
elif [[ -f "$MONO_CELESTE_EXE" ]]; then
  if command -v xvfb-run >/dev/null; then
    bash third_party/build-fna-stack.sh
    # Door test: flat door calls through dlopen (no Mono needed).
    clang++ -std=c++20 -O2 -Wall -no-pie \
      tests/test_mono_doors.cpp -ldl -lpthread \
      -o "$out/test_mono_doors"
    "$out/test_mono_doors" \
      "$here/third_party/mono-bridges/out/libfmod-real.so" \
      "$here/third_party/mono-bridges/out/libcsteamworks-real.so" \
      "$MONO_CELESTE_BANKS"
    # -rdynamic: libFNA3D.so (dlopen'd by Mono's dllmap) references
    # trt_trace (FNA3D_CreateDevice brackets); without it the symbol is
    # not in the binary's .dynsym, the lazy binding fails, and ld.so
    # terminates the process with exit code 127.
    clang++ -std=c++20 -O2 -Wall -no-pie -rdynamic \
      tests/test_mono_celeste.cpp "$out/libtrt.a" -ldl -lpthread \
      -o "$out/test_mono_celeste"
    # FNA's dllmap resolves FNA3D/FAudio by soname; the real bridges +
    # libmono are passed by absolute path and need no LD_LIBRARY_PATH.
    LD_LIBRARY_PATH="$here/third_party/fna3d/out:$here/third_party/faudio/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    xvfb-run -a "$out/test_mono_celeste" "$MONO_CELESTE_EXE" \
      "$here/third_party/fna/out" \
      "$here/third_party/mono-bridges/out/libfmod-real.so" \
      "$here/third_party/mono-bridges/out/libcsteamworks-real.so" \
      "$here/third_party/mono/usr/lib/libmono-2.0.so.1" 90
  else
    echo "SKIP mono/celeste: xvfb-run not found (no virtual display)"
  fi
else
  echo "SKIP mono/celeste: no game at $MONO_CELESTE_EXE"
fi

# --- 46. FNA platform backend: Tiramisu_FNAPlatform initializes headless
#         under embedded Mono and reports a fully populated function
#         table (Path B step 2). The probe needs no game files and no
#         display (headless by design); it runs twice, once per backend
#         selection path (trt_mono_config.fna_platform, then the raw
#         TIRAMISU_FNA_PLATFORM env var). SKIP when mcs or libmono is
#         missing. Section 45 above still proves the SDL2 path. ---
MONO_LIB="$here/third_party/mono/usr/lib/libmono-2.0.so.1"
if command -v mcs >/dev/null && [[ -f "$MONO_LIB" ]]; then
  bash third_party/build-fna-stack.sh
  mcs -out:"$out/probe_fna_platform.exe" \
    -r:"$here/third_party/fna/out/Microsoft.Xna.Framework.dll" \
    tests/probe_fna_platform.cs
  clang++ -std=c++20 -O2 -Wall -no-pie \
    tests/test_fna_platform.cpp "$out/libtrt.a" -ldl -lpthread \
    -o "$out/test_fna_platform"
  # FNA's dllmap resolves FNA3D/FAudio by soname for the log hook; the
  # probe itself never creates a graphics device.
  LD_LIBRARY_PATH="$here/third_party/fna3d/out:$here/third_party/faudio/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$out/test_fna_platform" "$out/probe_fna_platform.exe" \
    "$here/third_party/fna/out" "$MONO_LIB" config
  LD_LIBRARY_PATH="$here/third_party/fna3d/out:$here/third_party/faudio/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$out/test_fna_platform" "$out/probe_fna_platform.exe" \
    "$here/third_party/fna/out" "$MONO_LIB" env
else
  echo "SKIP fna/platform: mcs or libmono missing"
fi

# --- 47. Autonomous Engine Synthesizer & Dynamic API Synthesizer:
#         introspection, engine scoring, profile generation, Steam AppID
#         auto-synthesis, adaptive thunk invocation, arity deduction ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_engine_synthesizer.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_engine_synthesizer"
"$out/test_engine_synthesizer"

# --- 48. Persistent SPIR-V->HLSL cache: codec round-trip (every field the
#         pipeline builder reads), key sensitivity, corrupt-entry rejection,
#         and pruning that never drops the entry the caller is using ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_spirv_cache.cpp src/trt/spirv_cache.cpp \
  -o "$out/test_spirv_cache"
"$out/test_spirv_cache"

# --- 49. Crash report bundle: fault/registers/settings/census/log sections,
#         explicit empty sections, and a bounded report directory ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_crash_report.cpp src/host/crash_report.cpp \
  -o "$out/test_crash_report"
"$out/test_crash_report"

# --- 50. ABI conformance ratchet: every claimed BUILTIN symbol must answer the
#         x86 dispatcher's argument-count question. A symbol without an entry
#         is a silent stack corruption, not a missing feature. The gap set is
#         compared with tests/abi-gaps.baseline and may only shrink. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_conformance.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_conformance"
"$out/test_conformance"

# --- 51. waveOut is a REAL device: enumerable, readable caps, PCM-only open,
#         and write/prepare/position honour the WAVEHDR contract ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_waveout.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_waveout"
"$out/test_waveout"

# --- 52. msacm32 decodes: hand-computed IMA/MS-ADPCM and u-law/A-law vectors,
#         PCM format conversion, and the ACMSTREAMHEADER prepare/convert/close
#         contract (it used to answer NOTPOSSIBLE for everything but PCM) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_acm.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_acm"
"$out/test_acm"

# --- 53. waveIn is a REAL capture device: enumerable, readable caps, PCM-only
#         open, and addBuffer returns a completed WAVEHDR with recorded bytes
#         (it used to report zero devices, which disabled voice capture) ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_wavein.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_wavein"
"$out/test_wavein"

# --- 54. D3D12 root-signature round trip: the serializers must carry the real
#         layout (parameters, descriptor ranges, static samplers, flags), the
#         deserializers must hand it back at 1.0 and 1.1, and
#         CreateRootSignature must be able to see it. They used to return S_OK
#         with an empty container and E_NOTIMPL respectively. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_d3d12_rootsig.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_d3d12_rootsig"
"$out/test_d3d12_rootsig"

# --- 55. Media Foundation objects and the Steam lifecycle are REAL, and the
#         classifier says so exactly where it is true: the attribute store,
#         buffer and sample return what was set on them, a registered Steam
#         callback really runs and really stops when unregistered, the call
#         result table holds and drops entries, and the surfaces that are still
#         empty (SteamMatchmaking/Networking/UserStats, MFShutdown, the source
#         reader, shell32 icon extraction) stay named-silent. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_mf_steam_impl.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_mf_steam_impl"
"$out/test_mf_steam_impl"

# --- 56. The ordinal imports the corpus actually uses bind and are
#         classifiable, and the two mis-mapped oleaut32 ordinals behave:
#         ordinal 12 is VariantChangeType (it used to clear the destination
#         and never convert) and ordinal 15 is SafeArrayCreate (it used to
#         read three scalars where the caller passes one bounds pointer). ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_ordinal_binds.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_ordinal_binds"
"$out/test_ordinal_binds"

# --- 57. Multimedia timers are REAL: timeSetEvent runs a thread that fires the
#         guest callback (or sets the caller's event) at the requested interval,
#         TIME_ONESHOT fires once, TIME_PERIODIC repeats until timeKillEvent
#         joins it, and an unknown id answers TIMERR_NOCANDO. It used to return
#         0 ("no timer exists") for every request. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_winmm_timer.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_winmm_timer"
"$out/test_winmm_timer"

# --- 58. WINMM MIDI virtual synthesizer is REAL: midiOutGetNumDevs reports 1
#         virtual synthesizer device, DevCapsW/A describe the device, and
#         open/close/shortMsg/longMsg/volume control all succeed with standard
#         WinMM semantics. ---
clang++ -std=c++20 -O2 -Wall -no-pie \
  tests/test_winmm_midi.cpp "$out/libtrt.a" -lpthread \
  -o "$out/test_winmm_midi"
"$out/test_winmm_midi"

# --- activation contexts + .lnk reader (mapped from DesktopMode_uwp, MIT) -----
clang++ -std=c++17 -O2 -Wall -no-pie -Isrc/trt tests/test_actctx.cpp \
  -o "$out/test_actctx"
"$out/test_actctx"

# --- version.dll bridge (GetFileVersionInfo*/VerQueryValue*; mapped from
#     DesktopMode_uwp VersionShims, MIT) --------------------------------------
clang++ -std=c++17 -O2 -Wall -no-pie -Isrc/trt tests/test_version.cpp \
  -o "$out/test_version"
"$out/test_version" corpus-local/botw-native/BotW-Native-Setup.exe
