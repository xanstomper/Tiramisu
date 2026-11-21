# Stub re-audit + death between Steamworks.NET load and first pinvoke

Date: 2026-11-20 ~22:30 EST. Agent: stub-reaudit (read-only offline analysis —
no console touch, no builds, no clones, no deploys).

Context: Tiramisu .125 (Mono patches 31+32, static interp stubs, lib rebuilt
21:53 EST) dies at the SAME point as .122: after `mono image: Steamworks.NET`,
before the first pinvoke. No steam-door lines, no SEH, no FATAL, no watchdog,
no PROBE, :9127 dark. The .125 exe is 3,072 B larger than .122 (= 256 x 12-byte
thunk table — the new lib IS linked). The mono-aot-trampoline-analysis.md
finding 3 (interp-to-native trampoline execution = the kill) is refuted by
the .125 test.

Sources read: `mono-aot-trampoline-analysis.md`, `mono-static-stubs.md`,
patches 31+32, `asm/win64.S`, the patched Mono 6.8.0.105 tree at
`~/.cache/tiramisu/mono-uwp/src/mono-6.8.0.105/`, `mono_host.cpp`,
`mono_steam_door.cpp`, `steamworks.implmap`, `host-bridges.def`, the raw
console logs `/tmp/t125.log` + `/tmp/t122.log`, and the .125 exe's export
table (`out-brown-125/layout/tiramisu.exe`).

## Verdict

1. **Stub bug found? NO.** The static stubs (win64.S + patches 31/32) are
   correct. Independently re-verified by disassembling `win64.obj` extracted
   from the rebuilt lib (see §a). No wrong offset, no misalignment, no missing
   save/restore, no thunk-math error, no .seh error.
2. **Top 2 death candidates** (see §b): **(1)** first-finalization JIT compile
   (`gc.c:296-306` → `mono_compile_method_checked` → RWX denied →
   `mono_error_assert_ok` → `abort()`, silent); **(2)** a `g_assert` /
   `mono_error_assert_ok` abort inside the transform / class-init /
   wrapper-create path for the first Steamworks touch (exact site unknown —
   the diagnostic will print it).
3. **Minimal next diagnostic: host-side only, no Mono rebuild** (see §c): one
   .126 test build with `MONO_GC_PARAMS=nursery-size=64m` (the decisive
   discriminator for candidate 1) + stderr redirected to the log (prints the
   exact assert file:line for candidate 2) + the VEH installed + a SIGABRT
   handler + `MONO_LOG_LEVEL/MASK`. Expected outcome per candidate in §c.

Two further real bugs were found along the way (NOT the silent death, but the
next failures after it is fixed): **(i)** the first pinvoke's native entry
`Init` is not exported by the exe (39 door functions defined but unexported,
184 Steamworks imports unexported in total — §b.7); **(ii)**
`Marshal.GetFunctionPointerForDelegate` bypasses patch 32 entirely and
JIT-compiles (marshal.c:331 — §b.6).

---

## (a) Stub re-audit: NO BUG

Method: (1) read win64.S + patches 31/32 against the upstream emitters in
`mono/mini/tramp-amd64.c`; (2) cross-checked every offset against
`CallContext` (mini-amd64.h), `AMD64_Reg_No` (amd64-codegen.h),
`MonoFtnDesc` (mini.h), and `mono_arch_set_native_call_context_args` /
`get_call_info` (mini-amd64.c); (3) INDEPENDENTLY disassembled `win64.obj`
extracted from the rebuilt lib (`~/.cache/tiramisu/mono-uwp/out/
libmonosgen-uwp.lib`) — the disassembly below is my own run, not the
specialist's.

### (a.1) interp→native stub — correct, instruction for instruction

Disassembly of `mono_interp_to_native_trampoline_static` @ 0xc2 (145 bytes):

| Addr | Instruction | Check |
|------|-------------|-------|
| c2 | `push rbp` | prologue ✓ |
| c3 | `mov rbp, rsp` | frame pointer ✓ |
| c6 | `sub rsp, 0x10` | 16-byte save area ✓ |
| ca | `mov [rbp-8], rdx` | CallContext* save ✓ (matches emitter `off_methodargs = -8`) |
| ce | `mov [rbp-16], rcx` | target save ✓ (`off_targetaddr = -16`) |
| d2 | `mov r11, rdx` | ctx temp (volatile, same as emitter) ✓ |
| d5 | `mov rax, [r11+0x108]` | `stack_size` @ 264 ✓ |
| dc | `sub rsp, rax` | stack-args space ✓ |
| df–100 | copy loop (`mov r8,[rdx]; mov [rcx],r8; +8/+8; sub rax,8; test/jz`) | identical to emitter; temps RCX/RDX/R8 only ✓ |
| 102–10e | `mov rcx,[r11+8]; mov rdx,[r11+16]; mov r8,[r11+0x40]; mov r9,[r11+0x48]` | gregs[RCX=1, RDX=2, R8=8, R9=9] ✓ |
| 112–12d | `movsd xmm0–3, [r11+0x88..0xa0]` | fregs[0..3] @ 136..160 ✓ |
| 136 | `mov r11, [rbp-16]` | target reload ✓ |
| 13a | `call r11` | ✓ |
| 13d–144 | `mov r11,[rbp-8]; mov [r11],rax; movsd [r11+0x88],xmm0` | gregs[RAX=0] + fregs[0] return ✓ |
| 14d–152 | `lea rsp,[rbp]; pop rbp; ret` | frame-pointer restore (alloca pattern) ✓ |

Checked specifically, all clear:

- **CallContext offsets**: gregs[17] @ 0, fregs[16] @ 136, stack_size @ 264
  (0x108), stack @ 272 (0x110), sizeof 280 — mini-amd64.h CallContext.
- **Register numbers**: RAX=0, RCX=1, RDX=2, R8=8, R9=9
  (amd64-codegen.h:38-48); Win32 param_regs = {RCX, RDX, R8, R9},
  return_regs = {RAX}, float 4/1 (mini-amd64.h:228-239, TARGET_WIN32
  branch). The stub hardcodes exactly the Win32 set — correct for this build.
- **Stack alignment at `call`**: entry RSP%16==8 → push rbp → 0 → sub 16 →
  0 → sub stack_size. `stack_size = ALIGN_TO(stack_usage, 16)`
  (mini-amd64.c:1195, MONO_ARCH_FRAME_ALIGNMENT=16, mini-amd64.h:180), so
  RSP%16==0 at `call`. ✓ No misalignment.
- **32-byte shadow space**: `get_call_info` on TARGET_WIN32 starts
  `stack_size = 4 * sizeof(target_mgreg_t)` = 32 ("Reserve space where the
  callee can save the argument registers", mini-amd64.c ~880), so
  stack_size ≥ 32 always and the callee's home-register spills land in the
  copied stack buffer, never on `[rbp-16]`/`[rbp-8]`. ✓ (This was the one
  real hazard I chased; the C side guarantees it.)
- **Copy-loop overrun**: stack_size is a multiple of 16, so the 8-byte-at-a-
  time loop (`test; jz; copy; sub 8`) can never overrun or wrap. ✓
- **Callee-saved registers**: only caller-saved temps touched (RCX/RDX/R8/
  RAX/R11); RBX/RSI/RDI/R12–R15/XMM6–15 untouched; RBP saved/restored. ✓
- **.seh unwind info** (.xdata decoded by hand from the object):
  `01 08 03 05` = v1, prolog 8, 3 codes, FrameReg RBP, FrameOffset 0;
  codes `08 12` (ALLOC_SMALL 16 @8), `04 03` (SET_FPREG @4), `01 50`
  (PUSH_NONVOL RBP @1). All correct; the dynamic `sub rsp, rax` is after
  `.seh_endprologue` and invisible to the unwinder, which restores RSP from
  RBP — exactly the MSVC alloca pattern. .pdata entry 0xc2–0x153 ✓.

### (a.2) native→interp stub — correct

Disassembly of `mono_native_to_interp_trampoline_static` @ 0x153 (125 bytes):
push rbp; mov rbp,rsp; `sub rsp, 0x140` (320 = 32 home + 280 ctx, aligned
to 16 — matches the emitter's `framesize` computation, tramp-amd64.c:1055);
saves RCX/RDX/R8/R9 to `[rsp+32+{8,16,64,72}]` and XMM0–3 to
`[rsp+32+{136,144,152,160}]`; `ctx.stack = rbp+16` (`lea r11,[rbp+16]`,
stored at `[rsp+0x130]` = ctx+272 — matches the emitter); `rcx = rsp+32`
(ctx), `rdx = [r10+8]` (ftndesc->arg), `r11 = [r10]` (ftndesc->addr),
`call r11`; restores RAX from `[rsp+0x20]` (ctx+0) and XMM0 from
`[rsp+0xa8]` (ctx+136); `lea rsp,[rbp]; pop rbp; ret`. ✓
Alignment at `call`: entry 8 → push → 0 → sub 320 → 0 ✓; the 32-byte home
space for `interp_entry_from_trampoline` is `[rsp+0..32)` ✓.
.xdata: `01 0b 04 05` (prolog 11, 4 codes, RBP/0); `0b 01 28 00`
(ALLOC_LARGE 320 @11); `04 03` (SET_FPREG @4); `01 50` (PUSH_NONVOL RBP @1).
✓ .pdata entry 0x153–0x1c5 ✓. R10 = MONO_ARCH_RGCTX_REG
(MONO_ARCH_IMT_REG = AMD64_R10, mini-amd64.h:423/431) ✓. MonoFtnDesc
addr @ 0, arg @ 8 (mini.h) ✓.

### (a.3) Ftnptr thunks — correct

`mono_ftnptr_thunks` @ 0x1d0, 256 × 12 bytes (`mov r10,[rip+disp32]` 7B +
`jmp rel32` 5B), all jumping to 0x153, stride 12 confirmed through the
block; `.text` size 0xdd0 = 0x1d0 + 0xC00 ✓; `.data` 0x800 = 256×8 table ✓.
Relocations independently verified: **256 × IMAGE_REL_AMD64_REL32 against
`mono_ftnptr_table`**, first at 0x1d3 (thunk 0's disp32 field), spacing 12,
last at 0xdc7 (thunk 255) ✓. The stored disp32 addends are 0/8/16/24/…/2040
= i*8 ✓, so `mono_ftnptr_thunks + i*12` (interp.c) lands on the thunk that
loads `table[i]` ✓. Thunks are pure jumps (no frame, no calls); no unwind
info needed — the native caller's return address stays on top of the stack
across the jump ✓.

### (a.4) Patch 31/32 C side — correct

- Patch 31: `get_interp_to_native_trampoline` (interp.c:1369) returns the
  static stub on UWP. One site, no marshalling change. ✓
- Patch 32: `interp_create_method_pointer` (interp.c:2751).
  `entry_wrapper` is initialized to NULL at declaration, so the UWP
  `#else` (empty) in the `#ifndef MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE`
  block is safe — and `MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE` is NOT defined
  on amd64, so the block compiles and the UWP branch takes the
  `MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE` path (=1, mini-amd64.h:470).
  `entry_func = interp_entry_from_trampoline` for every signature on UWP;
  the static thunk always jumps to the static trampoline, which is exactly
  what `entry_wrapper` is set to on this path — consistent with the JIT
  version's `mono_create_ftnptr_arg_trampoline (ftndesc, entry_wrapper)`. ✓
- `mono_uwp_alloc_ftnptr_thunk`: atomic slot claim (`mono_atomic_inc_i32`),
  bounds-checked with a loud `g_error` on exhaustion, store + memory
  barrier before the address escapes. No race, no use-before-fill (the
  address is handed to native code only after the store). ✓
- LMF skip: the direct-entry choice is sound for interpreter-only mode
  (no JITted code to resume into); the documented caveat (exceptions from
  native-invoked callbacks can't propagate across native frames) matches
  the desktop primary path. Not a bug, and irrelevant to the current death
  (no delegate has been invoked yet — see §b).

### (a.5) Dead ends checked (not bugs)

- `MONO_ARCH_HAS_NO_PROPER_MONOCTX` is defined only in mini-arm64.h — the
  `ves_pinvoke_method` LMF-wrapper JIT path (interp.c:1412-1415) is dead
  on amd64. ✓
- `MINT_JIT_CALL` / `do_jit_call` (interp.c:2148, 3718) is unreachable:
  `jit_call_supported` (transform.c:846) returns FALSE for pinvoke, icall,
  inflated, and >6-param methods; its AOT branch needs `mono_aot_only`
  (=0 in .125); `mono_interp_jit_classes` is empty (populated only by the
  `--interpreter=jit=...` option, interp.c:6729; the host passes plain
  `--interpreter`). ✓
- `mono_interp_jit_classes` / `mono_aot_only`: both inert in .125. ✓

**§a conclusion: the stubs are not the kill point. The .125 test refutes
the trampoline theory, and this re-audit finds nothing wrong with the
replacement either.**

---

## (b) Between `mono image: Steamworks.NET` and the first pinvoke

### (b.1) The evidence: what the logs actually show

From `/tmp/t125.log` (5,125 lines, cumulative over ~6 launches) and
`/tmp/t122.log` (identical tail):

- Last run: `TRACE mono_jit_exec enter` (5120) → images Game, Framework,
  System, Graphics, **Steamworks.NET (5125, LAST LINE)**. Every launch in
  the file dies at the same point. Deterministic.
- **Zero occurrences in the whole file** of: `DllImport`, `VEH:`,
  `UNHANDLED`, `FATAL`, `fna-native`, `FNA3D_CreateDevice`. (The 200
  `SEH` matches are all the literal text "SEH-guarded" plus old .109
  `mono_jit_init` failures from earlier in the file — none in the last run.)
- No `TRACE mono_jit_exec returned`, no `managed Main returned`, no
  `mono_jit_exec failed` → `mono_jit_exec` never returns: the process dies
  inside it.
- No watchdog tick, no PROBE. The watchdog thread sleeps 3 s, then logs
  via the allocation-free `trt_trace` writer (mono_host.cpp:507-534). The
  watchdog is a plain host `std::thread` (not a Mono thread, so SGen
  stop-the-world cannot suspend it) and `trt_trace` takes only its own
  mutex and writes to a pre-opened FILE* — it would tick during any
  freeze (heap-lock hold, log deadlock, GC stall). It never ticks ⇒ **the
  process is DEAD within ~3 s of exec enter, not frozen.** (This also
  reframes the .112–.114 "hang": those builds predate the watchdog, so
  their log-freeze + dark server is equally consistent with this same
  death; the FNA3D null-device finding remains a real *later* bug, not
  this one.)
- No steam-door lines ⇒ the native `Init` entry was never reached. No
  `fna-native` / `FNA3D_CreateDevice` lines ⇒ NO pinvoke or FNA native
  call executed at all in any run. The death precedes the first pinvoke
  *call*, not just the first pinvoke *return*.

### (b.2) What the silence rules out (and what it doesn't)

- **Main-thread SEH: RULED OUT.** `mono_jit_exec` is inside
  `__try/__except` (mono_host.cpp:540-551) with a filter that always
  executes the handler, which logs "managed Main raised SEH..." via both
  `mono_log` and `trt_trace`. No such line ⇒ no SEH on the main thread
  (except the pathological sub-case of stack overflow so deep the filter
  itself cannot run — see §b.6).
- **Managed exception escaping Main: RULED OUT.** The unhandled hook
  (mono_host.cpp:505, `mono_unhandled_hook` at :165) logs
  `MONO UNHANDLED EXCEPTION: <name>` before `mono_jit_exec` returns -1.
  Neither line appears ⇒ no managed exception reached the top.
- **Mono FATAL log: RULED OUT as an indicator, not as an event.** The
  host routes Mono's structured log to tiramisu.log (mono_host.cpp:312-314),
  but a `g_assert` does NOT go through that path (see §b.4).
- **VEH: NOT INSTALLED — a gap, not evidence.** `mono_veh` is defined
  (mono_host.cpp:177) but nothing ever calls `AddVectoredExceptionHandler`
  for it (the only references are the guest-kernel32 bridge). So an AV /
  illegal instruction / stack overflow **on any non-main thread kills the
  process with zero logging**. The absence of a `VEH:` line proves nothing
  about non-main threads.
- **DllImport warnings: SUPPRESSED BY DEFAULT — absence proves nothing.**
  Mono's default log level is `G_LOG_LEVEL_ERROR` (mono-logger.c:44) and
  `mono_tracev_inner` (mono-logger.c:91) drops anything above the level
  or outside the mask. The host never calls `mono_trace_set_level/mask`.
  So the WARNING `DllImport unable to load library` and all INFO
  `DllImport ...` traces are silently dropped even on resolution failure.

### (b.3) The ordered steps (Mono 6.8.0.105, patched tree on mineplex)

The first pinvoke is `Steamworks.NativeMethods::SteamAPI_Init()` →
import name **`Init`**, module **`CSteamworks`** (steamworks.implmap:1).
The door is `Init()` in mono_steam_door.cpp:77 ("steam-door: Init
called"). Dllmap `CSteamworks → __Internal` is installed by the image
hook (mono_host.cpp:237). Steps after the hook logs the image line:

1. **Assembly-load tail.** The load hook fires at the END of
   `mono_assembly_request_load_from` (assembly.c:3176; only a profiler
   event follows). After the hook returns, the load is essentially done
   and control returns to whatever triggered the load (a transform
   resolving a MemberRef to Steamworks.NET, or a type load). *Silent-death
   risk: none — nothing left to run here.*
2. **Resolver continuation.** Method/type resolution
   (`mono_get_method` / `mono_class_get` / `mono_method_get_signature` /
   `mono_class_vtable_checked`). *Silent-death risk: LOW — metadata
   decode failures surface as MonoError → managed exception (would log
   via the hook), unless a `g_assert` fires inside class/vtable setup.*
3. **Class init of the touched Steamworks type.**
   `mono_runtime_class_init_full` (interp.c:2935 macro, :3144 newobj path,
   transform.c:6754) runs the .cctor if any (none known for
   `SteamAPI`/`NativeMethods`). *Silent-death risk: LOW — a failing .cctor
   throws a managed exception (would log); an assert inside class init
   would be silent.*
4. **Transform of the calling method.** Lazy, at first execution:
   `interp_exec_method_full` → `do_transform_method` (interp.c).
   `interp_transform_call` (transform.c:~2100-2360) →
   `interp_transform_internal_calls` (transform.c:1715): a PINVOKE_IMPL
   target is replaced by `mono_marshal_get_native_wrapper` (marshal.c).
   *Silent-death risk: MEDIUM — the transform is assert-dense; a
   `g_assert`/`mono_error_assert_ok` here aborts silently (candidate 2).
   A MonoError here becomes a managed exception (would log).*
5. **Wrapper creation + pinvoke resolution.**
   `mono_marshal_get_native_wrapper` → `mono_lookup_pinvoke_call_internal`
   (marshal.c:3513) → `lookup_pinvoke_call_impl` (loader.c:1310) →
   `mono_dllmap_lookup` (loader.c:1355 → :1020: `CSteamworks → __Internal`)
   → `pinvoke_probe_for_module` (loader.c:~1500: `__Internal` →
   `mono_dl_open(NULL)` → `GetModuleHandleW(NULL)`, mono-dl-windows.c:79)
   → `pinvoke_probe_for_symbol` → `mono_dl_lookup_symbol`
   (mono-dl-windows.c:156: `GetProcAddress`). **`Init` is NOT exported by
   the exe (verified against the .125 export table — §b.7), so this
   returns NULL with `LOOKUP_PINVOKE_ERR_NO_SYM`.** *Silent-death risk:
   NONE at this step — the failure is handled (step 6).*
6. **Throw-wrapper.** `!piinfo->addr` → `mb_emit_exception_for_error`
   (marshal.c:~3568-3585): the wrapper is built to THROW
   `EntryPointNotFoundException` when called. No assert, no abort.
   *Silent-death risk: NONE.*
7. **(Never reached in .125) The call.** Caller executes the wrapper →
   `MINT_CALLI_NAT` (interp.c:3622) → `ves_pinvoke_method` (interp.c:1403)
   → `mono_arch_set_native_call_context_args` (mini-amd64.c:1186) →
   `INTERP_PUSH_LMF_WITH_CTX` → static stub → native entry. For `Init`
   this step is replaced by the step-6 throw; for an exported pinvoke
   (e.g. `RestartAppIfNecessary`, `FNA3D_CreateDevice`) it would run the
   stub (verified correct, §a). *Silent-death risk if reached: the stub
   itself is clean; a bad `addr` would AV on the main thread → SEH guard
   would log (it doesn't).*
8. **(Racing steps 2–7) First GC / finalization.** Assembly loads +
   transforms allocate heavily; the SGen nursery fills → first minor GC
   → if any finalizable garbage exists, the finalizer thread runs
   `mono_gc_run_finalize` → **lazily JIT-compiles the finalizer
   runtime-invoke wrapper** (gc.c:296-306, see candidate 1). *Silent-death
   risk: HIGH — this is the only JIT site whose trigger is
   allocation-volume-driven and therefore lands deterministically at this
   point in startup.*

Note the export asymmetry that sharpens the conclusion: `Init` is
unexported (step 5 fails → step-6 throw-wrapper), but
`RestartAppIfNecessary` and `FNA3D_CreateDevice` ARE exported — and
neither their door lines nor the `FNA3D_CreateDevice` trace ever appear.
So the death precedes the first pinvoke call *regardless of which
pinvoke is first and regardless of the export bug*. The throw-wrapper
from step 6 also cannot be the silent death: if it ever threw, the
unhandled hook would log it (§b.2).

### (b.4) The silent-death mechanisms that fit (and the one that fits best)

- **`g_assert` / `mono_error_assert_ok` → `abort()`.** `mono_error_assert_ok`
  is `g_assertf` (mono-error-internals.h:146). eglib `g_assert` prints
  `* Assertion at %s:%d, condition '%s' not met` (eglib/glib.h:935) via
  `g_assertion_message` (eglib/goutput.c:178) → `g_logv_nofree` at ERROR
  level → the default handler prints to **stderr (invisible on the
  console — the host's print handlers do not intercept eglib `g_log`)**
  and calls `g_assert_abort()` → `abort()` → SIGABRT. No SEH, no VEH, no
  Mono-log FATAL, no unhandled hook, no watchdog tick (death <3 s). **This
  is the best-fitting mechanism: deterministic, instant, and totally
  silent with the current logging.**
- **Dynamic-code-blocked termination (0xC0000604).** The kernel kills the
  process on executing non-image code under the dynamic-code policy. No
  exception is dispatched (no SEH/VEH). Fits equally well IF some JIT code
  is executed — i.e. only via a live JIT site (§b.6).
- **`__fastfail` (heap corruption / /GS).** Terminates without dispatch.
  No evidence for heap corruption; no specific suspect. Ranked low.
- **AV on a non-main thread.** Silent only because the VEH was never
  installed (§b.2). Candidate threads: the finalizer thread (see
  candidate 1 — an AV there during JIT compile would also be silent),
  the web-server thread (nondeterministic; the death is deterministic).
  Ranked below the assert mechanism (no specific fault identified).

Ruled out as the death: GC deadlock / heap-lock freeze / log deadlock
(watchdog would tick); stack overflow via transform recursion (transforms
are lazy and non-recursive — `mono_interp_get_imethod` creates imethods
without transforming callees, and managed call depth at startup is
shallow); the image hook itself (the identical hook already ran for 6
images in the same run); `do_jit_call` / LMF-wrapper JIT (dead code, §a.5);
pinvoke resolution failure (handled → throw-wrapper → managed exception →
hook would log, §b.3 steps 5–6).

### (b.5) Top 2 death candidates

**Candidate 1 — First-finalization JIT compile (gc.c:296-306).**
On the first finalization, `mono_gc_run_finalize` lazily compiles the
finalizer runtime-invoke wrapper:

```c
if (!domain->finalize_runtime_invoke) {
    ... mono_marshal_get_runtime_invoke (finalize_method, TRUE);
    domain->finalize_runtime_invoke = mono_compile_method_checked (invoke, error);
    mono_error_assert_ok (error); /* expect this not to fail */
}
```

`mono_compile_method_checked` is NOT interpreter-aware (object.c:788-797:
it just calls `callbacks.compile_method` — the JIT). With `mono_aot_only`
= 0 it proceeds to full codegen, which allocates RWX via the code manager
(patch 29/30 fallback: RW alloc + `VirtualProtectFromApp` to RWX). The
console probe already proved RWX allocation is denied (err=87); if the
protect fails, compilation fails, the error is set, and
`mono_error_assert_ok` aborts — silent (§b.4). If the protect somehow
succeeded, the finalizer thread would then EXECUTE JIT code and the
kernel would kill the process with STATUS_DYNAMIC_CODE_BLOCKED — also
silent. Either sub-case fits. Why this timing: the trigger is
allocation-volume-driven (first GC after 7 assembly loads + transforms),
which explains why the death point is identical across .112–.125
regardless of the FNA3D fix, patch 30, and the static stubs. Caveat: it
requires finalizable garbage to exist by ~3 s into Main (plausible —
startup IO/framework init creates finalizable objects — but not proven
from here). The §c nursery test settles it in one run.

**Candidate 2 — `g_assert` abort in the transform / class-init /
wrapper-create path for the first Steamworks touch.** The window (steps
2–4, 6) runs through assert-dense code: `mono_interp_get_imethod` /
transform (interp.c:466/468, 901, 1506, 3157/3159; transform.c:2108),
`mono_class_init` / vtable setup, `mono_marshal_get_native_wrapper`
(`g_assert (pinvoke)`, `g_assert (piinfo->addr)` after the handled
failure branch). Any one of them firing aborts silently (§b.4). No
specific assert is identified — Steamworks.NET is ordinary metadata and
this path is well-trodden upstream, which is why this ranks below
candidate 1 — but the mechanism is proven and the §c stderr redirect
will print the exact `file:line` if this is the cause.

### (b.6) Other live JIT sites (audited — not the death, but real landmines)

`mono_compile_method_checked` has no interpreter check, so every caller
is a live JIT site in `--interpreter` mode. Audited callers:

- **marshal.c:331 — `mono_delegate_to_ftnptr_impl` → JIT. LIVE.**
  `ves_icall_Marshal_GetFunctionPointerForDelegateInternal` calls the
  impl directly with NO interpreter check. Patch 32 only covers the
  *delegate-construction* path (`interp_delegate_ctor` →
  `create_method_pointer` callback, interp.c:1536). A managed call to
  `Marshal.GetFunctionPointerForDelegate` (Steamworks.NET callback
  registration does this right after `SteamAPI_Init`) will JIT-compile
  and die the same silent death. Next-order bug — needs a Mono patch
  routing the icall through the interp callback when
  `mono_use_interpreter`. (Not the current death: callback registration
  happens after Init, and no pinvoke has executed yet.)
- **marshal.c:508 — `mono_ftnptr_to_delegate_impl`** (GetDelegateFor
  FunctionPointer): JIT. No caller before the first pinvoke. Landmine.
- **icall.c:8144 — `RuntimeMethodHandle.GetFunctionPointer`**: JIT. No
  caller before the first pinvoke. Landmine.
- **marshal.c:4050/4065 — VTFixup wrappers**: JIT, but only for methods
  with VtFixup (none in Celeste/Steamworks). Dead in-window.
- **object.c:3295 — `mono_method_get_unmanaged_thunk`**: JIT; used by
  native-to-managed thunks, not by this host. Dead in-window.
- **object.c:8222/8224 — ExecutionContext capture**: JIT; async only.
  Dead in-window.
- **remoting.c / cominterop.c**: JIT; no remoting/COM at startup. Dead.

### (b.7) The missing `Init` export — confirmed next-order bug

Diffed the 872 Steamworks.NET pinvoke imports (steamworks.implmap)
against the .125 exe's export table (1,474 exports,
`out-brown-125/layout/tiramisu.exe`):

- **184 of 872 imports are NOT exported**, including the first pinvoke
  `Init`, plus `Shutdown`, `RunCallbacks`, `IsSteamRunning`,
  `SteamClient_`, `GetHSteamPipe_`, `GetHSteamUser_`,
  `Steam_GetHSteamUserCurrent_`, `GetSteamInstallPath`,
  `SetTryCatchCallbacks`, `ReleaseCurrentThreadMemory`,
  `ISteamUser_GetSteamID`, `ISteamUser_BLoggedOn`,
  `ISteamUtils_GetAppID`, `ISteamFriends_GetPersonaName`,
  `ISteamUserStats_RequestCurrentStats`, `ISteamUserStats_StoreStats`.
- **39 of the 184 ARE defined in mono_steam_door.cpp** but missing from
  `host-bridges.def` (a .def-only fix): `Init`, `Shutdown`,
  `RunCallbacks`, `IsSteamRunning`, `SteamClient_`, `GetHSteamPipe_`,
  `GetHSteamUser_`, `Steam_GetHSteamUserCurrent_`, `GameServer_RunCallbacks`,
  `ISteamApps_BIsCybercafe`, `ISteamApps_BIsLowViolence`,
  `ISteamApps_BIsSubscribed`, `ISteamApps_BIsVACBanned`,
  `ISteamApps_GetAppBuildId`, `ISteamApps_GetAvailableGameLanguages`,
  `ISteamApps_GetCurrentGameLanguage`,
  `ISteamClient_BShutdownIfAllPipesClosed`, `ISteamClient_CreateSteamPipe`,
  `ISteamClient_GetIPCCallCount`,
  `ISteamController_GetControllerForGamepadIndex`, `ISteamController_Init`,
  `ISteamController_RunFrame`, `ISteamController_Shutdown`,
  `ISteamFriends_GetPersonaName`, `ISteamFriends_GetPersonaState`,
  `ISteamRemoteStorage_GetFileCount`,
  `ISteamRemoteStorage_IsCloudEnabledForAccount`,
  `ISteamRemoteStorage_IsCloudEnabledForApp`,
  `ISteamRemoteStorage_SetCloudEnabledForApp`, `ISteamUser_BLoggedOn`,
  `ISteamUser_GetSteamID`, `ISteamUtils_GetAppID`,
  `ISteamUtils_GetConnectedUniverse`, `ISteamUtils_GetCurrentBatteryPower`,
  `ISteamUtils_GetIPCountry`, `ISteamUtils_GetSecondsSinceAppActive`,
  `ISteamUtils_GetSecondsSinceComputerActive`,
  `ISteamUtils_GetServerRealTime`, `ISteamUtils_GetSteamUILanguage`.
  The remaining 145 have no door function in the .cpp (some may exist via
  `gen/gen_steam_failvisible.inc` — the same diff should be run against
  it; the .def should be regenerated from the implmap + door).
- Consequence: once the silent death is fixed, the first pinvoke
  (`Init`) will throw `EntryPointNotFoundException` (throw-wrapper, §b.3
  step 6) unless the exports are added. This is NOT the current silent
  death (a throw would log via the unhandled hook), but it is the
  immediate next failure. (Note the door file has no `__declspec(dllexport)`
  and build-host.sh:123 links only `/DEF:host-bridges.def`, so the .def
  is the sole export mechanism for the door.)

---

## (c) Minimal next diagnostic — host-side .126 test (preferred, no Mono rebuild)

One test build, host changes only (all in `trt_mono_run`, before
`mono_jit_init`). The changes are observational except #1, which is the
decisive discriminator for candidate 1:

1. **Nursery delay (the discriminator).** `_putenv_s("MONO_GC_PARAMS",
   "nursery-size=64m")` before `mono_jit_init`. SGen reads
   `MONO_GC_PARAMS` (`nursery-size=` must be a power of two,
   sgen-gc.c:3460). A 64 MB nursery defers the first minor GC — and hence
   the first finalization — well past startup. If the game proceeds past
   Steamworks.NET, candidate 1 is CONFIRMED.
2. **stderr redirect (prints the exact assert).** `freopen` stderr onto
   tiramisu.log (append) before `mono_jit_init`. eglib `g_assert` /
   `g_error` write to stderr directly (the host's Mono print handlers do
   not intercept them); the message is `* Assertion at <file>:<line>,
   condition '<expr>' not met`. This pinpoints candidate 2's exact site.
3. **Install the VEH.** `AddVectoredExceptionHandler(1, mono_veh)` before
   `mono_jit_init`. `mono_veh` (mono_host.cpp:177) already logs
   `VEH: exception 0x... at 0x...` for AV / illegal instruction / stack
   overflow / breakpoint — it was simply never installed. Distinguishes
   AV (any thread) from abort / kernel kill.
4. **SIGABRT handler.** `signal(SIGABRT, ...)` → `trt_trace("TRACE
   SIGABRT (g_assert/abort)")` then re-raise / `_exit(3)`. Fires for the
   `g_assert_abort()` path (§b.4).
5. **Mono log level.** `_putenv_s("MONO_LOG_LEVEL", "info")` +
   `_putenv_s("MONO_LOG_MASK", "dll,asm")` before `mono_jit_init`
   (env vars read by `mono_trace_init`, mono-logger.c:44-52; `dll` =
   MONO_TRACE_DLLIMPORT, mono-logger.c:281+). Surfaces the DllImport +
   assembly traces that are suppressed by default, so a resolution
   failure would show as `DllImport unable to load library ...`.
6. **Test agent:** record the process exit code / termination status
   from the portal (0 = clean exit, 3 = abort/SIGABRT, 0xC0000604 =
   dynamic-code-blocked, 0xC0000005 = unhandled AV) and confirm the
   process is gone (not suspended).

### Expected outcomes

| Observation in .126 | Conclusion |
|---|---|
| Game proceeds past Steamworks.NET (new image lines, `FNA3D_CreateDevice` trace, steam-door lines) | **Candidate 1 CONFIRMED** — death is GC/finalizer-driven. Fix: Mono patch making finalization interp-safe (route `finalize_runtime_invoke` through the interpreter instead of `mono_compile_method_checked`; audit the §b.6 sites the same way) + add the §b.7 exports. |
| Still dies at same point + stderr shows `* Assertion at <file>:<line>` | **Candidate 2 CONFIRMED**, exact site printed. Fix that assert's root cause. |
| Still dies + `VEH: exception ...` line | AV — fault address distinguishes NULL deref / heap / image code; follow the address. |
| Still dies + `TRACE SIGABRT` but no stderr text | `abort()` from outside eglib (e.g. CRT / `std::terminate`) — investigate host/CRT path. |
| Still dies + `DllImport unable to load library` | Resolution failure (unexpected — the throw-wrapper should then log via the unhandled hook; if the hook still doesn't fire, the death is in exception propagation — use the Mono markers below). |
| Still dies + NOTHING new (no VEH, no SIGABRT, no stderr, no DllImport) | Kernel kill without dispatch: dynamic-code-blocked termination (some JIT code still executes — re-audit §b.6 sites for a pre-first-pinvoke caller) or `__fastfail` / `ExitProcess`. Proceed to the Mono-side markers below. |

### Follow-up if the host-side test comes back empty: Mono-side markers

Small patch (new patch file, lib rebuild ~3 min): `g_print` markers —
`g_print` IS routed to tiramisu.log via `mono_print_to_log`
(mono_host.cpp:312). Minimal set, in firing order:

- `mono/metadata/gc.c:~296` (before `if (!domain->finalize_runtime_invoke)`):
  `MARKER finalize enter` + after the compile: `MARKER finalize compiled %p`
- `mono/metadata/loader.c:1310` (`lookup_pinvoke_call_impl` entry):
  `MARKER lookup enter %s`; after the probe: `MARKER lookup exit addr=%p`
- `mono/mini/interp/transform.c:~1715` (`interp_transform_internal_calls`,
  pinvoke branch): `MARKER wrap pinvoke %s`
- `mono/mini/interp/interp.c:1403` (`ves_pinvoke_method` entry):
  `MARKER pinvoke enter addr=%p`; immediately before `entry_func (...)`:
  `MARKER pinvoke calling stub`; immediately after: `MARKER pinvoke returned`
- `mono/mini/interp/interp.c` `do_transform_method` entry:
  `MARKER transform %s` (method full name — shows which method's transform
  is in progress when it dies)

The last marker printed brackets the death to one step. (Do NOT use
`mono_trace` for these — its default level/mask suppresses everything
below ERROR; `g_print` is the routed channel.)

### Fixes queued behind the diagnosis (do not apply blind)

- **§b.7 exports**: add the 39 defined-but-unexported door functions to
  `host-bridges.def` (list above); regenerate the .def from
  steamworks.implmap + the door + `gen_steam_failvisible.inc` for the
  remaining 145.
- **§b.6 delegate-ftnptr JIT bypass**: route
  `ves_icall_Marshal_GetFunctionPointerForDelegateInternal`
  (marshal.c:5620) through `mini_get_interp_callbacks()->
  create_method_pointer` when running under the interpreter.
- **Candidate 1 fix** (if confirmed): same treatment for
  `domain->finalize_runtime_invoke` in gc.c:296-306.

## Evidence index

- Disassembly: `win64.obj` extracted from
  `~/.cache/tiramisu/mono-uwp/out/libmonosgen-uwp.lib` via `llvm-ar x`
  + `llvm-objdump -d/-r/-h/-s -j .pdata/.xdata` (this agent's own runs).
- Logs: `/tmp/t125.log` (5,125 lines), `/tmp/t122.log` (4,311 lines).
- Export diff: `llvm-readobj --coff-exports
  out-brown-125/layout/tiramisu.exe` (1,474 exports) vs the 872 imports
  in `third_party/mono-bridges/steamworks.implmap`.
- Mono tree: `~/.cache/tiramisu/mono-uwp/src/mono-6.8.0.105/` (patched
  build tree; patches 31/32 confirmed applied in interp.c).
- Host: `~/tiramisu-brown-merge/src/trt/mono_host.cpp`,
  `third_party/mono-bridges/{mono_steam_door.cpp,steamworks.implmap,
  host-bridges.def}`, `tools/build-host.sh:95,123`.
