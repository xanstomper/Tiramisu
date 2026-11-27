// tiramisu/src/trt/x86_bridge.cpp
// 32-bit → 64-bit bridge thunking for x86 guest execution.
//
// When a 32-bit guest calls an imported function (via CALL [IAT]),
// this layer intercepts the call, reads arguments from the 32-bit stack,
// invokes the corresponding 64-bit Tiramisu bridge function, and
// writes the 32-bit result back to EAX.
#include "x86_cpu.h"
#include "apiset.h"
#include "trt.h"
#include "images.h"
#include "pe.h"
#include "pe_exports.h"
#include "dynamic_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// MSVC has no POSIX strcasecmp/strncasecmp; map to the _stricmp family.
#if defined(_WIN32) && !defined(strcasecmp)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

extern "C" const char* TRT_STDCALL trt_GetCommandLineA(void);
extern "C" const u16* TRT_STDCALL trt_GetCommandLineW(void);

// Registry of bridge functions: maps (module, symbol) -> function pointer
struct bridge_entry {
    const char* module;
    const char* symbol;
    void* fn;  // 64-bit function pointer
};

// Module-table resolver (defined in module_table.cpp). The 32-bit bridge
// resolves imports through the same table the 64-bit loader uses, so a symbol
// implemented for 64-bit guests is reachable from 32-bit ones too.
extern "C" void* trt_resolve_symbol(const char* module, const char* symbol) {
    return trt_resolve_module(nullptr, module, symbol, 0);
}

static int lookup_arg_count(const char* module, const char* symbol);
static int lookup_cdecl(const char* module, const char* symbol);
static int mod_eq(const char* a, const char* b);
static int sym_eq(const char* a, const char* b);
static uint32_t lookup_ptr_mask(x86_context* ctx, const char* module,
                                const char* symbol, const uint32_t* args,
                                int nargs);

// Every bridge in the trap tables is declared TRT_STDCALL, which on clang/x64
// is __attribute__((ms_abi)): arguments live in RCX/RDX/R8/R9 and the caller
// must reserve the 32-byte shadow space. Calling one through a plain SysV
// `(...)` pointer delivers args in RDI/RSI/RDX, so the callee reads whatever
// the previous guest code left in RCX. (Seen live: OpenTTD's
// QueryPerformanceCounter received a leftover code address as its out pointer
// and stored through it.) The call site therefore has to use the same ABI.
#if defined(__x86_64__) && defined(__clang__)
#  define TRT_BRIDGE_ABI __attribute__((ms_abi))
#else
#  define TRT_BRIDGE_ABI
#endif

// Call an already-resolved 64-bit bridge function with 32-bit arguments.
static uint32_t x86_call_fn(x86_context* ctx, void* fn, const char* module,
                            const char* symbol, uint32_t* args, int nargs) {

    // Widen every argument to 64 bits. On both Win64 and SysV x64 an integer
    // argument narrower than 64 bits occupies the low bits of its register
    // slot, so a bridge expecting a 32-bit int reads the correct value, while
    // a bridge expecting a pointer gets the full host address.
    uint32_t mask = lookup_ptr_mask(ctx, module, symbol, args, nargs);
    static int trace = -1;
    if (trace < 0) { const char* e = getenv("TRT_X86_TRACE"); trace = (e && e[0] == '1') ? 1 : 0; }
    if (trace && ctx->log) {
        char t[256];
        snprintf(t, sizeof(t), "x86: call %s!%s nargs=%d a0=%08x a1=%08x mask=%x",
                 module, symbol, nargs, args[0], nargs > 1 ? args[1] : 0u, mask);
        ctx->log(t);
    }
    uint64_t a[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = (i < nargs) ? args[i] : 0;
        if (i < nargs && (mask & (1u << i)) && ctx->mem.base && v) {
            // Guest VA -> host pointer into the contiguous 4GB reservation.
            a[i] = (uint64_t)(uintptr_t)(ctx->mem.base + v);
        } else {
            a[i] = (uint64_t)v;
        }
    }

    typedef uint64_t (TRT_BRIDGE_ABI *generic_fn)(...);
    generic_fn gfn = (generic_fn)fn;
    uint64_t r = 0;
    switch (nargs) {
        case 0: r = gfn(); break;
        case 1: r = gfn(a[0]); break;
        case 2: r = gfn(a[0], a[1]); break;
        case 3: r = gfn(a[0], a[1], a[2]); break;
        case 4: r = gfn(a[0], a[1], a[2], a[3]); break;
        case 5: r = gfn(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        case 10: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
        case 11: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]); break;
        case 12: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]); break;
        case 13: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12]); break;
        case 14: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]); break;
        case 15: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]); break;
        case 16: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); break;
        default: {
            char buf[256];
            snprintf(buf, sizeof(buf), "x86_bridge: too many args (%d) for %s!%s", nargs, module, symbol);
            if (ctx->log) ctx->log(buf);
            return 0;
        }
    }

    if (mod_eq(module, "msvcrt")) {
        // Win32 returns a 64-bit offset in EDX:EAX and a pointer in EAX.
        if (sym_eq(symbol, "_telli64")) ctx->regs.edx = (uint32_t)(r >> 32);
        if (sym_eq(symbol, "_ultoa")) return r ? args[1] : 0;
    }
    return (uint32_t)r;
}

// Resolve then call (public entry used by the older direct-call path).
uint32_t x86_call_bridge(x86_context* ctx, const char* module, const char* symbol,
                         uint32_t* args, int nargs) {
    void* fn = trt_resolve_symbol(module, symbol);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "x86_bridge: unresolved %s!%s -> returning 0", module, symbol);
        if (ctx->log) ctx->log(buf);
        return 0;
    }
    return x86_call_fn(ctx, fn, module, symbol, args, nargs);
}


// ---- Argument count database for common Windows APIs ----
// Maps "module!symbol" -> number of arguments (for __stdcall)
static int lookup_arg_count(const char* module, const char* symbol) {
    // winmmbase.dll is the api-set that backs winmm. Same functions, same
    // calling conventions, so every arity answer must apply to both names -
    // otherwise a PE32 title importing winmmbase gets the 4-argument guess.
    if (module && (strcasecmp(module, "winmmbase") == 0 ||
                   strcasecmp(module, "winmmbase.dll") == 0))
        module = "winmm";
    // api-ms-win-core-version-l1-1-* is the api-set alias of version.dll;
    // give it the same arity answers (bridge_version registers all three).
    if (module && strncasecmp(module, "api-ms-win-core-version", 23) == 0)
        module = "version";
    // kernelbase is the implementation module behind kernel32; the arity
    // table is keyed on kernel32, so consult it for kernelbase too.
    if (module && (strcasecmp(module, "kernelbase") == 0 ||
                   strcasecmp(module, "kernelbase.dll") == 0))
        module = "kernel32";
    // wsock32 forwards WinSock1 calls to ws2_32; dstoragecore backs dstorage.
    if (module && (strcasecmp(module, "wsock32") == 0 ||
                   strcasecmp(module, "wsock32.dll") == 0))
        module = "ws2_32";
    if (module && (strcasecmp(module, "dstoragecore") == 0 ||
                   strcasecmp(module, "dstoragecore.dll") == 0))
        module = "dstorage";
    // Module aliases: single canonical ABI table for equivalent Win32/middleware DLL names.
    if (module && (strcasecmp(module, "sechost") == 0 ||
                   strcasecmp(module, "sechost.dll") == 0))
        module = "advapi32";
    if (module && strncasecmp(module, "api-ms-win-core-winrt", 21) == 0)
        module = "combase";
    if (module && (strcasecmp(module, "d3d12core") == 0 ||
                   strcasecmp(module, "d3d12core.dll") == 0))
        module = "d3d12";
    if (module && (strcasecmp(module, "fmodl") == 0 || strcasecmp(module, "fmodl.dll") == 0 ||
                   strcasecmp(module, "fmodex") == 0 || strcasecmp(module, "fmodex.dll") == 0 ||
                   strcasecmp(module, "fmodex64") == 0 || strcasecmp(module, "fmodex64.dll") == 0 ||
                   strcasecmp(module, "fmodstudio") == 0 || strcasecmp(module, "fmodstudio.dll") == 0 ||
                   strcasecmp(module, "fmodstudiol") == 0 || strcasecmp(module, "fmodstudiol.dll") == 0))
        module = "fmod";
    if (module && (strcasecmp(module, "bink2w32") == 0 || strcasecmp(module, "bink2w32.dll") == 0 ||
                   strcasecmp(module, "bink2w64") == 0 || strcasecmp(module, "bink2w64.dll") == 0 ||
                   strcasecmp(module, "binkw32") == 0 || strcasecmp(module, "binkw32.dll") == 0 ||
                   strcasecmp(module, "binkw64") == 0 || strcasecmp(module, "binkw64.dll") == 0))
        module = "bink";
    if (module && (strcasecmp(module, "aksoundengine") == 0 || strcasecmp(module, "aksoundengine.dll") == 0 ||
                   strcasecmp(module, "aksoundenginep") == 0 || strcasecmp(module, "aksoundenginep.dll") == 0 ||
                   strcasecmp(module, "akmemorymgr") == 0 || strcasecmp(module, "akmemorymgr.dll") == 0 ||
                   strcasecmp(module, "akstreammgr") == 0 || strcasecmp(module, "akstreammgr.dll") == 0 ||
                   strcasecmp(module, "akmusicengine") == 0 || strcasecmp(module, "akmusicengine.dll") == 0 ||
                   strcasecmp(module, "ak::soundengine") == 0 || strcasecmp(module, "ak::soundengine.dll") == 0))
        module = "wwise";
    if (module && strncasecmp(module, "eossdk-win", 10) == 0)
        module = "eos";
    if (module && (strcasecmp(module, "galaxy64") == 0 || strcasecmp(module, "galaxy64.dll") == 0 ||
                   strcasecmp(module, "galaxy") == 0 || strcasecmp(module, "galaxy.dll") == 0))
        module = "galaxy";

    // ---- General table (module, symbol, stdcall arg count) ----
    struct arg_ent { const char* mod; const char* sym; int n; };
    static const arg_ent k_args[] = {
        // kernel32
        {"kernel32","GetModuleHandleA",1},{"kernel32","GetModuleHandleW",1},
        {"kernel32","GetProcAddress",2},{"kernel32","LoadLibraryA",1},
        {"kernel32","LoadLibraryW",1},{"kernel32","LoadLibraryExA",3},
        {"kernel32","LoadLibraryExW",3},{"kernel32","FreeLibrary",1},
        {"kernel32","CreateFileA",7},{"kernel32","CreateFileW",7},
        {"kernel32","ReadFile",5},{"kernel32","WriteFile",5},
        {"kernel32","CloseHandle",1},{"kernel32","GetFileSize",2},
        {"kernel32","GetFileSizeEx",2},{"kernel32","SetFilePointer",4},
        {"kernel32","SetFilePointerEx",4},{"kernel32","VirtualAlloc",4},
        {"kernel32","VirtualFree",3},{"kernel32","VirtualProtect",4},
        {"kernel32","VirtualQuery",3},{"kernel32","HeapAlloc",3},
        {"kernel32","HeapFree",3},{"kernel32","HeapCreate",3},
        {"kernel32","HeapDestroy",1},{"kernel32","HeapReAlloc",4},
        {"kernel32","HeapSize",3},{"kernel32","GetProcessHeap",0},
        {"kernel32","GetLastError",0},{"kernel32","SetLastError",1},
        {"kernel32","GetCommandLineA",0},{"kernel32","GetCommandLineW",0},
        {"kernel32","ExitProcess",1},{"kernel32","GetCurrentProcess",0},
        {"kernel32","GetCurrentThread",0},{"kernel32","GetCurrentThreadId",0},
        {"kernel32","GetCurrentProcessId",0},{"kernel32","Sleep",1},
        {"kernel32","SleepEx",2},{"kernel32","GetTickCount",0},
        {"kernel32","GetTickCount64",0},{"kernel32","QueryPerformanceCounter",1},
        // Time / locale / startup. These were missing and fell through to the
        // 4-arg default, so a 1-arg stdcall (GetSystemTimeAsFileTime) popped
        // 12 extra bytes off the guest stack. OpenTTD lost its return address
        // there and jumped to 0x0000B5E3 right after QueryPerformanceCounter.
        {"kernel32","GetSystemTimeAsFileTime",1},{"kernel32","GetSystemTime",1},
        {"kernel32","GetLocalTime",1},{"kernel32","FileTimeToSystemTime",2},
        {"kernel32","SystemTimeToFileTime",2},{"kernel32","CompareFileTime",2},
        {"kernel32","GetTimeZoneInformation",1},{"kernel32","FileTimeToLocalFileTime",2},
        {"kernel32","GetSystemTimeAsFileTime",1},{"kernel32","QueryPerformanceFrequency",1},
        {"kernel32","GetVersion",0},{"kernel32","GetVersionExA",1},
        {"kernel32","GetVersionExW",1},{"kernel32","IsDebuggerPresent",0},
        {"kernel32","SetUnhandledExceptionFilter",1},{"kernel32","UnhandledExceptionFilter",1},
        {"kernel32","GetStartupInfoA",1},{"kernel32","GetStartupInfoW",1},
        {"kernel32","GetStdHandle",1},{"kernel32","SetStdHandle",2},
        {"kernel32","GetACP",0},{"kernel32","GetOEMCP",0},
        {"kernel32","IsValidCodePage",1},{"kernel32","GetCPInfo",2},
        {"kernel32","GetLocaleInfoA",4},{"kernel32","GetLocaleInfoW",4},
        // Localization api-set entry points OpenTTD resolves by hand. A wrong
        // arity here is fatal, not cosmetic: the 4-arg default popped 12 bytes
        // too many off IsValidLocaleName's stack and the CRT returned into
        // stack data (EIP=0x001FFF3A, executed as 0xEA).
        {"kernel32","GetLocaleInfoEx",4},{"kernel32","IsValidLocale",2},
        {"kernel32","IsValidLocaleName",1},{"kernel32","LocaleNameToLCID",2},
        {"kernel32","LCIDToLocaleName",4},
        {"kernel32","TlsAlloc",0},{"kernel32","TlsFree",1},
        {"kernel32","TlsGetValue",1},{"kernel32","TlsSetValue",2},
        // Fiber-local storage has the same shape as TLS. Without these the
        // 4-arg default shifted the guest stack 8 bytes on every FlsSetValue.
        {"kernel32","FlsAlloc",1},{"kernel32","FlsFree",1},
        {"kernel32","FlsGetValue",1},{"kernel32","FlsGetValue2",1},
        {"kernel32","FlsSetValue",2},
        {"kernel32","lstrlenA",1},{"kernel32","lstrlenW",1},
        {"kernel32","lstrcpyA",2},{"kernel32","lstrcpyW",2},
        {"kernel32","MultiByteToWideChar",6},{"kernel32","WideCharToMultiByte",8},
        {"kernel32","LocalAlloc",2},{"kernel32","LocalFree",1},
        {"kernel32","GlobalAlloc",2},{"kernel32","GlobalFree",1},
        {"kernel32","GlobalLock",1},{"kernel32","GlobalUnlock",1},
        {"kernel32","GetSystemInfo",1},{"kernel32","GetSystemDirectoryA",2},
        {"kernel32","GetWindowsDirectoryA",2},{"kernel32","GetTempPathA",2},
        {"kernel32","SetCurrentDirectoryA",1},{"kernel32","GetCurrentDirectoryA",2},
        {"kernel32","GetEnvironmentVariableA",2},{"kernel32","SetEnvironmentVariableA",2},
        {"kernel32","GetEnvironmentStrings",0},{"kernel32","FreeEnvironmentStringsA",1},
        {"kernel32","FindFirstFileA",2},{"kernel32","FindNextFileA",2},
        {"kernel32","FindClose",1},{"kernel32","CreateDirectoryA",2},
        {"kernel32","RemoveDirectoryA",1},{"kernel32","DeleteFileA",1},
        {"kernel32","MoveFileA",2},{"kernel32","GetFileAttributesA",1},
        {"kernel32","SetFileAttributesA",2},{"kernel32","GetFileType",1},
        {"kernel32","FlushFileBuffers",1},{"kernel32","SetEndOfFile",1},
        {"kernel32","QueryPerformanceFrequency",1},
        {"kernel32","InitializeCriticalSection",1},
        {"kernel32","InitializeCriticalSectionAndSpinCount",2},
        // Reached through LoadLibraryExW("api-ms-win-core-synch-l1-2-0") +
        // GetProcAddress by the MSVC CRT (and OpenTTD's initterm loop). Each
        // one bound as an api-set name would be a NAMED GAP and drift the
        // stack; they are bound under the resolved kernel32 name instead.
        {"kernel32","InitializeCriticalSectionEx",3},
        {"kernel32","InitializeSListHead",1},
        {"kernel32","GetSystemTimePreciseAsFileTime",1},
        {"kernel32","EncodePointer",1},{"kernel32","DecodePointer",1},
        {"kernel32","RtlCaptureContext",1},
        {"kernel32","GetCurrentProcessorNumber",0},
        {"kernel32","GetActiveProcessorCount",1},
        {"kernel32","SetThreadStackGuarantee",1},
        {"kernel32","GetThreadTimes",5},{"kernel32","GetProcessTimes",5},
        {"kernel32","QueryThreadCycleTime",2},
        {"kernel32","GetProcessAffinityMask",3},
        {"kernel32","SetProcessAffinityMask",2},
        {"kernel32","GetFileInformationByHandleEx",4},
        {"kernel32","InitializeProcThreadAttributeList",4},
        {"kernel32","UpdateProcThreadAttribute",7},
        {"kernel32","EnterCriticalSection",1},{"kernel32","LeaveCriticalSection",1},
        {"kernel32","DeleteCriticalSection",1},{"kernel32","TryEnterCriticalSection",1},
        {"kernel32","CreateEventA",4},{"kernel32","CreateEventW",4},
        {"kernel32","OpenEventA",3},{"kernel32","OpenEventW",3},
        {"kernel32","SetEvent",1},{"kernel32","ResetEvent",1},
        {"kernel32","WaitForSingleObject",2},{"kernel32","WaitForMultipleObjects",4},
        {"kernel32","CreateThread",6},{"kernel32","ExitThread",1},
        {"kernel32","GetExitCodeThread",2},{"kernel32","TerminateThread",2},
        {"kernel32","SetThreadPriority",2},{"kernel32","GetThreadPriority",1},
        {"kernel32","SuspendThread",1},{"kernel32","ResumeThread",1},
        {"kernel32","GetModuleFileNameA",3},{"kernel32","GetModuleFileNameW",3},
        {"kernel32","GetEnvironmentVariableA",3},{"kernel32","GetEnvironmentVariableW",3},
        {"kernel32","SetEnvironmentVariableA",2},{"kernel32","SetEnvironmentVariableW",2},
        {"kernel32","MultiByteToWideChar",6},{"kernel32","WideCharToMultiByte",8},
        {"kernel32","GetStdHandle",1},{"kernel32","SetStdHandle",2},
        {"kernel32","WriteConsoleA",5},{"kernel32","WriteConsoleW",5},
        {"kernel32","ReadConsoleA",5},{"kernel32","ReadConsoleW",5},
        {"kernel32","GetConsoleMode",2},{"kernel32","SetConsoleMode",2},
        {"kernel32","GetSystemInfo",1},{"kernel32","GlobalMemoryStatus",1},
        {"kernel32","GlobalMemoryStatusEx",1},{"kernel32","GetVersionExA",1},
        {"kernel32","GetVersionExW",1},{"kernel32","IsDebuggerPresent",0},
        {"kernel32","OutputDebugStringA",1},{"kernel32","OutputDebugStringW",1},
        {"kernel32","RaiseException",4},{"kernel32","UnhandledExceptionFilter",1},
        {"kernel32","SetUnhandledExceptionFilter",1},{"kernel32","TerminateProcess",2},
        {"kernel32","RtlMoveMemory",3},{"kernel32","RtlZeroMemory",2},
        {"kernel32","RtlFillMemory",3},
        // ntdll: the Rtl* primitives bridge_ntdll registers under the ntdll
        // name (Bun/WebKit startup futexes and JIT unwind tables). api-set
        // imports forward here, and the trap binds the resolved name, so the
        // arity answer must be keyed on ntdll or a PE32 title guesses.
        {"ntdll","RtlWaitOnAddress",4},
        {"ntdll","RtlWakeAddressSingle",1},
        {"ntdll","RtlWakeAddressAll",1},
        {"ntdll","RtlAddGrowableFunctionTable",6},
        {"ntdll","RtlDeleteGrowableFunctionTable",1},
        {"ntdll","RtlGrowFunctionTable",2},
        {"ntdll","RtlCaptureStackBackTrace",4},
        {"kernel32","CreateFileMappingA",6},
        {"kernel32","CreateFileMappingW",6},{"kernel32","MapViewOfFile",5},
        {"kernel32","UnmapViewOfFile",1},{"kernel32","FlushViewOfFile",2},
        {"kernel32","OpenFileMappingA",3},{"kernel32","FindFirstFileA",2},
        {"kernel32","FindFirstFileW",2},{"kernel32","FindNextFileA",2},
        {"kernel32","FindNextFileW",2},{"kernel32","FindClose",1},
        {"kernel32","CreateDirectoryA",2},{"kernel32","CreateDirectoryW",2},
        {"kernel32","RemoveDirectoryA",1},{"kernel32","DeleteFileA",1},
        {"kernel32","DeleteFileW",1},{"kernel32","CopyFileA",3},
        {"kernel32","MoveFileA",2},{"kernel32","SetFileAttributesA",2},
        {"kernel32","GetFileAttributesA",1},{"kernel32","GetFileAttributesW",1},
        {"kernel32","GetTempPathA",2},{"kernel32","GetTempPathW",2},
        {"kernel32","GetFullPathNameA",4},{"kernel32","GetFullPathNameW",4},
        {"kernel32","SetCurrentDirectoryA",1},{"kernel32","GetCurrentDirectoryA",2},
        {"kernel32","SetThreadAffinityMask",2},{"kernel32","GetSystemTimes",3},
        {"kernel32","CreateMutexA",3},{"kernel32","CreateMutexW",3},
        {"kernel32","ReleaseMutex",1},{"kernel32","DuplicateHandle",7},
        {"kernel32","GetHandleInformation",2},{"kernel32","SetHandleInformation",3},
        {"kernel32","FileTimeToSystemTime",2},{"kernel32","GetSystemTime",1},
        {"kernel32","GetLocalTime",1},{"kernel32","SystemTimeToFileTime",2},
        {"kernel32","TlsAlloc",0},{"kernel32","TlsFree",1},
        {"kernel32","TlsGetValue",1},{"kernel32","TlsSetValue",2},
        {"kernel32","InterlockedIncrement",1},{"kernel32","InterlockedDecrement",1},
        {"kernel32","InterlockedExchange",2},{"kernel32","InterlockedCompareExchange",3},
        {"kernel32","InterlockedExchangeAdd",2},{"kernel32","InterlockedAdd",2},
        {"kernel32","lstrlenA",1},{"kernel32","lstrlenW",1},
        {"kernel32","lstrcpyA",2},{"kernel32","lstrcpyW",2},
        {"kernel32","GetACP",0},{"kernel32","GetOEMCP",0},
        {"kernel32","IsProcessorFeaturePresent",1},
        // user32
        {"user32","MessageBoxA",4},{"user32","MessageBoxW",4},
        {"user32","RegisterClassA",1},{"user32","RegisterClassW",1},
        {"user32","RegisterClassExA",1},{"user32","RegisterClassExW",1},
        {"user32","UnregisterClassA",2},{"user32","UnregisterClassW",2},
        {"user32","CreateWindowExA",12},{"user32","CreateWindowExW",12},
        {"user32","DestroyWindow",1},{"user32","ShowWindow",2},
        {"user32","UpdateWindow",1},{"user32","GetMessageA",4},
        {"user32","GetMessageW",4},{"user32","PeekMessageA",5},
        {"user32","PeekMessageW",5},{"user32","TranslateMessage",1},
        {"user32","DispatchMessageA",1},{"user32","DispatchMessageW",1},
        {"user32","DefWindowProcA",4},{"user32","DefWindowProcW",4},
        {"user32","PostQuitMessage",1},{"user32","PostMessageA",4},
        {"user32","PostMessageW",4},{"user32","SendMessageA",4},
        {"user32","SendMessageW",4},{"user32","LoadIconA",2},
        {"user32","LoadIconW",2},{"user32","LoadCursorA",2},
        {"user32","LoadCursorW",2},{"user32","GetDC",1},
        {"user32","ReleaseDC",2},{"user32","BeginPaint",2},
        {"user32","EndPaint",2},{"user32","GetClientRect",2},
        {"user32","GetWindowRect",2},{"user32","SetWindowTextA",2},
        {"user32","SetWindowTextW",2},{"user32","GetWindowTextA",3},
        {"user32","GetWindowTextW",3},{"user32","GetWindowLongA",2},
        {"user32","GetWindowLongW",2},{"user32","SetWindowLongA",3},
        {"user32","SetWindowLongW",3},{"user32","InvalidateRect",3},
        {"user32","GetSystemMetrics",1},{"user32","SetTimer",4},
        {"user32","KillTimer",2},{"user32","GetKeyState",1},
        {"user32","GetAsyncKeyState",1},{"user32","GetKeyboardState",1},
        {"user32","SetKeyboardState",1},{"user32","MapVirtualKeyA",2},
        {"user32","MapVirtualKeyW",2},{"user32","GetCursorPos",1},
        {"user32","SetCursorPos",2},{"user32","ShowCursor",1},
        {"user32","ClipCursor",1},{"user32","SetCapture",1},
        {"user32","ReleaseCapture",0},{"user32","GetForegroundWindow",0},
        {"user32","SetForegroundWindow",1},{"user32","IsWindow",1},
        {"user32","IsWindowVisible",1},{"user32","FindWindowA",2},
        {"user32","FindWindowW",2},{"user32","AdjustWindowRect",3},
        {"user32","AdjustWindowRectEx",4},{"user32","wsprintfA",0},
        {"user32","wsprintfW",0},{"user32","GetDesktopWindow",0},
        // gdi32
        {"gdi32","CreateCompatibleDC",1},{"gdi32","DeleteDC",1},
        {"gdi32","SelectObject",2},{"gdi32","DeleteObject",1},
        {"gdi32","CreateSolidBrush",1},{"gdi32","CreatePen",3},
        {"gdi32","TextOutA",5},{"gdi32","TextOutW",5},
        {"gdi32","BitBlt",9},{"gdi32","StretchBlt",11},
        {"gdi32","GetStockObject",1},{"gdi32","SetBkMode",2},
        {"gdi32","SetTextColor",2},{"gdi32","CreateFontA",14},
        {"gdi32","CreateFontW",14},{"gdi32","CreateFontIndirectA",1},
        {"gdi32","GetDeviceCaps",2},{"gdi32","SetPixel",4},
        {"gdi32","GetPixel",3},{"gdi32","Rectangle",5},
        {"gdi32","FillRect",3},{"gdi32","GetObjectA",3},
        {"gdi32","SetDCBrushColor",2},{"gdi32","SetDCPenColor",2},
        {"gdi32","Arc",9},{"gdi32","Pie",9},{"gdi32","Chord",9},
        // GDI paths: recording, replay and clip selection
        {"gdi32","BeginPath",1},{"gdi32","EndPath",1},{"gdi32","AbortPath",1},
        {"gdi32","CloseFigure",1},{"gdi32","GetPath",4},
        {"gdi32","FlattenPath",1},{"gdi32","WidenPath",1},
        {"gdi32","StrokePath",1},{"gdi32","FillPath",1},
        {"gdi32","StrokeAndFillPath",1},{"gdi32","SelectClipPath",2},
        {"gdi32","SetPolyFillMode",2},{"gdi32","GetPolyFillMode",1},
        {"gdi32","PathToRegion",1},{"gdi32","PolylineTo",3},
        {"gdi32","PolyBezier",3},{"gdi32","PolyBezierTo",3},
        // gdiplus: the GDI+ 2D flat API renders on the DC surface
        {"gdiplus","GdiplusStartup",3},{"gdiplus","GdiplusShutdown",1},
        {"gdiplus","GdipAlloc",1},{"gdiplus","GdipFree",1},
        {"gdiplus","GdipCreateFromHDC",2},{"gdiplus","GdipCreateFromHDCEx",6},
        {"gdiplus","GdipCreateFromHWND",2},{"gdiplus","GdipDeleteGraphics",1},
        {"gdiplus","GdipGetDC",2},{"gdiplus","GdipReleaseDC",2},
        {"gdiplus","GdipGetDpiX",2},{"gdiplus","GdipGetDpiY",2},
        {"gdiplus","GdipTranslateTransform",4},
        {"gdiplus","GdipResetTransform",1},{"gdiplus","GdipScaleTransform",4},
        {"gdiplus","GdipRotateTransform",3},{"gdiplus","GdipSetPageUnit",2},
        // GDI+ matrices and the world transform
        {"gdiplus","GdipCreateMatrix",1},{"gdiplus","GdipCreateMatrix2",7},
        {"gdiplus","GdipCloneMatrix",2},{"gdiplus","GdipDeleteMatrix",1},
        {"gdiplus","GdipSetMatrixElements",7},{"gdiplus","GdipGetMatrixElements",2},
        {"gdiplus","GdipMultiplyMatrix",3},{"gdiplus","GdipTranslateMatrix",4},
        {"gdiplus","GdipScaleMatrix",4},{"gdiplus","GdipRotateMatrix",3},
        {"gdiplus","GdipInvertMatrix",1},{"gdiplus","GdipTransformMatrixPoints",3},
        {"gdiplus","GdipSetWorldTransform",2},{"gdiplus","GdipGetWorldTransform",2},
        {"gdiplus","GdipGetMatrixLastStatus",1},{"gdiplus","GdipTransformPoints",5},
        {"gdiplus","GdipSetSmoothingMode",2},
        {"gdiplus","GdipSetInterpolationMode",2},
        {"gdiplus","GdipSetPixelOffsetMode",2},
        {"gdiplus","GdipSetCompositingMode",2},
        {"gdiplus","GdipSetCompositingQuality",2},
        {"gdiplus","GdipSetTextRenderingHint",2},
        {"gdiplus","GdipCreateSolidFill",2},
        {"gdiplus","GdipSetSolidFillColor",2},
        {"gdiplus","GdipGetSolidFillColor",2},{"gdiplus","GdipDeleteBrush",1},
        {"gdiplus","GdipCloneBrush",2},{"gdiplus","GdipCreatePen1",4},
        {"gdiplus","GdipCreatePen2",4},{"gdiplus","GdipDeletePen",1},
        {"gdiplus","GdipSetPenColor",2},{"gdiplus","GdipSetPenWidth",2},
        {"gdiplus","GdipSetPenEndCap",2},{"gdiplus","GdipSetPenStartCap",2},
        {"gdiplus","GdipSetPenLineJoin",2},
        {"gdiplus","GdipSetPenDashStyle",2},
        {"gdiplus","GdipSetPenLineCap197819",4},
        {"gdiplus","GdipFillRectangleI",6},{"gdiplus","GdipFillRectangle",6},
        {"gdiplus","GdipFillRectanglesI",4},
        {"gdiplus","GdipDrawRectangleI",6},{"gdiplus","GdipDrawRectangle",6},
        {"gdiplus","GdipDrawLineI",6},{"gdiplus","GdipDrawLine",6},
        {"gdiplus","GdipDrawLinesI",4},{"gdiplus","GdipDrawLines",4},
        {"gdiplus","GdipFillEllipseI",6},{"gdiplus","GdipFillEllipse",6},
        {"gdiplus","GdipDrawEllipseI",6},{"gdiplus","GdipDrawEllipse",6},
        {"gdiplus","GdipFillPolygonI",5},{"gdiplus","GdipDrawPolygonI",4},
        {"gdiplus","GdipDrawArcI",8},{"gdiplus","GdipDrawPieI",8},
        {"gdiplus","GdipCreatePath",2},{"gdiplus","GdipDeletePath",1},
        {"gdiplus","GdipResetPath",1},{"gdiplus","GdipAddPathRectangle",5},
        {"gdiplus","GdipAddPathEllipse",5},{"gdiplus","GdipAddPathLine",5},
        {"gdiplus","GdipAddPathLine2",3},{"gdiplus","GdipAddPathLines",3},
        {"gdiplus","GdipAddPathArc",7},{"gdiplus","GdipAddPathPolygon",3},
        {"gdiplus","GdipAddPathPolygonI",3},{"gdiplus","GdipStartFigure",1},
        {"gdiplus","GdipClosePathFigure",1},
        {"gdiplus","GdipClosePathFigures",1},{"gdiplus","GdipFillPath",3},
        {"gdiplus","GdipDrawPath",3},
        {"gdiplus","GdipCreateBitmapFromScan0",6},
        {"gdiplus","GdipCloneImage",2},{"gdiplus","GdipDisposeImage",1},
        {"gdiplus","GdipGetImageWidth",2},{"gdiplus","GdipGetImageHeight",2},
        {"gdiplus","GdipBitmapGetPixel",4},{"gdiplus","GdipBitmapSetPixel",4},
        {"gdiplus","GdipBitmapLockBits",5},
        {"gdiplus","GdipBitmapUnlockBits",2},{"gdiplus","GdipDrawImageI",4},
        {"gdiplus","GdipDrawImage",4},{"gdiplus","GdipDrawImageRectI",6},
        {"gdiplus","GdipDrawImageRect",6},
        {"gdiplus","GdipDrawImageRectRectI",14},
        {"gdiplus","GdipDrawImageRectRect",14},
        {"gdiplus","GdipCreateImageAttributes",1},
        {"gdiplus","GdipDisposeImageAttributes",1},
        {"gdiplus","GdipSetImageAttributesWrapMode",4},
        {"gdiplus","GdipCreateFontFamilyFromName",3},
        {"gdiplus","GdipDeleteFontFamily",1},{"gdiplus","GdipCreateFont",5},
        {"gdiplus","GdipDeleteFont",1},{"gdiplus","GdipGetFontSize",2},
        {"gdiplus","GdipCreateStringFormat",3},
        {"gdiplus","GdipDeleteStringFormat",1},
        {"gdiplus","GdipSetStringFormatAlign",2},
        {"gdiplus","GdipDrawString",7},{"gdiplus","GdipMeasureString",9},
        // msimg32: blended / transparent / gradient composites
        {"msimg32","AlphaBlend",11},{"msimg32","TransparentBlt",11},
        {"msimg32","GradientFill",6},
        // advapi32
        {"advapi32","RegOpenKeyExA",5},{"advapi32","RegOpenKeyExW",5},
        {"advapi32","RegQueryValueExA",6},{"advapi32","RegQueryValueExW",6},
        {"advapi32","RegSetValueExA",6},{"advapi32","RegSetValueExW",6},
        {"advapi32","RegCloseKey",1},{"advapi32","RegCreateKeyExA",9},
        {"advapi32","RegCreateKeyExW",9},{"advapi32","RegEnumKeyExA",8},
        {"advapi32","RegEnumValueA",8},{"advapi32","RegDeleteKeyA",2},
        {"advapi32","OpenProcessToken",3},{"advapi32","GetTokenInformation",5},
        // ole32 / oleaut32
        {"ole32","CoInitialize",1},{"ole32","CoInitializeEx",2},
        {"ole32","CoUninitialize",0},{"ole32","CoCreateInstance",5},
        {"ole32","CoTaskMemAlloc",1},{"ole32","CoTaskMemFree",1},
        {"oleaut32","SysAllocString",1},{"oleaut32","SysFreeString",1},
        {"oleaut32","SysStringLen",1},{"oleaut32","VariantInit",1},
        {"oleaut32","VariantClear",1},{"oleaut32","#148",3},
        {"oleaut32","SafeArrayPtrOfIndex",3},
        // shell32 / shlwapi / comctl32
        {"shell32","SHGetFolderPathA",5},{"shell32","SHGetFolderPathW",5},
        {"shell32","SHGetKnownFolderPath",4},{"shell32","ShellExecuteA",6},
        {"shell32","ShellExecuteW",6},{"shell32","SHGetFileInfoA",5},
        {"shlwapi","PathFileExistsA",1},{"shlwapi","PathFileExistsW",1},
        {"shlwapi","PathCombineA",3},{"shlwapi","PathRemoveFileSpecA",1},
        {"comctl32","InitCommonControls",0},{"comctl32","InitCommonControlsEx",1},
        // winmm / ws2_32
        {"winmm","timeBeginPeriod",1},{"winmm","timeEndPeriod",1},
        {"winmm","timeGetTime",0},{"winmm","PlaySoundA",3},
        {"winmm","PlaySoundW",3},{"winmm","mciSendStringA",4},
        {"winmm","mciSendStringW",4},{"winmm","mciSendCommandA",4},
        {"winmm","mciSendCommandW",4},{"winmm","mciGetErrorStringA",3},
        {"winmm","mciGetErrorStringW",3},
        {"ws2_32","WSAStartup",2},{"ws2_32","WSACleanup",0},
        {"ws2_32","socket",3},{"ws2_32","closesocket",1},
        {"ws2_32","connect",3},{"ws2_32","send",4},{"ws2_32","recv",4},
        {"ws2_32","bind",3},{"ws2_32","listen",2},{"ws2_32","accept",3},
        {"ws2_32","inet_addr",1},{"ws2_32","htons",1},{"ws2_32","ntohs",1},
        {"ws2_32","select",5},{"ws2_32","ioctlsocket",3},
        // crypt32 / psapi / version
        {"crypt32","CryptAcquireContextA",5},{"crypt32","CryptReleaseContext",2},
        {"crypt32","CryptGenRandom",3},
        {"psapi","EnumProcesses",3},{"psapi","GetProcessMemoryInfo",3},
        {"psapi","GetModuleBaseNameA",4},{"psapi","GetModuleBaseNameW",4},
        {"psapi","GetModuleInformation",4},{"psapi","K32GetModuleInformation",4},
        {"version","GetFileVersionInfoSizeA",2},{"version","GetFileVersionInfoSizeW",2},
        {"version","GetFileVersionInfoSizeExA",3},{"version","GetFileVersionInfoSizeExW",3},
        {"version","GetFileVersionInfoA",4},{"version","GetFileVersionInfoW",4},
        {"version","GetFileVersionInfoExA",5},{"version","GetFileVersionInfoExW",5},
        {"version","VerQueryValueA",4},{"version","VerQueryValueW",4},
        {"version","VerLanguageNameA",2},{"version","VerLanguageNameW",2},
        // activation contexts (kernelbase forwards to kernel32)
        {"kernel32","CreateActCtxA",1},{"kernel32","CreateActCtxW",1},
        {"kernel32","AddRefActCtx",1},{"kernel32","ReleaseActCtx",1},
        {"kernel32","ActivateActCtx",2},{"kernel32","DeactivateActCtx",2},
        {"kernel32","GetCurrentActCtx",1},{"kernel32","ZombifyActCtx",1},
        {"kernel32","QueryActCtxW",7},{"kernel32","QueryActCtxSettingsW",7},
        {"kernel32","FindActCtxSectionStringA",5},{"kernel32","FindActCtxSectionStringW",5},
        {"kernel32","FindActCtxSectionGuid",5},
        // msvcrt / ucrtbase
        {"msvcrt","printf",0},{"msvcrt","sprintf",0},{"msvcrt","snprintf",0},
        {"msvcrt","malloc",1},{"msvcrt","calloc",2},{"msvcrt","realloc",2},
        {"msvcrt","free",1},{"msvcrt","memcpy",3},{"msvcrt","memset",3},
        {"msvcrt","memmove",3},{"msvcrt","memcmp",3},{"msvcrt","strlen",1},
        {"msvcrt","strcmp",2},{"msvcrt","strcpy",2},{"msvcrt","strcat",2},
        {"msvcrt","fopen",2},{"msvcrt","fclose",1},{"msvcrt","fread",4},
        {"msvcrt","fwrite",4},{"msvcrt","fseek",3},{"msvcrt","ftell",1},
        {"msvcrt","exit",1},{"msvcrt","_exit",1},{"msvcrt","abort",0},
        {"msvcrt","_telli64",1},{"msvcrt","_ultoa",3},{"msvcrt","strspn",2},
        {"msvcrt","__CxxFrameHandler3",4},
        // d3d / dxgi
        {"d3d9","Direct3DCreate9",1},{"d3d9","Direct3DCreate9Ex",2},
        {"d3d11","D3D11CreateDevice",7},{"d3d11","D3D11CreateDeviceAndSwapChain",12},
        {"d3d12","D3D12CreateDevice",3},{"dxgi","CreateDXGIFactory",1},
        {"dxgi","CreateDXGIFactory1",1},{"dxgi","CreateDXGIFactory2",3},
        {"d3dcompiler_47","D3DCompile",10},{"d3dcompiler_47","D3DCompileFromFile",7},
        // input / audio
        {"dinput8","DirectInput8Create",4},{"xinput1_3","XInputGetState",2},
        {"xinput1_4","XInputGetState",2},{"dsound","DirectSoundCreate",3},
        {"dsound","DirectSoundCreate8",3},{"xaudio2_9","XAudio2Create",3},
        {"opengl32","wglGetProcAddress",1},{"opengl32","wglCreateContext",1},
        {"opengl32","wglDeleteContext",1},{"opengl32","wglMakeCurrent",2},
        // dstorage / dstoragecore
        {"dstorage","DStorageGetFactory",2},
        {"dstorage","DStorageSetConfiguration",1},
        {"dstorage","DStorageSetConfiguration1",1},
        {"dstorage","DStorageCreateCompressionCodec",4},
        // additional kernel32 / kernelbase PE32 coverage
        {"kernel32","GetOverlappedResult",4},{"kernel32","GetOverlappedResultEx",5},
        {"kernel32","CreateIoCompletionPort",4},{"kernel32","GetQueuedCompletionStatus",5},
        {"kernel32","GetQueuedCompletionStatusEx",6},{"kernel32","PostQueuedCompletionStatus",4},
        {"kernel32","CreateSemaphoreA",4},{"kernel32","CreateSemaphoreW",4},
        {"kernel32","OpenSemaphoreA",3},{"kernel32","OpenSemaphoreW",3},
        {"kernel32","OpenMutexA",3},{"kernel32","OpenMutexW",3},
        {"kernel32","OpenFileMappingW",3},{"kernel32","MapViewOfFileEx",6},
        {"kernel32","GetComputerNameA",2},{"kernel32","GetComputerNameW",2},
        {"kernel32","GetSystemDefaultLCID",0},{"kernel32","GetSystemDefaultLangID",0},
        {"kernel32","GetUserDefaultLCID",0},{"kernel32","GetUserDefaultLangID",0},
        {"kernel32","GetTempFileNameA",4},{"kernel32","GetTempFileNameW",4},
        {"kernel32","GetTempPath2W",2},{"kernel32","GetThreadId",1},
        {"kernel32","GetWindowsDirectoryW",2},{"kernel32","PulseEvent",1},
        {"kernel32","QueueUserAPC",3},{"kernel32","QueueUserWorkItem",3},
        {"kernel32","RemoveDirectoryW",1},{"kernel32","SetThreadContext",2},
        {"kernel32","GetThreadContext",2},{"kernel32","CreateFiber",3},
        {"kernel32","CreateFiberEx",5},{"kernel32","DeleteFiber",1},
        {"kernel32","SwitchToFiber",1},{"kernel32","ConvertThreadToFiber",1},
        {"kernel32","ConvertThreadToFiberEx",2},{"kernel32","ConvertFiberToThread",0},
        {"kernel32","GetCurrentFiber",0},{"kernel32","GetFiberData",0},
        {"kernel32","CreateThreadpool",1},{"kernel32","CloseThreadpool",1},
        {"kernel32","CreateThreadpoolWork",3},{"kernel32","CloseThreadpoolWork",1},
        {"kernel32","CreateThreadpoolCleanupGroup",0},{"kernel32","CloseThreadpoolCleanupGroup",1},
        {"kernel32","RegisterWaitForSingleObject",6},{"kernel32","RegisterWaitForSingleObjectEx",5},
        {"kernel32","UnregisterWait",1},{"kernel32","UnregisterWaitEx",2},
        {"kernel32","InitializeThreadpoolEnvironment",1},{"kernel32","DestroyThreadpoolEnvironment",1},
        {"kernel32","LeaveCriticalSectionWhenCallbackReturns",2},
        {"kernel32","ReleaseMutexWhenCallbackReturns",2},
        {"kernel32","ReleaseSemaphoreWhenCallbackReturns",3},
        {"kernel32","SetEventWhenCallbackReturns",2},
        {"kernel32","ProcessIdToSessionId",2},
        {"kernel32","PowerCreateRequest",1},{"kernel32","PowerSetRequest",2},
        {"kernel32","PowerClearRequest",2},
        // additional ntdll coverage
        {"ntdll","NtAllocateVirtualMemory",6},{"ntdll","NtAllocateVirtualMemoryEx",7},
        {"ntdll","NtFreeVirtualMemory",4},{"ntdll","NtProtectVirtualMemory",5},
        {"ntdll","NtQueryVirtualMemory",6},{"ntdll","NtClose",1},
        {"ntdll","NtCreateEvent",5},{"ntdll","NtCreateFile",11},
        {"ntdll","NtReadFile",9},{"ntdll","NtWriteFile",9},
        {"ntdll","NtWaitForSingleObject",3},{"ntdll","NtSetEvent",2},
        {"ntdll","NtTerminateProcess",2},{"ntdll","ZwTerminateProcess",2},
        {"ntdll","NtQueryInformationFile",5},{"ntdll","NtQueryInformationProcess",5},
        {"ntdll","NtQuerySystemInformation",4},{"ntdll","RtlGetVersion",1},
        {"ntdll","RtlVerifyVersionInfo",4},{"ntdll","RtlAddFunctionTable",3},
        {"ntdll","RtlDeleteFunctionTable",1},{"ntdll","RtlCaptureContext",1},
        {"ntdll","RtlLookupFunctionEntry",3},{"ntdll","RtlVirtualUnwind",8},
        {"ntdll","RtlPcToFileHeader",2},{"ntdll","RtlNtStatusToDosError",1},
        {"ntdll","RtlInitUnicodeString",2},{"ntdll","RtlInitAnsiString",2},
        {"ntdll","RtlFreeUnicodeString",1},{"ntdll","RtlFreeAnsiString",1},
        {"ntdll","RtlFreeOemString",1},{"ntdll","RtlAnsiStringToUnicodeString",3},
        {"ntdll","RtlUnicodeStringToAnsiString",3},{"ntdll","RtlCompareUnicodeString",3},
        {"ntdll","RtlEqualUnicodeString",3},{"ntdll","RtlUnwind",4},
        {"ntdll","RtlUnwindEx",6},{"ntdll","RtlExitUserProcess",1},
        {"ntdll","RtlGetLastWin32Error",0},{"ntdll","RtlSetLastWin32Error",1},
        // additional advapi32 + registry apiset coverage
        {"advapi32","RegOpenKeyA",3},{"advapi32","RegOpenKeyW",3},
        {"advapi32","RegCreateKeyA",3},{"advapi32","RegCreateKeyW",3},
        {"advapi32","RegDeleteKeyW",2},{"advapi32","RegDeleteKeyExA",4},
        {"advapi32","RegDeleteKeyExW",4},{"advapi32","RegDeleteTreeA",2},
        {"advapi32","RegDeleteTreeW",2},{"advapi32","RegDeleteValueA",2},
        {"advapi32","RegDeleteValueW",2},{"advapi32","RegEnumKeyA",4},
        {"advapi32","RegEnumKeyW",4},{"advapi32","RegEnumKeyExW",8},
        {"advapi32","RegEnumValueW",8},{"advapi32","RegFlushKey",1},
        {"advapi32","RegGetValueA",7},{"advapi32","RegGetValueW",7},
        {"advapi32","RegLoadKeyA",3},{"advapi32","RegLoadKeyW",3},
        {"advapi32","RegSaveKeyA",3},{"advapi32","RegSaveKeyW",3},
        {"advapi32","RegOpenCurrentUser",2},{"advapi32","RegQueryInfoKeyA",12},
        {"advapi32","RegQueryInfoKeyW",12},{"advapi32","RegConnectRegistryW",3},
        {"advapi32","RegDisablePredefinedCache",0},{"advapi32","RegQueryValueA",4},
        {"advapi32","RegQueryValueW",4},{"advapi32","RegSetValueA",5},
        {"advapi32","RegSetValueW",5},{"advapi32","RegSetKeyValueA",6},
        {"advapi32","RegSetKeyValueW",6},{"advapi32","RegDeleteKeyValueA",3},
        {"advapi32","RegDeleteKeyValueW",3},{"advapi32","RegNotifyChangeKeyValue",5},
        {"advapi32","RegOverridePredefKey",2},{"advapi32","GetUserNameA",2},
        {"advapi32","GetUserNameW",2},{"advapi32","CryptAcquireContextA",5},
        {"advapi32","CryptAcquireContextW",5},{"advapi32","CryptReleaseContext",2},
        {"advapi32","CryptGenRandom",3},{"advapi32","CryptCreateHash",5},
        {"advapi32","CryptHashData",4},{"advapi32","CryptGetHashParam",5},
        {"advapi32","CryptSetHashParam",4},{"advapi32","CryptDestroyHash",1},
        {"advapi32","CryptDestroyKey",1},{"advapi32","CryptDecrypt",6},
        {"advapi32","CryptExportKey",6},{"advapi32","CryptGetProvParam",5},
        {"advapi32","CryptGetUserKey",3},{"advapi32","CryptEnumProvidersW",6},
        {"advapi32","CryptSignHashW",6},{"advapi32","SystemFunction036",2},
        {"advapi32","AllocateAndInitializeSid",11},{"advapi32","FreeSid",1},
        {"advapi32","EqualSid",2},{"advapi32","IsValidSid",1},
        {"advapi32","GetLengthSid",1},{"advapi32","GetSidSubAuthority",2},
        {"advapi32","GetSidSubAuthorityCount",1},{"advapi32","ConvertSidToStringSidW",2},
        {"advapi32","ConvertStringSecurityDescriptorToSecurityDescriptorW",4},
        {"advapi32","InitializeSecurityDescriptor",2},{"advapi32","SetSecurityDescriptorDacl",4},
        {"advapi32","CheckTokenMembership",3},{"advapi32","LookupPrivilegeValueA",3},
        {"advapi32","LookupPrivilegeValueW",3},{"advapi32","AdjustTokenPrivileges",6},
        {"advapi32","LsaNtStatusToWinError",1},
        // additional gdi32 coverage
        {"gdi32","CombineRgn",4},{"gdi32","CreateBitmap",5},
        {"gdi32","CreateBrushIndirect",1},{"gdi32","CreateCompatibleBitmap",3},
        {"gdi32","CreateDCA",4},{"gdi32","CreateDCW",4},
        {"gdi32","CreateDIBPatternBrushPt",2},{"gdi32","CreateEllipticRgn",4},
        {"gdi32","CreatePatternBrush",1},{"gdi32","CreatePenIndirect",1},
        {"gdi32","CreatePolygonRgn",3},{"gdi32","CreateRectRgn",4},
        {"gdi32","CreateRectRgnIndirect",1},{"gdi32","DescribePixelFormat",4},
        {"gdi32","Ellipse",5},{"gdi32","ExtCreatePen",5},
        {"gdi32","ExtEscape",6},{"gdi32","ExtSelectClipRgn",3},
        {"gdi32","ExtTextOutW",8},{"gdi32","GdiFlush",0},
        {"gdi32","GetClipBox",2},{"gdi32","GetDC",1},
        {"gdi32","GetDIBits",7},{"gdi32","GetDeviceGammaRamp",2},
        {"gdi32","GetICMProfileW",3},{"gdi32","GetKerningPairsW",3},
        {"gdi32","GetMapMode",1},{"gdi32","GetObjectW",3},
        {"gdi32","GetPixelFormat",1},{"gdi32","GetRgnBox",2},
        {"gdi32","GetStretchBltMode",1},{"gdi32","GetTextExtentPoint32A",4},
        {"gdi32","GetTextExtentPoint32W",4},{"gdi32","GetTextMetricsA",2},
        {"gdi32","GetTextMetricsW",2},{"gdi32","LineTo",3},
        {"gdi32","MoveToEx",4},{"gdi32","OffsetRgn",3},
        {"gdi32","PatBlt",6},{"gdi32","Polygon",3},
        {"gdi32","Polyline",3},{"gdi32","PtInRegion",3},
        {"gdi32","RectInRegion",2},{"gdi32","ReleaseDC",2},
        {"gdi32","RemoveFontResourceExW",3},{"gdi32","RestoreDC",2},
        {"gdi32","RoundRect",7},{"gdi32","SaveDC",1},
        {"gdi32","SelectClipRgn",2},{"gdi32","SetDIBits",7},
        {"gdi32","SetDIBitsToDevice",12},{"gdi32","SetDeviceGammaRamp",2},
        {"gdi32","SetStretchBltMode",2},{"gdi32","SetViewportOrgEx",4},
        {"gdi32","SetWindowOrgEx",4},{"gdi32","StretchDIBits",13},
        // additional ws2_32 / wsock32 coverage
        {"ws2_32","WSAGetLastError",0},{"ws2_32","WSASetLastError",1},
        {"ws2_32","sendto",6},{"ws2_32","recvfrom",6},
        {"ws2_32","setsockopt",5},{"ws2_32","getsockopt",5},
        {"ws2_32","getpeername",3},{"ws2_32","getsockname",3},
        {"ws2_32","shutdown",2},{"ws2_32","gethostbyname",1},
        {"ws2_32","gethostbyaddr",3},{"ws2_32","gethostname",2},
        {"ws2_32","getaddrinfo",4},{"ws2_32","freeaddrinfo",1},
        {"ws2_32","getnameinfo",7},{"ws2_32","inet_ntoa",1},
        {"ws2_32","htonl",1},{"ws2_32","ntohl",1},
        {"ws2_32","WSAIoctl",9},{"ws2_32","WSAPoll",3},
        {"ws2_32","WSAAsyncSelect",4},{"ws2_32","__WSAFDIsSet",2},
        // steam
        {"steam_api","SteamAPI_Init",0},{"steam_api","SteamAPI_Shutdown",0},
        {"steam_api","SteamAPI_RunCallbacks",0},
        {"steam_api","SteamAPI_ISteamUserStats_RequestCurrentStats",1},
        {"steam_api","SteamAPI_GetSteamInstallPath",0},
        {"steam_api","SteamAPI_InitSafe",0},
        {"steam_api","SteamAPI_SetBreakpadAppID",1},
        {"steam_api","SteamAPI_SetMiniDumpComment",1},
        {"steam_api","SteamAPI_SetTryCatchCallbacks",1},
        {"steam_api","SteamAPI_UseBreakpadCrashHandler",1},
        {"steam_api","SteamAPI_WriteMiniDump",1},
        {"steam_api","SteamGameServer_BSecure",0},
        {"steam_api","SteamGameServer_GetHSteamPipe",0},
        {"steam_api","SteamGameServer_GetHSteamUser",0},
        {"steam_api","SteamGameServer_GetSteamID",0},
        {"steam_api","SteamGameServer_InitSafe",0},
        {"steam_api","SteamGameServer_RunCallbacks",0},
        {"steam_api","SteamGameServer_Shutdown",0},
        {"steam_api","Steam_GetHSteamUserCurrent",0},
        {"steam_api","Steam_RegisterInterfaceFuncs",2},
        {"steam_api","Steam_RunCallbacks",0},
        {"steam_api64","SteamAPI_Init",0},{"steam_api64","SteamAPI_Shutdown",0},
        {"steam_api64","SteamAPI_RunCallbacks",0},
        {"steam_api64","SteamAPI_ISteamUserStats_RequestCurrentStats",1},
        {"steam_api64","SteamAPI_GetSteamInstallPath",0},
        {"steam_api64","SteamAPI_InitSafe",0},
        {"steam_api64","SteamAPI_SetBreakpadAppID",1},
        {"steam_api64","SteamAPI_SetMiniDumpComment",1},
        {"steam_api64","SteamAPI_SetTryCatchCallbacks",1},
        {"steam_api64","SteamAPI_UseBreakpadCrashHandler",1},
        {"steam_api64","SteamAPI_WriteMiniDump",1},
        {"steam_api64","SteamGameServer_BSecure",0},
        {"steam_api64","SteamGameServer_GetHSteamPipe",0},
        {"steam_api64","SteamGameServer_GetHSteamUser",0},
        {"steam_api64","SteamGameServer_GetSteamID",0},
        {"steam_api64","SteamGameServer_InitSafe",0},
        {"steam_api64","SteamGameServer_RunCallbacks",0},
        {"steam_api64","SteamGameServer_Shutdown",0},
        {"steam_api64","Steam_GetHSteamUserCurrent",0},
        {"steam_api64","Steam_RegisterInterfaceFuncs",2},
        {"steam_api64","Steam_RunCallbacks",0},
        {"ws2_32","WSAIoctl",9},
        {"ws2_32","getnameinfo",7},
        {"ws2_32","freeaddrinfo",1},
        {"ws2_32","getaddrinfo",4},
        {"winmm","midiOutShortMsg",2},
        {"winmm","timeKillEvent",1},
        {"winmm","midiOutGetNumDevs",0},
        {"winmm","midiOutGetDevCapsW",3},
        {"winmm","midiOutGetDevCapsA",3},
        {"winmm","midiOutGetErrorTextA",3},{"winmm","midiOutGetErrorTextW",3},
        {"winmm","midiOutGetVolume",2},
        {"winmm","midiOutSetVolume",2},
        {"winmm","midiOutOpen",5},
        {"winmm","waveOutReset",1},
        {"winmm","waveOutWrite",3},
        {"winmm","waveOutUnprepareHeader",3},
        {"winmm","waveOutPrepareHeader",3},
        {"winmm","waveOutClose",1},
        {"winmm","waveOutOpen",6},
        {"winmm","timeGetDevCaps",2},
        {"winmm","midiOutReset",1},
        {"winmm","midiOutLongMsg",3},
        {"winmm","midiOutClose",1},
        {"winmm","midiOutUnprepareHeader",3},
        {"winmm","midiOutPrepareHeader",3},
        {"winmm","timeSetEvent",5},
        // winmm capture/output surface: waveIn and waveOut are REAL devices now,
        // so a PE32 caller needs the true arity. A guessed count pops the wrong
        // number of stack slots and corrupts the return address.
        {"winmm","waveInGetNumDevs",0},{"winmm","waveInGetDevCapsW",3},
        {"winmm","waveInGetDevCapsA",3},{"winmm","waveInOpen",6},
        {"winmm","waveInClose",1},{"winmm","waveInPrepareHeader",3},
        {"winmm","waveInUnprepareHeader",3},{"winmm","waveInAddBuffer",3},
        {"winmm","waveInStart",1},{"winmm","waveInStop",1},
        {"winmm","waveInReset",1},{"winmm","waveInMessage",4},
        {"winmm","waveInGetErrorTextW",3},{"winmm","waveInGetErrorTextA",3},
        {"winmm","waveOutGetNumDevs",0},{"winmm","waveOutGetDevCapsW",3},
        {"winmm","waveOutGetDevCapsA",3},{"winmm","waveOutGetPosition",3},
        {"winmm","waveOutSetVolume",2},{"winmm","waveOutGetVolume",2},
        {"winmm","waveOutPause",1},{"winmm","waveOutRestart",1},
        {"winmm","waveOutBreakLoop",1},{"winmm","waveOutMessage",4},
        {"winmm","waveOutGetErrorTextW",3},{"winmm","waveOutGetErrorTextA",3},
        {"winmm","joyGetNumDevs",0},{"winmm","joyGetPos",2},
        {"winmm","joyGetPosEx",2},{"winmm","joyGetDevCapsA",3},
        {"winmm","joyGetDevCapsW",3},{"winmm","joyGetThreshold",2},
        {"winmm","joySetThreshold",2},{"winmm","joySetCapture",4},
        {"winmm","joyReleaseCapture",1},{"winmm","joyConfigChanged",1},
        // msacm32 decodes for real now, so the 32-bit dispatcher must know
        // every one of its arities instead of guessing four arguments.
        {"msacm32","acmGetVersion",0},{"msacm32","acmStreamOpen",8},
        {"msacm32","acmStreamPrepareHeader",3},
        {"msacm32","acmStreamUnprepareHeader",3},
        {"msacm32","acmStreamConvert",3},{"msacm32","acmStreamSize",4},
        {"msacm32","acmStreamReset",2},{"msacm32","acmStreamClose",2},
        {"msacm32","acmFormatSuggest",5},{"msacm32","acmFormatTagEnumW",4},
        {"msacm32","acmFormatTagEnumA",4},{"msacm32","acmFormatEnumW",5},
        {"msacm32","acmFormatEnumA",5},{"msacm32","acmGetDriverHandle",2},
        {"msacm32","acmDriverEnum",3},{"msacm32","acmDriverID",3},
        {"imm32","ImmSetCompositionWindow",2},
        {"imm32","ImmNotifyIME",4},
        {"imm32","ImmGetCompositionStringW",4},
        {"imm32","ImmReleaseContext",2},
        {"imm32","ImmGetContext",1},
        {"imm32","ImmGetProperty",2},
        {"imm32","ImmSetCandidateWindow",2},
        {"usp10","ScriptGetLogicalWidths",6},
        {"usp10","ScriptFreeCache",1},
        {"usp10","ScriptItemize",7},
        {"usp10","ScriptLayout",4},
        {"usp10","ScriptShape",10},
        {"usp10","ScriptPlace",9},
        {"usp10","ScriptBreak",4},
        {"winhttp","WinHttpOpen",5},
        {"winhttp","WinHttpCloseHandle",1},
        {"winhttp","WinHttpConnect",4},
        {"winhttp","WinHttpOpenRequest",7},
        {"winhttp","WinHttpSendRequest",7},
        {"winhttp","WinHttpReceiveResponse",2},
        {"winhttp","WinHttpQueryDataAvailable",2},
        {"winhttp","WinHttpReadData",4},
        {"winhttp","WinHttpQueryHeaders",6},
        {"winhttp","WinHttpCrackUrl",4},
        {"winhttp","WinHttpSetStatusCallback",4},
        {"winhttp","WinHttpSetTimeouts",5},
        {"bcrypt","BCryptGenRandom",4},
        {"kernel32","SetConsoleCtrlHandler",2},
        {"kernel32","GetModuleHandleExW",3},
        {"kernel32","FreeLibraryAndExitThread",2},
        {"kernel32","CreatePipe",4},
        {"kernel32","IsThreadAFiber",0},
        {"kernel32","GetDateFormatW",6},
        {"kernel32","GetTimeFormatW",6},
        {"kernel32","LCMapStringW",6},
        {"kernel32","GetConsoleOutputCP",0},
        {"kernel32","InterlockedFlushSList",1},
        {"kernel32","InterlockedPushEntrySList",2},
        {"kernel32","RtlUnwind",4},
        {"kernel32","GetStringTypeW",4},
        {"kernel32","CompareStringEx",9},
        {"kernel32","LCMapStringEx",9},
        {"kernel32","CloseThreadpoolWait",1},
        {"kernel32","SetThreadpoolWait",3},
        {"kernel32","CreateThreadpoolWait",3},
        {"kernel32","CloseThreadpoolTimer",1},
        {"kernel32","WaitForThreadpoolTimerCallbacks",2},
        {"kernel32","SetThreadpoolTimer",4},
        {"kernel32","CreateThreadpoolTimer",3},
        {"kernel32","FreeLibraryWhenCallbackReturns",2},
        {"kernel32","FormatMessageW",7},
        {"kernel32","FormatMessageA",7},
        {"kernel32","ExpandEnvironmentStringsW",3},
        {"kernel32","AddVectoredExceptionHandler",2},
        {"kernel32","GetLocaleInfoEx",4},
        {"kernel32","GetCurrentDirectoryW",2},
        {"kernel32","GetUserDefaultLCID",0},
        {"kernel32","GetLogicalDriveStringsW",2},
        {"kernel32","SetErrorMode",1},
        {"kernel32","CompareStringW",6},
        {"kernel32","GetUserDefaultUILanguage",0},
        {"kernel32","AllocConsole",0},
        {"kernel32","FreeConsole",0},
        {"kernel32","GetConsoleScreenBufferInfo",2},
        {"kernel32","SetConsoleScreenBufferSize",2},
        {"kernel32","SignalObjectAndWait",4},
        {"kernel32","VerSetConditionMask",4},
        {"kernel32","VerifyVersionInfoW",4},
        {"kernel32","SetConsoleTitleW",1},
        {"kernel32","SetConsoleOutputCP",1},
        {"kernel32","ReleaseSemaphore",3},
        {"kernel32","CreateSemaphoreW",4},
        {"kernel32","CreateSemaphoreExW",6},
        {"kernel32","OpenThread",3},
        {"kernel32","GetProcessId",1},
        {"kernel32","GetThreadContext",2},
        {"kernel32","VirtualQueryEx",4},
        {"kernel32","SetNamedPipeHandleState",4},
        {"kernel32","TransactNamedPipe",7},
        {"kernel32","WaitNamedPipeW",2},
        {"kernel32","EnumSystemLocalesW",2},
        {"kernel32","FlushProcessWriteBuffers",0},
        {"kernel32","CreateEventExW",4},
        {"kernel32","InitOnceExecuteOnce",4},
        {"kernel32","CreateSymbolicLinkW",3},
        {"kernel32","CreateHardLinkW",3},
        {"kernel32","MoveFileExW",3},
        {"kernel32","CopyFileW",3},
        {"kernel32","CreateDirectoryExW",3},
        {"kernel32","DeviceIoControl",8},
        {"kernel32","AreFileApisANSI",0},
        {"kernel32","SetFileTime",4},
        {"kernel32","SetFileInformationByHandle",4},
        {"kernel32","SetFileAttributesW",2},
        {"kernel32","GetFinalPathNameByHandleW",4},
        {"kernel32","GetFileInformationByHandle",2},
        {"kernel32","GetFileAttributesExW",3},
        {"kernel32","FindFirstFileExW",6},
        {"kernel32","SetCurrentDirectoryW",1},
        {"kernel32","TryAcquireSRWLockShared",1},
        {"kernel32","AcquireSRWLockShared",1},
        {"kernel32","ReleaseSRWLockShared",1},
        {"kernel32","SleepConditionVariableSRW",4},
        {"kernel32","WakeAllConditionVariable",1},
        {"kernel32","WakeConditionVariable",1},
        {"kernel32","TryAcquireSRWLockExclusive",1},
        {"kernel32","AcquireSRWLockExclusive",1},
        {"kernel32","ReleaseSRWLockExclusive",1},
        {"kernel32","GetNativeSystemInfo",1},
        {"kernel32","SwitchToThread",0},
        {"kernel32","WaitForSingleObjectEx",3},
        {"kernel32","GetEnvironmentStringsW",0},
        {"kernel32","FreeEnvironmentStringsW",1},
        {"kernel32","IsValidLocale",2},
        {"kernel32","GetDiskFreeSpaceExW",4},
        {"kernel32","GetDiskFreeSpaceExA",4},
        {"kernel32","GetDriveTypeA",1},
        {"kernel32","FindFirstFileExA",6},
        {"kernel32","VerifyVersionInfoA",4},
        {"kernel32","ExpandEnvironmentStringsA",3},
        {"kernel32","GetSystemTimeAdjustment",3},
        {"kernel32","SetSystemTime",1},
        {"kernel32","SetThreadExecutionState",1},
        {"kernel32","GetFileInformationByHandleExW",4},
        {"kernel32","SetFileInformationByHandleW",4},
        {"kernel32","GetStringTypeA",5},
        {"kernel32","LCMapStringA",6},
        {"kernel32","GetWindowsDirectoryW",2},
        {"advapi32","RegisterEventSourceA",2},
        {"advapi32","ReportEventA",9},
        {"advapi32","RegCreateKeyTransactedW",11},
        {"ws2_32","WSARecvFrom",9},
        {"user32","CharLowerBuffA",2},
        {"d3d9","D3DPERF_QueryRepeatFrame",0},
        {"d3d9","D3DPERF_SetOptions",1},
        {"d3d9core","D3DPERF_QueryRepeatFrame",0},
        {"d3d9core","D3DPERF_SetOptions",1},
        {"dwmapi","DwmGetColorizationColor",2},
        {"dstorage","DStorageGetFactory",2},
        {"dstorage","DStorageSetConfiguration",1},
        {"dstorage","DStorageSetConfiguration1",1},
        {"dstorage","DStorageCreateCompressionCodec",4},
        {"dstoragecore","DStorageGetFactory",2},
        {"dstoragecore","DStorageSetConfiguration",1},
        {"dstoragecore","DStorageSetConfiguration1",1},
        {"dstoragecore","DStorageCreateCompressionCodec",4},
        {"hid","HidP_SetUsageValueArray",9},
        {"hid","HidP_InitializeReportForID",5},
        // Timer queues + kernel32 gaps (Sekiro / Cuphead / Unity Mono)
        {"kernel32","CreateTimerQueue",0},
        {"kernel32","DeleteTimerQueue",1},
        {"kernel32","DeleteTimerQueueEx",2},
        {"kernel32","CreateTimerQueueTimer",7},
        {"kernel32","ChangeTimerQueueTimer",4},
        {"kernel32","DeleteTimerQueueTimer",3},
        {"kernel32","CreateProcessA",10},
        {"kernel32","VerLanguageNameW",3},
        {"kernel32","VerLanguageNameA",3},
        {"kernel32","GetPriorityClass",1},
        {"kernel32","SetPriorityClass",2},
        {"kernel32","GetProcessWorkingSetSize",3},
        {"kernel32","SetProcessWorkingSetSize",3},
        {"kernel32","SetHandleCount",1},
        {"kernel32","OpenFile",3},
        {"kernel32","ReadDirectoryChangesW",8},
        {"kernel32","RtlInstallFunctionTableCallback",6},
        {"kernel32","FlushConsoleInputBuffer",1},
        {"kernel32","ReadConsoleInputA",4},
        {"kernel32","SetConsoleTextAttribute",2},
        // user32 gaps
        {"user32","FindWindowExW",4},
        {"user32","FindWindowExA",4},
        {"user32","SendInput",3},
        {"user32","WaitForInputIdle",2},
        {"user32","CopyImage",5},
        {"user32","DragDetect",3},
        {"user32","CreateDialogParamW",5},
        {"user32","CreateDialogParamA",5},
        {"user32","InsertMenuW",5},
        {"user32","GetMenuItemInfoW",4},
        {"user32","DeleteMenu",3},
        {"user32","GetClipboardFormatNameA",3},
        // advapi32 security/token/ACL gaps
        {"advapi32","OpenThreadToken",4},
        {"advapi32","DuplicateToken",3},
        {"advapi32","ImpersonateLoggedOnUser",1},
        {"advapi32","RevertToSelf",0},
        {"advapi32","LookupAccountSidW",7},
        {"advapi32","CopySid",3},
        {"advapi32","GetNamedSecurityInfoW",8},
        {"advapi32","SetNamedSecurityInfoW",7},
        {"advapi32","SetEntriesInAclW",4},
        {"advapi32","BuildTrusteeWithSidW",2},
        {"advapi32","GetEffectiveRightsFromAclW",3},
        {"advapi32","CreateProcessWithLogonW",11},
        // d3dcompiler fallbacks
        {"d3dcompiler_47","D3DCreateBlob",2},
        {"d3dcompiler_47","D3DGetBlobPart",5},
        {"d3dcompiler_47","D3DStripShader",4},
        // BotWRecomp / Unity IL2CPP / Cuphead discovered gaps
        {"kernel32","QueryFullProcessImageNameW",4},
        {"kernel32","QueryFullProcessImageNameA",4},
        {"ntdll","NtQueryDirectoryFile",11},
        {"ntdll","ZwQueryDirectoryFile",11},
        {"ntdll","NtQueryDirectoryFileEx",10},
        {"ntdll","ZwQueryDirectoryFileEx",10},
        {"mfplat","MFCreateFile",5},
        {"mfplat","MFCreateSourceResolver",1},
        {"mf","MFCreateSourceResolver",1},
        {"evr","MFCreateSourceResolver",1},
        {"iphlpapi","GetIpAddrTable",3},
        {"shell32","ShellExecuteExA",1},
        {"shfolder","ShellExecuteExA",1},
        {"wsock32","#55",2},
        {"wsock32","#56",2},
        {"wsock32","getservbyname",2},
        {"wsock32","getservbyport",2},
        {"ws2_32","#55",2},
        {"ws2_32","#56",2},
        {"ws2_32","getservbyname",2},
        {"ws2_32","getservbyport",2},
        // kernel32 resource / date-time / user32 icon / dnsapi
        {"kernel32","FindResourceW",3},
        {"kernel32","FindResourceExW",4},
        {"kernel32","FindResourceA",3},
        {"kernel32","FindResourceExA",4},
        {"kernel32","SizeofResource",2},
        {"kernel32","LoadResource",2},
        {"kernel32","LockResource",1},
        {"kernel32","UnlockResource",1},
        {"kernel32","FreeResource",1},
        {"kernel32","EnumResourceNamesW",4},
        {"kernel32","LoadStringW",4},
        {"kernel32","GetDateFormatA",6},
        {"kernel32","GetTimeFormatA",6},
        {"user32","CreateIconFromResource",4},
        {"dnsapi","DnsFree",2},
        {"dnsapi","DnsRecordListFree",2},
        {"dnsapi","DnsQuery_A",6},
        {"dnsapi","DnsQuery_W",6},
        {"dnsapi","DnsQuery_UTF8",6},
        {"user32","IsClipboardFormatAvailable",1},
        {"user32","SetWindowPos",7},
        {"user32","EnumDisplayMonitors",4},
        {"user32","GetMonitorInfoW",2},
        {"user32","MonitorFromWindow",2},
        {"user32","EnumDisplaySettingsW",3},
        {"user32","ChangeDisplaySettingsW",2},
        {"user32","SetRectEmpty",1},
        {"user32","SetRect",5},
        {"user32","ClientToScreen",2},
        {"user32","ValidateRect",2},
        {"user32","GetUpdateRect",3},
        {"user32","GetWindowDC",1},
        {"user32","TrackMouseEvent",1},
        {"user32","GetKeyboardLayout",1},
        {"user32","GetClipboardData",1},
        {"user32","CloseClipboard",0},
        {"user32","OpenClipboard",1},
        {"user32","GetActiveWindow",0},
        {"user32","SendDlgItemMessageW",5},
        {"user32","SetDlgItemTextW",3},
        {"user32","GetDlgItem",2},
        {"user32","DialogBoxParamW",5},
        {"gdi32","SetDIBColorTable",4},
        {"gdi32","GetDIBColorTable",4},
        {"gdi32","CreateDIBSection",6},
        {"gdi32","SetPixelFormat",3},
        {"gdi32","SelectPalette",3},
        {"gdi32","GetPaletteEntries",4},
        {"gdi32","SetPaletteEntries",4},
        {"gdi32","SetMapMode",2},
        {"gdi32","AddFontResourceExW",4},
        {"gdi32","SwapBuffers",1},
        {"gdi32","GetOutlineTextMetricsW",3},
        {"gdi32","GetGlyphOutlineW",7},
        {"gdi32","EnumFontFamiliesExW",5},
        {"gdi32","CreateFontIndirectW",1},
        {"gdi32","GetGlyphIndicesW",5},
        {"gdi32","CreatePalette",1},
        {"gdi32","RealizePalette",1},
        {"gdi32","ChoosePixelFormat",2},
        // kernel32 / kernelbase remaining threadpool, VirtualAlloc2, WER
        {"kernel32","CallbackMayRunLong",1},{"kernel32","CloseThreadpoolCleanupGroupMembers",3},
        {"kernel32","CreateFileMappingNumaW",7},{"kernel32","DisassociateCurrentThreadFromCallback",1},
        {"kernel32","IsThreadpoolTimerSet",1},{"kernel32","QueueUserAPC2",4},
        {"kernel32","SetThreadpoolCallbackCleanupGroup",3},{"kernel32","SetThreadpoolCallbackPool",2},
        {"kernel32","SetThreadpoolCallbackRunsLong",1},{"kernel32","SetThreadpoolThreadMaximum",2},
        {"kernel32","SetThreadpoolThreadMinimum",2},{"kernel32","SetThreadpoolTimerEx",4},
        {"kernel32","SetThreadpoolWaitEx",4},{"kernel32","SubmitThreadpoolWork",1},
        {"kernel32","TrySubmitThreadpoolCallback",3},{"kernel32","VirtualAlloc2",7},
        {"kernel32","VirtualAlloc2FromApp",7},{"kernel32","VirtualAllocEx",5},
        {"kernel32","VirtualFreeEx",4},{"kernel32","VirtualProtectEx",5},
        {"kernel32","WaitForMultipleObjectsEx",5},{"kernel32","WaitForThreadpoolWaitCallbacks",2},
        {"kernel32","WaitForThreadpoolWorkCallbacks",2},{"kernel32","WerGetFlags",2},
        {"kernel32","WerRegisterMemoryBlock",2},{"kernel32","WerRegisterRuntimeExceptionModule",2},
        {"kernel32","WerSetFlags",1},{"kernel32","WerUnregisterMemoryBlock",1},
        {"kernel32","WerUnregisterRuntimeExceptionModule",2},
        // advapi32 / sechost (SCM, ETW, LSA, SIDs)
        {"advapi32","CloseServiceHandle",1},{"advapi32","ControlService",3},
        {"advapi32","ConvertSidToStringSidW",2},{"advapi32","ConvertStringSidToSidA",2},
        {"advapi32","ConvertStringSidToSidW",2},{"advapi32","CreateServiceA",13},
        {"advapi32","CreateServiceW",13},{"advapi32","DeleteService",1},
        {"advapi32","EnumServicesStatusExA",10},{"advapi32","EnumServicesStatusExW",10},
        {"advapi32","EventEnabled",2},{"advapi32","EventProviderEnabled",3},
        {"advapi32","EventRegister",4},{"advapi32","EventSetInformation",4},
        {"advapi32","EventUnregister",1},{"advapi32","EventWrite",4},
        {"advapi32","EventWriteEx",8},{"advapi32","EventWriteString",4},
        {"advapi32","EventWriteTransfer",6},{"advapi32","LookupAccountNameA",7},
        {"advapi32","LookupAccountNameW",7},{"advapi32","LookupAccountSidA",7},
        {"advapi32","LookupAccountSidW",7},{"advapi32","LookupPrivilegeValueA",3},
        {"advapi32","LookupPrivilegeValueW",3},{"advapi32","LsaClose",1},
        {"advapi32","LsaFreeMemory",1},{"advapi32","LsaLookupNames2",6},
        {"advapi32","LsaLookupSids",5},{"advapi32","LsaLookupSids2",6},
        {"advapi32","LsaOpenPolicy",4},{"advapi32","LsaQueryInformationPolicy",3},
        {"advapi32","NotifyServiceStatusChangeA",3},{"advapi32","NotifyServiceStatusChangeW",3},
        {"advapi32","OpenSCManagerA",3},{"advapi32","OpenSCManagerW",3},
        {"advapi32","OpenServiceA",3},{"advapi32","OpenServiceW",3},
        {"advapi32","QueryServiceConfigA",4},{"advapi32","QueryServiceConfigW",4},
        {"advapi32","QueryServiceStatus",2},{"advapi32","QueryServiceStatusEx",5},
        {"advapi32","RegisterServiceCtrlHandlerA",2},{"advapi32","RegisterServiceCtrlHandlerW",2},
        {"advapi32","RegisterServiceCtrlHandlerExA",3},{"advapi32","RegisterServiceCtrlHandlerExW",3},
        {"advapi32","SetServiceStatus",2},{"advapi32","StartServiceA",3},
        {"advapi32","StartServiceW",3},{"advapi32","StartServiceCtrlDispatcherA",1},
        {"advapi32","StartServiceCtrlDispatcherW",1},
        // combase / api-ms-win-core-winrt-*
        {"combase","CoCreateFreeThreadedMarshaler",2},{"combase","CoCreateGuid",1},
        {"combase","CoCreateInstance",5},{"combase","CoFreeUnusedLibraries",0},
        {"combase","CoGetApartmentType",2},{"combase","CoGetClassObject",5},
        {"combase","CoInitializeEx",2},{"combase","CoTaskMemAlloc",1},
        {"combase","CoTaskMemFree",1},{"combase","CoTaskMemRealloc",2},
        {"combase","CoUninitialize",0},{"combase","GetRestrictedErrorInfo",1},
        {"combase","PropVariantClear",1},{"combase","RoActivateInstance",2},
        {"combase","RoGetActivationFactory",3},{"combase","RoGetAgileReference",4},
        {"combase","RoGetBufferMarshaler",1},{"combase","RoInitialize",1},
        {"combase","RoOriginateError",2},{"combase","RoOriginateErrorW",3},
        {"combase","RoSetErrorReportingFlags",1},{"combase","RoTransformError",3},
        {"combase","RoUninitialize",0},{"combase","SetRestrictedErrorInfo",1},
        {"combase","StringFromGUID2",3},{"combase","WindowsCompareStringOrdinal",3},
        {"combase","WindowsConcatString",3},{"combase","WindowsCreateString",3},
        {"combase","WindowsCreateStringReference",4},{"combase","WindowsDeleteString",1},
        {"combase","WindowsDuplicateString",2},{"combase","WindowsGetStringLen",1},
        {"combase","WindowsGetStringRawBuffer",2},{"combase","WindowsIsStringEmpty",1},
        // ole32
        {"ole32","CLSIDFromProgID",2},{"ole32","CLSIDFromString",2},
        {"ole32","CoCreateFreeThreadedMarshaler",2},{"ole32","CoCreateGuid",1},
        {"ole32","CoFreeUnusedLibraries",0},{"ole32","CoFreeUnusedLibrariesEx",2},
        {"ole32","CoGetApartmentType",2},{"ole32","CoGetClassObject",5},
        {"ole32","CoGetContextToken",1},{"ole32","CoGetCurrentProcess",0},
        {"ole32","CoGetInterfaceAndReleaseStream",3},{"ole32","CoGetObjectContext",2},
        {"ole32","CoInitializeSecurity",9},{"ole32","CoMarshalInterThreadInterfaceInStream",3},
        {"ole32","CoRegisterInitializeSpy",2},{"ole32","CoRevokeInitializeSpy",2},
        {"ole32","CoSetProxyBlanket",8},{"ole32","CoTaskMemRealloc",2},
        {"ole32","CoTaskMemSize",1},{"ole32","CoWaitForMultipleHandles",5},
        {"ole32","CreateStreamOnHGlobal",3},{"ole32","GetErrorInfo",2},
        {"ole32","GetHGlobalFromStream",2},{"ole32","IIDFromString",2},
        {"ole32","OleFlushClipboard",0},{"ole32","OleGetClipboard",1},
        {"ole32","OleInitialize",1},{"ole32","OleSetClipboard",1},
        {"ole32","OleUninitialize",0},{"ole32","ProgIDFromCLSID",2},
        {"ole32","PropVariantClear",1},{"ole32","PropVariantCopy",2},
        {"ole32","PropVariantInit",1},{"ole32","RegisterDragDrop",2},
        {"ole32","ReleaseStgMedium",1},{"ole32","RevokeDragDrop",1},
        {"ole32","RoGetAgileReference",4},{"ole32","SetErrorInfo",2},
        {"ole32","StringFromCLSID",2},{"ole32","StringFromGUID2",3},
        // oleaut32 (named + ordinals)
        {"oleaut32","GetErrorInfo",2},{"oleaut32","SetErrorInfo",2},
        {"oleaut32","SafeArrayAccessData",2},{"oleaut32","SafeArrayCreateVector",3},
        {"oleaut32","SafeArrayDestroy",1},{"oleaut32","SafeArrayGetDim",1},
        {"oleaut32","SafeArrayGetElement",3},{"oleaut32","SafeArrayGetLBound",3},
        {"oleaut32","SafeArrayGetUBound",3},{"oleaut32","SafeArrayGetVartype",2},
        {"oleaut32","SafeArrayLock",1},{"oleaut32","SafeArrayPutElement",3},
        {"oleaut32","SafeArrayUnaccessData",1},{"oleaut32","SafeArrayUnlock",1},
        {"oleaut32","SysAllocStringByteLen",2},{"oleaut32","SysAllocStringLen",2},
        {"oleaut32","SysReAllocString",2},{"oleaut32","SysReAllocStringLen",3},
        {"oleaut32","SysStringByteLen",1},{"oleaut32","VariantCopy",2},
        {"oleaut32","#2",1},{"oleaut32","#3",2},{"oleaut32","#4",2},{"oleaut32","#5",3},
        {"oleaut32","#6",1},{"oleaut32","#7",1},{"oleaut32","#8",1},{"oleaut32","#9",1},
        {"oleaut32","#10",2},{"oleaut32","#12",4},{"oleaut32","#15",3},{"oleaut32","#16",1},
        {"oleaut32","#17",1},{"oleaut32","#19",3},{"oleaut32","#20",3},{"oleaut32","#21",1},
        {"oleaut32","#22",1},{"oleaut32","#23",2},{"oleaut32","#24",1},{"oleaut32","#25",3},
        {"oleaut32","#26",3},{"oleaut32","#77",2},{"oleaut32","#148",3},{"oleaut32","#149",1},
        {"oleaut32","#150",2},{"oleaut32","#200",2},{"oleaut32","#201",2},{"oleaut32","#411",3},
        // ws2_32 / wsock32 ordinals + legacy async APIs
        {"ws2_32","#1",3},{"ws2_32","#2",3},{"ws2_32","#3",1},{"ws2_32","#4",3},
        {"ws2_32","#5",3},{"ws2_32","#6",3},{"ws2_32","#7",5},{"ws2_32","#8",1},
        {"ws2_32","#9",1},{"ws2_32","#10",3},{"ws2_32","#11",1},{"ws2_32","#12",1},
        {"ws2_32","#13",2},{"ws2_32","#14",1},{"ws2_32","#15",1},{"ws2_32","#16",4},
        {"ws2_32","#17",6},{"ws2_32","#18",5},{"ws2_32","#19",4},{"ws2_32","#20",6},
        {"ws2_32","#21",5},{"ws2_32","#22",2},{"ws2_32","#23",3},{"ws2_32","#51",3},
        {"ws2_32","#52",1},{"ws2_32","#53",1},{"ws2_32","#55",2},{"ws2_32","#56",2},
        {"ws2_32","#57",2},{"ws2_32","#103",5},{"ws2_32","#108",1},{"ws2_32","#111",0},
        {"ws2_32","#112",1},{"ws2_32","#115",2},{"ws2_32","#116",0},{"ws2_32","#151",2},
        {"ws2_32","WSAAsyncGetHostByName",5},{"ws2_32","WSACancelAsyncRequest",1},
        {"ws2_32","getprotobyname",1},
        // openal32
        {"openal32","alBufferData",5},{"openal32","alDeleteBuffers",2},
        {"openal32","alDeleteSources",2},{"openal32","alDisable",1},
        {"openal32","alDistanceModel",1},{"openal32","alDopplerFactor",1},
        {"openal32","alEnable",1},{"openal32","alGenBuffers",2},
        {"openal32","alGenSources",2},{"openal32","alGetBufferi",3},
        {"openal32","alGetEnumValue",1},{"openal32","alGetError",0},
        {"openal32","alGetListener3f",4},{"openal32","alGetListenerf",2},
        {"openal32","alGetListenerfv",2},{"openal32","alGetProcAddress",1},
        {"openal32","alGetSourcef",3},{"openal32","alGetSourcei",3},
        {"openal32","alGetString",1},{"openal32","alIsBuffer",1},
        {"openal32","alIsEnabled",1},{"openal32","alIsExtensionPresent",1},
        {"openal32","alIsSource",1},{"openal32","alListener3f",4},
        {"openal32","alListenerf",2},{"openal32","alListenerfv",2},
        {"openal32","alSource3f",5},{"openal32","alSourcePause",1},
        {"openal32","alSourcePausev",2},{"openal32","alSourcePlay",1},
        {"openal32","alSourcePlayv",2},{"openal32","alSourceQueueBuffers",3},
        {"openal32","alSourceStop",1},{"openal32","alSourceStopv",2},
        {"openal32","alSourceUnqueueBuffers",3},{"openal32","alSourcef",3},
        {"openal32","alSourcei",3},{"openal32","alSpeedOfSound",1},
        {"openal32","alcCloseDevice",1},{"openal32","alcCreateContext",2},
        {"openal32","alcDestroyContext",1},{"openal32","alcGetError",1},
        {"openal32","alcGetIntegerv",4},{"openal32","alcGetString",2},
        {"openal32","alcMakeContextCurrent",1},{"openal32","alcOpenDevice",1},
        {"openal32","alcProcessContext",1},{"openal32","alcSuspendContext",1},
        {"openal32","trt_openal_pump",1},
        // uxtheme
        {"uxtheme","AllowDarkModeForWindow",2},{"uxtheme","BeginBufferedPaint",5},
        {"uxtheme","BufferedPaintInit",0},{"uxtheme","BufferedPaintSetAlpha",3},
        {"uxtheme","BufferedPaintUnInit",0},{"uxtheme","CloseThemeData",1},
        {"uxtheme","DrawThemeBackground",6},{"uxtheme","DrawThemeBackgroundEx",6},
        {"uxtheme","DrawThemeText",9},{"uxtheme","EnableThemeDialogTexture",2},
        {"uxtheme","EndBufferedPaint",2},{"uxtheme","GetCurrentThemeName",6},
        {"uxtheme","GetThemeBool",5},{"uxtheme","GetThemeColor",5},
        {"uxtheme","GetThemeInt",5},{"uxtheme","GetThemeMargins",7},
        {"uxtheme","GetThemePartSize",7},{"uxtheme","GetThemeSysColor",2},
        {"uxtheme","IsAppThemed",0},{"uxtheme","IsThemeActive",0},
        {"uxtheme","IsThemeBackgroundPartiallyTransparent",3},{"uxtheme","IsThemePartDefined",3},
        {"uxtheme","OpenThemeData",2},{"uxtheme","OpenThemeDataEx",3},
        {"uxtheme","OpenThemeDataForDpi",3},{"uxtheme","SetPreferredAppMode",1},
        {"uxtheme","SetWindowTheme",3},{"uxtheme","ShouldAppsUseDarkMode",0},
        // d3d12 / d3d12core
        {"d3d12","D3D12CoreCreateLayeredDevice",5},{"d3d12","D3D12CoreGetLayeredDeviceSize",2},
        {"d3d12","D3D12CoreRegisterLayers",2},{"d3d12","D3D12CreateRootSignatureDeserializer",4},
        {"d3d12","D3D12CreateVersionedRootSignatureDeserializer",4},{"d3d12","D3D12EnableExperimentalFeatures",4},
        {"d3d12","D3D12GetDebugInterface",2},{"d3d12","D3D12GetInterface",3},
        {"d3d12","D3D12PIXEventsReplaceBlock",1},{"d3d12","D3D12PIXGetThreadInfo",0},
        {"d3d12","D3D12PIXNotifyWakeFromFenceSignal",1},{"d3d12","D3D12PIXReportCounter",4},
        {"d3d12","D3D12SerializeRootSignature",4},{"d3d12","D3D12SerializeVersionedRootSignature",3},
        {"d3d12","#101",0},{"d3d12","#102",0},{"d3d12","#103",0},
        {"d3d12","#104",0},{"d3d12","#105",0},{"d3d12","#106",0},
        // fmod / fmodL / fmodex / fmodstudio / fmodstudioL
        {"fmod","FMOD_ChannelGroup_AddChannelGroup",2},{"fmod","FMOD_ChannelGroup_SetVolume",2},
        {"fmod","FMOD_Channel_GetPaused",2},{"fmod","FMOD_Channel_GetVolume",2},
        {"fmod","FMOD_Channel_IsPlaying",2},{"fmod","FMOD_Channel_SetFrequency",2},
        {"fmod","FMOD_Channel_SetPaused",2},{"fmod","FMOD_Channel_SetPosition",3},
        {"fmod","FMOD_Channel_SetVolume",2},{"fmod","FMOD_Channel_Stop",1},
        {"fmod","FMOD_Debug_Initialize",3},{"fmod","FMOD_Memory_Initialize",4},
        {"fmod","FMOD_Sound_GetLength",3},{"fmod","FMOD_Sound_GetMode",2},
        {"fmod","FMOD_Sound_Release",1},{"fmod","FMOD_Sound_SetMode",2},
        {"fmod","FMOD_System_Close",1},{"fmod","FMOD_System_Create",2},
        {"fmod","FMOD_System_CreateSound",5},{"fmod","FMOD_System_CreateStream",5},
        {"fmod","FMOD_System_GetMasterChannelGroup",2},{"fmod","FMOD_System_Init",4},
        {"fmod","FMOD_System_PlaySound",5},{"fmod","FMOD_System_Release",1},
        {"fmod","FMOD_System_SetDSPBufferSize",3},{"fmod","FMOD_System_SetOutput",2},
        {"fmod","FMOD_System_SetSpeakerMode",2},{"fmod","FMOD_System_Update",1},
        // bink / bink2w32 / bink2w64
        {"bink","BinkAllocateFrameBuffers",2},{"bink","BinkClose",1},
        {"bink","BinkCopyToBuffer",7},{"bink","BinkDoFrame",1},
        {"bink","BinkFreeFrameBuffers",1},{"bink","BinkGetAudioInfo",2},
        {"bink","BinkGetError",0},{"bink","BinkGetFrameNum",1},
        {"bink","BinkGetKeyFrame",3},{"bink","BinkGetRealtime",3},
        {"bink","BinkGetRects",2},{"bink","BinkGetSummary",2},
        {"bink","BinkGetVideoInfo",2},{"bink","BinkGoto",3},
        {"bink","BinkNextFrame",1},{"bink","BinkOpen",2},
        {"bink","BinkOpenDirect",2},{"bink","BinkPause",2},
        {"bink","BinkRegisterFrameBuffers",2},{"bink","BinkSetMixBinVolumes",5},
        {"bink","BinkSetMixBins",4},{"bink","BinkSetPan",3},
        {"bink","BinkSetSoundOnOff",2},{"bink","BinkSetSoundTrack",2},
        {"bink","BinkSetVideoOnOff",2},{"bink","BinkSetVolume",3},
        {"bink","BinkShouldSkip",1},{"bink","BinkWait",1},
        // wwise (AkSoundEngine / AkSoundEngineP / AkMemoryMgr / AkStreamMgr / AkMusicEngine)
        {"wwise","AK_MemoryMgr_Init",1},{"wwise","AK_MemoryMgr_Term",0},
        {"wwise","AK_MemoryMgr_Malloc",1},{"wwise","AK_MemoryMgr_Free",1},
        {"wwise","AK_StreamMgr_Create",1},{"wwise","AK_StreamMgr_Destroy",0},
        {"wwise","AK_StreamMgr_SetCurrentLanguage",1},{"wwise","AK_StreamMgr_SetBasePath",1},
        {"wwise","AK_StreamMgr_SetFileLocationResolver",1},
        {"wwise","AK_SoundEngine_Init",2},{"wwise","AK_SoundEngine_Term",0},
        {"wwise","AK_SoundEngine_IsInitialized",0},{"wwise","AK_SoundEngine_RenderAudio",1},
        {"wwise","AK_SoundEngine_GetDefaultInitSettings",1},{"wwise","AK_SoundEngine_GetDefaultPlatformInitSettings",1},
        {"wwise","AK_SoundEngine_RegisterGameObj",2},{"wwise","AK_SoundEngine_UnregisterGameObj",1},
        {"wwise","AK_SoundEngine_UnregisterAllGameObj",0},{"wwise","AK_SoundEngine_SetPosition",2},
        {"wwise","AK_SoundEngine_SetListenerPosition",2},{"wwise","AK_SoundEngine_SetDefaultListeners",2},
        {"wwise","AK_SoundEngine_LoadBank",2},{"wwise","AK_SoundEngine_LoadBankMemory",3},
        {"wwise","AK_SoundEngine_LoadBankMemoryView",3},{"wwise","AK_SoundEngine_LoadBankMemoryCopy",3},
        {"wwise","AK_SoundEngine_LoadBankID",1},{"wwise","AK_SoundEngine_UnloadBank",2},
        {"wwise","AK_SoundEngine_UnloadBankID",2},{"wwise","AK_SoundEngine_ClearBanks",0},
        {"wwise","AK_SoundEngine_PostEvent",5},{"wwise","AK_SoundEngine_PostEventID",5},
        {"wwise","AK_SoundEngine_ExecuteActionOnEvent",5},{"wwise","AK_SoundEngine_StopAll",1},
        {"wwise","AK_SoundEngine_StopPlayingID",3},{"wwise","AK_SoundEngine_SetRTPCValue",5},
        {"wwise","AK_SoundEngine_SetRTPCValueID",5},{"wwise","AK_SoundEngine_SetState",2},
        {"wwise","AK_SoundEngine_SetStateID",2},{"wwise","AK_SoundEngine_SetSwitch",3},
        {"wwise","AK_SoundEngine_SetSwitchID",3},{"wwise","AK_SoundEngine_GetIDFromString",1},
        {"wwise","AK_MusicEngine_Init",1},{"wwise","AK_MusicEngine_Term",0},
        {"wwise","AK_MusicEngine_GetDefaultInitSettings",1},
        {"wwise","CSharp_Init",2},{"wwise","CSharp_Term",0},
        {"wwise","CSharp_RenderAudio",1},{"wwise","CSharp_PostEvent",5},
        {"wwise","CSharp_LoadBank",2},{"wwise","CSharp_LoadBankMemoryView",3},
        // Epic Online Services (EOSSDK-Win64/32-Shipping.dll)
        {"eos","EOS_Initialize",1},{"eos","EOS_Shutdown",0},{"eos","EOS_GetVersion",0},
        {"eos","EOS_EResult_ToString",1},{"eos","EOS_EResult_IsOperationComplete",1},
        {"eos","EOS_Logging_SetCallback",1},{"eos","EOS_Logging_SetLogLevel",2},
        {"eos","EOS_Platform_Create",1},{"eos","EOS_Platform_Release",1},
        {"eos","EOS_Platform_Tick",1},{"eos","EOS_Platform_CheckForLauncherAndRestart",1},
        {"eos","EOS_Platform_SetNetworkStatus",2},{"eos","EOS_Platform_GetNetworkStatus",1},
        {"eos","EOS_Platform_SetApplicationStatus",2},{"eos","EOS_Platform_GetApplicationStatus",1},
        {"eos","EOS_Platform_GetActiveCountryCode",4},{"eos","EOS_Platform_GetActiveLocaleCode",4},
        {"eos","EOS_Platform_GetAuthInterface",1},{"eos","EOS_Platform_GetConnectInterface",1},
        {"eos","EOS_Platform_GetAchievementsInterface",1},{"eos","EOS_Platform_GetStatsInterface",1},
        {"eos","EOS_Platform_GetFriendsInterface",1},{"eos","EOS_Platform_GetUserInfoInterface",1},
        {"eos","EOS_Platform_GetPresenceInterface",1},{"eos","EOS_Platform_GetPlayerDataStorageInterface",1},
        {"eos","EOS_Platform_GetTitleStorageInterface",1},{"eos","EOS_Platform_GetUIInterface",1},
        {"eos","EOS_Platform_GetEcomInterface",1},{"eos","EOS_Platform_GetLobbyInterface",1},
        {"eos","EOS_Platform_GetSessionsInterface",1},{"eos","EOS_Platform_GetMetricsInterface",1},
        {"eos","EOS_Platform_GetP2PInterface",1},{"eos","EOS_Platform_GetLeaderboardsInterface",1},
        {"eos","EOS_Platform_GetProgressionSnapshotInterface",1},{"eos","EOS_Platform_GetReportsInterface",1},
        {"eos","EOS_Platform_GetSanctionsInterface",1},{"eos","EOS_Platform_GetRTCInterface",1},
        {"eos","EOS_Platform_GetModsInterface",1},{"eos","EOS_Platform_GetKWSInterface",1},
        {"eos","EOS_Platform_GetCustomInvitesInterface",1},{"eos","EOS_Platform_GetIntegratedPlatformInterface",1},
        {"eos","EOS_Platform_GetAntiCheatClientInterface",1},
        {"eos","EOS_Auth_Login",4},{"eos","EOS_Auth_GetLoggedInAccountsCount",1},
        {"eos","EOS_Auth_GetLoggedInAccountByIndex",2},{"eos","EOS_Auth_GetLoginStatus",2},
        {"eos","EOS_Auth_AddNotifyLoginStatusChanged",4},{"eos","EOS_Auth_RemoveNotifyLoginStatusChanged",2},
        {"eos","EOS_Connect_Login",4},{"eos","EOS_Connect_GetLoggedInUsersCount",1},
        {"eos","EOS_Connect_GetLoggedInUserByIndex",2},{"eos","EOS_Connect_GetLoginStatus",2},
        {"eos","EOS_Connect_AddNotifyAuthExpiration",4},{"eos","EOS_Connect_RemoveNotifyAuthExpiration",2},
        {"eos","EOS_Connect_AddNotifyLoginStatusChanged",4},{"eos","EOS_Connect_RemoveNotifyLoginStatusChanged",2},
        {"eos","EOS_Ecom_QueryOwnership",4},{"eos","EOS_Achievements_QueryDefinitions",4},
        {"eos","EOS_Achievements_GetAchievementDefinitionCount",2},
        {"eos","EOS_UI_AddNotifyDisplaySettingsUpdated",4},{"eos","EOS_UI_RemoveNotifyDisplaySettingsUpdated",2},
        {"eos","EOS_UI_SetToggleFriendsKey",2},{"eos","EOS_UI_SetDisplayPreference",2},
        {"eos","EOS_UI_GetFriendsVisible",2},{"eos","EOS_Metrics_BeginPlayerSession",2},
        {"eos","EOS_Metrics_EndPlayerSession",2},{"eos","EOS_EpicAccountId_IsValid",1},
        {"eos","EOS_ProductUserId_IsValid",1},{"eos","EOS_EpicAccountId_ToString",3},
        {"eos","EOS_ProductUserId_ToString",3},{"eos","EOS_EpicAccountId_FromString",1},
        {"eos","EOS_ProductUserId_FromString",1},{"eos","EOS_ByteArray_ToString",4},
        // GOG Galaxy (Galaxy64.dll / Galaxy.dll)
        {"galaxy","Galaxy_Init",1},{"galaxy","Galaxy_Shutdown",0},
        {"galaxy","Galaxy_ProcessData",0},{"galaxy","Galaxy_IsSignedIn",0},
        {"galaxy","Galaxy_IsLoggedOn",0},
        {"galaxy","?Init@api@galaxy@@YAXAEBUInitOptions@12@@Z",1},
        {"galaxy","?Shutdown@api@galaxy@@YAXXZ",0},
        {"galaxy","?ProcessData@api@galaxy@@YAXXZ",0},
        {"galaxy","?User@api@galaxy@@YAPEAVIUser@12@XZ",0},
        {"galaxy","?Friends@api@galaxy@@YAPEAVIFriends@12@XZ",0},
        {"galaxy","?Stats@api@galaxy@@YAPEAVIStats@12@XZ",0},
        {"galaxy","?Utils@api@galaxy@@YAPEAVIUtils@12@XZ",0},
        {"galaxy","?Apps@api@galaxy@@YAPEAVIApps@12@XZ",0},
        {"galaxy","?Storage@api@galaxy@@YAPEAVIStorage@12@XZ",0},
    };
    for (size_t i = 0; i < sizeof(k_args) / sizeof(k_args[0]); i++) {
        if (mod_eq(module, k_args[i].mod) && sym_eq(symbol, k_args[i].sym))
            return k_args[i].n;
    }

    // Helper macro for entries
    #define ARGENTRY(mod, sym, n) if (strcasecmp(module, mod) == 0 && strcmp(symbol, sym) == 0) return n;

    // kernel32
    if (strcasecmp(module, "kernel32.dll") == 0 || strcasecmp(module, "kernel32") == 0) {
        if (strcmp(symbol, "GetModuleHandleA") == 0) return 1;
        if (strcmp(symbol, "GetModuleHandleW") == 0) return 1;
        if (strcmp(symbol, "GetProcAddress") == 0) return 2;
        if (strcmp(symbol, "LoadLibraryA") == 0) return 1;
        if (strcmp(symbol, "LoadLibraryW") == 0) return 1;
        if (strcmp(symbol, "LoadLibraryExA") == 0) return 3;
        if (strcmp(symbol, "LoadLibraryExW") == 0) return 3;
        if (strcmp(symbol, "FreeLibrary") == 0) return 1;
        if (strcmp(symbol, "CreateFileA") == 0) return 7;
        if (strcmp(symbol, "CreateFileW") == 0) return 7;
        if (strcmp(symbol, "ReadFile") == 0) return 5;
        if (strcmp(symbol, "WriteFile") == 0) return 5;
        if (strcmp(symbol, "CloseHandle") == 0) return 1;
        if (strcmp(symbol, "GetFileSize") == 0) return 2;
        if (strcmp(symbol, "GetFileSizeEx") == 0) return 2;
        if (strcmp(symbol, "SetFilePointer") == 0) return 4;
        if (strcmp(symbol, "SetFilePointerEx") == 0) return 4;
        if (strcmp(symbol, "VirtualAlloc") == 0) return 4;
        if (strcmp(symbol, "VirtualFree") == 0) return 3;
        if (strcmp(symbol, "VirtualProtect") == 0) return 4;
        if (strcmp(symbol, "HeapAlloc") == 0) return 3;
        if (strcmp(symbol, "HeapFree") == 0) return 3;
        if (strcmp(symbol, "HeapCreate") == 0) return 3;
        if (strcmp(symbol, "GetLastError") == 0) return 0;
        if (strcmp(symbol, "SetLastError") == 0) return 1;
        if (strcmp(symbol, "GetCommandLineA") == 0) return 0;
        if (strcmp(symbol, "GetCommandLineW") == 0) return 0;
        if (strcmp(symbol, "ExitProcess") == 0) return 1;
        if (strcmp(symbol, "GetCurrentProcess") == 0) return 0;
        if (strcmp(symbol, "GetCurrentThread") == 0) return 0;
        if (strcmp(symbol, "GetCurrentThreadId") == 0) return 0;
        if (strcmp(symbol, "Sleep") == 0) return 1;
        if (strcmp(symbol, "GetTickCount") == 0) return 0;
        if (strcmp(symbol, "QueryPerformanceCounter") == 0) return 1;
        if (strcmp(symbol, "QueryPerformanceFrequency") == 0) return 1;
        if (strcmp(symbol, "InitializeCriticalSection") == 0) return 1;
        if (strcmp(symbol, "EnterCriticalSection") == 0) return 1;
        if (strcmp(symbol, "LeaveCriticalSection") == 0) return 1;
        if (strcmp(symbol, "DeleteCriticalSection") == 0) return 1;
        if (strcmp(symbol, "CreateEventA") == 0) return 4;
        if (strcmp(symbol, "CreateEventW") == 0) return 4;
        if (strcmp(symbol, "SetEvent") == 0) return 1;
        if (strcmp(symbol, "WaitForSingleObject") == 0) return 2;
        if (strcmp(symbol, "WaitForMultipleObjects") == 0) return 4;
        if (strcmp(symbol, "CreateThread") == 0) return 6;
        if (strcmp(symbol, "GetModuleFileNameA") == 0) return 3;
        if (strcmp(symbol, "GetModuleFileNameW") == 0) return 3;
        if (strcmp(symbol, "GetEnvironmentVariableA") == 0) return 3;
        if (strcmp(symbol, "GetEnvironmentVariableW") == 0) return 3;
        if (strcmp(symbol, "SetEnvironmentVariableA") == 0) return 2;
        if (strcmp(symbol, "MultiByteToWideChar") == 0) return 6;
        if (strcmp(symbol, "WideCharToMultiByte") == 0) return 8;
        if (strcmp(symbol, "GetStdHandle") == 0) return 1;
        if (strcmp(symbol, "WriteConsoleA") == 0) return 5;
        if (strcmp(symbol, "WriteConsoleW") == 0) return 5;
        if (strcmp(symbol, "GetConsoleMode") == 0) return 2;
        if (strcmp(symbol, "SetConsoleMode") == 0) return 2;
        if (strcmp(symbol, "GetSystemInfo") == 0) return 1;
        if (strcmp(symbol, "GetVersionExA") == 0) return 1;
        if (strcmp(symbol, "GetVersionExW") == 0) return 1;
        if (strcmp(symbol, "IsDebuggerPresent") == 0) return 0;
        if (strcmp(symbol, "OutputDebugStringA") == 0) return 1;
        if (strcmp(symbol, "OutputDebugStringW") == 0) return 1;
        if (strcmp(symbol, "RaiseException") == 0) return 4;
        if (strcmp(symbol, "UnhandledExceptionFilter") == 0) return 1;
        if (strcmp(symbol, "SetUnhandledExceptionFilter") == 0) return 1;
        if (strcmp(symbol, "TerminateProcess") == 0) return 2;
        if (strcmp(symbol, "GetProcessHeap") == 0) return 0;
        if (strcmp(symbol, "RtlMoveMemory") == 0) return 3;
        if (strcmp(symbol, "RtlZeroMemory") == 0) return 2;
        if (strcmp(symbol, "RtlFillMemory") == 0) return 3;
    }

    // user32
    if (strcasecmp(module, "user32.dll") == 0 || strcasecmp(module, "user32") == 0) {
        if (strcmp(symbol, "MessageBoxA") == 0) return 4;
        if (strcmp(symbol, "MessageBoxW") == 0) return 4;
        if (strcmp(symbol, "RegisterClassA") == 0) return 1;
        if (strcmp(symbol, "RegisterClassW") == 0) return 1;
        if (strcmp(symbol, "RegisterClassExA") == 0) return 1;
        if (strcmp(symbol, "RegisterClassExW") == 0) return 1;
        if (strcmp(symbol, "CreateWindowExA") == 0) return 12;
        if (strcmp(symbol, "CreateWindowExW") == 0) return 12;
        if (strcmp(symbol, "DestroyWindow") == 0) return 1;
        if (strcmp(symbol, "ShowWindow") == 0) return 2;
        if (strcmp(symbol, "UpdateWindow") == 0) return 1;
        if (strcmp(symbol, "GetMessageA") == 0) return 4;
        if (strcmp(symbol, "GetMessageW") == 0) return 4;
        if (strcmp(symbol, "PeekMessageA") == 0) return 5;
        if (strcmp(symbol, "PeekMessageW") == 0) return 5;
        if (strcmp(symbol, "TranslateMessage") == 0) return 1;
        if (strcmp(symbol, "DispatchMessageA") == 0) return 1;
        if (strcmp(symbol, "DispatchMessageW") == 0) return 1;
        if (strcmp(symbol, "DefWindowProcA") == 0) return 4;
        if (strcmp(symbol, "DefWindowProcW") == 0) return 4;
        if (strcmp(symbol, "PostQuitMessage") == 0) return 1;
        if (strcmp(symbol, "LoadIconA") == 0) return 2;
        if (strcmp(symbol, "LoadIconW") == 0) return 2;
        if (strcmp(symbol, "LoadCursorA") == 0) return 2;
        if (strcmp(symbol, "LoadCursorW") == 0) return 2;
        if (strcmp(symbol, "GetDC") == 0) return 1;
        if (strcmp(symbol, "ReleaseDC") == 0) return 2;
        if (strcmp(symbol, "BeginPaint") == 0) return 2;
        if (strcmp(symbol, "EndPaint") == 0) return 2;
        if (strcmp(symbol, "GetClientRect") == 0) return 2;
        if (strcmp(symbol, "GetWindowRect") == 0) return 2;
        if (strcmp(symbol, "SetWindowTextA") == 0) return 2;
        if (strcmp(symbol, "SetWindowTextW") == 0) return 2;
        if (strcmp(symbol, "GetWindowTextA") == 0) return 3;
        if (strcmp(symbol, "GetWindowTextW") == 0) return 3;
        if (strcmp(symbol, "InvalidateRect") == 0) return 3;
        if (strcmp(symbol, "GetSystemMetrics") == 0) return 1;
    }

    // gdi32
    if (strcasecmp(module, "gdi32.dll") == 0 || strcasecmp(module, "gdi32") == 0) {
        if (strcmp(symbol, "CreateCompatibleDC") == 0) return 1;
        if (strcmp(symbol, "DeleteDC") == 0) return 1;
        if (strcmp(symbol, "SelectObject") == 0) return 2;
        if (strcmp(symbol, "DeleteObject") == 0) return 1;
        if (strcmp(symbol, "CreateSolidBrush") == 0) return 1;
        if (strcmp(symbol, "CreatePen") == 0) return 3;
        if (strcmp(symbol, "TextOutA") == 0) return 5;
        if (strcmp(symbol, "TextOutW") == 0) return 5;
        if (strcmp(symbol, "BitBlt") == 0) return 9;
        if (strcmp(symbol, "StretchBlt") == 0) return 11;
        if (strcmp(symbol, "GetStockObject") == 0) return 1;
        if (strcmp(symbol, "SetBkMode") == 0) return 2;
        if (strcmp(symbol, "GetBkMode") == 0) return 1;
        if (strcmp(symbol, "SetTextColor") == 0) return 2;
        if (strcmp(symbol, "SetBkColor") == 0) return 2;
        if (strcmp(symbol, "SetTextAlign") == 0) return 2;
        if (strcmp(symbol, "GetTextAlign") == 0) return 1;
        if (strcmp(symbol, "GetCurrentObject") == 0) return 2;
        if (strcmp(symbol, "GetTextExtentPointA") == 0) return 4;
        if (strcmp(symbol, "GetTextExtentPointW") == 0) return 4;
        if (strcmp(symbol, "GetTextExtentExPointA") == 0) return 7;
        if (strcmp(symbol, "GetTextExtentExPointW") == 0) return 7;
        if (strcmp(symbol, "GetCharWidth32A") == 0) return 4;
        if (strcmp(symbol, "GetCharWidth32W") == 0) return 4;
        if (strcmp(symbol, "GetCharWidthA") == 0) return 4;
        if (strcmp(symbol, "GetCharWidthW") == 0) return 4;
        if (strcmp(symbol, "GetCharABCWidthsFloatA") == 0) return 4;
        if (strcmp(symbol, "GetCharABCWidthsFloatW") == 0) return 4;
        if (strcmp(symbol, "GetCharacterPlacementW") == 0) return 5;
        if (strcmp(symbol, "TranslateCharsetInfo") == 0) return 3;
        if (strcmp(symbol, "IntersectClipRect") == 0) return 5;
        if (strcmp(symbol, "ExcludeClipRect") == 0) return 5;
        if (strcmp(symbol, "UnrealizeObject") == 0) return 1;
        if (strcmp(symbol, "UpdateColors") == 0) return 1;
        if (strcmp(symbol, "ExtTextOutA") == 0) return 8;
        if (strcmp(symbol, "GetOutlineTextMetricsA") == 0) return 3;
        if (strcmp(symbol, "SetDCBrushColor") == 0) return 2;
        if (strcmp(symbol, "SetDCPenColor") == 0) return 2;
    }

    // advapi32
    if (strcasecmp(module, "advapi32.dll") == 0 || strcasecmp(module, "advapi32") == 0) {
        if (strcmp(symbol, "RegOpenKeyExA") == 0) return 5;
        if (strcmp(symbol, "RegOpenKeyExW") == 0) return 5;
        if (strcmp(symbol, "RegQueryValueExA") == 0) return 6;
        if (strcmp(symbol, "RegQueryValueExW") == 0) return 6;
        if (strcmp(symbol, "RegCloseKey") == 0) return 1;
        if (strcmp(symbol, "RegCreateKeyExA") == 0) return 9;
        if (strcmp(symbol, "RegCreateKeyExW") == 0) return 9;
    }

    // ole32
    if (strcasecmp(module, "ole32.dll") == 0 || strcasecmp(module, "ole32") == 0) {
        if (strcmp(symbol, "CoInitialize") == 0) return 1;
        if (strcmp(symbol, "CoInitializeEx") == 0) return 2;
        if (strcmp(symbol, "CoUninitialize") == 0) return 0;
        if (strcmp(symbol, "CoCreateInstance") == 0) return 5;
    }

    // winmm
    if (strcasecmp(module, "winmm.dll") == 0 || strcasecmp(module, "winmm") == 0) {
        if (strcmp(symbol, "timeBeginPeriod") == 0) return 1;
        if (strcmp(symbol, "timeEndPeriod") == 0) return 1;
        if (strcmp(symbol, "timeGetTime") == 0) return 0;
    }

    // d3d9
    if (strcasecmp(module, "d3d9.dll") == 0 || strcasecmp(module, "d3d9") == 0) {
        if (strcmp(symbol, "Direct3DCreate9") == 0) return 1;
    }

    // Oodle (oo2core_{1..9}_win32/win64.dll)
    if (strncasecmp(module, "oo2core", 7) == 0) {
        if (strcmp(symbol, "OodleLZ_Decompress") == 0) return 14;
        if (strcmp(symbol, "Oodle_GetErrorHandler") == 0) return 0;
        if (strcmp(symbol, "Oodle_SetErrorHandler") == 0) return 1;
    }

    // Registry apiset aliases (registered from bridge_reg.cpp k_advapi32_fns)
    if (strncasecmp(module, "api-ms-win-core-registry-", 25) == 0) {
        return lookup_arg_count("advapi32", symbol);
    }

    // Default: unknown, return -1 to indicate "use default"
    return -1;
    #undef ARGENTRY
}

// Conformance API: the argument count this bridge will pop for module!symbol,
// or -1 when no ABI entry exists. -1 is not harmless - the trap dispatcher then
// falls back to a 4-argument guess, which is exactly how a 1-arg stdcall once
// ate OpenTTD's return address and jumped to 0x0000B5E3. The conformance suite
// requires every registered BUILTIN symbol to answer without guessing.
int trt_abi_arg_count(const char* module, const char* symbol) {
    if (!module || !symbol) return -1;
    int n = lookup_arg_count(module, symbol);
    if (n >= 0) return n;
    // The table stores module names without the extension; retry stripped.
    char bare[96];
    size_t len = strcspn(module, ".");
    if (len == 0 || len >= sizeof(bare)) return -1;
    memcpy(bare, module, len);
    bare[len] = 0;
    if (strcmp(bare, module) == 0) return -1;
    return lookup_arg_count(bare, symbol);
}

// ---- Calling-convention helpers ----
static int ieq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}
static int mod_eq(const char* a, const char* b) {
    size_t la = strcspn(a, "."), lb = strcspn(b, ".");
    return la == lb && ieq_n(a, b, la);
}
static int sym_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

// __cdecl / variadic exports: the CALLER cleans the stack, so the trap
// dispatcher pops only the return address.
static int lookup_cdecl(const char* module, const char* symbol) {
    if (mod_eq(module, "msvcrt") && (sym_eq(symbol, "_telli64") ||
        sym_eq(symbol, "_ultoa") || sym_eq(symbol, "strspn"))) return 1;
    static const char* k_cdecl[] = {
        "printf", "fprintf", "sprintf", "snprintf", "_snprintf", "vsprintf",
        "vprintf", "scanf", "fscanf", "sscanf", "malloc", "calloc", "realloc",
        "free", "wsprintfA", "wsprintfW", "wvsprintfA", "wvsprintfW", nullptr};
    for (int i = 0; k_cdecl[i]; i++)
        if (sym_eq(symbol, k_cdecl[i])) return 1;
    return 0;
}

// Which arguments are guest pointers (bit i = arg i). Known APIs get an exact
// mask; unknown ones fall back to a conservative heuristic.
static uint32_t lookup_ptr_mask(x86_context* ctx, const char* module,
                                const char* symbol, const uint32_t* args,
                                int nargs) {
    #define P(m) return (m)
    if (mod_eq(module, "kernel32")) {
        if (sym_eq(symbol, "CreateFileA") || sym_eq(symbol, "CreateFileW")) P(0x9);
        if (sym_eq(symbol, "ReadFile") || sym_eq(symbol, "WriteFile")) P(0x1A);
        if (sym_eq(symbol, "GetModuleHandleA") || sym_eq(symbol, "GetModuleHandleW")) P(0x1);
        if (sym_eq(symbol, "LoadLibraryA") || sym_eq(symbol, "LoadLibraryW")) P(0x1);
        if (sym_eq(symbol, "LoadLibraryExA") || sym_eq(symbol, "LoadLibraryExW")) P(0x1);
        if (sym_eq(symbol, "GetModuleFileNameA") || sym_eq(symbol, "GetModuleFileNameW")) P(0x2);
        if (sym_eq(symbol, "GetEnvironmentVariableA") || sym_eq(symbol, "GetEnvironmentVariableW")) P(0x3);
        if (sym_eq(symbol, "SetEnvironmentVariableA") || sym_eq(symbol, "SetEnvironmentVariableW")) P(0x3);
        if (sym_eq(symbol, "MultiByteToWideChar") || sym_eq(symbol, "WideCharToMultiByte")) P(0x14);
        if (sym_eq(symbol, "OutputDebugStringA") || sym_eq(symbol, "OutputDebugStringW")) P(0x1);
        if (sym_eq(symbol, "GetSystemInfo") || sym_eq(symbol, "GetNativeSystemInfo") || sym_eq(symbol, "GetVersionExA") ||
            sym_eq(symbol, "GetVersionExW")) P(0x1);
        if (sym_eq(symbol, "RtlMoveMemory")) P(0x3);
        // Symbols the CRT resolves at runtime (api-set -> kernel32) take guest
        // pointers too; without these the bridge would see a raw guest VA.
        if (sym_eq(symbol, "InitializeCriticalSection") ||
            sym_eq(symbol, "InitializeCriticalSectionEx") ||
            sym_eq(symbol, "InitializeSListHead") ||
            sym_eq(symbol, "RtlCaptureContext") ||
            sym_eq(symbol, "SetThreadStackGuarantee") ||
            sym_eq(symbol, "EncodePointer") || sym_eq(symbol, "DecodePointer")) P(0x1);
        if (sym_eq(symbol, "GetThreadTimes") || sym_eq(symbol, "GetProcessTimes")) P(0x1C);
        if (sym_eq(symbol, "QueryThreadCycleTime")) P(0x2);
        if (sym_eq(symbol, "GetProcessAffinityMask")) P(0x6);
        if (sym_eq(symbol, "GetFileInformationByHandleEx")) P(0x8);
        if (sym_eq(symbol, "RtlZeroMemory")) P(0x1);
        if (sym_eq(symbol, "RtlFillMemory")) P(0x1);
        if (sym_eq(symbol, "lstrlenA") || sym_eq(symbol, "lstrlenW")) P(0x1);
        if (sym_eq(symbol, "CreateEventA") || sym_eq(symbol, "CreateEventW")) P(0x4);
        if (sym_eq(symbol, "WaitForMultipleObjects")) P(0x1);
    }
    if (mod_eq(module, "user32")) {
        if (sym_eq(symbol, "MessageBoxA") || sym_eq(symbol, "MessageBoxW")) P(0x6);
        if (sym_eq(symbol, "RegisterClassA") || sym_eq(symbol, "RegisterClassW") ||
            sym_eq(symbol, "RegisterClassExA") || sym_eq(symbol, "RegisterClassExW")) P(0x1);
        if (sym_eq(symbol, "GetMessageA") || sym_eq(symbol, "GetMessageW") ||
            sym_eq(symbol, "PeekMessageA") || sym_eq(symbol, "PeekMessageW")) P(0x2);
        if (sym_eq(symbol, "BeginPaint") || sym_eq(symbol, "EndPaint") ||
            sym_eq(symbol, "GetClientRect") || sym_eq(symbol, "GetWindowRect")) P(0x2);
        if (sym_eq(symbol, "SetWindowTextA") || sym_eq(symbol, "SetWindowTextW") ||
            sym_eq(symbol, "GetWindowTextA") || sym_eq(symbol, "GetWindowTextW")) P(0x2);
        if (sym_eq(symbol, "LoadIconA") || sym_eq(symbol, "LoadIconW") ||
            sym_eq(symbol, "LoadCursorA") || sym_eq(symbol, "LoadCursorW")) P(0x2);
        if (sym_eq(symbol, "DispatchMessageA") || sym_eq(symbol, "DispatchMessageW")) P(0x1);
        if (sym_eq(symbol, "InvalidateRect")) P(0x2);
        if (sym_eq(symbol, "CreateWindowExA") || sym_eq(symbol, "CreateWindowExW")) P(0x7006);
    }
    if (mod_eq(module, "gdi32")) {
        if (sym_eq(symbol, "TextOutA") || sym_eq(symbol, "TextOutW")) P(0x8);
        if (sym_eq(symbol, "SelectObject")) P(0x0);
    }
    if (mod_eq(module, "advapi32")) {
        if (sym_eq(symbol, "RegOpenKeyExA") || sym_eq(symbol, "RegOpenKeyExW")) P(0x2);
        if (sym_eq(symbol, "RegQueryValueExA") || sym_eq(symbol, "RegQueryValueExW")) P(0x32);
        if (sym_eq(symbol, "RegSetValueExA") || sym_eq(symbol, "RegSetValueExW")) P(0x12);
        if (sym_eq(symbol, "RegCreateKeyExA") || sym_eq(symbol, "RegCreateKeyExW")) P(0x2);
    }
    if (mod_eq(module, "msvcrt") || mod_eq(module, "ucrtbase")) {
        if (sym_eq(symbol, "printf") || sym_eq(symbol, "sprintf") ||
            sym_eq(symbol, "wsprintf")) P(0x1);
        if (sym_eq(symbol, "strlen") || sym_eq(symbol, "strcmp") ||
            sym_eq(symbol, "strcpy") || sym_eq(symbol, "memcpy") ||
            sym_eq(symbol, "memset")) P(0x3);
    }
    if (mod_eq(module, "msvcrt")) {
        if (sym_eq(symbol, "_telli64")) P(0x0);
        if (sym_eq(symbol, "_ultoa")) P(0x2);
        if (sym_eq(symbol, "strspn")) P(0x3);
    }
    if (mod_eq(module, "winmm") || mod_eq(module, "winmmbase")) {
        if (sym_eq(symbol, "midiOutGetErrorTextA") ||
            sym_eq(symbol, "midiOutGetErrorTextW")) P(0x2);
    }
    #undef P
    // Heuristic: a value in user space that lands on a committed page is
    // treated as a data pointer. Small integers and sizes stay untouched.
    uint32_t m = 0;
    if (!ctx->mem.base) return 0;
    for (int i = 0; i < nargs; i++) {
        uint32_t v = args[i];
        if (v >= 0x10000u && v < 0x80000000u &&
            x86_page_mapped(ctx, v >> X86_PAGE_SHIFT)) {
            m |= (1u << i);
        }
    }
    return m;
}

// ---- 32-bit runtime loading: LoadLibrary / GetProcAddress / GetModuleHandle --
// A 32-bit Windows guest resolves most of its imports at RUNTIME:
// LoadLibraryExW("api-ms-win-core-synch-l1-2-0") then GetProcAddress. Both
// halves must answer in GUEST terms, or the CRT's _initterm loop ends up
// calling truncated host pointers (that was the OpenTTD __fastfail):
//   * the module handle is a small integer the guest can store and pass back
//     (a host module pointer does not survive 32 bits),
//   * the returned address is a guest VA. For a 64-bit bridge function that is
//     a TRAP (0xF0000000 + 4*N), which x86_trap_dispatch turns back into a
//     bridge call using the guest's own stacked arguments. Guest code inside an
//     already-mapped PE32 image keeps its real VA -- pe_loader binds those.
extern "C" uint32_t trt_pe32_bind_dynamic(const char* module, const char* symbol,
                                          uint32_t ordinal, void* host_fn);

#define X86_MOD_HANDLE_BASE 0x00010001u
#define X86_MOD_MAX 256
static struct x86_mod_entry { uint32_t h; char name[80]; } g_x86_mods[X86_MOD_MAX];
static int g_x86_mod_count = 0;

static const char* x86_mod_name(uint32_t h) {
    for (int i = 0; i < g_x86_mod_count; i++)
        if (g_x86_mods[i].h == h) return g_x86_mods[i].name;
    return nullptr;
}

// Find (or mint) the 32-bit handle for a module name. Handles are dense small
// integers so they round-trip through a 32-bit register untouched.
static uint32_t x86_mod_handle(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_x86_mod_count; i++)
        if (strcasecmp(g_x86_mods[i].name, name) == 0) return g_x86_mods[i].h;
    if (g_x86_mod_count >= X86_MOD_MAX) return 0;
    x86_mod_entry& e = g_x86_mods[g_x86_mod_count];
    e.h = X86_MOD_HANDLE_BASE + (uint32_t)g_x86_mod_count;
    snprintf(e.name, sizeof(e.name), "%s", name);
    g_x86_mod_count++;
    return e.h;
}

static bool x86_read_cstr(x86_context* ctx, uint32_t va, char* out, size_t cap) {
    if (!ctx->mem.base || !va || cap == 0) return false;
    for (size_t i = 0; i + 1 < cap; i++) {
        const char c = (char)x86_mem_read8(ctx, va + (uint32_t)i);
        out[i] = c;
        if (!c) return true;
    }
    out[cap - 1] = 0;
    return true;
}

static bool x86_read_wstr(x86_context* ctx, uint32_t va, char* out, size_t cap) {
    if (!ctx->mem.base || !va || cap == 0) return false;
    for (size_t i = 0; i + 1 < cap; i++) {
        const uint16_t c = x86_mem_read16(ctx, va + (uint32_t)(i * 2));
        out[i] = (c < 0x80) ? (char)c : '?';
        if (!c) return true;
    }
    out[cap - 1] = 0;
    return true;
}

// Resolve a symbol for a 32-bit guest. api-set / ext-ms forwarder names carry
// no implementations of their own; the real entry point lives in
// kernelbase/kernel32, which is where the bridges are registered.
// `resolved_module` receives the module that actually supplied the function.
// That matters beyond diagnostics: the trap is registered under the resolved
// name, so the stdcall arg-count database (keyed on kernel32/user32/...) finds
// the symbol. Registered under an api-set name it would be a NAMED GAP and the
// 4-arg default would shift the guest stack.
static void* x86_resolve_dynamic(const char* module, const char* symbol,
                                 uint32_t ordinal, const char** resolved_module) {
    // An api-set contract ("api-ms-win-core-synch-l1-2-0") is a forwarding
    // name. Resolve through its real target AND bind the trap under that
    // target name, so the arg-count database finds it.
    if (const char* target = trt_apiset_target(module)) module = target;
    if (resolved_module) *resolved_module = module;
    void* fn = trt_resolve_export(module, symbol, (unsigned short)ordinal);
    if (fn) return fn;
    if (strncasecmp(module, "api-ms-win", 10) == 0 ||
        strncasecmp(module, "ext-ms-win", 10) == 0) {
        static const char* kForwards[] = {"kernel32", "kernelbase", "ntdll", nullptr};
        for (int i = 0; kForwards[i]; i++) {
            fn = trt_resolve_export(kForwards[i], symbol, (unsigned short)ordinal);
            if (fn) {
                if (resolved_module) *resolved_module = kForwards[i];
                return fn;
            }
        }
    }
    return nullptr;
}

static int x86_pe32_runtime(x86_context* ctx, const char* module,
                            const char* symbol, const uint32_t* args,
                            uint32_t* out) {
    if (!ctx->mem.base) return 0;  // 64-bit guest: the classic bridges apply

    if (mod_eq(module, "kernel32") && sym_eq(symbol, "ExitProcess")) {
        // An emulated guest can exit without ending the hosting UWP app.
        ctx->running = 0;
        if (args[0]) ctx->execution = X86_EXEC_GUEST_ERROR;
        *out = args[0];
        return 1;
    }
    if (sym_eq(symbol, "GetCommandLineA")) {
        if (!ctx->command_line_a) {
            const char* text = trt_GetCommandLineA();
            const uint32_t size = static_cast<uint32_t>(strlen(text) + 1);
            ctx->command_line_a = x86_guest_alloc(ctx, size, 0);
            if (ctx->command_line_a)
                memcpy(ctx->mem.base + ctx->command_line_a, text, size);
        }
        *out = ctx->command_line_a;
        return 1;
    }
    if (sym_eq(symbol, "GetCommandLineW")) {
        if (!ctx->command_line_w) {
            const u16* text = trt_GetCommandLineW();
            uint32_t length = 0;
            while (text[length]) ++length;
            const uint32_t size = (length + 1) * sizeof(u16);
            ctx->command_line_w = x86_guest_alloc(ctx, size, 0);
            if (ctx->command_line_w)
                memcpy(ctx->mem.base + ctx->command_line_w, text, size);
        }
        *out = ctx->command_line_w;
        return 1;
    }

    // ---- module handles ----
    if (sym_eq(symbol, "GetModuleHandleA") || sym_eq(symbol, "GetModuleHandleW") ||
        sym_eq(symbol, "GetModuleHandleExA") || sym_eq(symbol, "GetModuleHandleExW")) {
        // GetModuleHandleExA(flags, name, out) reports through a pointer and
        // returns a BOOL; the 1-arg form returns the handle.
        char name[128] = {0};
        uint32_t h = 0;
        const int is_ex = symbol[16] != 0;
        if (!is_ex) {
            if (!args[0]) {
                h = ctx->image_va;
            } else if (x86_read_cstr(ctx, args[0], name, sizeof(name))) {
                h = x86_mod_handle(name);
            }
            *out = h;
            return 1;
        }
        if (sym_eq(symbol, "GetModuleHandleExA")) {
            if (args[1]) x86_read_cstr(ctx, args[1], name, sizeof(name));
        } else {
            if (args[1]) x86_read_wstr(ctx, args[1], name, sizeof(name));
        }
        h = name[0] ? x86_mod_handle(name) : ctx->image_va;
        if (args[2]) x86_mem_write32(ctx, args[2], h);
        *out = h ? 1 : 0;
        return 1;
    }
    if (sym_eq(symbol, "LoadLibraryA") || sym_eq(symbol, "LoadLibraryW") ||
        sym_eq(symbol, "LoadLibraryExA") || sym_eq(symbol, "LoadLibraryExW")) {
        char name[128] = {0};
        const bool wide = symbol[strlen(symbol) - 1] == 'W';
        if (wide) x86_read_wstr(ctx, args[0], name, sizeof(name));
        else x86_read_cstr(ctx, args[0], name, sizeof(name));
        const uint32_t h = x86_mod_handle(name);
        char line[256];
        snprintf(line, sizeof(line),
                 "x86: LoadLibrary(\"%s\") -> handle 0x%08X (32-bit dynamic "
                 "symbol resolution; a real PE32 DLL image is not mapped yet)",
                 name[0] ? name : "?", h);
        if (ctx->log) ctx->log(line);
        *out = h;
        return 1;
    }
    if (sym_eq(symbol, "FreeLibrary") || sym_eq(symbol, "FreeLibraryAndExitThread")) {
        *out = 1;
        return 1;
    }
    if (sym_eq(symbol, "GetProcAddress")) {
        const uint32_t hmod = args[0];
        char sym[128] = {0};
        uint32_t ordinal = 0;
        bool by_ordinal = false;
        // A name pointer below 64 KB is really an ordinal (the standard
        // MAKEINTRESOURCE idiom).
        if (args[1] < 0x10000u) {
            by_ordinal = true;
            ordinal = args[1] & 0xFFFFu;
        } else {
            x86_read_cstr(ctx, args[1], sym, sizeof(sym));
        }
        // Guest DLL (PE32 or PE64) loaded by the loader: resolve its export
        // table directly and hand back the export's GUEST VA (emulated code
        // calling emulated code). This bypasses the trap mechanism, which is
        // only for host (64-bit) bridges.
        trt_image* gimg = trt_images_by_handle((void*)(uintptr_t)hmod);
        if (gimg && gimg->is_pe32) {
            void* fn = trt_pe_find_export(trt_runtime_backend(), nullptr, gimg,
                                          by_ordinal ? nullptr : sym,
                                          by_ordinal ? (u16)ordinal : 0);
            // trt_pe_find_export returns a host pointer (base+func_rva); convert
            // to guest VA.
            if (fn) {
                const u32 guest_va = gimg->guest_base +
                    (u32)((uintptr_t)fn - (uintptr_t)gimg->base);
                char line[288];
                snprintf(line, sizeof(line),
                         "x86: GetProcAddress(guest %s!%s) -> guest VA 0x%08X",
                         by_ordinal ? "#ordinal" : sym, by_ordinal ? "#ordinal" : sym,
                         guest_va);
                if (ctx->log) ctx->log(line);
                *out = guest_va;
                return 1;
            }
        }

        const char* modname = (hmod == 0 || hmod == ctx->image_va) ? "" : x86_mod_name(hmod);
        char line[288];
        if (!modname || !modname[0]) {
            snprintf(line, sizeof(line),
                     "x86: GetProcAddress(handle 0x%08X, \"%s\") -> 0 (handle is "
                     "not a known 32-bit module)",
                     hmod, by_ordinal ? "#ordinal" : sym);
            if (ctx->log) ctx->log(line);
            *out = 0;
            return 1;
        }
        const char* resolved_mod = modname;
        void* fn = x86_resolve_dynamic(modname, by_ordinal ? nullptr : sym,
                                       ordinal, &resolved_mod);
        if (!fn) {
            snprintf(line, sizeof(line),
                     "x86: GetProcAddress(%s!%s) -> 0 (NAMED GAP: no bridge for "
                     "this symbol)",
                     modname, by_ordinal ? "#ordinal" : sym);
            if (ctx->log) ctx->log(line);
            *out = 0;
            return 1;
        }
        const uint32_t trap =
            trt_pe32_bind_dynamic(resolved_mod, by_ordinal ? nullptr : sym, ordinal, fn);
        if (!trap) {
            snprintf(line, sizeof(line),
                     "x86: GetProcAddress(%s!%s) -> 0 (trap table full)",
                     modname, by_ordinal ? "#ordinal" : sym);
            if (ctx->log) ctx->log(line);
            *out = 0;
            return 1;
        }
        snprintf(line, sizeof(line),
                 "x86: GetProcAddress(%s!%s) -> trap 0x%08X (bound as %s!%s, "
                 "32-bit direct call to a 64-bit bridge)",
                 modname, by_ordinal ? "#ordinal" : sym, trap, resolved_mod,
                 by_ordinal ? "#ordinal" : sym);
        if (ctx->log) ctx->log(line);
        *out = trap;
        return 1;
    }
    if (sym_eq(symbol, "GetModuleFileNameA") || sym_eq(symbol, "GetModuleFileNameW")) {
        const uint32_t hmod = args[0];
        const char* nm = hmod ? x86_mod_name(hmod) : nullptr;
        char text[128];
        snprintf(text, sizeof(text), "%s", nm && nm[0] ? nm : "program.exe");
        const uint32_t cap = args[2];
        const bool wide = symbol[strlen(symbol) - 1] == 'W';
        uint32_t n = 0;
        for (; text[n] && n + 1 < cap; n++) {
            if (wide) x86_mem_write16(ctx, args[1] + n * 2, (uint16_t)(uint8_t)text[n]);
            else x86_mem_write8(ctx, args[1] + n, (uint8_t)text[n]);
        }
        if (wide) x86_mem_write16(ctx, args[1] + n * 2, 0);
        else x86_mem_write8(ctx, args[1] + n, 0);
        *out = n;
        return 1;
    }

    // ---- allocators: everything the guest keeps must live in the arena ----
    if (sym_eq(symbol, "GetProcessHeap")) { *out = 1; return 1; }
    if (sym_eq(symbol, "HeapCreate") || sym_eq(symbol, "HeapDestroy")) { *out = 1; return 1; }
    if (sym_eq(symbol, "HeapAlloc")) {
        *out = x86_guest_alloc(ctx, args[2], (args[1] & 0x8u) != 0 /*HEAP_ZERO_MEMORY*/);
        return 1;
    }
    if (sym_eq(symbol, "HeapReAlloc")) {
        *out = x86_guest_realloc(ctx, args[2], args[3], (args[1] & 0x8u) != 0);
        return 1;
    }
    if (sym_eq(symbol, "HeapFree")) { *out = (uint32_t)x86_guest_free(ctx, args[2]); return 1; }
    if (sym_eq(symbol, "HeapSize")) { *out = x86_guest_blocksize(ctx, args[2]); return 1; }
    if (sym_eq(symbol, "LocalAlloc") || sym_eq(symbol, "GlobalAlloc")) {
        *out = x86_guest_alloc(ctx, args[1], (args[0] & 0x40u) != 0 /*LMEM_ZEROINIT*/);
        return 1;
    }
    if (sym_eq(symbol, "LocalFree") || sym_eq(symbol, "GlobalFree")) {
        x86_guest_free(ctx, args[0]);
        *out = 0;
        return 1;
    }
    if (sym_eq(symbol, "VirtualAlloc")) {
        const uint32_t addr = args[0];
        const uint32_t size = args[1];
        if (!size) { *out = 0; return 1; }
        if (addr) {
            // Honour a caller-chosen address INSIDE the reservation; anything
            // outside it cannot be handed back to a 32-bit guest safely.
            if (addr >= 0x10000u && addr + size > addr &&
                x86_mem_commit(ctx, addr, size) == 0) {
                *out = addr;
            } else {
                char line[160];
                snprintf(line, sizeof(line),
                         "x86: VirtualAlloc(0x%08X, %u) refused (outside the "
                         "guest arena)", addr, size);
                if (ctx->log) ctx->log(line);
                *out = 0;
            }
            return 1;
        }
        *out = x86_guest_alloc(ctx, size, 1);
        return 1;
    }
    if (sym_eq(symbol, "VirtualFree")) {
        if (args[2] & 0x8000u /*MEM_RELEASE*/) x86_guest_free(ctx, args[0]);
        *out = 1;
        return 1;
    }
    if (sym_eq(symbol, "VirtualQuery")) {
        // 32-bit MEMORY_BASIC_INFORMATION is 28 bytes.
        const uint32_t addr = args[0];
        const uint32_t mbi = args[1];
        const uint32_t len = args[2];
        if (!mbi || len < 28u) { *out = 0; return 1; }
        // Zero the struct first.
        for (int i = 0; i < 28; i++) x86_mem_write8(ctx, mbi + i, 0);
        const uint32_t page = addr & ~0xFFFu;
        // Report the page as committed (MEM_COMMIT) with PAGE_READWRITE.
        x86_mem_write32(ctx, mbi + 0, page);     // BaseAddress
        x86_mem_write32(ctx, mbi + 4, page);     // AllocationBase
        x86_mem_write32(ctx, mbi + 8, 0x04u);    // AllocationProtect (PAGE_READWRITE)
        x86_mem_write32(ctx, mbi + 12, 0x1000u); // RegionSize
        x86_mem_write32(ctx, mbi + 16, 0x1000u); // State (MEM_COMMIT)
        x86_mem_write32(ctx, mbi + 20, 0x04u);   // Protect
        x86_mem_write32(ctx, mbi + 24, 0x20000u);// Type (MEM_PRIVATE)
        *out = 28u;
        return 1;
    }
    if (sym_eq(symbol, "VirtualProtect")) {
        const uint32_t addr = args[0];
        const uint32_t size = args[1];
        // Honour the request inside the reservation; report success.
        if (size && x86_mem_commit(ctx, addr, size) == 0) {
            if (args[3]) x86_mem_write32(ctx, args[3], 0x04u); // old protect
            *out = 1;
        } else {
            *out = 0;
        }
        return 1;
    }
    if (sym_eq(symbol, "GetStartupInfoA") || sym_eq(symbol, "GetStartupInfoW")) {
        // 32-bit STARTUPINFO has seven pointer/handle fields that would
        // otherwise cause the Win64 bridge to overwrite the caller's stack.
        const uint32_t info = args[0];
        constexpr uint32_t size = 68;
        if (info && info <= UINT32_MAX - (size - 1)) {
            x86_mem_write32(ctx, info, size);
            for (uint32_t offset = 4; offset < size; offset += 4)
                x86_mem_write32(ctx, info + offset, 0);
        }
        *out = 0;
        return 1;
    }
    if (sym_eq(symbol, "GetSystemInfo") || sym_eq(symbol, "GetNativeSystemInfo")) {
        const uint32_t si = args[0];
        if (!si) { *out = 0; return 1; }
        // 32-bit SYSTEM_INFO is 36 bytes.
        for (int i = 0; i < 36; i++) x86_mem_write8(ctx, si + i, 0);
        x86_mem_write16(ctx, si + 0, 0);        // wProcessorArchitecture (0 = x86)
        x86_mem_write16(ctx, si + 2, 0);        // wReserved
        x86_mem_write32(ctx, si + 4, 4096);     // dwPageSize
        x86_mem_write32(ctx, si + 8, 0x10000);  // lpMinimumApplicationAddress
        x86_mem_write32(ctx, si + 12, 0x7FFEFFFF); // lpMaximumApplicationAddress
        x86_mem_write32(ctx, si + 16, 0xFF);     // dwActiveProcessorMask
        x86_mem_write32(ctx, si + 20, 8);       // dwNumberOfProcessors
        x86_mem_write32(ctx, si + 24, 586);      // dwProcessorType (PROCESSOR_INTEL_PENTIUM)
        x86_mem_write32(ctx, si + 28, 0x10000); // dwAllocationGranularity
        x86_mem_write16(ctx, si + 32, 6);       // wProcessorLevel
        x86_mem_write16(ctx, si + 34, 0);       // wProcessorRevision
        *out = 0;
        return 1;
    }
    if (mod_eq(module, "msvcrt") || mod_eq(module, "ucrtbase")) {
        if (sym_eq(symbol, "malloc")) { *out = x86_guest_alloc(ctx, args[0], 0); return 1; }
        if (sym_eq(symbol, "calloc")) {
            const uint32_t total = args[0] * args[1];
            if (args[0] && args[1] && total / args[1] != args[0]) { *out = 0; return 1; }
            *out = x86_guest_alloc(ctx, total, 1);
            return 1;
        }
        if (sym_eq(symbol, "realloc")) { *out = x86_guest_realloc(ctx, args[0], args[1], 0); return 1; }
        if (sym_eq(symbol, "free") || sym_eq(symbol, "_free")) {
            x86_guest_free(ctx, args[0]);
            *out = 0;
            return 1;
        }
        if (sym_eq(symbol, "_msize")) { *out = x86_guest_blocksize(ctx, args[0]); return 1; }
    }
    return 0;
}

// Per-thread state the guest expects to be local, not host-global.
static int special_import(x86_context* ctx, const char* module, const char* symbol,
                          const uint32_t* args, uint32_t* out) {
    // The 32-bit guest needs its OWN runtime loader and allocator semantics;
    // both are answered before any 64-bit bridge sees the call.
    if (x86_pe32_runtime(ctx, module, symbol, args, out)) return 1;
    if (mod_eq(module, "kernel32") || mod_eq(module, "msvcrt")) {
        if (sym_eq(symbol, "GetLastError")) { *out = (uint32_t)ctx->last_error; return 1; }
        if (sym_eq(symbol, "SetLastError")) { ctx->last_error = (int)args[0]; *out = 0; return 1; }
        if (sym_eq(symbol, "TlsAlloc")) {
            for (int i = 0; i < 64; i++) {
                if (!ctx->regs.tls_used[i]) { ctx->regs.tls_used[i] = 1; *out = (uint32_t)i; return 1; }
            }
            ctx->last_error = 0x5AA; // ERROR_NO_MORE_ITEMS-ish
            *out = 0xFFFFFFFFu;
            return 1;
        }
        if (sym_eq(symbol, "TlsFree")) {
            uint32_t i = args[0];
            if (i < 64) { ctx->regs.tls_used[i] = 0; ctx->regs.tls_slot[i] = 0; *out = 1; }
            else *out = 0;
            return 1;
        }
        if (sym_eq(symbol, "TlsGetValue")) {
            uint32_t i = args[0];
            *out = (i < 64) ? ctx->regs.tls_slot[i] : 0;
            return 1;
        }
        if (sym_eq(symbol, "TlsSetValue")) {
            uint32_t i = args[0];
            if (i < 64) { ctx->regs.tls_slot[i] = args[1]; *out = 1; }
            else { ctx->last_error = 87; *out = 0; }
            return 1;
        }
        // Interlocked*: the guest is single-threaded here, so a plain RMW is
        // equivalent to the atomic form.
        if (symbol[0] == 'I' && ctx->mem.base) {
            uint32_t* p = (uint32_t*)(void*)(ctx->mem.base + args[0]);
            if (sym_eq(symbol, "InterlockedIncrement")) { *out = ++*p; return 1; }
            if (sym_eq(symbol, "InterlockedDecrement")) { *out = --*p; return 1; }
            if (sym_eq(symbol, "InterlockedExchange")) { uint32_t o = *p; *p = args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedExchangeAdd")) { uint32_t o = *p; *p += args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedAdd")) { *p += args[1]; *out = *p; return 1; }
            if (sym_eq(symbol, "InterlockedCompareExchange")) {
                uint32_t o = *p;
                if (o == args[2]) *p = args[1];
                *out = o;
                return 1;
            }
            if (sym_eq(symbol, "InterlockedOr")) { uint32_t o = *p; *p |= args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedAnd")) { uint32_t o = *p; *p &= args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedXor")) { uint32_t o = *p; *p ^= args[1]; *out = o; return 1; }
        }
    }
    return 0;
}

extern "C" void* trt_pe32_trap_host_fn(int index);

// Test/introspection hook: is this import __cdecl (caller cleans)?
extern "C" int x86_is_cdecl(const char* module, const char* symbol) {
    return lookup_cdecl(module, symbol);
}
extern "C" int x86_arg_count(const char* module, const char* symbol) {
    return lookup_arg_count(module, symbol);
}

// Dispatch one imported call. trap_index is -1 when the caller already knows
// the module/symbol (direct call path).
extern "C" uint32_t x86_dispatch_import(x86_context* ctx, int trap_index,
                                        const char* module, const char* symbol,
                                        int* out_nargs, int* out_cdecl) {
    uint32_t args[16];
    uint32_t esp = ctx->regs.esp + 4; // skip return address
    for (int i = 0; i < 16; i++) args[i] = x86_mem_read32(ctx, esp + i * 4);

    int cdecl_call = lookup_cdecl(module, symbol);
    int nargs = lookup_arg_count(module, symbol);
    if (nargs < 0) {
        nargs = trt_dynamic_deduce_arg_count(module, symbol);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "x86_bridge: [AUTONOMOUS SYNTHESIS] %s!%s deduced stdcall nargs=%d",
                 module, symbol ? symbol : "?", nargs);
        if (ctx->log) ctx->log(buf);
    }

    uint32_t result = 0;
    if (special_import(ctx, module, symbol, args, &result)) {
        *out_nargs = nargs;
        *out_cdecl = cdecl_call;
        return result;
    }

    void* fn = (trap_index >= 0) ? trt_pe32_trap_host_fn(trap_index) : nullptr;
    if (!fn && symbol && symbol[0]) fn = trt_resolve_symbol(module, symbol);
    if (!fn) {
        uint32_t syn_val = (uint32_t)trt_dynamic_deduce_return_val(module, symbol);
        char buf[256];
        snprintf(buf, sizeof(buf), "x86_bridge: unresolved %s!%s -> dynamic synth return 0x%x",
                 module, symbol ? symbol : "?", syn_val);
        if (ctx->log) ctx->log(buf);
        *out_nargs = nargs;
        *out_cdecl = cdecl_call;
        return syn_val;
    }
    result = x86_call_fn(ctx, fn, module, symbol, args, nargs);
    *out_nargs = nargs;
    *out_cdecl = cdecl_call;
    return result;
}

// Legacy entry point kept for direct callers.
extern "C" uint32_t x86_import_thunk_handler(x86_context* ctx, const char* module,
                                             const char* symbol) {
    uint32_t args[16];
    uint32_t esp = ctx->regs.esp + 4;
    for (int i = 0; i < 16; i++) args[i] = x86_mem_read32(ctx, esp + i * 4);
    int nargs = lookup_arg_count(module, symbol);
    if (nargs < 0) nargs = 4;
    return x86_call_bridge(ctx, module, symbol, args, nargs);
}
