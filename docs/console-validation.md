# Console validation — captured 2026-11-22 (Series S, 10.0.0.23)

Real Device Portal session (`XBOX_IP=10.0.0.23`, OS 26100.9662, Xbox Series S),
not an offline simulation. Evidence files:

- `logs/20261122-020206-tiramisu.log` — real failed boots of **Sekiro**.
- `logs/20261122-021634-tiramisu.log` — real boot of **ultrapool**.
- `tests/fixtures/logs/real-console-sekiro-failure.log` — extracted failure.
- `tests/fixtures/logs/real-console-ultrapool-boot.log` — extracted boot.
- `docs/proof/ultrapool-autopilot-console.png` — TV screenshot during the run.

## Crash containment — validated 2026-11-22 (build 0.2.2.197)

Deployed freshly packed `out/Tiramisu.msix` (0.2.2.197), launched Sekiro via
`/api/launch/Sekiro`, waited 45 s. Result: **the app survived** — `/api/status`
still returned `running:true`, dashboard responsive, `guest_running:false`.
The log shows `guest fault: 0xc0000005 at sekiro.exe+0x115c66b ... read address
FFFFFFFFFFFFFFE8` followed by the GPR/stack dump and a clean return to shelf.
Extracted as `tests/fixtures/logs/real-console-seh-contained.log`.

## Live autofix match on a real console log

`python3 tools/autofix.py <tmp> logs/20261122-063452-tiramisu.log` (pulled live
from 10.0.0.23) matched these real signatures:
`sekiro-invalid-pointer-named`, `missing-game-data-named`,
`vulkan-present-stall-named`, `godot-resource-named`, `dstorage-missing-named`,
`mscoree-stub-named` — all named work items, no hidden gaps.

## D3D12 host hook regression check (build 0.2.2.200)

Packed and deployed the build carrying the D3D12 host submit hook to the
Series S, relaunched ultrapool: `vkQueuePresentKHR frame=1` still appears, so
the D3D11/Vulkan path is unaffected. No D3D12 title is staged, so the submit
hook itself is only gated offline (triangle gate: HOST SUBMIT PASS).

## D3D12 host submit hook on hardware — staged and launched

Uploaded `tests/out/guest_triangle.exe` as game `d3d12triangle` via
`PUT /api/upload-file`, launched it via `POST /api/launch/d3d12triangle`.
Console log shows `launching ...games\d3d12triangle\guest_triangle.exe`,
`d3d: guest presented its first frame`, `launch d3d12triangle: rc=OK`.
No `d3d12:` bridge lines appeared: the console routes `d3d12.dll` to the
REAL UWP system library (`module_table.cpp` default `TRT_ROUTE_SYSTEM`), so
the guest used hardware D3D12 and never entered our bridge's submit hook.
The hook remains gated offline (triangle gate: HOST SUBMIT PASS). A per-title
route override would be needed to force the bridge path on console.

## What is proven on hardware

- Signed `Xan.Tiramisu_0.2.2.192` is installed and launches via Device Portal.
- The in-console web dashboard (`http://10.0.0.23:9127`) is live and serves the
  staged shelf (Sekiro, stage-ultrapool, ultrapool).
- `POST /api/launch/ultrapool` queued the title; `/api/status` reported
  `guest_running=true, running_game=ultrapool, last_launch_result=started`.
- The guest reached **real D3D11 swapchain creation** (`1920x1080`, 3 images),
  Vulkan 1.3 ICD initialization (`AMD - Tiramisu Vulkan->D3D11`), a real render
  pass (`Draw #1 pipe=1230 idx=1 vtx=6`) and `vkQueuePresentKHR frame=1`.
- WASAPI initialized a real 48 kHz stereo float client (`IAudioClient::Start -> S_OK`).

## Named blockers from the same session (fail-visible, not hidden)

1. **Sekiro** repeatedly launches, then faults:
   - `kernel32: CreateFileW open failed on host` for `Z:\S\pack.dat` — required
     game data is not staged; no bridge can repair missing content.
   - Later `FATAL VEH: unhandled exception 0xc0000005 ... at ntdll.dll`, with
     `p1=ffffffffffffffff` — a bogus guest pointer reaching a host API; the
     host VEH returns `EXCEPTION_CONTINUE_SEARCH`, so the app terminates.
   Both signatures are now in `tools/fix_db.json` as named work items.
2. **ultrapool** presents exactly one frame (`frame=1` never advances) and the
   TV screenshot is blank apart from the cursor. Rendering path is proven, but
   the frame loop stalls after the first present — next console work item.

## What this means for the autopilot

The transfer -> audit -> generate -> launch -> log -> fix -> retry loop is live
on the console (autopilot fields are exposed in `/api/status`). Console
validation now has real evidence for both a successful boot path and a named
failure path. Remaining console work is the frame-loop stall and the Sekiro
missing-content / invalid-pointer faults.

## Play checkpoint after a restart

On 0.2.4.27 and newer, `GET /api/launch-checkpoint` returns the last
persisted Play phase and outcome. The response contains numeric fields
only and can be read after reopening Tiramisu. The private crash bundle
remains in LocalState.

| Phase | Meaning |
|---|---|
| 1 | Preparing a launch |
| 2 | Base guest drives configured |
| 3 | Native executable TLS callbacks |
| 4 | Native entry point invoked |
| 5 | x86 execution invoked |
| 6 | Managed runtime invoked |

| Outcome | Meaning |
|---|---|
| 1 | The phase was entered |
| 2 | Control returned to the launcher |
| 3 | Guest ExitProcess was trapped |
| 4 | Guest exception reached the launch boundary |
| 5 | Unhandled host exception |
| 6 | Host abort handler ran |
| 7 | Host process exit handler ran |

If the app disappears and the last outcome is 1, the checkpoint establishes
the last entered phase. The exit reason is still unconfirmed. Exception
outcomes carry the Windows exception code in `code`.

On 0.2.4.33 and newer, `GET /api/launch-api-tail` shows up to 12 recent
instrumented runtime APIs, newest first. It reports names and consecutive
repeat counts for the current Play attempt. It contains no arguments,
paths, or memory addresses. The trace is available while the process is
alive. The last observed call establishes where the sample stopped.

On 0.2.4.42 and newer, `GET /api/launch-worker-fault` reports the phase and
exception code of the first fault that escaped a guest worker routine or
DLL thread-attach callback. The worker report is cleared for each Play
attempt and kept separately from the main launch checkpoint. An available
worker report indicates that the worker reached the exception boundary.

On 0.2.4.43 and newer, the `shader_input_*` fields in `GET /api/render-status`
include a bounded sample of a 2D texture bound to the pixel shader at
Present. They describe state at that moment and report no pixel data.
The sampled texture may differ from inputs used by earlier draw calls.
