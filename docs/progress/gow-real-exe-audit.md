# God of War authentic-executable audit

Updated: 2026-11-20

## Current verdict

The latest Xbox attempt selected the Tiramisu library entry God of War → GoW.exe. Tiramisu reports guest unhandled exception 0xC0000005; it did not reach the main menu or gameplay. This is an attempted guest launch, not a forest-render fallback.

The original executable's byte-for-byte provenance is not yet confirmed. The installed Tiramisu app reports the game as storage: local, and the persistent source branch hardcodes its game root to UWP LocalState games. The PC's read-only SMB/SSHFS source share is now available at \\\\10.0.0.97\\tiranetworkprobe\\NetworkPlay\\network-games\\God of War, but the current app is not selecting that share.

## Evidence captured

- Xbox package listing reports main Tiramisu 0.2.2.83; the persistent source worktree is commit ae2091a, manifest 0.2.2.79. The exact installed source for 0.2.2.83 is not present in the persistent checkout.
- Tiramisu GET /games returned God of War, executable GoW.exe, storage local, plus two crash helper exes.
- The UI showed God of War: guest unhandled exception 0xC0000005 after selecting that entry. No title/menu/gameplay was visible.
- The PE host code in src/host/main.cpp builds the path from the game root and selected exe, reads the PE into memory, calls trt_map_pe, invokes its TLS callbacks, then calls the mapped image entrypoint through trt_call_guest_img under SEH. The displayed guest-exception result is consistent with reaching the mapped guest entrypoint path; it does not by itself prove the local PE bytes are the original GoW binary.
- Guest memory report at the failed attempt: app usage about 1,727/5,120 MiB; guest budget about 40/5,120 MiB committed, 296 MiB peak, zero refusals. The 5 GiB UWP cap was not reached.
- The current /api/logs tail contains repeated guest-memory lines and two host CreateFileW failures, but no launch path, fault PC, or stack frames. Device Portal crash-dump listing was empty.
- The original source directory on Vermes is mounted read-only over SSHFS and exposed by Samba. Metadata-only check of its GoW.exe: 19,833,344 bytes, dated 2022-06-01. No executable contents were read.
- Four multi-gigabyte texture-pack uploads to the Xbox LocalState endpoint failed with connection resets (3.52, 4.06, 6.94, and 7.25 GB). This establishes the LocalState game copy is incomplete; it does not establish whether GoW.exe itself was among earlier successful uploads.
- The SMB share is verified from the PC with smbclient; Xbox-side access and game launch from that share have not been verified.

## Runtime audit

- PE loading, section mapping, relocation fallback, TLS callback invocation, unwind metadata registration, guest exception interception, and UWP-backed allocation hooks exist in the persistent source.
- The host implements many Win32/DX compatibility bridges, including kernel32, user32, D3D/DXGI, GameInput/XInput, and memory-budget APIs. Presence of a bridge is not evidence that the required GoW call semantics are complete.
- The 0xC0000005 fault has no address or caller stack in the currently retrievable logs, so the earliest failing game subsystem is not yet identified. Do not attribute it to D3D, memory pressure, or missing assets without a PC/stack/module trace.
- The main UWP source root is LocalState games; the current installed build is not using the newly restored PC SMB share. The network source mount is a ready transport path, not proof that the game reads it.

## Next steps

1. Recover/source-control the exact 0.2.2.83 source (or explicitly rebase the known 0.2.2.79 line before replacing the installed package).
2. Ensure the selected entry records resolved path, size, PE timestamp, image base, entry RVA, import resolution summary, TLS callbacks, and the first-chance fault PC plus stack before memory polling can bury the log.
3. Compare only executable metadata for the LocalState GoW.exe against the mounted original; do not dump or decompile game contents.
4. Make the launcher select the original mounted game tree or complete a reliable transfer of all required files. Current storage: local proves it is not using the SMB source.
5. Deploy an instrumented build, capture the exact module/PC/stack and first failing game subsystem, then fix that first blocker.
6. Preserve the forest scene as a separate regression check; do not use it as evidence of menu or gameplay.

## Milestones

- App reaches Home and lists God of War: verified.
- Selected GoW.exe guest attempt: verified; exits with 0xC0000005.
- Original executable provenance for the local selected copy: unverified.
- Original main menu, controller input in-game, gameplay, save/load: not achieved.

## 2026-11-20 diagnostic follow-up

- A live log capture was started before a POST to /api/launch/God%20of%20War. The endpoint replied "launching God of War", but during the next four seconds running_game stayed empty and the log acquired no lines. This is only an accepted request, not evidence that a second guest launch started. Capture: docs/progress/gow-live-capture.log.
- In the recovered 0.2.2.79 source, src/host/main.cpp now records mapping result, guest base, entry RVA, and each TLS callback entry/return, and samples running memory every 30 seconds instead of every two. This is diagnostic instrumentation, not a fix for the access violation.
- The diagnostic candidate builds separately in out-gow-trace. The installed version is 0.2.2.83, whose exact source remains unavailable; do not overwrite it with this older candidate without reconciling the version gap and preserving the current installed functionality.

## Full log recovered and diagnostic build, 2026-11-20

- Device Portal /api/filesystem/apps/file retrieved the complete 12,558,326-byte LocalState log to docs/progress/xbox-tiramisu-full.log. Local game GoW.exe metadata is 19,833,344 bytes, matching the source file size; content identity is not established.
- A GoW launch in the 0.2.2.82 session mapped the selected GoW.exe with image base 0x23baac00000, image size 0x519f000, entry RVA 0x3ec680, one TLS callback and 51,869 registered unwind entries. It reached mapped guest code and failed with a null read at GoW.exe+0x4b113b. Callers: GoW.exe+0x40bf6a, +0x40a983, +0x3ffaa7, ucrtbase.dll+0x707d6, GoW.exe+0x3ec520. The last host API log was SHGetKnownFolderPath returning C:\users\player\Saved Games. This is an ordering observation, not proof of causation.
- During mapping, the local steam_api64.dll failed to load because steamclient64.dll!p was unresolved, after which the log says nine steam_api64 imports were bound. The recovered source may route Steam to builtins but cannot establish the exact 0.2.2.82 build behavior or whether this is the null-read cause.
- The GoW asset directories are under exec, including exec/wad, cinematics, dc, languages, and sound. Some are populated. The failed large texture transfers still require a file-by-file completeness check.
- The isolated diagnostic build completed successfully (exit 0), including offline loader suite; signed standalone package GoWDiag.msix uses a distinct identity and passed signature verification.
- Xbox Device Portal accepted the separate install request with CSRF token, then returned 0x80070070, insufficient disk space. No diagnostic installation occurred. LocalState/incoming, LocalCache, and TempState list empty. Do not delete game data or unrelated apps without first establishing a safe recovery path. No main menu or gameplay reached.

## Storage cleanup, 2026-11-20

Xbox LocalState exec/wad/pc_le/010_forest_010.texpack listed 2,973,364,224 bytes; source SSHFS copy is 7,251,508,848 bytes. The Xbox copy is incomplete and requires replacement. The user authorized freeing space; attempt removal through Device Portal. Source: /home/opc/GOW/GodOfWar/exec/wad/pc_le/010_forest_010.texpack on vermes. Preserve record until restored and verified.

- Removed the truncated Xbox 010_forest_010.texpack (2,973,364,224 bytes) after confirming the source copy exists (7,251,508,848 bytes). Xbox version must be restored before gameplay; no complete copy was deleted. The diagnostic app then installed successfully as Xan.Tiramisu.GoWDiag_0.0.1.0_x64__x81jywntpvgne.
- Started the separate app and uploaded the source GoW.exe and root DLLs; selected GoW.exe from its library. Device Portal log is docs/progress/gow-diag-xbox.log. The app connection reset during import mapping, before the new entrypoint markers. The last visible sequence is repeated alternating loads of libSceGnm.dll and libSceGpuAddress.dll. The diagnostic sandbox lacked game assets, and this build uses 0.2.2.79-derived code; its result should not be equated with the earlier 0.2.2.82 null fault.
- Added a fail-visible per-thread cyclic DLL import guard in src/trt/images.cpp. It does not implement Windows two-phase cyclic imports, so the correct outcome for such a dependency is an import failure, not gameplay. A separate build is underway in out-gow-cycle; do not claim tested until the build and Xbox run complete.

## Storage and transfer audit, 2026-11-20

- Spider Launcher documents USB or local Wi-Fi folder imports and Steam downloads, plus validation and resumable transfers. It requires adequate free storage and a separate matching runtime. Source: https://github.com/BBCPrince/UWP-Spider-Launcher/blob/main/docs/GETTING-STARTED.md
- Read-only metadata comparison of Vermes source vs Xbox LocalState (not a hash check): 33,106 source files, 66,339,671,134 file bytes; 32,543 same-size files totaling 15,436,968,076 bytes; 529 missing files totaling 26,606,570,130 bytes; 34 different-size files whose source versions total 24,296,132,928 bytes. Some different files may be mutable data. The removed partial forest pack is among missing files.
- Xbox Device Portal confirms D:\DevelopmentFiles\Tiramisu\games exists. The captured 0.2.2.83 session previously loaded a small guest executable from that folder. Free capacity, full-file transfer, and GoW selection from that location are not yet verified.
- Found src/host/webxfer.cpp stream_to_file ignored fwrite and fclose errors, incrementing counters by bytes received even if disk writes failed. This can falsely report success on full storage, but no claim is made about the exact cause of any earlier truncated pack.
- Changed stream_to_file to count written bytes and report open/write/close errors. Folder uploads stage to a unique temporary file and rename only after completion, preserving a prior file if a transfer fails. On Windows a rename over an existing destination may fail safely rather than replace it; replacement/resume support still needs implementation and Xbox testing. Existing partial files are not automatically repaired.
- Added transfer tests for an interrupted replacement and forced file-size-limit write failure. Focused suite passed 29/29 on the PC. The UWP build runs in out-gow-transfer; check docs/progress/gow-transfer-build.rc before use. No storage or upload change has yet been deployed on Xbox.
- The out-gow-transfer Xbox UWP build completed with exit code 0 and offline loader checks passed. It is not deployed; the recovered source is older than the main Xbox install. The transfer changes still require an Xbox-specific replacement test and resumable uploads.

## Xbox memory-cap probe, 2026-11-20

- Built and signed the opt-in memory probe as isolated diagnostic package Xan.Tiramisu.GoWDiag_0.0.1.1_x64__x81jywntpvgne, installed via Device Portal and launched normally outside a debugger. Main Tiramisu package was not replaced.
- Xbox log: memory probe: before=5120 MiB expected=5120 MiB request=8192 MiB accepted=0 after=5120 MiB. The documented TrySetAppMemoryUsageLimit request for 8 GiB was rejected and did not raise the limit. This result is for this API and this launch; it does not prove every alternative impossible.
- Removed the opt-in LocalState marker after the test. The instrumentation runs only when that marker exists.
- Microsoft documentation says the normal resource limits do not apply when running under the Visual Studio debugger, but this older documentation has not been tested against this console/OS, and such a mode is not evidence of ordinary gameplay viability: https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation
- Future work should measure game and GPU resource peaks, maintain headroom, and stream/evict/reuse resources based on actual pressure. Do not attribute the prior GoW null-pointer fault to memory capacity without evidence.

## Memory step probe and Linux debugger investigation

- Built and deployed separate GoWDiag 0.0.1.2; normal launch outside debugger. Expected and actual caps were 5120 MiB. TrySetAppMemoryUsageLimit requests of 5121, 5376, 5632, 6144, 7168 and 8192 MiB all returned false and all read back 5120 MiB. Opt-in marker was removed after testing. This tests the supported request API, not every possible architecture.
- PC is Linux, has qemu-system-x86_64 and accessible KVM, but only about 12 GiB free and no discovered Windows ISO, Windows VM, or installed Visual Studio. Windows UWP debugging via Visual Studio is documented: https://learn.microsoft.com/en-us/visualstudio/debugger/debug-installed-app-package. The debugger exception is from older documentation and has not been demonstrated on this Xbox; PLM debug-mode controls do not themselves establish an increased memory cap.
- The game has not reached its menu. Game files remain incomplete, including the removed partial forest texture pack; the unmodified source remains on Vermes.

## Package debug class probe

- Built and installed GoWDiag 0.0.1.3 with an opt-in probe of the documented PackageDebugSettings COM class through CoCreateInstanceFromApp. Offline loader tests and UWP cross-build passed; signed diagnostic package install succeeded.
- Xbox log on normal launch: debug probe: activation=0x80040154 query=0x80040154 (class not registered/available). EnableDebugging was not called, and no debug setting was enabled. This result concerns activation from this UWP app, not a Visual Studio remote debugger attachment.
- Removed debug-probe.enable after the test. No increase in RAM was achieved.

## Fresh resolution trace, 2026-11-20

- Started installed main Tiramisu 0.2.2.83 and requested God of War via /api/launch. The guest completed too quickly for the one-second status poll, but the full LocalState log retrieved to docs/progress/gow-quality-latest.log confirms the local 19,833,344-byte GoW.exe was read, mapped and entered.
- The Xbox recorded the same null read at GoW.exe+0x4b113b, exception 0xc0000005, following SHGetKnownFolderPath. App memory immediately after mapping was 210/5120 MiB. No GoW swapchain, first guest frame, render resolution or graphics preset was recorded in this attempt.
- The 1920x1080 CreateSwapChainForHwnd lines in the earlier full log belong to RACCOIN, based on their preceding executable launch. They are not evidence for GoW's forest resolution.
- Project documentation describes four 256x144 forest previews, but these do not establish framebuffer size, quality preset, frame rate, or active GoW engine execution.

## Additional trace-package evidence, 2026-11-20

- Retrieved LocalState tiramisu.log files from installed GoWTrace75 and GoWTrace80-89 packages into docs/progress/xbox-traces. The older audit's statement that all 1920x1080 swapchain lines belong to RACCOIN applies only to the main-package log; the separately installed GoWTrace logs contain GoW.exe launch paths and 1920x1080 CreateSwapChainForHwnd requests.
- GoWTrace87 and GoWTrace88 report a CoreWindow render surface of 1280x720 and a guest first-frame message; versions 75 and 80-85 report 960x540 and guest first-frame messages. GoWTrace88 also records 1920x1080 texture allocations. None of this alone establishes the native internal rendering resolution, graphics quality preset, FPS, provenance of the screenshots or whether the GoW menu appeared. The first-frame message is not proof of gameplay.
- Recreated PC Samba path NetworkGoWTrace88 as a symlink to existing read-only NetworkPlay game source, and copied a small guest_hello.exe self-test there. smbclient metadata confirms the source GoW.exe is visible through that alias. Windows Device Portal launch of installed GoWTrace88_0.0.7.0 returned HTTP 400 with 0x80070002 (file not found). No new trace session or live FPS measurement was captured; exact original trace build/layout is not available in the persisted checkout.

## Loader cycle reproduction and guard, 2026-11-20

- GoWDiag 0.0.1.3 with the initial sibling-path guard still repeatedly mapped libSceGnm.dll and libSceGpuAddress.dll and its HTTP server stopped responding. Log saved as docs/progress/gow-cycle-test.log. The resolver's native override can call trt_load_pe directly and bypassed the sibling-only guard; base_name also incorrectly chose the last backslash even when a later slash existed.
- Moved the per-thread cycle guard to trt_load_pe, covering both resolver routes, and fixed mixed slash basename selection in src/trt/images.cpp. This detects and rejects cycles; it does not support them. Added mixed-separator regression checks in tests/test_libload.cpp and synthetic mutually importing PE DLLs in tests/test_cycle_loader.cpp and tests/test_cycle_loader.sh; focused tests pass. tools/build-test.sh now includes the cycle test.
- The UWP build out-loader-cycle-v2 completed successfully; signed distinct GoWDiag 0.0.1.4 was installed and launched. The Xbox opened the diagnostic LocalState copy of GoW.exe (19,833,344 bytes), then reported cyclic guest DLL import libSceGnm.dll and unresolved imports between libSceGnm.dll and libSceGpuAddress.dll. It returned TRT_ERR_IMPORT with the web server still responding. App usage after the failure was 127/5120 MiB. Full diagnostic session log: docs/progress/gow-cycle-v2-xbox.log.
- No GoW entry point, title screen, or gameplay was reached in the 0.0.1.4 test. The diagnostic LocalState copy has only GoW.exe and neighboring DLLs; assets remain absent from that diagnostic app. This is a distinct codebase from the installed main 0.2.2.83, which has a different recorded null-read failure after its own guest entrypoint.
- Real cyclic PE support requires mapping and relocating all dependency images before resolving cross-imports, exposing only valid in-progress exports, deferring TLS callbacks and DllMain until the graph is bound and sealed, and rolling back the entire partially loaded graph on any failure. The current fail-visible guard is not that implementation. Avoid publishing in-progress exports without rollback: another DLL could retain function pointers into an image that later fails and is unmapped.
- Original trace 88 executable and tiramisu_core.dll were staged under /tmp/tiramisu-storage.* and /tmp/tiramisu-gow-trace.* according to the previous agent session; those temporary paths no longer exist. Existing trace 88 package cannot launch because its network layout files are gone. The previously restored network game alias only recovers source game visibility, not the missing trace runtime.

## Cyclic loader recovery and early startup trace, 2026-11-20

- Recovered prior-session PE loader patch notes and applied the two-phase cyclic-import design in isolated worktree `/home/jewboy420/tiramisu-cycle-recovery` based on commit `ae2091a`; the main Xbox Tiramisu 0.2.2.83 package remains untouched.
- Added synthetic mutually importing DLL fixtures: both DLLs map, resolve each other's exports, and invoke each other's sealed exports from initialization; a missing later dependency rolls back the dependency graph. The full offline suite passes.
- GoWDiag 0.0.1.5 on Xbox loaded the real local `GoW.exe` and resolved the mutual `libSceGnm.dll` / `libSceGpuAddress.dll` imports. Before the next fix, GoW requested Saved Games using SDK GUID `4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4`; the bridge table contained the wrong GUID `4C5C32FA-BB47-4C5C-...`, returned E_FAIL, and GoW showed a permissions error. The app was at about 250/5120 MiB.
- Corrected the Saved Games GUID and synthetic fixture. GoWDiag 0.0.1.6 then returned `C:\users\player\Saved Games`; the permissions dialog disappeared. GoW progressed to an access violation at `GoW.exe+0x4b113b` reading address zero. App usage was 219/5120 MiB with zero allocation refusals.
- GoWDiag 0.0.1.7 added fault register capture and reproduced the same access violation. Registers included RAX=0, RBX=0, RDX=0; callers were `GoW.exe+0x40bf6a`, `+0x40a983`, `+0x3ffaa7`, `ucrtbase.dll+0x707d6`, `GoW.exe+0x3ec520`.
- GoWDiag 0.0.1.8 logged startup calls: `SteamAPI_Init -> 1 (auth-only)`, Saved Games lookup succeeded (flags 0x8800; non-null output slot and allocated buffer), then `SteamInternal_ContextInit(data=...)`; immediately afterward GoW faulted at the same null-read PC. No CoTaskMemFree line appeared before the fault. This ordering makes the Steam context path worth inspecting, but does not prove it caused the fault.
- The fault occurs before game D3D device/swapchain initialization, game rendering, or menu. The only D3D-ready line is Tiramisu's own UWP UI/swapchain setup. App memory is far below the 5 GiB cap, so this trace does not implicate the RAM limit.
- The 1.8 build and full offline gate passed; signed diagnostic package 0.0.1.8 was installed side by side and launched. Full Xbox log is `docs/progress/gow-cycle-recovery-xbox.log`. The source executable's contents were not inspected; its size matches the mounted source metadata, but byte identity remains unverified.
- Current task: GoWDiag 0.0.1.9 logs the Steam context initializer pointer, counter, and returned context, plus Steam interface requests. Do not change Steam authentication/ownership results or infer the next compatibility fix until that trace is captured.
- Still not achieved: authentic GoW title menu, in-game input, gameplay, saves/loads. The Xbox copy is incomplete; restore the removed forest texture pack and audit required assets before any gameplay milestone.

## Steam user ABI trace and 1.11 trial, 2026-11-20

- GoWDiag 0.0.1.10 loaded the real selected GoW.exe and reached Steam user context initialization. Runtime logs show the initializer ran and returned; it requested `SteamUser021`; the `ISteamUser.GetSteamID` vtable stub ran; the guest then faulted at the same null read, `GoW.exe+0x4b113b`, before game graphics initialization. Full log: `docs/progress/gow-cycle-recovery-xbox.log`.
- The bridge's direct `SteamAPI_ISteamUser_GetSteamID` export is declared as a one-argument `u64(void*)` and returns the 64-bit Steam ID in RAX. The vtable equivalent had instead taken a second `u64*` and written through it. The 1.10 trace showed a non-null value in that second register, but the ABI contract did not define it as an output pointer; writing through it could corrupt guest state.
- Corrected the vtable thunk to `u64(void*)`, removed the extra memory write, and retained call logging. This leaves the auth-only identity result unchanged. GoWDiag 0.0.1.11 is rebuilding and will be used to validate whether the null read moves.
- Not achieved: main menu, game renderer, controller input in GoW, or gameplay. The 5 GiB UWP cap is not implicated by the observed startup fault; measured usage remained about 230 MiB.

- GoWDiag 0.0.1.11 passed the offline loader suite, built as a valid Windows GUI PE, was signed and installed as a side-by-side diagnostic update, and launched on the Series S.
- The 1.11 runtime trace confirms the corrected `ISteamUser.GetSteamID(self)` thunk returned `0110000100000001` without writing through a second register, then GoW faulted at the same `GoW.exe+0x4b113b` null-read PC. App usage after image load was 230/5120 MiB; no game D3D device or renderer startup appeared. This disproves the extra write as the sole cause.
- Next diagnostic: log the initialized Steam context's first four 64-bit words and log calls/results from direct `SteamAPI_GetSteamID` and `SteamAPI_GetHSteamUser` imports. Do not infer a fix from the unchanged fault; preserve the 1.11 log for comparison.

- GoWDiag 0.0.1.12 passed the offline suite, built as a Windows GUI PE, was signed and installed side by side, then launched the real GoW.exe again. The context dump shows its first word changes from 0 to the returned `ISteamUser` pointer; `SteamAPI_GetHSteamUser` returns 1; the corrected `GetSteamID` returns the 64-bit ID. GoW still faults at the same null read before graphics startup. Log: `docs/progress/gow-cycle-recovery-xbox.log`.
- The TLS registration log reports GoW's static TLS block is 4,592 bytes at index 1025. Source inspection shows the UWP fallback for restricted `K32EnumProcessModules` preserves only graphics modules; it omits `ucrtbase.dll`, even though the fault's caller chain includes `ucrtbase.dll`. Whether module enumeration succeeded on this Xbox is not yet logged, so this is a testable TLS hypothesis, not a confirmed cause.
- Next: log module-enumeration outcome, native TLS slots for UCRT/VCRuntime, and GoW's allocated TLS block/slot before entry. Add fallback preservation for the CRT only if the trace shows the module-enumeration path leaves its slot absent.


## Skye recovery, GoWDiag 1.13 trace, and console activation failure — 2026-11-20

- Skye HTTPS bridge recovered. GoWDiag 0.0.1.13 LocalState log was refreshed at docs/progress/gow-cycle-recovery-xbox.log (11,180 lines before later app restarts).
- Confirmed authentic selected GoW.exe launch from the runtime log: mapped size 0x519f000, entry RVA 0x3ec680; imports include d3d11.dll, dxgi.dll, xinput1_4.dll, and bundled SIE libSce* modules. No executable contents were dumped or disassembled.
- GoW reached SteamAPI_Init, SteamInternal_ContextInit, SteamUser021, and ISteamUser.GetSteamID. Then it faulted with 0xc0000005 on a null read at GoW.exe+0x4b113b; callers were +0x40bf6a, +0x40a983, +0x3ffaa7, ucrtbase.dll+0x707d6, and +0x3ec520. No game D3D device, renderer, or menu was initialized.
- TLS hypothesis is ruled out as the current blocker: native module enumeration succeeded (count=108); ucrtbase.dll has no TLS directory; VCRuntime modules were absent natively but loaded as guest DLLs; GoW received a 4,592-byte static TLS block at guest index 1025. This is not evidence of a TLS fix.
- Memory after GoW image load was 215/5120 MiB, zero allocation refusals. The 5 GiB cap did not cause this null-read trace.
- GoWDiag 0.0.1.14 was packaged and the package list showed it installed. A subsequent large file transfer reached its port 9127 while that diagnostic app owned the port. The main app was restored afterward; future diagnostic builds use fixed port 9128. Some uploaded files were visible in diagnostic LocalState before the diagnostic package family was later removed; their survival after reinstall is unverified. Do not assume those files reached main Tiramisu LocalState.
- GoWDiag 0.0.1.15 adds callsite logging for the first 32 zero-return Steam stubs and binds web transfer directly to port 9128. tools/build-host.sh out-recovered-gow passed the offline suite and UWP cross-build; the signed MSIX verified. Device Portal package listing confirmed .15 installed.
- After the diagnostic install/update, the console returned to Dev Home. Device Portal launch requests for main Xan.Tiramisu_0.2.2.91, GoWDiag .15, and GoWTrace88 all returned HTTP 400 Failed to launch the application, error -2147219190 (0x8004090A). The Xbox showed no app running; crash-dump listing was empty. Restarting the console and removing stale GoWTrace75 and 80–86 did not restore app activation. The archived logs for trace75/80–89 remain in docs/progress/xbox-traces; trace87–89, main .91, and GoWDiag .15 remain installed.
- The package-manager deployment status previously returned -2147023880 (0x800703F8, registry I/O failure), although package listing showed .14 installed. After restart it later returned 404. The current barrier is Xbox package activation, before any guest launch; no fresh GoW test occurred after 1.13.
- Next: restore app activation from Dev Home/Device Portal without replacing main .91 or deleting game data. Once Tiramisu is running, verify the main app remains on 9127, launch GoWDiag .15 on 9128, then send one GoW launch and capture the first Steam zero-stub callsite. Preserve current game files and forest regression packages.
- Status remains: authentic GoW main menu, controller input, gameplay, and saves are not achieved.


- Follow-up after cleanup: removed only stale GoWTrace75 and 80–86 packages; trace87–89 remain. Reinstalled GoWDiag 0.0.1.15 from the verified MSIX. After a second console restart, Device Portal again returned HTTP 400 / 0x8004090A when launching main .91. The .15 and .88 launches produced the same error. No crash dump was generated. Device Portal now lists main .91 and GoWDiag .15 installed, but neither app is active. Their LocalState directory listings were empty or returned server errors, so the survival of files formerly transferred while GoWDiag owned 9127 cannot be confirmed. No game data was intentionally deleted from the main package.


## Tiramisu activation restored; game-control port blocked — 2026-11-20

- Skye HTTPS and Device Portal are reachable again. Device Portal accepted a launch of main Tiramisu 0.2.2.91 (HTTP 200).
- Captured the Xbox screen at the Tiramisu Setup page: all displayed checks are green and it reports the transfer service at http://10.0.0.23:9127.
- Device Portal port 11443 responds from the same PC, but TCP/HTTP connections to Tiramisu ports 9127 and 9128 time out. No game launch request or new GoW run could be sent. The cause remains unconfirmed; investigate port reachability/firewall/listener while keeping the main app active.
- No packages or game files were changed in this recovery attempt. No GoW menu/gameplay milestone is claimed.


## Port ownership check — 2026-11-20

- PC is 10.0.0.97/24 and Xbox is 10.0.0.23/24 on the same Wi-Fi subnet.
- PC UFW is default-deny inbound but allow-outgoing; Device Portal at 10.0.0.23:11443 works. TCP to Xbox ports 9127 and 9128 times out. No local PC process is listening on 9127/9128.
- Therefore opening PC inbound ports 9127/9128 would not fix this path: those listeners belong to the Xbox app, and PC outbound is already allowed. No unnecessary PC firewall exposure was added. The remaining blocker is Xbox-side listener/firewall reachability or the app's socket state.


## Follow-up restart attempt — 2026-11-20

- Tiramisu 0.2.2.91 was launched again through Device Portal (HTTP 200). Fresh screenshot shows Setup and green checks, including “Transfer server ready” at http://10.0.0.23:9127.
- Direct PC access to Xbox port 9127 still times out. PC is 10.0.0.97/24, Xbox is 10.0.0.23/24; PC UFW allows outbound. No PC inbound rules were added because they cannot open the Xbox listener.
- Device Portal briefly showed all apps stopped after a failed stop/relaunch; the subsequent launch restored Tiramisu. GoW was not launched.


## Zero-SteamID A/B run — 2026-11-20

- Built isolated source copy `/home/jewboy420/tiramisu-cycle-recovery-gow18` with a no-user SteamID diagnostic. The adapted auth-surface test and complete offline loader suite passed; UWP host build succeeded.
- Registered and launched side-by-side package `Xan.Tiramisu.GoWNetPlay18_0.0.1.18_x64__x81jywntpvgne` from the PC SMB share. This left the main Tiramisu package untouched and ran the real selected GoW.exe.
- Returning SteamID zero did not change the failure: UCRT initializer callback 6 still faulted at `GoW.exe+0x4b113b`, null read `0xc0000005`, with the same caller chain/register pattern. The app used about 253/5,120 MiB and reported no allocation refusals. No game D3D device, renderer, or menu was reached.
- Full Xbox runtime log: `/home/jewboy420/tiramisu-gow-skye/docs/progress/xbox-traces/gownetwork-0.0.1.18.log` (50,348 bytes; SHA-256 `e3d8908e173ced4836989780851b2dfc49277967613958dac53229170228a159`).
- Next: test the shim's unconditional `SteamAPI_Init -> true`. Valve's current Steamworks documentation says true means required interfaces were acquired and accessible, and false includes a non-running client or unidentified App ID. Since no Steam client is available here, the success return may be masking an unavailable-interface state. Test truthful failure as a diagnostic; do not claim progress unless GoW runtime evidence advances.
## No-Steam-client A/B run — 2026-11-20

- Isolated .19 package built and passed offline and UWP builds; registered/launched side-by-side from the PC share. Main Tiramisu package remains untouched.
- Real GoW.exe logged SteamAPI_Init() failed. when the diagnostic shim returned false, then raised breakpoint 0x80000003 at GoW.exe+0x3ff97f, before game graphics/menu.
- In .18, falsely returning Steam init success instead produced the null-read AV at GoW.exe+0x4b113b. The fake SteamID value is ruled out; the current auth-only shim does not provide the Steamworks state GoW expects.
- Memory was about 253/5,120 MiB with no refusals. No D3D device or menu.
- Full log: /home/jewboy420/tiramisu-gow-skye/docs/progress/xbox-traces/gownetwork-0.0.1.19.log (49,266 bytes, SHA-256 e768b64a3341c4237abb0d905ce33deab419416c742b6fc271d500cecd5dff9d).
- The runtime also reports CreateProcessW -> BLOCKED (expected); the UWP host cannot launch a Steam client. Do not fake subscription/ownership. A genuine Steamworks client or different architecture is required to continue this path.
