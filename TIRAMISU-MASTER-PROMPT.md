# Tiramisu universal runtime master prompt

## Mission

Work in the existing Tiramisu checkout through the authorized Skye Bridge. Build
a Windows game compatibility runtime for Xbox Dev Mode with the scope and
discipline of Wine and Proton. Given a complete, authorized Windows game
installation, normal Play should resolve its launch contract, mount it with
persistent writable state, execute game code, present the actual menu, and accept
input. Use the same shared runtime across titles and Xbox families.

The compatibility target is broad Windows game coverage. Track unsupported
APIs, architectures, packages, and hardware explicitly. Measure success with
the title and device, using the gates below.

## Work order for every session

1. Check the branch, active editors, and console availability. Preserve working
   changes and saved games. Build in isolated output directories and apply
   only targeted patches. Obtain authorization before committing, pushing,
   publishing, uninstalling, or replacing user data.
2. Read the current title matrix, known issues, support roadmap, and
   [universal runtime work map](docs/universal-runtime-work-map.md). Verify
   assumptions against the current Play path and the latest device evidence.
3. Reproduce the first failing stage in an authorized title. The 0.2.4.59
   Series S replay reached 1,594 successful D3D11 Presents for Cuphead.
   Readbacks and five screen captures remained black. Sekiro reached native entry before the
   dashboard became unreachable, with zero observed D3D11 device calls.
   Investigate the shared mechanisms behind those findings. Follow the user's
   current title and console test scope.
   [Series S replay](docs/proof/xbox-series-s-2026-11-27-runtime-059.json)
4. Write a focused failing regression, implement the shared contract, run the
   relevant offline and UWP gates, and repeat through Play. Continue into the
   next confirmed blocker after a green build. Preserve a verified fallback
   path when the host lacks a capability.
5. Save code, the measured before and after, and a small reproducible launch
   report. Record the first failing operation and its status, the remaining
   unverified stages, and what evidence would distinguish the next causes.

## Xbox compatibility targets

Use documented Win32 behavior and conformance fixtures as the baseline for
the guest runtime. Use the actual Xbox hardware profile as the baseline for
presentation, device input, audio and memory. Study upstream behavior in its
loader tests and component manifests before selecting a provider.
[Wine loader](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/loader.c)
[Proton component pins](https://github.com/ValveSoftware/Proton/blob/proton_10.0/.gitmodules)
[Xbox UWP resource limits](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)

Compare releases on the same game builds, console model and settings. Track the
share of authorized installs that reach a visible menu and accept a controller
action, cold and warm time to menu, peak memory, controller reconnect, and clean
exit followed by relaunch. Record pass, fail and untested counts for each
architecture, engine, graphics API and console model. Use these measurements to
evaluate improvements to the Xbox experience.

## The shared Play pipeline

### Install and mount

Accept user-owned local installs and authorized store installs. Resolve the
actual app ID, build, branch, architecture, language, and eligible depots from
trusted metadata. Stage executable and asset depots in one directory tree with
their relative paths intact and publish it after validation. Detect conflicting
paths and missing assets. Use the recorded executable, working directory, and
arguments. Use bounded, deterministic discovery for older installs without
metadata. Mount game content, writable saves, config, and temp with a consistent
case and path model. Test interrupted updates, cross-depot DLLs, and save
persistence.

### Discover and resolve

Walk the selected PE and reachable shipped DLLs. Record ordinary imports,
delay imports, ordinals, API-set contracts, and forwarders. Use the guest
loader's actual resolution order and keep the graph bounded. An import records
a dependency. Observe actual use through LoadLibrary, GetProcAddress, COM,
managed P/Invoke, and the first graphics device, swap chain, draw, and Present.
Retain multiple renderer candidates when evidence is ambiguous.

Use one capability registry with API, version, ABI, architecture, host
requirements, provider, behavior classification, and tests. Prioritize a
suitable built-in implementation, a legitimate game-shipped library in the
guest, or an approved redistributable with pinned version, digest, provenance,
license, dependencies, and host constraints. Verify packages before use and
support offline cache and rollback. Expose unavailable capabilities through the
API's documented result. Count executed behavior and validated output as
coverage.

### Prepare and execute

Produce a reproducible launch plan from the install fingerprint, host profile,
entry point, arguments, mounts, provider versions, and resource budgets. Share
the plan between Play and the harness. Invalidate it when a recorded input
changes. Keep x64 native guest execution, x86-32 interpretation or approved
execution paths, and managed runtime hosting as separate ABI targets.

Match loader and OS behavior for relocations, TLS and DLL initialization, PE
unwind, virtual memory, handles and lifetimes, synchronization, timers, file
and registry behavior, windows and messages, COM, CRT, and teardown. Render
through the available Xbox UWP backend. For D3D11 and DXGI, track real device
capabilities, resource format and lifetime, state and commands, resolves,
copies, and presentation. Mature legacy D3D, D3D12, GL, and Vulkan translation
with backend-specific conformance fixtures. Test controller behavior through the
API the game consumes. Measure audio, startup video, and save persistence
independently.

### Boundaries

Respect DRM, entitlement, anti-cheat, signing, and Xbox security restrictions.
Use authentic metadata and approved components with verifiable provenance.
Guest libraries and writes stay within the constrained guest environment.
Keep credentials, private logs, and proprietary assets out of patches and public
evidence. Record host-side diagnostics when private logs cannot be retrieved.

## Implementation prompts

### Prove the mount before changing the renderer

Start with the install that normal Play actually selected. Identify the trusted
launch executable, content root, working directory, arguments and writable
root. Follow one asset from its source depot to its mounted path and through
the guest file API. For an install with multiple depots, test a nested
executable that needs a root DLL and a data directory beside the executable.
Reconcile manifest sizes and available digests before publishing the install.
Exercise missing, truncated and case-colliding assets, interrupted updates and
save persistence. Record relative paths and outcomes in the local diagnosis.
Publish aggregate results. Preserve the original executables and directory tree.

### Match folder and disc-image launches

Run the same synthetic install as a folder and as an ISO9660 or UDF image
through Play. Give it a nested executable, a sibling DLL, a DLL at the install
root, and a large archive. Compare the selected executable, working directory,
guest path resolution, DLL search results, and writable root. Record what is
materialized before guest entry and what is read on demand. Measure time,
cache bytes, and peak memory through the first guest archive read. Check
missing and truncated files, media replacement, and a clean launch from cold
cache. Use the first difference to select a shared mount or loader fix.
[PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
[Windows DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)

### Turn a dependency into a verified API capability

Read the launch profile in startup order and select the first required
operation that fails after a verified mount. Trace the requesting image,
ABI, symbol or ordinal, resolved provider, initialization result, and error
returned to the guest. Include dynamic lookups. Compare the operation with
the Windows contract and applicable Wine conformance case. Exercise the real
Play provider selection with minimal guest fixtures for wrong architecture,
missing and forwarded exports, and optional delay loads. Require a pinned
version, provenance, and license before adding a provider. Treat a bundled
candidate as a discovery result. Mark the capability verified after guest
checks for output, errors, ownership, and teardown pass. Repeat on an
independent title using the same API when its launch is authorized.
[LoadLibraryW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryw)
[Wine loader conformance tests](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/tests/loader.c)

### Locate the first missing game pixels

For a black frame, verify content availability, capture the renderer's first
successful device creation and identify the active guest backbuffer after each
resize. Follow sampled shader inputs, bound render targets, copies and resolves
to the presented surface. Record texture dimensions, formats, sample counts
and readback results. Compare game pixels before and after input with the TV
capture. Advance the title matrix only when menu selection or gameplay responds.

### Validate the first playable session within the measured budget

Capture the UWP memory usage and current limit at mount, guest entry, first
presented game frame, and the transition into gameplay. Sample peak memory and
record the first failed allocation if one occurs. Repeat from a cold launch
outside the debugger and label the console model and package version. Test
menu input, entering gameplay, sustained input, audio, save and reload, and
clean exit. Record which milestones occurred in the real game. Optimize the
first measured bottleneck and repeat on another title using the same subsystem.
[Xbox UWP resource allocation](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
[MemoryManager.AppMemoryUsageLimit](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.appmemoryusagelimit)

## Verification and release gates

- Run focused regressions, the offline loader and API suite, the load-only
  corpus, and the Xbox UWP build. Label synthetic and load-only results.
- Exercise unchanged Play on the console. Record the build, hardware, API and
  provider, first fault or failing HRESULT, timing, memory peak, frame counts,
  and approved screen evidence. Label hardware and offline outcomes separately.
- Track install verified → mounted → guest entry → renderer and swap chain →
  first visible game frame → interactive menu → controlled gameplay.
- **Playable menu** requires the actual menu on the Xbox and a visible response
  to controller input. **Gameplay verified** additionally requires controlling
  the game after entering play. Report audio, saves, and clean exit separately.
- Test separate Xbox Series S, Series X, original Xbox One, One S, and One X
  profiles. Device evidence belongs to the model and firmware actually used.
  Track a real x86 D3D9 game, another engine family using D3D11, a DX12-only
  title, and an authorized multi-depot install as coverage gates when available.

The [title matrix](docs/title-matrix.md) records tested titles. The
[pre-release master prompt](https://github.com/xanstomper/Tiramisu/blob/v0.2.4.12/TIRAMISU-MASTER-PROMPT.md)
preserves historical implementation notes.
