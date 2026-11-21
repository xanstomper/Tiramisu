# FNA3D CreateDevice path analysis (after the attach fix)

Offline read-only analysis, 2026-11-21 ~01:30 EST. Sources: `~/tiramisu-brown-merge`
(opencode's tree, the current line). No console touch, no builds, no clones, no deploys.

Context: Celeste on Tiramisu (Xbox Series S, UWP) runs under embedded Mono 6.8.0.105 in
INTERPRETER mode. The managed fixes WORK (.122 test: FNA3D attach rc=0 +
PrepareWindowAttributes ok). .129 test (furthest yet): SteamAPI_Init SUCCEEDS, FNA
native calls executing (Ping, GetWindowHandle, GetDisplaySize), then dies on
UnauthorizedAccessException writing error_log.txt (CWD fix in progress, opencode
iterating .130-.138). After the CWD fix, the next expected milestone is FNA3D
CreateDevice (the game creating its graphics device) → GAME DISPLAYED.

## Bottom line

**CreateDevice should work on the host-adopted path.** The adoption is correctly
implemented (AddRef ownership, the BGRA RTV format fix, the faux-backbuffer size
decision, host-safe destroy), and the .122 test already proved the attach + driver
selection end to end. The CreateDevice path itself has no functional blocker.

The remaining risks are diagnostic gaps (no trace markers inside the D3D11 driver,
silent RTV/Present failures, the missing managed null check) and one robustness gap
(a failed attach is non-fatal in main.cpp, which would route the launch into the
stock path → a guaranteed AV crash). None of these block the next test — the .132+
test will show whether CreateDevice succeeds, and the existing trace (enter/exit
dev=%p) + the FNA3D log lines are enough to diagnose the outcome.

## 1. (a) Host-device path vs stock path — the host path wins

`D3D11_CreateDevice` (`third_party/fna3d-src/src/FNA3D_Driver_D3D11.c:5912`) FIRST
calls `D3D11_INTERNAL_TakeHostDevice` (line 5946). If a host device is pending
(`g_hostDevice != NULL`), it returns `D3D11_CreateDeviceWithDevice(...)`
immediately — the stock path (DXGI factory load, adapter enum, D3D11CreateDevice,
swapchain creation) is SKIPPED entirely. The take is one-shot: it copies the
pointers out and clears the registration (`FNA3D_D3D11_ClearHostDevice`).

The call order in the host (`src/host/main.cpp:1435-1445`) is correct:

1. `trt_fna3d_attach_from_hooks(log)` — pre-registers device/context/swapchain
   (from `trt_gpu_hooks_get()`), installs the FNA3D log hooks.
2. `FNA3D_PrepareWindowAttributes()` — driver selection.
3. `trt_mono_run(...)` — the game starts; its GraphicsDevice constructor calls
   `FNA3D_CreateDevice`, which consumes the pending registration.

`D3D11_PrepareWindowAttributes` (line 5400+) has a TIRAMISU fast path: if
`g_hostDevice != NULL`, it returns 1 immediately, skipping the throwaway-device
probe ("a pre-registered host device is already known-good"). So driver selection
succeeds trivially on the host path — provided the attach ran first (it does).

Only D3D11 is compiled in (`/DFNA3D_DRIVER_D3D11 /DWINAPI_FAMILY_WINRT=1`,
`tools/build-host.sh:128`). `drivers[]` (`FNA3D.c:49`) is `{ &D3D11Driver, NULL }`,
so `selectedDriver = 0` on success. Driver selection is deterministic: D3D11 or
nothing.

The .122 test confirmed this end to end (attach rc=0, PrepareWindowAttributes ok).

## 2. (b) Host device + swapchain adoption — correctly implemented

### What the host owns (`src/host/swapchain.cpp`, `src/trt/bridge_d3d.h`)

- `ID3D11Device` (ID3D11Device1), created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`,
  feature levels 11_1 / 11_0. The BGRA_SUPPORT flag is REQUIRED for creating an
  RTV on a B8G8R8A8 swapchain back buffer — it is set.
- `ID3D11DeviceContext` (immediate, Context1).
- `IDXGISwapChain1` over the CoreWindow (`CreateSwapChainForCoreWindow`):
  `DXGI_FORMAT_B8G8R8A8_UNORM`, 2 buffers, `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`,
  size = CoreWindow bounds (default 1280x720, scaled by the resolution setting).
- `IDXGIFactory2` (the console's real factory) + `IDXGIFactory6`.
- Published via `trt_gpu_set_hooks()` / `trt_gpu_hooks_get()`.

### What FNA3D adopts (`D3D11_CreateDeviceWithDevice`, line 5842)

- Requires hostDevice AND hostSwapchain non-NULL (else logs
  "D3D11_CreateDeviceWithDevice: device and swapchain are required!" and returns
  NULL). A NULL context is fine — it derives the immediate context via
  `GetImmediateContext` (failure → logs "Could not get immediate context from host
  device!", releases the device, returns NULL).
- `AddRef`s the device, context, and swapchain; sets `renderer->device`,
  `renderer->context`, `renderer->featureLevel` (from the host device),
  `renderer->hostAdopted = 1`, `renderer->supportsTearing = FALSE` ("never present
  with tearing on a swapchain we did not create").
- Factory/adapter/DLLs stay NULL on this path ("we never load or create them").
- Calls the shared tail `D3D11_INTERNAL_FinishCreateDevice` with
  `adapterDesc = NULL` and the host swapchain.

### Swapchain registration (`D3D11_INTERNAL_RegisterHostSwapchain`, line 5690)

- Registers the host swapchain as `swapchainDatas[0]` (windowHandle NULL — "there
  is no SDL window on the console", RTV NULL initially, format = the requested
  backbuffer format). Returns 0 only on OOM.
- Caches the host back-buffer size via `IDXGISwapChain_GetDesc`
  (`hostSwapWidth`/`hostSwapHeight`) for the SwapBuffers RTV refresh. A GetDesc
  failure does NOT fail the registration (see gap #6).

### Backbuffer (`D3D11_INTERNAL_CreateBackbuffer`, line 2955)

- Host branch (line 2997): never creates, resizes, or recreates the host
  swapchain. Releases the old RTV so the tail recreates it from the current back
  buffer, re-reads the host size via GetDesc, and decides the faux backbuffer:
  `useFauxBackbuffer = (hostW != requestedW || hostH != requestedH || MSAA > 0)`.
  Sizes match + no MSAA → render DIRECTLY to the host back buffer
  (`BACKBUFFER_TYPE_NULL`). Sizes differ or MSAA → faux backbuffer (render to an
  offscreen texture at the requested size, then blit to the host back buffer).
  Celeste does not use MSAA, so the decision is purely size-based. Both paths are
  implemented; the likely path is faux (if the CoreWindow is 1920x1080 and
  Celeste requests 1280x720) or direct (if they match).
- RTV format fix (line 3323, TIRAMISU EXTENSION): on the host path, the RTV
  format is read from the host swapchain desc (`B8G8R8A8_UNORM`), NOT the XNA
  backbuffer format (`R8G8B8A8_UNORM`) — "a mismatched RTV fails to create."
  This is the critical format fix; without it the RTV creation would fail.
- `D3D11_INTERNAL_UpdateSwapchainRT` (line 2920): `GetBuffer(0)` +
  `CreateRenderTargetView` with the host format. On failure it logs
  ("Could not get buffer from swapchain" / "Swapchain RT view creation failed")
  and returns with the RTV still NULL (see gap #2).
- Depth/stencil buffer is created at the backbuffer size with the XNA depth
  format mapped to D3D (standard path).
- Ends with `D3D11_SetRenderTargets(..., NULL, 0, NULL, NONE, 0)` to bind the
  default render target.

### Present (`D3D11_SwapBuffers`, line 1593)

- Faux path: the drawable size comes from the host swapchain's GetDesc (or the
  cached hostSwapWidth/Height if GetDesc fails); the faux blit vertex buffer is
  updated if the rects changed; the faux backbuffer is blitted to the host RTV.
  The blit samples the faux texture (RGBA8) and writes to the host RTV (BGRA8);
  the format conversion is handled by the RTV format (hardware swizzle) — there
  is no swizzle code in the blit shader, and none is needed.
- RTV refresh (line 1695): if the host resized its swapchain since the RTV was
  created, the old RTV is released and re-created from the current back buffer.
  The swapchain itself is never resized by FNA3D.
- `IDXGISwapChain_Present(swapchain, syncInterval, presentFlags)` (line 1794) on
  the HOST swapchain. `presentFlags = 0` on the host path (supportsTearing is
  FALSE). `syncInterval` comes from the presentation interval (DEFAULT/ONE → 1,
  TWO → 2, IMMEDIATE → 0). Celeste uses the default (vsync, 60fps cap).
  Present's HRESULT is IGNORED (see gap #3).

### Destroy (`D3D11_DestroyDevice`, line 1137)

Host-safe: the swapchain/device/context Releases only drop FNA3D's own references
(taken at adopt); the host's com_ptr references keep the objects alive, so FNA3D
never destroys the host device/swapchain. The factory/DLL releases are
NULL-guarded (NULL on the host path). The RTV is FNA3D's own and is always
released. (One caveat: the COBJMACROS Release macros have no NULL check, so a
NULL RTV at destroy time would AV — reachable only if RTV creation failed; see
gap #8. Shutdown-path only.)

## 3. (c) Remaining gaps and failure modes

### Gap 1 — Attach failure is non-fatal (the dangerous one)

`main.cpp` logs the attach result (`TRACE fna3d attach rc=%d attached=%d`) but
does NOT check it — the launch proceeds to PrepareWindowAttributes +
`trt_mono_run` regardless. If the attach fails (GPU hooks not published, no
device/swapchain), the cascade is:

1. `g_hostDevice` is NULL, so `D3D11_PrepareWindowAttributes` takes the STOCK
   probe path — which SUCCEEDS on UWP: the MojoShader context creation uses the
   linked `D3DCompile` (WinRT branch, no LoadLibrary); `SDL_LoadObject` in the
   sdl-shim is `GetModuleHandleA` (succeeds because dxgi.dll/d3d11.dll are
   already loaded in the process); the throwaway `D3D11CreateDevice` succeeds.
   So `selectedDriver = D3D11`.
2. `FNA3D_CreateDevice` → `D3D11_CreateDevice` → `TakeHostDevice` returns 0 (no
   registration) → the STOCK path runs: DXGI factory load, adapter enum,
   D3D11CreateDevice (a SECOND device), then `D3D11_INTERNAL_CreateSwapChain`.
3. The stock swapchain creation calls `SDL_GetWindowWMInfo`, which the sdl-shim
   implements as `return SDL_FALSE` WITHOUT filling `info`
   (`sdl_shim.c:209-214`). The return value is UNCHECKED, so
   `dxgiHandle = info.info.win.window` reads UNINITIALIZED STACK garbage.
4. `CreateSwapChain(..., OutputWindow=garbage, ...)` fails
   (DXGI_ERROR_INVALID_CALL). `ERROR_CHECK` logs and CONTINUES (does not
   return). Then `IDXGISwapChain_GetParent(swapchain=NULL, ...)` dereferences
   NULL → AV → process crash (no VEH installed, no SEH guard around
   `trt_mono_run`).

In practice the attach works (.122 showed rc=0 — the hooks are published at app
startup, before any launch is requested — so the race is theoretical, not
observed). But IF it ever fails, the result is a crash instead of a clean error.
FIX: make the attach failure fatal in main.cpp — abort the launch with a clear
error if `arc != TRT_OK` (or gate PrepareWindowAttributes + `trt_mono_run` on
`trt_fna3d_is_attached()`).

### Gap 2 — RTV creation failure is silent

`UpdateSwapchainRT` uses `ERROR_CHECK_RETURN` (void return). On GetBuffer or
CreateRenderTargetView failure, `swapchainRTView` stays NULL and CreateBackbuffer
does NOT check — it proceeds to `D3D11_SetRenderTargets`, which binds a NULL RTV
(`views[0] = swapchainDatas[0]->swapchainRTView`). `OMSetRenderTargets` with a
NULL RTV is legal D3D11 (it unbinds the target), so there is no crash — just a
black screen with the game otherwise running (CreateDevice returns non-NULL,
Present runs, nothing renders). The only signal is the
"FNA3D error: Swapchain RT view creation failed" line in tiramisu.log (via the
FNA3D log hooks). FIX: check the RTV after UpdateSwapchainRT and fail CreateDevice
loudly (or add a trace marker).

### Gap 3 — Present failure is invisible

`SwapBuffers` ignores `IDXGISwapChain_Present`'s HRESULT (no ERROR_CHECK). A
device-removed or invalid-call Present silently does nothing. FIX: log Present's
HRESULT (or trace it) — the frame-time measurement (timestamp Present, log avg +
p95) needs this anyway.

### Gap 4 — No trace markers inside the D3D11 driver

The trace covers only main.cpp (attach + PrepareWindowAttributes) and FNA3D.c
(CreateDevice enter/exit). If CreateDevice fails or hangs inside the driver, the
trace shows only "enter" + (no exit / NULL exit). The FNA3D log has the error
lines, but only if tiramisu.log is being read (it froze in the hang cases; the
trace is the reliable channel). FIX: add `trt_trace` markers for the
TakeHostDevice result, CreateDeviceWithDevice entry, RegisterHostSwapchain result,
RTV creation result, and first Present.

### Gap 5 — The managed null check is missing

`GraphicsDevice.cs` (`third_party/fna-src/src/Graphics/GraphicsDevice.cs:562-575`)
has NO null check on GLDevice after `FNA3D_CreateDevice` — only a try/catch for
exceptions. If CreateDevice returns NULL, the constructor silently proceeds with a
NULL device (the original hang signature: the game spins, no render, no crash).
The `FNAPlatform-tiramisu.patch` only touches `FNAPlatform.cs`; the null check
recommended by the hang-managed analysis was never landed. FIX: add
`if (GLDevice == IntPtr.Zero) throw new NoSuitableGraphicsDeviceException(...)`
after the CreateDevice call (and rebuild the FNA DLLs). The trace's
"exit dev=%p" (dev=0) is the current backstop. NOTE: the brown-merge
`third_party/fna/out/` DLL (1,182,208 B, built Nov 20 16:21) differs in size from
the staged mono-tree variant (1,169,408 B) — the test agent should verify which
variant is on the console.

### Gap 6 — GetDesc failure fallback uses the wrong format

If `IDXGISwapChain_GetDesc` fails in both RegisterHostSwapchain and
CreateBackbuffer, `hostSwapWidth`/`hostSwapHeight` stay 0 → `useFauxBackbuffer =
true` (safe) BUT the RTV format falls back to `dxgiFormat` (RGBA8) instead of the
host's BGRA8 → CreateRenderTargetView with a mismatched format FAILS → NULL RTV →
black screen (see gap #2). GetDesc on a valid swapchain essentially never fails,
so this is low-probability — but the fallback should fail loudly, not silently use
the wrong format.

### Gap 7 — The requested backbuffer size isn't traced

The faux-vs-direct decision depends on Celeste's requested size vs the host
swapchain size, but neither is logged. The test agent can't tell from the log
which path was taken. FIX: add a trace line in CreateBackbuffer (requested WxH,
host WxH, useFauxBackbuffer).

### Gap 8 — Destroy-time NULL RTV release (minor, shutdown-path only)

The COBJMACROS Release macros (`FNA3D_Driver_D3D11.h:32`, `#define COBJMACROS`)
have no NULL check, so `ID3D11RenderTargetView_Release(NULL)` at destroy time
would AV. Reachable only if RTV creation failed (gap #2). Note it; don't chase it.

### Verified non-issues

- **MojoShader context creation is UWP-clean.** `MOJOSHADER_d3d11CreateContext`
  takes the `#if WINAPI_FAMILY_WINRT` branch (`compileFunc = D3DCompile` —
  linked directly, no LoadLibrary). The LoadLibrary path is non-WinRT only. It
  can fail only on OOM. `FinishCreateDevice` doesn't NULL-check the result, but
  OOM is the only failure mode.
- **Driver selection is deterministic.** Only D3D11 is compiled in; there is no
  other driver to fall back to (or be confused by).
- **`FNA3D_GetDrawableSize` returning 0x0 is irrelevant.** The sdl-shim's
  `SDL_GetWindowSizeInPixels` returns 0x0, but `FNA3D_GetDrawableSize` is called
  only by the SDL2/SDL3 backends — never by the Tiramisu backend, which uses
  `NativeGetDisplaySize` (verified executing in the .129 test).
- **The host swapchain being replaced by the D3D12 path** (`acquire_d3d12_swapchain`
  / `configure_d3d11_swapchain` can replace `g_swap`) only matters if a D3D12
  guest ran first. For a fresh Celeste launch the D3D11 swapchain is current.
- **The window handle is irrelevant on the host path.** `Tiramisu_FNAPlatform.CreateWindow`
  passes the host handle (or the headless handle) to `FNAWindow`, but the
  host-adopted CreateBackbuffer never touches `deviceWindowHandle` — it uses
  `swapchainDatas[0]`. The handle matters only for mouse coordinate scaling
  (via `NativeGetDisplaySize`, which works).

## 4. (d) What the trace/log shows

### Success markers (in order)

1. `TRACE fna3d attach rc=0 attached=1` (main.cpp, trace)
2. `TRACE PrepareWindowAttributes ok` (main.cpp, trace)
3. `TRACE FNA3D_CreateDevice enter selectedDriver=0` (FNA3D.c, trace)
4. `FNA3D Driver: D3D11` + `D3D11 Adapter: host-provided device (no adapter
   picked)` (tiramisu.log, via the FNA3D log hooks installed by the attach)
5. `TRACE FNA3D_CreateDevice exit dev=0x...` (non-NULL pointer) (FNA3D.c, trace)
6. First Present → GAME DISPLAYED (the managed side logs it; the TV shows the
   game).

### Failure markers

- `TRACE fna3d attach rc!=0 attached=0` → the attach failed. If the launch
  proceeds anyway, expect a stock-path AV crash (gap #1): the trace will show
  PrepareWindowAttributes ok, CreateDevice enter selectedDriver=0, then the
  process dies (no exit line, no further log lines).
- `TRACE FNA3D_CreateDevice exit dev=0x0000000000000000` → CreateDevice returned
  NULL. Check tiramisu.log for the FNA3D error line ("Could not register host
  swapchain!", "Could not get immediate context from host device!", "Backbuffer
  ... creation failed"). The managed side will NOT throw (no null check, gap #5)
  — the game spins silently with a NULL device.
- `FNA3D error: Swapchain RT view creation failed` in tiramisu.log → the RTV
  creation failed (gap #2). Expect a black screen with the game otherwise running
  (CreateDevice returns non-NULL, Present runs, nothing renders).
- `TRACE FNA3D_CreateDevice enter` with NO exit line → the process died inside
  CreateDevice (a crash in the driver — e.g., the stock-path AV, or a MojoShader
  failure). Check the crash dump / stderr.
- No `FNA3D Driver: D3D11` line in the log → `FinishCreateDevice` was never
  reached. The failure is before it: TakeHostDevice → CreateDeviceWithDevice
  (the NULL check or GetImmediateContext).

## Recommended fixes (priority order)

1. **Make the attach failure fatal in main.cpp** (one-line guard on
   `trt_fna3d_is_attached()` / the attach rc before PrepareWindowAttributes).
   Turns a guaranteed crash into a clean, diagnosable error. Do this before the
   next test — it costs nothing and removes the worst failure mode.
2. **Add the managed GLDevice null check** (`GraphicsDevice.cs`) + rebuild the
   FNA DLLs. Turns a silent NULL-device spin into a loud exception with a
   stack trace.
3. **Add trace markers inside the driver** (TakeHostDevice result,
   CreateDeviceWithDevice entry, RegisterHostSwapchain result, RTV creation
   result, first Present + Present HRESULT). Makes the CreateDevice outcome
   diagnosable from the trace alone, without depending on tiramisu.log.
4. **Check the RTV after UpdateSwapchainRT** and fail CreateDevice loudly if it
   is NULL (or at minimum trace the RTV creation result).
5. **Trace the backbuffer sizes** in CreateBackbuffer (requested WxH, host WxH,
   useFauxBackbuffer) so the test agent knows which render path was taken.

None of these block the next test. The .132+ test will show whether CreateDevice
succeeds, and the existing trace + FNA3D log lines are enough to diagnose the
outcome.

## Files read (all in ~/tiramisu-brown-merge)

- `third_party/fna3d-src/src/FNA3D_Driver_D3D11.c` (CreateDevice 5912,
  CreateDeviceWithDevice 5842, FinishCreateDevice 5720, RegisterHostSwapchain
  5690, CreateBackbuffer 2955, UpdateSwapchainRT 2920, SwapBuffers 1593,
  SetRenderTargets 2581, DestroyDevice 1137, PrepareWindowAttributes 5400,
  stock CreateSwapChain 2729)
- `third_party/fna3d-src/src/FNA3D.c` (drivers[] 49, PrepareWindowAttributes 148,
  CreateDevice 234, CreateDeviceWithDevice 266)
- `third_party/fna3d-src/MojoShader/mojoshader_d3d11.c` (CreateContext 630)
- `third_party/fna3d-src/sdl-shim/sdl_shim.c` (LoadObject 73, GetWindowWMInfo
  209, GetWindowSizeInPixels 196)
- `src/trt/fna3d_host.cpp` (whole file, 135 lines)
- `src/trt/bridge_d3d.h` (trt_gpu_hooks struct)
- `src/host/main.cpp` (managed launch path 1414-1450)
- `src/host/swapchain.cpp` (publish_gpu_hooks 273, create_d3d11_corewindow_swapchain
  333, swapchain_create 360)
- `third_party/fna/Tiramisu_FNAPlatform.cs` (CreateWindow 286)
- `third_party/fna-src/src/Graphics/GraphicsDevice.cs` (constructor 562)
- `tools/build-host.sh` (FNA3D sources 99-102, defines 128)
