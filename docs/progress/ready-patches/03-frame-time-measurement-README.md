# 03 — Frame-time measurement (FNA3D D3D11 driver)

**Patch:** `03-frame-time-measurement.patch` (applies to
`third_party/fna3d-src/src/FNA3D_Driver_D3D11.c` — the driver source the
UWP host build compiles, per `tools/build-host.sh:100`).
**Apply from the repo root (`~/tiramisu-brown-merge`):**
`git apply 03-frame-time-measurement.patch`.
**Independent** of patches 01/02 — it can land alone, before or after them.
**Verified** (02:57 EST): `git apply --check` passed; a real `git apply`
produced output byte-identical to the edited copy; the shared tree was
untouched during generation. Caveat: verified against the tree as of
02:57 EST — if opencode edits `FNA3D_Driver_D3D11.c` in these regions
first, re-run `git apply --check` before applying.

## What it does

Timestamps every `IDXGISwapChain::Present` in `D3D11_SwapBuffers` and
logs the rolling average + p95 frame time to `tiramisu.log` every 60
frames, so the first boot on the console produces real framerate numbers
instead of guesses. This is the essential next step from
`celeste-interpreter-perf.md`: the interpreter is ~10–15x slower than
JIT on the managed update loop, and only a console measurement can say
whether Celeste holds 60fps.

## How it works

- **Timer:** `QueryPerformanceCounter` / `QueryPerformanceFrequency`,
  with an explicit `#include <profileapi.h>` — required because the host
  build defines `WIN32_LEAN_AND_MEAN`, which excludes `profileapi.h`
  from `windows.h`. Both APIs are in the UWP API set (AppContainer-safe).
- **Frame time:** the interval between consecutive pre-Present
  timestamps = the frame period as the game experiences it (including
  any vsync block inside Present).
- **Ring buffer:** the last 120 frame times in a fixed `double[120]`
  array — zero allocation in the Present path. A separate total counter
  drives the 60-frame log cadence (the ring count caps at 120, so it
  cannot be used as the trigger).
- **p95:** copy + insertion sort of the window (120 elements, once per
  60 frames — negligible). Index = round(0.95 × (n−1)).
- **Logging:** `FNA3D_LogInfo("trt: frame avg=%.1fms p95=%.1fms n=%d")`,
  routed by the host's log hooks (`trt_fna3d_attach`) into
  `tiramisu.log`. The host prepends `FNA3D info: `, so the line reads:
  `FNA3D info: trt: frame avg=12.3ms p95=16.1ms n=120`
- **Threading:** runs under the `ctxLock` already held by
  `D3D11_SwapBuffers` — no extra synchronisation needed.
- **Non-Windows:** the whole block (include, helper, call site) is
  guarded by `#if defined(_WIN32)` — on Linux the patch compiles to
  nothing. (The sdl-shim has no `SDL_GetPerformanceCounter`, so the SDL
  timing route was not available.)
- **Failure mode:** if `QueryPerformanceFrequency` fails or returns 0,
  the helper disables itself quietly (no log spam, no crash). The first
  frame has no previous timestamp and is skipped, not recorded.

## What the log lines look like

One line every 60 frames (~1/second at 60fps), starting after the first
60 frames:

```
FNA3D info: trt: frame avg=12.3ms p95=16.1ms n=60
FNA3D info: trt: frame avg=14.8ms p95=22.4ms n=120
FNA3D info: trt: frame avg=15.1ms p95=21.9ms n=120
```

`n` grows 60 → 120 as the ring fills, then stays 120 (rolling window).

## How to read the numbers

- **p95 < 16.7ms** — locked 60fps. The interpreter is fast enough; no
  AOT needed for framerate.
- **p95 16.7–25ms** — mostly 60fps with dips into the 40s in busy
  scenes. Playable (FNA slows the game down rather than stuttering);
  AOT is a nice-to-have.
- **p95 25–35ms** — sustained 30–40fps in busy scenes. Playable but
  noticeably slow; AOT compilation is the next step (see
  `aot-compilation-plan.md` — feasible, needs the assembly-translation
  script + host changes, do not start until the boot lands).
- **avg ≫ p95 or p95 spiking** — hitches, not a steady bottleneck.
  Look at what the game was doing at that point (atlas load, GC pause,
  bank decode) before blaming the interpreter.
- **No lines at all** — Present is never reached (the game died before
  the first frame) or the patch did not land (see below).

## How to verify it landed

1. `grep -c D3D11_INTERNAL_FrameTimeSample third_party/fna3d-src/src/FNA3D_Driver_D3D11.c`
   → 2 (definition + call site).
2. After the next console test, `grep "trt: frame" tiramisu.log` →
   one line per 60 frames once the game is presenting.
