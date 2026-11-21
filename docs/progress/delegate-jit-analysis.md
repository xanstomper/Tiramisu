# GetFunctionPointerForDelegate on UWP — delegate JIT analysis

Offline analysis, 2026-11-21 ~01:30 EST. No console touch, no builds, no clones.
Question: after the CWD fix, will GetFunctionPointerForDelegate (Steamworks
callback registration, marshal.c:331) break because it needs JIT codegen?

## Bottom line

**GetFunctionPointerForDelegate will NOT break, and it does NOT need JIT codegen.**
The interpreter intercepts the compile request before the JIT is ever involved,
and the UWP patch set (patch 32) already provides a complete static mechanism
for native-to-managed delegate calls: a fixed pool of 256 precompiled entry
thunks in win64.S plus a precompiled native-to-interp trampoline. The
delegate-to-function-pointer conversion is JIT-free end to end. The mechanism
is built and linked into the current lib (symbols verified present in
libmonosgen-uwp.lib, rebuilt Nov 21 01:29). It has NOT yet been exercised on
the console — no native code has invoked a delegate callback yet — so the
first Steamworks callback dispatch is the live test.

The REAL next blocker in the callback path is NOT the delegate mechanism —
it is the bridge's `RunCallbacks`, which never dispatches (`.run` is always
nullptr). Even with a working function pointer, Steamworks callbacks will not
fire until the bridge reads the Run function from the CCallbackBase vtable.

## The call chain (all verified in source)

1. Managed code needs a native function pointer for a delegate. Two triggers,
   same convergence:
   - Explicit: `Marshal.GetFunctionPointerForDelegate(d)` -> icall
     `ves_icall_..._GetFunctionPointerForDelegateInternal` (marshal.c:5623)
     -> `mono_delegate_to_ftnptr_impl` (marshal.c:284).
   - Implicit: a delegate marked `[UnmanagedFunctionPointer]` passed as a
     P/Invoke argument -> the pinvoke wrapper's marshal IL calls the
     `mono_delegate_to_ftnptr` icall (registered marshal.c:218) -> the same
     `mono_delegate_to_ftnptr_impl`.
   - Steamworks.NET uses the IMPLICIT path: monodis MemberRef on
     `~/celeste-xbox/Celeste/Steamworks.NET.dll` (489 refs) shows NO
     `GetFunctionPointerForDelegate`, but DOES show
     `UnmanagedFunctionPointerAttribute..ctor` plus
     `Marshal.WriteIntPtr` / `AllocHGlobal` / `StructureToPtr` (it builds
     the CCallbackBase + vtable in unmanaged memory and writes the
     marshaled delegate pointer into it).
2. `mono_delegate_to_ftnptr_impl` (marshal.c:284-345): caches per-delegate
   (`delegate_trampoline` field); resolves virtuals; P/Invoke methods go
   direct; otherwise builds a managed wrapper via
   `mono_marshal_get_managed_wrapper` (marshal.c:3827) and compiles it via
   `mono_compile_method_checked` (marshal.c:331).
3. `mono_marshal_get_managed_wrapper` creates the wrapper with wrapper_type
   **MONO_WRAPPER_NATIVE_TO_MANAGED** (marshal.c:3869) — NOT
   MANAGED_TO_NATIVE. This matters (see step 5).
4. `mono_compile_method_checked` (object.c:788) -> `callbacks.compile_method`
   = `mono_jit_compile_method` (mini-runtime.c:4180, jit_only=FALSE) ->
   `mono_jit_compile_method_with_opt` (mini-runtime.c:2276):
   ```c
   if (mono_ee_features.force_use_interpreter && !jit_only)
       use_interp = TRUE;
   ...
   if (use_interp) {
       code = mini_get_interp_callbacks ()->create_method_pointer (method, TRUE, error);
       if (code)
           return code;
   }
   ```
   The host passes `--interpreter` (mono_host.cpp:446-448), which sets
   `force_use_interpreter` (driver.c:2863/2876). So the interpreter's
   `create_method_pointer` is tried FIRST. Only if it returns NULL does
   execution fall through to JIT compilation.
5. `interp_create_method_pointer` (interp.c:2751):
   - The early return `if (method->wrapper_type ==
     MONO_WRAPPER_MANAGED_TO_NATIVE) return imethod;` does NOT apply (the
     delegate wrapper is NATIVE_TO_MANAGED, step 3).
   - AMD64 defines MONO_ARCH_HAVE_INTERP_NATIVE_TO_MANAGED and
     MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE (mini-amd64.h:470-471);
     MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE is NOT defined for AMD64, so the
     interp_in-wrapper branch is taken — but on UWP the JIT compile of that
     wrapper is skipped by patch 32 (`#if
     !G_HAVE_API_SUPPORT(HAVE_UWP_WINAPI_SUPPORT)` guards the
     `mono_jit_compile_method_jit_only` call; entry_wrapper stays NULL).
   - With entry_wrapper NULL, the MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
     branch runs: on UWP, `mono_native_to_interp_trampoline =
     mono_native_to_interp_trampoline_static` (the precompiled stub) and
     `entry_func = interp_entry_from_trampoline` (direct entry — no LMF
     wrapper, no JIT).
   - It builds a MonoFtnDesc (addr=entry_func, arg=imethod) and returns a
     thunk: on UWP, `addr = mono_uwp_alloc_ftnptr_thunk (ftndesc)`
     (interp.c:2884); otherwise `mono_create_ftnptr_arg_trampoline` (the
     JIT path).
6. `mono_uwp_alloc_ftnptr_thunk` (interp.c:1092-1103): hands out the next
   slot from a fixed pool of 256 thunks in win64.S (`.rept 256`, each
   exactly 12 bytes: 7-byte RIP-relative `mov r10, [rip + table + i*8]` +
   5-byte `jmp mono_native_to_interp_trampoline_static`). Stores the
   ftndesc in `mono_ftnptr_table[slot]`. Pool exhaustion -> `g_error`
   (loud abort, not silent). Slots are never freed.
7. At invocation: native code calls the thunk -> thunk loads the ftndesc
   into r10 (MONO_ARCH_RGCTX_REG) -> jumps to the static trampoline ->
   transitions into `interp_entry_from_trampoline` -> the wrapper's IL
   runs in the interpreter -> the wrapper marshals args and invokes the
   delegate's target method (also interpreted). Zero JIT anywhere.

## Why the JIT question is moot

- The interpreter intercepts FIRST (step 4). The JIT is only reached if
  `create_method_pointer` returns NULL — which happens only if
  `mono_interp_transform_method` fails on the wrapper's IL (a generated
  marshal wrapper; transform failure is not expected).
- `mono_aot_only` is NOT set (mono_host.cpp:468,543 — reverted after
  .123/.124 proved it non-viable without AOT images), so there is no
  AOT-only gate in the way either.
- The JIT fallback, if ever reached, is dead on UWP anyway: patch 32's
  comment states it explicitly — "The UWP AppContainer forbids executing
  dynamically generated code, so the JIT trampoline (and the JIT ftnptr arg
  trampoline below) would be killed on first execution (first delegate
  passed to native code)." Patch 29's RWX fallback proves ALLOCATION works
  (code memory can be reserved), but EXECUTION of dynamically generated code
  is what the AppContainer forbids. So a JIT-compiled wrapper would
  allocate fine and die the first time it is called. This is a
  fallback-of-last-resort scenario, not the expected path.

## Verification status (proven vs. untested)

- PROVEN (linked in): win64.S is assembled by build-mono-uwp.sh (line 215)
  and merged into libmonosgen-uwp.lib via llvm-lib (line 219); the current
  lib (11,669,174 B, Nov 21 01:29) contains the stub symbols (8 strings
  matches). The 256-thunk pool (`.rept 256`) matches
  MONO_UWP_FTNPTR_COUNT 256; each thunk is 12 bytes per the win64.S header
  (verified by disassembly after the build per the README).
- PROVEN (opposite direction): the interp-to-native static stub (patch 31)
  works on the console — .129's SteamAPI_Init SUCCEEDS through it, and FNA
  native calls execute.
- UNTESTED (this direction): no native code has yet CALLED a delegate
  function pointer on the console. The native-to-interp stub (patch 32)
  has never fired. The first Steamworks callback dispatch is the live test.
- Doc gap: the `mono-static-stubs.md` file referenced by the win64.S /
  interp.c comments does not exist in the tree (only
  third_party/mono-uwp/README.md). The disassembly verification is asserted
  in comments but the verification record could not be located. Minor
  documentation gap, not a code gap.

## The REAL next blockers (ranked)

1. **Bridge dispatch gap (the actual blocker):**
   `trt_SteamAPI_RunCallbacks` (bridge_steam.cpp:103-110) iterates
   `g_callbacks` but only calls `g_callbacks[i].run` if non-null — and
   `RegisterCallback` (line 113-120) always sets `.run = nullptr` ("the
   game fills the vtable"). Nothing ever sets `.run`. So callbacks are
   registered but NEVER dispatched. Steamworks.NET's CCallbackBase vtable
   (built in unmanaged memory, Run slot = the static thunk) is never read.
   FIX (host-only, no Mono rebuild): in RunCallbacks, read the vtable from
   the stored CCallbackBase pointer (`*(void**)callback` -> vtable, slot 0
   = Run) and call `Run(callback, param)`. Small, self-contained. NOTE:
   this changes behavior — callbacks will start firing, which is what
   Celeste expects (achievement/stat callbacks), but it also means the
   native-to-interp stub gets exercised for the first time. Test it.
   - Does Celeste NEED callbacks to fire? Not answered by this analysis —
     Celeste sets achievements via SetAchievement + StoreStats
     (synchronous, no callback needed). RequestCurrentStats fires
     UserStatsReceived asynchronously; whether Celeste blocks on it is a
     game-code question. If Celeste does not wait, the dispatch gap is
     benign (callbacks silently never fire). If it waits, the gap is a
     hang. The .132 build script checks
     ISteamUserStats_RequestCurrentStats exports, so the stats path is
     being exercised — watch for a wait-on-callback hang after the CWD fix.
2. **First live firing of the native-to-interp stub (untested direction):**
   the first callback dispatch exercises patch 32 end to end. If the
   thunk/trampoline has a bug (e.g. a calling-convention mismatch in the
   CallContext transition, or the 12-byte thunk size assumption is wrong
   for one slot), it shows up as a crash on first callback — not at
   registration. Watch the log for a crash immediately after the first
   RunCallbacks with a registered callback.
3. **Thunk pool exhaustion (unlikely):** 256 slots, one per distinct
   delegate-to-ftnptr conversion, never freed. Steamworks.NET registers a
   handful of callbacks (UserStatsReceived, UserAchievementStored, etc.);
   FNA/Celeste add a few more. 256 is very likely sufficient. If
   exhausted, `g_error` aborts with a clear message ("UWP ftnptr thunk
   pool exhausted... increase MONO_UWP_FTNPTR_COUNT"). FIX: bump
   MONO_UWP_FTNPTR_COUNT in interp.c + the .rept count in win64.S
   together, rebuild the Mono lib.
4. **Exception caveat:** per the patch 32 comment, a managed exception
   thrown from a native-invoked delegate callback cannot propagate across
   native frames to a managed pinvoke caller (same as the desktop primary
   path); the unhandled-exception hook fires instead — and an unhandled
   exception aborts (SIGABRT, as seen in .127). Steamworks.NET dispatch
   methods are simple event invocations; a throw there would most likely
   come from game code. Keep callbacks exception-free.

## Fixes ranked (for the actual blockers)

1. Bridge RunCallbacks vtable dispatch (host-only, small) — the one fix
   that unblocks callbacks firing. Do this when the CWD fix lands and
   callbacks start registering.
2. Nothing needed for GetFunctionPointerForDelegate itself — no Mono
   change, no AOT work, no static callback table. The mechanism exists
   and is wired.
3. Thunk pool bump (only if the pool exhausts — the loud abort tells you).
4. AOT delegate trampolines: NOT recommended — the wrong tool. The
   wrappers are runtime-generated (mono_marshal_get_managed_wrapper builds
   them per delegate), and the static stubs already solve the
   native-to-managed transition without AOT. AOT would add build
   complexity for zero benefit here.

## What to watch in the .130-.138 tests

- First callback registration: does the log show SteamAPI_RegisterCallback
  with a valid callback pointer? (Registration succeeding proves the
  delegate-to-ftnptr conversion worked — the thunk was allocated.)
- First RunCallbacks with a registered callback: does anything fire? If
  the game proceeds without callbacks, the dispatch gap is benign for
  Celeste. If the game stalls waiting for a callback (e.g. after
  RequestCurrentStats), it is the dispatch gap — implement fix #1.
- A crash on first callback dispatch: suspect the native-to-interp stub
  (patch 32) — check the fault address against the thunk pool range and
  the trampoline.
- "UWP ftnptr thunk pool exhausted" in the log: bump the pool (fix #3).
- Do NOT reintroduce mono_aot_only as a "fix" for any delegate issue —
  .123/.124 proved it kills Mono without AOT images.

## Sources read

- ~/.cache/tiramisu/mono-uwp/src/mono-6.8.0.105/mono/metadata/marshal.c
  (mono_delegate_to_ftnptr_impl :284, mono_marshal_get_managed_wrapper
  :3827, mono_marshal_get_native_wrapper :3452, icall :5623)
- .../mono/metadata/object.c (mono_compile_method_checked :788)
- .../mono/mini/mini-runtime.c (mono_jit_compile_method_with_opt :2276,
  mono_jit_compile_method :2490, callbacks.compile_method :4180,
  mono_jit_runtime_invoke :2988)
- .../mono/mini/interp/interp.c (mono_uwp_alloc_ftnptr_thunk :1092,
  interp_create_method_pointer :2751, llvmonly variant :2669)
- .../mono/mini/mini-amd64.h (MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE /
  INTERP_NATIVE_TO_MANAGED :470-471)
- .../mono/mini/driver.c (force_use_interpreter :2863/2876)
- ~/tiramisu-brown-merge/src/trt/mono_host.cpp (--interpreter :446-448,
  aot_only NOT set :468/543)
- ~/tiramisu-brown-merge/src/trt/bridge_steam.cpp (RunCallbacks :103,
  RegisterCallback :113, RegisterCallResult :154, callback_entry :40)
- ~/tiramisu-brown-merge/third_party/mono-uwp/asm/win64.S (trampoline
  :192, thunks :243-249, table :261)
- ~/tiramisu-brown-merge/third_party/mono-uwp/build-mono-uwp.sh
  (win64.S assembly :215, lib merge :219)
- ~/tiramisu-brown-merge/third_party/mono-uwp/patches/32-uwp-native-to-interp-static-stub.patch
- ~/celeste-xbox/Celeste/Steamworks.NET.dll (monodis MemberRef — no
  GetFunctionPointerForDelegate; UnmanagedFunctionPointerAttribute +
  Marshal.WriteIntPtr/AllocHGlobal/StructureToPtr present)
