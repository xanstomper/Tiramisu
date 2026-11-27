# Universal runtime work map

Wine separates a Windows executable loader from the Windows API implementation.
Proton integrates and versions Wine, graphics backends, media components, and
support libraries as a distribution. Tiramisu needs the same layered approach
on the Xbox UWP host. Keep the PE ABI and observable Windows behavior separate
from Xbox renderer, input, memory, and storage implementations.
[Wine](https://github.com/wine-mirror/wine/blob/master/README.md)
[Proton](https://github.com/ValveSoftware/Proton/blob/proton_10.0/README.md)
[Proton components](https://github.com/ValveSoftware/Proton/blob/proton_10.0/.gitmodules)

DXVK supplies D3D8 through D3D11 over Vulkan. vkd3d-proton supplies D3D12 over a
Vulkan 1.3 feature and extension baseline. Their API behavior, state tracking,
and conformance cases are useful references. Use the console's supported native
D3D and existing translation paths where the host can execute the contract.
Evaluate any imported component against its license, transitive requirements,
actual UWP imports, and console driver capabilities before integration.
[DXVK](https://github.com/doitsujin/dxvk)
[vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton)
[Xbox UWP resource allocation](https://learn.microsoft.com/windows/uwp/xbox-apps/system-resource-allocation)

## Implementation prompts

### Unblock presentation through Play

Use Cuphead to measure the first game frame across shader inputs, viewport and
output merger, intermediate targets, resolve or copy, guest backbuffer, host
backbuffer, and Present. Capture actual readback and a TV frame. Build
0.2.4.58 reported 1,616 successful Presents with black guest and host
backbuffers. Every sampled Present retained an intermediate render target.
Record the target, depth and blend state, and viewport where the draw is
issued. Track later copies and resolves by source and destination to find
the last operation that wrote the guest backbuffer. Compare that pass and
its inputs with the Windows reference.
[Series S replay](proof/xbox-series-s-2026-11-26-runtime-058.json)

Ship format, sample count,
lifetime, and resize behavior with deterministic DXGI/D3D11 conformance cases.
Retest an independent affected title when authorized. Then validate actual
D3D9-to-D3D11 rendering and D3D12 command execution across graphics families.

Start in [the DXGI bridge](../src/trt/bridge_d3d.cpp),
[CoreWindow presentation](../src/host/swapchain.cpp), and
[the D3D11 gate](../tests/test_d3d.cpp). Legacy blt and CoreWindow flip
swap chains can require different backbuffer and resolve behavior. The
0.2.4.53 Series S probe verified a four-sample guest texture resolved into a
single-sample host backbuffer by reading back the expected pixel.
[DXGI swap effects](https://learn.microsoft.com/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect)
[D3D11 ResolveSubresource](https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-resolvesubresource)

### Unblock guest startup

Use Sekiro to bound the last verified startup phase on Xbox. Persist narrowly
scoped host diagnostics for failures before graphics. Compare offline and Xbox
traces before connecting their causes. Exercise native entry, worker start,
TLS, DLL initialization, wait and handle lifetimes, exceptions, process exit,
and cleanup with controlled fixtures. Use Wine's API tests as behavioral
references and verify applicable results against supported Windows contracts.

A PE64 fixture on 0.2.4.56 returned a module handle after its initializer
rejected the load. Keep this fixture as the process-attach regression.
The 0.2.4.57 protection-failure fix covers the earlier stage before the
initializer runs.

Start in [the PE loader](../src/trt/pe_loader.cpp),
[guest image handling](../src/trt/images.cpp),
[thread and synchronization](../src/trt/bridge_thread.cpp), and
[the Play entry point](../src/host/main.cpp).
[Wine ntdll conformance tests](https://github.com/wine-mirror/wine/tree/master/dlls/ntdll/tests)

### Preserve module identity and complete initialization

Start with two game-shipped DLLs that share a filename under different
directories. An explicit load must select the resolved file and reuse that
file for aliases of its path. A bare name reuses the first loaded module.
Check exported behavior and the module filename returned to the guest.
Then use the same identity for handles, dependency edges, and attach state.
Keep the requesting module and search path in the launch report.

Stage the dependency graph, relocate and bind it, and perform process attach
in dependency order. Track cycles and serialize publication. Keep initializing
modules accessible to valid reentrant dependencies, and release unwind entries,
TLS and dependent references on failure. Require `LoadLibrary` to return
null and set `ERROR_DLL_INIT_FAILED` for a rejected attach. Test a failed attach followed by
a successful retry and parallel loads of the same file.
[Windows LoadLibrary](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryw)
[Windows DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain)
[Wine module lookup and initialization](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/loader.c)

### Verify PE32 dependencies in one process

The offline PE32 gate now runs a real executable that calls its bundled DLL
through the static import table. It checks the returned value, guest module
handles, and heap allocations around a loaded image. Extend this with a
transitive dependency that uses static TLS and a dynamic load after entry.
Give each dependency its own TLS index and initialization state, pass Windows
attach and detach notifications in dependency order, and verify a retry after
an initializer rejects the load. Confirm live loads enter the same interpreter
address space and exports use guest addresses. Validate an interactive menu
from a real PE32 D3D9 title on Xbox Series and Xbox One hardware as available.

The interpreter currently commits its entire virtual address space at startup.
Measure resident and committed memory on Xbox and test a page-commit policy
against bridge reads and writes before enabling it. Keep allocation and
mapping failures visible before the guest entry point.
[PE32 execution](../src/trt/x86_exec.cpp)
[PE32 tests](../tests/test_pe32.cpp)

### Make launch decisions reproducible

Use one resolved install tree, one provider registry, and one launch plan for
Play and the offline harness. Extend current import discovery to record the
requesting image and runtime provider. Preserve both startup and delay-load
evidence. Compare static renderer candidates against runtime calls. Make a
verified plan portable across hardware profiles through explicit limits and
backends. Test actual multi-depot publication, partial files, optional APIs,
missing providers, architecture mismatches, and rollback.

Start in [Steam content handling](../src/host/steam_client.cpp),
[install readiness](../src/host/steam_install.cpp),
[the synthesizer](../src/trt/engine_synthesizer.cpp),
[the module registry](../src/trt/module_table.cpp), and
[the profile gate](../tests/test_console_profile.cpp).

### Close the remaining playability gaps

Take a real title from menu into gameplay through the Windows input API it
uses. Exercise reconnect, focus, saved settings, resume, and teardown.
Verify timing, lifetime, and sound in mixer and media tests. Require real
shader output and device status from GPU APIs. Keep a per-family coverage
matrix for the five Xbox models, with original Xbox One hardware validation
of launcher startup as a separate gate. Use title-specific traces to find
shared API improvements. Keep proprietary game payloads in the authorized
local corpus.

Use the [support roadmap](04-support-roadmap.md),
[known issues](known-issues.md), and [title matrix](title-matrix.md) to select
the next highest-impact measured blocker.
