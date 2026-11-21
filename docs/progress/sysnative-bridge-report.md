# System.Native host bridge — report (.128 implementation, shipped in .129)

Date: 2026-11-20 ~23:26–23:50 EST. Implementer: WooWop subagent (System.Native specialist).
Status: IMPLEMENTED + VERIFIED COMPILING (UWP TU + Linux gate). Console test pending (parent test agent owns the console lane).

## TL;DR

Celeste died in .127 with `DllNotFoundException: System.Native` (first file-IO
call, via `Interop+Sys..cctor`). The fix is a host-side bridge:
`src/trt/bridge_system_native.cpp` implements the full System.Native surface
(46 native entry points) as real Win32/UWP calls where a real implementation
exists, plus named fail-visible stubs where no console equivalent exists.
Exports are wired through `host-bridges.def`; the dllmap
`System.Native -> __Internal` was already in the UWP assembly-load hook
(`mono_host.cpp`, UWP-only — Linux gate untouched).

**Coordination note (read first):** a parallel agent (opencode) built .128
first with a MINIMAL version of this bridge (26 exports, skeleton file,
packed 23:36, holds the console lock as `opencode-128`). This report covers
the COMPLETE implementation, which is in the tree and compiled into the
.129 build that was running when this was written. See "Coordination" below
for the .128/.129 situation and the recommendation.

## What was implemented

File: `~/tiramisu-brown-merge/src/trt/bridge_system_native.cpp`
(1024 lines, UWP-only via `#if defined(_WIN32)` — empty TU on Linux).
Table: `src/trt/sysnative_errno_table.inc` (generated, see below).
Exports: `third_party/mono-bridges/host-bridges.def` (+20 entries; 46 total
System.Native names). No Mono rebuild, no dllmap change needed (already in
place), no git push.

### Real Win32/UWP implementations (24)

| Function | Implementation |
|---|---|
| Stat2 / LStat2 | `GetFileAttributesExW` + best-effort `CreateFile2` handle for Dev/Ino. Full FileStatus fill: Mode (S_IFDIR/S_IFREG/S_IFLNK + 0755/0644, read-only aware), Size, ATime/MTime/CTime/BirthTime with nsec, Flags=HasBirthTime, UF_HIDDEN from FILE_ATTRIBUTE_HIDDEN. LStat reports reparse points as S_IFLNK; Stat follows. |
| FStat2 | `GetFileInformationByHandle` on the MonoIO HANDLE (SafeFileHandle marshals as its HANDLE value). Same fill as Stat2. |
| Open | `CreateFile2`; Mono OpenFlags decoded (O_RDONLY=0/O_WRONLY=1/O_RDWR=2, O_CREAT=32, O_EXCL=64, O_TRUNC=128 — Mono PAL values, NOT Linux). Full share mode. Returns HANDLE as intptr. |
| Close | `CloseHandle`. (Called from SafeFileHandle.ReleaseHandle; must really close.) |
| Read / Write | `ReadFile`/`WriteFile` on pipe HANDLEs (System.Core pinvoke, NOT the System.dll Sys.Read icall). Broken pipe on read = EOF (0). |
| CopyFile | Real ReadFile/WriteFile loop, current position to EOF. Implemented for real because `File.Copy` has NO managed fallback — CheckIo throws on failure (verified in IL). |
| OpenDir / ReadDirR / CloseDir | `FindFirstFileW`/`FindNextFileW`/`FindClose` with a heap `DirState`. ReadDirR contract verified vs real lib + IL: 0 = entry, -1 = end of dir, +errno = error. NameLength = real UTF-8 byte length (managed GetName also accepts -1 = scan 256 bytes, but real length is correct for long names). InodeType DT_DIR/DT_REG/DT_LNK. |
| GetReadDirRBufferSize | 4096 (real Linux value is 287; 4096 covers long Windows names as UTF-8). |
| MkDir / RmDir / Rename / Unlink | `CreateDirectoryW` / `RemoveDirectoryW` / `MoveFileExW(REPLACE_EXISTING)` / `DeleteFileW` (Unlink returns EISDIR for directories). |
| UTime / UTimes | `CreateFile2(FILE_WRITE_ATTRIBUTES)` + `SetFileTime` (Unix sec/usec -> FILETIME). |
| ChMod | Best-effort: no write bits -> FILE_ATTRIBUTE_READONLY, else clear it. (Unix permission bits have no fuller Windows equivalent.) |
| StrErrorR | Static table of 33 POSIX messages + "Unknown error %d" fallback. GNU semantics (returns buf). |
| GetNonCryptographicallySecureRandomBytes | `BCryptGenRandom` (bcrypt.lib already linked). xorshift fallback only if BCrypt fails (essentially never). |
| GetEGid / GetEUid | Return 0. |
| GetHostName | `GetComputerNameW` (real). Note: GetComputerNameExW is DESKTOP-partition; GetComputerNameW is APP. |
| GetDomainSocketSizes | Real Linux constants measured from libmono-native.so: (2, 108, 110). |
| ConvertErrorPlatformToPal / ConvertErrorPalToPlatform | Measured table (see below) + reverse linear search. |
| FcntlSetCloseOnExec | Returns 0 (no-op success): Win32 handles are non-inheritable by default, so close-on-exec already holds. |
| FcntlCanGetSetPipeSz | Returns 0 (capability probe, like LChflagsCanSetHiddenFlag). |
| LChflagsCanSetHiddenFlag | Returns 0 (false). The `Interop+Sys..cctor` calls ONLY this (verified in IL) and must succeed. |
| mono_pal_init | No-op (void). System.dll's `Sys..cctor` calls ONLY this (verified in IL) and must succeed. |

### Fail-visible stubs (12) — each logs once, then -1 + errno

| Function | errno | Reason |
|---|---|---|
| ReadLink | EINVAL (22) | No symlinks on console; EINVAL is what readlink returns for a non-link, and the managed CopyDanglingSymlink check depends on it. |
| Symlink / Link | ENOSYS (38) | No console equivalent. |
| LChflags | ENOSYS (38) | No chflags on Windows. |
| INotifyInit / INotifyAddWatch / INotifyRemoveWatch | ENOSYS (38) | No inotify; FileSystemWatcher correctly fails when enabled. INotifyInit returns -1 (invalid SafeFileHandle). |
| FLock | ENOSYS (38) | No flock; result is discarded by SafeFileHandle.ReleaseHandle (LOCK_UN) anyway. |
| Poll | returns Error ENOSYS (65591) | Returns an Interop/Error directly, not -1+errno. FileSystemWatcher-only. |
| Pipe | ENOSYS (38) | UNSOUND to implement: fds are int32 but HANDLEs are 64-bit (truncation). Mono's Windows pipe code never calls this (Unix-only path). |
| GetPeerID | ENOSYS (38) | Unix domain socket credentials; no equivalent. |
| GetPeerUserName | NULL | Returns NULL (marshals as null string; the marshaler never frees NULL — returning a static "" would be freed -> crash). |
| SetEUid | ENOSYS (38) | No setuid on Windows. |
| FcntlGetPipeSz / FcntlSetPipeSz | ENOSYS (38) | No pipe-size fcntl on Windows. |

## Layout sources (all measured, not guessed)

- **Signatures**: `monodis --implmap` on the packaged framework assemblies
  (byte-identical to /usr/lib/mono/4.5/: mscorlib md5 772d5673…,
  System 9dfa0805…). mscorlib 28 + System.dll 19 + System.Core 27 entries
  = 46 unique natives. System.Core (Unix sockets + pipes) was found by
  scanning ALL 12 packaged DLLs, not just the two named in the brief.
- **Struct layouts**: C# reflection probe (`Marshal.OffsetOf`/`SizeOf`) run
  under system Mono 6.8.0.105. FileStatus size 112 (offsets as in the code's
  static_asserts); DirectoryEntry size 16; UTimBuf 16; TimeValPair 32
  (flat ASec/AUSec/MSec/MUSec — no TimeVal struct exists); PollEvent 8
  (fd int32 @0, Events/TriggeredEvents int16 @4/@6).
- **Error enum**: Mono's OWN numbering (0x10000 + alphabetical index), NOT
  Linux errno values. E.g. ENOENT = 65581, ENOSYS = 65591, EINVAL = 65564.
  Identity mapping would be WRONG.
- **errno -> PAL table**: measured live from the real
  `/usr/lib/libmono-native.so.0.0.0` by dlopening it (with the repo's
  libmono-2.0.so.1 preloaded for `mono_add_internal_call_with_flags`) and
  calling `SystemNative_ConvertErrorPlatformToPal` for errno 0..135.
  Generated into `src/trt/sysnative_errno_table.inc` by script (no hand
  transcription). Unmapped errnos -> 131071 (0x1FFFF), matching the real
  implementation. Table verified injective (excluding the sentinel), so the
  reverse lookup is well-defined. Reverse spot-checks match the real lib:
  65581 -> 2, 65591 -> 38, 0 -> 0, unknown -> -1.
- **ReadDirR contract**: live-probed against the real lib (0 per entry,
  -1 at end of dir with Name=NULL, NULL+errno=2 for missing dir) AND
  verified against the managed callers' IL (`FileSystemEnumerator`:
  -1 = DirectoryFinished, 0 = entry, +errno = ErrorInfo; `TimeZoneInfo`:
  non-zero = done). Real lib returns NameLength=-1; managed GetName
  treats -1 as "scan 256 bytes for NUL" — this bridge sets the real
  length instead (correct for long names).
- **.cctor dependencies**: mscorlib `Sys..cctor` calls ONLY
  `LChflagsCanSetHiddenFlag`; System.dll `Sys..cctor` calls ONLY
  `mono_pal_init`. Both return success here.

## The Read question — RESOLVED (MonoIO covers reads, no icall needed)

Evidence (IL disassembly of the packaged mscorlib/System.dll):

1. `FileStream::ReadData` calls
   `MonoIO::Read(SafeHandle, byte[], int32, int32, out MonoIOError)` —
   a runtime internal call implemented in w32file.c (present in the UWP
   lib). FileStream reads NEVER go through `Interop/Sys::Read`.
2. `Interop/Sys::Read` is referenced NOWHERE in mscorlib (no memberref,
   no IL call). In System.dll it has exactly 2 call sites, both in
   `System.IO.CoreFX.FileSystemWatcher` (inotify event reads):
   `Sys::Read(SafeFileHandle,...)` wrapper + `RunningInstance::TryReadEvent`.
3. FileSystemWatcher cannot start on the console anyway: `INotifyInit`
   fails visibly (ENOSYS), so the inotify handle is never valid.

Conclusion: no `mono_add_internal_call("Interop/Sys::Read", ...)`
registration. The System.Core `SystemNative_Read` PINVOKE (different
method — anonymous pipes) IS implemented for real with ReadFile.

## Errno discipline (the sharp edge)

- The managed layer reads failures via `Marshal.GetLastWin32Error()` after
  a `lasterr` pinvoke, then `ConvertErrorPlatformToPal` (indexed by LINUX
  errno numbers). MSVC's errno.h DIFFERS (ENOSYS 40 vs Linux 38, ENOTEMPTY
  41 vs 39, ENAMETOOLONG 38 vs 36, ELOOP 42 vs 40), so the bridge NEVER
  uses CRT errno macros — it uses explicit Linux numbers (`L_*`).
- Every failure sets BOTH `errno` and `SetLastError()` to the Linux value
  (Mono's Windows wrapper captures GetLastError; errno covers the CRT path).
- OpenDir failure returns NULL (managed checks IntPtr.Zero); Open /
  INotifyInit failure returns -1 (SafeFileHandle invalid = -1).

## UWP API notes (found by compiling, not by guessing)

- `CreateFileW` is DESKTOP-partition in the SDK headers (undeclared under
  WINAPI_FAMILY_APP). The bridge uses `CreateFile2` (APP-partition) via
  an `open_file2` helper. (The first version used CreateFileW and failed
  to compile — caught by the single-TU UWP check, not by the full build.)
- `GetComputerNameExW` is DESKTOP-partition; `GetComputerNameW` is APP.
- Every other Win32 call in the bridge compiled clean under the exact
  UWP crossbuild flags (clang-cl, WINAPI_FAMILY_APP, /FI msvc-compat.h +
  faudio_uwp_compat.h, all build-host.sh defines).
- Long paths: `wpath()` normalizes `/` -> `\` and prepends `\\?\` to
  absolute drive paths >= 240 chars (buffers are 4096 wchar_t; overflow
  -> ENAMETOOLONG). Strings are converted as UTF-8 first (matches Mono's
  marshaling + the ReadDirR round-trip), CP_ACP fallback.

## Verification

### Linux gate: PASS (with this bridge in the tree)
The .129 build's gate (out-brown-129/obj/test.log) ran with the new
`bridge_system_native.cpp` in the tree: 1447 PASS lines, no failures
(the "FAIL" matches are expected negative-test log lines, each followed
by a PASS). The bridge TU compiles to an empty object on Linux
(`#if defined(_WIN32)` guard; verified directly with the gate's own
clang++ command line). The Linux Mono path is unaffected: the
System.Native -> __Internal dllmap exists ONLY in the UWP hook.

### UWP single-TU compile: PASS (clean, zero warnings/errors)
`bridge_system_native.cpp` compiled under the exact UWP crossbuild flags
(clang-cl 19.1.7, xwin SDK, WINAPI_FAMILY_APP). Object: 46,319 bytes.
`llvm-nm`: all 46 symbols defined (45 SystemNative_* + mono_pal_init).

### Full-build export check: PASS (0.2.2.129, built 23:48 EST)
The .129 build (sibling's run, current tree) completed: gate PASS, UWP
link OK, `out-brown-129/layout/tiramisu.exe` (7,768,064 B, 23:48:43) +
`out-brown-129/Tiramisu.msix` (12,485,940 B, 23:48:52, manifest
0.2.2.129). `llvm-readobj --coff-exports` on the exe: needed 46 /
exported 46 / missing 0 / extra 0, diffed programmatically against the
implmap-derived required set from all three framework assemblies
(mscorlib + System + System.Core). The bridge TU object in the build is
`out-brown-129/obj/src_trt_bridge_system_native.2932255887.obj`.

### NOT verified (console-only, for the test agent)
- That Mono's interpreter pinvoke wrapper captures the last error
  correctly on UWP (if File.Exists throws instead of returning false for
  a missing file, the wrapper — not the bridge — is the suspect; the
  bridge sets both errno and SetLastError).
- That `CreateFile2` with `FILE_FLAG_BACKUP_SEMANTICS` + 0 access opens
  directory handles inside the AppContainer (used only for Dev/Ino;
  failure degrades to Dev=Ino=0, Stat2 still succeeds).
- End-to-end: Celeste past File.Exists into content loading.

## Coordination (.128 vs .129 — the parent must read this)

Timeline (all 2026-11-20 EST):
- 23:25 — opencode writes a MINIMAL bridge_system_native.cpp (99 lines,
  26 exports, skeleton) and bumps the manifest to 0.2.2.128.
- 23:26:52 — this task starts (build the bridge + .128).
- 23:35 — opencode logs ".128 BUILT" (skeleton bridge), packs .128
  (12.48 MB MSIX in out-brown-128/), and takes the console lock
  (`opencode-128`, 23:37:08Z) to deploy/test it.
- 23:38 — this agent's COMPLETE bridge + .def lands in the tree.
- 23:39 — a build of out-brown-129 starts (manifest bumped to 0.2.2.129;
  mono_host.cpp +14 lines: exception-message logging in the unhandled
  hook — complementary, no conflict). That build compiles the CURRENT
  tree, i.e. it INCLUDES this complete bridge.

Why no second .128 was built: out-brown-128/Tiramisu.msix already exists
and opencode holds the console lock deploying/testing it — overwriting
it mid-deploy could break their test, and two packages with the same
version (0.2.2.128) cannot be distinguished on the console anyway.
The .129 build is the natural test vehicle for the complete bridge.

Gaps in opencode's .128 skeleton vs this implementation (for the test
agent to know what .128 does NOT cover):
- File-IO core stubbed ENOSYS: OpenDir/ReadDirR/CloseDir, MkDir, RmDir,
  Rename, Unlink, UTime/UTimes, ChMod, FStat2 (returns ENOSYS + zeroed
  struct), ReadLink, CopyFile. So .128 can do File.Exists (Stat2) but
  NOT directory enumeration, file creation/deletion, or File.Copy.
- No System.dll entries (Open/Close/FLock/Poll/INotify/mono_pal_init):
  System.dll's Sys..cctor calls mono_pal_init — MISSING from .128, so
  anything touching System.dll's Sys (e.g. FileSystemWatcher paths)
  throws DllNotFoundException on .128.
- No System.Core entries (12 pipe/socket natives missing).
- Stat2 fills NO times (all zero) — File.GetLastWriteTime etc. return
  epoch on .128. No Dev/Ino, no UF_HIDDEN.
- GetNonCryptographicallySecureRandomBytes is FAKE (QueryPerformanceCounter
  bytes, deterministic-ish) — this build uses BCryptGenRandom.
- GetEGid/GetEUid return -1 + ENOSYS (should return 0).
- ConvertError* return -1 + ENOSYS (should map via the measured table):
  on .128, error reporting after a failed Stat2 produces Error -1
  (invalid) instead of e.g. ENOENT — File.Exists may misbehave.
- No-arg signatures on most stubs (e.g. `int OpenDir()`) — works by
  accident on x64 (args ignored) but is UB.

RECOMMENDATION: test .129 (complete bridge), not .128 (skeleton). If .128
must be re-tested, rebuild it from the current tree AFTER the .129 build
finishes and the console lane frees up (manifest flip .129 -> .128 for
the pack step only, then flip back; build into a separate out dir to
avoid clobbering opencode's artifacts — do NOT overwrite
out-brown-128/Tiramisu.msix while opencode holds the lock).

## Deliberately left out

- `Interop/Sys::Read` icall registration — not needed (see Read question).
- Real `Pipe` — unsound (int32 fd vs 64-bit HANDLE truncation); Unix-only
  path, never called on Windows.
- Real symlink/hardlink/inotify/flock — no console equivalent; fail-visible.
- `GetPeerUserName` returns NULL (see stub table for why "" would crash).
- No changes to the Mono lib, patches, dllmap table, or pack script.
- Nothing pushed to git; all changes are working-tree only in
  ~/tiramisu-brown-merge (bridge_system_native.cpp [new content],
  sysnative_errno_table.inc [new], host-bridges.def [modified]).

## Probes / evidence (on mineplex, /tmp — ephemeral)

- /tmp/probe.out, /tmp/probe2.out — reflection layout dumps.
- /tmp/errno-probe2.out — measured errno->PAL table (0..135).
- /tmp/readdir-probe.c + output — live ReadDirR/Stat2 contract probe.
- /tmp/mscorlib.il, /tmp/system.il — full IL dumps (37MB + 25MB).
- /tmp/mscorlib.implmap, /tmp/system.implmap — implmap dumps.
- /tmp/sysnative-uwp.obj — UWP-compiled bridge TU (46 symbols).
- /tmp/gen-errno-table.py — table generator (rerun to regenerate the .inc).
