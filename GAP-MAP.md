# GAP-MAP — what is needed to boot real PC games (paste with TIRAMISU-MASTER-PROMPT.md)

> Companion work order for AGY/Gemini sessions. Read `TIRAMISU-MASTER-PROMPT.md` (the
> contract) first; this file is the measured map of everything left.
> Produced 2026-11-14 ~14:15 EST from tool runs on THIS tree + the corpus +
> donor research (links inline). Nothing here is from memory.
>
> **Before trusting any number: re-run the two audit commands in §1. The tree
> moves hourly — two sibling sessions are actively editing it.**

---

## 0. How to work (unchanged from the contract)

- **Measure first** — reproduce with a corpus PE + the depth ladder/audit before
  writing any bridge.
- **Solo gates** — `tools/build-test.sh` etc. run ONE at a time; check for live
  writers first (`find ~/tiramisu -type f -mmin -15 -not -path './.git/*'
  -not -path './tests/out/*'`). Two sessions are live in this repo right now.
- **Fail named, never silent.** STUB hits are work items, not success.
- Do not commit unless the user asks; iteration reports are the deliverable.

---

## 1. Ground truth (measured 2026-11-14)

### 2026-11-24 — Autonomous Universal Engine Synthesizer & Dynamic API Provisioning

Transformed Tiramisu's engine synthesis and launch pipeline so clicking "Play" on a covered x64 Windows title on Xbox Series S/X autonomously inspects the binary, synthesizes the optimal engine profile, binds the covered APIs, provisions translation layers, and blanket-thunks unknown middleware DLLs without per-game bridge-registration hacks. This is generic provisioning, not universal compatibility; the boundaries below still apply:
- **Autonomous Click-to-Play Runtime Provisioning (`src/trt/engine_synthesizer.cpp`, `.h`):** `trt_autonomous_provision_runtime` unconditionally provisions all graphics translation backends (Direct3D 11 passthrough, Direct3D 12 root-signature/descriptor bridge, Vulkan SPIR-V->HLSL translator, OpenGL GLSL->HLSL translator, Direct3D 9/8/DDraw -> D3D11 bridge), all audio middleware (XAudio2, FMOD Studio PCM decoder, Wwise AkSoundEngine, OpenAL 1.1, DirectSound, WASAPI, WinMM), video & storage middleware (Bink Video, CriWare, Miles, Oodle, DirectStorage), and controllers (GameInput, XInput, DirectInput, HID, WinRT, Steamworks).
- **Universal Dynamic API Blanket & Thunk Synthesis (`src/trt/dynamic_api.cpp`, `pe_loader.cpp`, `x86_image.cpp`):** Every uncataloged module in the game's PE import and delay-import tables is automatically registered as `TRT_ROUTE_STUB`. In `pe_loader.cpp` (both PE64 and PE32) and `x86_image.cpp`, missing symbols generate adaptive machine-code thunks with deduced return values (pseudo-handles in `0x77000000` range, S_OK, TRUE, Windows 10 versions, memory basic info, display dimensions) instead of failing at load time with `TRT_ERR_IMPORT`.
- **Universal COM Object Interface Dispatch (`src/trt/dynamic_api.cpp`):** `trt_dynamic_universal_com_object()` provides an `IUnknown` interface instance backed by a 256-method vtable following Microsoft x64 `TRT_GUEST_ABI` (`ms_abi`). `QueryInterface` returns `self` with `S_OK`, `AddRef` returns `2`, `Release` returns `1`, and all other method slots return `S_OK` (0) so virtual interface method calls never crash.
- **OOM-Safe Bounded Memory PE Scanner (`src/trt/engine_synthesizer.cpp`):** Replaced unbounded whole-file allocations in `mapped_file::open` with adaptive bounded allocation (max 64 MB, gracefully dropping to 32MB/16MB if memory is tight), eliminating heap allocation crashes on 500MB+ game executables under Xbox UWP AppContainer memory constraints.
- **Executable Target Redirection & Sibling Alignment (`src/host/main.cpp`):** If the synthesizer selects a shipping binary in a subdirectory (e.g., `Binaries\Win64\Game-Win64-Shipping.exe`), `run_game` redirects `exe_path`, `guest_exe`, `guest_cwd`, and `exe_dir` so sibling DLLs (`UnityPlayer.dll`, `GameAssembly.dll`, `fmod.dll`, `binkw64.dll`) are loaded directly by the guest image loader without path mismatches.
- **Universal Save Directory Provisioning (`src/host/main.cpp`):** Automatically creates `AppData\LocalLow\<Game>`, `AppData\Local\<Game>`, `AppData\Roaming\<Game>`, `Saved Games\<Game>`, and `Documents\My Games\<Game>` for every title launched.
- **Verification Gate (`tests/test_engine_synthesizer.cpp`):** 12 comprehensive test sections covering dynamic API arity/return deduction, machine-code thunk synthesis, honest NULL GetProcAddress contracts, engine signature profiling, PE import scanning, universal COM dispatch, middleware return heuristics, unknown module blanket provisioning, and single-file synthesis (`trt_synthesize_engine_for_file`). All 12 sections passed cleanly alongside `test_engine` and `test_kernel32b`.

**Scope boundary (2026-11-24).** Generic provisioning does not make these work: ARM64 guests; Xbox GDK/GameOS packages; kernel anti-cheat (EasyAntiCheat/BattlEye) by contract; D3D12 real-game execution verified on hardware; Bink video, Wwise audio, and Miles audio; PE32 beyond the experimental x86-32 JIT; and title-specific archive/save/launcher quirks. Wwise, Bink, and Miles are classified `TRT_IMPL_NAMED_SILENT` in coverage, not `REAL`, unless a game-shipped native DLL overrides the fallback.

### 2026-11-24 — Xbox One family detection/profiling scaffolding (offline-tested; no hardware proof)

Detection and host-profile scaffolding landed for Original Xbox One (Durango), Xbox One S (Edmonton), and Xbox One X (Scorpio), alongside the existing Series S/X path. No One-family console has been used for verification:
- **Hardware detection (`src/host/console_detect.h`, `.cpp`):** Best-effort identification via CPUID (AMD Jaguar Family 16h lacking AVX2 vs Zen 2 Family 17h/19h with AVX2), physical memory sizing (8 GB vs 12 GB vs 10 GB vs 16 GB), WinRT `EasClientDeviceInformation`, and a Device Portal tweak file. CPUID/RAM heuristics can misclassify a non-Xbox PC; this is not a hardware proof.
- **Dynamic memory budget (`src/trt/bridge_membudget.cpp`, `trt.h`):** `trt_mem_budget_set_mb` lets the host apply a runtime budget and returns named `ERROR_COMMITMENT_LIMIT` (1455) to guest low-memory paths. The 5120 MB figure is a target, not a measured concession on each console.
- **32 MB ESRAM budget ledger (`src/trt/bridge_esram.h`, `.cpp`):** Tracks candidate render targets/depth surfaces against a 32 MB budget for Durango and Edmonton and reports spillover. It is **accounting only**: UWP D3D11 does not expose explicit ESRAM allocation, so the driver still owns residency. It does not by itself accelerate a resource. Scorpio has no ESRAM ledger.
- **Startup crash guard & early logging (`src/host/main.cpp`):** Vectored exception handler, pre-COM disk logging, D3D11 feature-level fallback, and GameInput-to-XInput fallback are implemented in code; their behavior on One-family OS builds is unverified.
- **Tooling & offline gates (`tools/console_type.py`, `tests/py_tools/test_console_type.py`):** Alias resolution and Python unit tests for the added families.

**Scope boundary (2026-11-24).** Do not describe One/One S/One X as supported or verified until boot, input, D3D11, memory headroom, and the ESRAM ledger are measured on each console. The existing hardware proof remains Series S.

### 2026-11-21 — corpus LOAD gaps closed (gdi32 stretch mode, shell32 icons, ktmw32)

Corpus-smoke was failing three PEs with a named `FAIL IMPORT` gap. Each was a
LOAD-time export, not a runtime option:

- **gdi32!SetStretchBltMode / GetStretchBltMode** — binkw32.dll (RAD Game
  Tools, shipped by many AAA titles) imports SetStretchBltMode. Implemented as
  real per-DC state: default COLORONCOLOR (3), modes 1..4 validated; an invalid
  mode or a non-user32 DC fails visibly (0) instead of pretending. Evidence:
  `tests/test_gdi.cpp` default/round-trip/invalid-mode/garbage-DC checks.
- **shell32!ExtractIconExW/A, ExtractIconW/A** — SDL2 imports ExtractIconExW at
  load time for its window-icon path. Honest answer: 0 icons / NULL with the
  requested file named in the log and both out-params cleared. Evidence:
  `tests/test_shell.cpp` resolve + call checks.
- **ktmw32.dll (new bridge)** — discord_game_sdk.dll imports CreateTransaction
  and CommitTransaction at load time. Transaction handles are REAL kernel32
  event handles (CloseHandle/GetTransactionId behave), CommitTransaction
  answers TRUE with a named log (VFS writes are already applied, so it is
  truthful about durability and explicit about the missing isolation), and
  RollbackTransaction FAILS NAMED (ERROR_CALL_NOT_IMPLEMENTED) because nothing
  can be undone. Evidence: `tests/test_kernel32b.cpp` create/id/commit/
  garbage-handle/rollback checks.

Files: `src/trt/bridge_gdi.cpp`, `src/trt/bridge_shell.cpp`,
`src/trt/bridge_ktmw32.{h,cpp}`, `src/trt/bridge_kernel32.cpp` (registration),
and the manual build lists in `tools/build-host.sh`, `tools/corpus-smoke.sh`,
`tools/corpus-run.sh`.

**Gates on the current tree (2026-11-15, ui+settings wave):**
`build-test.sh` → gate_rc=0, 38× `RESULT: ALL PASS` (incl. the new
funclet-rethrow + boundary-escape fixtures + test_settings) · corpus-run EXE
stage: RACCOIN 1 booted/0 failed (7/7 ladder), Go Next demo.exe 1/0 with
UNITYMAIN2 REACHED, OrTMM 1/0, GoWLauncher 1/0.

**RACCOIN.exe** (real Unity IL2CPP Steam game, now in `corpus-local/raccoin/`,
gitignored) — **BOOTS (2026-11-15, ehfix16)**:

```
CRT-STARTUP OK / WINMAIN ENTERED / WINDOW CREATED (hwnd=0x10101, title 'RACCOIN')
UnityPlayer.dll DllMain -> returned 0x1
GameAssembly.dll (96MB IL2CPP) DllMain -> returned 0x1
D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT
kernel32: RaiseException(0xe06d7363) x2 -> swallowed at host->guest call boundary
                                          (UnityPlayer wrapper-catch emulation)
CORPUS-EXE: 1 booted, 0 failed, 0 skipped (x86)
```

The game then exits through its own clean shutdown path
(`IDXGISwapChain::SetFullscreenState(0) -> S_OK` → `TerminateProcess(0)`).
23→all C++ exceptions handled; the full causal chain (pass-1/pass-2 splice
clobber, stale-record windows, boundary escape semantics) is measured in
`/tmp/opencode/raccoin-ehfix9..19.log` and locked by two new fixtures in
`tests/test_guest_eh.cpp` (funclet rethrow caught; funclet rethrow escaping to
the boundary → synthetic 0 return). The `UNITYMAIN2 CALLED` marker is fixed
(ehfix19): the guest calls UnityMain2 guest→guest — no bridge line ever names
the call — so the harness now patches the EXE's IAT slot with a NAKED asm shim
(flag store + tail-jump; a C shim's prologue corrupts the guest's return
address — measured ehfix18 SIGSEGV). RACCOIN ladder: 7/7 REACHED. Go Next
demo.exe also reports `UNITYMAIN2 CALLED: REACHED (IAT-probed)` — two real
Unity games through the same gate.

### 2026-11-16 — unwinder hardening + livelock cap + native DLL overrides

- **trt_RtlVirtualUnwind hardened** (kernel32 bridge): UnityPlayer's exception
  path (Go Next demo full-corpus run) handed the unwinder unwind metadata that
  is unmapped/corrupt — host SIGSEGV at `trt_RtlVirtualUnwind`, guest fault
  address `image+0x64a4894c`. The unwinder now validates the unwind-info
  pointer against registered guest images (`safe_unwind_info`) and returns a
  clean "unwind failed" verdict (0 = no handler) instead of crashing the
  host. A corrupt .pdata can never again take the runtime down.
- **EH bounce livelock capped** (kernel32 bridge, hw-fault handler): OrTMM
  (first native-C++ corpus entry) spins 14.5M identical (rip,fault) bounces —
  measured, was the cause of every OrTMM wedge/timeout. Per-thread cap at
  100k bounces now aborts with a named verdict ("guest livelocked in EH
  bounce loop").
- **Native DLL override (Proton WINEDLLOVERRIDES=native equivalent) landed**
  (module_table.cpp `try_native_override` + `is_overridable_module`): a
  game-shipped API DLL (dxvk's d3d9.dll, cnc-ddraw, dgVoodoo, ...) shadows the
  builtin bridge for overridable modules. Loader-critical modules are never
  shadowed. dxvk-2.5.3 x64 d3d9.dll verified: maps + seals + DllMain + boots
  as EXE entry (RtlAddFunctionTable registered 8822 funcs). This is the
  wiring path for existing third-party implementations.
- **corpus-run.sh**: per-EXE `timeout -s KILL 60` bound — a guest that never
  reaches WM_QUIT is a NAMED hang, not a gate wedge. Go Next demo.exe added
  to the EXE gate.
- **Two hardening holes found by writing the regression test** (same day):
  1. `ptr_in_guest_image`'s "wide host range" fallback accepted UNMAPPED
     addresses — the exact Go Next shape (metadata at `image+0x64a4894c`,
     fault `0x1a4a4894c`) would pass a range check and still SIGSEGV at the
     first `ui[0]` dereference. Fixed with a kernel-backed readability probe
     (write() to /dev/null returns EFAULT for unmapped pages, no signal;
     probe fd cached, errno saved/restored — unwinding runs inside exception
     dispatch).
  2. The chained-unwind forwarding loop (`UNW_FLAG_CHAININFO`) was unbounded:
     a self-referential chain (`fn->unwind` pointing back at itself) hangs
     the dispatcher forever — same livelock family as the OrTMM spin. Fixed:
     16-hop cap + self-reference check → named verdict, clean fail.
  Regression tests: `tests/test_unwind_safety.cpp` (6 cases: leaf pop,
  garbage fn_entry, unmapped metadata, valid minimal unwind still correct,
  unmapped-in-range probe rejection, chained self-loop). Wiring:
  `tools/build-test.sh` step 8c.
- **OrTMM re-identified**: not native C++ — imports python313.dll (embedded
  CPython 3.13 app). First Python-family corpus entry; explains the EH
  livelock (CPython signal/EH machinery). COMPATIBILITY.md corrected.

### History — the one wall (now broken): our own SEH/unwind implementation.

**Import audit of the full RACCOIN stack** (`tools/audit_boot.py` — approximate;
the C++ resolver in `tests/audit_boot.cpp` is authoritative):

| PE | imports | audit gaps | real gaps after reconciliation |
|---|---|---|---|
| RACCOIN.exe | 72 | 1 | 0 (the 1 = game-dir export artifact) |
| UnityPlayer.dll | 546 | 52 | **dwmapi (2 fns)** — rest are false ws2_32 alarms |
| GameAssembly.dll | 252 | 48 | **GetWriteWatch family** — rest are baselib game-dir artifacts |
| baselib.dll | 129 | 27 | 0 real (ws2 false alarms) |

Why most gaps are false alarms: the Python tool's regex misses the ws2_32
registration style and can't know game-dir DLL exports (`UnityMain2`, the 12
C++-mangled `?Baselib_...` symbols) — those resolve at load from the game dir
(proven v0.29). **Fix the tool: its `__main__` guard is misspelled `'main'`**
so the CLI path never runs; the live session ran it via import (see
`tools/__pycache__`).

**Structural fact that changes the EH plan:**
- NO `__CxxFrameHandler*` / `__C_specific_handler` imports anywhere in the
  stack. The CRT and the C++ EH personality are **statically linked inside
  GameAssembly**. The guest brings its own EH — we must supply only faithful NT
  primitives.
- Every PE imports `KERNEL32!RtlVirtualUnwind` / `RtlUnwindEx` / `RtlUnwind`
  directly. All three ARE registered in both the kernel32 and ntdll bridge
  tables — but "registered" ≠ "faithful". That is the work.
- **IL2CPP raises EVERY managed exception as a C++ exception**
  (`il2cpp_codegen_raise_exception` → `Il2CppExceptionWrapper` — Unity's own
  IL2CPP internals doc). So C++ EH is load-bearing for gameplay, not an edge
  case: every `try/catch` in C# game code goes through our unwinder.

**In flight RIGHT NOW (orchestrated parallel agents, 2026-11-15 — do not duplicate):**
- (none — wave 5 (items 24/25/30/29/35/46-remainder) is complete and verified;
  M0 passed 2026-11-19; next is M1 — God of War's interactive front end plus
  RACCOIN soak/frame-time, or the command-list recording surface (item 27
  remainder) on request.)

**M0 PLAYABLE (2026-11-19):** RACCOIN reaches its real main menu and plays a
run on console — coin physics, HUD, score/round progression — captured in
`docs/proof/raccoin-main-menu.png`, `raccoin-gameplay.png` and the 7.2 s
`raccoin-gameplay.mp4`. God of War boots and renders the opening forest across
four independent boots (`docs/proof/gow-forest-01..04.webp`). Next gate:
God of War's interactive front-end/menu layer and sustained gameplay; RACCOIN soak
and frame-time capture.

**M0 CONSOLE FINDINGS (first real hardware run, 2026-11-17):**
- The MSIX **installed, activated, and ran** on the Xbox Series S (OS 26100.9662):
  the dashboard served on LAN, the capability probe ran (RWX denied / W->RX
  ALLOWED — mono viable; dstorage MISSING; XAudio2 2.8 OK; gamepads=0).
- **UnityPlayer.dll relocated on hardware**: 70,501 relocations fixed at a
  non-preferred base — the reloc machinery is console-proven.
- **RACCOIN boot reached Unity's GC**, then died: "Fatal error in GC:
  SuspendThread loop failed" -> int3 (0x80000003). ROOT CAUSE: the console
  build's trt_thread_handle_for_current() returned 0 under _WIN32, so
  DuplicateHandle(GetCurrentThread()) handed the GC the -2 pseudo-handle
  itself; the collector's SuspendThread loop then failed forever. FIXED:
  real thread adoption (TID match, real DuplicateHandle) + a Boehm-style
  safe-suspend loop (park -> verify the registry mutex is takeable ->
  resume+retry, closing the suspend-while-holding-our-lock deadlock).
- **Console activation blocker (0x80070002 "Please try again")**: the pack
  carried the full 16-asset set (per-scale Square44x44 subdirs,
  Square310x310, Wide310x150) — the console REFUSES activation of packages
  with asset variants beyond the four-asset envelope. pack.sh now strips
  to the proven envelope before packing.
- **Selftest rc=IMPORT on console**: guest_hello imports TWO symbols
  (TiramisuHostLogU32 AND TiramisuHostQueryBuild); only tests registered
  them. main.cpp now registers both (build number 0.2.0).
- Crash evidence path that works: log_line fflush's to LocalState\tiramisu.log
  every line; /api/logs serves the DISK tail (survives the crash).

**M0 CONSOLE FINDINGS (first real hardware run, 2026-11-17):**
- The MSIX **installed, activated, and ran** on the Xbox Series S (OS 26100.9662):
  the dashboard served on LAN, the capability probe ran (RWX denied / W->RX
  ALLOWED — mono viable; dstorage MISSING; XAudio2 2.8 OK; gamepads=0).
- **UnityPlayer.dll relocated on hardware**: 70,501 relocations fixed at a
  non-preferred base — the reloc machinery is console-proven.
- **RACCOIN boot reached Unity's GC**, then died: "Fatal error in GC:
  SuspendThread loop failed" -> int3 (0x80000003). ROOT CAUSE: the console
  build's trt_thread_handle_for_current() returned 0 under _WIN32, so
  DuplicateHandle(GetCurrentThread()) handed the GC the -2 pseudo-handle
  itself; the collector's SuspendThread loop then failed forever. FIXED:
  real thread adoption (TID match, real DuplicateHandle) + a Boehm-style
  safe-suspend loop (park -> verify the registry mutex is takeable ->
  resume+retry, closing the suspend-while-holding-our-lock deadlock).
- **Console activation blocker (0x80070002 "Please try again")**: the pack
  carried the full 16-asset set (per-scale Square44x44 subdirs,
  Square310x310, Wide310x150) — the console REFUSES activation of packages
  with asset variants beyond the four-asset envelope. pack.sh now strips
  to the proven envelope before packing.
- **Selftest rc=IMPORT on console**: guest_hello imports TWO symbols
  (TiramisuHostLogU32 AND TiramisuHostQueryBuild); only tests registered
  them. main.cpp now registers both (build number 0.2.0).
- Crash evidence path that works: log_line fflush's to LocalState\tiramisu.log
  every line; /api/logs serves the DISK tail (survives the crash).

**OPEN BUG (named, measured): missed relocation at non-preferred bases.**
The TSan gate SEGV'd inside `name_eq` (module_table.cpp:29) while resolving
a guest import whose module-name pointer landed OUTSIDE the relocated image
(rsi=0x100004200c20, image size 0x352000). Signature: a guest loaded at a
non-preferred base takes a fault through a pointer the reloc pass did not
fix. The same signature appeared in RACCOIN's earlier flake (plugin load at
a relocated base -> UnityPlayer NULL-call). The reloc machinery itself is
proven (guest_hello_reloc verifies values), so the suspect is RELOC
COVERAGE for a specific guest shape — e.g. lld-link emitting reloc blocks
the loader's walk skips, or the import-directory/name RVAs being treated as
absolute. Reproduce: run test_ntdll under tools/tsan-gate.sh (the guest maps
at a kernel-chosen base there). This is the LAST loader-class blocker for
guaranteeing "boots at ANY base".

**LANDED (certainty layer, 2026-11-17):**
- **tools/sanitize-gate.sh** — the whole runtime under ASan+UBSan
  (recover=false), every guest-executing suite with its exact guest args
  (mirrored from build-test.sh): **35 suites ALL CLEAN**.
- **tools/tsan-gate.sh** — same suites under ThreadSanitizer: caught a REAL
  data race in bridge_fmod (channel state written by PlaySound, read by the
  feeder thread) — FIXED with a channel mutex; caught a mixed-atomicity
  race in WaitOnAddress (plain read vs the guest's Interlocked store) —
  FIXED with acquire-atomic loads (also more correct Win32 semantics).
- **tests/test_fuzz_loader (suite 44)** — 4000 random PE mutations + 9
  structural attacks; the host survives every input, failures named.
- **REAL UB fixed**: bridge_d3dx9 misaligned u32/u16 loads from guest file
  data (memcpy reads now — the old code was UB on strict platforms).
- **ASan environment fixes**: guest-image unpoison at map time (ASan cannot
  see mmap-backed guest memory); test allocators avoid ASan's shadow gap
  (0x140000000 preferred bases are INSIDE it); function-type CFI disabled
  in the gates (the guest<->host boundary is an intentional dual-ABI
  crossing — impossible on console where one ABI exists).

**LANDED (FMOD audio bridge — new game-class unlock, 2026-11-17 early):**
- **NEW bridge_fmod.cpp** — fmodex.dll / fmodex64.dll / fmod.dll /
  fmodstudio.dll / fmodstudioL.dll registered as one BUILTIN table (28
  functions): the FMOD Ex and Studio C exports share the lifecycle names,
  and the C++ API is inline wrappers over them, so one table covers both
  families. Unlocks the FMOD game class (thousands of indie/AA titles)
  which previously died at audio init.
- **Real audio path**: System lifecycle (Create/Init/Update/Release) ->
  CreateSound/CreateStream resolve guest paths through the VFS -> WAV
  (RIFF PCM 8/16/24/32) decodes for real -> trt_audio_submit_voice to the
  host mixer. OGG payloads decode via **vendored stb_vorbis 1.24 (public
  domain, src/trt/vendor_stb_vorbis.c, STB_VORBIS_NO_STDIO)** — the biggest
  FMOD payload class is now audible, not silent.
- **Looping**: FMOD_LOOP_NORMAL sounds are fed by a bridge scheduler thread
  that re-pushes decoded PCM until Channel_Stop (the mixer FIFO absorbs it).
- **Named-silent path**: MP3 / FSB banks / ADPCM have no decoder yet — the
  sound handle, channel control, and state machine are real; audio is
  SILENT with a named per-sound log ("decoder pending"). Never a fake
  success.
- **Ownership discipline**: Sound payload pointers may point INSIDE the
  malloc'd file buffer — fmod_sound carries alloc_base (the owned base) and
  pcm (possibly interior); Release frees the base only. Measured: freeing
  the interior pointer heap-corrupted and SIGSEGV'd the host (gdb:
  arena_for_chunk) before the fix.
- **Gate: test_fmod (suite 43)** — 21 checks: lifecycle, WAV decode via VFS
  staging (CreateFileW write -> bridge read), mixer capture hook verifying
  the exact PCM payload (22050 frames 44.1k stereo s16), volume, stop,
  loop feeder, ogg named-silent, handle discipline (released handle
  rejected, garbage handle -> FMOD_ERR_INVALID_HANDLE). ALL PASS.
- **Bridge-surface audit correction**: the old matrix listed
  Interlocked*/ConditionVariables/RawInput as gaps — ALL PRESENT (stale
  matrix). The memory bridge already implements the W^X two-phase JIT
  fallback (VirtualProtect RWX denied -> arm RW -> flip RX on
  FlushInstructionCache), which is the mono-JIT enabler; Unity mono games
  are architecturally viable, not blocked.

**LANDED (RACCOIN playable-path proof + watchdog fix, 2026-11-16 night):**
- **The render loop is PROVEN offline.** The mock-d3d11 harness (COM GPU
  hooks injected so Unity's D3D11CreateDevice succeeds) now counts every
  vtbl-slot hit. Measured on RACCOIN, three consecutive stable runs:
  **2.7-2.8 MILLION device-context calls and ~268k DrawIndexed in the 10s
  window** — Unity's frame loop spins end-to-end through the D3D11 surface
  (per-frame RSSetState / IASetVertexBuffers / IASetIndexBuffer /
  VSSetShaderResources / VSSetSamplers + per-draw calls, 88k swapchain
  calls), then a clean ExitProcess(0). The mock histogram is printed in the
  boot report (per-slot template thunks — slot positions are MEASURED, never
  guessed; named handlers overwrite after the counting fill).
- **NEW GATE: mock-render** (corpus-run.sh) — boots RACCOIN with
  --mock-d3d11 and enforces ctx_calls >= 100000. A regression that starves
  the pump or breaks device init now fails named instead of silently.
- **Watchdog root-cause fix (the intermittent RACCOIN boot failure):** the
  old watchdog fired WM_CLOSE after 10s WALL TIME. Under desktop load
  RACCOIN's init (il2cpp metadata + 40-thread storm) exceeds 10s, the
  watchdog fired MID-INITIALIZATION, the shutdown raced the plugin load
  (SetDllDirectoryW(Plugins/x86_64) -> relocated module load), and
  UnityPlayer NULL-called during teardown (SIGSEGV at 0x0, UnityPlayer
  +0xa37d63, intermittent with machine load — 3 clean runs then 2 failures
  before the fix). The watchdog now measures 10s of ENGINE SILENCE
  (observe() stamps every trt log line as activity) with a 120s hard wall.
  Three consecutive stability runs after the fix: RC=0, zero failures.
- **RACCOIN ladder is now 7/7 with a device present** (mock hooks): the
  D3D11 DEVICE ACQUIRED marker that was console-only offline now flips in
  the harness — the guest-side chain (Unity -> device -> swapchain ->
  render loop) is fully proven; only the console's REAL device execution
  remains hardware-verifiable (M0).

**LANDED (BotW-Native corpus wave + UCRT real-answer surface, 2026-11-16 evening, all gates green):**
- **BotW-Native v0.1.2 added to the corpus** (corpus-local/botw-native/): the
  static recomp of BOTW Wii U v1.5.0 — BotWRecomp.exe (497 MB, the actual
  game), 3 launchers, and the full MinGW/SDL3/OpenSSL/glslang/SPIRV DLL set.
  Vulkan-only renderer rides the PROVEN ICD path (the same one Godot booted
  through). Bridge gaps closed to make the whole set load + init clean:
  gdi32 GetICMProfileW/GetPixelFormat/SetDCBrushColor, crypt32
  CertFindCertificateInStore, imm32 ImmGetIMEFileNameA, setupapi
  CM_Locate_DevNodeA/CM_Get_Parent/CM_Get_Device_IDA, ws2_32
  getservbyname/getservbyport/WSADuplicateSocketA/WSAGetOverlappedResult/
  WSAStringToAddressA, shell32 DragFinish/SHBrowseForFolderW/
  SHGetPathFromIDListW, comctl32 SetWindowSubclass/RemoveWindowSubclass/
  DefSubclassProc (recorded chain, base-wndproc forward), and a NEW
  bridge_gdiplus.cpp (32 GDI+ flat-API functions; object lifecycle real,
  draw sites recorded — launcher UI surface).
- **THE BIG ONE — UCRT blanket-0 replaced with real answers**
  (tests/corpus_ucrt_wrappers.c): the corpus runner's SYSTEM bind answered
  every whitelisted-but-unnamed symbol with a 0-returning stub. MinGW DLLs'
  DllMain CRT init calls ucrtbase!calloc first thing -> NULL -> store fault
  -> EH bounce livelock -> abort (measured: libstdc++-6.dll signal 6, 100k
  identical bounces at libwinpthread +0x6d0c, rax=0). 115 ms_abi wrappers
  over host libc now answer the pure-computation UCRT surface (heap, mem/
  str/wcs, ctype, math, qsort with a Win64-ABI compare callback, _initterm
  family, _errno family, rand_s, exit/_exit through the guest exit trap).
  Deliberately still blanket-0 (named): printf family (MS va_list ABI),
  Win32-handle file family (needs VFS), _gmtime64/strftime (struct tm
  layout), localeconv (lconv layout), _beginthreadex, setjmp/longjmp.
  Result: EVERY corpus DLL now runs DllMain(PROCESS_ATTACH) clean —
  libwinpthread, libgcc, libstdc++, SDL3, libcrypto, glslang, SPIRV-Tools,
  zlib1, libzstd, libiconv + RACCOIN's UnityPlayer/GameAssembly/baselib.
- **Gate plumbing fixes:** corpus-smoke.sh/corpus-run.sh explicit compile
  lists were missing bridge_gdiplus/bridge_nvapi/bridge_res (added);
  build-test.sh parallelism is now TRT_JOBS-overridable (the N97 OOM-killed
  lld-link mid-gate under desktop load — deterministic silent death after
  test_d3d12_triangle, cost three false gate failures to pin).
- **Gate status:** build-test RC=0 (48x ALL PASS), corpus-smoke 29 ok /
  0 failed / 3 skipped, corpus-run RC=0 with 3/3 EXE boots. RACCOIN ladder
  6/7 offline — the missing marker (D3D11 DEVICE ACQUIRED) is host-device
  presence: the console host's publish_gpu_hooks() injects the real D3D11
  device + CoreWindow swapchain, so that marker flips on Xbox (M0).

**LANDED (wave-6 completion + mmap streaming I/O, 2026-11-16 later, all four gates green):**
- **Suite 39 (test_d3d12_desc) COMPLETED** — the half-merged block finished:
  `queue_desc_pod` defined (16B, bridge-layout), the `res_desc_pod` typo ->
  the existing `resource_desc_pod`, UPLOAD heap-props + 64 KiB BUFFER desc for
  CreateCommittedResource2 (slot 69), a REAL root signature created through
  device slot 16 for the root-signature recording checks, and
  `clvt = vtbl_of(cl)`. 77 checks ALL PASS — the D3D12 command-list recording
  surface (topology/viewports/scissors/PSO/both root sigs/VB binds/RTVs/
  queries/bundle-reject/ClearState-reset) is now gate-enforced, no stash
  gymnastics needed for full-gate runs.
- **mmap fast path for large positional reads** (`file_pread_mmap_locked`,
  shared by `trt_file_pread` — the MapViewOfFile window population — and the
  overlapped ReadFile branch): reads >= `TRT_IO_MMAP_MIN` (default 1 MiB, 0
  disables) map the page range and copy from the page cache — no stdio
  buffering, no seek dance. EOF clamp + past-EOF zero-byte read preserve the
  fread path's exact contract (ERROR_HANDLE_EOF shape). Console keeps its
  native path. Suite 42 block E: 4 MiB pattern file, 2 MiB read at offset
  1 MiB (payload verified), EOF clamp, past-EOF — ALL PASS.
- **Gate status 2026-11-16 final:** build-test 42× ALL PASS (no stash),
  build-host RC=0, corpus-smoke 14/0/3, corpus-run 3/3 EXE boots, MSIX
  repacked + signed (out/Tiramisu.msix).

**LANDED (5 GB budget governor + async-I/O contract, 2026-11-16, all gates green):**
- **`bridge_membudget.cpp` NEW — the 5 GB memory-budget governor.** ONE ledger
  for every host-side byte that stands in for guest commit:
  VirtualAlloc commit/decommit/release (extent-summed at RELEASE),
  HeapAlloc/HeapFree/HeapReAlloc, MapViewOfFile/UnmapViewOfFile + both LRU
  eviction paths. Budget = `TRT_MEM_BUDGET_MB` (default 5120 = the console
  Game-package allocation, clamp 1..16384, parsed once per process). Over-budget
  commits/maps refuse with a named rate-limited log + ERROR_COMMITMENT_LIMIT
  (1455) so the guest's own low-memory paths run instead of a platform kill;
  80%/90% soft logs fire once; `trt_mem_budget_report()` renders the crash-report
  line. Evidence: NEW `tests/test_membudget.cpp` (suite 41) — ledger arithmetic,
  peak, mapped subclass, over-discharge clamp, plus a re-exec'd 16 MB-budget
  integration probe (VirtualAlloc refuse/commit/decommit/release, heap
  refuse/free/retry, map refuse/unmap) ALL PASS.
- **Async file-I/O contract locked (NEW `tests/test_io_async.cpp`, suite 42).**
  Overlapped ReadFile at a 64-bit offset → OVERLAPPED.Internal/InternalHigh
  filled, hEvent signaled, payload from the right offset, and GQCS dequeues the
  packet with the association's key + the caller's OVERLAPPED pointer; FIFO
  packet order across multiple reads.
- **`SetFileCompletionNotificationModes` became REAL (was an ok-stub — engines
  that set FILE_SKIP_COMPLETION_PORT_ON_SUCCESS got phantom double-completions:
  event AND packet).** Modes stored per file handle (`file_rec.io_modes`),
  `trt_iocp_notify_io` consults bit 0 and suppresses the success packet (the
  event still signals); invalid flag combinations fail ERROR_INVALID_PARAMETER,
  unknown handles ERROR_INVALID_HANDLE. test_ntdll's stub-era check updated to
  the real contract (real handle accepts / unknown → 6 / 0x4 → 87).
- **Wiring trap fixed for future bridge files:** corpus-smoke.sh and
  corpus-run.sh link test_corpus/test_corpus_run from INDIVIDUAL sources (not
  libtrt.a) and build-host.sh runs build-test.sh as its "offline loader test"
  stage — a new `src/trt/*.cpp` must be added to FOUR script lists
  (build-test glob is automatic; build-host, corpus-smoke, corpus-run are
  manual) or the gates fail with confusing undefined references.
- **UWP host build fixed** (3da09af "community UX" had committed host code that
  never compiled under clang-cl: the splice ate `draw_tabs()`'s header in
  ui.cpp leaving its body at file scope, and main.cpp declared `qm_toggle`
  inside the `if` but used it in the `else`). Both mechanical-fixed; gate 2 now
  reaches the host compile for the first time since the dashboard wave.
- **Gate status 2026-11-16:** build-test.sh 42× ALL PASS (suites 1-42 incl. the
  two new ones); corpus-smoke 14 ok / 0 failed / 3 skipped; corpus-run 3/3 EXE
  boots (Go Next, OrTMM, RACCOIN rc=0 through its own shutdown chain) with the
  governor live — zero BUDGET REFUSAL lines across all boots.

**LANDED (boot-depth wave, 2026-11-15, worktree `work/boot-ultrapool` @ b6a7b51,
all verified offline; gate rc=0, 42× ALL PASS, corpus 8/8 load, 5/5 run-depth):**
- **Ultra Pool (Godot 4.3 + GodotSteam) — FULL BOOT LIFECYCLE**: CRT → WinMain →
  window ('Ultrapool') → Vulkan Forward Mobile on the Tiramisu ICD → scene load
  (achievements/save/daily state) → pump → watchdog WM_CLOSE → Godot's own
  quit chain (`close_requested` → `SceneTree::_quit`) → ExitProcess(0). The
  wedge chain that stopped it, each fixed to real Win32 semantics:
  CV lost-wakeup (release→wait-registration serialized under a wake mutex,
  table 64→512 slots), WASAPI pacing (GetCurrentPadding timestamp-decay model
  — Godot paces solely on `buffer_size - padding`; the zero-collapse caused a
  97%-CPU mix storm holding the CS the main thread needed), MS_VC_EXCEPTION
  swallowed like the OS does (no debugger), EH dispositions + faithful
  `trt_C_specific_handler`, dwm `TRT_GUEST_ABI`, AttachConsole=FALSE +
  synthetic stdio (real pseudo-handles → log ring), Vulkan limits/features
  from real `vulkan_core.h` types, watchdog posts WM_CLOSE (Godot never reads
  WM_QUIT — measured against 4.3 sources). Regression-locked in
  `tests/test_wasapi_pacing.cpp` (padding contract + CV wake atomicity +
  huge-malloc refusal).
- **RACCOIN re-measured under the new sync surface**: with `--mock-d3d11`
  (the documented config) the 8/8 ladder holds AND the game now runs healthy
  for the full 10s until the watchdog WM_CLOSE, then exits through its own
  shutdown chain (`SetFullscreenState(0)` → TerminateProcess) — previously it
  never survived a watchdog interval. Plain `--exe` (stricter, real GPU path)
  reaches Unity init + pump, then dies in the Unity CrashReporter C++
  exception path (throw site escapes to a host frame; boundary swallow →
  WM_QUIT) — that escape attribution is the next named gap, not a sync bug.
- **Import-gap closures (fail-visible honest stubs)**: dbghelp
  `SymGetModuleBase64`/`SymFunctionTableAccess64`/`StackWalk64` (0/NULL/FALSE —
  no module table on console); winmm `waveOutMessage`/`Pause`/`Restart`/
  `BreakLoop`/`GetErrorText` + `waveInMessage` (MMSYSERR_BADDEVICEID).
  steam_api64.dll now maps + seals (was FAIL IMPORT) → corpus load 8/8.
- **`GetStdHandle` synthetic-stdio contract locked** in the guest_engine
  fixture; `corpus-run.sh` no longer crashes when the D3D12 triangle fixture
  is not yet built (named SKIP; ordering fix).

- **UWP host build re-verified (`build-host.sh` rc=0)**: the Linux-only CS-stall
  probe (`syscall(SYS_gettid)`, `::write(2,...)`) was breaking the clang-cl
  `--uwp` cross-compile (no POSIX fd 2 / sys/syscall.h in WINAPI_FAMILY_APP).
  Guarded behind `__linux__` so the probe still names stalls on the dev host
  while the UWP host build stays clean. Duplicated lock block in
  `trt_EnterCriticalSection` also collapsed. MSIX produced.
- **DirectInput8 COM personality** closes the `REGDB_E_CLASSNOTREG` gap:
  `CoCreateInstance(CLSID_DirectInput8)` now hands out the same object
  `DirectInput8Create` serves (A-twin -> W vtable; unknown IID -> E_NOINTERFACE
  honestly). Verified S_OK + E_NOINTERFACE, both named.
- **Items 24/25/30 — D3D12 descriptor wave** (`bridge_d3d12.cpp`): the silent
  `dev_view_noop` at device slots 17-24 is GONE. Real implementation:
  per-heap typed record store (`d3d12_view_record`, kind/dim/format/mips/
  slices/gpu_va/resource), live-heap registry with CPU-handle resolution
  (handle = cpu_base + idx*increment, boundary + range checked), CreateCBV/
  SRV/UAV/RTV/DSV/Sampler writing validated records (RTV/DSV require a live
  resource; CBV requires a nonzero GPU VA; sampler Filter/AddressU/V/W/
  MaxAnisotropy/ComparisonFunc range-checked; SRV/UAV parse both the BUFFER
  and TEX2D union arms; null-descriptor init requires BOTH pResource and
  pDesc null — (null, non-null) now fails VISIBLY in SRV and UAV),
  CopyDescriptors/CopyDescriptorsSimple with per-range expansion, cross-heap
  copies and bounds-checked fail-visible rejection. Command-list side:
  SetDescriptorHeaps (replacing, like the real API), Set[Compute|Graphics]
  RootDescriptorTable and RootConstantBufferView recorded per slot with
  soft-resolve WARN-once for unresolvable GPU handles (matching real
  draw-time faulting), CopyBufferRegion/CopyResource recorded.
  **Two real bugs the new suite caught and the orchestrator fixed:**
  1. `k_descheap` IID had `...90f7-12b7...` instead of `...90f7-127b...`
     (transposed byte pair) — every guest QI for ID3D12DescriptorHeap got
     E_NOINTERFACE. Fixed; full 24-IID audit of the bridge against the SDK
     header (xwin d3d12.h + mingw d3d12.h) now passes 24/24.
  2. WriteToSubresource/ReadFromSubresource packed box content at backing
     offset 0, ignoring box.left — for buffers (the standard upload path)
     box.left is a BYTE offset. Both now honor D3D12_BOX semantics (buffer:
     left = byte offset; texture: texel grid on the 4-byte-per-texel backing,
     top/front select start row/slice), bounds-checked fail-visible.
  Also: PSO creation is real (item 29) — CreateGraphicsPipelineState/
  CreateComputePipelineState validate the legacy descs (root-sig liveness,
  paired shader bytecode) and CreatePipelineState (slot 47) walks the
  D3D12_PIPELINE_STATE_STREAM subobject stream (16-byte headers, types
  0..28, payload-overrun checks, ROOT_SIGNATURE/VS/PS/CS recorded; garbage
  type or overrun -> E_INVALIDARG + named log). Struct mirrors pinned by
  static_assert vs the SDK (GPSD 656, CPSD 56, SRV/UAV 40, RTV/DSV 24,
  SAMPLER 52, stream desc 16 — SDK-verbatim SizeInBytes-first layout
  verified by header measurement).
  Evidence: NEW `tests/test_d3d12_desc.cpp` 59/59 ALL PASS (orchestrator-
  written and -run: handle arithmetic, all six view kinds, 8 fail-visible
  rejections, range + cross-heap + simple copies, cmdlist binding/copy
  recording, PSO legacy + stream + 2 garbage rejections, Write/Read
  subresource round-trip visible through Map, QI on heap/PSO/resource);
  regressions test_d3d12, test_d3d12_triangle, test_wave2 ALL PASS;
  RACCOIN `--exe --mock-d3d11` 1 booted / 0 failed (all relinked against
  the updated libtrt.a by the orchestrator).
- **Item 35 + item 46 remainder — memory ceiling + crash filter**
  (`bridge_mem.cpp`, `bridge_kernel32.cpp`): every mapped view tracked in a
  live-view registry; ceiling 64 live views (env `TRT_MAX_MAPS`, clamped
  8..4096); over-ceiling maps evict the LEAST-RECENTLY-USED view with a
  named log line, use-after-evict fails visibly; when nothing is evictable
  the map fails honestly (NULL + last-error + named log). LRU stamps
  updated on every map/unmap touch. SetUnhandledExceptionFilter/
  UnhandledExceptionFilter added to the kernel32 table (filter pointer
  stored, named registration log) and wired into the runtime's fatal
  exception path: the stored filter is invoked with the exception record,
  verdict logged (`no-filter`/`handled`/`unhandled`), RaiseFailFastException
  semantics preserved, MiniDumpWriteDump's text crash report remains
  available on the fatal path.
  Evidence: NEW `tests/test_mem_crash.cpp` ALL PASS (70-map eviction
  sequence, TRT_MAX_MAPS clamp incl. 8192->4096 probe, unevictable honest
  failure, filter registration + invocation with correct record, no-filter
  path); regressions test_kernel32b 17/17, test_ntdll 34/34, test_ole 16/16,
  test_engine ALL PASS (relinked + rerun by the orchestrator).
- **Gate wiring**: `tools/build-test.sh` suites 39 (test_d3d12_desc) and 40
  (test_mem_crash) appended after suite 38.

**LANDED (wave 4, 2026-11-15, all verified offline):**
- **Item 16 / 1K — ntdll native surface** (`bridge_ntdll.cpp`, `bridge_ole.cpp`):
  NtCreateFile (OBJECT_ATTRIBUTES/UNICODE_STRING parse, `\??\` strip,
  RootDirectory-relative, 6 dispositions -> kernel32 mapping, full
  dos_to_ntstatus) + NtReadFile routed onto the same VFS the kernel32 file
  surface uses; NtCreateEvent/NtSetEvent/NtWaitForSingleObject/
  NtSignalAndWaitForSingleObject on the kernel32 event primitives;
  RaiseFailFastException honors fail-fast (logs, then terminates — tested
  by writing DONE to results.txt first); CoWaitForMultipleHandles STA wait
  (S_OK signaled / RPC_S_CALLPENDING timeout / E_HANDLE).
  Evidence: `test_ntdll` 34/34, `test_ole` 16/16 (both rerun by the
  orchestrator, not just the agent).
- **Item 12 remainder — kernel32 gap** (`bridge_kernel32_gap.cpp`,
  `bridge_kernel32.cpp`, `bridge_nvapi.cpp` NEW): job objects
  (CreateJobObjectW/A, SetInformationJobObject, AssignProcessToJobObject,
  TerminateJobObject, OpenJobObject honest-fail, IsProcessInJob),
  AllocConsole/FreeConsole/AttachConsole, Toolhelp module walk UPGRADED
  from honest-fail to a real image-registry-backed MODULEENTRY32W walk,
  Get/SetProcessMitigationPolicy, Get/SetThreadStackGuarantee;
  `bridge_nvapi.cpp` honest not-supported stubs for nvapi/nvml/atiadlxx
  (NVAPI_NVIDIA_NOT_FOUND / NVML_ERROR_NOT_SUPPORTED / ADL_ERR_NOT_SUPPORTED,
  log-once), wired into host `main.cpp` boot.
  Evidence: `test_kernel32b` 17/17 (rerun by the orchestrator).
- **BUG-6 remainder — version-aware QI everywhere** (`bridge_d3d12.cpp`):
  every D3D12 object type now answers QI from a verified IID chain
  (IUnknown + Object + DeviceChild + Pageable + own versions) and fails
  VISIBLY with E_NOINTERFACE for anything beyond its vtbl — previously
  `base_qi_self` accepted ANY IID, handing a 60-slot vtbl to a game that
  QIed GCL4 (77 slots) = call off the end. Command-list vtbl extended
  60 -> 84 slots (GCL1..9; 24 ABI-correct logged no-op stubs, arities from
  the SDK header), fence 11 -> 12 with a REAL Fence1::GetCreationFlags
  (CreateFence now records flags), and the FABRICATED Object GUID
  (c7e2816f... — not in the SDK header at all) replaced by the real
  c4fec28f. All IIDs verified verbatim against the xwin SDK d3d12.h.
  Evidence: `test_d3d12` ALL PASS with 15 new versioned-QI checks (GCL..GCL9
  QI, cross-type E_NOINTERFACE, Fence1, Object-IID), TRIANGLE GATE 12/12,
  test_wave2 ALL PASS, RACCOIN `1 booted, 0 failed`.
- **BUG-6 applied to DXGI** (`bridge_d3d.cpp`): adapter vtbl 19 slots
  (IDXGIAdapter4 chain), factory 30 -> 32 (Factory7
  Register/UnregisterAdaptersChangedEvent real), output 23 -> 29
  (Output2..6: SupportsOverlays honest FALSE, CheckOverlaySupport
  DXGI_ERROR_UNSUPPORTED, GetDesc1 with real RDNA2-class colorimetry),
  all version chains verified against the SDK headers, static_asserts
  pinning every size. Evidence: `test_wave2` wave2f block ALL PASS.
- **Item 17 / 1L — WinRT ThreadPool + D3DKMT** (`bridge_winrt.cpp`,
  `bridge_gdi.cpp`): IThreadPoolStatics (RunAsync/RunWithPriorityAsync/
  RunWithPriorityAndOptionsAsync — work items run on a detached host
  thread, returned IAsyncAction reports Completed; IIDs verified against
  mingw windows.system.threading.h/asyncinfo.h/windows.foundation.h);
  D3DKMTOpenAdapterFromLuid/CloseAdapter/EnumAdapters honest synthetic
  Xbox-APU adapter + D3DKMTQueryAdapterInfo STATUS_NOT_IMPLEMENTED
  (fail-visible); Windows.Gaming.Input event surface VERIFIED
  already-present (GamepadAdded/Removed/FromGamepad were landed with the
  Gamepad statics).
  Evidence: `test_console_pack` ThreadPool block ALL PASS (handler invoked
  off-thread, AsyncStatus::Completed), `test_gdi` D3DKMT block ALL PASS.
- **Item 19 — save VFS overlay VERIFIED already-implemented**: Z: writes
  route to `<C: mount>/_z_write/` with whiteout-aware reads
  (`vfs.cpp` trt_vfs_resolve_utf16_rw); existing test_engine coverage
  ("Z:\ro.txt write succeeds via _z_write overlay") is the evidence. No
  code change needed — the gap list was stale.
- **Wiring**: `build-test.sh` suites 37 (ntdll) + 38 (kernel32b) appended;
  `main.cpp` registers nvapi stubs. Full canonical gate running as final
  verification of the whole wave.

**LANDED (same orchestrated wave, 2026-11-15, all verified offline):**
- **BUG-1 FIXED** (`bridge_d3d12.cpp`): CommandQueue vtbl rebuilt 19 -> 13
  entries in exact ABI order — ExecuteCommandLists at slot 8 (was a no-op
  UpdateTile at slot 8, exec at 10), Signal 9, Wait 10, ts-freq 11,
  clock-cal 12. ECL now records submitted lists (TRIANGLE GATE-ready).
  static_assert(13) + runtime `d3d12_vtbl_selfcheck()` fail-named.
- **BUG-6 FIXED**: Device QI chain extended to ID3D12Device5-9 (SDK-verbatim
  GUIDs; the master prompt's example Device5 GUID was wrong, headers win).
- **Sprint-1 1B/1C LANDED** (`bridge_kernel32.cpp`): 9 Interlocked* exports
  (fixed-width i32/i64, correct new/old-value semantics) + RtlCopy/Move/
  Zero/FillMemory, registered on kernel32+kernelbase, Rtl* merged into ntdll.
- **Sprint-1 1D VERIFIED already-registered** (GetSystemInfo/NativeSystemInfo/
  GetEnvironmentVariableW/VirtualAllocEx/VirtualFreeEx were in the merged
  table; the master prompt's gap list was stale here).
- **Sprint-1 1J LANDED** (`bridge_res.cpp` NEW): PE resource directory walk —
  FindResourceW/A/ExW, SizeofResource, LoadResource, LockResource (returns
  real mapped-image pointer), Unlock/FreeResource, EnumResourceNamesW,
  LoadStringW (real string-table format). Section-relative offset rebasing
  measured the hard way (first walk SIGSEGV'd treating child offsets as
  image-relative). Registered on kernel32; wired into host `main.cpp` boot +
  `build-host.sh` + `build-test.sh` suite 35.
- **Test evidence**: `test_d3d12` 24/24 ALL PASS (new), `test_wave1` 112/112
  (28 new), `test_res` 20/20 (new; real guest PE with hand-built .rsrc via
  `tools/build_guest_res.sh` + `tools/gen_guest_res.py`), `test_wave2` ALL
  PASS after slot-constant update (10->8, 14->9), test_engine/test_stage5/
  test_libload ALL PASS, **RACCOIN re-booted `1 booted, 0 failed`** with the
  whole wave linked (log `/tmp/opencode/raccoin-reswave.log`).
- **Sprint-1 1A LANDED** (`bridge_gdi.cpp`, measured — the master prompt's
  list was stale): GetDC/ReleaseDC were user32-only, now shared into the
  gdi32 table (same handler, both tables); GetStockObject case 10
  (OEM_FIXED_FONT) returned NULL — splash-text killer — now a real handle,
  stock ids 11-19 labels corrected to the real Win32 table; BitBlt/
  StretchBlt/PatBlt/StretchDIBits/SetDIBitsToDevice/TextOut*/ExtTextOut*/
  SaveDC/RestoreDC/SetMapMode made fail-visible (named log lines, never
  silent). CreateCompatibleBitmap + GetDeviceCaps were already real.
  `test_gdi` ALL PASS both argless (module-table resolution) and with the
  legacy guest-PE flow; RACCOIN re-booted `1 booted, 0 failed` again with
  GDI wave linked (log `/tmp/opencode/raccoin-gdiwave.log`).
- **Sprint-2 item 21 LANDED — THE D3D12 TRIANGLE GATE**
  (`tests/test_d3d12_triangle.cpp` + `src/guest/guest_triangle.c` +
  `src/guest/d3d12.def`/`dxgi.def`): a synthetic freestanding game walks the
  FULL D3D12 render path through the real loader — device -> queue ->
  allocator -> list -> rootsig -> PSO -> committed vertex buffer ->
  SetRootSignature/SetPipelineState/IASetVertexBuffers (GPU VA read back
  from the resource itself) -> DrawInstanced(3,1,0,0) -> Close ->
  ExecuteCommandLists (slot 8) -> fence SetEventOnCompletion/Signal ->
  CreateSwapChainForCoreWindow (queue-as-device, the D3D12 DXGI rule) ->
  Present. 12 ladder markers printed; host cross-checks the guest's queue
  pointer via `trt_d3d12_queue_recorded_count` (new accessor) — the
  submission RECORDED host-side on the object the guest created.
  **The gate caught a real ABI bug on its first run**:
  `fake_CreateSwapChainForCoreWindow` was missing the `pFullscreenDesc`
  param (6 vs the real 7-param ABI) — the out-pointer read one stack slot
  off, saved from a null write only by its own null check ->
  E_NOINTERFACE for every UWP D3D12 game. Fixed in `bridge_d3d.cpp`
  (CreateSwapChainForHwnd was already correct). Plus fail-visible log
  lines for DrawInstanced/ECL, ABI static-asserts pinning the resource(15)
  /fence(11)/cmdlist(60) vtbls, `CreateEventW`/`WaitForSingleObject` added
  to the kernel32 def stub. Suite 36 in build-test.sh + gate stage in
  corpus-run.sh.
  Evidence: 12/12 markers + ALL PASS (log `/tmp/opencode/tri.log`);
  regressions test_game_boot/test_d3d12/test_wave2/test_d3d ALL PASS;
  RACCOIN `1 booted, 0 failed` (log `/tmp/opencode/raccoin-tri.log`).

---

## W0 — hygiene first (fast, no design, high value)

1. **Commit the WIP as v0.35** (EH machinery + 9 new bridges + wave1 +
   UNITYMAIN2 IAT probe). ~27.8k insertions across 133 paths uncommitted
   after the RACCOIN boot — the single biggest risk to the project.
2. **gdb discipline:** never `gdb -batch -ex "commands 1" ...` — gdb reads the
   command-list body from the tty, wedges at the `>` prompt, and the guest never
   starts (that is exactly today's PID-301706 hang: no child process, stdin on
   `/dev/pts/179`). Use `gdb -batch -x script.gdb`.
3. **Ladder instrumentation fixes** (`tests/test_corpus_run.cpp observe()`):
   - D3D11 marker must also accept `"FL contract ->"` — today a real device
     acquisition reports `-` (false negative, hides progress).
   - UNITYMAIN2 marker must match only a *call* log line, not
     `"bind UnityPlayer.dll!UnityMain2"` (false positive).
4. **Adopt RACCOIN into the corpus:** copy to `corpus-local/raccoin/`
   (gitignored), add to `tools/corpus-run.sh` STAGE 7 EXES, so its depth is
   gate-enforced instead of ad-hoc.
5. `tools/audit_boot.py`: `'main'` → `'__main__'`.

## W1 — the EH wall — **BROKEN THROUGH (2026-11-15, ehfix9–19; DONE for C++ EH)**

Was: "the guest throws and we cannot walk/unwind". Now: faithful x64 SEH on
the `.pdata` walk is IMPLEMENTED and gated —
`bridge_kernel32.cpp`: `trt_RaiseException`, `trt_RtlUnwindEx`,
`call_pe_eh_handler`, `trt_RtlVirtualUnwind`, VEH pass-1, PLUS the measured
hard parts: pass-2 funclet resume records with geometric stale-purge and
per-guest-call retirement, and the host→guest boundary-escape swallow
(UnityPlayer wrapper-catch). Acceptance evidence: RACCOIN + Go Next boot 1/0
with the 7/7 ladder; `tests/test_guest_eh.cpp` ALL PASS including
funclet-rethrow and boundary-escape fixtures. What remains open below is the
scoping record; the wall itself is down.

1. **`RtlVirtualUnwind` decode fidelity** — `UNWIND_INFO` (Version/Flags/
   SizeOfProlog/CountOfCodes/Frames), all `UWOP_*` opcodes with real prolog
   mimicry (push of RBP, big/small stack allocs, save XMM, `UWOP_SET_FPREG`),
   `UNW_FLAG_CHAININFO` chained entries, personality + LSDA extraction, the
   `Flags & UNW_FLAG_EHANDLER|UHANDLER` cases, epilogue-boundary cases.
2. **Phase 1 (search):** walk frames from the faulting RIP through `.pdata`,
   call each personality with a correct `EXCEPTION_RECORD` +
   `DispatcherContext` (FunctionEntry/EstablisherFrame/ContextRecord/
   HandlerData=LSDA). Honor CONTINUE_SEARCH / CONTINUE_EXECUTION / HANDLER.
   VEH first-chance pass already exists — keep it ahead of the frame walk.
3. **Phase 2 (unwind):** `RtlUnwindEx` walks from the established frame to the
   target, runs every frame's unwind handlers/funclets (C++ destructors!), and
   restores the CONTEXT at the target rip. `call_pe_eh_handler` already pins
   the return PC so unwinding crosses the ELF host boundary — keep that.
4. **The ugly cases the logs already hit:** `flags=1` (NONCONTINUABLE),
   exceptions raised *during* unwinding, `RaiseException` from a guest worker
   thread (per-thread TEB — just fixed, keep its test), and the second
   exception with a null caller frame (unhandled → real termination path).

**Donors (license-checked):**

| Donor | License | Use |
|---|---|---|
| LLVM libunwind `Unwind-seh.cpp` + `UnwindCursor.hpp` | Apache-2.0 w/ LLVM exception | complete SEH-over-.pdata machinery incl. personality dispatch. Vendor with NOTICE, or port semantics line-by-line into MIT code — **license decision needed** (Apache patent grant + NOTICE retention) |
| Wine `dlls/ntdll/signal_x86_64.c` + unwinding | LGPL 2.1 | semantics only (repo rule: no vendoring) |
| MSDN "x64 exception handling" internals doc | free | the spec for UNWIND_INFO/UWOP decoding |

**Do NOT implement** `__CxxFrameHandler3/4` — measured absent from every import
table; the guest's statically-linked CRT provides its own personality. Implement
only NT primitives, faithfully.

**Test:** `tests/test_guest_eh.cpp` — clang-built freestanding guest exe with:
nested try/catch (C++), throw across guest→bridge→guest, catch-type matching +
rethrow, destructor unwinding during phase 2, SEH `__try/__except` AV handler,
raise-inside-funclet. All through the real IAT path. This is the test that
would have caught today's fault.

**Acceptance (UPDATED 2026-11-15):** ~~RACCOIN ladder prints `UNITYMAIN2
CALLED` (fixed marker) + `PUMP ENTERED`~~ → **DONE: full ladder 7/7 REACHED
(UNITYMAIN2 via naked-asm IAT probe, ehfix19), RACCOIN + Go Next demo.exe both
boot 1/0, all repo gates green.** Remaining for broader PC coverage: PE32
hardening, ARM64, D3D12 hardware verification, and real Bink/Wwise/Miles decode
(the generic loader/translator ladder is in place for the covered x64 path).

## W2 — measured bridge gaps (small; do alongside W1)

1. **`GetWriteWatch` / `ResetWriteWatch`** — the hidden IL2CPP requirement.
   il2cpp's GC is **Boehm-Demers-Weiser (bdwgc)**, incremental by default in
   modern Unity. bdwgc *probes* at runtime:
   `GetProcAddress(kernel32, "GetWriteWatch")` + a
   `VirtualAlloc(NULL, page, MEM_WRITE_WATCH|MEM_RESERVE, PAGE_READWRITE)`
   probe call — GWW_VDB is enabled only if BOTH succeed; otherwise it falls
   back to MPROTECT_VDB (write-protect + fault) or DEFAULT_VDB (all pages
   dirty: correct, but incremental GC off → frame hitches).
   - **DONE 2026-11-15 (honest, dual-surface):** `VirtualAlloc` accepts
     `MEM_WRITE_WATCH` as a request modifier (real Windows semantics); on the
     console `_WIN32` passthrough to the real kernel32 GWW; on Linux gate,
     soft-dirty emulation via `/proc/self/pagemap` gated behind a one-time
     liveness probe (bit 55, and bit 63 present required — this 6.14 kernel
     marks fresh absent pages dirty; a dead bit or clear_refs wipe would
     silently lie "clean" to a GC = heap corruption, so it fails named →
     bdwgc falls back to MPROTECT_VDB). Known limitation: Linux
     `ResetWriteWatch` clear_refs "4" is process-wide (console: real
     per-range API). Gate: `test_wave1` Wave 6G (GC probe exact-shape,
     2-page dirty report, reset-clean).
2. **`dwmapi.dll`** (UnityPlayer: `DwmGetWindowAttribute`,
   `DwmSetWindowAttribute`) — honest record / fail-visible.
3. After W2: run the **C++-resolver audit** (`tests/audit_boot.cpp`) over all
   four RACCOIN PEs. Expect only game-dir exports outside builtin.

## W3 — console M0 + the W^X truth (research-backed expectations)

Research (xbox-uwp-lab `devmode-limitations.md`, quoting dosbox-pure-uwp /
xenia-canary-uwp / PPSSPP source): **UWP AppContainers enforce strict W^X** —
plain `VirtualAlloc(PAGE_EXECUTE_READWRITE)` fails; the `...FromApp` family
works; PPSSPP's `PlatformIsWXExclusive()` returns TRUE for UWP; the RW→RX flip
is mandatory, not defensive. (XBSX2 is the unverified outlier using
`codeGeneration`+`runFullTrust` with plain RWX.)

The current WIP *allows* RWX in `trt_VirtualAlloc`/`uwp_protect`. Keep it, and
let the already-coded probe decide (`main.cpp`: `probe: jit RWX alloc ->
ALLOWED/DENIED`, W→RX transition, DLL presence). Outcome table:

| Probe result | Consequence |
|---|---|
| RWX DENIED + W→RX ALLOWED (expected) | two-phase shim: guest RWX requests become RW + auto-protect-RX on `FlushInstructionCache`; patch mono/Boehm alloc at IAT level (we own the loader) |
| RWX ALLOWED | keep as-is (trivial path) |
| both DENIED | Mono/JIT games (G3) blocked on console — report honestly, do not fake |

**M0 order (from the contract):** deploy signed MSIX → boot on TV → swapchain
HRESULT 0 + selftest `0x6494cb0d` → screenshot → pull `tiramisu.log` →
phone-transfer round trip → **then** the JIT probe → **then** RACCOIN boot.
No console claim without a TV screenshot + the log pull.

## W4 — after first frame (the game loop, per master prompt G2/G4/G5)

- Pump: host-driven message pump at vsync, `Present(1)` pacing, FPS in log.
- Audio: FMOD→WASAPI (bridge exists, offline test green) — prove ≥5s of real
  game PCM in the log.
- Input: XInput (bridged) + keyboard; gamepad-as-mouse fallback.
- Save: VFS `C:\` writes — verify RACCOIN's save lands and round-trips.
- Cutscenes: OrTMM ships `binkw32.dll` (in corpus); Unity video = mfplat.
  Fail-visible skip first; build mfplat/IMFSourceReader only when a game's
  boot blocks on it.
- `discord_game_sdk.dll` (in OrTMM corpus): loads as game-shipped DLL; expect a
  named handshake failure and continue.
- `UnityCrashHandler64.exe`: `CreateProcessW` is console-denied — keep the
  named denial; ensure the guest treats it as non-fatal.

## W5 — gonna be needed next (dependency order + triggers)

| # | Item | Status | Trigger to start |
|---|---|---|---|
| 1 | Streaming/windowed `MapViewOfFile`, unmap reclamation, >64 maps | **IN FLIGHT** (test_wave1.cpp) | — |
| 2 | `DispatcherQueue` passthrough | **DONE 2026-11-15** (offline DQC/DQ factories + QI in bridge_winrt.cpp; test_console_pack §9) | UnityPlayer calls it early; on console combase is REAL — passthrough first |
| 3 | GetWriteWatch honest impl (W2) | **DONE 2026-11-15** (dual-surface honest: console passthrough / Linux soft-dirty+probe; test_wave1 Wave 6G) | frame hitches / GC assert in log |
| 4 | IOCP/OVERLAPPED async + dstorage | dstorage DllMain already runs | streaming asserts |
| 5 | Memory ceiling (5 GB game budget) | partly in flight (streaming maps) | any real game OOM |
| 6 | Perf: pump/present pacing, guest-on-render-thread | session-2 Wave-1 plan | first-frame stutter |
| 7 | D3D12 | not started | an engine that refuses D3D11 (RACCOIN negotiated D3D11 fine — UE-class is the trigger) |
| 8 | x86-32 (G8) | **in-tree JIT, hardware proof pending** | a real PE32 title runs through the x86-32 JIT; ARM64 stays open |
| 9 | Steam depot downloader | **CONSOLE-VERIFIED v0.2.4.11** — HoloCure (2420510) downloaded 334 MB through CM PICS + per-CDN token to `LocalState\games` and appears on the shelf as `x64 D3D11 Ready`; no DRM bypass | — |

## NOT needed yet (scope discipline — cut from any plan)

- Anti-cheat / DRM bypass — contract rule, always.
- OpenGL/gl4es — no corpus demand.
- Steam client emulation beyond auth — rule.

---

## The distance, honestly (updated 2026-11-21)

- **Offline (Linux gate):** the SEH wall is broken through (W1), all corpus PEs
  load/bind with zero unresolved imports, and D3D12 recording has a triangle
  gate. Per-title capability is measured by `tools/support-matrix.sh`
  (symbol classes: real / recorded / named-silent, delay imports included).
  The earlier "one wall away, our own SEH unwinder" claim is stale — that
  wall is down.
- **Console (the actual goal):** G1 has run — signed MSIX deploys and
  **RACCOIN is playable, Celeste is playable (Chapter 1), God of War renders
  the forest** (see README "Console proof"). Still unproven on hardware:
  sustained soak + frame times, save/profile round-trip, GoW front-end/menu,
  720p+ output, and **audio** (Celeste's FSB/Vorbis decode is disabled on the
  Series S after a heap-corruption fault). Those are the remaining walls,
  not "G1 never ran".

### 2026-11-21 — the two walls worth naming, and what changed

- **FSB5 decode (console audio).** The corrupt-header shapes that could reach
  `reserve()`/vector growth are now refused by name in `fsb_parse`
  (`trt_fsb5_header_sane`) and per sample before decoding
  (`trt_fsb5_sample_sane`),  the `num_samples * 4` u32 wrap in the name-table
  check is fixed to a 64-bit compare, and a `bad_alloc` during decode is a
  named skip instead of a termination. Decode remains **off by default** on
  console — the fault was never reproduced under a debugger — and
  `fsb_decode=1` (settings.ini, or a per-game `tiramisu.profile.ini` that
  `game-stage.py --set fsb_decode=1` writes) enables the hardened path for one
  run; a packaged app inherits no environment, so an env var alone would not
  have reached it. Offline
  proof: `tests/test_fsb5_sanity.cpp` (`build-test.sh` §43c), no game banks or
  console required. Console proof is still pending; do not write "Celeste has
  audio" until a `proof.json` run says so.
- **x86-32 (W1/G8).** PE32 execution is in-tree: `src/host/main.cpp` runs
  `img->is_pe32` through `x86_run_pe32()` and `tools/game_stage.py` stages PE32
  (`MACHINES[0x014C] = ("PE32 (x86-32, JIT)", True)`). This is still unproven
  on hardware; the Boxedwine license-decision note is obsolete unless an
  engine needs more than the tree's JIT. `tools/game_stage.py` still refuses
  ARM64. If you drop the JIT, don't claim it here.
  The probe sequence in W3 converts all of them from speculation to measurement
  in one deploy session.
