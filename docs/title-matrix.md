# Title matrix — what actually runs on the console

> One row per title, one rule: **a row exists only if an artifact exists.**
> Screenshot, log and `proof.json` live in `docs/proof/<name>/`. A row with no
> evidence is deleted, not softened.

Why this file exists: "most game support" is only a claim until it is a table
someone else can check. Competing projects publish screenshots and frame
timings; this is Tiramisu's equivalent, generated from the same tooling that
launches the games.

## Measured

This table is generated — do not hand-edit it. Facts live in
`docs/proof/titles.json`; machine columns are overwritten by
`docs/proof/<dir>/proof.json` when a run produced one. Regenerate with
`python3 tools/title-matrix.py`, and `--check` verifies every cited artifact
exists (that is the only thing standing between this table and wishful
thinking).

<!-- title-matrix:measured:begin -->
| Title | Build | Machine | Boots | Renders | Audio | Input | Playable | Evidence |
|---|---|---|---|---|---|---|---|---|
| RACCOIN | Unity 6 IL2CPP, D3D11, Steam | PE32+ x64 | yes | yes | not measured | on-console pad | **yes** — menu + a full run (Round 8/15) | `docs/proof/raccoin-main-menu.png`, `docs/proof/raccoin-gameplay.png`, `docs/proof/raccoin-gameplay.mp4` |
| Celeste v1.4.0.0 | XNA/Mono (FNA3D), managed .NET | PE32+ x64 + embedded Mono | yes | yes | **silent** — FSB5 Vorbis decode is disabled on console after it heap-corrupted | on-console pad | **yes** — title, menu, Chapter 1 gameplay | `docs/proof/celeste-title.jpg`, `docs/proof/celeste-menu.jpg`, `docs/proof/celeste-gameplay.mp4` |
| Cuphead | Unity, D3D11 | PE32+ x64 | yes — guest GPU work verified | **no visible game frame** — dark guest framebuffer on 0.2.4.47 | not measured | game response unverified | **no** — menu unreached | `docs/proof/xbox-series-s-2026-11-26-launch-check.json` |
| Sekiro | Windows PC | PE32+ x64 | guest entry invoked on 0.2.4.47 | **no** — process exits before D3D11 initialization | not measured | not reached | **no** — menu unreached | `docs/proof/xbox-series-s-2026-11-26-launch-check.json` |
| God of War (PC) | AAA in-house engine, D3D11 | PE32+ x64 | yes | **yes** — opening forest across four boots | not measured | not reached | **no** — renders, does not yet reach gameplay | `docs/proof/gow-forest-01.webp`, `docs/proof/gow-forest-02.webp`, `docs/proof/gow-forest-03.webp`, `docs/proof/gow-forest-04.webp` |
<!-- title-matrix:measured:end -->

What the table does not say: frame-rate numbers. RACCOIN and Celeste run at
50–60 fps on the console, but the recorded clips are assembled from Device
Portal screenshots at 2–5 fps, which is the capture rate, not the game's. Do
not quote the clip rate as the game's.

## Queued (staged, not yet proven)

| Title | Why it is here | Blocker |
|---|---|---|
| Factorio 2.0 (x64, D3D11) | The reference title another loader runs at 60 UPS with audio — the bar to match | Untested here. Needs `game-stage.py` + `console.py proof` |
| Any pre-2015 x86-32 title | Most of the PC catalogue | **W1**: PE32 execution is experimental via the in-tree x86-32 JIT; no 32-bit title has a console completion artifact yet |
| Xbox GDK / GameOS console builds | Often x64, but they import `xgameruntime`/`gamingservices` and ship `MicrosoftGame.Config` + `gameos.xvd`; they are console packages, not Win32 desktop games | **Not a target.** The dashboard now flags them `Xbox GDK / GameOS build (Unsupported)`; Tiramisu does not bypass platform or DRM checks |

## Adding a row

```bash
# 1. stage the game you own (offline; refuses PE32 by default)
python3 tools/game-stage.py ~/Games/Factorio_2.0.77 --out staging/Factorio \
    --exe bin/x64/factorio.exe --args "--load-game save.zip"

# 2. put it on the console
python3 tools/console.py upload --game staging/Factorio \
    --console <ip> --user <user> --password <password>
python3 tools/console.py launch --game staging/Factorio --console <ip>

# 3. capture the evidence (screenshot + tiramisu.log + proof.json)
python3 tools/console.py proof --game staging/Factorio --console <ip>

# 4. add (or keep) the title in docs/proof/titles.json, fill the columns only
#    a human can answer (build, audio, input, playable), then regenerate
python3 tools/title-matrix.py
```

`proof.json` records the console OS build, the package version, the exe's
sha256 and the machine type, so two different builds of a title cannot be
confused later. `tools/title-matrix.py --check` fails when a row cites an
artifact that is not on disk, or when a `proof.json` has no row — that is how
the table stays a claim someone else can check.

## Where the honest gaps are

Ranked by how much of the catalogue each one gates (from
`docs/04-support-roadmap.md`):

1. **x86-32 execution** — blocks most pre-2015 PC games. The x86-32 JIT is in
   the tree and gated, but PE32 remains experimental until a real title has a
   console completion artifact.
2. **Console audio decode** — Celeste's FSB5 Vorbis path is disabled because it
   corrupted the heap on a Series S. The offline decode is real and tested, and
   the corrupt-header shapes that could reach the heap are now refused by name
   (`trt_fsb5_header_sane` / `trt_fsb5_sample_sane`, `test_fsb5_sanity`), but
   the fault was never reproduced under a debugger, so decode stays off by
   default. `fsb_decode=1` (settings.ini, or `game-stage.py --set fsb_decode=1`
   for one title) enables the hardened path for a run: when that run has a
   `proof.json` with audio checked by ear, this line changes. Until then a
   playable title can still be silent, and it is written down that way.
3. **D3D12 execution** — the offline triangle fixture exercises draw
   submission and Present. A real DX12-only game still needs an Xbox run
   that verifies its visible menu and controller response.
4. **Video decode** — no frames; cutscene-driven titles stall or skip.
5. **Wwise** — the built-in fallback is a stub (no bank decode); a game-shipped native `AkSoundEngine.dll` is preferred, but without it Wwise titles lose all audio.
