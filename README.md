<div align="center">

<img src="docs/banner-chrome.png" alt="Tiramisu UWP — PC games on Xbox Series X|S" width="100%"/>

# Tiramisu UWP

### Run PC games on Xbox Series X|S & Xbox One / One S / One X — natively.

**A Windows PE64 application runtime for Xbox Dev Mode.**
Wine's job, targeting UWP instead of POSIX.

![Status](https://img.shields.io/badge/status-RACCOIN%20playable%20%7C%20God%20of%20War%20renders%20%7C%20Celeste%20playable-brightgreen)
![Suites](https://img.shields.io/badge/offline%20suites-78-blue)
![Coverage](https://img.shields.io/badge/corpus%20imports-100%25%20real-brightgreen)
![Platform](https://img.shields.io/badge/platform-Xbox%20One%20%7C%20One%20S%20%7C%20One%20X%20%7C%20Series%20X%7CS-107C10?logo=xbox&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-green)
![DRM](https://img.shields.io/badge/DRM%20bypass-none%2C%20ever-red)

[Install](#-quick-start-xbox-dev-mode) · [What Works](#-before-you-download-what-works-today) · [Console proof](#console-proof-2026-11-19) · [What boots](#-what-boots-today) · [Build](#-build-from-source-linux) · [Issues](https://github.com/xanstomper/Tiramisu/issues/new/choose)

**[⬇ Download Latest Release](https://github.com/xanstomper/Tiramisu/releases)** · **[🎮 Compatibility Matrix](docs/title-matrix.md)**

</div>

> [!IMPORTANT]
> **Before you download — what works today:**
> - **x64 (`PE32+`) Windows games are the supported target. PE32 (`x86-32`) is experimental via the in-tree JIT.** ARM64 guests, kernel anti-cheat (EasyAntiCheat/BattlEye), Xbox GDK/GameOS packages (for example a `MicrosoftGame.Config` + `gameos.xvd` console build), and heavy desktop productivity suites (After Effects, DaVinci Resolve) are **not** supported.
> - **APIs today:** D3D11, D3D9/D3D8, Vulkan (via SPIR-V→HLSL→D3D11), and OpenGL. **D3D12 execution, Bink video cutscenes, and Wwise audio** are still incomplete/experimental.
> - **Celeste v1.4.0.0 is playable but silent.** FSB5 Vorbis decode is disabled on console after it heap-corrupted on a Series S; `fsb_decode=1` is test-only. See [known issues](docs/known-issues.md).
> - **Xbox One family profiling (offline-tested):** Native CPUID/RAM detection and host profile defaults cover Original Xbox One (Durango), Xbox One S (Edmonton), and Xbox One X (Scorpio). The 32 MB ESRAM path is budget accounting only, and none of this has been verified on One-family hardware yet. See [docs/xbox-one-family.md](docs/xbox-one-family.md).
> - **Set Dev Home to Game mode (5 GB RAM)** and **back up your saves** via the LAN dashboard (`Saves & Support → Backup Saves (.zip)`) before uninstalling or updating Tiramisu — Xbox Dev Mode wipes `LocalState` on uninstall.

---

## What is this?

One signed UWP app that loads Windows `.exe`s **in-process** on Xbox
Series X|S Dev Mode. No CPU emulation for 64-bit games: the console is x64,
so guests run at **native speed** on the same core, the same ABI, the same
process — Tiramisu is the operating system around them.

```text
Tiramisu UWP host (CoreWindow + D3D11 flip + XInput/GameInput, 5 GB as a Game package)
  └── trt — Tiramisu runtime
        ├── PE64 loader      map → sections → relocate → bind imports → TLS → delay imports
        ├── Win64 EH         .pdata personality, RtlUnwindEx, funclets, VEH
        ├── module table     every import routed: SYSTEM | BUILTIN | STUB
        ├── ABI ratchet      every claimed symbol answers the dispatcher's argument count
        ├── bridges          Win32 families implemented on UWP
        │   ├── graphics     D3D11/DXGI (real device passthrough), D3D12 (real on console,
        │   │                software runtime offline, root-signature serialization),
        │   │                D3D9/8/DDraw→D3D11, D3DX9/10/11, OpenGL→D3D11 (gl4es-derived),
        │   │                Vulkan 1.0–1.3 ICD (SPIR-V→HLSL→D3D11, on-disk shader cache),
        │   │                GDI32, GDI+ (launcher UI)
        │   ├── audio        XAudio2 / DirectSound / WASAPI / OpenAL / winmm waveOut+waveIn /
        │   │                MIDI synth / FMOD → one real XAudio2 2.8 mixer;
        │   │                msacm32 decodes ADPCM and u-law/A-law instead of refusing
        │   ├── system       kernel32 core+mem+thread, ntdll, advapi32 registry, ole32/oleaut,
        │   │                mfplat, shell32/shlwapi, comctl32, crypt32, bcrypt, wininet,
        │   │                psapi, iphlpapi, imm32, dbghelp, setupapi/HID, winrt (Gamepad…)
        │   └── steam        QR sign-in to YOUR account, WebAPI library/friends,
        │                    CM WebSocket + CDN depot download — no DRM bypass, ever
        ├── VFS              C:\ → LocalState\drive_c, D:\ → mounted .iso, Z:\ → package
        ├── budget governor  one 5 GB ledger for VirtualAlloc commit + heap + mapped views;
        │                    read-only file views charge ADDRESS SPACE only (SSD↔RAM overlap)
        ├── async I/O        overlapped reads → GQCS packets, mmap fast path (TRT_IO_MMAP_MIN)
        └── forensics        crash bundles (fault, registers, stack, settings, log tail) +
                             a 1 Hz watchdog that names a wedged guest before killing it
```

Two ideas carry the whole design:

- **Fail visible, never silently.** Every unimplemented import binds to a
  thunk that logs itself and names the gap; every over-budget allocation is
  refused with a name; every stub hit in a log is roadmap work. A game that
  cannot do something should say so, not corrupt itself quietly.
- **The console already has the hardware.** D3D11, D3D12, XAudio2 and the CPU
  are real. Tiramisu translates API *semantics*, not instructions.

## Console proof (2026-11-19)

RACCOIN, God of War and Celeste are running on an Xbox Series S in Dev Mode, loaded by
the signed Tiramisu MSIX. No CPU emulation, no streaming, no PC in the loop.

### RACCOIN — playable

RACCOIN is a Unity 6 IL2CPP build using D3D11. It reaches its own menu and
plays a run.

<table>
<tr>
<td width="50%"><img src="docs/proof/raccoin-main-menu.png" width="100%" alt="RACCOIN main menu on Xbox"/><br/>
<sub><b>Main menu.</b> Play Game, Collection, Profile, Quit. Version 1.0.15.</sub></td>
<td width="50%"><img src="docs/proof/raccoin-gameplay.png" width="100%" alt="RACCOIN gameplay on Xbox"/><br/>
<sub><b>Run in progress.</b> Round 8/15, Current 392, Combo 35, a full coin bed.</sub></td>
</tr>
</table>

[![RACCOIN gameplay, 7.2 s capture](docs/proof/raccoin-gameplay.gif)](docs/proof/raccoin-gameplay.mp4)

Over the clip the HUD goes from Current 116 to 392, Top 1 from 22 to 61, Hand
In from 44 to 19, and Score Rate from 1.0 to 2.8, while the coin bed keeps
moving. The last frame matches the gameplay still, so both come from the same
run. The clip is 36 frames captured at 5 fps; that is the capture rate, not
the game's frame rate.

### Celeste — playable

**Celeste v1.4.0.0** (XNA/Mono, FNA stack) is playable on the Series S — the
first **managed .NET game** to run on Tiramisu. The 32-bit engine embeds a
UWP build of Mono 6.8.0.105 and drives FNA3D against the host's real D3D11
device + CoreWindow swapchain.

<table>
<tr>
<td width="50%"><img src="docs/proof/celeste-title.jpg" width="100%" alt="Celeste title screen on Xbox"/><br/>
<sub><b>Title screen.</b> Mountain logo, icy letters, snow particles. v1.4.0.0.</sub></td>
<td width="50%"><img src="docs/proof/celeste-menu.jpg" width="100%" alt="Celeste main menu on Xbox"/><br/>
<sub><b>Main menu.</b> Climb, Options, Credits, Exit — fully navigable.</sub></td>
</tr>
</table>

[![Celeste first gameplay test, in greyscale](docs/proof/celeste-greyscale-test.gif)](docs/proof/celeste-greyscale-test.mp4)

**The greyscale bug.** The first gameplay test rendered in full greyscale:
the PNG decoder computed the wrong bytes-per-pixel for RGB images (2 instead
of 3), scrambling the color-grading lookup tables Celeste loads as PNGs — so
the (correct) scene graded to grey and Madeline's red hair showed green.
Console tests plus a GPU LUT readback pinned it, and a one-line patch fixed
it ([`e776951`](https://github.com/xanstomper/Tiramisu/commit/e7769512dadd0d71f0dede486e283dfb49046f0f)).

[![Celeste Chapter 1 gameplay, in color](docs/proof/celeste-gameplay.gif)](docs/proof/celeste-gameplay.mp4)

**Chapter 1: Forsaken City, in full color.** Madeline runs, jumps and climbs
through the opening screen — navy sky, green pines, blue snow, red hair. The
clip looks low-framerate because it is assembled from Device Portal
screenshots captured at ~2 fps; the game itself runs smoothly at 50–60 fps
on the console. Audio is silent — FMOD FSB/Vorbis sample decode is disabled
on console because it heap-corrupted on the Series S.

### God of War — boots and renders the forest

God of War is the PC build, running on the same D3D11 path. These are four
separate boots, ordered by how much of the scene each one rendered.

<table>
<tr>
<td width="50%"><img src="docs/proof/gow-forest-01.webp" width="100%" alt="God of War forest, boot 1"/><br/>
<sub><b>1.</b> Scene appears. Heavy black occluders, missing foliage, broken materials.</sub></td>
<td width="50%"><img src="docs/proof/gow-forest-02.webp" width="100%" alt="God of War forest, boot 2"/><br/>
<sub><b>2.</b> Character materials resolve: skin, tattoo, cloth, axe, handprint, bark.</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/proof/gow-forest-03.webp" width="100%" alt="God of War forest, boot 3"/><br/>
<sub><b>3.</b> Geometry and depth return: denser foliage, layered trees, readable sky.</sub></td>
<td width="50%"><img src="docs/proof/gow-forest-04.webp" width="100%" alt="God of War forest, boot 4"/><br/>
<sub><b>4.</b> Full composite: god rays, layered foliage, character and axe together.</sub></td>
</tr>
</table>

The captures are 256×144 preview stills. They show the scene and the render
path, not final resolution or frame pacing.

### What the evidence does and doesn't cover

Proven: RACCOIN's own menu renders; a RACCOIN run executes with live game
state; God of War boots to the forest and renders a composite frame at preview
resolution.

Not shown: God of War's menu or gameplay, audio for either title, frame times,
long-session stability, save/profile round-trips, or 720p+ output for God of
War. Two titles are not universal compatibility.

### Console fixes behind these frames

- **Native TLS preserved during guest execution.** Without it, RACCOIN's D3D11
  resource creation stalled and the splash alternated with black frames.
- **One stable render target plus a host flip copy.** The guest renders into a
  single target; the host copies it into the Xbox flip chain. This removed the
  alternating-black-frame failure.
- **GC thread adoption.** Unity's collector suspends guest threads. The first
  run died with `Fatal error in GC: SuspendThread loop failed` because it was
  handed a pseudo-handle; the fix adopts the real thread and uses a
  park/verify/resume loop.
- **Native GameInput path.** Keyboard and mouse reach the foreground guest
  through the console's GameInput instead of a desktop-only assumption.
- **Manifest and memory-ledger changes for God of War.** The larger title
  needed host manifest work and a bigger slice of the 5 GB Game package.

## ⚙️ How it works

The stack answers nine separate questions, in the order a game asks them:
**load it, bind it, run it, fit it in memory, draw it, make it audible,
connect it, survive in it, and prove it.** Each section below is one of those.

### 1. Load — the PE64 loader

A real Windows PE64 is mapped from the game directory (or a mounted `.iso`):
DOS → NT headers → section mapping with per-section protections → DIR64 /
HIGHLOW relocations → import binding → TLS callbacks → **delay imports bound
eagerly** (resolvable symbols get real addresses; unresolvable ones get a
fail-visible thunk so the game boots and the log names the gap) → the whole
image is **W^X sealed** (code pages RX, no RWX ever — a two-phase JIT
fallback arms RW and flips RX on `FlushInstructionCache` for engines that
JIT, like Unity mono).

Before any of that, two scanners decide *what* is about to be loaded:

- **The pre-boot compat scanner** reads each shelf entry's PE machine type and
  full import table (RVA→file-offset through the section table) and flags
  `[steam-auth]`, `[d3d9 OK]`, `[d3d11 OK]`, 32-bit, managed — so a title that
  cannot start is labelled before it is launched instead of after it fails.
- **The engine synthesizer** classifies a transferred or local game folder by
  evidence: it parses the chosen executable's real import *and delay-import*
  tables, so a title is called Unity only when it actually imports
  `UnityPlayer.dll`/`mono`, FNA when it imports `FNA.dll`, D3D9 when it imports
  `d3d9.dll`, and so on (file names are a secondary signal, used only where an
  engine is loaded by name at runtime). It then synthesizes the graphics,
  audio, Steam sandbox and launch profile the title needs.

### 2. Bind — the module table

Every import routes to one of three classes:

- `SYSTEM` — the console's real DLL (proven-present families only; the host
  binds via `LoadLibraryW`+`GetProcAddress`),
- `BUILTIN` — Tiramisu implements it with Wine-observable semantics
  (kernel32 core/memory/thread, user32 window layer, graphics, audio, COM,
  registry, `mfplat`, `d3d12`, ~25 families, ~2,000 functions),
- `STUB` — exported, logs when called, returns 0: **fail-visible, never a
  silent lie**.

Guest-shipped DLLs (UnityPlayer, GameAssembly, SDL3, libstdc++, FMOD, Oodle,
Bink, DirectStorage…) load recursively as real PEs and their
`DllMain(PROCESS_ATTACH)` actually executes. Third-party middleware that only
needs to exist (`oo2core_*.dll`, `bink*64.dll`, `dstorage*.dll`) is claimed and
answered honestly, so the title takes its fallback path (standard
decompression, skipped video, standard I/O) instead of failing at load.

Three mechanisms keep binding from lying about itself:

- **Ordinal imports bind and are classified by their real identity.** An
  ordinal has no name, and the classifier used to fall through to
  `UNRESOLVED` even though the bind had succeeded (85 ordinals across the
  corpus: 45 oleaut32, 38 wsock32, 2 xinput1_3). Ordinals now classify as the
  `#<n>` export they bound, and the ordinals the corpus actually uses map to
  real semantics (oleaut32 conversions instead of a zero-filling
  `VariantInit` stand-in).
- **The dynamic API synthesizer** covers the unknown: a symbol or module that
  is not in any catalog gets a synthesized, fail-visible thunk with a deduced
  calling convention and a plausible return (a valid pseudo-handle, `TRUE`,
  `S_OK`, a version number) instead of `TRT_ERR_IMPORT` at load. A game loads
  and logs the gap rather than refusing to start.
- **The ABI conformance ratchet** is the guard rail. Tiramisu's worst failure
  mode is not a missing function — it is a *registered* function whose calling
  convention the x86 dispatcher must guess; a 1-argument stdcall guessed as
  4-argument pops 12 extra bytes and returns into garbage (OpenTTD jumped to
  `0x0000B5E3` exactly that way). Suite 50 enumerates the whole registry,
  requires an ABI answer for every symbol Tiramisu claims, and compares the
  remaining gaps against `tests/abi-gaps.baseline`: the list may shrink, never
  grow.

### 3. Run — native execution

The guest's entry point is called on a host thread at native speed. `trt`
provides Win32 thread semantics, sync primitives, TLS, real file enumeration,
async file I/O with completion ports, and the memory governor. Everything a
title can tune is read once per process and pushed into the bridges through
`extern "C"` hooks — and those hooks are now *proven* to be connected: they
used to be weak symbols whose getters lived in a C++ namespace, so six
documented knobs (OS personality, write-watch, pad vibration, input deadzone,
memory budget, audio buffer) silently reported their built-in defaults
whatever the user set. Suite 34b declares the hooks strongly and fails the
link if `settings.cpp` stops exporting or forwarding any of them.

Settings live in `LocalState\settings.ini` and in a per-title
`tiramisu.profile.ini` (the only channel a packaged app can read for a single
game). The dashboard and the phone web UI edit the same store over
`GET/POST /api/settings`:

| Key | What it controls |
|---|---|
| `default_input`, `mouse_speed`, `wheel_speed`, `input_deadzone`, `pad_vibration` | input mode (`auto`/`controller`/`kbm`/`pad2kbm`), pointer feel, pad rumble |
| `os_major`, `os_minor`, `os_build` | the Windows version reported to games (Win11 personality) |
| `write_watch` | `GetWriteWatch` kill-switch |
| `memory_budget_mb`, `fsb_decode`, `audio_buffer_ms` | the 5 GB ledger, console FSB5 decode, audio latency |
| `frame_rate_cap`, `vsync`, `resolution_scale`, `texture_quality` | presentation pacing, render scale, anisotropy tier (§5) |
| `shader_cache`, `log_lines`, `crash_report`, `steam_auth` | SPIR-V cache, log ring size, crash bundles (§8), Steam sign-in |

### 4. Fit — the 5 GB ledger and the SSD↔RAM overlap

A packaged UWP app on Xbox gets **5 GB of commit**. Games do not politely stay
under it: a Unity title maps its `StreamingAssets` bundle, an Unreal title maps
`.pak`, and most engines call `CreateFileMapping` + `MapViewOfFile` on a rolling
window over a multi-GB archive to read whatever the next level needs. Copying
each window into private memory charges that window against the same 5 GB the
game needs for textures, meshes and heaps — so the archive competes with the
game for the budget, and the game loses.

Tiramisu answers the two questions the guest actually asks separately:

| Question the guest asks | API | What it costs |
|---|---|---|
| "How much may I **commit**?" | `VirtualAlloc`, heap, anonymous mappings | the 5 GB ledger, byte for byte |
| "How much may I **address**?" | read-only `MapViewOfFile` over a real file | address space only (`TRT_MEM_VA_BUDGET_MB`, 64 GB default) |

A read-only view over a real file is **mapped, not copied**:
`mmap(PROT_READ, MAP_PRIVATE, fd, page_aligned_offset)`. The view's pages *are*
the file's clean page cache — the bytes the SSD already delivered — so:

- the window costs **address space, not commit**. A 4 GB archive streams through
  the ledger instead of being copied into it, and the game keeps the 5 GB for its
  own allocations;
- the pages are **shared** with every other reader of the same file and are
  **reclaimable for free**. Under pressure the kernel drops them and the next
  touch re-faults them from the SSD — a slower frame beat instead of a refused
  allocation (`OutOfMemory` / a failed `MapViewOfFile`) that ends the session;
- `madvise(MADV_WILLNEED)` starts the SSD→page-cache read **asynchronously**
  when the view is created, so the first touch usually hits resident pages
  (`TRT_MAP_READAHEAD=0` turns the hint off);
- nothing is written back: a read-only view has no divergent state to lose, and
  the archive stays byte-identical — the suite reads the file back after the
  views are gone and compares it byte for byte.

The copy path stays, because three cases cannot be mapped:

- **write, `FILE_MAP_COPY` and `FILE_MAP_EXECUTE` views** — a copy-on-write
  view is writable through its own private pages (mapping it read-only would
  fault the guest's first store), a write view must never land in the archive,
  and an execute view needs executable protection. Write views are flushed back
  on `FlushViewOfFile` / `UnmapViewOfFile` exactly as Windows guarantees; COW
  pages are discarded with the view, as Windows does;
- **views that reach past EOF** — a mapped page beyond the end of the file would
  fault with `SIGBUS` on touch, so the window is copied and **zero-filled** to
  the requested length;
- **non-regular files and non-Linux hosts** (the console build) — there is no
  host descriptor to map, so the private-buffer path runs unchanged. A build
  that cannot map behaves exactly as it did before.

Per-view bookkeeping keeps the ledger exact. Every view records what it actually
charged (`charge_span`, VA-only for file views, the full window for copy views)
and what it owns (`munmap` base + span, or a heap block), so `UnmapViewOfFile`
and LRU eviction discharge exactly that and release exactly that: a file view is
never `free()`d, a copy view is never `munmap`ed. The governor's one-line report
separates the two pools:

```text
memory budget: 1834/5120 MB committed (612 MB mapped views,
               2880 MB file-backed page cache, peak 2049 MB, 0 refusals;
               VA 4210/65536 MB reserved, peak 4822 MB, 0 refusals)
```

Everything is read once per process, so a title can be diagnosed without a
rebuild:

| Variable | Default | Meaning |
|---|---|---|
| `TRT_MAP_MODE` | `file` | `copy` restores the private-copy path for **every** view (diagnosis / A-B a title) |
| `TRT_MAP_READAHEAD` | on | `0` disables the `MADV_WILLNEED` SSD prefetch hint |
| `TRT_MEM_BUDGET_MB` | `5120` | commit ledger size — the console's Game-package limit |
| `TRT_MEM_VA_BUDGET_MB` | `65536` | address-space budget for mapped views |
| `TRT_IO_MMAP_MIN` | `1048576` | reads at or above this size take the mmap fast path |

The first read-only view logs the active strategy, so a console log always says
which one a title got:

```text
trt: mem: read-only map mode = file — views are file-backed page cache
     (mapped, not copied; clean pages reclaimable, re-faulted from the SSD;
      TRT_MAP_MODE=copy restores the copy path)
```

What this changes in practice: an archive window that used to consume commit
now consumes VA, so titles whose data files are bigger than the budget can
stream them; the pages the game is actually using stay hot in the page cache
while the ones it has moved past are free to drop; and the failure mode for a
too-large working set becomes a slower read from the SSD rather than a refused
allocation. Offline proof is suite 41b (`tests/test_mem_mapfile.cpp`): bytes
served from the archive, no commit charged, VA charged, misaligned offsets,
write and copy-on-write views staying on the copy path (a COW view takes a
real guest write), cross-EOF zero-fill, the archive left byte-identical, and
both ledgers returning to baseline — plus a re-exec probe that proves
`TRT_MAP_MODE=copy` restores the old accounting.

### 5. Draw — the console passthrough

The guest's `D3D11CreateDevice` gets the **host's real console device**;
`CreateSwapChainForHwnd` (and every other swapchain-creation variant) gets a
proxy over the **host's real CoreWindow swapchain** — `Present`,
`GetBuffer`, `ResizeBuffers` forward to real console DXGI. D3D9/D3D8/DDraw
games are translated onto the same backend. Vulkan games run through the
built-in Vulkan 1.0–1.3 ICD: SPIR-V → HLSL → D3D11 on the console GPU.
OpenGL games ride the gl4es-derived GL→D3D11 path. Shader execution is
**the console's real D3D11** — no shader translation needed for D3D11 guests.

D3D12 has two implementations behind one interface, because it has two jobs:

- **on console** it forwards to the real hardware `d3d12.dll` and integrates
  with the DXGI CoreWindow swapchain in the D3D11/DXGI bridge — the console
  has a real D3D12 driver, so the guest's device, queues and fences are real;
- **offline, on Linux** it is a full stateful software COM runtime —
  `ID3D12Device4`, command queue, graphics command list, fence, resource,
  descriptor heap, root signature and pipeline state — laid out at the exact
  MSVC Win64 vtable offsets, so a title's command recording path executes and
  its draws are recorded and named (that is what `recorded` means in the
  support matrix; it is honest about being a recording, not a render).

Root signatures are real in both: `D3D12SerializeRootSignature` and
`D3D12SerializeVersionedRootSignature` carry the actual layout (not `S_OK`
with an empty container), both deserializers hand the layout back, and
`CreateRootSignature` sees it — suite 54. `texture_quality` is a real lever
too, not a stored number: the tier caps anisotropic filtering at 1× / 4× /
16× / 16× and **never raises** a guest's request, so a lower tier spends less
bandwidth on angled surfaces and a higher tier can never exceed what the title
asked for.

Vulkan's SPIR-V → HLSL results are cached on disk (`shader_cache`): the codec
round-trips every field a caller reads — the text *and* the bindings and push
constants the pipeline builder consumes — rejects anything malformed, and keeps
the directory bounded. A cold start compiles once; a warm start reads a file.

### 6. Hear — one mixer, real devices, real codecs

Every audio API a game ships with lands on **one real XAudio2 2.8 mixer**
(48 kHz stereo master voice) fed by a per-format source-voice ring: XAudio2,
DirectSound, WASAPI, OpenAL, `winmm` and FMOD all submit into it, and the
console's real audio hardware plays it.

The `winmm` surface — which engines use as the *cheap* audio path and as the
frame clock — is no longer a set of zero answers:

- **`waveOut` is one real device**: enumerable, readable `WAVEOUTCAPS`,
  PCM-only open (everything else rejected rather than silently mangled), and a
  written `WAVEHDR` comes back marked done instead of sitting in the queue
  forever. A title that sees `waveOutGetNumDevs() == 0` disables audio
  entirely; it now sees one.
- **`waveIn` is one real capture device** with the real `WAVEHDR` contract:
  device enumeration, caps, PCM-only open, and an added buffer comes back
  completed with recorded bytes. Zero devices makes an engine disable voice
  capture outright.
- **MIDI has a virtual synthesizer device**: `midiOutGetNumDevs` reports one,
  `DevCapsW/A` describe it, open/close works, short and long messages dispatch,
  volume is readable and settable, and headers prepare and unprepare.
- **Multimedia timers are real**: `timeSetEvent` starts a thread that sleeps to
  its deadline and fires — `TIME_ONESHOT` exactly once, `TIME_PERIODIC`
  repeatedly — and `timeKillEvent` joins that thread and stops the firing
  (unknown id: `TIMERR_NOCANDO`).

`msacm32` used to answer `ACMERR_NOTPOSSIBLE` for every non-PCM request, which
made engines fall back to uncompressed audio (or to nothing). The codec engine
now **decodes** IMA and MS ADPCM and u-law/A-law and performs PCM format
conversion, driven through the real `ACMSTREAMHEADER` contract
(open → prepare → convert → close). Its output is pinned against
hand-computed vectors, not against itself (suites 51–53, 57–58).

FMOD gets its own bridge (WAV/OGG decode for real); FSB5 sample decode stays
off on console behind `fsb_decode` because it heap-corrupted on the Series S,
with envelope sanity checks (`trt_fsb5_header_sane` / `trt_fsb5_sample_sane`)
so a malformed bank fails named instead of reading out of bounds.

### 7. Connect — Steam, Media Foundation, COM

**Steam** is the one place where "your own account" is the whole rule. The
guest's `steam_api64.dll` surface (277 imports in the corpus, all real)
answers honestly, and the host carries a native Steam client:

- **QR sign-in** to *your* account (`BeginAuthSessionViaQR` →
  `PollAuthSessionStatus`), with persisted credentials and the identity pushed
  into `trt_steam_identity` so the guest's `SteamAPI_GetSteamID` agrees with
  who actually signed in;
- **WebAPI**: account profile, owned games (`GetOwnedGames`) and friends feed
  the dashboard's Steam tab and the shelf's library;
- **CM WebSocket + protobuf** with BCrypt AES-256/SHA-1, and a **CDN depot
  downloader** (VDF `appinfo` parse → depot/manifest → VZ chunks → LZMA1 →
  files into the games directory), so a title you own can be fetched on the
  console itself;
- **the lifecycle the API expects**: `SteamAPI_Init`/`Shutdown`,
  `RunCallbacks`, callback and call-result registration/unregistration, plus
  `SteamApps`/`SteamClient`/`SteamFriends`/`SteamUser`/`SteamUtils` and the
  `ISteamFriends`/`ISteamUserStats`/`ISteamMatchmaking`/`ISteamNetworking`
  accessors — every one of them driven through its real vtable in suite 55.

What it will not do: bypass DRM. There is no license forging, no decryption of
someone else's content, no Xbox security bypass. An ownership question is
answered from your own account's library, and where it cannot be answered it
says so.

**Media Foundation** (`mfplat.dll`) was claimed and named-silent; it now backs
real objects — `MFStartup`, `MFCreateMediaType`, `MFCreateMemoryBuffer`,
`MFCreateSample` — with working COM vtables, so a video path can set a type,
fill a buffer, wrap a sample and read its values back instead of getting an
empty object that answers `S_OK` and nothing else.

**COM/OLEAUT** gets the same treatment: `CoInitialize` (STA/MTA nesting),
CLSID personalities with GUID-named failures, `BSTR`, `SafeArray`, `Variant`,
and the ordinal mapping fixes above.

### 8. Survive — exceptions, crash bundles, watchdog

Full Win64 C++ EH — the part that took the longest: `.pdata` personality
search, two-pass `RtlUnwindEx`, MSVC funclet rethrow splicing, VEH
first-chance, and the host→guest boundary pad for exceptions that escape the
guest. Measured on real Unity IL2CPP games whose engines raise managed
exceptions through native frames.

On a console there is no debugger and no way to attach one, so failure has to
leave a body:

- **Crash bundles.** A fatal fault writes one text file into
  `LocalState\crash` containing every fact a post-mortem needs — the fault,
  registers, stack, the settings in force, the shader census and the log tail.
  Empty sections say so instead of vanishing, and the directory is bounded
  (`crash_report_trim`) so a crash loop cannot fill the console disk. It is
  opt-in through `crash_report`; the toggle existed long before anything
  behind it did.
- **A 1 Hz watchdog.** The host logs total app memory and the guest ledger
  trajectory every second, so the last lines before a death show the slope
  rather than a cliff, and a guest thread that ignores every polite signal is
  force-killed as a last resort after the log has named it.
- **Lifecycle.** Where the host exposes it, `CoreApplication` suspend/resume
  is hooked; where it does not, the log says so instead of pretending.

### 9. Prove — measured, not promised

Every claim in the status table traces to a test or a captured log — see
[docs/03-evidence.md](docs/03-evidence.md). `tools/build-test.sh` is the
offline gate: 78 stages that build the runtime, link real PE guests against
it and execute them, on Linux, with no console and no network. Two of them
are worth calling out because they define the shape of every other claim:

- **The mock-render gate.** With mock GPU hooks injected, a real game's
  **render loop executes end-to-end** and every vtbl call is counted:

  ```text
  RACCOIN.exe (real Unity IL2CPP Steam game) — offline boot ladder:
    CRT-STARTUP → WINMAIN → UNITYMAIN2 (IAT-probed) → WINDOW →
    D3D11 DEVICE ACQUIRED → PUMP ENTERED → PUMP QUIT
    mock-render gate: 2,730,468 device-context calls, ~268,000 DrawIndexed
    in the 10 s window, clean ExitProcess(0).                     7/7 markers
  ```

- **The corpus coverage self-test.** The corpus is bind-simulated PE by PE
  and every import classified `real` / `recorded` / `named-silent` /
  `unresolved`. The current matrix is 100% real with **zero unresolved** and
  zero named-silent across the whole corpus; `docs/gap-worklist.md` (generated
  from the same run) is empty, which is the point — a non-empty worklist is a
  measurement of what is missing, not a wish list.

## 🎮 What boots today

| Game (corpus, gitignored) | Type | Result |
|---|---|---|
| **RACCOIN** | Unity 6 IL2CPP, Steam, D3D11 | **Playable on console.** Menu, live run, coin physics, HUD progression; 7.2 s capture. |
| **God of War** (PC) | AAA in-house engine, x64, D3D11 | **Boots and renders on console.** The opening forest, across four boots; menu and gameplay next. |
| **Celeste** (v1.4.0.0) | XNA / Mono + FNA (managed .NET) | **Playable on console.** Title, menu, Chapter 1 gameplay; first managed game to run. Audio silent. |
| **Ultra Pool** | Godot 4.3 + GodotSteam (Vulkan) | **Full lifecycle** — scene load → pump → Godot's own quit chain, exit(0) |
| **Go Next (demo)** | Unity IL2CPP | boots, reaches `UNITYMAIN2` |
| **OrTMM** | PyInstaller / commercial | full CRT→WinMain→window→pump lifecycle |
| **BotW-Native (launcher + recomp exe)** | BOTW Wii U static recomp | whole MinGW/SDL3/OpenSSL/glslang binary set loads + `DllMain` clean |
| **GoWLauncher** | Win32 launcher | loads 1/0 — launcher only, not the game above |

**Engine classes covered:** Unity IL2CPP + **mono**, **XNA/Mono via FNA (managed .NET)**,
AAA in-house D3D11, Godot 4.x (Vulkan),
PyInstaller/Python, static-recomp ports, Win32 launchers, GDI+ UI apps,
FMOD-audio games, DirectStorage/Bink/Oodle-shipping titles (honest fallbacks).

**Not yet:** ARM64 and other non-x86 guests; D3D12 AAA titles verified on
hardware (the console path exists, hardware-verification pending); Wwise/Miles-audio
games; save/profile round-trips. See [GAP-MAP.md](GAP-MAP.md). PE32 (x86-32)
titles are staged and run via the host x86-32 JIT (`tools/game-stage.py`,
`src/host/main.cpp`); treat them as experimental and validate on hardware.

## 🖥️ Dashboard (TV) & phone/web dashboard

The launcher on the TV has five screens — cycle with **RB/LB** (or `Tab`):

- **Shelf** — game list (U/D pick, L/R choose the exe when a game has several),
  A launches, Y opens the Steam QR link, R3/F1 toggles gamepad-as-mouse.
  The LAN URL + QR for game transfer is always on screen, and each entry shows
  what the pre-boot scanner found (`[d3d11 OK]`, `[steam-auth]`, 32-bit…).
- **Steam** — QR sign-in, your account, your library, friends, and depot
  downloads with progress ("Downloading Steam depot…") straight into the
  shelf.
- **Settings** — editable rows: default input mode (`auto`/`controller`/`kbm`/
  `pad2kbm`), pad-as-mouse + wheel speed, pad vibration on/off, the **Windows
  version reported to games** (major.minor.build, Win11 personality toggle),
  write-watch GC on/off, Steam link on/off, QR overlay on/off, log tail size,
  frame cap, VSync, render scale, texture tier, shader cache, crash reports.
  Every change persists to `LocalState\settings.ini` and applies to guests
  immediately.
- **Setup** — pairing and the first-run checklist: storage, D3D11, gamepads,
  audio, transfer, shelf, self-test, each one live.
- **Live log** — the same ring the web dashboard shows, streamed from memory.

An in-game quick overlay (Steam social) sits on top of any of them.

The phone/PC web dashboard (open the LAN URL shown on the TV, or scan the QR)
adds a Connect card, the game manager (**upload folder/zip → extract → pick
exe**, per-game args/appid/input, launch/stop/delete), the live log, and the
**same settings editor** (`GET/POST /api/settings`) — all offline-gated in
`tests/test_webxfer.cpp`.

Autodetect the console family (`series_x`, `series_s`, `one_x`, `one_s`) with
`tools/console_type.py detect --console <ip>`; see [docs/console-type.md](docs/console-type.md).

## 🚀 Quick start (Xbox Dev Mode)

No Linux or coding tools required to install a release on your Xbox.

> **New here? Start with the [setup guide](docs/BEGINNER-INSTALL.md) — it walks through every step (Dev Mode, Device Portal, install, game transfer), including what to do with the `dev.cer` file.**

### 1. Install Tiramisu on your console


1. Enable **Xbox Dev Mode** and open **Dev Home**.
2. Go to **Settings → Dev Home** and turn **ON** `"Treat UWP apps as games by default"` *(this gives Tiramisu **5 GB RAM** instead of 1 GB)*.
3. Download **`Tiramisu.msix`** + **`dev.cer`** (normal build) **or** **`Tiramisu-EdgeSpoof.msix`** + **`edge-dev.cer`** (KBM edition, below) from the [latest GitHub Release](https://github.com/xanstomper/Tiramisu/releases).
4. Open your Xbox **Device Portal** in any Windows, Mac, or phone browser (`https://<your-xbox-ip>:11443`).
5. Under **My games & apps → Add**, upload **only** the `.msix` you chose. Do not upload the `.cer` — the current portal has no certificate box. (If the install fails with a signature error, use the command-line installer in the [setup guide](docs/BEGINNER-INSTALL.md), which sends the certificate automatically.)
   *(From a Linux dev PC you can also deploy with `DP_HOST=<console-ip> DP_PASS=<portal-pass> tools/deploy.sh install out/Tiramisu.msix out/dev.cer`.)*

#### The `.cer` file — just leave it alone

The `.cer` is the **signing certificate**, not a dependency or an app. It tells
Xbox to trust the signature on the `.msix`.

**Do nothing with it.** Do not open it. Do not double-click it. Do not install
it on your computer. Do not put it in the Dependencies list. Just leave it in
the same folder as the `.msix`. The command-line installer (`tools/deploy.sh`)
sends it automatically when needed; the portal website does not ask for it.
See the [setup guide](docs/BEGINNER-INSTALL.md) for the full steps.

Correct pairs:

| Package | Certificate |
|---|---|
| `Tiramisu.msix` | `dev.cer` |
| `Tiramisu-EdgeSpoof.msix` | `edge-dev.cer` |

If you mix them, install fails with `0x8007000B` (publisher/signature mismatch).
If the app installs but closes instantly, check that "Treat UWP apps as games by
default" is ON and pull `LocalState\tiramisu.log` through Device Portal.

### 2. Transfer games & play

- **Over Wi-Fi / LAN:** Launch **Tiramisu** on Xbox, scan the **QR code** (or open `http://<xbox-ip>:9127` on PC/phone):
  - **On PC:** Drag & drop a game folder or `.zip`, or select an uncompressed folder.
  - **On iPhone / mobile:** Mobile browsers don't allow folder selection — compress your game folder to a **`.zip`** first and tap **Upload .zip** (or use **Pick Files** / a direct `.zip` URL).
  - **External USB SSD or hard drive:** Use a drive Xbox Dev Mode exposes to apps as media storage. Put each game in its own folder under `Games` or `TiramisuGames` at the drive root. Tiramisu refreshes the shelf while idle when the drive appears or disappears. The LAN dashboard shows detected volumes and which game folders the runtime can access.
  - **Download directly on Xbox:** Paste a direct ZIP or disc image URL into the LAN dashboard and select **Download on Xbox**. Tiramisu downloads to temporary internal storage and adds the game to the library after it finishes. For an archive, leave room for both the ZIP and the extracted game. The Steam tab downloads games from the signed-in account.
- **Mouse games on Xbox (normal build):** use your **Xbox controller (`pad2kbm` right stick = mouse, triggers = click)** or the **Phone Remote Trackpad (`/controls` link on the LAN dashboard)**.
- **Keyboard & mouse (KBM) edition:** the same release ships **`Tiramisu-EdgeSpoof.msix` + `edge-dev.cer`**. It is the experimental Microsoft Edge-identity build that asks the Xbox shell for real hardware pointer input; install it *instead of* the normal package on your own Dev Mode console. It is unsupported and unverified — read [XBOX-MOUSE.md](XBOX-MOUSE.md) before installing. The Edge identity has its own package family name, so games/saves start in a fresh `LocalState`; use `tools/migrate-localstate.py --from-pfn <old> --to-pfn <new>` to move an existing library across.
- **Back up saves before updating:** Open the LAN dashboard → **Saves & Support → Backup Saves (.zip)** before uninstalling/reinstalling Tiramisu, since Dev Mode wipes `LocalState` when a package is removed.

### For developers / maintainers (CLI staging & proof)

```sh
python3 tools/game-stage.py "~/Games/<Title>" --out staging/<Title> --exe <rel.exe>
python3 tools/console.py upload --game staging/<Title> --console <console-ip>
python3 tools/console.py proof  --game staging/<Title> --console <console-ip>
```

`game-stage.py` runs PE32 (x86-32) titles via the host x86-32 JIT and blocks
the remaining unsupported archs (ARM64 today) instead of uploading gigabytes
that cannot start. `--set key=value` stages a per-game `tiramisu.profile.ini`
(e.g. `--set fsb_decode=1` to try FSB5 audio decode on one title); it is the
only settings channel a packaged app can read. Proven titles live in
[docs/title-matrix.md](docs/title-matrix.md), which is generated from
`docs/proof/titles.json` + `proof.json` — `python3 tools/title-matrix.py
--check` fails if a row cites an artifact that is not on disk.

No console on the network? The whole deploy path can be rehearsed offline:
`python3 -m unittest discover -s tests/py_tools` runs `console.py` against a
fake Device Portal over real TLS (CSRF cookie/header, basic auth, multipart
install, batched file upload, launch, screenshot, log fetch, `proof.json`).

Prereqs on the console: Device Portal enabled and **"Treat UWP apps as games
by default" ON** (5 GB limit, not 1 GB). Games you **own**; DRM-free builds
or your own Steam account auth. **No DRM bypass, ever.**

## 🔨 Build from source (Linux)

```sh
tools/build-test.sh    # full offline gate: 78 stages, real PE guests executed
tools/build-host.sh    # the UWP host cross-build (clang-cl + lld-link)
tools/release.sh       # host once, then BOTH packages:
                       #   out/release/Tiramisu.msix + dev.cer
                       #   out/release/Tiramisu-EdgeSpoof.msix + edge-dev.cer
tools/pack.sh          # one flavor only; EDGE_SPOOF=1 for the KBM package
```

Needs: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, `makemsix`, `osslsigncode`, and the
uwp-crossbuild wrapper (defaults to `~/gow-uwp/ref/uwp-crossbuild`; override
with `UWP_CROSSBUILD`). Python + Pillow for the branding assets
(`package/branding/` is the source of every packaged logo; regenerate with
`python3 tools/gen-assets.py`).

The gate is the contract: it builds `src/trt/*.cpp` into `libtrt.a`, links
every suite against it, and executes real PE images through the loader. A
failure is a red gate, never a skipped test.

## 📊 Status

> Every layer is "done" only when a test or a console run proves it.
> Failures are **visible and named** — never silent.

| Layer | State |
|---|---|
| Offline loader (real PE guest executed on Linux) | **PASS** — mapping, DIR64/HIGHLOW reloc, import binding, delay imports, W^X seal, entry execution |
| Win64 C++ EH (SEH) | **PASS** — pass-1 personality search, RtlUnwindEx pass-2, funclet rethrow splice, geometric purge, VEH, boundary swallow pad |
| Real-game corpus | **PASS** — 29 PEs load; RACCOIN playable and God of War rendering on console; UltraPool/GoNext/OrTMM boot |
| Import coverage | **PASS** — corpus bind simulation: 100% real, 0 unresolved, 0 named-silent, 0 recorded; empty gap worklist |
| ABI conformance ratchet | **PASS** — every claimed BUILTIN answers the dispatcher's argument count; baseline may only shrink (suite 50) |
| D3D11/DXGI guest | **PASS on console** — real device + CoreWindow swapchain used by both titles |
| D3D12 | **BUILT** — console forwards to real `d3d12.dll`; offline software runtime at MSVC vtable offsets; real root-signature serialization (suite 54) |
| D3D9/D3D8/DDraw → D3D11 bridges | **BUILT** — SDK-exact vtables |
| Vulkan 1.0–1.3 ICD (SPIR-V→HLSL→D3D11) | **BUILT** — Godot 4.3 boots through it; bounded on-disk SPIR-V cache |
| Audio (XAudio2 / DirectSound / WASAPI / OpenAL / winmm / **FMOD**) | **BUILT** — one real XAudio2 2.8 sink; FMOD WAV/OGG decode for real |
| winmm devices (waveOut / waveIn / MIDI synth / timers) | **BUILT** — one real playback device, one real capture device, a virtual MIDI synthesizer, real `timeSetEvent` threads (suites 51, 53, 57, 58) |
| msacm32 codecs | **BUILT** — IMA/MS ADPCM and u-law/A-law decode + PCM conversion, pinned to hand-computed vectors (suite 52) |
| Memory + budget governor | **BUILT** — 5 GB commit ledger; read-only file views **mapped, not copied** (VA-only, SSD↔RAM overlap); W^X two-phase JIT (mono enabler), write-watch |
| Guest-shipped DLLs, delay imports, api-set contracts | **BUILT** — recursive bind, DllMain run, UCRT C-library real answers |
| Ordinal imports | **FIXED** — ordinals classify as the `#<n>` export they bind; oleaut32 ordinals map to real conversions |
| Dynamic API synthesizer | **BUILT** — unknown symbols get fail-visible thunks with deduced ABI and plausible returns |
| Registry, COM/OLEAUT, shell, COMCTL, GDI, **GDI+** | **BUILT** |
| Media Foundation (`mfplat`) | **BUILT** — real media type / memory buffer / sample objects (suite 55) |
| Steam | **BUILT** — QR sign-in to your account, WebAPI library/friends, CM WebSocket + CDN depot download; **no DRM bypass** |
| ISO / disc images | **BUILT** — `.iso` → read-only `D:` drive, lazy per-file extraction |
| Settings + linkage | **BUILT** — 30 keys, per-title profile, all bridge hooks proven connected (suite 34b) |
| Crash bundles + watchdog | **BUILT** — bounded one-file post-mortems, 1 Hz memory-ledger trail, last-resort guest-thread kill |
| Input (keyboard/mouse/gamepad, DInput8, XInput, Raw Input) | **PASS (RACCOIN)** — CoreWindow events → guest getters + pump; pad-as-mouse |
| Console run | **PASS (2026-11-21)** — RACCOIN playable; God of War renders the forest; **Celeste playable (title, menu, Chapter 1)** |
| ARM64 or other non-x86 guests | **not supported** — see GAP-MAP; PE32 (x86-32) is staged via the x86-32 JIT |

<details>
<summary><b>Full layer-by-layer table (70+ rows)</b> — click to expand</summary>

| Layer | State |
|---|---|
| UWP host (CoreWindow/D3D11/XInput/LocalState log) | **BUILDS** — signed `Tiramisu.msix` produced and verified |
| Phone transfer (QR on TV → upload zip → launch) | **BUILT** — ws2_32 HTTP server, miniz zip extraction, launch handoff; console verdict pending |
| DirectInput 8 | **BUILT** — SDK-exact vtables, mouse deltas + 256-key map from the real input ring |
| Real file enumeration (FindFirstFile family) | **BUILT** — dual-platform REAL (Linux readdir / console native passthrough), SDK-exact 592-byte `WIN32_FIND_DATAW` |
| Temp-path boot chain (PyInstaller) | **BUILT** — `GetTempPathW/2W`, VFS-relative resolution |
| /GS cookie killer | **FIXED** — `GetFileAttributesExW` layout bug found by hardware watchpoint; regression test in `test_engine.cpp` STAGE 9 |
| shlwapi / shell32 / shfolder | **BUILT** — Path* helpers, SHGetFolderPath W/A, KnownFolders, real IMalloc |
| COM boot + OLEAUT | **BUILT** — CoInitialize (STA/MTA nesting), CLSID personalities with GUID-named failures, BSTR, SafeArrays, Variants |
| COMCTL32 + GDI32 + GDIPLUS | **BUILT** — InitCommonControlsEx + ordinal `#380`, subclass family, GDI object table, GDI+ flat API (launcher UI, recorded draws) |
| UCRT C-library surface | **BUILT** — 115 ms_abi host-libc wrappers (heap/str/wcs/math/qsort/_initterm) — MinGW guests survive DllMain |
| Boot determinism — UNCERTAIN-7 all BUILTIN | **BUILT** — wininet/psapi/iphlpapi/imm32/crypt32/dbghelp honest-runtime bridges; MiniDumpWriteDump writes a REAL text crash report through the VFS |
| D3DX9 / D3DX10/11 | **BUILT** — exact math, texture load (BMP/TGA/PNG/DDS), ID3DXSprite quad batching |
| Image parsers + stb_image | **BUILT** — hand PNG/DDS decoders + vendored stb_image fallback |
| GLSL/SPIR-V → HLSL | **VENDORED** — mojoshader, d3d8to9, gl4es, stb_vorbis (licenses kept) |
| msacm32/msvfw32/winspool | **BUILT** — ACM decodes ADPCM/u-law/A-law; no-printer answers stay honest |
| Pre-boot compat scanner | **BUILT** — shelf reads each PE and flags steam-auth/d3d9/d3dx9/dsound/ddraw/d3d11 before launch |
| Engine synthesizer | **BUILT** — evidence-based engine classification from import + delay-import tables, profile synthesis |
| Launcher setup UI | **BUILT** — first-run live checklist; A opens the shelf |
| Steam tab + library | **BUILT** — QR, account, owned games, friends, depot download progress |
| Async file I/O + mmap | **BUILT** — overlapped reads → GQCS packets, `TRT_IO_MMAP_MIN` mmap fast path, suite 42 |
| Budget governor | **BUILT** — one 5 GB ledger; over-budget refusals named; suite 41 |
| File-backed views (SSD↔RAM overlap) | **BUILT** — read-only `MapViewOfFile` maps the page cache instead of copying; VA-only charge, async readahead, write/COW + past-EOF + `TRT_MAP_MODE=copy` keep the copy path; suite 41b |
| Texture quality policy | **BUILT** — anisotropy tiers that only lower a guest's own request |
| SPIR-V shader cache | **BUILT** — bounded on-disk cache round-tripping bindings + push constants |
| Crash bundles | **BUILT** — fault, registers, stack, settings, shader census, log tail; bounded directory |
| Watchdog | **BUILT** — 1 Hz memory/ledger trail, last-resort kill |
| ABI gap baseline | **BUILT** — `tests/abi-gaps.baseline`, may only shrink |

</details>

## 🗺️ Roadmap

- **M0 console PoC — done (2026-11-19).** Signed MSIX deploys; RACCOIN
  reaches its menu, starts a run and plays; God of War boots and renders the
  forest; Celeste boots and renders its title screen. Evidence:
  [Console proof](#console-proof-2026-11-19).
- **M1b — Celeste gameplay — done (2026-11-21).** Menu navigation, level
  entry and Chapter 1 gameplay verified on console. Console audio still to
  restore (FSB/Vorbis decode currently disabled due to a heap-corruption
  fault in the Series S AppContainer).
- **Offline surface — current.** Corpus imports now bind 100% real with an
  empty gap worklist, the ABI ratchet is green, and the previously
  named-silent families (winmm devices, ACM codecs, Media Foundation, Steam
  lifecycle, oleaut32 ordinals, D3D12 root signatures) are driven through
  their real contracts in suites 50–58.
- **M1 front end + sustained play — in progress.** God of War's normal
  front-end/menu path; RACCOIN soak test and frame-time capture; audio and
  save validation.
- **M6 Steam library — console-verified 2026-11-24.** QR sign-in, WebAPI
  library, CM/CDN depot download all worked on Xbox Series S: HoloCure
  (2420510) downloaded through PICS appinfo + per-CDN auth token and appeared
  on the shelf ready to play.
- **Any-game breadth** — x86-32 PE32 loader + translator ladder; streaming
  maps; IOCP/dstorage; D3D12 hardware verification (see [GAP-MAP.md](GAP-MAP.md))
- **Audio breadth** — MP3/ADPCM/FSB decoders for FMOD; Wwise; Miles

## 🧱 Rules of the road

- Games you **own**; DRM-free builds or your own Steam account auth.
- **No DRM bypass, ever.** No Xbox security bypass. No game files or
  credentials in the repo.
- MIT for our code; vendored third-party keeps its licenses
  (see `THIRD-PARTY.md` and vendored dirs).

## 💬 Community & Reporting Issues

- **Setup help, compatibility reports, clips, dev bridges:** use [GitHub Discussions](https://github.com/xanstomper/Tiramisu/discussions).
- **Game doesn't boot or crashes?** On the LAN dashboard open **Saves & Support → Download Debug Bundle (.zip)** and attach it to a [Game Compatibility Report](https://github.com/xanstomper/Tiramisu/issues/new?template=game-compatibility.yml) or [Bug Report](https://github.com/xanstomper/Tiramisu/issues/new?template=bug-report.yml).
- **Want to contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md) and [GAP-MAP.md](GAP-MAP.md) for active bridge & runtime work items.

## 📚 Documentation

| Doc | Contents |
|---|---|
| [docs/03-evidence.md](docs/03-evidence.md) | what is proven vs assumed — every fix traces to a measurement |
| [docs/04-support-roadmap.md](docs/04-support-roadmap.md) | the walls between "a few titles boot" and "any PC game runs", ranked by library share |
| [docs/support-matrix.md](docs/support-matrix.md) | auto-generated, measured per-title `real / recorded / named-silent` import coverage |
| [docs/gap-worklist.md](docs/gap-worklist.md) | auto-generated gap list (currently empty) — the next thing to implement if it is not |
| [docs/console-type.md](docs/console-type.md) | `tools/console_type.py` console-generation autodetect (Series X/S, Xbox One X/S) |
| [docs/xbox-one-family.md](docs/xbox-one-family.md) | Xbox One / One S / One X support map — hardware, manifest capabilities, test matrix |
| [docs/promo-reel.md](docs/promo-reel.md) | 60-second setup/promo shot list and capture recipe |
| [docs/known-issues.md](docs/known-issues.md) · [docs/troubleshooting.md](docs/troubleshooting.md) | measured limitations, and how to get unstuck |
| [docs/proof/](docs/proof/) | console captures: RACCOIN menu, run and clip; four God of War boots; Celeste title, menu, greyscale test and gameplay |
| [GAP-MAP.md](GAP-MAP.md) | live gap table, W-lanes, dependency order, triggers |
| [CHANGELOG.md](CHANGELOG.md) | version + change history |

---

<div align="center">

**🍰 Tiramisu** — because the console already has the hardware; it just needed
the cake.

*Built for Xbox Dev Mode. Homebrew the honest way.*

⭐ Star this repo if you like native-speed PC games on Series X|S.

</div>

---

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=xanstomper%2Ftiramisu&type=Date)](https://star-history.com/#xanstomper/Tiramisu&Date)
