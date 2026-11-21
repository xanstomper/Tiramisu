# Celeste .141 ceiling: OOM or silent crash? — analysis

**Question:** after ~6s the process disappears with NO error (log frozen,
:9127 dark, TV back to Dev Home). Opencode's hypothesis: OOM (big banks /
PCM decode under the 5 GB budget) or an external kill.

**Bottom line: OOM is UNPROVEN — the memory math does not close from the
known loads, and the .141 log contains ZERO memory numbers.** The signature
matches a UWP platform kill perfectly, but the accounting at 6s is only
~300–500 MB, and even the worst case (all atlases + all RGBA dead in the
managed heap + PCM cache full) is ~2.7 GB. External kill is REFUTED (every
host kill path logs and kills a thread, not the process). A silent crash
(fast-fail / heap corruption / Mono abort) is possible but also unproven —
mono-stderr.log was not captured. **The decisive next step is
instrumentation, not a fix:** log AppMemoryUsage + PCM cache bytes + SGen
heap on every watchdog tick and at each bank/atlas load. That one
measurement (does memory climb to 5 GB or stay flat?) settles it.

---

## 1. What the .141 log actually shows (t141.log, 7,256 lines)

The .141 run is the last section (~line 7000–7256). Its final sequence:

1. Mono images load (Steamworks.NET, System.Core, System.Xml).
2. sysnative bridge active; Steam Init -> 1; language "english".
3. FNA native calls (Ping, GetWindowHandle, GetDisplaySize).
4. FNA3D_CreateDevice SUCCEEDS (dev=0000028D36ECA2B0).
5. 10x `img: PNG 256x16 parsed` (the ColorGrading LUTs — exactly 10 exist).
6. FMOD System_Create + Init (maxch=1024).
7. **All 7 banks load** with real parsing (music: 94 events, 6,065
   instruments, 647 wavs; sfx: 587 events, 3,965 instruments, 3,464 wavs).
8. 9x `img: PNG ~250x64 parsed` (small UI/font textures, ~64 KB RGBA each).
9. **Watchdog tick 3s** + `TRACE PROBE malloc ok`.
10. GameInput keyboard + mouse lines.
11. One more PNG (268x64).
12. **Watchdog tick 6s** + `TRACE PROBE malloc ok`.
13. **LOG ENDS.** The 9s tick never appears.

Key facts from the log:

- **No "guest memory" line anywhere in the log.** `log_guest_memory`
  (main.cpp:1030) reads the REAL `MemoryManager::AppMemoryUsage()` /
  `AppMemoryUsageLimit()` — but it is called only once, at main.cpp:1486
  ("after image load"), in the NATIVE PE guest path, not the Mono path.
  It never fired for Celeste. There is no per-bank memory log, no PCM
  cache bytes log, no periodic memory log. **The .141 run has ZERO
  memory numbers.**
- **No BUDGET REFUSAL, no malloc NULL, no out-of-memory, no FORCE-KILL,
  no stop requested, no ExitProcess, no SEH, no UNHANDLED, no Assertion
  in the .141 run.** (The SEH lines in the grep are all from earlier
  runs, lines 449–823, the old mono_jit_init crashes.)
- **The watchdog's malloc probe SUCCEEDED at 3s AND 6s.** The CRT heap
  could still allocate at 6s — not locked, not exhausted. The process
  was alive and healthy at 6s.
- **The death is between 6s and 9s.** The next watchdog tick (9s) never
  appears. The game was in managed code (silent in the host log) when it
  died — most likely entering the atlas-load phase (see §3).

## 2. FMOD banks: how much memory? (a)

### Bank files are NOT loaded into memory

`bank_load` (bridge_fmod_studio.cpp:1055) opens the file with `fopen`,
keeps the FILE* in `bank_data.f`, and walks the chunk structure parsing
ONLY metadata into unordered_maps (events, timelines, instruments, wavs,
FSB headers with file offsets). The sample data stays ON DISK;
`decode_fsb_sample` reads each compressed sample on demand via
`file_read_at` (seek + read).

The 7 banks on disk total 665,719,576 B (~635 MB):

| bank | on disk |
|---|---|
| music.bank | 393,143,808 B (375 MB) |
| sfx.bank | 164,503,840 B (157 MB) |
| dlc_music.bank | 67,452,832 B (64 MB) |
| dlc_sfx.bank | 34,768,032 B (33 MB) |
| ui.bank | 5,400,800 B (5 MB) |
| Master Bank.bank | 405,344 B |
| Master Bank.strings.bank | 46,920 B |

**None of this 635 MB is resident.** The parsed metadata is ~11,245
instruments + ~5,000 wavs + ~1,000 events/timelines across all banks —
estimated **5–15 MB total**. Negligible.

### `loadSampleData` does not decode

`trt_fmod_studio_bank_load_sample_data` (:2394) is a NO-OP: it sets
`sample_loading_state = LOADED` and returns. Comment: "Sample data is
demand-decoded from disk (decoding a whole bank eagerly would take
minutes + GBs for music.bank)." The event-level
`trt_fmod_studio_event_load_sample_data` (:2853) DOES pre-decode an
event's samples, but Celeste calls the bank-level one at startup.

### PCM cache: capped at 512 MB

`get_decoded` (:1274) decodes ONE sample on demand and caches it in
`g_pcm_cache` with `k_pcm_cache_cap = 512 MB` (LRU eviction). The cache
can transiently exceed the cap by ONE sample (eviction happens after
the decode). A decoded music track: 3-min stereo 48 kHz = ~34 MB;
5-min = ~57 MB. **PCM cache max ≈ 512 + ~60 = ~570 MB.**

Decode transient per sample: compressed (data_len, ≤256 MB guard —
the .141 fix) + Ogg rebuild (~data_len) + PCM (~10–15x data_len for
Vorbis). A typical 5–10 MB compressed track transiently uses ~70–170 MB.
The compressed + Ogg vectors are locals, freed on return.

### FMOD total

- At 6s (no music event started yet — no `get_decoded` lines in the
  log): **~10 MB** (metadata only).
- Worst case (cache full): **~580 MB** (10 MB metadata + 570 MB PCM).
- **The FMOD bridge cannot OOM the process on its own.** The 635 MB of
  bank files is the red herring — it is never resident.

## 3. Textures / atlas: how much memory? (b)

Measured from the actual files (read every .data header: width, height
→ width × height × 4 bytes RGBA):

**Total: 1,065 pages, 449 MB on disk, 1,010 MB decoded RGBA.**

| atlas | pages | on disk | decoded RGBA |
|---|---|---|---|
| (root: Gameplay, Gui, Journal, Memo, FileSelect, WaveDashing) | 6 | 43 MB | 216 MB |
| Portraits | 736 | 78 MB | 186 MB |
| Mountain | 36 | 98 MB | 155 MB |
| TheFall | 35 | 30 MB | 108 MB |
| Farewell | 26 | 80 MB | 94 MB |
| SummitEnd | 31 | 11 MB | 71 MB |
| ForsakenCity | 14 | 18 MB | 33 MB |
| Core | 9 | 8 MB | 24 MB |
| CelestialResort | 10 | 16 MB | 23 MB |
| Cliffside | 9 | 13 MB | 19 MB |
| Overworld | 14 | 10 MB | 17 MB |
| OldSite | 11 | 12 MB | 16 MB |
| SummitIntro | 19 | 9 MB | 16 MB |
| MirrorTemple | 17 | 14 MB | 16 MB |
| Checkpoints | 80 | 6 MB | 11 MB |
| Misc | 10 | 2 MB | 4 MB |
| Opening | 2 | 0.2 MB | 2 MB |

- Gameplay0.data and Gui0.data are each 4096×4096 = 64 MB RGBA.
- The .data files are NOT PNG (header is width/height + compressed
  pixel data — the Monocle custom atlas format). They load via the
  MANAGED path (Monocle decompresses → `Texture2D.SetData`), which is
  invisible in the host log. The `img: PNG` lines in the log are only
  the loose PNGs (ColorGrading LUTs + small UI textures).
- **GPU texture total if ALL atlases load: ~1 GB.** Title screen
  (Gameplay + Gui + Overworld + Mountain + Portraits) ≈ 574 MB GPU.
- **At 6s, the game has loaded almost NO textures** — only the 10 LUTs
  (16 KB RGBA each) + 9 small PNGs (~64 KB each), total < 1 MB. The
  atlas loads come AFTER the 6s tick. **The game died BEFORE the big
  atlas loads.** The atlas RGBA / GPU math is therefore IRRELEVANT to
  the 6s death.
- Managed transient: each page decompression allocates a managed RGBA
  byte[] (up to 64 MB) in the SGen large-object space. Freed after
  SetData but collected ONLY by a major GC (see §4).

## 4. Total vs the 5 GB cap (c)

**At 6s (what the log proves loaded):**

| component | memory |
|---|---|
| Mono runtime + assemblies (mscorlib, System, FNA, Celeste, Steamworks.NET) | ~100 MB |
| FNA3D device + swapchain + backbuffer | ~50 MB |
| FMOD bank metadata | ~10 MB |
| Tiny PNG textures | < 1 MB |
| SGen heap (game objects, content pipeline state) | ~100–300 MB (est.) |
| **Total at 6s** | **~300–500 MB** |

**Worst case (everything loaded, nothing collected):**

| component | memory |
|---|---|
| Runtime + assemblies | ~100 MB |
| GPU textures (ALL atlases) | ~1,010 MB |
| Dead RGBA buffers in SGen LOS (all pages, no major GC) | ~1,010 MB |
| PCM cache (full) | ~570 MB |
| Bank metadata | ~10 MB |
| **Worst-case total** | **~2.7 GB** |

**The OOM math does not reach 5 GB from the known loads.** For OOM to
be the cause, there must be an additional ~2.3 GB+ from a source not
accounted for above — a leak, a runaway, or a pathological SGen
behavior. The UWP limit is confirmed 5 GB (the RACCOIN run logged
"guest memory 1541/5120 MiB" via log_guest_memory).

### The SGen heap: the only unbounded path

`MONO_GC_PARAMS` is set to ONLY `nursery-size=64m` (mono_host.cpp:460).
There is NO max-heap-size, NO soft-heap-limit. SGen's major heap grows
unbounded as the game allocates. The specific risk: the atlas RGBA
byte[]s (up to 64 MB each) go directly to the large-object space (too
big for the 64 MB nursery); minor GCs do NOT collect the major heap /
LOS; only a major GC frees them. If the game decompresses many pages
before a major GC runs, the LOS holds dead RGBA buffers — bounded by
the total RGBA (1 GB) if ALL pages load without a major GC. This is
the most likely OOM mechanism IF it is OOM — but it is a hypothesis,
not a finding, and even it caps at ~2.7 GB total.

### Other paths checked (all bounded, all clean)

- **Host mixer FIFO:** the feeder submits 0.5s chunks and only when
  <1s is buffered ahead; the mixer caps each voice's FIFO at 2s
  (~384 KB/voice at 48 kHz stereo). `g_voices` is "uncapped" but that
  is the voice LIST (one small entry per voice), not PCM. With a
  handful of title-screen voices, the mixer holds < 5 MB. NOT a runaway.
- **FNA3D texture path:** `D3D11_CreateTexture2D` creates the GPU
  texture (no CPU copy, no staging buffer) and returns NULL on failure
  (ERROR_CHECK_RETURN — no crash). `D3D11_SetTextureData2D` calls
  `UpdateSubresource` with the managed RGBA pointer (the driver copies
  to the GPU; no host-side copy). A 4096×4096 upload is routine. No
  NULL deref, no overrun, no unbounded allocation.
- **PNG decode (`trt_img_parse`):** stbi decodes to RGBA, freed after
  upload. The PNGs in the log are tiny (< 64 KB RGBA each).

## 5. OOM kill vs silent crash vs external kill (d)

### External kill: REFUTED

Every host kill path logs, and every one kills the GUEST THREAD, not
the process:

- `request_guest_stop()` (main.cpp:1066) logs "host: stop requested ->
  posting WM_CLOSE/WM_QUIT". No such line in the .141 log. Triggers:
  /api/stop (not called during the test), Alt+F4 (not pressed),
  Back+Start 45-tick hold (line 2581, not held).
- `force_kill_guest()` (main.cpp:1107) logs "host: FORCE-KILL guest
  thread". No such line. It fires ONLY if a stop request is ignored
  for >10s (no stop → no fire). And it TerminateThreads the guest
  thread only — the app (UI + web server) would STAY ALIVE. But :9127
  went dark and the TV went to Dev Home: the PROCESS died, not just
  the guest thread. The force-kill watchdog CANNOT explain it.
- The Mono watchdog (mono_host.cpp:645) is MONITOR-ONLY: it logs a
  tick + a malloc probe every 3s for 40 iterations. It has NO kill path.
- `check_guest_stop_trap()` only fires if a stop was requested.
- A guest ExitProcess would log "guest ExitProcess(%d)" (line 1234).
  No such line.

### Silent crash: POSSIBLE, unproven

The signature fits a crash the host cannot catch:

- The `__try/__except` around `mono_jit_exec` (mono_host.cpp:682)
  would catch a guest-thread AV and log "managed Main raised SEH".
  No such line — so the crash was NOT a catchable AV on the guest
  thread.
- A **fast-fail** (CRT heap integrity check, `__fastfail`) terminates
  the process immediately with NO exception handling and NO log.
- A **stack overflow on a non-guest thread** (feeder, mixer, web
  server — no SEH guards) also kills silently.
- A **Mono abort** (SIGABRT on a GC assertion or a GC out-of-memory)
  prints to mono-stderr.log — **NOT captured for the .141 run** (no
  mono-stderr.log from .141 exists on mineplex). This is a gap: if
  Mono aborted, the evidence is in stderr and was not collected.
- The .141 fix removed ONE heap corruption (get_decoded bogus
  data_len). Another could remain — but the CRT heap operations in
  the 6–9s window are few (atlas decompression is managed/SGen,
  texture upload is D3D11), and the watchdog's malloc probe
  SUCCEEDED at 6s (heap intact at 6s).
- procs141.json (the console process list, captured 02:38, AFTER the
  death) does NOT contain the Tiramisu process — confirming the
  process is gone, but giving no memory data (post-mortem capture;
  the largest remaining process is WWAHost.exe at 20 MB private).

### UWP OOM kill: POSSIBLE, but the math does not close

The signature fits perfectly. bridge_membudget.cpp's header states
it explicitly: "An Xbox Dev Mode 'Game' package gets ~5 GB of
committed memory; **anything beyond it is terminated by the platform,
not refused**." A platform kill is silent from the app's perspective:
no log, no exception, no dump, the process just disappears. That is
EXACTLY the .141 symptom.

But the accounting (§4) puts the process at ~300–500 MB at 6s and
~2.7 GB worst case. For the platform to kill it, ~2.3 GB+ of
allocation must come from a source not in the accounting. The only
candidate is SGen heap growth (no max size, dead large objects in
the LOS) — plausible, unproven.

### STRUCTURAL FINDING: the memory budget governor is BLIND to the Mono path

The governor (bridge_membudget.cpp) charges ONLY the guest PE's
bridged allocations: VirtualAlloc commit (bridge_kernel32.cpp),
HeapAlloc (bridge_kernel32.cpp slab pool), MapViewOfFile
(bridge_mem.cpp), PE SizeOfImage (pe_loader.cpp), CRT malloc family
(bridge_msvcrt.cpp). Mono's SGen does its OWN VirtualAlloc via
mono_valloc (direct Win32, not through the bridge), and the host's
own allocations (std::vector in the FMOD bridge, the PCM cache,
stbi buffers) use the host CRT malloc directly — NONE go through
the governor. **The governor's ledger would show a SMALL number
while the process's actual commit exceeds 5 GB, and it would NEVER
log a refusal.** This explains the absence of a BUDGET REFUSAL line
even if OOM is the cause — and it means the governor cannot warn
about a Mono-path OOM. (The governor was built for the native PE
guest path; the Mono path bypasses it entirely.)

## 6. What to do next (e)

**The immediate need is DATA, not a fix — the cause is unproven.**
The next build (.142, opencode is building it now) should add:

1. **Per-tick memory logging in the Mono watchdog** (the watchdog
   already ticks every 3s — add `MemoryManager::AppMemoryUsage()` +
   `AppMemoryUsageLimit()` + PCM cache bytes + SGen heap size to
   each tick). This is the DECISIVE measurement: if AppMemoryUsage
   climbs toward 5120 MiB before the death, it is OOM; if it stays
   flat (~500 MB), it is a silent crash.
2. **Log at each bank load** (AppMemoryUsage + PCM cache bytes after
   each LoadBankFile) — the lane note's "next" step.
3. **Log at each atlas page load** (AppMemoryUsage + SGen heap after
   each large texture SetData) — the atlas phase is where the death
   happens.
4. **Capture mono-stderr.log after the test** (if Mono aborted, the
   evidence is there — it was not captured for .141).
5. **Add the Mono path to the memory budget governor** (charge
   mono_valloc + the host's large allocations) so the governor can
   warn before the platform kills the app. Longer-term; the logging
   is the priority.

**If the data shows OOM** (memory climbing to 5 GB):

- (a) Set a SGen max heap size (`MONO_GC_PARAMS max-heap-size`,
  e.g. 2–3 GB) so the GC collects aggressively instead of growing
  unboundedly.
- (b) Force a major GC after each atlas page load (or after
  LoadContent) to free the dead RGBA buffers in the LOS.
- (c) Reduce the PCM cache cap (512 MB → 256 MB) if the cache is
  the driver.
- (d) Unload unused banks (the DLC banks are not needed for the
  title screen) — saves ~5 MB of metadata only; NOT a fix for GBs.
- (e) Do NOT "stream the banks" as a fix — the bridge already
  demand-decodes (the bank files are never resident). Streaming
  is already the design.

**If the data shows flat memory (silent crash):**

- Add SEH guards / a VEH on ALL threads (not just the guest
  thread), heap-integrity checks around the atlas load, and capture
  mono-stderr.log + any WER dump. The crash is then diagnosable
  from the fault address + the thread.

**Do NOT do:** do not lower the PCM cache cap or unload banks as a
blind OOM fix — the accounting shows neither is the driver (FMOD
is ~10 MB at 6s, ~580 MB worst case). Do not assume OOM without
the AppMemoryUsage measurement.

---

## Appendix: sources read

- `~/tiramisu-brown-merge/src/trt/bridge_fmod_studio.cpp` (bank_load,
  decode_fsb_sample, get_decoded / PCM cache, load_sample_data,
  feeder, mixer submission)
- `~/tiramisu-brown-merge/src/trt/bridge_fmod.cpp` (WAV/raw decode)
- `~/tiramisu-brown-merge/src/trt/bridge_audio.cpp` (mixer FIFO, voices)
- `~/tiramisu-brown-merge/src/trt/bridge_membudget.cpp` (governor:
  budget, ledgers, refusal shape)
- `~/tiramisu-brown-merge/src/trt/mono_host.cpp` (GC params,
  watchdog, SEH guard around mono_jit_exec)
- `~/tiramisu-brown-merge/src/host/main.cpp` (log_guest_memory,
  request_guest_stop, check_guest_stop_trap, force_kill_guest,
  main-loop stop logic)
- `~/tiramisu-brown-merge/src/trt/bridge_img.cpp` (trt_img_parse)
- `~/tiramisu-brown-merge/third_party/fna-src/lib/FNA3D/src/FNA3D_Driver_D3D11.c`
  (CreateTexture2D, SetTextureData2D)
- `/tmp/opencode/t141.log` (7,256 lines — the .141 run; t141b.log is
  byte-identical) + `/tmp/opencode/procs141.json` (post-mortem
  process list, no Tiramisu process present)
- `~/celeste-xbox/Celeste/Content/FMOD/Desktop/*.bank` (7 banks,
  635 MB on disk) + `~/celeste-xbox/Celeste/Content/Graphics/Atlases/`
  (1,065 .data pages measured: 449 MB disk, 1,010 MB RGBA)
