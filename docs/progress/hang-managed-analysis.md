# Managed-side hang analysis — Celeste on Tiramisu (.112/.113/.114)

**Status note:** Reconstructed from the analyst's completion report (2026-11-20 18:36 EST).
The analyst's own transfer of the full report was interrupted mid-chunk (only 3 of 6
base64 chunks reached /tmp/hma.b64 on mineplex), so the destination file was never
created. This file contains the complete findings from that report, in the analyst's
own words. The partial /tmp/hma.b64 is harmless (incomplete base64) and can be deleted.

## Context

Tiramisu .112/.113/.114 all reach `mono_jit_init: domain ok` ->
`fna platform: native internal calls registered` -> `invoking managed Main` and then
hang — no further log lines, no FNA `GAME DISPLAYED`. Mono init is FIXED. The game
starts executing managed code and freezes. The .112 log shows
`GameInput mouse callback failed hr=0x80004001`.

## Top 2 hang points + fixes

### 1. NULL FNA3D device — `FNA3D_PrepareWindowAttributes` is never called (TOP managed defect)

**Mechanism (code-verified).** `FNA3D_CreateDevice` (native, `FNA3D.c:234`) requires
`FNA3D_PrepareWindowAttributes` to have run first: `selectedDriver` starts at -1
(`FNA3D.c:148`) and is set ONLY by PrepareWindowAttributes (`FNA3D.c:217`).
PrepareWindowAttributes is called ONLY by the SDL2/SDL3 backends' CreateWindow
(`SDL2_FNAPlatform.cs:420`, `SDL3_FNAPlatform.cs:346`). The Tiramisu backend's
CreateWindow (`Tiramisu_FNAPlatform.cs:286-312`) does NOT call it (verified: no
reference anywhere in `third_party/fna/*.cs`). So on the console, `selectedDriver`
stays -1 and `FNA3D_CreateDevice` returns NULL immediately.

The GraphicsDevice constructor (`GraphicsDevice.cs:562-575`) has NO null check on
GLDevice — it proceeds to `FNA3D_GetMaxTextureSlots(NULL)`, which NULL-checks and
returns early without setting the out params. The constructor COMPLETES with a NULL
device. Every hot-path FNA3D function NULL-checks (verified: SwapBuffers,
DrawIndexedPrimitives, SetRenderTargets, Clear, CreateTexture2D, GetMaxTextureSlots
all `if (device == NULL) return;`). Result: the game loop runs with no render and no
present, no GAME DISPLAYED, no crash, no log output — matching the observed hang
signature exactly (except the watchdog/server silence, which needs the heap-lock
mechanism). The D3D11 probe itself is NOT the problem: if PrepareWindowAttributes
were called, D3D11 WOULD be selected (MojoShader context succeeds on WinRT —
D3DCompile is linked directly; the throwaway D3D11CreateDevice succeeds since the
host already has a device; UWP build has ONLY the D3D11 driver).

**Fix (must land WITH #3 below).** Call `FNA3D.FNA3D_PrepareWindowAttributes()` from
the Tiramisu backend's CreateWindow (mirroring SDL2) OR from the host before Main.
AND add a null check in the GraphicsDevice constructor (throw
NoSuitableGraphicsDeviceException if GLDevice is Zero) so a NULL device fails LOUDLY.
AND call `trt_fna3d_attach_from_hooks` before Main — otherwise fixing this alone
unmasks a crash (see #3).

**What tracing would show:** guest markers bracketing FNA3D_CreateDevice — "before
CreateDevice" then "after CreateDevice, GLDevice=0" CONFIRMS the NULL device. If the
trace shows the guest stuck BEFORE CreateDevice, this is refuted as the hang point
(but remains a real bug to fix).

### 2. Heap-freeze trip during startup's heavy allocation (assembly load + FMOD banks + atlas)

**Mechanism.** The sibling's hang-log-analysis.md H1 (heap-lock freeze) needs the
guest thread allocating heavily when it trips. The heaviest allocation phases in the
first ~3s of Main are: (1) assembly loading + type initialization (Mono loader:
metadata, method tables, interpreted code), and (2) LoadContent: FMOD Studio bank
loading (music.bank alone is 393 MB; the Studio engine allocates for bank
data/events/samples) + atlas texture loading (Content.Load reads + decompresses
texture data into managed buffers — the FNA3D_CreateTexture2D calls no-op with a
NULL device, but the MANAGED side still allocates the pixel buffers). The watchdog's
first marker is due at +3s and never appears, so the freeze trips within ~3s —
during assembly loading or early LoadContent, NOT in the game loop (which starts
after LoadContent, ~8s+ under the interpreter offline). The exact phase is what the
tracing build's guest markers will pin down.

**Fix.** This is a SYMPTOM location, not a root cause — the root cause is the heap
corruption (sibling's domain: debug heap / Application Verifier / pageheap; suspect
areas: Mono patches 28/29 allocation bookkeeping). Managed-side contribution: fix #1
so LoadContent runs against a REAL device. The tracing build's heap-health probe
(watchdog periodic malloc/free) distinguishes "heap corrupted" from "heap fine, block
elsewhere".

**What tracing would show:** the LAST guest marker before the freeze names the phase
(e.g., "FMOD bank load started" with no matching "done" = bank loading trips it;
assembly-load markers = loader trips it).

## H3 (critical sequencing): the D3D11 stock path — garbage-HWND swapchain → AV

Once PrepareWindowAttributes IS called (#1 fix), the D3D11 driver is selected and
FNA3D_CreateDevice calls D3D11_CreateDevice, which FIRST tries
`D3D11_INTERNAL_TakeHostDevice` — which returns false because
**`trt_fna3d_attach` / `trt_fna3d_attach_from_hooks` is NEVER CALLED anywhere in the
tree** (verified: zero callers outside fna3d_host.cpp; main.cpp's managed launch path
at :1414-1430 sets only exe_path/log/fna_platform and calls trt_mono_run directly).
So CreateDevice takes the STOCK path: DXGI factory → adapter → D3D11CreateDevice
(all fine) → `D3D11_INTERNAL_CreateSwapChain` with the CoreWindow ABI pointer as the
window handle. Inside it, the sdl-shim's `SDL_GetWindowWMInfo` returns SDL_FALSE
WITHOUT filling `info`, so `dxgiHandle` is UNINITIALIZED STACK garbage.
`CreateSwapChain(..., OutputWindow=garbage, ...)` fails (DXGI_ERROR_INVALID_CALL);
ERROR_CHECK logs-and-continues (does NOT return); then
`IDXGISwapChain_GetParent(swapchain=NULL, ...)` dereferences NULL → AV → process
CRASH (no VEH installed — mono_veh is defined but never registered; no SEH guard
around trt_mono_run — the __try blocks wrap only the NATIVE guest path). The process
is currently ALIVE, which CONFIRMS the stock path is never reached today (#1's NULL
return happens first). **Fixing #1 WITHOUT the attach fix turns the hang into a
crash.** Fix: call `trt_fna3d_attach_from_hooks(log)` in main.cpp's managed launch
path BEFORE trt_mono_run, TOGETHER with the #1 fix. Bonus: the FNA3D log hooks are
only installed by trt_fna3d_attach — calling it ALSO routes FNA3D's errors into
tiramisu.log (currently OutputDebugStringA only, invisible).

## Ruled out (with evidence)

- **(b) GameInput mouse callback:** the "mouse callback failed hr=0x80004001" line is
  a BOOT-time registration failure (native_gameinput.h:70-78, E_NOTIMPL; host logs and
  continues). NOTHING waits on it — fire-and-forget. FNA reads mouse via non-blocking
  `trt_mouse_pos` getters. NOT a hang point.
- **(d) FNA platform bridge deadlock:** all native internal calls (fna_platform.cpp)
  are non-blocking getters (atomics, input-layer reads, no waits). The managed backend
  has no waits (ProgramInit, CreateWindow, RegisterGame, PollEvents capped at 1024
  non-blocking, GetGraphicsAdapters pure managed). `NeedsPlatformMainLoop()` returns
  false. input.cpp has NO log calls (no lock-order inversion with g_file_mu). No
  deadlock path exists.
- **FMOD init blocking:** `fmod_System_Create`/`fmod_System_Init` return FMOD_OK
  immediately (host mixer backend, no WASAPI init). No waits/sleeps/threads anywhere
  in bridge_fmod*.cpp. Cannot hang. (Bank LOADING allocates heavily — see #2 — but
  doesn't block.)
- **Content file IO blocking:** LocalState reads are fast and non-blocking. Content
  loading matters as an ALLOCATION trigger (#2), not an IO block.
- **Plain managed hang (alone):** would leave the watchdog + server alive. The
  system-wide freeze needs the heap-lock mechanism (sibling's H1).

## How the two analyses fit together

- Sibling (hang-log-analysis.md): FREEZE MECHANISM = CRT heap lock (guest thread
  stuck inside malloc/free on a corrupted free-list). Corruption source unknown.
- This report: MANAGED ACTIVITY = NULL-device startup (#1) + heavy allocation in
  startup/LoadContent (#2). The NULL device is a REAL BUG that must be fixed
  regardless. The heap freeze trips during the heavy allocation of startup (within
  ~3s).
- BOTH fixes needed: (1) managed fixes (PrepareWindowAttributes + fna3d attach +
  null check) so the game gets a REAL device; (2) heap-corruption hunt so the freeze
  doesn't trip during startup allocation. Fixing (1) without (2) may still freeze;
  fixing (2) without (1) leaves a silently spinning NULL-device game.

## Fix order for the test agent

1. Next build: call `FNA3D_PrepareWindowAttributes()` AND
   `trt_fna3d_attach_from_hooks(log)` — TOGETHER. Add the GLDevice null check (fail
   loudly). Include Mono patch 30 (sibling's H3).
2. Tracing build (allocation-free writer + heap-health probe + guest markers
   bracketing FNA3D_CreateDevice, LoadContent phases, game loop) CONFIRMS the NULL
   device (#1) and PINS the freeze-trip phase (#2).
3. Heap-corruption hunt (debug heap / pageheap on a dev build) — sibling's
   recommendation.

**Constraints honored:** no console touch, no builds, no clones, no deploys, no
commits/pushes, no lock touch — read-only analysis plus the report file.
