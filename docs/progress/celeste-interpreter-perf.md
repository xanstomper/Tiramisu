# Celeste under the Mono interpreter — framerate analysis (offline)

Date: 2026-11-20 ~18:45 EST. Author: perf-analysis agent. Status: ESTIMATE — no console framerate data exists yet; all fps figures are derived, not measured. Read this as a risk assessment, not a verdict.

## Bottom line

**Likely playable, but a locked 60fps is NOT assured under the pure interpreter.**

- Celeste's per-frame managed work is small (it holds 60fps on a Raspberry Pi 5 and Intel HD 4000 under JIT), and the Series S Zen 2 is faster than both. That headroom is what saves us.
- The Mono interpreter is ~10-15x slower than JIT for managed compute (Microsoft's own benchmark data: median ~11x, range 2.5-35x). Applied to Celeste's ~1-2ms JIT frame cost on the Series S, the interpreted frame lands at ~7-31ms — straddling the 16.7ms 60fps budget.
- Best estimate: **light scenes (title screen, early levels) hit 60fps; busy scenes (many entities/particles) dip to 30-50fps.** The game stays playable throughout — the failure mode is slow-motion (fixed timestep), not a slideshow or input lag.
- Rendering (D3D11) and audio (FMOD) are native and run at full speed — they are NOT the bottleneck. The bottleneck is the interpreted managed update loop.
- The real fix, if the interpreter proves too slow, is **full AOT compilation** (the iOS model) — and the AOT runtime is already compiled into the UWP Mono build. AOT the hot assemblies (Celeste.exe + FNA) and the interpreter only runs cold code.
- The essential next step: **measure real frame times on the console** once Celeste boots (see section 7). This report is an estimate; measurement settles it.

## 1. What the offline test actually measured (and didn't)

The offline harness (tests/test_mono_celeste.cpp) is a BOOT gate, not a performance test. It PASSES when GAME DISPLAYED + DONE LOADING appear in the log, then kills the game with _exit(0). Quote from the header: "The game loop is killed by _exit once the gate markers are seen — the test's job is the boot, not the play session."

- **No frame times. No FPS. No steady-state measurement of any kind** — neither JIT nor interpreter. The game never runs a single gameplay frame in the test.
- What WAS measured (load times to the title screen, mineplex Intel N97 4-core @ 2.8/3.6 GHz):
  - JIT, earlier run (Step 1 report): GAME DISPLAYED 2127ms, DONE LOADING 4695ms.
  - JIT, .107 gate run (woowop-pkg107-gate.log lines 3818-3858): GAME DISPLAYED 4782ms, DONE LOADING 12352ms (GFX 2689ms, MTN 1353ms, AUDIO 251ms, GFX DATA 231ms, MTN DATA 1682ms, DIA/FONT 2778ms, LEVELS 2406ms). The 2.6x spread between the two JIT runs is machine load (the .107 gate ran alongside other builds) — load times are not stable measurements.
  - Interpreter, Step 1 report: DONE LOADING 8030ms (~1.7x the 4695ms JIT figure).
- **The 1.7x load slowdown does NOT predict the steady-state slowdown.** The load is dominated by file I/O + native parsing (FMOD bank parsing, texture uploads, level-data file reads) — work the interpreter doesn't touch. The managed parsing portions are a small fraction of the load. Steady-state gameplay is the opposite: mostly pure managed compute (entity updates, physics, collision), which is exactly what the interpreter slows most. Expect the steady-state multiplier to be MUCH larger than 1.7x (see section 2).
- One more confounder: the offline test renders with **llvmpipe** (Mesa software rasterizer — "OpenGL Renderer: llvmpipe (LLVM 20.1.2, 256 bits)"), not a real GPU. Its rendering cost is CPU-bound software rasterization and tells us nothing about console rendering (real D3D11 GPU, native, full speed).

## 2. The interpreter multiplier (measured data)

Source: dotnet/runtime discussion #40318 (Steve Harter, Microsoft, Aug 2020) — the canonical Mono interpreter benchmark.
- "A rough expected multiplier is that the Mono interpreter should be at most 15x slower than the Mono JIT."
- Real-world JSON (de)serialization: interpreter 12-18x slower than Mono JIT (serialize 58ms JIT vs 689ms interp; deserialize 138ms vs 2484ms).
- ~300 JSON benchmarks: range 2.5x to 35x slower, **median ~11x slower**.
- The 2.5x end is I/O-bound / native-call-heavy code (the interpreter isn't the bottleneck); the 20-35x end is tight micro-benchmark loops (pure IL, no native calls).
- Game update code (branchy, object-oriented, many small method calls + property accesses) sits in the middle: **use 10-15x for the managed update loop, ~2-3x for the render-issue path (mostly native calls).**
- Note: these figures are for Mono's interpreter on x86-64 (the same architecture as the Series S). The UWP build is stock Mono 6.8.0.105 — same interpreter.

## 3. Celeste's CPU profile (why it's light)

- Minimum specs (Steam): Intel Core i3, 2 GB RAM, integrated graphics (Intel HD 4000). Famously light: "runs at a locked 60fps on virtually any modern integrated GPU," "zero frame drops even on budget hardware."
- Raspberry Pi 5 (quad-core ARM Cortex-A76 @ 2.4 GHz, VideoCore VII) runs Celeste "at full resolution and frame rate" (60fps) natively on Linux. The Pi 5's single-thread is roughly half the Series S Zen 2's.
- Per-frame work (fixed 60Hz update, XNA/FNA):
  - Update: player physics (a few hundred ops), ~20-100 entity updates (simple math + grid collision), up to a few hundred particle updates, tilemap culling. Estimated ~50k-100k IL instructions/frame of managed work.
  - Render: SpriteBatch — the managed side iterates visible tiles and issues draw calls (a few hundred); the actual rasterization is the GPU's job (native D3D11 via FNA3D).
  - Audio: FMOD Studio, fully native, own thread.
- Under JIT on the Series S Zen 2, the total frame CPU cost is estimated at ~1-3ms (update ~0.5-1.5ms managed, render-issue ~0.2-0.5ms managed + native GPU work). This is an estimate from the Pi 5 / i3 evidence, not a measurement.
- The Series S CPU (Zen 2 @ 3.6 GHz) is ~1.2-1.5x the mineplex N97 per core and ~2x the Pi 5's A76 per core. The interpreter runs on ONE core (the game thread), so core count doesn't help — but the per-core speed does.

## 4. The estimate (frame-budget math)

60fps budget: 16.7ms/frame. Managed (interpreted) portion: ~0.7-2ms JIT on the Series S. Native portion (render, audio, GC): ~0.3-1ms, unaffected by the interpreter.

| Case | Managed JIT | x Interpreter | + Native | Total frame | Result |
|---|---|---|---|---|---|
| Best | 0.7ms | 10x -> 7ms | 0.5ms | ~7.5ms | 60fps, headroom |
| Mid | 1.2ms | 12x -> 14.4ms | 0.5ms | ~15ms | 60fps, just barely |
| Worst | 2ms | 15x -> 30ms | 0.5ms | ~30.5ms | ~30fps, slow-motion |

- The estimate straddles the 16.7ms line. **Light scenes: 60fps. Busy scenes: 30-50fps.** The game remains playable throughout.
- Sanity check (IL-count method): ~100k IL instructions/frame x ~20ns/interp-instruction = ~2ms — comfortably under budget. This UNDERCOUNTS (it ignores per-method-call frame setup, property dispatch, and bounds checks, which dominate real interpreter cost). The multiplier-based estimate (section 2, measured on real code) is more reliable.
- Failure mode if the interpreter can't sustain 60Hz updates: FNA's fixed timestep runs the game in SLOW MOTION (fewer than 60 updates per real second), not with input lag — input timing is preserved relative to game time, so precision platforming still works, just slower. This is the worst case, and it's still playable.

## 5. Bottlenecks, ranked

1. **The interpreted managed update loop** (player physics, entity updates, particles, collision, tilemap iteration). This is the whole ballgame — everything else is native.
2. **FNA platform bridge call overhead** (managed -> native transitions per draw call / input poll). Each crossing costs ~100ns-1us from the interpreter; hundreds of calls/frame add ~0.1-0.5ms. A contributor, not the dominant cost.
3. **GC (SGen).** Minor collections are fast (~1ms); major collections are rare in a game like Celeste. The interpreter allocates at the same rate as JIT code, but fewer frames/second means fewer allocations/second — GC pressure per second is actually LOWER under the interpreter. Minor concern.
4. **NOT bottlenecks: rendering (FNA3D -> D3D11, native, GPU-bound, trivial 2D sprite workload) and audio (FMOD Studio, native, own thread).** Both run at full speed regardless of the interpreter.

## 6. Mitigations (if the interpreter is too slow)

1. **Full AOT compilation — the real fix.** Pre-compile the hot assemblies (Celeste.exe + Microsoft.Xna.Framework.dll) to native code at build time (mono --aot). The AOT'd code runs at near-JIT speed with NO runtime codegen, so it works in the AppContainer (this is the iOS model — iOS forbids JIT, so Xamarin/Unity ship full AOT). GOOD NEWS: the AOT runtime is ALREADY compiled into the UWP Mono build (mono-sources.txt lists mono/mini/aot-compiler.c + aot-runtime.c in libmonosgen-uwp.lib). What's missing: (a) AOT-compiling the assemblies at build time, (b) packaging the AOT images in the MSIX, (c) host code to load them (mono_host.cpp currently has zero AOT references). With --interpreter + AOT images present, Mono uses AOT code where available and interprets the rest — so AOT the hot assemblies and interpret the cold framework code. Caveats: full AOT requires generic instantiations to be known at compile time (exotic generics can fail at runtime); Reflection.Emit / dynamic methods don't work under AOT (Celeste's core loop doesn't use them; FNA's content manager uses reflection to instantiate types, which works under AOT as long as the types are pre-compiled). UNTESTED on this UWP build — needs a build + console test.
2. **Interpreter optimizations.** Mono 6.8's interpreter is already reasonably optimized (threaded-code dispatch). Gains available: ~10-30%, not 10x. Not a substitute for AOT.
3. **Reduce the update-loop cost.** Not available — Celeste's code is fixed; we can't modify the game.
4. **Accept 30fps.** If the interpreter can't sustain 60fps, FNA can run at 30Hz updates (or the game just drops frames). Celeste at 30fps is playable but noticeably less smooth for a precision platformer. Fallback, not a fix.

## 7. What to measure next (settles this for real)

Once Celeste boots on the console, add frame-time logging to the next build:
- Timestamp each IDXGISwapChain::Present in the FNA3D D3D11 driver (third_party/fna3d-src/src/FNA3D_Driver_D3D11.c) and log a rolling average + p95 frame time to tiramisu.log every 60 frames. This measures the REAL frame rate (render + update + present).
- Alternatively, timestamp FNA Game.Update entry/exit in the platform bridge (the managed update cost specifically).
- Run a busy level (e.g., Chapter 3, lots of entities) for 60s and record the distribution. If p95 < 16.7ms, 60fps is proven. If p95 is 20-30ms, AOT (section 6.1) is the next step.
- Do NOT infer framerate from load times — section 1 explains why they don't predict steady-state.

## 8. Sources

- Offline test: tests/test_mono_celeste.cpp (mono tree, md5 79df55da) + woowop-pkg107-gate.log lines 3818-3858 (JIT load timings, llvmpipe renderer, N97 CPU).
- Host: src/trt/mono_host.cpp (monouwp tree) — interpreter forced on UWP (lines 253-260); no AOT references in the host.
- Mono interpreter multiplier: dotnet/runtime discussion #40318 (github.com/dotnet/runtime/discussions/40318) — median ~11x, range 2.5-35x, real-world 12-18x.
- Celeste lightness: Steam minimum specs (i3 / 2GB / HD 4000); Raspberry Pi 5 runs Celeste at full frame rate (computerstationnation.com SBC roundup); "locked 60fps on virtually any modern integrated GPU" (cheatcc.com low-end games list).
- Mono AOT: mono/mini/aot-compiler.c + aot-runtime.c present in the UWP build (mono-sources.txt); iOS full-AOT model (Xamarin docs).
- Series S CPU: Zen 2, 8-core @ 3.6 GHz. Mineplex CPU: Intel N97, 4-core Gracemont @ 2.8/3.6 GHz (lscpu).
