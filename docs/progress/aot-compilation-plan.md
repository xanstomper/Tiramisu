# AOT Compilation Plan for Celeste on Tiramisu

**Date:** 2026-11-21 ~02:10 EST. **Author:** offline analysis (no console touch, no builds in the shared tree; all probes in /tmp). **Status:** plan only — no code written, no builds run, no console state changed.

**Context:** Celeste runs under embedded Mono 6.8.0.105 in INTERPRETER mode on UWP (no JIT in AppContainer). The interpreter perf analysis (celeste-interpreter-perf.md, 18:32 EST) concluded Celeste is LIKELY playable (60fps light scenes, 30-50fps busy scenes) but a locked 60fps is NOT assured — the managed update loop is ~10-15x slower under the interpreter. The REAL mitigation is full AOT compilation (the iOS model).

## Bottom line: AOT is FEASIBLE, but NOT via the naive path

Three corrections to the perf analysis's framing (all verified by code + probes, see Evidence Log):

1. **The interpreter-first problem (the big one).** With `--interpreter` forced, Mono NEVER consults AOT code. `mono_jit_compile_method_with_opt` (mini-runtime.c:2290-2300) tries the interpreter's `create_method_pointer` FIRST and returns immediately if it succeeds — the AOT lookup (`mono_aot_get_method`) is never reached. So "add AOT images, keep --interpreter, Mono uses AOT where available" is WRONG for Mono 6.8.0.105. The host must switch to **MONO_AOT_MODE_INTERP** (`--full-aot-interp` / `mono_jit_set_aot_mode`) — AOT first, interpreter fallback. This is a host code change, not just "add AOT images."
2. **The `interp` AOT option is a trap.** `mono --aot=interp` compiles ZERO native methods (verified: `Compiled: 0/0`, both native and cross-triple). It sets JIT_FLAG_INTERP, which emits interpreter bytecode instead of native code — a startup optimization, not native AOT. Do NOT use it.
3. **The COFF problem.** The Linux host mono emits ELF/GAS assembly even with `mtriple=x86_64-pc-windows-msvc` (the triple affects codegen, not the emitter). clang for the Windows triple rejects it (ELF directives: `.subsection`, `.hword`, `.text 0`). MinGW `as` rejects it (`.local`, `.type @function`, `.size`). objcopy ELF→COFF produces WRONG relocations (R_X86_64_PC32 → IMAGE_REL_AMD64_ADDR32, verified by disassembly: the RIP-relative `mov rax,[rip+disp]` at P_Main+0x4 would compute a garbage address). **The viable path is assembly translation** — a script that rewrites the AOT .s from ELF/GAS to COFF/MinGW syntax, then assembles with `x86_64-w64-mingw32-as` (installed on mineplex). That script is the main engineering work in this plan.

**Feasibility verdict:** FEASIBLE with real work. The runtime side is proven (AOT runtime in the UWP lib, all symbols present, INTERP mode available). The build-time side needs the translation script (the main risk) + host changes + build integration + console verification. Do NOT start until Celeste boots on the interpreter build (Step 0) — AOT is a perf optimization, not a boot fix.

## 1. What was verified

| Claim | Evidence |
|---|---|
| Host mono matches the runtime | `/usr/bin/mono` = Mono JIT 6.8.0.105 (Debian 6.8.0.105+dfsg-3.6ubuntu2) — SAME version as the UWP runtime (6.8.0.105). AOT file version matches. |
| AOT runtime in the UWP lib | mono-sources.txt:49-51 lists `mono/mini/aot-compiler.c`, `aot-runtime.c`, `aot-runtime-wasm.c` in libmini. |
| AOT symbols present in libmonosgen-uwp.lib | out-brown-138/obj/mono-symbols.txt: `mono_aot_register_module` (T), `mono_aot_init` (T), `mono_aot_get_method` (T), `mono_aot_can_enter_interp` (T), `mono_jit_set_aot_mode` (T), `mono_jit_parse_options` (T), `mono_jit_set_aot_only` (T), `mono_aot_mode` (D), `mono_aot_only` (B). driver.c is in the lib (mono-sources.txt:46). |
| FNA core AOT-compiles | `mono --aot=static Microsoft.Xna.Framework.dll` → `Compiled: 4380/4380`, 1,182,208 B IL → 4,122,256 B .o (3.5x), linking symbol `mono_aot_module_Microsoft_Xna_Framework_info`, ~1.1s. |
| Cross-triple codegen works | `mono --aot=static,mtriple=x86_64-pc-windows-msvc` on a probe assembly → `Compiled: 3/3`. The triple is honored by the JIT backend. |
| `interp` option compiles nothing | `mono --aot=static,interp` → `Compiled: 0/0` (native AND cross). Sets JIT_FLAG_INTERP (aot-compiler.c:8609). |
| clang cannot assemble the .s | `clang -target x86_64-pc-windows-msvc -c p.exe.s` → errors on `.subsection`, `.hword`, `.text 0`, `.local`. Fails with and without `nodebug`. |
| MinGW as cannot assemble the .s | `x86_64-w64-mingw32-as` → "unknown pseudo-op: `.local`", `.type`/`.size` "junk at end of line". Fails via `tool-prefix=` too. |
| objcopy COFF conversion is UNSOUND | `objcopy -O pe-x86-64` produces a valid-looking COFF (symbols + sections intact, `mono_aot_module_p_info` present) but maps R_X86_64_PC32 → IMAGE_REL_AMD64_ADDR32. Disassembly proves the relocated instruction is RIP-relative (`49 8b 05` mov rax,[rip+disp] at P_Main+0x4) — ADDR32 would write an absolute address into the displacement field, computing a garbage address. REJECTED. |
| LLVM backend unavailable | `mono --aot=llvm` → "--aot=llvm requires a runtime compiled with llvm support." The Debian mono has no LLVM support. |
| MONO_AOT_MODE_INTERP is the hybrid | driver.c:2851-2858: INTERP sets `mono_aot_only=TRUE` + `mono_use_interpreter=TRUE` + `use_aot_trampolines=TRUE`, WITHOUT `force_use_interpreter`. CLI: `--full-aot-interp` (driver.c:2186). API: `mono_jit_set_aot_mode(MONO_AOT_MODE_INTERP)`. jit.h:63 doc: "Uses Interpreter, JIT is disabled and not allowed, equivalent to '--full-aot --interpreter'." |
| Interpreter fallback exists in INTERP mode | mini-runtime.c:2415: `if (!jit_only && !code && mono_aot_only && mono_use_interpreter && method->wrapper_type != MONO_WRAPPER_OTHER)` → falls back to the interpreter's `create_method_pointer`. This is why INTERP mode works where bare `mono_aot_only` (.123/.124) died: .123 set aot_only WITHOUT the interpreter fallback. |
| Static AOT registration API | `mono_aot_register_module(gpointer *aot_info)` (aot-runtime.c:2482) — "called by embedding code to register normal AOT modules statically linked into the executable." Param = the `mono_aot_module_<ASSEMBLY_NAME>_info` global symbol. "This could be called before startup." Inserts into `static_aot_modules` keyed by assembly name. `load_aot_module` (aot-runtime.c:2110) looks up `static_aot_modules` by `assembly->aname.name` at assembly-load time and logs "Found statically linked AOT module '%s'." |
| Host has zero AOT registration today | mono_host.cpp references only `mono_jit_set_aot_only` (declared line 138, never called — the .124/.125 notes say it is NOT set). Zero `mono_aot_register_module` references in src/. |

## 2. Build-time steps (a)

### 2.1 The AOT set

Celeste.exe's assembly refs (monodis --assemblyref): mscorlib, Microsoft.Xna.Framework, System.Xml, Microsoft.Xna.Framework.Graphics, System, System.Core, Microsoft.Xna.Framework.Game, Steamworks.NET.

**Phase 1 (game + direct deps — do this first):**
- `Celeste.exe` (2,570,752 B, ~/celeste-xbox/Celeste/Celeste.exe)
- `Microsoft.Xna.Framework.dll` / FNA core (1,182,208 B, ~/tiramisu-brown-merge/third_party/fna/out/ — the BROWN-MERGE variant on the console, NOT the mono-tree variant (1,169,408 B); verify by hash against the console copy before AOT)
- `Steamworks.NET.dll` (265,216 B, ~/celeste-xbox/Celeste/Steamworks.NET.dll)
- SKIP the forwarders (Microsoft.Xna.Framework.Graphics.dll, Microsoft.Xna.Framework.Game.dll) — they contain only `[TypeForwardedTo]` attributes, no methods, nothing to AOT.

**Phase 2 (framework — only if Phase 1 doesn't reach 60fps in busy scenes):**
- `mscorlib.dll` (4,811,264 B), `System.dll`, `System.Core.dll`, `System.Xml.dll` — from /usr/lib/mono/4.5/ (the same source build-host.sh §2d copies into the layout). NOTE: System/System.Core/System.Xml in /usr/lib/mono/4.5 are SYMLINKS into ../gac/ — resolve them to the real file before AOT. AOT the files AFTER build-host.sh copies them to $out/layout/mono/4.5, so the AOT input is byte-identical to the shipped file.

### 2.2 The AOT command (per assembly)

```
mono --aot=static,nodebug,full,mtriple=x86_64-pc-windows-msvc,asmonly <assembly>
```

- `static` — emit the `mono_aot_module_<Name>_info` symbol for static linking (also sets `no_dlsym`).
- `nodebug` — drop debug sections (smaller .s, fewer ELF directives to translate).
- `full` — compile in full-AOT mode (all methods + wrappers; the standard flag for full-AOT images).
- `mtriple=x86_64-pc-windows-msvc` — Windows calling convention + codegen (verified: Compiled 3/3).
- `asmonly` — emit the .s file only; do NOT invoke the host assembler (it would produce an ELF .o).
- **Do NOT use:** `interp` (0 methods), `llvm` (unavailable), `direct-pinvoke` (bypasses the __Internal dllmaps — would break the Steam/FMOD/FNA bridges), `direct-icalls` (bakes in icall addresses; the host registers custom internal calls at runtime).

### 2.3 The translation step (the new engineering)

```
python3 tools/aot-translate.py <assembly>.s <assembly>.coff.s
x86_64-w64-mingw32-as --64 -o <assembly>.obj <assembly>.coff.s
```

Translation rules (verified against the probe .s, 314 lines / ~15 ELF directives):
- Delete `.local sym` lines (COFF symbols are local by default unless `.globl`).
- Delete `.type sym,@function` / `.type sym,@object` lines.
- Delete `.size sym,.-sym` lines.
- Delete `.subsection N` lines.
- `.section ".data.rel.ro"` → `.section .rdata` (pointer tables; the linker resolves the relocations at static link time).
- `.section ".rodata"` → `.section .rdata`.
- `.text N` → `.text` (drop the subsection number).
- Pass through: `.balign`, `.quad`, `.long`, `.byte`, `.string`, `.globl` (MinGW `as` supports all of these).
- Validate per module: (a) MinGW `as` assembles with ZERO errors; (b) `llvm-nm` shows `mono_aot_module_<Name>_info`; (c) `objdump -r` shows REL32 (IMAGE_REL_AMD64_REL32) for the RIP-relative sites, NOT ADDR32; (d) the .obj links into a trivial Windows test exe with lld-link (proves the COFF object is linkable).

Fallback if translation proves fragile: patch the AOT compiler's emitter to support COFF from Linux (the Windows emitter code exists in aot-compiler.c but is guarded by HOST_WIN32 — the host mono was built on Linux, so it is not compiled in). Significant Mono patch; not the first path.

### 2.4 The link step + caching

- Add the .obj files to the build-host.sh link (as inputs to the uwp-crossbuild wrapper, or via `--link-arg`). The .obj files are COFF (MinGW `as` output) — lld-link accepts them.
- Cache the .obj files keyed by (IL file sha256 + AOT flags + mono version), e.g. `~/.cache/tiramisu/aot/`. Re-AOT only when the IL changes. (FNA AOT takes ~1s; mscorlib will take minutes — caching matters.)

### 2.5 Size estimate (from the measured 3.5x FNA ratio)

| Assembly | IL size | Est. AOT .obj |
|---|---|---|
| Celeste.exe | 2.57 MB | ~9 MB |
| Microsoft.Xna.Framework.dll (FNA) | 1.18 MB | 4.1 MB (measured) |
| Steamworks.NET.dll | 0.27 MB | ~0.9 MB |
| **Phase 1 total** | | **~14 MB** |
| mscorlib.dll | 4.81 MB | ~17 MB |
| System.dll | ~3 MB | ~10 MB |
| System.Core.dll | ~1 MB | ~3.5 MB |
| System.Xml.dll | ~3 MB | ~10 MB |
| **Phase 2 total** | | **~40 MB** |
| **Full set** | | **~54 MB** |

The exe grows from 7.7 MB to ~22 MB (Phase 1) or ~62 MB (full set). The MSIX (compressed) grows from ~12 MB to ~20 MB (Phase 1) or ~40-50 MB (full set). Deploy time grows proportionally (the 12 MB MSIX takes ~10 min over the portal; a 50 MB MSIX might take 30-50 min — a workflow cost, not a blocker). The 5 GB UWP memory cap is not threatened by 50 MB of code. **Optimization (Phase 3):** profile-guided AOT (`profile=` + `profile-only`) — AOT only the hot methods from a runtime profile; could cut the size by 50-70%. Needs a profile run first (on Linux via the offline test, or on the console with MONO_PROFILE).

## 3. Host code changes (b)

mono_host.cpp, `trt_mono_run` (UWP path):

**1. Register the AOT modules** — after `mono_jit_init` succeeds (after line 541 "domain ok", alongside the internal-call registration ~548) and BEFORE `mono_domain_assembly_open` (line 597). The AOT lookup happens at assembly-load time, so registration must precede it.

```cpp
extern "C" void mono_aot_register_module(gpointer *aot_info);
extern "C" gpointer mono_aot_module_Celeste_info[];
extern "C" gpointer mono_aot_module_Microsoft_Xna_Framework_info[];
extern "C" gpointer mono_aot_module_Steamworks_NET_info[];  // VERIFY the exact symbol — see §7
mono_aot_register_module(mono_aot_module_Celeste_info);
mono_aot_register_module(mono_aot_module_Microsoft_Xna_Framework_info);
mono_aot_register_module(mono_aot_module_Steamworks_NET_info);
```

- The symbol name is `mono_aot_module_<AssemblyName>_info` with dots → underscores (verified: `mono_aot_module_Microsoft_Xna_Framework_info` for Microsoft.Xna.Framework). The AOT compiler prints "Linking symbol: '...'" — capture it in the build log and generate the registration code (or a header) from it; do not hand-write symbol names.
- Log each registration (`mono_log("AOT module registered: <name>")`) so the console log proves it.
- Registration CAN happen before `mono_jit_init` (aot-runtime.c: "This could be called before startup") — but after init is clearer and lets the log lines appear in order.

**2. Switch the execution mode** — replace the `--interpreter` option (line 446-447) with `--full-aot-interp`:

```cpp
char* opt_argv[2] = {(char*)"tiramisu-mono", (char*)"--full-aot-interp"};
mono_jit_parse_options(2, opt_argv);
```

OR call `mono_jit_set_aot_mode(MONO_AOT_MODE_INTERP)` directly (declare it like `mono_jit_set_aot_only`; the enum is in <mono/jit/jit.h>, MONO_AOT_MODE_INTERP = 5 — verify the numeric value at build time rather than hard-coding it). Either route sets mono_aot_only=TRUE + mono_use_interpreter=TRUE + use_aot_trampolines=TRUE, WITHOUT force_use_interpreter. AOT code is used first; methods without AOT code fall back to the interpreter (mini-runtime.c:2415).

- Do NOT call `mono_jit_set_aot_only(TRUE)` separately (redundant — INTERP mode sets it; and setting it alone WITHOUT the interpreter fallback is what killed .123).
- Make the mode switch CONDITIONAL on AOT images being present: a compile-time flag (e.g. `-DTIRAMISU_AOT`) that selects `--full-aot-interp` + the registrations; without it, the current `--interpreter` path is used. This keeps the interpreter build as the fallback. (INTERP mode with NO AOT images would fall back to the interpreter for everything — it should work, but it is pointless + carries the .123 risk if the fallback is ever broken.)

**3. AOT verification logging** — add `aot` to MONO_LOG_MASK (line ~436: `"gc,thread,dll,asm,aot"`) so the runtime's "Found statically linked AOT module '%s'." lines appear in tiramisu.log.

**Linux path (tests/test_mono_celeste.cpp):** the offline test loads libmono via dlopen. For the offline AOT test, link the ELF AOT .o files (built with plain `mono --aot=static` on Linux, no mtriple) + register them + set INTERP mode. The Linux AOT .o files are ELF (different objects from the COFF ones for the console) — the AOT compilation runs twice, once per target, or the Linux test uses dynamic .so AOT images (which the Linux mono loads automatically).

## 4. Packaging changes (c)

**Static AOT: NO pack.sh change needed.** The AOT code is linked into tiramisu.exe. The MSIX grows (the exe is bigger), but no new files are added. pack.sh packs $out/layout wholesale — the bigger exe flows through automatically.

**The IL assemblies MUST match the AOT inputs.** The AOT module records the assembly GUID + version; at load time, the runtime verifies the AOT image matches the loaded IL assembly. If the IL file on the console differs from the AOT input (e.g., a different FNA build), the AOT image is rejected SILENTLY — the runtime logs a mismatch and falls back to the interpreter. The game still boots, but with NO perf gain, and the only signal is the ABSENCE of "Found statically linked AOT module" in the log. So the build must AOT-compile the EXACT IL files that ship (see §2.1) and record their sha256 in the build log.

**Dynamic AOT (REJECTED):** separate .dll AOT images in the MSIX, loaded via mono_dl_open → LoadPackagedLibrary. Not recommended: (a) the AOT compiler on Linux produces ELF .so, not Windows DLLs (same COFF problem, worse — a DLL needs a full PE link); (b) the runtime's dynamic lookup (image name + MONO_SOLIB_EXT) would need awkward filenames; (c) UWP LoadPackagedLibrary has restrictions. Static AOT avoids all of this.

## 5. Risks (d)

1. **The translation script is the main engineering risk.** The .s→COFF translation must handle every directive the AOT compiler emits. The probe .s (314 lines) had ~15 ELF directives; the FNA .s will have thousands. If the translator misses a construct, the assembly FAILS LOUDLY (a build error, not a silent runtime bug). Mitigation: validate the translator on the FNA .s (the largest Phase 1 assembly) before committing to it (Step 1 gate).
2. **AOT + interpreter interop (the PLT fallback).** AOT code calls cross-assembly methods via the PLT (verified in the probe disassembly: `call plt_System_Console_WriteLine_int`). If the target has no AOT code, the PLT resolves to the interpreter entry (via the INTERP-mode fallback). This is the standard Xamarin mechanism, but it is UNTESTED on this UWP build. Risk: if the PLT resolution fails for a non-AOT method, the game crashes on the first cross-assembly call to a non-AOT method. Mitigation: Phase 1 AOTs the full closure of the hot path (Celeste + FNA + Steamworks.NET) so cross-assembly calls from AOT code mostly hit AOT code; framework calls (mscorlib) fall back to the interpreter (the tested path — the interpreter already runs mscorlib today). Gates: the offline Linux AOT test (Step 4) + the console boot test (Step 5).
3. **Generics.** AOT cannot compile generic instantiations that are not visible at compile time (e.g., `List<Foo>` where Foo is instantiated via reflection). In INTERP mode, those fall back to the interpreter (correct, just slower). Risk LOW (a perf miss, not a crash) — unless a generic method is on the hot path. Mitigation: profile-guided AOT (Phase 3) captures the actual instantiations from a runtime profile.
4. **Reflection.Emit / dynamic methods.** Cannot be AOT-compiled; fall back to the interpreter. Celeste's core loop does not use them (per the perf analysis). FNA's content manager uses reflection to instantiate types (works under AOT as long as the types are pre-compiled — they are in the AOT image). Risk LOW.
5. **The delegate trampolines (patch 32) are UNAFFECTED.** `mono_delegate_to_ftnptr` is a runtime function (in the Mono lib, not AOT-compiled). The wrapper it creates is runtime-generated; in INTERP mode its code pointer comes from the interpreter's static thunk pool (patch 32). AOT does not change this path. The delegate-JIT analysis's conclusion (GetFunctionPointerForDelegate will NOT break) stands.
6. **The pinvoke path (patch 31) needs a console test.** AOT-compiled methods call pinvoke via AOT pinvoke wrappers (compiled into the AOT image by `add_wrappers`). The wrapper resolves the native function via `mono_dl_lookup` (__Internal → GetProcAddress) at AOT-load time. This SHOULD work (the resolution is dynamic), but it is untested. The .129 test proved pinvoke works through the interpreter; the AOT pinvoke path is a separate gate. Mitigation: the first AOT console test must check that SteamAPI_Init still succeeds (it exercises pinvoke from AOT code — Celeste calls Steamworks.NET, which pinvokes the Steam bridge).
7. **AOT image / IL mismatch (silent fallback).** See §4. If the IL on the console differs from the AOT input, the runtime silently ignores the AOT image and interprets everything. The game boots with NO perf gain. Mitigation: the verification gates (§6) explicitly check for the AOT load lines; the build hashes the IL inputs and records them.
8. **MSIX size + deploy time.** Full AOT adds ~50 MB to the exe; the MSIX grows to ~40-50 MB; deploy time grows (possibly 30-50 min over the portal). Mitigation: Phase 1 only (~14 MB) first; profile-guided AOT (Phase 3) to shrink.
9. **mono_aot_only + the .123 lesson.** INTERP mode sets mono_aot_only=TRUE. .123/.124 proved aot_only WITHOUT AOT images kills Mono. With AOT images + the interpreter fallback, it should work — but if the fallback is broken (e.g., a MONO_WRAPPER_OTHER method without AOT code — the line-2415 fallback EXCLUDES wrapper_type == MONO_WRAPPER_OTHER), the game dies at startup. Mitigation: the offline Linux AOT test catches this before any console deploy; keep the --interpreter build as the fallback (the TIRAMISU_AOT flag). Note: wrappers ARE AOT-compiled by the AOT compiler (`add_wrappers`), so wrapper coverage should be complete.
10. **AOT compiler version skew.** The AOT file version (MONO_AOT_FILE_VERSION) must match between the host mono (6.8.0.105 Debian) and the UWP runtime (6.8.0.105). Both are 6.8.0.105 — MATCH. But if the Debian mono is updated (apt upgrade) or the UWP Mono source is patched in a way that touches the AOT format, the versions could skew. Mitigation: record the mono version + AOT file version in the build log; the runtime asserts on version mismatch (loud failure, not silent).

## 6. Implementation order with verification gates (e)

**Step 0 — Preconditions (before any AOT work).** Celeste boots to the menu on the interpreter build (the current push). AOT is a PERF optimization, not a boot fix — it must not be attempted until the interpreter boot works (otherwise AOT bugs and boot bugs are indistinguishable). **Gate:** the interpreter build reaches GAME DISPLAYED + the menu.

**Step 1 — The translation script (offline, no console).** Write `tools/aot-translate.py` (the ELF→COFF .s rewriter, §2.3). Validate on the probe .s + the FNA .s: (a) MinGW `as` assembles with zero errors; (b) `llvm-nm` shows `mono_aot_module_<Name>_info`; (c) `objdump -r` shows REL32 (not ADDR32) for the RIP-relative sites; (d) the .obj links into a trivial Windows test exe with lld-link (proves the COFF object is linkable). **Gate:** FNA .s translates + assembles + links cleanly.

**Step 2 — The AOT build script (offline, no console).** Write `tools/aot-build.sh` (or a section in build-host.sh): for each Phase 1 assembly, run `mono --aot=static,nodebug,full,mtriple=x86_64-pc-windows-msvc,asmonly` → translate → assemble → cache the .obj (keyed by IL sha256 + flags + mono version). **Gate:** all Phase 1 .obj files build; the .obj files contain the expected module info symbols; total size within the estimate (~14 MB).

**Step 3 — Host changes (offline, no console).** mono_host.cpp: AOT registration + `--full-aot-interp` (behind `-DTIRAMISU_AOT`) + `aot` in MONO_LOG_MASK (§3). build-host.sh: add the .obj files to the link + a check that `mono_aot_register_module` is referenced (like the §2c symbol checks). **Gate:** the host builds (offline gate green); the exe contains the AOT module info symbols (`objdump`/`llvm-nm` on tiramisu.exe).

**Step 4 — Offline Linux AOT test (no console).** Extend tests/test_mono_celeste.cpp (or a new test) to link the ELF AOT .o files (built with plain `mono --aot=static` on Linux, no mtriple) + register them + set INTERP mode. Run Celeste offline: (a) it boots to the title screen (the existing gate); (b) the log shows "Found statically linked AOT module" for each assembly; (c) a targeted AOT-execution check (§6, method 2 below). **Gate:** offline AOT boot passes + AOT load lines present.

**Step 5 — Console AOT test.** Build the UWP host with AOT (TIRAMISU_AOT), pack, deploy, launch Celeste. **Gates:** (a) it boots (menu); (b) tiramisu.log shows "Found statically linked AOT module" for Celeste + FNA + Steamworks.NET; (c) SteamAPI_Init succeeds (pinvoke from AOT code works); (d) frame times improve (Present timestamps: p95 in busy scenes — compare against the interpreter baseline; expect p95 < 16.7ms if AOT delivers).

**Step 6 — How to confirm AOT code is actually executing (not falling back to the interpreter).** This is the critical gate — a boot alone does NOT prove AOT is active (the interpreter fallback would silently run everything). Methods:
1. **Log lines:** MONO_TRACE_AOT "Found statically linked AOT module '%s'." for each assembly (proves the AOT image loaded). "AOT: image '%s' not found" proves the opposite.
2. **Method execution probe:** after the assembly loads, call `mono_aot_get_method` for a known hot method (e.g., a Celeste.Player method or an FNA method) and log whether it returns non-NULL. (`mono_aot_get_method` is in the lib — T symbol confirmed.) NULL for a method that should be AOT-compiled = the AOT image is not active.
3. **Frame-time measurement (the decisive perf proof):** Present timestamps (avg + p95) before (interpreter) vs after (AOT). If p95 drops from 20-30ms to <16.7ms in busy scenes, AOT is executing. If frame times are unchanged, AOT is NOT executing (falling back to the interpreter).
4. **Negative control:** build WITHOUT the AOT registration (but with `--full-aot-interp`) — the game should still boot (interpreter fallback) but with interpreter frame times. This proves the fallback works AND that the AOT build's speedup (if any) comes from AOT, not from the mode switch alone.

**Step 7 — Phase 2 (framework AOT) + Phase 3 (profile-guided AOT).** Only if Phase 1 does not reach 60fps in busy scenes. Phase 2: AOT mscorlib + System + System.Core + System.Xml (same pipeline). Phase 3: capture a runtime profile (MONO_PROFILE=aot on the console or Linux), then re-AOT with `profile=` + `profile-only` to shrink the image to the hot methods.

## 7. Open questions / unknowns

- The exact AOT module info symbol for Celeste.exe (`mono_aot_module_Celeste_info`? — verify by AOT-compiling Celeste.exe; the symbol derives from the assembly NAME, not the file name).
- The exact AOT module info symbol for Steamworks.NET.dll (`mono_aot_module_Steamworks_NET_info`? — dots → underscores; verify).
- Whether the translation script handles every directive in the FNA .s (validate in Step 1).
- Whether the PLT fallback to the interpreter works on UWP (validate in Step 4/5).
- Whether AOT pinvoke wrappers resolve __Internal correctly (validate in Step 5 via SteamAPI_Init).
- The MSIX deploy time for a 40-50 MB package (measure in Step 5).
- Whether `full` (the AOT compiler flag) is required or whether plain `static` suffices (the `full` flag affects which methods/wrappers the compiler emits; test both in Step 2 and compare the method counts).

## Appendix: evidence log (probes run 2026-11-21 02:04-02:07 EST, all in /tmp, no tree builds)

- `mono --version` → 6.8.0.105 (Debian 6.8.0.105+dfsg-3.6ubuntu2).
- Probe assembly (p.exe, 3 methods): `mono --aot=static` → Compiled 3/3, ELF .o 6,624 B. `mono --aot=static,interp` → Compiled 0/0. `mono --aot=static,mtriple=x86_64-pc-windows-msvc` → Compiled 3/3, ELF .o (host `as` used). `mono --aot=static,interp,mtriple=...,asmonly` → Compiled 0/0, .s 4,813 B (GAS syntax, empty code sections).
- FNA core: `mono --aot=static Microsoft.Xna.Framework.dll` → Compiled 4380/4380, .o 4,122,256 B, symbol `mono_aot_module_Microsoft_Xna_Framework_info`, JIT 468ms / Gen 190ms / Asm+Link 444ms.
- clang: `clang -target x86_64-pc-windows-msvc -c p.exe.s` → errors on `.subsection`, `.hword`, `.text 0`, `.local` (with and without nodebug).
- MinGW as: `x86_64-w64-mingw32-as --64` → "unknown pseudo-op: `.local`", `.type`/`.size` "junk at end of line". Via `tool-prefix=x86_64-w64-mingw32-` → "junk at end of line, first unrecognized character is `-'" at line 656.
- objcopy: `objcopy -O pe-x86-64 p.exe.o p.exe.coff.obj` → valid COFF (35 symbols, 6 sections, `mono_aot_module_p_info` present) BUT R_X86_64_PC32 → IMAGE_REL_AMD64_ADDR32. Disassembly: `44: 49 8b 05` mov rax,[rip+disp] at P_Main+0x4, reloc at 0x47 → ADDR32 breaks it. REJECTED.
- LLVM: `mono --aot=llvm` → "--aot=llvm requires a runtime compiled with llvm support."
- Toolchain present: x86_64-w64-mingw32-as/-ld/-gcc, llvm-mc, llc, objcopy, clang 18.
- IL sizes: Celeste.exe 2,570,752 B; Steamworks.NET.dll 265,216 B; mscorlib.dll 4,811,264 B; System/System.Core/System.Xml are GAC symlinks in /usr/lib/mono/4.5/.
