# Tiramisu — Game Compatibility Database

> Living document. Every entry is measured on the **offline Linux gate** unless
> marked `[XBOX]`. Status reflects the deepest boot-depth ladder rung reached.
>
> Ladder rungs: `CRT` → `WINMAIN` → `UNITYMAIN2` → `WINDOW` → `D3D11` →
> `PUMP` → `QUIT` → `EXIT(0)`. A game at `PUMP`+ is considered "boots".

## Tier definitions (Proton-like spectrum)

| Tier | Meaning |
|------|---------|
| **S** | Boots, runs sustained, playable with controller |
| **A** | Boots to pump, exits cleanly, untested for sustained play |
| **B** | Boots to window/D3D, dies during init (named gap) |
| **C** | Loads PE + DllMains, fails before window |
| **X** | Permanent exclusion (anti-cheat, Denuvo, W^X-denied JIT, OOM) |

## Verified games

### Ultra Pool (Godot 4.3 + GodotSteam)
- **Status:** Tier A (offline), unverified on Xbox
- **Engine:** Godot 4.3, Forward Mobile renderer, Vulkan→D3D11
- **Depth:** CRT ✓ WINMAIN ✓ WINDOW ✓ D3D11 ✓ PUMP ✓ QUIT ✓ EXIT(0) ✓
- **Notes:** Cleanest boot. Watchdog fires at 10s (expected). WM_CLOSE triggers
  Godot's own quit chain. Vulkan Forward Mobile initializes on the Tiramisu ICD.
  All shaders compile (maxFragmentInputComponents=128).
- **Gaps:** None known. First candidate for sustained-play testing.

### RACCOIN (Unity 6 IL2CPP, Steam) — **FIRST-ON-CONSOLE TARGET**
- **Status:** Tier B (offline, mock-d3d11) / Tier C (plain --exe)
- **Engine:** Unity 6000.3.0f1, IL2CPP, DirectX 11
- **Depth (mock-d3d11):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ D3D11 ✓ PUMP ✓ QUIT ✓ EXIT(0) ✓
- **Depth (plain --exe):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ PUMP ✓ QUIT ✓ (dies in CrashReporter exception path during teardown)
- **Notes:** Runs healthy for 10s under mock-d3d11. Plain-GPU path dies in the
  Unity CrashReporter insights-crash-report.json write (CreateFileW fails on the
  CrashReporter dir — guest never calls CreateDirectoryW). Named gap.
- **Gaps:** CrashReporter shutdown exception attribution; insights JSON write path.
- **Target selection (2026-11-16):** promoted to first-on-console candidate.
  Rationale: deepest-proven boot in the corpus (7/7 ladder, clean
  self-shutdown), Unity-IL2CPP+D3D11 path matches built bridge surface, small
  footprint (~524 MB disk; est. <1 GB RAM resident — comfortable under the
  Series S ~4 GB app-cap worry, which M0 measures with this game first).
- **PROVENANCE CAVEAT (blocks console deploy):** the corpus copy traces to a
  warez release (`RACCOIN-CPR-SteamRIP.com.rar` in user Downloads). The boot
  evidence above remains valid as runtime engineering, but per contract
  (own-copy-only, no DRM-circumvented binaries) the game cannot deploy to
  console or appear in the catalog until replaced with a legitimate Steam
  download. Engineering evidence transfers 1:1 to the legit copy (same app,
  same IL2CPP build — re-verify ladder after swap).

### Go Next! Demo (Unity 6 IL2CPP, Steam)
- **Status:** Tier C (offline) — named blocker, active investigation
- **Engine:** Unity 6000, IL2CPP + DStorage, DirectX 11/12
- **Depth:** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ — dies in SEH unwinding
- **Notes:** Minimal-corpus run (exe + UnityPlayer + GameAssembly) reaches
  UnityMain2 then exits rc=1. Full-corpus run (with baselib, DStorage, data)
  SIGSEGVs inside `trt_RtlVirtualUnwind` — UnityPlayer's exception path hands
  the unwinder corrupted/unmapped unwind metadata. Fixed: the unwinder now
  validates unwind-info pointers against registered guest images and returns
  a clean "unwind failed" verdict instead of crashing the host. Re-measurement
  pending with the hardened unwinder.
- **Gaps:** UnityPlayer SEH unwind metadata validation (hardened, re-measure);
  DStorage DLLs excluded from minimal corpus.

### OrTMM (embedded CPython 3.13 app, PE32+)
- **Status:** Tier C (offline) — named livelock, capped
- **Engine:** Embedded CPython 3.13 (python313.dll imports) — NOT Unity, NOT
  a classic game engine; first Python-family corpus entry
- **Depth:** CRT ✓ WINMAIN ✓ — enters an EH bounce livelock
- **Notes:** Boots through CRT + WinMain, then a single (rip,fault) pair is
  "handled" forever — 14.5M identical EH bounces (measured). CPython's
  signal-handling and exception machinery stress the EH path differently
  than game engines; the 100k bounce cap now terminates with a named
  verdict instead of spinning. Root cause of the identical (rip,fault)
  reproduction: rip=7e014aeefbe1 fault=0 — a guest fault handler restoring
  the faulting context without advancing PC.
- **Gaps:** EH livelock root cause (Python signal handler restart semantics).

## Corpus load matrix (DLLs that map + seal cleanly)
| PE | Status |
|----|--------|
| baselib.dll | DllMain(PROCESS_ATTACH) → 0x1 |
| libgodotsteam.windows.template_release.x86_64.dll | DllMain → 0x1 |
| steam_api64.dll | Maps + seals (was FAIL IMPORT; closed) |
| UnityPlayer.dll | DllMain → 0x1 |
| GameAssembly.dll (96MB IL2CPP) | DllMain → 0x1 |
| dxvk-2.5.3 d3d9.dll (native override) | Maps + seals + DllMain; boots as EXE entry |
| RACCOIN.exe | Boots (see above) |
| Ultra Pool game.exe | Boots (see above) |
| OrTMM.exe | Boots to EH livelock (see above) |
| Go Next demo.exe | Boots to UnityMain2 (see above) |
| openttd.exe (15.3, MinGW) | Boots: CRT-STARTUP → WinMain → window created → clean ExitProcess(1) (no MIDI/font engine on host — named gap, degrades to stdio sound path) |
| cataclysm-bn-tiles.exe (0.13.0, 632MB) | Loads (all imports bound); boots to named EH bounce-cap at rip=1437258d1 (all-zero GPRs: its C++ throw machinery) |
| veloren-voxygen.exe (weekly, Rust+wgpu) | Boots: CRT-STARTUP → WinMain entered → clean ExitProcess(2) (its own named exit before first wgpu surface request) |
| veloren-server-cli.exe (headless server) | Loads; import chain through libgcc_s/libstdc++/libwinpthread fully bound (msvcrt/bcryptprimitives/crypt32 fixes) |

### Wave-1 candidate board (2026-11-16)

First-on-console order (all DX11, 64-bit, Steam DRM only, no anti-cheat/Denuvo):

| # | Game | Engine/API | Disk | RAM est. | Why |
|---|------|-----------|------|----------|-----|
| 1 | **RACCOIN** | Unity 6 IL2CPP / DX11 | ~524 MB | <1 GB | Deepest-proven boot (7/7 ladder); blocked only by provenance swap → buy legit copy |
| 2 | **Stardew Valley** | MonoGame / DX11 | <1 GB | <1 GB | Lightest popular target; trivial 60 fps |
| 3 | **Celeste** | MonoGame / DX11 | ~1.2 GB | <1 GB | Light, beloved, cheap |
| 4 | **Hollow Knight** | Unity / DX11 | ~9 GB | 1–2 GB | Original Wave-1 pick; fits disk after cleanup |
| 5 | **Cuphead** | Unity / DX11 | ~4 GB | 1–2 GB | Best traction-clip value |

PC-only flagship candidate: **Dyson Sphere Program** (Unity IL2CPP / DX11,
~4 GB) — free Steam demo gives a legitimate no-purchase first probe of the
same proven path; demo boot gates full-game purchase.

**Vulkan-gated (excluded until a Vulkan→D3D12 story exists):**
- **BOTW via SavageWindRecomp** (4.6k★ static recomp, native x64, DRM-free,
  BYO-dump legitimate) — renderer is Vulkan 1.3-only; no Tiramisu Vulkan
  surface exists. Unlock paths (either): vkd3d-proton-class Vulkan→D3D12 shim
  post-d3d12-lane-proof, or fork the MIT-licensed recomp and port its renderer
  to D3D12 (weeks-scale, makes it a port rather than a Tiramisu win).
  Cemu route additionally excluded (emulator guest class + Vulkan-only).
- Decision recorded so this is not re-litigated. Revisit after sibling's
  d3d12 lane is proven on console.

**Wave-2 flagships (unchanged, in feasibility order):** Sekiro (DX11-only,
60-capped, 25 GB, top pick — PCGamingWiki verified: no Denuvo, no AC,
Havok/FMOD/Scaleform/Bink statically linked) > Witcher 3 classic (pin
classic; next-gen update is DX12) > Kakarot (40 GB, avoid `-d3d12`) >
Sparking! Zero (D3D12-only + framerate-tied = 60-or-nothing, last).
RDR2 excluded (DX12/150 GB/12 GB RAM). All gated on: user ownership,
disk cleanup (~60 GB free needed), Dev Mode creds, M0 memory measurement.

### Wave-2 engine-family entries (2026-11-16)
- **OpenTTD 15.3 — Tier C+ (offline):** full boot through window creation with
  60,268 unwind funcs registered via RtlAddFunctionTable; exits ExitProcess(1)
  on its own named "no sound/font backend" path (the console host answers 0
  MIDI devices and GDI_ERROR for outline glyphs — both the API's real no-device
  contract). Multimedia timers (timeSetEvent/timeKillEvent) proven by the
  guest_winmm 0xff contract: oneshot fires the guest TimeProc from a host
  thread through trt_call_guest with the real timer id; timeKillEvent freezes
  a periodic within ≤1 in-flight fire. Timer bug this wave: mm_timer_thread
  passed the raw epoch-ns target to nanosleep (wants a relative delta) — the
  thread slept ~1.7 billion seconds; caught by the 0x40/0x80 guest contract.
- **Cataclysm-BN 0.13.0 — Tier C (offline):** heaviest PE in the corpus (632MB,
  .text 0x39c554c); all imports bound (imm32 IME-filename, shell32 pickers,
  gdi32 pixel-format/ICM added this wave). Boots into its C++ exception
  machinery and hits the EH bounce cap with a named verdict
  (rip=1437258d1, zeroed GPRs — a fault handler restoring context without
  advancing PC, the same class as the OrTMM livelock). The abort path needs
  the boot-depth harness to classify "signal 6 after a named ehfix verdict"
  as BOOT-NAMED, not BOOT FAILED — report-format gap, tracked.
- **Veloren (weekly) — Tier C (offline):** first Rust+wgpu engine in the
  corpus. Load chain required: msvcrt printf/_ultoa/__C_specific_handler
  (the SEH language handler is the SAME trt_C_specific_handler kernel32
  exports — one implementation, two module exports), _close/_telli64/
  _wfullpath/_wopen/_wutime64/_fpreset/_localtime64/_onexit/strspn + 8
  narrow-float math (acosf…tanf), bcryptprimitives!ProcessPrng (Win10+
  direct random, BOOL return — distinct ABI from BCryptGenRandom),
  mmdevapi!ActivateAudioInterfaceAsync (UWP audio activation: creates the
  REAL wasapi_client and fires ActivateCompleted slot 3 through
  trt_call_guest), ole32!StringFromIID, crypt32 cert-chain family.
  veloren-voxygen boots CRT → WinMain → ExitProcess(2) (its own named exit
  before the first wgpu surface request — Vulkan/DX12 backend probe is the
  next named gap).

## Native DLL overrides (Proton-style)

Games shipping their own API DLLs (dxvk, cnc-ddraw, dgVoodoo, special K, the
VC runtime) shadow Tiramisu's builtin bridge for that module. Drop the DLL in
the game directory next to the .exe and it is loaded instead of our bridge —
the same `WINEDLLOVERRIDES=native` mechanism Proton uses.

**Never overridden** (loader-critical): kernel32, ntdll, user32, gdi32,
advapi32, ole32, oleaut32, comctl32, shell32, shlwapi, ws2_32, crypt32,
bcrypt, sechost, rpcrt4, kernelbase, ucrtbase, vcruntime140*, msvcp140*,
combase, dwrite, d2d1, d3d10, d3d12, dxgi, version.

Everything else (d3d9, d3d8, ddraw, dsound, dinput8, winmm, xinput1_4,
steam_api64 handled separately) is overridable. dxvk-2.5.3's d3d9.dll is in
the corpus and maps+seals+boots.

## Permanent exclusions
| Category | Examples | Reason |
|----------|----------|--------|
| Anti-cheat | EAC, BattlEye, Vanguard | Contract rule — never bypass |
| Denuvo multiplayer | Various | DRM bypass prohibited |
| JIT without W^X | Mono/IL2CPP (if W^X denied) | OS policy, not code |
| Memory pressure | Cyberpunk-class (>5GB) | Console RAM cap |
| 32-bit x86 | Legacy titles | License decision pending |
| OpenGL-only | GL-only engines | Cut from scope (no corpus demand) |

## How to add a game
1. Drop the game folder into `corpus-local/<name>/`
2. Run: `./tests/out/test_corpus_run --exe corpus-local/<name>/<exe>.exe`
3. Read the boot-depth ladder in the log
4. File a gap report naming the exact bridge/import/EH failure
5. Add an entry here with the measured depth

## Testing on Xbox
Requires Dev Mode credentials (`DP_HOST`, `DP_PASS`). See `TIRAMISU-MASTER-PROMPT.md`
for the deploy sequence. Games verified offline are Tier A candidates for M0.