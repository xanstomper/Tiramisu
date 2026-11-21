# mono_jit_init crash — offline code analysis

**Date:** 2026-11-20 ~16:45 EST
**Analyst:** WooWop subagent (offline analysis only; no console touch, no builds, no clones)
**Status:** Root cause identified with high confidence — full chain verified in the Mono 6.8.0.105 source. Awaiting opencode's .108 crash data for confirmation.

## 1. Verdict

**The crash is a NULL-dereference write inside `mono_arch_create_sdb_trampoline`, reached from `mono_arch_init` during `mini_init` — because the code manager's first executable-memory allocation is denied by the AppContainer (ACG), returns NULL, and the trampoline emitter writes to address 0 with no NULL check.**

- Predicted fault signature: **write access violation at address 0x0000000000000000**, in `mono_arch_create_sdb_trampoline` (tramp-amd64.c), called from `mini_get_breakpoint_trampoline` <- `mono_arch_init` <- `mini_init` <- `mono_jit_init_version`.
- Why now: the host runs Mono in `--interpreter` mode, which sets `mono_use_interpreter` but NOT `mono_aot_only`. With `mono_aot_only == FALSE`, `mini_init` eagerly creates native trampolines — the SDB breakpoint trampoline first, then 7-9 JIT trampolines in `mono_trampolines_init`. Every one of those allocations requests `PAGE_EXECUTE_READWRITE`, which the AppContainer denies (proven by the .104 boot probe in the same log: `jit RWX alloc -> DENIED (err=87)`).
- **The fix is one line in the host, no Mono rebuild:** call `mono_jit_set_aot_only(TRUE)` in `trt_mono_run` (mono_host.cpp) before `mono_jit_init`. This is the iOS configuration (AOT-only + interpreter), proven on a platform with the same no-dynamic-code constraint.

## 2. The crash chain (every step verified in the Mono 6.8.0.105 source)

1. Host: `trt_mono_run` (src/trt/mono_host.cpp:271-274) calls `mono_jit_init(...)` after logging `opening ...\Celeste.exe`. The .104 log ends there — consistent with a crash inside `mono_jit_init`.
2. `mono_jit_init` -> `mono_jit_init_version` (mono/mini/driver.c:2783) -> `mini_init` (mono/mini/mini-runtime.c:4099).
3. `mini_init` calls `mono_arch_init()` (mini-runtime.c, ~line 4138) — EARLY, before the domain is created and before corlib is loaded.
4. `mono_arch_init` (mono/mini/mini-amd64.c:1432):
   `if (!mono_aot_only) bp_trampoline = mini_get_breakpoint_trampoline ();`
   `mono_aot_only` is FALSE: the host passes `--interpreter` via `mono_jit_parse_options`, which selects `MONO_EE_MODE_INTERP` (driver.c) — that sets `mono_use_interpreter = TRUE` but leaves `mono_aot_only` FALSE. (Contrast `MONO_AOT_MODE_INTERP`, which sets both.)
5. `mini_get_breakpoint_trampoline` (mono/mini/mini-trampolines.c:1689): `mono_ee_features.use_aot_trampolines` is FALSE (no AOT module loaded) -> `mono_arch_create_sdb_trampoline (FALSE, &info, FALSE)`.
6. `mono_arch_create_sdb_trampoline` (mono/mini/tramp-amd64.c:835):
   `code = buf = (guint8 *)mono_global_codeman_reserve (tramp_size + MONO_MAX_TRAMPOLINE_UNWINDINFO_SIZE);`
   **There is NO NULL check.** A few lines later `amd64_push_reg (code, AMD64_RBP)` emits the first trampoline byte with `*code++ = 0x55` — a write to address 0 if `code == NULL`.
7. `mono_global_codeman_reserve` -> `mono_code_manager_reserve` -> `new_codechunk` (mono/utils/mono-codeman.c:421) -> `codechunk_valloc` (mono-codeman.c:107) -> `mono_valloc (NULL, size, MONO_PROT_RWX | ARCH_MAP_FLAGS, ...)` (mono-codeman.c:130) -> `VirtualAlloc (..., PAGE_EXECUTE_READWRITE)` (mono/utils/mono-mmap-windows.c:81-96; `mono_mmap_win_prot_from_flags` maps MONO_MMAP_READ|WRITE|EXEC to PAGE_EXECUTE_READWRITE).
8. **VirtualAlloc RWX is DENIED under the AppContainer (ACG).** Proven by the .104 boot probe: `jit RWX alloc -> DENIED (err=87)`. Both attempts in `codechunk_valloc` (preferred address, then NULL) fail -> returns NULL -> `new_codechunk` returns NULL -> `mono_code_manager_reserve` returns NULL (it propagates NULL; it does not assert or abort).
9. `code = buf = NULL` -> `amd64_push_reg` writes to address 0 -> **ACCESS VIOLATION, write to 0x0**. Silent: no error print (the AV is inside the emitter, before any logging) and no crash dump (patch 06: `AddVectoredExceptionHandler` is NOT installed under UWP, so the AppContainer's default unhandled-exception path just terminates the process).

**The second shoe:** even if the breakpoint trampoline were skipped, `mono_trampolines_init` (mini-trampolines.c:1274, called later in `mini_init`) eagerly creates 7-9 more trampolines via `create_trampoline_code` (JIT, JUMP, RGCTX_LAZY_FETCH, AOT, AOT_PLT, DELEGATE, GENERIC_VIRTUAL_REMOTING, VCALL) — the same RWX-denial crash, in a different caller. It returns early ONLY if `mono_aot_only` is TRUE. So a fix that only skips the breakpoint trampoline is insufficient; `mono_jit_set_aot_only(TRUE)` covers both.

## 3. The fix

### Fix A (RECOMMENDED — host, 1 line, no Mono rebuild)

In `src/trt/mono_host.cpp`, in `trt_mono_run`, before the `mono_jit_init` call (after `mono_jit_parse_options`, ~line 270):

`mono_jit_set_aot_only (TRUE);  // AppContainer: no RWX, no JIT — interpreter only (iOS config)`

- Declared in `<mono/jit/jit.h>` (mono/mini/jit.h:44; defined in mono/mini/driver.c:2816) — the host already includes it (mono_host.cpp:114). No new include needed.
- Effect: `mono_arch_init` skips the breakpoint trampoline; `mono_trampolines_init` returns early; the JIT refuses to compile (all execution via the interpreter, which is the intent).
- Safety: this is exactly Mono-on-iOS (AOT-only + interpreter, no dynamic code). The interp installs its own compile callback (`mini_install_interp_callbacks`), so method execution never touches the JIT; P/Invoke is handled by the interp's own call machinery (no JIT wrapper needed). The 7-9 JIT trampolines are only used by JIT-compiled code, which never runs in this configuration.
- After this fix, the NEXT risk is any runtime codegen during execution (e.g. something calling `mono_jit_compile_method` directly or requesting a marshal wrapper through the JIT). In AOT-only mode those return NULL / fail cleanly rather than crashing on RWX — but if Celeste hits one, the fix is Fix C.

### Fix B (Mono patch — INSUFFICIENT alone, do NOT use as the only fix)

- mini-amd64.c:1432: `if (!mono_aot_only && !mono_use_interpreter)` — skips the breakpoint trampoline in interp mode.
- ALSO needed: mini-trampolines.c:1274: `if (mono_aot_only || disable_tramps || mono_use_interpreter) return;` — skips the 7-9 JIT trampolines.
- Requires a Mono UWP rebuild. Fix A achieves the same with one host line and no rebuild. Only do Fix B if Fix A is rejected for some reason.

### Fix C (follow-up — ACG-safe code manager; for P/Invoke wrappers and any future runtime codegen)

- The boot probe proved `W->RX transition -> ALLOWED (err=0)`. The code manager could allocate chunks as RW, emit code, then `VirtualProtect` to RX (in `mono_code_manager_commit`, or per-reserve). This would make ALL code allocation work under ACG. Invasive (the reserve/commit API is used inconsistently; some callers never commit), so do it AFTER Fix A unblocks the boot, only if a later crash shows runtime codegen is needed.

### Fix D (defense in depth — NULL checks in trampoline creators)

- tramp-amd64.c:835: check `mono_global_codeman_reserve`'s return; return NULL instead of writing to NULL. Same for `create_trampoline_code` (mini-trampolines.c) and every other `mono_code_manager_reserve` caller. Turns a silent AV into a clean failure. Worth landing regardless.

### What NOT to do

- Disabling SDB (DISABLE_SDB) does NOT fix this: `mono_arch_init`'s breakpoint-trampoline creation does not depend on SDB (the `if (!mono_aot_only)` guard is unconditional).
- Fixing only the breakpoint trampoline (Fix B without the trampolines_init patch) just moves the crash to `mono_trampolines_init`.

## 4. What .108's crash data will show (prediction)

Opencode's .108 SEH capture (fault address + RVA + 24-frame stack) should show:

- **Fault address: 0x0000000000000000**, access type WRITE.
- **Top frames:** `mono_arch_create_sdb_trampoline` (tramp-amd64.c, in/near `amd64_push_reg`) <- `mini_get_breakpoint_trampoline` (mini-trampolines.c:1701) <- `mono_arch_init` (mini-amd64.c:1434) <- `mini_init` (mini-runtime.c) <- `mono_jit_init_version` (driver.c:2783) <- `mono_jit_init` <- `trt_mono_run` (mono_host.cpp:271-274).
- **If the fault address is 0x0 and the stack matches:** H1 confirmed; land Fix A.
- **If the fault address is non-zero or the stack shows a different function:** H1 refuted; go to H2-H8 below. In particular, a fault inside `create_trampoline_code` (same NULL-write pattern, different caller) would mean the breakpoint trampoline somehow succeeded and the crash is the "second shoe" — Fix A still covers it.
- **If the stack shows corlib/metadata functions** (`mono_assembly_load_corlib`, `mono_class_load_from_name`, `mono_image_open`): the crash is later than predicted — see H4.

## 5. Other hypotheses (ranked)

### H2. The "second shoe": mono_trampolines_init's 7-9 JIT trampolines (same mechanism)

Same RWX-denial -> NULL -> unchecked write, in `create_trampoline_code` (mini-trampolines.c). Only reachable if the breakpoint trampoline is skipped/fixed first. Fix A covers it. (If .108 shows this stack instead of H1's, the diagnosis is the same and Fix A is still the fix.)

### H3. Runtime codegen during execution (after mono_jit_init returns)

Once Fix A lands, the next RWX-denial risk is any JIT/marshal-wrapper allocation during Celeste's execution (P/Invoke wrappers, delegate trampolines). In AOT-only mode these fail cleanly (`mono_jit_compile_method` returns NULL) rather than crashing — but if the interp falls back to the JIT for something, it could surface as a managed exception or a NULL-return crash. Fix: Fix C (ACG-safe code manager) if/when observed. Not the init crash.

### H4. Corlib metadata / class-load fault (low)

During `mono_init_internal` -> `mono_assembly_load_corlib` -> ~30 `mono_class_load_from_name` calls. Evidence against: the packaged mscorlib.dll is byte-identical to the engine build's reference copy (4,811,264 bytes, MD5 772d56732a3249c36ea9c3ba07d9f22b, verified 2026-11-20); a failed corlib load prints an error and calls exit(1) (domain.c:643), not a silent crash; the UWP file-mmap path uses the correct AppContainer APIs (CreateFileMappingFromApp / MapViewOfFileFromApp, mono-mmap-windows.c) with proper error handling. The crash is also EARLIER than corlib load (`mono_arch_init` runs before the domain is created). Only revisit if .108's stack shows metadata functions.

### H5. SGen GC init / first GC (low)

`sgen_gc_init` (sgen-gc.c:3319): VirtualAlloc RW (nursery, card table) + CreateThread (worker pool, finalizer) — all allowed under AppContainer. The first GC (during corlib load) uses SuspendThread/GetThreadContext, which are called directly in mono-threads-windows.c (compiled in, mono-sources.txt:251) and resolve at load (the exe loads and runs). During init there is only one thread. The win64.S asm (`mono_context_get_current`, `copy_stack_data_internal_win32_wrapper`) was verified offset-correct against the MonoContext layout. Only revisit if .108's stack shows sgen/gc functions.

### H6. SDB debugger agent init (low)

`mono_debugger_agent_init` runs (DISABLE_SDB is not defined in the UWP build). It is mostly lazy (no socket until a debugger connects). It does not allocate code. Low risk. (Note: disabling SDB would NOT fix H1.)

### H7. mono_w32* inits (low)

`mono_w32file_init`, `mono_w32process_init`, `mono_w32mutex_init`, `mono_w32event_init`, `mono_w32semaphore_init` run first in `mono_init_internal`. They are handle-table inits; the w32file/w32process files were patched for WinRT (patches 04, 05, 11, 14, 17). No code alloc, no risky API at init time.

### H8. Interpreter init (eliminated)

`mono_ee_interp_init` (interp.c:6984): TLS alloc + option parse + table init (`mono_interp_transform_init`). No code alloc, no risky API.

## 6. Eliminated hypotheses (with evidence)

- **E1 Missing/corrupt mscorlib** — Present in package; byte-identical to reference (MD5 match); a load failure prints + exit(1), not a silent crash.
- **E2 CRT mismatch** — Exe and Mono lib both /MT (one static-CRT heap).
- **E3 Pre-init hook-install lock crash** — `mono_install_assembly_load_hook` takes no lock (g_new0 + list prepend only).
- **E4 Dynamic code manager HeapCreate(EXECUTE)** — `mono_code_manager_new_dynamic` has ZERO callers.
- **E5 Perfcounters shared-memory NULL deref** — Windows `mono_shared_area` is just malloc (mono-mmap-windows.c:264); no named objects.
- **E6 win64.S translation bug** — Offsets verified correct against the MonoContext layout; Unity ships the same TEB struct on Xbox.
- **E7 TEB/TLS offset wrong** — Struct matches the real x64 TEB (TlsSlots at 0x1480).
- **E8 dllmap-hook deadlock** — Process died, didn't hang; the hook is lock-safe.
- **E9 Malformed etc/mono/config** — Stock, well-formed, parsed before mono_jit_init with no errors.
- **E10 Caller config bug** — main.cpp:1191 passes a minimal correct config (exe_path, log, fna_platform).
- **E11 Thread suspension failure** — SuspendThread/GetThreadContext resolve (the exe loads); only 1 thread during init.
- **E12 SGen nursery/heap alloc failure** — VirtualAlloc RW is allowed; 5 GB free (boot probe).
- **E13 File-mmap failure for corlib** — UWP APIs correct (CreateFileMappingFromApp); failure prints + exits, not silent.
- **E14 SDB socket/bind failure** — Agent init is lazy; no socket at init.

## 7. Appendix: what was read

- Crash log: /tmp/log104e.txt (lines 100-106 = launch window; boot probes).
- Host: ~/tiramisu-celeste-test/src/trt/mono_host.cpp (718 lines, incl. opencode's SEH patch), src/host/main.cpp:1191 (caller).
- Mono UWP build: ~/tiramisu-woowop-engine-monouwp/third_party/mono-uwp/ (build-mono-uwp.sh, mono-sources.txt, 27 patches, asm/win64.S).
- Framework: ~/tiramisu-celeste-test/out-celeste-106/layout/ (mono/4.5/, etc/mono/config); mscorlib MD5-verified against the engine build's reference copy.
- Mono 6.8.0.105 source (patched extraction, ~/.cache/tiramisu/mono-uwp/src/mono-6.8.0.105/): mini_init, mono_init_internal, mono_jit_init_version, mono_arch_init, mini_get_breakpoint_trampoline, mono_arch_create_sdb_trampoline, mono_trampolines_init, mono_code_manager_reserve / new_codechunk / codechunk_valloc, mono_valloc, mono_mmap_win_prot_from_flags, mono_file_map_error, mono_shared_area, sgen_gc_init, mono_ee_interp_init, mini_jit_init, register_trampolines, mono_arch_finish_init, mono_jit_set_aot_mode / mono_jit_set_aot_only, mono_debugger_agent_init, mono-threads-windows.c.
