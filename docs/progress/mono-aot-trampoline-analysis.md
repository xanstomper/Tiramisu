# Mono AOT-only / trampoline death analysis — .122 vs .123, .124 verdict

Date: 2026-11-20 ~21:20 EST. Read-only offline analysis (no console touch, no builds).
Sources: Mono 6.8.0.105 source at `~/.cache/tiramisu/mono-uwp/src/mono-6.8.0.105/`
on mineplex; `libmonosgen-uwp.lib` built 18:14 (patches 28+29+30);
`~/tiramisu-brown-merge/src/trt/mono_host.cpp` (working tree, .124 arrangement);
`/tmp/t122.log`, `/tmp/t123.log` (4,311 / 4,529 lines);
`~/tiramisu-gow-skye/docs/progress/woowop-lane.md` (02:15 / 02:45 UTC entries);
`out-brown-124/layout/tiramisu.exe` (PE headers).

Correction to the task brief, up front: **the Mono build is x86_64 (amd64), not x86-32**
(`obj-libmini` contains `mini-amd64.obj` / `exceptions-amd64.obj`, no `mini-x86`;
build is `Release|x64`, `clang-cl -target x86_64-pc-windows-msvc`). All
`mini-x86.c` / `exceptions-x86.c` findings are therefore inapplicable; the
live files are `mini-amd64.c` / `exceptions-amd64.c`. (The game is 32-bit PE;
Mono itself runs 64-bit and interprets the managed code.)

## TL;DR — the four answers

1. **Top explanation for the .123 early death: there is no Mono code path by
   which `mono_aot_only=1` can kill `mini_init` before mscorlib loads.**
   Exhaustive audit (below, §a): the only pre-mscorlib effects of aot_only are
   one flag set (`mono_set_generic_sharing_vt_supported(TRUE)`, no side
   effects) and three *skipped* initializations (SDB trampoline, generic
   trampoline codes, AOT cache) — none can crash, abort, or hang. The death
   signatures independently exclude SEH on the guest thread (no SEH line; the
   `__except` guard would have caught it), `g_assert`/`g_error` (no
   `mono[…FATAL…]` line; the host-installed log adapter guarantees such a
   line would appear — §d), and managed exceptions (no managed code runs
   during init). The single .123 run (n=1) most plausibly died of something
   *other* than aot_only — an unhandled exception on a non-guest thread
   (the GameInput thread is the most active; a crash there kills the process
   silently with no Mono log and no SEH line) or an external/OS kill.
   **But pre-init aot_only is still refuted as a fix — for a different
   reason than the lane entry claims:** had .123 survived to mscorlib, it
   would have died moments later at `mono_exceptions_init` →
   `mono_aot_get_trampoline("restore_context")` →
   `get_mscorlib_aot_module()` → `g_assert(amodule)` abort, because the
   package contains **zero AOT modules** (no `*.dll.dll` anywhere, no
   statically registered AOT module). That death would have been VISIBLE
   (a `mono[?/error] FATAL` assertion line). So: the lane entry's
   conclusion ("pre-init aot_only kills mini_init") is right in outcome,
   wrong in mechanism and in timing — it kills init *after* mscorlib, not
   before it, and visibly, not silently.
2. **Is the post-init aot_only experiment (.124) sound? No — unsound as a
   fix, valuable as a diagnostic.** With no AOT modules at all, *every* lazy
   trampoline lookup during Main funnels into `get_mscorlib_aot_module()` →
   `g_assert(amodule)` → abort. The `.124` comment's claim ("any codegen
   need fails cleanly (visible error)") is true ONLY for
   `mono_jit_compile_method`'s error return (mini-runtime.c:2434); all
   `mono_aot_get_*` trampoline paths abort. Worse, `mono_aot_only` *disables*
   the interpreter's safe delegate path: `mono_create_delegate_trampoline`
   returns the inert `no_delegate_trampoline` stub only when
   `mono_use_interpreter && !mono_aot_only` (mini-trampolines.c:1530) — with
   aot_only set it falls through and tries to build a real delegate
   trampoline via AOT lookup → abort. And post-init aot_only does NOT set
   the code manager read-only (that happens only inside `mini_init`) and
   does NOT un-create the trampolines init already built. **Predicted .124
   outcome:** dies at the first delegate/pinvoke trampoline need in Main,
   *with* a visible `mono[?/error] FATAL * Assertion … condition 'amodule'
   not met` line — which is still useful: it identifies the exact first
   trampoline need by timing. Run it, read the line, then stop pursuing
   aot_only: aot_only without AOT images is not a mode, it's an abort
   waiting for the first trampoline.
3. **Single most likely .122 death mechanism: first EXECUTION of
   JIT-generated code in Main — the interp→native trampoline at the first
   P/Invoke** (`ves_pinvoke_method` → `get_interp_to_native_trampoline` →
   `mono_arch_get_interp_to_native_trampoline`, interp.c:1315/1336),
   almost certainly the `SteamAPI_Init` call immediately after
   Steamworks.NET loads. Init *proves* RWX allocation works (throw
   trampolines allocate fine via the patch-29 fallback — §b), so the kill
   is on execution, not allocation: the trampolines init creates are never
   executed during init, and the first pinvoke is the first dynamic-code
   execution in the process — killed by the Xbox dynamic-code policy
   (HVCI/CI), the platform's documented no-JIT stance. Runner-up:
   an AV inside the native pinvoke target (CSteamworks bridge) or inside
   JIT emission — indistinguishable from the log. CFG is excluded
   (not enabled in the binary — §b).
4. **Fix:** stop generating *and* executing JIT code on UWP entirely — see
   §b "Fix". aot_only cannot do this (it aborts instead). The correct fix
   is precompiled (static, image-backed) replacements for the two
   transitions the interpreter needs at runtime: interp→native (pinvoke)
   and native→interp (delegate ftnptr).

## (a) What `mini_init` does differently with `mono_aot_only=1`

`mono_jit_set_aot_only(1)` (driver.c:2816) sets two globals:
`mono_aot_only = TRUE` and `mono_ee_features.use_aot_trampolines = TRUE`.
`mini_init` is in `mono/mini/mini-runtime.c:4099` (not `mini.c`).

Pre-mscorlib aot_only effects in `mini_init`, exhaustively (every
`mono_aot_only` / `use_aot_trampolines` read in the tree was checked
against the init order; the build is amd64 so x86/arm/arm64 files are
excluded):

| Site | Effect with aot_only | Can it kill? |
|---|---|---|
| mini-runtime.c:4150 | `mono_set_generic_sharing_vt_supported(TRUE)` (x86_64 defines `MONO_ARCH_GSHAREDVT_SUPPORTED`, mini-amd64.h:477). The setter (mini-generic-sharing.c:4199) only flips a flag. | No |
| `mono_arch_init` (mini-amd64.c:1432) | Patch 28: unconditional `return` — bp trampoline skipped in BOTH modes. (Upstream x86 code also skips it under aot_only, mini-x86.c:740, but that file isn't compiled.) | No |
| `mono_trampolines_init` (mini-trampolines.c:1282) | Early return — `mono_trampoline_code[]` stays NULL, no trampoline codes created. Less work, not more. | No — nothing before mscorlib reads `mono_trampoline_code[]` (only reader is `mono_get_trampoline_code`, used by lazy trampoline creation post-init) |
| `mono_aot_init` → `aot_cache_init` (aot-runtime.c:1547) | Early return — AOT cache stays disabled. Less work. | No |
| `win32_seh_init` (exceptions-amd64.c:201, via mini-windows.c:194) | Skips creating the `restore_stack` trampoline. Less work. | No |
| `mono_ee_interp_init` (interp.c:6984) | No aot_only read at all. | No |
| `mono_init_internal` → corlib load (domain.c:510) | No `mono_aot_only` read anywhere in `mono/metadata/` or `mono/utils/`. | No |
| `load_aot_module` hook (aot-runtime.c:2060, installed by `mono_aot_init` since `MONO_OPT_AOT` is in `DEFAULT_OPTIMIZATIONS`, driver.c:132) | No AOT file found for mscorlib → the `g_error` at :2180 is gated on `!mono_use_interpreter`; the interpreter is on, so it returns quietly. Identical in both modes (`mono_aot_mode` is untouched by `mono_jit_set_aot_only`). | No |

Post-mscorlib aot_only effects in `mini_init` (for completeness — these
are what .123 *would* have hit):

- mini-runtime.c:4319: `mono_code_manager_set_read_only(domain->code_mp)`
  + `mono_marshal_use_aot_wrappers(TRUE)`.
- mini-runtime.c:4328: IMT trampoline builder becomes
  `mono_aot_get_imt_trampoline`.
- mini-runtime.c:4338 `mono_exceptions_init` (mini-exceptions.c:217):
  `use_aot_trampolines` is set, so the first statement calls
  `mono_aot_get_trampoline("restore_context")` →
  `mono_aot_get_trampoline_full` (aot-runtime.c:5429) →
  `get_mscorlib_aot_module()` (aot-runtime.c:5404) →
  `g_assert(amodule)`. mscorlib has no AOT module (no `mscorlib.dll.dll`
  in the package; no static registration), so this aborts. `mono_arch_finish_init`
  (mini-amd64.c:8469) runs just before it and is a no-op on Win32, so
  `mono_exceptions_init` is the first post-mscorlib killer.
- `mono_global_codeman_reserve`'s `g_error` (mini-runtime.c:376) and the
  code-manager `g_assert(!read_only)` (mono-codeman.c:522) are unreachable
  in practice: the AOT assert fires first.

Why the observed .123 death cannot be any in-init Mono failure:

- **Not SEH on the guest thread:** `mono_jit_init` runs under the host's
  `__try/__except` (mono_host.cpp:405-414), which logs code/address/stack.
  The .123 run shows no SEH line. (The SEH lines at t123.log:449-738 are
  from .108 — verified by the `0.2.2.108` mono-dirs line at ~640 — not
  .123.)
- **Not `g_assert`/`g_error`:** the host installs
  `mono_trace_set_log_handler(mono_log_to_log)` before init, which routes
  eglib `g_log` through `eglib_log_adapter` (mono-logger.c:462, :400) into
  tiramisu.log as `mono[…]` lines. A fired assert/error would leave a
  `mono[?/error] FATAL` line before exiting/looping. The .123 run has zero
  `mono[` lines (last one in the file is line 2368, an old run). See §d.
- **Not a hang:** the game runs on `g_guest_thread` (main.cpp:2907),
  separate from the webxfer listener thread (webxfer.cpp:1709). A hung
  guest thread would leave :9127 up. :9127 went dark ⇒ the process died.
- **Not lost log output:** `log_line` fflushes after every line
  (host_log.cpp:97-115), and `trt_trace` fflushes per call. The missing
  `mono image: mscorlib` line is real evidence the corlib load hook never
  fired — death genuinely precedes corlib load completing.
- The process lived at least one input-poll cycle after entering
  `mono_jit_init` (two GameInput lines follow the `calling` line), so the
  death is not instantaneous at init entry either.

Net: the .123 evidence (n=1, silent, pre-mscorlib, no SEH, no assert
line) has **no aot_only mechanism**. Most plausible: an unhandled
exception on a non-guest thread (GameInput polling is the most active
background work; its mouse callback already fails with
`hr=0x80004001`), or an external/OS kill. The lane entry's "pre-init
aot_only kills mini_init" is not established by this run; what *is*
established by the code is that pre-init aot_only kills init *just after*
mscorlib, visibly, at `mono_exceptions_init`. Testable prediction: a
re-run of .123 that gets past mscorlib dies with a `mono[?/error] FATAL`
`amodule` assertion line.

## (b) What generates trampolines during Main in non-aot-only mode (.122)

`.122` dies inside `mono_jit_exec` (Main), last line `mono image:
Steamworks.NET` (t122.log tail; same 7-image sequence 6 times —
deterministic). `mono_jit_init` returned `domain ok`; the unhandled
managed-exception hook was installed and never fired, so this is not a
managed exception. `mono_jit_exec` has no SEH guard, so a native fault
there leaves no log line (and `mono_veh` is defined but never registered,
mono_host.cpp:177).

Trampoline/code generation on the Main path (amd64, interpreter,
aot_only OFF):

1. **First P/Invoke → interp→native trampoline (PRIME SUSPECT).**
   `ves_pinvoke_method` (interp.c:1336) lazily builds `entry_func` on the
   first pinvoke. amd64 does not define `MONO_ARCH_HAS_NO_PROPER_MONOCTX`
   (arm64 only), so it takes `entry_func =
   get_interp_to_native_trampoline()` (interp.c:1315) →
   `mono_arch_get_interp_to_native_trampoline` — JIT codegen into the
   code manager (`mono_valloc` RWX, patch-29 fallback) — and then CALLS
   through it to the native function. First pinvoke in Celeste's Main is
   almost certainly `SteamAPI_Init`, immediately after Steamworks.NET
   loads. This is the first dynamic-code execution in the process.
2. **First delegate→function-pointer → interp_in wrapper + rgctx
   trampoline.** `Marshal.GetFunctionPointerForDelegate` →
   `mono_delegate_to_ftnptr_impl` (marshal.c:284) →
   `mono_marshal_get_managed_wrapper` → `mono_compile_method_checked` →
   `interp_create_method_pointer` (interp.c:2686). amd64 lacks
   `MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE` (ARM only), so it JIT-compiles
   the interp_in wrapper (`mono_jit_compile_method_jit_only`, interp.c:2727)
   and then `mono_create_ftnptr_arg_trampoline` →
   `mono_arch_get_static_rgctx_trampoline` (mini-trampolines.c:162-170).
   Both are JIT code, executed when native code invokes the callback
   (Steam callbacks are the likely first use). Note `mono_create_ftnptr`
   itself is a no-op on amd64 (mini-runtime.c:3782, PPC-only descriptors)
   — the trampolines come from the wrapper path, not ftnptr.
3. **First delegate invoke** uses the interpreter directly (no trampoline):
   `mono_create_delegate_trampoline` returns the inert
   `no_delegate_trampoline` stub when `mono_use_interpreter &&
   !mono_aot_only` (mini-trampolines.c:1530).
4. **Generic sharing / gsharedvt trampolines** (`mono_arch_get_gsharedvt_arg_trampoline`,
   mini-generic-sharing.c:2046; `mono_create_static_rgctx_trampoline`,
   mini-trampolines.c:127; unbox trampolines, :374; lazy-fetch, :1560)
   are created lazily on generic instantiation/call. They allocate via
   the same code manager. Possible but later than the first pinvoke.
5. **Exception trampolines** (throw/rethrow/restore-context) are created
   EAGERLY during `mono_exceptions_init` (mini-exceptions.c:217-241,
   exceptions-amd64.c:993-1010) via `mono_global_codeman_reserve` — but
   never *executed* during init. If Main's first action threw a caught
   exception, the throw trampoline would execute — a secondary candidate,
   but the pinvoke comes first in practice.

Why allocation failure is EXCLUDED: init already allocates RWX code
successfully. `mono_exceptions_init` creates five-plus throw trampolines
via `mono_global_codeman_reserve` → code manager → `mono_valloc` RWX
(mono-codeman.c:128). A NULL reserve would be dereferenced by the emitter
(patch 28's own comment: `mono_arch_create_sdb_trampoline` "would
dereference a NULL reserve" — the .104/.108 signature). .122 init
succeeds ⇒ the patch-29 fallback (RW alloc + `VirtualProtectFromApp` to
RWX) works. Corroborated by the boot probe: direct RWX `VirtualAlloc`
DENIED (err=87) but W→RX transition ALLOWED — the two-step fallback is
the path that works. `mono_valloc` returns NULL (never asserts) on
failure; `mono_valloc_aligned`'s `g_assert(aligned)` is SGEN/lock-free
only (§d).

Why the kill is on EXECUTION: init creates trampolines but executes none;
Main's first pinvoke is the first execution of dynamically allocated
code in the process. Xbox (Dev Mode UWP) does not permit JIT / dynamic
code execution — allocation succeeding while execution is blocked is
exactly the observed shape. CFG is excluded as the mechanism: the .124
binary's `DllCharacteristics` is `0x9160` (APPCONTAINER + NX +
DYNAMIC_BASE + HIGH_ENTROPY_VA), GUARD_CF bit clear,
`GuardCFFunctionTable = 0`. So the kill is the platform code-integrity /
HVCI dynamic-code policy (or, indistinguishably from the log, an AV
inside the trampoline jump or inside the native pinvoke target).

Runner-up that cannot be excluded offline: the death is inside
`SteamAPI_Init` itself (the CSteamworks bridge) rather than in the
trampoline — same observation point (first pinvoke). Discriminating test:
a minimal managed call that pinvokes a trivial no-op host function as
the very first pinvoke; death there ⇒ execution is blocked generally;
survival ⇒ the killer is Steam/FNA-specific.

### Fix for .122

aot_only cannot fix this (it aborts instead — §c). The fix is to make
the UWP interpreter never generate or execute JIT code:

1. **Pinvoke transition:** patch `get_interp_to_native_trampoline`
   (interp.c:1310) with a UWP-only path returning a *precompiled*,
   image-backed transition stub (new `.S` file in the host, same pattern
   as `third_party/mono-uwp/asm/win64.S`) instead of
   `mono_arch_get_interp_to_native_trampoline`. The stub must implement
   the `MonoPIFunc (addr, ccontext)` transition in static code.
2. **Delegate ftnptr transition:** patch `interp_create_method_pointer`
   (interp.c:2686) similarly — a precompiled interp_in entry + static
   rgctx-style stub — or, failing that, route delegate callbacks through
   a small fixed pool of precompiled host thunks. Harder than (1); do (1)
   first and measure (Steam callbacks may come later than the first
   pinvoke).
3. Do NOT pursue `DISABLE_JIT`: it replaces the trampoline generators
   with `g_assert_not_reached` stubs (visible abort, still a death).
4. Do NOT pursue AOT images as the near-term fix: full-AOT-compiling
   mscorlib + Celeste + XNA for this Mono version is a large project
   (and `MONO_AOT_MODE_INTERP` still needs the AOT'd mscorlib
   trampolines); the static-stub patch is smaller and directly targets
   the two transitions.
5. Until the patch lands, every game that pinvokes or passes a delegate
   to native code will die at the first such call. No host-side flag
   avoids it.

## (c) Would post-init aot_only (.124) work?

No. Setting `mono_aot_only=1` after `mono_jit_init` returns but before
`mono_jit_exec` (mono_host.cpp:450, already staged in the working tree)
avoids the init-time killers (exceptions init, SDB trampoline, code
manager read-only — all run with aot_only OFF), but every lazy
trampoline need during Main then takes an AOT branch that aborts,
because there are no AOT modules:

| Lazy creation point (all post-init) | aot_only branch | Result with no AOT module |
|---|---|---|
| `mono_aot_get_trampoline` (aot-runtime.c:5429) — used by exceptions init, `mono_arch_exceptions_init`, interp native/interp transitions, SDB trampolines, delegate invoke (mini-amd64.c:8415/8438) | `get_mscorlib_aot_module()` → `g_assert(amodule)` (aot-runtime.c:5404) | Abort |
| `get_interp_to_native_trampoline` (interp.c:1315) — first pinvoke | `mono_aot_get_trampoline("interp_to_native_trampoline")` | Abort |
| `interp_create_method_pointer` (interp.c:2754) — first ftnptr | `mono_aot_get_trampoline("native_to_interp_trampoline")` | Abort (reached because the wrapper JIT fails under aot_only) |
| `mono_create_delegate_trampoline` (mini-trampolines.c:1530) | Guard `mono_use_interpreter && !mono_aot_only` FAILS ⇒ builds a real delegate trampoline ⇒ AOT lookup | Abort (regression vs .122, which returns the inert stub) |
| `mono_create_static_rgctx_trampoline` (:127), ftnptr-arg (:162/167), unbox (:374), specific (:1328), lazy-fetch (:1560), gsharedvt (mini-generic-sharing.c:2046), IMT (via builder) | `mono_aot_get_*` family; several additionally `g_assert(tramp)` / `g_assert(code)` (aot-runtime.c:5796, :5882ff) | Abort |
| `mono_create_jit_trampoline` (:1412) | Tries cached compiled code, then `mono_create_specific_trampoline` ⇒ AOT branch | Abort |
| `mono_create_jump_trampoline` (:1347) | `mono_use_interpreter && !mono_aot_only` guard FAILS ⇒ skips the interp `create_method_pointer` path ⇒ JIT/AOT path | Abort / error |

`mono_trampolines_init`'s early return (mini-trampolines.c:1282) is NOT
the only creation point — trampolines are created lazily all through
Main, as the table shows. So post-init aot_only does not "prevent
Main-time trampoline creation"; it converts each creation into an abort.

Two further things post-init aot_only does NOT do:

- It does **not** set the code manager read-only: that happens only
  inside `mini_init` (mini-runtime.c:4319-4322), which already ran. Code
  allocation during Main is still permitted by the manager.
- It does **not** un-create the trampolines init already built (throw
  trampolines etc.), and paths that don't check the flag still generate
  code (e.g. `mono_jit_compile_method_jit_only` callers that ignore the
  aot_only error return).

The one thing that *does* fail cleanly is `mono_jit_compile_method`
(mini-runtime.c:2415/2434): with aot_only + interpreter it falls back to
`create_method_pointer`, and otherwise returns a clean
`MonoError` ("Attempting to JIT compile method … while running in
aot-only mode"). That is the only sense in which the .124 comment is
right — and it is not the path trampoline creation takes.

**Verdict:** .124 is unsound as a fix. Run it anyway as a diagnostic:
its death will carry a visible `mono[?/error] FATAL * Assertion …
condition 'amodule' not met` line (§d), and the *timing* of that line
(which managed call precedes it) identifies the first trampoline need
— i.e. what .122 died executing. After that, drop aot_only and do the
static-stub fix (§b).

## (d) Is `g_assert` active, and which asserts could fire?

**Yes — unconditionally active.** `g_assert` in `mono/eglib/glib.h:931-936`
has no `G_DISABLE_ASSERT` guard at all (that macro appears nowhere in
`mono/`); the only guard is `DISABLE_ASSERT_MESSAGES`, which merely
strips the condition text. `NDEBUG` (defined in build-mono-uwp.sh) is
irrelevant — it disables C `assert`, not glib `g_assert`. Verified: no
`G_DISABLE_ASSERT` / `DISABLE_ASSERT_MESSAGES` / `DISABLE_AOT` in
build-mono-uwp.sh, the crossbuild scripts, or `msvc/mono.sln`; the
assertion format string `* Assertion at %s:%d, condition '%s' not met`
is in the lib (249 `not met` strings).

What a fired assert does in this build (this is why the .122/.123 logs
prove none fired):

- `g_assertion_message` (mono/eglib/goutput.c:178) calls
  `g_logv_nofree(ERROR)` then `exit(0)`.
- Because the host installs `mono_trace_set_log_handler` before init,
  eglib `g_log` is routed through `eglib_log_adapter`
  (mono-logger.c:462, :400) to the host's `mono_log_to_log` — the
  message lands in tiramisu.log as a `mono[?/error] FATAL` line
  (fflushed per line). The adapter does NOT abort (the doc says the
  *callback* must abort; the host's callback just logs and returns), so
  `exit(0)` runs: clean silent process death, :9127 dark, no dump, no
  SEH — but WITH the assertion line in the log.
- `g_error` (glib.h:786) is `g_log(ERROR)` followed by `for(;;)` — with
  the host adapter installed it would log the FATAL line and then hang
  in a busy loop (mono thread spins, process alive, :9127 up).
- Neither a FATAL line nor a hang signature appears in the .122/.123
  Celeste runs (zero `mono[` lines after file line 2368, an old run),
  so **no `g_assert`/`g_error` fired in either run**. (The lane entry's
  "assert hypothesis weakened" conclusion is correct; its reasoning
  cited the empty `mono-stderr.log`, but the operative channel is the
  log adapter → tiramisu.log, not stderr. Note the eglib *default*
  handler writes to stdout, not stderr — the stderr redirect alone
  would never have caught an assert message.)
- `fatal = G_LOG_LEVEL_ERROR` (goutput.c:35) only matters for the
  default handler; with the host handler installed the default handler
  (and its `g_assert_abort`) is bypassed.

The patch-30 assert specifically: `g_assert(aligned)` in
`mono_valloc_aligned` (mono/utils/mono-mmap-windows.c:137) fires only
if BOTH the RWX and the RW fallback allocations fail. Callers are SGEN
(`sgen-memory-governor.c:420`) and the lock-free allocator
(`lock-free-alloc.c:143`), both with non-EXEC protection flags, so the
RWX fallback doesn't even apply and plain RW allocation would have to
fail (out of memory; the boot probe shows 5062/5120 MB free). **Not on
either death path.** (Patch 29's `mono_valloc` returns NULL on failure
rather than asserting; a NULL code reserve is dereferenced by the
emitters — the .104/.108 mechanism, now fixed by patch 28 + the
working fallback.)

Assert inventory for the .122/.123/.124 paths:

| Assert / error | Where | Reachable when | Verdict |
|---|---|---|---|
| `g_assert(amodule)` in `get_mscorlib_aot_module` | aot-runtime.c:5412 | Any `mono_aot_get_*` call with no AOT module — .123 post-mscorlib, .124 Main | THE aot_only killer; visible via FATAL line |
| `g_assert(tramp)` / `g_assert(code)` in AOT getters | aot-runtime.c:5796, :5882ff | Same paths, if the module lookup somehow returned | Backup abort on the same paths |
| `g_assert(!cman->read_only)` | mono-codeman.c:522 | .123 only, domain code alloc after `mini_init` sets read-only | Unreachable in practice (AOT assert fires first) |
| `g_error` in `mono_global_codeman_reserve` | mini-runtime.c:376 | Any global-codeman alloc with aot_only | Unreachable in practice (AOT assert fires first) |
| `g_assert(aligned)` | mono-mmap-windows.c:137 | RW alloc failure in SGEN/lock-free paths | Unreachable in practice |
| `g_assert(entry_func)` | interp.c:2778 | `interp_create_method_pointer` with both wrapper JIT and entry trampoline failed | Unreachable in .122 (RWX works) |
| `g_assert(mono_trampoline_code[…])` | mini-trampolines.c:1317 | `mono_get_trampoline_code` with pre-init aot_only (codes never created) | .123-only theoretical; AOT assert fires first |
| `mono_error_assert_ok` in `ves_pinvoke_method` | interp.c:1350 | LMF-wrapper JIT failure | amd64 takes the other branch (`MONO_ARCH_HAS_NO_PROPER_MONOCTX` is arm64-only); unreachable here |
| `g_assert_not_reached` in `no_delegate_trampoline` | mini-trampolines.c:1530 | Something *calling* the interp delegate stub | Interpreter never calls it; unreachable |

## Corrections to prior conclusions

- Lane entry 02:45 UTC ("ASSERT HYPOTHESIS WEAKENED … stderr redirect
  … so an assert message would land in mono-stderr.log"): conclusion
  right, mechanism wrong. Assert messages go through the
  `mono_trace_set_log_handler` adapter to tiramisu.log (and via the
  eglib default to stdout, never stderr). The absence of a `mono[…FATAL…]`
  line — not the empty stderr file — is the proof.
- Lane entry 02:15 UTC ("KILLED externally … consistent with an
  AppContainer code-integrity kill on executing a dynamically generated
  Mono trampoline"): the execution-kill half is the leading .122 theory
  (§b), but "killed externally" should not be conflated with the
  allocation failure the patches already fixed — init proves RWX
  allocation works; only *execution* is unproven.
- Task brief ("died … before the mscorlib image line" ⇒ pre-mscorlib
  aot_only kill): the *observation* is correct; the *attribution* to
  aot_only is unsupported by the code (§a). The pre-mscorlib silent
  death has no aot_only mechanism; the real aot_only init kill is
  post-mscorlib and visible.
- Task brief line numbers for `mini_init` (`mini.c`) and the x86 files:
  `mini_init` is in `mini-runtime.c`; the build is amd64, so the live
  trampoline/exception files are `mini-amd64.c` / `exceptions-amd64.c`.

## Recommended next steps (ordered)

1. Run .124 (already staged/built: `out-brown-124/` exists) and read the
   log. Expect a visible `mono[?/error] FATAL … 'amodule'` assertion at
   the first delegate/pinvoke trampoline need. Record *which managed
   call precedes it* — that is the .122 kill point.
2. Drop aot_only after that run. Do not re-run .123 expecting a
   different outcome; if .123 is re-run and gets past mscorlib, expect
   the same visible `amodule` assertion at `mono_exceptions_init`.
3. Discriminate the .122 execution-kill from a bridge crash with a
   minimal first-pinvoke test (trivial no-op host pinvoke as the first
   managed native call).
4. Implement the static-stub fix (§b): precompiled interp→native
   transition first (one patch site, `get_interp_to_native_trampoline`),
   then the native→interp (ftnptr) transition.
5. Consider adding a `mono_veh` registration (the handler exists at
   mono_host.cpp:177 but is never installed) and an SEH guard around
   `mono_jit_exec` (currently only `mono_jit_init` is guarded) so the
   next native fault in Main leaves an address/RVA instead of silence.
   Also consider making `mono_log_to_log` abort on `fatal=true`, as the
   Mono docs require of the log callback — today a `g_error` would hang
   in `for(;;)` instead of dying visibly.
