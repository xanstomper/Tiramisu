# Memory instrumentation patches (06–08) — Celeste OOM investigation

Context: Celeste on Tiramisu (.141) runs to deep asset load, then the
process disappears after ~6s with NO error. The OOM analysis
(`../oom-analysis.md`) found OOM is UNPROVEN (only ~300–500 MB at 6s,
worst case ~2.7 GB vs the 5 GB cap) and the log has ZERO memory numbers.
The decisive next step is INSTRUMENTATION, not a fix: log memory so the
next test shows whether memory climbs to 5 GB (OOM) or stays flat
(silent crash).

## The patches

| Patch | What | Where |
|---|---|---|
| `06-mono-watchdog-mem.patch` | Per-tick memory logging in the Mono watchdog (every 3s): AppMemoryUsage + AppMemoryUsageLimit + PCM cache bytes + SGen heap. Also adds the PCM cache accessor (declaration + definition) that patch 07 uses. | `src/trt/mono_host.cpp`, `src/trt/bridge_fmod_studio.h`, `src/trt/bridge_fmod_studio.cpp` |
| `07-bank-load-mem.patch` | Per-bank-load memory logging: AppMemoryUsage + PCM cache bytes after each FMOD bank load. REQUIRES patch 06 (the accessor). | `src/trt/bridge_fmod_studio.cpp` |
| `08-atlas-setdata-mem.patch` | Per-atlas-page memory logging: AppMemoryUsage + SGen heap after each large texture SetData (>= 1 MiB). Adds the host AppMemoryUsage helper (the C driver cannot call WinRT). | `third_party/fna-src/lib/FNA3D/src/FNA3D_Driver_D3D11.c`, `src/trt/fna3d_host.h`, `src/trt/fna3d_host.cpp` |

## Apply

From the repo root (`~/tiramisu-brown-merge`), in order:

```
git apply 06-mono-watchdog-mem.patch
git apply 07-bank-load-mem.patch
git apply 08-atlas-setdata-mem.patch
```

06 and 08 are independent (either can land alone). 07 REQUIRES 06 (it
calls `trt_fmod_studio_pcm_cache_bytes`, defined in 06) — applying 07
without 06 will link-fail (undefined reference). 07's hunks don't
overlap 06's, so it applies cleanly on top of 06 (verified).

## How to verify each patch landed

- 06: `grep -c "TRACE mem: app" src/trt/mono_host.cpp` → 2 (the format
  string + the failure line); `grep -c trt_fmod_studio_pcm_cache_bytes
  src/trt/bridge_fmod_studio.h src/trt/bridge_fmod_studio.cpp` → 2
  (declaration + definition).
- 07: `grep -c "mem after bank" src/trt/bridge_fmod_studio.cpp` → 3
  (UWP success, UWP failure, Linux).
- 08: `grep -c "mem after SetData"
  third_party/fna-src/lib/FNA3D/src/FNA3D_Driver_D3D11.c` → 1;
  `grep -c trt_fna3d_app_memory_usage src/trt/fna3d_host.h
  src/trt/fna3d_host.cpp` → 3 (declaration + UWP definition + Linux stub).

## What the log lines look like (tiramisu.log)

Patch 06 (every 3s, via the allocation-free trace writer — the same
channel as the watchdog ticks):

```
TRACE mem: app 412/5120 MiB, pcm 0 MiB, sgen heap 180 used 120 MiB
TRACE mem: app usage query failed        (only if the WinRT query throws)
```

Patch 07 (after each of the 7 bank loads):

```
fmod-studio: mem after bank music.bank: app 380/5120 MiB, pcm cache 0 MiB
```

Patch 08 (after each texture upload >= 1 MiB; routed through
FNA3D_LogInfo → the `FNA3D info: ` prefix):

```
FNA3D info: mem after SetData 4096x4096 (67108864 B): app 900/5120 MiB, sgen heap 1200 used 800 MiB
```

## How to read the result (the decision the data makes)

- **App usage climbs toward 5120 MiB before the death → OOM.** Follow
  oom-analysis.md §6: set a SGen max heap (`MONO_GC_PARAMS
  max-heap-size` 2–3 GB), force a major GC after each atlas page,
  reduce the PCM cap only if the cache is the driver.
- **App usage stays flat (~300–500 MB) → silent crash.** Follow
  oom-analysis.md §6: SEH guards / VEH on ALL threads, heap-integrity
  checks around the atlas load, capture mono-stderr.log + any WER dump.
- **SGen heap grows while app usage stays flat → the managed heap is
  the driver** (dead RGBA buffers in the large-object space awaiting a
  major GC).
- **PCM cache grows toward 512 MiB → the audio cache is the driver**
  (unexpected — the analysis says FMOD is ~10 MB at 6s).

## Notes / caveats

- Patches generated programmatically (exact-anchor edits on /tmp copies,
  then `diff -u` with git-style labels). `git apply --check` passed for
  all three; a real `git apply` produced byte-identical output
  (verified 2026-11-21 ~03:00 EST against the tree as of then).
- Opencode is actively iterating in this tree — if it edits
  mono_host.cpp, bridge_fmod_studio.cpp/.h, fna3d_host.cpp/.h, or
  FNA3D_Driver_D3D11.c in these regions before the patches are applied,
  re-run `git apply --check` first; a context mismatch needs a rebase
  of the patch.
- The watchdog memory line (06) uses trt_trace (allocation-free), like
  the rest of the watchdog — it will NOT hang on the heap lock. The
  bank-load (07) and SetData (08) lines use the normal log paths
  (logf / FNA3D_LogInfo), which is fine — they fire during normal
  operation, not during a freeze.
- The 1 MiB threshold in patch 08 keeps small textures quiet: atlas
  pages are >= 1 MiB, loose PNGs are < 64 KiB. The Gameplay0/Gui0 pages
  (64 MB each) will each produce one line.
- `mono_gc_get_heap_size` / `mono_gc_get_used_size` are declared
  directly (mono/metadata/mono-gc.h:118-119) — verified exported from
  libmonosgen-uwp.lib (T symbols in mono-symbols.txt). They are safe to
  call from any thread (public embedding API, no runtime attach
  required).
- Patch 07's Linux branch logs PCM bytes only (no WinRT on Linux).
  Patch 08's Linux stub returns 0 (the D3D11 driver is not built for
  Linux).
