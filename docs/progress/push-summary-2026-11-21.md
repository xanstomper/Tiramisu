# Tiramisu — full push record, 2026-11-21 ~04:10 EST

Ben order (2026-11-21 03:57 EST): push EVERYTHING done in the last 2 days
(Nov 19–21) on Tiramisu — from Codex, OpenCode, Hermes, and every agent.
Everything updated/changed that had not been pushed, push it. This also
answered the open preserve-or-delete question on the unlanded engine
worktrees: PRESERVE by pushing.

Repo: https://github.com/xanstomper/Tiramisu.git
All pushes ran ON mineplex through the Skye bridge. Never force-push.

## What was already pushed (by opencode, ~03:55 EST, before this run)

- master = 836e7e5 (was a616dd2): brown-merge merged (bed806c), Celeste
  boots/renders (2fe1a4e), manifest 0.2.2.144 (f79dd34), MONO_DOOR_EXPORT
  (6a53616), README + proof screenshot (836e7e5).
- work/brown-merge = f79dd34, work/gow-cycle-recovery = 4c49c82,
  work/gow-host-debug = 4ae3a13, work/boot-ultrapool = d0130a2,
  work/raccoin-native-input = 21d8747, work/raccoin-ui = e3efe1e.
- Verified by this run: local == origin for every branch (fetch + compare).

## What this run pushed

### work/celeste-105-diagnostics — 019ed38 (1 commit, 2 files)

From ~/tiramisu-celeste-test (detached HEAD 20a4662, dormant tree): the
.105/.106 Mono crash-diagnostic patch — SEH-guarded mono_jit_init with
exception-code/fault-address reporting + 24-frame CaptureStackBackTrace
logged as RVAs (src/trt/mono_host.cpp, +86/-3) and the AppxManifest bump.
Untracked third_party build/ dirs (faudio/build, fna/gen, fna3d/build,
mono-bridges/gen) intentionally excluded. Secret scan clean.

### 13 work/snapshot-* branches — 390 files total

Source: the 16 NO-GIT ~/tiramisu-woowop-engine-* dirs + gow18/gow19 +
woowop-fix. Method: content-hash every text file in each snapshot
(excluding build outputs, binaries, files >1 MB, TIRAMISU-MASTER-PROMPT.md)
and keep only files whose blob is reachable from NO pre-push ref — i.e.
content that exists nowhere in repo history. Each distinct snapshot gets
its own branch; identical dir pairs share one branch (noted in the commit
message). Snapshot branches are preservation-only, do not merge.

| Branch | Commit | Files | Base | Source dir(s) |
|---|---|---|---|---|
| work/snapshot-gow18 | c9a0c3755 | 24 | 4c49c82 | tiramisu-cycle-recovery-gow18 |
| work/snapshot-gow19 | ef2e34b61 | 25 | 4c49c82 | tiramisu-cycle-recovery-gow19 |
| work/snapshot-woowop-fix | 6a4cecb3e | 26 | 4c49c82 | tiramisu-woowop-fix |
| work/snapshot-engine | 860716aed | 31 | 836e7e5 | engine, engine-m7p1 |
| work/snapshot-engine-94 | 6a5320e7f | 28 | 836e7e5 | engine-94 |
| work/snapshot-engine-95 | 58729650d | 27 | 836e7e5 | engine-95 |
| work/snapshot-engine-audit1 | f0c3f2af2 | 30 | 836e7e5 | engine-audit1 |
| work/snapshot-engine-audit2 | 94ee515f5 | 29 | 836e7e5 | engine-audit2 |
| work/snapshot-engine-celeste | 5100ee97f | 30 | 836e7e5 | engine-celeste, engine-celeste1 |
| work/snapshot-engine-fna | fbf53465f | 38 | 836e7e5 | engine-fna, engine-fna1 |
| work/snapshot-engine-fna3d | bcfebc627 | 36 | 836e7e5 | engine-fna3d, engine-fna3d1 |
| work/snapshot-engine-mono | 00b86a041 | 36 | 836e7e5 | engine-mono, engine-mono1 |
| work/snapshot-engine-vk | 41eb636d2 | 30 | 836e7e5 | engine-vk, engine-vk1 |

Notes:
- woowop-fix, fna, fna3d, mono have 2 commits each: the first pass
  under-counted (a rescan after the first pushes masked files whose blobs
  were already in the object DB); a second pass recomputed uniqueness
  against the pre-push reachable set and appended the rest as fast-forward
  commits (+22, +30, +30, +30). Same for 94 (+1) and 95 (+1), whose first
  scan missed one file each (blob present in the DB but unreachable from
  any ref). A final verification pass over all 13 groups reports missing=0.
- The unique content is the unlanded snapshot work: main.cpp variants
  (5 distinct), pe_loader.cpp variants (7 distinct), images.cpp,
  bridge_steam.cpp, bridge_membudget.cpp, bridge_shell.cpp, webxfer.cpp,
  module_table.cpp, pe.h, AppxManifest.xml, tools/build-test.sh, plus
  tests/out/ artifacts and (fna/mono groups) third_party/fna-stubs
  generated files.
- Secret scan: the only hits were the words "password" in
  third_party/fna-stubs/gen/gen_steam.c — the Steamworks API function
  name ISteamGameServer_SetPasswordProtected (false positive, no
  credential). TIRAMISU-MASTER-PROMPT.md (contains a DP_PASS line) was
  excluded from every snapshot. No secrets committed.
- woowop-fix shares its .git pointer with the cycle-recovery worktree
  (copied dir); no git write commands were ever run from inside it — its
  content was committed via plumbing from ~/tiramisu.

## Not pushed (with reasons)

- Brown-merge .144 input-diagnostics work: the test agent
  (celeste-continue-3) is ACTIVELY editing ~/tiramisu-brown-merge right
  now (1 dirty file at ~04:05 EST). Left alone per the coordination rule;
  it pushes when the agent commits. work/brown-merge itself is pushed
  at f79dd34.
- ~/tiramisu-cycle-recovery-gow18/gow19 are NOT tiramisu repos — they sit
  inside the home dotfiles repo (~/.git, 4244 dirty lines, Ben's whole
  home dir). That repo was NOT touched. Their Tiramisu content is
  preserved via snapshot-gow18/gow19 above.
- ~/tiramisu-land95: absent (removed in an earlier cleanup).
- ~/tiramisu-networkplay: the GoW source SSHFS mount, not a repo.
- No Hermes Tiramisu work exists in any tree (Hermes/vermes works on
  Mochi, a separate repo and out of scope for this push).
- Build outputs, MSIX packages, out-*/build/ dirs, binaries: excluded
  everywhere by rule.

## Other changes

- ~/tiramisu remote URL fixed to https://github.com/xanstomper/Tiramisu.git
  (capital T; was lowercase tiramisu.git). GitHub treats the names
  identically; this matches the pending parent decision (a).
- The raw scan file push-scan-results.jsonl was removed from the worktree
  after use (its rescan numbers were masked; the branches + this file are
  the authoritative record).

## Verification

- `git fetch origin` + per-branch compare: all pre-existing branches
  local == origin before this run.
- Every new branch verified after push: `git rev-parse origin/<branch>`
  == local commit; `git ls-remote origin` confirms all 14 new branches
  on GitHub.
- Final verification pass (snap-fix.py, all 13 groups): missing=0 for
  every group — every file version not reachable from a pre-push ref is
  on its snapshot branch.
