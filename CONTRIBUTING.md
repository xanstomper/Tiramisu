# Contributing to Tiramisu

Welcome. Tiramisu is a community project — every bridge, every game profile,
every doc fix helps. Here's how to contribute effectively.

## Before you start

1. **Reproduce the issue.** Record the evidence, expose unsupported features
   through explicit errors, and include a regression check for compatibility fixes.
   Respect the game's entitlement and security boundaries.
2. **Read the gap map:** `GAP-MAP.md` — what's done, what's open, what's blocked.
3. **Read the evidence:** `docs/03-evidence.md` — what is proven vs. assumed.

## Development setup

```sh
# Linux dev host (cross-builds for Xbox)
git clone https://github.com/<org>/tiramisu.git
cd tiramisu

# Prereqs: LLVM 19 (clang-cl, lld-link), xwin splat, makemsix, osslsigncode
# See README.md "Build" section for full list.

# Run the offline gate (must pass before any PR)
tools/build-test.sh

# Cross-build the UWP host
tools/build-host.sh
```

## Workflow

1. **Measure first.** Reproduce the issue with a corpus PE + the depth ladder
   before writing any code. The ladder is `CRT → WINMAIN → UNITYMAIN2 → WINDOW →
   D3D11 → PUMP → QUIT → EXIT(0)`.

2. **One session, one gate.** `build-test.sh` runs ONE at a time. Check for live
   writers first: `find . -type f -mmin -15 -not -path './.git/*'`.

3. **Fail named, never silent.** Every stub must log a named failure. Never
   return a fake success.

4. **Land with tests.** Every code change needs a test or a measured boot-depth
   improvement. "It compiles" is not done.

5. **Update docs.** If you change behavior, update `GAP-MAP.md` and
   `COMPATIBILITY.md`.

## What to contribute

### High-impact (always welcome)
- **Game profiles** — `tiramisu.profile.ini` for games you've tested. See
  `docs/game-profiles.md`.
- **Compatibility reports** — run a game through the ladder, document the depth
  reached and the named gap. Add to `COMPATIBILITY.md`.
- **Bridge implementations** — missing Win32 APIs that a game needs. Check the
  log for `FAIL IMPORT (unresolvable symbol)` — that's your work item.
- **Documentation** — architecture notes, troubleshooting, setup guides.

### Medium-impact
- **Performance tuning** — frame pacing, memory pressure, shader caching.
- **Input mapping** — new controller layouts, keyboard→gamepad mapping.
- **Audio fixes** — new WASAPI edge cases, XAudio2 buffer sizing.

### Please don't
- Add DRM/anti-cheat bypasses (contract rule, always).
- Commit game binaries (gitignored, and legally problematic).
- Commit secrets (`.env`, tokens, certs).
- Modify the sibling lane's files without coordination (see below).

## Lane discipline

Two sessions share this repo. Coordinate:
- **My lane (d3d/boot-depth):** `src/trt/bridge_d3d*.cpp`, `bridge_vulkan.cpp`,
  `bridge_wasapi.cpp`, `bridge_kernel32.cpp` (CV/wake), boot-depth ladder,
  Ultra Pool / RACCOIN work.
- **Sibling lane (kernel/mem/ole/d3d12):** `bridge_mem.cpp`, `bridge_d3d12.cpp`,
  `bridge_kernel32.cpp` (other parts), `bridge_ole.cpp`, `bridge_ntdll.cpp`.

If you're unsure which lane owns a file, check `git log --oneline -3 -- <file>`.

## Adding a game to the compatibility database

1. Drop the game folder into `corpus-local/<name>/` (gitignored)
2. Run: `./tests/out/test_corpus_run --exe corpus-local/<name>/<exe>.exe`
3. Read the boot-depth ladder in the log
4. Add an entry to `COMPATIBILITY.md` with the measured depth
5. If the game needs specific settings, add a profile to `docs/game-profiles.md`

## Code style

- C++17/20, clang-compatible
- `TRT_GUEST_ABI` (ms_abi) on every bridge function called by the guest
- Fail-visible: `trt_core_log("name: what -> why")` on every failure path
- No silent successes; no fake data
- Tests: `CHECK(cond, "description")` macros, `RESULT: ALL PASS` at the end

## Submitting a PR

1. Fork + branch from `master`
2. Run `tools/build-test.sh` — must be green (rc=0)
3. Run `tools/build-host.sh` — must produce the MSIX
4. Update `GAP-MAP.md` and `COMPATIBILITY.md` if behavior changed
5. Describe what you measured, not what you think

## Getting help

- `GAP-MAP.md` — live gap table with dependency order
- `docs/03-evidence.md` — evidence vs. assumptions
- GitHub Issues — for bugs and feature requests
- The dashboard (port 9119) and Terminus (port 9120) for live debugging