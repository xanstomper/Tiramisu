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

### RACCOIN (Unity 6 IL2CPP, Steam)
- **Status:** **Tier S [XBOX] — fully playable**; Tier B (offline, mock-d3d11) / Tier C (plain --exe)
- **Engine:** Unity 6000.3.0f1, IL2CPP, DirectX 11
- **Depth (console):** main menu → live run → coin physics → HUD/score/round progression; 7.2 s capture
- **Depth (mock-d3d11):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ D3D11 ✓ PUMP ✓ QUIT ✓ EXIT(0) ✓
- **Depth (plain --exe):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ PUMP ✓ QUIT ✓ (dies in CrashReporter exception path during teardown)
- **Notes:** Console build reaches the real Unity main menu and starts a
  playable run through Tiramisu's D3D11 swapchain. Evidence:
  `docs/proof/raccoin-main-menu.png`, `raccoin-gameplay.png`,
  `raccoin-gameplay.gif`, `raccoin-gameplay.mp4`. Offline mock-d3d11 remains
  healthy for 10s; plain-GPU path dies in the Unity CrashReporter
  insights-crash-report.json write (CreateFileW fails on the CrashReporter
  dir — guest never calls CreateDirectoryW). Named gap.
- **Gaps:** sustained-session soak and frame-time capture; CrashReporter shutdown exception attribution; insights JSON write path (offline).

### God of War (PC, AAA in-house engine)
- **Status:** Tier A [XBOX] — boots and renders the opening forest; front-end/menu layer next
- **Engine:** AAA in-house engine, x64, DirectX 11
- **Depth (console):** boot → in-engine forest scene rendered across four independent boots
- **Notes:** The four captures in `docs/proof/gow-forest-01.webp` …
  `-04.webp` show a converging render path: first boot has large occluders and
  missing geometry; later boots restore character/materials, foliage,
  lighting and the full forest composite. The interactive front-end/menu layer and
  sustained gameplay are not yet proven.
- **Gaps:** interactive front-end/menu layer; sustained gameplay; performance
  and audio validation.

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

## Verified desktop applications

Non-game Win32 programs use a different surface than game engines: ANSI string
APIs, common dialogs, window icons, GDI text metrics, a message loop and no
D3D. They are measured on the same ladder.

### PuTTY 0.83 (Win32 GUI terminal, x64)
- **Status:** Tier A [offline] — full boot ladder, unverified on Xbox
- **Depth:** CRT ✓ WINMAIN ✓ WINDOW ✓ D3D11 – PUMP ✓ QUIT ✓ EXIT(0) ✓ (8 messages)
- **Notes:** First non-game desktop app in the corpus. It immediately exposed a
  host-side crash in `LoadImageA`: a `MAKEINTRESOURCE` atom (0xc8) was logged
  with `%s`, so the host read address 0xc8. Fixed with `name_or_id()` in
  `bridge_user32.cpp`; `LoadImageA` now returns real stock icon/cursor handles.
  PuTTY then boots to its own `ExitProcess(0)`.
- **Gaps:** PuTTY's window is created and receives the paint cycle, but it only
  paints a terminal once a session connects, so the offline run shows one
  font-probe cell rather than a terminal screen. Menus and common dialogs are
  record handles / honest failures; no printing (winspool is a named stub).

### Desktop paint path (offline gate)
- **Status:** Tier A [offline] — `tests/test_desktop_paint.cpp`
- **Depth:** RegisterClassEx ✓ CreateWindowEx ✓ WM_MOVE/SIZE/PAINT ✓ BeginPaint ✓
  PatBlt ✓ TextOutA ✓ Rectangle ✓ EndPaint ✓ pixels-on-surface ✓
- **Notes:** A desktop-shaped guest PE runs the whole path and the host counts
  the pixels: 640x400 white client area, black text, green box. The window's DC
  surface (`src/trt/gdi_raster.cpp`) is a real 32-bit raster with a clip
  rectangle and an embedded 8x16 coverage font, dumped as a BMP for inspection.
  Surfaces are sized to the window's client area (the dump is 640x400, not
  display-sized), and GetClientRect/BeginPaint report that same rect.
- **Depth now includes:** GDI+ (Graphics from HDC/HWND, ARGB brushes/pens with
  real blending, fill/draw rectangle/ellipse/line/polygon/arc/pie, path
  display lists, bitmaps from Scan0 + scaled DrawImage, DrawString/MeasureString
  at the requested em size), msimg32 AlphaBlend/TransparentBlt/GradientFill,
  RoundRect with elliptical corners and Arc/Pie/Chord, ScrollWindowEx pixel
  scrolling, a real update region (InvalidateRect/GetUpdateRect/ValidateRect
  and BeginPaint's rcPaint), a full GDI+ affine world transform (translate /
  scale / rotate / matrix objects / invert, applied vertex by vertex, with
  glyph and bitmap placement at the transformed origin or box), GDI path
  recording (BeginPath records
  Rectangle/Ellipse/RoundRect/Polygon/Arc/Pie/Chord/Polyline/Bezier geometry,
  then FillPath/StrokePath/StrokeAndFillPath replay it with the ALTERNATE or
  WINDING fill rule, GetPath reports the vertices, SelectClipPath turns a path
  into a per-pixel clip), and ASCII+Latin-1 text coverage. Named limits: no anti-aliasing,
  translate-only transforms, AC_SRC_ALPHA falls back to the constant alpha,
  GRADIENT_FILL_TRIANGLE is a named FALSE, code points above U+00FF draw a
  hollow box, rotated/skewed bitmaps draw their transformed bounding box,
  glyph scaling under a transform is placement-only, WidenPath is a named FALSE, SelectClipPath supports RGN_AND and
  RGN_COPY only (OR/XOR/DIFF are named FALSE), PathToRegion answers with the
  path's bounding box.

## Corpus load matrix (DLLs that map + seal cleanly)
| PE | Status |
|----|--------|
| baselib.dll | DllMain(PROCESS_ATTACH) → 0x1 |
| libgodotsteam.windows.template_release.x86_64.dll | DllMain → 0x1 |
| steam_api64.dll | Maps + seals (was FAIL IMPORT; closed) |
| binkw32.dll | Maps + seals (gdi32!SetStretchBltMode closed) |
| discord_game_sdk.dll | Maps + seals (ktmw32!CreateTransaction/CommitTransaction closed) |
| SDL2.dll | Maps + seals (shell32!ExtractIconExW closed) |
| UnityPlayer.dll | DllMain → 0x1 |
| GameAssembly.dll (96MB IL2CPP) | DllMain → 0x1 |
| dxvk-2.5.3 d3d9.dll (native override) | Maps + seals + DllMain; boots as EXE entry |
| RACCOIN.exe | Boots (see above) |
| Ultra Pool game.exe | Boots (see above) |
| OrTMM.exe | Boots to EH livelock (see above) |
| Go Next demo.exe | Boots to UnityMain2 (see above) |

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
| OpenGL-only | GL-only engines | Cut from scope (no corpus demand) |

**Experimental:** 32-bit x86 (PE32) executes through the in-tree x86-32 JIT and
is staged by `tools/game-stage.py`, but it is not a supported target until a
real 32-bit title produces a console completion artifact. The old licensing
decision block no longer applies.

## How to add a game
1. Drop the game folder into `corpus-local/<name>/`
2. Run: `./tests/out/test_corpus_run --exe corpus-local/<name>/<exe>.exe`
3. Read the boot-depth ladder in the log
4. File a gap report naming the exact bridge/import/EH failure
5. Add an entry here with the measured depth

## Testing on Xbox
Requires Dev Mode credentials (`DP_HOST`, `DP_PASS`). See `TIRAMISU-MASTER-PROMPT.md`
for the deploy sequence. Games verified offline are Tier A candidates for M0.