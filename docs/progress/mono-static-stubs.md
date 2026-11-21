# Mono static interp stubs — patches 31+32 + lib rebuild

Date: 2026-11-20 ~21:56 EST. Agent: mono-static-stubs (no console touch,
no deploys, no commits/pushes, no host build — the test agent owns .125).

Fix for the .122 Celeste console death (see
mono-aot-trampoline-analysis.md): the process died at the FIRST EXECUTION
of JIT-generated code in Main — the interp-to-native trampoline at the
first P/Invoke (almost certainly `SteamAPI_Init` right after
Steamworks.NET loads). RWX allocation works (patch-29 fallback); the
kill is on execution (Xbox HVCI/CI dynamic-code policy). The fix stops
the interpreter generating AND executing JIT code for its two runtime
transitions, replacing both with precompiled, image-backed static stubs.

## Build result

- `libmonosgen-uwp.lib` REBUILT OK (rc=0, ~3 min, log
  `mono-uwp-rebuild-31-32.log`): **11,666,328 bytes, Nov 20 21:53 EST**
  (was 11,659,836 @ 18:14; +6,492). Stamp
  `c08e5e59…-6.8.0.105`. The build's own symbol check passed
  (`mono_jit_init`, `mono_jit_exec`, +10 more). 295 archive members.
- Patches 31+32 applied cleanly in the real build (fresh extraction,
  sorted order, after 01–30). No patch failures, no compile errors.
- The rebuild was fast because the crossbuild wrapper reuses unchanged
  objects (only `interp.obj` + `win64.obj` changed content).

## What each patch does

Both patches touch only `mono/mini/interp/interp.c`, gated on
`G_HAVE_API_SUPPORT(HAVE_UWP_WINAPI_SUPPORT)` — the same idiom as
patches 04/23. The desktop/Linux path is untouched (the macro is 0
there; `config.h` includes `msvc/msvc-win32-support.h`, which defines
it 1 under the APP partition). The stubs themselves live in
`third_party/mono-uwp/asm/win64.S` (edited directly — patch files can
only touch the Mono tree, and win64.S is a host-tree file assembled
separately by `build-mono-uwp.sh` §3b).

### Patch 31 — interp-to-native static stub (pinvoke)

`31-uwp-interp-to-native-static-stub.patch` (1,341 bytes,
sha256 `350e920c…`). `get_interp_to_native_trampoline` (interp.c:1310)
returns `mono_interp_to_native_trampoline_static` (win64.S) on UWP
instead of `mono_arch_get_interp_to_native_trampoline` (JIT). This is
the .122 kill point: `ves_pinvoke_method` builds a `CallContext` in C
(`mono_arch_set_native_call_context_args`) and calls through this
trampoline, so the ONLY JIT part of a pinvoke was the trampoline
itself. One patch site, no C marshalling changes.

### Patch 32 — native-to-interp static stub (delegate ftnptr)

`32-uwp-native-to-interp-static-stub.patch` (5,463 bytes,
sha256 `ee55097c…`). `interp_create_method_pointer` (interp.c:2686) on
UWP avoids all three JIT steps in the ftnptr path:

1. Skips the interp_in wrapper JIT compile
   (`mono_jit_compile_method_jit_only (mini_get_interp_in_wrapper
   (sig))`) — leaving `entry_wrapper` NULL takes the existing
   `MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE` branch.
2. That branch uses `mono_native_to_interp_trampoline_static`
   (win64.S) instead of `mono_arch_get_native_to_interp_trampoline`
   (JIT).
3. Uses the direct `interp_entry_from_trampoline` C entry for EVERY
   signature, skipping the LMF-wrapper JIT compile for non-pinvoke
   sigs. The LMF wrapper exists for mixed JIT+interp mode (it lets EH
   resume into JITted code across native frames); UWP runs the
   interpreter only, there is no JITted code to resume into, and the
   desktop primary path (interp_in wrapper) runs these transitions
   without an LMF as well. See Caveats for the one behavioral note.
4. Replaces `mono_create_ftnptr_arg_trampoline` (JIT:
   `mov r10, <imm64 desc>; jmp addr`) with
   `mono_uwp_alloc_ftnptr_thunk`, which hands out the next thunk from
   a fixed pool of 256 static thunks in win64.S, stores the
   `MonoFtnDesc*` in its table slot, and returns the thunk address.
   Allocation is atomic (`mono_atomic_inc_i32`); the descriptor is
   never freed (same as the JIT path), so a slot stays valid for the
   life of the process. Pool exhaustion aborts loudly via `g_error`
   (visible FATAL line) — bump `MONO_UWP_FTNPTR_COUNT` in interp.c
   and the `.rept` count in win64.S together.

## Stub design (win64.S)

`win64.S` sha256 `34d51c39…` (10,332 bytes). Both stubs are mechanical
translations of the `tramp-amd64.c` emitters (TARGET_WIN32 branches),
so the C side (which builds/reads the `CallContext`) is unchanged.
The marshalling stays in C (`mono_arch_set/get_native_call_context_*`,
`interp_entry_from_trampoline`); the asm only shuffles registers/stack.

`CallContext` layout (mono/mini/mini-amd64.h): `gregs[17]` @ 0,
`fregs[16]` @ 136, `stack_size` @ 264, `stack` @ 272, sizeof 280.
`gregs[]` is indexed by the `AMD64_Reg_No` enum
(mono/arch/amd64/amd64-codegen.h): RAX=0, RCX=1, RDX=2, R8=8, R9=9,
R10=10 (`MONO_ARCH_RGCTX_REG`). `MonoFtnDesc` (mono/mini/mini.h):
`addr` @ 0, `arg` @ 8. Offsets were confirmed against the COMPILED C
code in the same lib (see Verification).

- `mono_interp_to_native_trampoline_static` (@ 0xc2, 145 bytes):
  `(callme, CallContext*)` in RCX/RDX. Saves both, allocates
  `ctx->stack_size` on the stack, copies the stack args 8 bytes at a
  time (same temps as the JIT emitter: RCX/RDX/R8/RAX — no
  callee-saved register touched), loads RCX/RDX/R8/R9 + XMM0–3 from
  the context, `call`s the target, writes RAX/XMM0 back into
  `ctx->gregs[RAX]` / `ctx->fregs[0]`. The dynamic `sub rsp, rax` is
  invisible to the unwinder (frame pointer restores rsp, as MSVC
  emits for alloca).
- `mono_native_to_interp_trampoline_static` (@ 0x153, 125 bytes):
  called by native code with the delegate signature; R10 holds the
  `MonoFtnDesc*` (set by the ftnptr thunk). Frame = 32 bytes home
  space + sizeof(CallContext)=280, aligned to 16 = 320; the context
  sits at rsp+32. Saves the arg regs + XMM0–3 into the context, sets
  `ctx.stack = rbp + 16` (caller's rsp at the call site), calls
  `ftndesc->addr(&ctx, ftndesc->arg)` (= `interp_entry_from_trampoline`,
  which runs the interpreted method and writes the return value back
  into the context), restores RAX/XMM0 from the context.
- `mono_ftnptr_thunks` (@ 0x1d0, 256 × 12 bytes) + `mono_ftnptr_table`
  (.data, 2048 bytes): each thunk is `mov r10, [rip + table + i*8]`
  (7 bytes) + `jmp mono_native_to_interp_trampoline_static`
  (5 bytes). The JIT equivalent bakes the descriptor pointer in as an
  immediate, which static code cannot do, so the pointer lives in the
  table (filled at runtime by the allocator).
- Unwind info: unlike the two original win64.S functions (leaves),
  the transition stubs call out, so they carry `.seh` unwind info
  (`.pdata`/`.xdata` — verified below). The thunks are pure jumps (no
  frame, no calls) and need none: the native caller's return address
  stays on top of the stack across the jump.

One assembler limitation: clang's integrated assembler rejects
`.if (. - symbol)` (not an absolute expression), so the 12-byte
thunk-size invariant has no assemble-time assert — it is verified by
disassembly instead (below).

## Verification

All checks run against the rebuilt lib
(`~/.cache/tiramisu/mono-uwp/out/libmonosgen-uwp.lib`).

(a) Static stubs in the lib:
- `llvm-nm --defined-only`: all four symbols defined —
  `mono_interp_to_native_trampoline_static` T @ 0xc2,
  `mono_native_to_interp_trampoline_static` T @ 0x153,
  `mono_ftnptr_thunks` T @ 0x1d0, `mono_ftnptr_table` D.
- `win64.obj` in the lib is byte-identical (sha256 `76b117a0…`) to
  the standalone test object that was disassembled and audited
  instruction-by-instruction: both trampolines match the JIT emitters
  exactly (all offsets, the copy loop, the call/return sequences);
  thunk 0/1/2/3/255 disassemble with the correct table displacements
  (0/8/16/24/2040) and all jump to 0x153; 256 ×
  `IMAGE_REL_AMD64_REL32` relocations against `mono_ftnptr_table`,
  spaced 12 bytes; `.text` size 0xdd0 = thunks end exactly at
  0x1d0+0xC00; `.data` size 0x800 = 256×8 table.
- `.pdata` has 2 `RUNTIME_FUNCTION` entries (0xc2–0x153, 0x153–0x1c5)
  and `.xdata` has 2 `UNWIND_INFO`s whose fields were decoded by
  hand: prologue sizes 8/11, 3/4 unwind codes (ALLOC_SMALL 16 /
  ALLOC_LARGE 320, SET_FPREG, PUSH_NONVOL RBP), FrameRegister RBP,
  FrameOffset 0 — all correct.
- `interp.obj` (the fresh compile) references all four static symbols
  as undefined (U) — proving the UWP branches compiled in
  (`HAVE_UWP_WINAPI_SUPPORT` evaluated to 1 in interp.c's compile).
  The pool allocator was inlined into `interp_create_method_pointer`
  (no separate symbol — expected at /O2): its string literal (`UWP
  ftnptr thunk pool exhausted…`), its `mono_uwp_ftnptr_next` BSS
  counter, and its table/thunk references are all present in
  interp.obj.

(b) Interp trampoline generators bypassed on UWP:
- `interp.obj` has NO reference to
  `mono_arch_get_interp_to_native_trampoline` or
  `mono_arch_get_native_to_interp_trampoline`. The ONLY referrer of
  either in the whole lib is `aot-compiler.obj` (aot-compiler.c:7606/
  7610 — the AOT compiler tool code, linked into libmini but never
  invoked by the UWP embedding; dead code on this path, present in
  the pre-patch lib too).
- NO object in the lib references `mono_create_ftnptr_arg_trampoline`
  at all — the ftnptr JIT path is fully unreachable from the interp.
- `CallContext` offsets cross-checked against the compiled C in the
  same lib: `mono_arch_set_native_call_context_args` accesses
  `0x108`/`0x110` (stack_size/stack) off the context pointer;
  `mono_arch_get_native_call_context_ret` computes `&fregs[0]` as
  `ctx + 0x88` (136). Combined with the struct definition
  (`gregs[17]` first), the asm offsets are fully pinned.

(c) Patches 28/29/30 still in (no regression of the mono_jit_init fix):
- Build log shows all three applied (fresh extraction, before 31/32).
- Patch 28: `mono_arch_init` disassembles to a single `retq` (the
  unconditional early return). (`mini_get_breakpoint_trampoline` is
  still referenced by OTHER functions in mini-amd64.obj — not by
  `mono_arch_init`.)
- Patches 29/30: `mono-mmap-windows.obj` has 3 relocations against
  `__imp_VirtualProtectFromApp`; the pristine tarball source has ZERO
  references to `VirtualProtectFromApp` — the references come solely
  from the patches. (The only other referrer in the lib is
  `coree.obj`, from patch 25's area, unrelated.)

(d) Lib validity: file exists, mtime Nov 20 21:53 EST (rebuild ran
21:50–21:53), 11,666,328 bytes, valid archive (295 members via
`llvm-ar t`), and the build script's own symbol check passed. A host
link check is the test agent's job (.125 build).

## Caveats / limitations

- LMF: patch 32 uses the direct `interp_entry_from_trampoline` entry
  for non-pinvoke delegate signatures too, skipping the JIT LMF
  wrapper. A managed exception thrown from a native-invoked delegate
  callback cannot propagate across the native frames to a managed
  pinvoke caller (same as the desktop interpreter's primary path);
  the unhandled-exception hook still fires. In practice Celeste/
  Steamworks callbacks do not throw; if a callback ever throws, the
  process dies via the unhandled hook instead of propagating — visible,
  not silent.
- Ftnptr pool: 256 distinct delegate methods can be passed to native
  code per process (one slot per method; cached per method in
  `imethod->jit_entry`). Steamworks.NET registers a handful of
  callbacks; exhaustion is unlikely, and if it happens it aborts
  loudly with a FATAL line naming the fix.
- Scope: the stubs cover the two transitions the interpreter needs at
  runtime. Other JIT generators still exist in the lib
  (`do_jit_call` gsharedvt wrappers, generic-sharing trampolines,
  `mono_jit_compile_method` itself) but are unreachable in
  interpreter-only mode — if some future game path reaches one, it
  dies the same CI death at a different point. The `.125` console
  test is the proof: if Celeste gets past the first pinvoke (SteamAPI
  init) and the first delegate registration, the transitions work.
- `.124`'s aot_only experiment must NOT be in `.125` (tree is at the
  .122 baseline, aot_only reverted — confirmed by the lane note).
  These patches do not need aot_only and do not interact with it.
- The `.if` thunk-size assert could not be kept (clang IAS
  limitation, see above); the 12-byte invariant is verified by
  disassembly in this report instead. If win64.S is ever edited,
  re-run the disassembly check (thunk block must be exactly 3072
  bytes, `.text` size 0xdd0 for the current file).

## Files changed (mineplex)

- `~/tiramisu-brown-merge/third_party/mono-uwp/patches/31-uwp-interp-to-native-static-stub.patch` (new)
- `~/tiramisu-brown-merge/third_party/mono-uwp/patches/32-uwp-native-to-interp-static-stub.patch` (new)
- `~/tiramisu-brown-merge/third_party/mono-uwp/asm/win64.S` (extended: +2 stubs + thunk pool + table)
- `~/tiramisu-brown-merge/third_party/mono-uwp/patches/README.md` (entries for 31/32 appended)
- `~/.cache/tiramisu/mono-uwp/out/libmonosgen-uwp.lib` (rebuilt 21:53 EST)
- Build log: `~/tiramisu-gow-skye/docs/progress/mono-uwp-rebuild-31-32.log` (rc=0)
- Cleanup: removed `out/.parts/` (12M) + `out/.mono-uwp-symbols.txt`
  (1.2M) + my /tmp scratch. Kept the lib, the stamp, the source
  extraction, the tarball, and the `obj-*` compile cache (~10M —
  removing it would slow the next Mono rebuild ~10x for no real disk
  gain; 2.5G free).

## Handoff to the test agent (.125)

No host source changes are needed for the stubs. Build .125 normally
(`build-host.sh` calls `build-mono-uwp.sh`, which sees the stamp match
and reuses this lib — do NOT force a Mono rebuild). Deploy + launch
Celeste. Expected: the process survives the first pinvoke
(SteamAPI_Init, right after the Steamworks.NET image line — the .122
kill point) and the first delegate-to-native registration. If it dies
there again, the death is NOT the interp trampolines anymore — read
the trace for the new first failure.
