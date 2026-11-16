# GAP-MAP — what is needed to boot real PC games (paste with TIRAMISU-MASTER-PROMPT.md)

> Companion work order for AGY/Gemini sessions. Read `TIRAMISU-MASTER-PROMPT.md` (the
> contract) first; this file is the measured map of everything left.
> Produced 2026-11-14 ~14:15 EST from tool runs on THIS tree + the corpus +
> donor research (links inline). Nothing here is from memory.
>
> **Before trusting any number: re-run the two audit commands in §1. The tree
> moves hourly — two sibling sessions are actively editing it.**

---

## 0. How to work (unchanged from the contract)

- **Measure first** — reproduce with a corpus PE + the depth ladder/audit before
  writing any bridge.
- **Solo gates** — `tools/build-test.sh` etc. run ONE at a time; check for live
  writers first (`find ~/tiramisu -type f -mmin -15 -not -path './.git/*'
  -not -path './tests/out/*'`). Two sessions are live in this repo right now.
- **Fail named, never silent.** STUB hits are work items, not success.
- Do not commit unless the user asks; iteration reports are the deliverable.

---

## 1. Ground truth (measured 2026-11-14)

**Gates on the current tree (2026-11-15, ui+settings wave):**
`build-test.sh` → gate_rc=0, 38× `RESULT: ALL PASS` (incl. the new
funclet-rethrow + boundary-escape fixtures + test_settings) · corpus-run EXE
stage: RACCOIN 1 booted/0 failed (7/7 ladder), Go Next demo.exe 1/0 with
UNITYMAIN2 REACHED, OrTMM 1/0, GoWLauncher 1/0.

**RACCOIN.exe** (real Unity IL2CPP Steam game, now in `corpus-local/raccoin/`,
gitignored) — **BOOTS (2026-11-15, ehfix16)**:

```
CRT-STARTUP OK / WINMAIN ENTERED / WINDOW CREATED (hwnd=0x10101, title 'RACCOIN')
UnityPlayer.dll DllMain -> returned 0x1
GameAssembly.dll (96MB IL2CPP) DllMain -> returned 0x1
D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT
kernel32: RaiseException(0xe06d7363) x2 -> swallowed at host->guest call boundary
                                          (UnityPlayer wrapper-catch emulation)
CORPUS-EXE: 1 booted, 0 failed, 0 skipped (x86)
```

The game then exits through its own clean shutdown path
(`IDXGISwapChain::SetFullscreenState(0) -> S_OK` → `TerminateProcess(0)`).
23→all C++ exceptions handled; the full causal chain (pass-1/pass-2 splice
clobber, stale-record windows, boundary escape semantics) is measured in
`/tmp/opencode/raccoin-ehfix9..19.log` and locked by two new fixtures in
`tests/test_guest_eh.cpp` (funclet rethrow caught; funclet rethrow escaping to
the boundary → synthetic 0 return). The `UNITYMAIN2 CALLED` marker is fixed
(ehfix19): the guest calls UnityMain2 guest→guest — no bridge line ever names
the call — so the harness now patches the EXE's IAT slot with a NAKED asm shim
(flag store + tail-jump; a C shim's prologue corrupts the guest's return
address — measured ehfix18 SIGSEGV). RACCOIN ladder: 7/7 REACHED. Go Next
demo.exe also reports `UNITYMAIN2 CALLED: REACHED (IAT-probed)` — two real
Unity games through the same gate.

### 2026-11-16 (later still) — full unresolved-list sweep; all 8 families load; 3 of 4 new families boot

- **Corpus gate: 23 ok / 0 LOAD FAILED** (build17). OpenTTD boots through
  window creation (60,268 unwind funcs) → clean ExitProcess(1); Veloren
  voxygen boots CRT → WinMain → clean ExitProcess(2); CDDA loads fully and
  boots to the named EH bounce-cap (verdict named, harness classification
  fixed the same session — see below).
- **The import chains peeled one symbol per build** until the FULL
  unresolved lists were pulled from the dry-run audits and fixed in one
  pass (build16): 5 midiOut (Close/Reset/ShortMsg/LongMsg/Prepare/Unprepare
  — no-device MMSYSERR_BADDEVICEID(4) answers), `ImmGetProperty` (0), 7
  gdi32 (CreatePalette/SelectPalette/RealizePalette — real handle-backed
  palette objects in the existing GDI object table, RealizePalette answers
  0 changed = truecolor identity map; SetDIBColorTable; GetGlyphIndicesW/
  GetOutlineTextMetricsW/GetGlyphOutlineW — no font engine: error glyph
  0xFFFF, empty-but-valid metrics, GDI_ERROR for outline rasterization),
  16 msvcrt (_close with fd-table detachment so a stale FILE gets EOF not
  a use-after-close; _telli64; _wfullpath; _wopen with VFS + fd-table
  integration; _wutime64 via /proc/self/fd utimensat; _fpreset no-op —
  guest FPU state is per-thread host state; _localtime64 with guest tm
  layout; _onexit through a captureless-trampoline atexit registry —
  guest fn pointers must go through the guest-call gate even at process
  exit; strspn; acosf/asinf/atanf/ceilf/coshf/log10f/sinhf/tanf 1:1
  libm delegates), 4 crypt32 (CertFreeCertificateChainEngine no-op on
  the empty store; CertGetEnhancedKeyUsage/CertSetCertificateContextProperty
  CRYPT_E_NOT_FOUND; CertVerifyTimeValidity -1).
- **winmm timer bug caught by the guest contract, not by inspection:**
  mm_timer_thread passed the raw epoch-ns deadline to nanosleep, which
  wants a RELATIVE delta — the timer thread slept ~1.7 billion seconds and
  never fired. The guest PE's fixed iteration-count spins could never catch
  it (they expired before the 20ms timer); the guest now waits on
  timeGetTime deadlines (500ms oneshot / 120ms periodic windows). Suite
  11/11 ALL PASS, guest returned 0xff: oneshot fires the guest TimeProc
  from a host thread through trt_call_guest with the real timer id as
  arg0; timeKillEvent freezes a periodic within ≤1 in-flight fire.
- **CDDA false-negative fixed (report-format):** the ehfix livelock
  terminator logs its named verdict THEN aborts; the corpus-run child
  installed fault handlers for SEGV/ILL/BUS/FPE/TRAP but not SIGABRT, so
  the parent classified the boot "died on signal 6 without the named
  report" even though the ring held the verdict. SIGABRT now routes
  through the same fault_report (ring dump + _exit(42)).
- **Diagnosis discipline note:** the stale-archive trap hit twice this
  wave (test_winmm binary from 01:05 predating the 14:49 archive; guest
  .obj not recompiled after a .c edit). Verify binary freshness via
  stat, not via which binary you THINK the build script rebuilt. And a
  probe that calls a TRT_GUEST_ABI (ms_abi) runtime function through a
  SysV declaration reads its args from the wrong registers — probe
  declares must carry the same ABI as the runtime surface.

### 2026-11-16 (later) — corpus grows to 8 families; named-gap fixes

- **Three new engine families staged** (all open-source, SHA-verified,
  outside git):
  - OpenTTD 15.3 — native C++/own engine; OpenGL + XAudio2_8 + WINMM
    multimedia-timer surfaces.
  - Cataclysm-BN 0.13 — native C++, SDL2 + GL/GLES + D3D11/12; heaviest
    PE yet (632 MB, ships mesa/libGLESv2/WinPix/steam_api64).
  - Veloren (weekly) — Rust + wgpu (Vulkan/DX12/GL backends); first Rust
    engine entry, exercises bridge_vulkan + shader-cache knob end-to-end.
- **Named gaps fixed from the gate** (each was a LOAD FAILED with the
  symbol named; fix = real implementation, not a whitelist edit):
  - `winmm!timeSetEvent/timeKillEvent` — real host-thread multimedia
    timers (steady-clock schedule, atomic kill flag, trt_call_guest gate
    for the guest callback; OpenTTD's scheduler tick).
  - `shell32!DragFinish/SHBrowseForFolderW/SHGetPathFromIDListW` —
    "picker cancelled" (the API's own named failure) + drag cleanup.
  - `gdi32!GetPixelFormat/GetICMProfileW` — constant-1 pixel format
    (matches ChoosePixelFormat/DescribePixelFormat/SetPixelFormat) and
    honest "no ICM profile" (0).
- **DStorage C->BUILTIN conversion** (boot-determinism): UnityPlayer 6
  imports `DSTORAGE.dll!DStorageGetFactory` even when the game does not
  ship the DLL — the SYSTEM(C) route made Go Next boots depend on
  unproven console presence (the pre-existing MISMATCH line). New
  `bridge_dstorage.cpp`: factory probe answers S_OK with a real vtable;
  CreateQueue/OpenFile fail visibly (E_NOTIMPL + named log) so optional-
  path games degrade to stdio and required-path games name the gap.
- **Live proof of the unwinder hardening** (build9 corpus run): Go
  Next's UnityPlayer crash path now produces the named verdict
  `ehfix: unwind metadata rejected (ui=1a4a4894c base=180000000) ->
  unwind failed cleanly` where the same input SIGSEGV'd the host before
  wave 2. OrTMM terminates via the 100k-bounce named cap. RACCOIN boots
  7/7 and exits clean (EXIT(0)).

### 2026-11-16 — unwinder hardening + livelock cap + native DLL overrides

- **trt_RtlVirtualUnwind hardened** (kernel32 bridge): UnityPlayer's exception
  path (Go Next demo full-corpus run) handed the unwinder unwind metadata that
  is unmapped/corrupt — host SIGSEGV at `trt_RtlVirtualUnwind`, guest fault
  address `image+0x64a4894c`. The unwinder now validates the unwind-info
  pointer against registered guest images (`safe_unwind_info`) and returns a
  clean "unwind failed" verdict (0 = no handler) instead of crashing the
  host. A corrupt .pdata can never again take the runtime down.
- **EH bounce livelock capped** (kernel32 bridge, hw-fault handler): OrTMM
  (first native-C++ corpus entry) spins 14.5M identical (rip,fault) bounces —
  measured, was the cause of every OrTMM wedge/timeout. Per-thread cap at
  100k bounces now aborts with a named verdict ("guest livelocked in EH
  bounce loop").
- **Native DLL override (Proton WINEDLLOVERRIDES=native equivalent) landed**
  (module_table.cpp `try_native_override` + `is_overridable_module`): a
  game-shipped API DLL (dxvk's d3d9.dll, cnc-ddraw, dgVoodoo, ...) shadows the
  builtin bridge for overridable modules. Loader-critical modules are never
  shadowed. dxvk-2.5.3 x64 d3d9.dll verified: maps + seals + DllMain + boots
  as EXE entry (RtlAddFunctionTable registered 8822 funcs). This is the
  wiring path for existing third-party implementations.
- **corpus-run.sh**: per-EXE `timeout -s KILL 60` bound — a guest that never
  reaches WM_QUIT is a NAMED hang, not a gate wedge. Go Next demo.exe added
  to the EXE gate.
- **Two hardening holes found by writing the regression test** (same day):
  1. `ptr_in_guest_image`'s "wide host range" fallback accepted UNMAPPED
     addresses — the exact Go Next shape (metadata at `image+0x64a4894c`,
     fault `0x1a4a4894c`) would pass a range check and still SIGSEGV at the
     first `ui[0]` dereference. Fixed with a kernel-backed readability probe
     (write() to /dev/null returns EFAULT for unmapped pages, no signal;
     probe fd cached, errno saved/restored — unwinding runs inside exception
     dispatch).
  2. The chained-unwind forwarding loop (`UNW_FLAG_CHAININFO`) was unbounded:
     a self-referential chain (`fn->unwind` pointing back at itself) hangs
     the dispatcher forever — same livelock family as the OrTMM spin. Fixed:
     16-hop cap + self-reference check → named verdict, clean fail.
  Regression tests: `tests/test_unwind_safety.cpp` (6 cases: leaf pop,
  garbage fn_entry, unmapped metadata, valid minimal unwind still correct,
  unmapped-in-range probe rejection, chained self-loop). Wiring:
  `tools/build-test.sh` step 8c.
- **OrTMM re-identified**: not native C++ — imports python313.dll (embedded
  CPython 3.13 app). First Python-family corpus entry; explains the EH
  livelock (CPython signal/EH machinery). COMPATIBILITY.md corrected.

### History — the one wall (now broken): our own SEH/unwind implementation.

**Import audit of the full RACCOIN stack** (`tools/audit_boot.py` — approximate;
the C++ resolver in `tests/audit_boot.cpp` is authoritative):

| PE | imports | audit gaps | real gaps after reconciliation |
|---|---|---|---|
| RACCOIN.exe | 72 | 1 | 0 (the 1 = game-dir export artifact) |
| UnityPlayer.dll | 546 | 52 | **dwmapi (2 fns)** — rest are false ws2_32 alarms |
| GameAssembly.dll | 252 | 48 | **GetWriteWatch family** — rest are baselib game-dir artifacts |
| baselib.dll | 129 | 27 | 0 real (ws2 false alarms) |

Why most gaps are false alarms: the Python tool's regex misses the ws2_32
registration style and can't know game-dir DLL exports (`UnityMain2`, the 12
C++-mangled `?Baselib_...` symbols) — those resolve at load from the game dir
(proven v0.29). **Fix the tool: its `__main__` guard is misspelled `'main'`**
so the CLI path never runs; the live session ran it via import (see
`tools/__pycache__`).

**Structural fact that changes the EH plan:**
- NO `__CxxFrameHandler*` / `__C_specific_handler` imports anywhere in the
  stack. The CRT and the C++ EH personality are **statically linked inside
  GameAssembly**. The guest brings its own EH — we must supply only faithful NT
  primitives.
- Every PE imports `KERNEL32!RtlVirtualUnwind` / `RtlUnwindEx` / `RtlUnwind`
  directly. All three ARE registered in both the kernel32 and ntdll bridge
  tables — but "registered" ≠ "faithful". That is the work.
- **IL2CPP raises EVERY managed exception as a C++ exception**
  (`il2cpp_codegen_raise_exception` → `Il2CppExceptionWrapper` — Unity's own
  IL2CPP internals doc). So C++ EH is load-bearing for gameplay, not an edge
  case: every `try/catch` in C# game code goes through our unwinder.

**In flight RIGHT NOW (orchestrated parallel agents, 2026-11-15 — do not duplicate):**
- (none — wave 5 (items 24/25/30/29/35/46-remainder) is complete and verified;
  next: console M0 when DP credentials arrive, or the command-list recording
  surface (item 27 remainder) on request.)

**LANDED (boot-depth wave, 2026-11-15, worktree `work/boot-ultrapool` @ b6a7b51,
all verified offline; gate rc=0, 42× ALL PASS, corpus 8/8 load, 5/5 run-depth):**
- **Ultra Pool (Godot 4.3 + GodotSteam) — FULL BOOT LIFECYCLE**: CRT → WinMain →
  window ('Ultrapool') → Vulkan Forward Mobile on the Tiramisu ICD → scene load
  (achievements/save/daily state) → pump → watchdog WM_CLOSE → Godot's own
  quit chain (`close_requested` → `SceneTree::_quit`) → ExitProcess(0). The
  wedge chain that stopped it, each fixed to real Win32 semantics:
  CV lost-wakeup (release→wait-registration serialized under a wake mutex,
  table 64→512 slots), WASAPI pacing (GetCurrentPadding timestamp-decay model
  — Godot paces solely on `buffer_size - padding`; the zero-collapse caused a
  97%-CPU mix storm holding the CS the main thread needed), MS_VC_EXCEPTION
  swallowed like the OS does (no debugger), EH dispositions + faithful
  `trt_C_specific_handler`, dwm `TRT_GUEST_ABI`, AttachConsole=FALSE +
  synthetic stdio (real pseudo-handles → log ring), Vulkan limits/features
  from real `vulkan_core.h` types, watchdog posts WM_CLOSE (Godot never reads
  WM_QUIT — measured against 4.3 sources). Regression-locked in
  `tests/test_wasapi_pacing.cpp` (padding contract + CV wake atomicity +
  huge-malloc refusal).
- **RACCOIN re-measured under the new sync surface**: with `--mock-d3d11`
  (the documented config) the 8/8 ladder holds AND the game now runs healthy
  for the full 10s until the watchdog WM_CLOSE, then exits through its own
  shutdown chain (`SetFullscreenState(0)` → TerminateProcess) — previously it
  never survived a watchdog interval. Plain `--exe` (stricter, real GPU path)
  reaches Unity init + pump, then dies in the Unity CrashReporter C++
  exception path (throw site escapes to a host frame; boundary swallow →
  WM_QUIT) — that escape attribution is the next named gap, not a sync bug.
- **Import-gap closures (fail-visible honest stubs)**: dbghelp
  `SymGetModuleBase64`/`SymFunctionTableAccess64`/`StackWalk64` (0/NULL/FALSE —
  no module table on console); winmm `waveOutMessage`/`Pause`/`Restart`/
  `BreakLoop`/`GetErrorText` + `waveInMessage` (MMSYSERR_BADDEVICEID).
  steam_api64.dll now maps + seals (was FAIL IMPORT) → corpus load 8/8.
- **`GetStdHandle` synthetic-stdio contract locked** in the guest_engine
  fixture; `corpus-run.sh` no longer crashes when the D3D12 triangle fixture
  is not yet built (named SKIP; ordering fix).

- **UWP host build re-verified (`build-host.sh` rc=0)**: the Linux-only CS-stall
  probe (`syscall(SYS_gettid)`, `::write(2,...)`) was breaking the clang-cl
  `--uwp` cross-compile (no POSIX fd 2 / sys/syscall.h in WINAPI_FAMILY_APP).
  Guarded behind `__linux__` so the probe still names stalls on the dev host
  while the UWP host build stays clean. Duplicated lock block in
  `trt_EnterCriticalSection` also collapsed. MSIX produced.
- **DirectInput8 COM personality** closes the `REGDB_E_CLASSNOTREG` gap:
  `CoCreateInstance(CLSID_DirectInput8)` now hands out the same object
  `DirectInput8Create` serves (A-twin -> W vtable; unknown IID -> E_NOINTERFACE
  honestly). Verified S_OK + E_NOINTERFACE, both named.
- **Items 24/25/30 — D3D12 descriptor wave** (`bridge_d3d12.cpp`): the silent
  `dev_view_noop` at device slots 17-24 is GONE. Real implementation:
  per-heap typed record store (`d3d12_view_record`, kind/dim/format/mips/
  slices/gpu_va/resource), live-heap registry with CPU-handle resolution
  (handle = cpu_base + idx*increment, boundary + range checked), CreateCBV/
  SRV/UAV/RTV/DSV/Sampler writing validated records (RTV/DSV require a live
  resource; CBV requires a nonzero GPU VA; sampler Filter/AddressU/V/W/
  MaxAnisotropy/ComparisonFunc range-checked; SRV/UAV parse both the BUFFER
  and TEX2D union arms; null-descriptor init requires BOTH pResource and
  pDesc null — (null, non-null) now fails VISIBLY in SRV and UAV),
  CopyDescriptors/CopyDescriptorsSimple with per-range expansion, cross-heap
  copies and bounds-checked fail-visible rejection. Command-list side:
  SetDescriptorHeaps (replacing, like the real API), Set[Compute|Graphics]
  RootDescriptorTable and RootConstantBufferView recorded per slot with
  soft-resolve WARN-once for unresolvable GPU handles (matching real
  draw-time faulting), CopyBufferRegion/CopyResource recorded.
  **Two real bugs the new suite caught and the orchestrator fixed:**
  1. `k_descheap` IID had `...90f7-12b7...` instead of `...90f7-127b...`
     (transposed byte pair) — every guest QI for ID3D12DescriptorHeap got
     E_NOINTERFACE. Fixed; full 24-IID audit of the bridge against the SDK
     header (xwin d3d12.h + mingw d3d12.h) now passes 24/24.
  2. WriteToSubresource/ReadFromSubresource packed box content at backing
     offset 0, ignoring box.left — for buffers (the standard upload path)
     box.left is a BYTE offset. Both now honor D3D12_BOX semantics (buffer:
     left = byte offset; texture: texel grid on the 4-byte-per-texel backing,
     top/front select start row/slice), bounds-checked fail-visible.
  Also: PSO creation is real (item 29) — CreateGraphicsPipelineState/
  CreateComputePipelineState validate the legacy descs (root-sig liveness,
  paired shader bytecode) and CreatePipelineState (slot 47) walks the
  D3D12_PIPELINE_STATE_STREAM subobject stream (16-byte headers, types
  0..28, payload-overrun checks, ROOT_SIGNATURE/VS/PS/CS recorded; garbage
  type or overrun -> E_INVALIDARG + named log). Struct mirrors pinned by
  static_assert vs the SDK (GPSD 656, CPSD 56, SRV/UAV 40, RTV/DSV 24,
  SAMPLER 52, stream desc 16 — SDK-verbatim SizeInBytes-first layout
  verified by header measurement).
  Evidence: NEW `tests/test_d3d12_desc.cpp` 59/59 ALL PASS (orchestrator-
  written and -run: handle arithmetic, all six view kinds, 8 fail-visible
  rejections, range + cross-heap + simple copies, cmdlist binding/copy
  recording, PSO legacy + stream + 2 garbage rejections, Write/Read
  subresource round-trip visible through Map, QI on heap/PSO/resource);
  regressions test_d3d12, test_d3d12_triangle, test_wave2 ALL PASS;
  RACCOIN `--exe --mock-d3d11` 1 booted / 0 failed (all relinked against
  the updated libtrt.a by the orchestrator).
- **Item 35 + item 46 remainder — memory ceiling + crash filter**
  (`bridge_mem.cpp`, `bridge_kernel32.cpp`): every mapped view tracked in a
  live-view registry; ceiling 64 live views (env `TRT_MAX_MAPS`, clamped
  8..4096); over-ceiling maps evict the LEAST-RECENTLY-USED view with a
  named log line, use-after-evict fails visibly; when nothing is evictable
  the map fails honestly (NULL + last-error + named log). LRU stamps
  updated on every map/unmap touch. SetUnhandledExceptionFilter/
  UnhandledExceptionFilter added to the kernel32 table (filter pointer
  stored, named registration log) and wired into the runtime's fatal
  exception path: the stored filter is invoked with the exception record,
  verdict logged (`no-filter`/`handled`/`unhandled`), RaiseFailFastException
  semantics preserved, MiniDumpWriteDump's text crash report remains
  available on the fatal path.
  Evidence: NEW `tests/test_mem_crash.cpp` ALL PASS (70-map eviction
  sequence, TRT_MAX_MAPS clamp incl. 8192->4096 probe, unevictable honest
  failure, filter registration + invocation with correct record, no-filter
  path); regressions test_kernel32b 17/17, test_ntdll 34/34, test_ole 16/16,
  test_engine ALL PASS (relinked + rerun by the orchestrator).
- **Gate wiring**: `tools/build-test.sh` suites 39 (test_d3d12_desc) and 40
  (test_mem_crash) appended after suite 38.

**LANDED (wave 4, 2026-11-15, all verified offline):**
- **Item 16 / 1K — ntdll native surface** (`bridge_ntdll.cpp`, `bridge_ole.cpp`):
  NtCreateFile (OBJECT_ATTRIBUTES/UNICODE_STRING parse, `\??\` strip,
  RootDirectory-relative, 6 dispositions -> kernel32 mapping, full
  dos_to_ntstatus) + NtReadFile routed onto the same VFS the kernel32 file
  surface uses; NtCreateEvent/NtSetEvent/NtWaitForSingleObject/
  NtSignalAndWaitForSingleObject on the kernel32 event primitives;
  RaiseFailFastException honors fail-fast (logs, then terminates — tested
  by writing DONE to results.txt first); CoWaitForMultipleHandles STA wait
  (S_OK signaled / RPC_S_CALLPENDING timeout / E_HANDLE).
  Evidence: `test_ntdll` 34/34, `test_ole` 16/16 (both rerun by the
  orchestrator, not just the agent).
- **Item 12 remainder — kernel32 gap** (`bridge_kernel32_gap.cpp`,
  `bridge_kernel32.cpp`, `bridge_nvapi.cpp` NEW): job objects
  (CreateJobObjectW/A, SetInformationJobObject, AssignProcessToJobObject,
  TerminateJobObject, OpenJobObject honest-fail, IsProcessInJob),
  AllocConsole/FreeConsole/AttachConsole, Toolhelp module walk UPGRADED
  from honest-fail to a real image-registry-backed MODULEENTRY32W walk,
  Get/SetProcessMitigationPolicy, Get/SetThreadStackGuarantee;
  `bridge_nvapi.cpp` honest not-supported stubs for nvapi/nvml/atiadlxx
  (NVAPI_NVIDIA_NOT_FOUND / NVML_ERROR_NOT_SUPPORTED / ADL_ERR_NOT_SUPPORTED,
  log-once), wired into host `main.cpp` boot.
  Evidence: `test_kernel32b` 17/17 (rerun by the orchestrator).
- **BUG-6 remainder — version-aware QI everywhere** (`bridge_d3d12.cpp`):
  every D3D12 object type now answers QI from a verified IID chain
  (IUnknown + Object + DeviceChild + Pageable + own versions) and fails
  VISIBLY with E_NOINTERFACE for anything beyond its vtbl — previously
  `base_qi_self` accepted ANY IID, handing a 60-slot vtbl to a game that
  QIed GCL4 (77 slots) = call off the end. Command-list vtbl extended
  60 -> 84 slots (GCL1..9; 24 ABI-correct logged no-op stubs, arities from
  the SDK header), fence 11 -> 12 with a REAL Fence1::GetCreationFlags
  (CreateFence now records flags), and the FABRICATED Object GUID
  (c7e2816f... — not in the SDK header at all) replaced by the real
  c4fec28f. All IIDs verified verbatim against the xwin SDK d3d12.h.
  Evidence: `test_d3d12` ALL PASS with 15 new versioned-QI checks (GCL..GCL9
  QI, cross-type E_NOINTERFACE, Fence1, Object-IID), TRIANGLE GATE 12/12,
  test_wave2 ALL PASS, RACCOIN `1 booted, 0 failed`.
- **BUG-6 applied to DXGI** (`bridge_d3d.cpp`): adapter vtbl 19 slots
  (IDXGIAdapter4 chain), factory 30 -> 32 (Factory7
  Register/UnregisterAdaptersChangedEvent real), output 23 -> 29
  (Output2..6: SupportsOverlays honest FALSE, CheckOverlaySupport
  DXGI_ERROR_UNSUPPORTED, GetDesc1 with real RDNA2-class colorimetry),
  all version chains verified against the SDK headers, static_asserts
  pinning every size. Evidence: `test_wave2` wave2f block ALL PASS.
- **Item 17 / 1L — WinRT ThreadPool + D3DKMT** (`bridge_winrt.cpp`,
  `bridge_gdi.cpp`): IThreadPoolStatics (RunAsync/RunWithPriorityAsync/
  RunWithPriorityAndOptionsAsync — work items run on a detached host
  thread, returned IAsyncAction reports Completed; IIDs verified against
  mingw windows.system.threading.h/asyncinfo.h/windows.foundation.h);
  D3DKMTOpenAdapterFromLuid/CloseAdapter/EnumAdapters honest synthetic
  Xbox-APU adapter + D3DKMTQueryAdapterInfo STATUS_NOT_IMPLEMENTED
  (fail-visible); Windows.Gaming.Input event surface VERIFIED
  already-present (GamepadAdded/Removed/FromGamepad were landed with the
  Gamepad statics).
  Evidence: `test_console_pack` ThreadPool block ALL PASS (handler invoked
  off-thread, AsyncStatus::Completed), `test_gdi` D3DKMT block ALL PASS.
- **Item 19 — save VFS overlay VERIFIED already-implemented**: Z: writes
  route to `<C: mount>/_z_write/` with whiteout-aware reads
  (`vfs.cpp` trt_vfs_resolve_utf16_rw); existing test_engine coverage
  ("Z:\ro.txt write succeeds via _z_write overlay") is the evidence. No
  code change needed — the gap list was stale.
- **Wiring**: `build-test.sh` suites 37 (ntdll) + 38 (kernel32b) appended;
  `main.cpp` registers nvapi stubs. Full canonical gate running as final
  verification of the whole wave.

**LANDED (same orchestrated wave, 2026-11-15, all verified offline):**
- **BUG-1 FIXED** (`bridge_d3d12.cpp`): CommandQueue vtbl rebuilt 19 -> 13
  entries in exact ABI order — ExecuteCommandLists at slot 8 (was a no-op
  UpdateTile at slot 8, exec at 10), Signal 9, Wait 10, ts-freq 11,
  clock-cal 12. ECL now records submitted lists (TRIANGLE GATE-ready).
  static_assert(13) + runtime `d3d12_vtbl_selfcheck()` fail-named.
- **BUG-6 FIXED**: Device QI chain extended to ID3D12Device5-9 (SDK-verbatim
  GUIDs; the master prompt's example Device5 GUID was wrong, headers win).
- **Sprint-1 1B/1C LANDED** (`bridge_kernel32.cpp`): 9 Interlocked* exports
  (fixed-width i32/i64, correct new/old-value semantics) + RtlCopy/Move/
  Zero/FillMemory, registered on kernel32+kernelbase, Rtl* merged into ntdll.
- **Sprint-1 1D VERIFIED already-registered** (GetSystemInfo/NativeSystemInfo/
  GetEnvironmentVariableW/VirtualAllocEx/VirtualFreeEx were in the merged
  table; the master prompt's gap list was stale here).
- **Sprint-1 1J LANDED** (`bridge_res.cpp` NEW): PE resource directory walk —
  FindResourceW/A/ExW, SizeofResource, LoadResource, LockResource (returns
  real mapped-image pointer), Unlock/FreeResource, EnumResourceNamesW,
  LoadStringW (real string-table format). Section-relative offset rebasing
  measured the hard way (first walk SIGSEGV'd treating child offsets as
  image-relative). Registered on kernel32; wired into host `main.cpp` boot +
  `build-host.sh` + `build-test.sh` suite 35.
- **Test evidence**: `test_d3d12` 24/24 ALL PASS (new), `test_wave1` 112/112
  (28 new), `test_res` 20/20 (new; real guest PE with hand-built .rsrc via
  `tools/build_guest_res.sh` + `tools/gen_guest_res.py`), `test_wave2` ALL
  PASS after slot-constant update (10->8, 14->9), test_engine/test_stage5/
  test_libload ALL PASS, **RACCOIN re-booted `1 booted, 0 failed`** with the
  whole wave linked (log `/tmp/opencode/raccoin-reswave.log`).
- **Sprint-1 1A LANDED** (`bridge_gdi.cpp`, measured — the master prompt's
  list was stale): GetDC/ReleaseDC were user32-only, now shared into the
  gdi32 table (same handler, both tables); GetStockObject case 10
  (OEM_FIXED_FONT) returned NULL — splash-text killer — now a real handle,
  stock ids 11-19 labels corrected to the real Win32 table; BitBlt/
  StretchBlt/PatBlt/StretchDIBits/SetDIBitsToDevice/TextOut*/ExtTextOut*/
  SaveDC/RestoreDC/SetMapMode made fail-visible (named log lines, never
  silent). CreateCompatibleBitmap + GetDeviceCaps were already real.
  `test_gdi` ALL PASS both argless (module-table resolution) and with the
  legacy guest-PE flow; RACCOIN re-booted `1 booted, 0 failed` again with
  GDI wave linked (log `/tmp/opencode/raccoin-gdiwave.log`).
- **Sprint-2 item 21 LANDED — THE D3D12 TRIANGLE GATE**
  (`tests/test_d3d12_triangle.cpp` + `src/guest/guest_triangle.c` +
  `src/guest/d3d12.def`/`dxgi.def`): a synthetic freestanding game walks the
  FULL D3D12 render path through the real loader — device -> queue ->
  allocator -> list -> rootsig -> PSO -> committed vertex buffer ->
  SetRootSignature/SetPipelineState/IASetVertexBuffers (GPU VA read back
  from the resource itself) -> DrawInstanced(3,1,0,0) -> Close ->
  ExecuteCommandLists (slot 8) -> fence SetEventOnCompletion/Signal ->
  CreateSwapChainForCoreWindow (queue-as-device, the D3D12 DXGI rule) ->
  Present. 12 ladder markers printed; host cross-checks the guest's queue
  pointer via `trt_d3d12_queue_recorded_count` (new accessor) — the
  submission RECORDED host-side on the object the guest created.
  **The gate caught a real ABI bug on its first run**:
  `fake_CreateSwapChainForCoreWindow` was missing the `pFullscreenDesc`
  param (6 vs the real 7-param ABI) — the out-pointer read one stack slot
  off, saved from a null write only by its own null check ->
  E_NOINTERFACE for every UWP D3D12 game. Fixed in `bridge_d3d.cpp`
  (CreateSwapChainForHwnd was already correct). Plus fail-visible log
  lines for DrawInstanced/ECL, ABI static-asserts pinning the resource(15)
  /fence(11)/cmdlist(60) vtbls, `CreateEventW`/`WaitForSingleObject` added
  to the kernel32 def stub. Suite 36 in build-test.sh + gate stage in
  corpus-run.sh.
  Evidence: 12/12 markers + ALL PASS (log `/tmp/opencode/tri.log`);
  regressions test_game_boot/test_d3d12/test_wave2/test_d3d ALL PASS;
  RACCOIN `1 booted, 0 failed` (log `/tmp/opencode/raccoin-tri.log`).

---

## W0 — hygiene first (fast, no design, high value)

1. **Commit the WIP as v0.35** (EH machinery + 9 new bridges + wave1 +
   UNITYMAIN2 IAT probe). ~27.8k insertions across 133 paths uncommitted
   after the RACCOIN boot — the single biggest risk to the project.
2. **gdb discipline:** never `gdb -batch -ex "commands 1" ...` — gdb reads the
   command-list body from the tty, wedges at the `>` prompt, and the guest never
   starts (that is exactly today's PID-301706 hang: no child process, stdin on
   `/dev/pts/179`). Use `gdb -batch -x script.gdb`.
3. **Ladder instrumentation fixes** (`tests/test_corpus_run.cpp observe()`):
   - D3D11 marker must also accept `"FL contract ->"` — today a real device
     acquisition reports `-` (false negative, hides progress).
   - UNITYMAIN2 marker must match only a *call* log line, not
     `"bind UnityPlayer.dll!UnityMain2"` (false positive).
4. **Adopt RACCOIN into the corpus:** copy to `corpus-local/raccoin/`
   (gitignored), add to `tools/corpus-run.sh` STAGE 7 EXES, so its depth is
   gate-enforced instead of ad-hoc.
5. `tools/audit_boot.py`: `'main'` → `'__main__'`.

## W1 — the EH wall — **BROKEN THROUGH (2026-11-15, ehfix9–19; DONE for C++ EH)**

Was: "the guest throws and we cannot walk/unwind". Now: faithful x64 SEH on
the `.pdata` walk is IMPLEMENTED and gated —
`bridge_kernel32.cpp`: `trt_RaiseException`, `trt_RtlUnwindEx`,
`call_pe_eh_handler`, `trt_RtlVirtualUnwind`, VEH pass-1, PLUS the measured
hard parts: pass-2 funclet resume records with geometric stale-purge and
per-guest-call retirement, and the host→guest boundary-escape swallow
(UnityPlayer wrapper-catch). Acceptance evidence: RACCOIN + Go Next boot 1/0
with the 7/7 ladder; `tests/test_guest_eh.cpp` ALL PASS including
funclet-rethrow and boundary-escape fixtures. What remains open below is the
scoping record; the wall itself is down.

1. **`RtlVirtualUnwind` decode fidelity** — `UNWIND_INFO` (Version/Flags/
   SizeOfProlog/CountOfCodes/Frames), all `UWOP_*` opcodes with real prolog
   mimicry (push of RBP, big/small stack allocs, save XMM, `UWOP_SET_FPREG`),
   `UNW_FLAG_CHAININFO` chained entries, personality + LSDA extraction, the
   `Flags & UNW_FLAG_EHANDLER|UHANDLER` cases, epilogue-boundary cases.
2. **Phase 1 (search):** walk frames from the faulting RIP through `.pdata`,
   call each personality with a correct `EXCEPTION_RECORD` +
   `DispatcherContext` (FunctionEntry/EstablisherFrame/ContextRecord/
   HandlerData=LSDA). Honor CONTINUE_SEARCH / CONTINUE_EXECUTION / HANDLER.
   VEH first-chance pass already exists — keep it ahead of the frame walk.
3. **Phase 2 (unwind):** `RtlUnwindEx` walks from the established frame to the
   target, runs every frame's unwind handlers/funclets (C++ destructors!), and
   restores the CONTEXT at the target rip. `call_pe_eh_handler` already pins
   the return PC so unwinding crosses the ELF host boundary — keep that.
4. **The ugly cases the logs already hit:** `flags=1` (NONCONTINUABLE),
   exceptions raised *during* unwinding, `RaiseException` from a guest worker
   thread (per-thread TEB — just fixed, keep its test), and the second
   exception with a null caller frame (unhandled → real termination path).

**Donors (license-checked):**

| Donor | License | Use |
|---|---|---|
| LLVM libunwind `Unwind-seh.cpp` + `UnwindCursor.hpp` | Apache-2.0 w/ LLVM exception | complete SEH-over-.pdata machinery incl. personality dispatch. Vendor with NOTICE, or port semantics line-by-line into MIT code — **license decision needed** (Apache patent grant + NOTICE retention) |
| Wine `dlls/ntdll/signal_x86_64.c` + unwinding | LGPL 2.1 | semantics only (repo rule: no vendoring) |
| MSDN "x64 exception handling" internals doc | free | the spec for UNWIND_INFO/UWOP decoding |

**Do NOT implement** `__CxxFrameHandler3/4` — measured absent from every import
table; the guest's statically-linked CRT provides its own personality. Implement
only NT primitives, faithfully.

**Test:** `tests/test_guest_eh.cpp` — clang-built freestanding guest exe with:
nested try/catch (C++), throw across guest→bridge→guest, catch-type matching +
rethrow, destructor unwinding during phase 2, SEH `__try/__except` AV handler,
raise-inside-funclet. All through the real IAT path. This is the test that
would have caught today's fault.

**Acceptance (UPDATED 2026-11-15):** ~~RACCOIN ladder prints `UNITYMAIN2
CALLED` (fixed marker) + `PUMP ENTERED`~~ → **DONE: full ladder 7/7 REACHED
(UNITYMAIN2 via naked-asm IAT probe, ehfix19), RACCOIN + Go Next demo.exe both
boot 1/0, all repo gates green.** Remaining for "any game": x86-32 (PE32)
loader + translator ladder.

## W2 — measured bridge gaps (small; do alongside W1)

1. **`GetWriteWatch` / `ResetWriteWatch`** — the hidden IL2CPP requirement.
   il2cpp's GC is **Boehm-Demers-Weiser (bdwgc)**, incremental by default in
   modern Unity. bdwgc *probes* at runtime:
   `GetProcAddress(kernel32, "GetWriteWatch")` + a
   `VirtualAlloc(NULL, page, MEM_WRITE_WATCH|MEM_RESERVE, PAGE_READWRITE)`
   probe call — GWW_VDB is enabled only if BOTH succeed; otherwise it falls
   back to MPROTECT_VDB (write-protect + fault) or DEFAULT_VDB (all pages
   dirty: correct, but incremental GC off → frame hitches).
   - **DONE 2026-11-15 (honest, dual-surface):** `VirtualAlloc` accepts
     `MEM_WRITE_WATCH` as a request modifier (real Windows semantics); on the
     console `_WIN32` passthrough to the real kernel32 GWW; on Linux gate,
     soft-dirty emulation via `/proc/self/pagemap` gated behind a one-time
     liveness probe (bit 55, and bit 63 present required — this 6.14 kernel
     marks fresh absent pages dirty; a dead bit or clear_refs wipe would
     silently lie "clean" to a GC = heap corruption, so it fails named →
     bdwgc falls back to MPROTECT_VDB). Known limitation: Linux
     `ResetWriteWatch` clear_refs "4" is process-wide (console: real
     per-range API). Gate: `test_wave1` Wave 6G (GC probe exact-shape,
     2-page dirty report, reset-clean).
2. **`dwmapi.dll`** (UnityPlayer: `DwmGetWindowAttribute`,
   `DwmSetWindowAttribute`) — honest record / fail-visible.
3. After W2: run the **C++-resolver audit** (`tests/audit_boot.cpp`) over all
   four RACCOIN PEs. Expect only game-dir exports outside builtin.

## W3 — console M0 + the W^X truth (research-backed expectations)

Research (xbox-uwp-lab `devmode-limitations.md`, quoting dosbox-pure-uwp /
xenia-canary-uwp / PPSSPP source): **UWP AppContainers enforce strict W^X** —
plain `VirtualAlloc(PAGE_EXECUTE_READWRITE)` fails; the `...FromApp` family
works; PPSSPP's `PlatformIsWXExclusive()` returns TRUE for UWP; the RW→RX flip
is mandatory, not defensive. (XBSX2 is the unverified outlier using
`codeGeneration`+`runFullTrust` with plain RWX.)

The current WIP *allows* RWX in `trt_VirtualAlloc`/`uwp_protect`. Keep it, and
let the already-coded probe decide (`main.cpp`: `probe: jit RWX alloc ->
ALLOWED/DENIED`, W→RX transition, DLL presence). Outcome table:

| Probe result | Consequence |
|---|---|
| RWX DENIED + W→RX ALLOWED (expected) | two-phase shim: guest RWX requests become RW + auto-protect-RX on `FlushInstructionCache`; patch mono/Boehm alloc at IAT level (we own the loader) |
| RWX ALLOWED | keep as-is (trivial path) |
| both DENIED | Mono/JIT games (G3) blocked on console — report honestly, do not fake |

**M0 order (from the contract):** deploy signed MSIX → boot on TV → swapchain
HRESULT 0 + selftest `0x6494cb0d` → screenshot → pull `tiramisu.log` →
phone-transfer round trip → **then** the JIT probe → **then** RACCOIN boot.
No console claim without a TV screenshot + the log pull.

## W4 — after first frame (the game loop, per master prompt G2/G4/G5)

- Pump: host-driven message pump at vsync, `Present(1)` pacing, FPS in log.
- Audio: FMOD→WASAPI (bridge exists, offline test green) — prove ≥5s of real
  game PCM in the log.
- Input: XInput (bridged) + keyboard; gamepad-as-mouse fallback.
- Save: VFS `C:\` writes — verify RACCOIN's save lands and round-trips.
- Cutscenes: OrTMM ships `binkw32.dll` (in corpus); Unity video = mfplat.
  Fail-visible skip first; build mfplat/IMFSourceReader only when a game's
  boot blocks on it.
- `discord_game_sdk.dll` (in OrTMM corpus): loads as game-shipped DLL; expect a
  named handshake failure and continue.
- `UnityCrashHandler64.exe`: `CreateProcessW` is console-denied — keep the
  named denial; ensure the guest treats it as non-fatal.

## W5 — gonna be needed next (dependency order + triggers)

| # | Item | Status | Trigger to start |
|---|---|---|---|
| 1 | Streaming/windowed `MapViewOfFile`, unmap reclamation, >64 maps | **IN FLIGHT** (test_wave1.cpp) | — |
| 2 | `DispatcherQueue` passthrough | **DONE 2026-11-15** (offline DQC/DQ factories + QI in bridge_winrt.cpp; test_console_pack §9) | UnityPlayer calls it early; on console combase is REAL — passthrough first |
| 3 | GetWriteWatch honest impl (W2) | **DONE 2026-11-15** (dual-surface honest: console passthrough / Linux soft-dirty+probe; test_wave1 Wave 6G) | frame hitches / GC assert in log |
| 4 | IOCP/OVERLAPPED async + dstorage | dstorage DllMain already runs | streaming asserts |
| 5 | Memory ceiling (5 GB game budget) | partly in flight (streaming maps) | any real game OOM |
| 6 | Perf: pump/present pacing, guest-on-render-thread | session-2 Wave-1 plan | first-frame stutter |
| 7 | D3D12 | not started | an engine that refuses D3D11 (RACCOIN negotiated D3D11 fine — UE-class is the trigger) |
| 8 | x86-32 (G8) | **license decision pending** | a real 32-bit target chosen |
| 9 | Steam depot downloader | `tools/steam_depot.py` exists (auth-only) | sideloading real GB-sized games |

## NOT needed yet (scope discipline — cut from any plan)

- Anti-cheat / DRM bypass — contract rule, always.
- OpenGL/gl4es — no corpus demand.
- Steam client emulation beyond auth — rule.

---

## The distance, honestly

- **Offline (Linux gate):** a real Unity IL2CPP Steam game boots through engine
  + IL2CPP init, window up, D3D11 device acquired — **one wall away from
  UnityMain2 + the pump: our own SEH unwinder.**
- **Console (the actual goal):** G1 has never run; G3's W^X answer is
  unmeasured; audio/input/save unproven on hardware. **3–4 unmeasured walls.**
  The probe sequence in W3 converts all of them from speculation to measurement
  in one deploy session.
