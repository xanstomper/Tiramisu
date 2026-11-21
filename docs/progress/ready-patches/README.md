# Ready-to-apply host patches (Celeste / Tiramisu)

Prepared 2026-11-21 ~02:45 EST, offline. Both patches are host-only (no Mono
rebuild), verified to apply cleanly against ~/tiramisu-brown-merge as of
02:45 EST (git apply --check + real apply + byte-compare against the edited
copies). The shared tree was NOT modified — these are diff files only.

## How to apply

From the repo root (~/tiramisu-brown-merge):

    git apply ~/tiramisu-gow-skye/docs/progress/ready-patches/01-attach-fatal-guard.patch
    git apply ~/tiramisu-gow-skye/docs/progress/ready-patches/02-steam-callback-dispatch.patch

Then build + pack + deploy as usual. Both patches are independent — either
can land alone.

## 01 — Attach-fatal guard (main.cpp)

File: src/host/main.cpp, managed-launch path (~line 1438, after the FNA3D
attach trace). Source: fna3d-createdevice-analysis.md, gap 1 (the dangerous
one).

What it does: if trt_fna3d_is_attached() is false after the attach, the
launch FAILS NOW with TRT_ERR_FNA3D (detail from trt_fna3d_last_error())
instead of proceeding to PrepareWindowAttributes + trt_mono_run.

Why: without the guard, a failed attach falls through to the stock D3D11
probe path — which SUCCEEDS on UWP — so CreateDevice takes the stock path,
builds a swapchain against a garbage window handle (SDL_GetWindowWMInfo
returns SDL_FALSE without filling info), and dereferences a NULL swapchain
-> AV crash. The guard turns that guaranteed crash into a clean, visible
launch failure.

Verify it landed:
- grep: `grep -n "FATAL fna3d attach failed" src/host/main.cpp`
- In the console log, a failed attach now shows:
  `trt: FATAL fna3d attach failed - refusing stock D3D11 path`
  and the launch aborts (no PrepareWindowAttributes / mono run after it).
- A successful attach is unchanged (no new log line, attach rc=0 path).

## 02 — Steam callback dispatch (bridge_steam.cpp)

File: src/trt/bridge_steam.cpp, trt_SteamAPI_RunCallbacks (~line 104).
Source: delegate-jit-analysis.md ("the REAL next blocker").

What it does: RunCallbacks now DISPATCHES registered callbacks. RegisterCallback
stores .run = nullptr (the game fills the vtable), so nothing ever set .run
and callbacks were registered but never fired. The patch reads the
CCallbackBase vtable directly from the stored callback object (first field
= vtable pointer, slot 0 = Run) and calls Run(callback, param). The
existing .run path is kept as a fallback if .run is ever set. Null checks
on the callback, the vtable, and slot 0.

Why: Celeste calls RequestCurrentStats, which fires UserStatsReceived
asynchronously. If the game waits on that callback and it never fires,
the game stalls. With this patch, registered callbacks actually run.

Verify it landed:
- grep: `grep -n "vtable\[0\]" src/trt/bridge_steam.cpp`
- In the console test, watch for: a game that previously stalled after
  RequestCurrentStats now proceeding; the first RunCallbacks with a
  registered callback no longer being a no-op.
- If a crash appears on first callback dispatch, suspect the patch 32
  static thunk (check the fault address against the thunk pool range)
  — see delegate-jit-analysis.md.

## Notes

- Neither patch changes the .141 behavior when things work: the guard only
  fires on attach failure (currently attach rc=0), and the dispatch only
  fires for registered callbacks (previously a no-op).
- Do NOT reintroduce mono_aot_only as a delegate fix (.123/.124 proved it
  kills Mono without AOT images) — the delegate path is already JIT-free
  via the patch 32 static stubs.
- Source analyses: ~/tiramisu-gow-skye/docs/progress/fna3d-createdevice-analysis.md
  and ~/tiramisu-gow-skye/docs/progress/delegate-jit-analysis.md.
