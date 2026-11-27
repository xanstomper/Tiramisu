# Changelog

## [0.2.5.1] — Dashboard preview, circular imports, and render diagnostics

- Restored and packaged the standalone controller-friendly dashboard preview.
- Integrated two-phase PE64 dependency loading and tested mutually importing
  DLLs and load rollback.
- Added D3D11 backbuffer identity and output-mask counters to the render report.
- Updated contributor and release documentation and removed internal prompt files.

[Complete 0.2.5.1 notes](docs/releases/0.2.5.1.md)

## [0.2.5.0] — USB and Device Portal imports, loader and middleware updates

- Added automatic USB discovery improvements and the Device Portal
  `LocalState/TiramisuGames` drop folder for unpacked games, disc images,
  and automatically imported ZIPs. Updated the beginner tutorial for all
  transfer paths.
- Added requester-aware dependency reports, on-demand root DLL loading for
  disc images, PE32 module and TLS work, and expanded Windows API coverage.
- Integrated the host refactor, FMOD and Wwise codecs, EOS and Galaxy
  fallback bridges, PE32 ABI additions, and state-preserving D3D11 blit.
  Added DirectStorage queues and broader rendering, input, and console
  diagnostics.
- Improved Steam install readiness, dashboard settings, atomic file uploads,
  and original Xbox One-family profiles. Recorded Series S evidence with the
  exact build and unreached milestones for each tested game.

The [complete 0.2.5.0 release notes](docs/releases/0.2.5.0.md) include setup,
all subsystems, known limitations, the commit history, and a file inventory.

## [0.2.4.12] — Steam Zstd chunks
- **Zstd-compressed Steam depot chunks now decode on Xbox.** The downloader
  understands Valve's `VSZa` container (8-byte magic + CRC header, inner Zstd
  frame, 15-byte `zsv` footer) and links the bundled decoder-only Zstandard
  1.5.6 library, so no external DLL is needed. This fixes newer depots that
  previously stopped with `unsupported Steam chunk codec VSZa (zstd not
  bundled on Xbox)`, including Bloons TD Battles and Terraria (both verified
  live) and BAPBAP.
- **Verified live on 0.2.4.12:** Bloons TD Battles (98.9 MB) and Terraria
  (766.7 MB) both downloaded through the `VSZa` path and reached `Ready to
  Play`; both had stopped at the first Zstd chunk on 0.2.4.11.
- The existing LZMA `VZa`, PKZip, and uncompressed chunk paths are unchanged.
- Added `docs/xbox-one-family.md`: hardware/API support map and acceptance
  matrix for Xbox One, One S and One X (untested targets).

## [0.2.4.11] — console verification
- **Steam depot download now works on Xbox Series S (live hardware proof).**
  HoloCure - Save the Fans! (app 2420510, depot 2420511) downloaded
  350,567,919 bytes (334.3 MiB) through CM PICS + per-CDN token, extracted to
  `LocalState\games`, and appeared on the TV shelf as `x64 D3D11 Ready` with
  status `Ready to Play`. Peggle Extreme (app 3483, depot 3484) also
  downloaded its 23,356,380-byte depot and reported `Ready to Play`; both logs
  show nonzero manifest request codes and no CDN 401.
- **Steam manifest authorization fixed:** the manifest request code is now
  requested from the CM with the unified
  `ContentServerDirectory.GetManifestRequestCode#1` service call instead of
  the WebAPI GET that returned an empty code. The public branch omits
  `app_branch`, matching SteamKit, because sending `"public"` is treated as a
  branch mismatch. Without a valid code the CDN returns HTTP 401.
- **Per-host CDN auth tokens:** each selected content server now gets a
  `ContentServerDirectory.GetCDNAuthToken#1` token, which is appended to
  manifest and chunk URLs as `?token=`. CDN URLs use the server's `vhost`
  (the host SteamKit uses for content requests) instead of the edge `host`,
  and manifest/chunk failures now log the token state.
- Prior fixes landed on the way: stack-lifetime crash in CM WebSocket
  callbacks, PICS appinfo field numbers, brace-depth depot parsing, CM unified
  manifest-request-code + per-host CDN auth token, vhost URLs, and Adler-32
  chunk verification. No DRM bypass; games you own only.

## [0.2.4.8]
- **Steam depot chunk verification fixed:** the downloader compared a SHA-1 of
  the decrypted plaintext with the manifest's chunk ID, but that ID is the CDN
  address and does not hash the plaintext, so every title failed at its first
  chunk with "chunk SHA-1 verification mismatch". Chunks are now checked against
  the manifest's Adler-32 (zero seed) and uncompressed length, matching Steam's
  `DepotChunk` contract, with the compressed length checked before decryption.
- **VZ chunk footer parsing fixed:** the decompressed size is the four bytes
  before the trailing `zv` marker, not the last four bytes, and the LZMA stream
  ends before the 10-byte footer.
- **Manifest request code parsing:** accept the code as either a JSON string or
  a number.

## [0.2.4.2]
- **Steam depot discovery fixed:** `ClientPICSProductInfoRequest` supplies a
  source job ID so the CM associates the response with the request, and reads
  appinfo from the response `apps` message. (`apps` is field 2; field 1 is
  `packages`, so the previous "wrong field" diagnosis was backwards.)

## [0.2.4.1]
- **Steam download crash fixed on console:** the CM WebSocket `MessageWebSocket`
  callbacks captured a stack-allocated session that was destroyed before the
  delegates were; the session is now shared and keept alive by its callbacks.
- **Steam appinfo parsing rewritten:** scan the `depots` block by brace depth,
  keep only Windows-eligible depots (ignore Linux/macOS-only and redist), and
  choose the largest public manifest. Console test with HoloCure (2420510) no
  longer crashes and no longer reports "no Windows depot found" for every title.

## [0.2.4.0]
- **Pre-launch PE compatibility badges (`/games` & TV UI):** Dashboard scans every game executable (`x64` vs unsupported `32-bit x86`, D3D11/D3D9/Vulkan/OpenGL/D3D12, EasyAntiCheat/BattlEye detection) and flags unsupported builds clearly before launch. Xbox GDK/GameOS console payloads (`MicrosoftGame.Config`, `gameos.xvd`, `xgameruntime`) are now flagged as unsupported instead of looking like D3D11 games.
- **One-click Save Backups & Restore (`/api/backup-saves`, `/api/restore-saves`):** Download or restore `drive_c`, `profiles`, and `settings.ini` as a `.zip` directly from the LAN dashboard before Dev Mode package updates wipe `LocalState`.
- **One-click Debug Bundle (`/api/debug-bundle`):** Export `tiramisu-debug-bundle.zip` (`tiramisu.log`, `settings.ini`, crash reports, PE compatibility summary) for fast GitHub bug/compatibility reports.
- **Mobile & Safari Folder Upload Guidance (Issue #1):** Detect unsupported `webkitdirectory` pickers on mobile browsers and guide users cleanly to `.zip` upload or Xbox USB drives.
- **KBM edition preserved in every release:** `tools/release.sh` builds and packs both `Tiramisu.msix` + `dev.cer` (public `Xan.Tiramisu`) and `Tiramisu-EdgeSpoof.msix` + `edge-dev.cer` (experimental Edge identity for Xbox hardware-mouse passthrough). The Edge/KBM artifact is never dropped from the release; `tools/pack.sh EDGE_SPOOF=1` now actually stages, signs, and verifies it again (the support had regressed out of pack.sh).
- **Documentation & Community Templates:** Added Windows/browser Xbox Device Portal install guide, Game Compatibility & Bug Report `.github/ISSUE_TEMPLATE` forms, and scope FAQ in `README.md`.

## [0.2.3.9]
- **Obsidian & Champagne UI + XMB Silk Waves:** Rebuilt Direct2D/D3D11 console UI with 4-card widescreen cinema shelf, 1920px HD Steam hero banners, 0.34s hero crossfades, animated 5-ribbon XMB wave background, procedural XAudio2 UI SFX, and 2-step safe game uninstall.
- **HD Boot Animation & Chrome App Branding:** Added `1280x720 @ 24 fps` stereo startup animation (`boot_anim.tanim`), transparent metallic chrome icon (`package/branding/logo.png`), and `#000000` splash handoff.
- **Xbox Storage & Transfer Reliability:** Initialized `LocalState\games` and `LocalState\incoming` at boot, preserved USB drive games across rescans, hardened `/api/download-url` (`.zip` + `.iso/.bin/.img`), and auto-persisted Steam AppIDs.
- Fixed Celeste (managed XNA titles) crashing with `Could not load Microsoft.Xna.Framework.Game`: package FNA substitute assemblies into `<install>/fna` in `tools/build-host.sh` and default Mono's `assemblies_path` to them in `src/trt/mono_host.cpp`.
- Added DirectStorage (`dstorage.dll`, `dstoragecore.dll`), Bink 2 (`bink2w64.dll`, `binkw64.dll`), Oodle (`oo2core_*`), and geo/locale bridge stubs, plus `tools/auto-compat.py`.

Format: [Keep a Changelog](https://keepachangelog.com/). Versions follow the
AppxManifest `Version` field (`Major.Minor.Patch.Build`).

## [Unreleased]

### Added
- Offline console pipeline rehearsal: `tests/py_tools/fake_portal.py` (TLS, CSRF,
  basic auth) + `tests/py_tools/test_console_e2e.py` (install → upload →
  rerun-skip → launch → proof, plus error paths).
- Generated title matrix: `tools/title-matrix.py` (`--check`) +
  `docs/proof/titles.json`, with `<!-- title-matrix:measured -->` markers in
  `docs/title-matrix.md`. `console.py proof` now points at `proof.json` +
  the generator.
- FSB5 hardening: `trt_fsb5_header_sane` / `trt_fsb5_sample_sane` envelope
  checks, 64-bit name-table compare, `bad_alloc` guards in `fsb_parse` and
  `get_decoded`. Offline `tests/test_fsb5_sanity.cpp` (15 checks) wired as
  build-test §43c.
- Console-usable FSB5 toggle: `fsb_decode` setting (ini key + per-game
  `tiramisu.profile.ini`), pushed from `main.cpp` via `trt_fsb5_set_console_decode`.
- `game-stage.py --set KEY=VALUE` for per-game ini + `settings` in `staging.json`.
- `docs/known-issues.md`, `docs/troubleshooting.md`, `docs/crash-reporting-plan.md`.

### Fixed
- `console.py` now prints `ERROR: …` on connect failure instead of a traceback.
- `devportal.py` misleading 401 message corrected.

### Changed
- `docs/title-matrix.md`, `GAP-MAP.md`, `docs/04-support-roadmap.md`, `README.md`
  updated to reflect measured vs. unmeasured state.

## [0.2.2.171] — current shipped build

Baseline. See `git log` for prior history.