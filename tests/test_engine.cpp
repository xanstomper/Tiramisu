// tiramisu/tests/test_engine.cpp
// The engine-API batch: memory mapping, threads, system info, wide-char,
// and the quit/focus lifecycle - the APIs a real engine needs before it
// will show a window.
//
// The guest spawns a real worker thread, waits on it, maps a file it wrote
// through the VFS, and queries the OS version. The assertions check that
// each of those reached the runtime.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

namespace {

// ---- bridge entry points (STAGE 6b) ----
// The bridges are TRT_STDCALL = ms_abi (Win64) on the Linux test host — the
// same ABI the guest uses — so the guest IAT binds the bridge addresses
// DIRECTLY, exactly like the console (which has zero trampolines). The old
// hand-written Win64->SysV asm trampolines are gone.
extern "C" {
void* TRT_STDCALL trt_CreateFileMappingW(void*, void*, u32, u32, u32, const u16*);
void* TRT_STDCALL trt_MapViewOfFile(void*, u32, u32, u32, u64);
int TRT_STDCALL trt_UnmapViewOfFile(void*);
u64 TRT_STDCALL trt_CreateThread(void*, void*, void*, void*, u32, u32*);
u32 TRT_STDCALL trt_WaitForSingleObject(u64, u32);
void* TRT_STDCALL trt_CreateEventW(void*, u32, u32, const u16*);
int TRT_STDCALL trt_SetEvent(u64);
int TRT_STDCALL trt_ResetEvent(u64);
int TRT_STDCALL trt_InitializeCriticalSection(void*);
int TRT_STDCALL trt_EnterCriticalSection(void*);
int TRT_STDCALL trt_LeaveCriticalSection(void*);
u32 TRT_STDCALL trt_GetVersionExW(void*);
void TRT_STDCALL trt_GetSystemInfo(void*);
int TRT_STDCALL trt_MultiByteToWideChar(u32, u32, const char*, i32, u16*, i32);
int TRT_STDCALL trt_WideCharToMultiByte(u32, u32, const u16*, i32, char*, i32, const char*, void*);
u32 TRT_STDCALL trt_GetTempPathW(u32, u16*);
u32 TRT_STDCALL trt_GetTempFileNameW(const u16*, const u16*, u32, u16*);
u32 TRT_STDCALL trt_GetTempFileNameA(const char*, const char*, u32, char*);
u32 TRT_STDCALL trt_GetFileAttributesW(const u16*);
int TRT_STDCALL trt_CreateDirectoryW(const u16*, void*);
u64 TRT_STDCALL trt_GetTickCount64(void);
void TRT_STDCALL trt_PostQuitMessage(i32);
void* TRT_STDCALL trt_SetFocus(void*);
void* TRT_STDCALL trt_GetFocus(void);
int TRT_STDCALL trt_GetClientRect(void*, void*);
void* TRT_STDCALL trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
int TRT_STDCALL trt_WriteFile(void*, const void*, u32, u32*, void*);
int TRT_STDCALL trt_CloseHandle(void*);
void* TRT_GUEST_ABI trt_GetStdHandle(u32);
int TRT_GUEST_ABI trt_SetStdHandle(u32, void*);
int TRT_GUEST_ABI trt_GetConsoleMode(void*, u32*);
int TRT_GUEST_ABI trt_SetConsoleCtrlHandler(void*, int);
int TRT_GUEST_ABI trt_ReadConsoleW(void*, void*, u32, u32*, void*);
int TRT_GUEST_ABI trt_WriteConsoleW(void*, const void*, u32, u32*, void*);
int TRT_GUEST_ABI trt_CreateProcessW(const u16*, u16*, void*, void*, int, u32, void*,
                        const u16*, const void*, void*);
void TRT_STDCALL trt_SetLastError(u32);
u32 TRT_STDCALL trt_GetLastError(void);
u32 TRT_STDCALL trt_IsProcessorFeaturePresent(u32);
int TRT_STDCALL trt_GetCPInfo(u32, void*);
void* TRT_STDCALL trt_FindFirstFileExW(const u16*, u32, void*, u32, void*, u32);
void* TRT_STDCALL trt_FindFirstFileA(const char*, void*);
int   TRT_STDCALL trt_FindNextFileA(void*, void*);
int TRT_STDCALL trt_GetFileAttributesExW(const u16*, u32, void*);
int TRT_STDCALL trt_FindNextFileW(void*, void*);
int TRT_STDCALL trt_FindClose(void*);
void* TRT_STDCALL trt_LoadLibraryW(const u16*);
void* TRT_STDCALL trt_GetProcAddress(void*, const char*);
u32 TRT_STDCALL trt_FlsAlloc(void*);
void* TRT_STDCALL trt_FlsGetValue(u32);
void* TRT_STDCALL trt_FlsGetValue2(u32);
int TRT_STDCALL trt_FlsSetValue(u32, void*);
u32 TRT_STDCALL trt_GetFileAttributesW(const u16*);
int TRT_STDCALL trt_ReadFile(void*, void*, u32, u32*, void*);
void* TRT_STDCALL trt_VirtualAlloc(void*, u64, u32, u32);
int TRT_STDCALL trt_VirtualFree(void*, u64, u32);
int TRT_STDCALL trt_VirtualProtect(void*, u64, u32, u32*);
u64 TRT_STDCALL trt_VirtualQuery(const void*, void*, u64);
int TRT_STDCALL trt_FlushInstructionCache(void*, const void*, u64);
}


struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = nullptr;
  if (preferred_base) {
    // ASan's shadow gap swallows the guest's usual preferred bases
    // (0x140000000 is INSIDE it — measured "unknown-crash" false positives):
    // under sanitizers, let the kernel pick and let the loader relocate.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    (defined(__clang__) && __has_feature(address_sanitizer)))
    const uintptr_t gap_lo = 0x00008fff7000ull, gap_hi = 0x02008fff6fffull;
    if (preferred_base >= gap_lo && preferred_base <= gap_hi) preferred_base = 0;
#endif
    if (preferred_base)
      p = mmap((void*)(uintptr_t)preferred_base, bytes, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!p || p == MAP_FAILED)
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

// Linux-host stand-in for RtlAddFunctionTable (mirrors test_corpus_run.cpp).
// The console host registers .pdata with the real RtlAddFunctionTable; the
// trt_RtlLookupFunctionEntry bridge scans img->pdata directly, so a no-op
// success here keeps the backend contract honest without a libgcc unwind
// dependency in this test.
static int linux_register_unwind(void* image_base, const void* pdata,
                                 u32 count) {
  (void)image_base; (void)pdata; (void)count;
  return 0;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: test_engine <guest.exe> <drive_c_dir>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  // STAGE 7: the SEH/unwind guest (argv[3], optional) exercises the
  // unwind-edit surface OrTMM's CRT startup runs before WinMain
  u8* seh_file = nullptr;
  size_t seh_size = 0;
  if (argc >= 4) seh_file = read_file(argv[3], &seh_size);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  // the guest binds the bridge addresses DIRECTLY (ms_abi = the guest's
  // own ABI — exactly like the console IAT, zero trampolines)
  {
    static const trt_named_fn k_k32[] = {
        {"CreateFileMappingW", (void*)&trt_CreateFileMappingW},
        {"MapViewOfFile", (void*)&trt_MapViewOfFile},
        {"UnmapViewOfFile", (void*)&trt_UnmapViewOfFile},
        {"CreateThread", (void*)&trt_CreateThread},
        {"WaitForSingleObject", (void*)&trt_WaitForSingleObject},
        {"CreateEventW", (void*)&trt_CreateEventW},
        {"SetEvent", (void*)&trt_SetEvent},
        {"ResetEvent", (void*)&trt_ResetEvent},
        {"InitializeCriticalSection", (void*)&trt_InitializeCriticalSection},
        {"EnterCriticalSection", (void*)&trt_EnterCriticalSection},
        {"LeaveCriticalSection", (void*)&trt_LeaveCriticalSection},
        {"GetVersionExW", (void*)&trt_GetVersionExW},
        {"GetSystemInfo", (void*)&trt_GetSystemInfo},
        {"MultiByteToWideChar", (void*)&trt_MultiByteToWideChar},
        {"WideCharToMultiByte", (void*)&trt_WideCharToMultiByte},
        {"GetTempPathW", (void*)&trt_GetTempPathW},
        {"GetTickCount64", (void*)&trt_GetTickCount64},
        {"CreateFileW", (void*)&trt_CreateFileW},
        {"WriteFile", (void*)&trt_WriteFile},
        {"CloseHandle", (void*)&trt_CloseHandle},
        {"GetStdHandle", (void*)&trt_GetStdHandle},
        {"SetStdHandle", (void*)&trt_SetStdHandle},
        {"GetConsoleMode", (void*)&trt_GetConsoleMode},
        {"SetConsoleCtrlHandler", (void*)&trt_SetConsoleCtrlHandler},
        {"ReadConsoleW", (void*)&trt_ReadConsoleW},
        {"WriteConsoleW", (void*)&trt_WriteConsoleW},
        {"CreateProcessW", (void*)&trt_CreateProcessW},
        {"SetLastError", (void*)&trt_SetLastError},
        {"GetLastError", (void*)&trt_GetLastError},
        {"IsProcessorFeaturePresent", (void*)&trt_IsProcessorFeaturePresent},
        {"FindFirstFileExW", (void*)&trt_FindFirstFileExW},
        {"FindNextFileW", (void*)&trt_FindNextFileW},
        {"FindClose", (void*)&trt_FindClose},
    };
    static const trt_named_fn k_u32[] = {
        {"PostQuitMessage", (void*)&trt_PostQuitMessage},
        {"SetFocus", (void*)&trt_SetFocus},
        {"GetFocus", (void*)&trt_GetFocus},
        {"GetClientRect", (void*)&trt_GetClientRect},
    };
    trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_k32,
                         (int)(sizeof(k_k32) / sizeof(k_k32[0])));
    trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_u32,
                         (int)(sizeof(k_u32) / sizeof(k_u32[0])));
  }

  // the guest's C: drive (its asset files live there)
  trt_vfs_mount("c:", argv[2]);

  printf("tiramisu engine-API batch test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (engine APIs bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x3f1fff)\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "GetVersionExW answered");
  CHECK(ret & 0x2u, "GetSystemInfo answered");
  CHECK(ret & 0x4u, "MultiByteToWideChar converted");
  CHECK(ret & 0x8u, "WideCharToMultiByte converted");
  CHECK(ret & 0x10u, "GetTempPathW answered");
  CHECK(ret & 0x20u, "CreateFileMapping/MapViewOfFile carried the file's bytes");
  CHECK(ret & 0x40u, "CreateThread started a real worker thread");
  CHECK(ret & 0x80u, "the worker ran and WaitForSingleObject returned");
  CHECK(ret & 0x100u, "SetEvent/ResetEvent worked");
  CHECK(ret & 0x200u, "focus lifecycle (SetFocus/GetFocus)");
  CHECK(ret & 0x400u, "PostQuitMessage accepted");
  CHECK(ret & 0x800u, "GetClientRect answered");
  CHECK(ret & 0x1000u, "GetTickCount64 moves");
  // console/child-process honesty (OrTMM's kernel32 imports)
  CHECK(ret & 0x2000u, "CreateProcessW -> FALSE (console blocks children)");
  CHECK(ret & 0x4000u, "CreateProcessW names ERROR_ACCESS_DENIED");
  CHECK(ret & 0x8000u, "CreateProcessW fills no fake PROCESS_INFORMATION");
  CHECK(ret & 0x10000u, "GetStdHandle -> synthetic stdio pseudo-handle (-11)");
  CHECK(ret & 0x4000000u, "GetStdHandle(non-std) -> NULL + ERROR_INVALID_HANDLE");
  CHECK(ret & 0x20000u, "SetStdHandle -> FALSE");
  CHECK(ret & 0x40000u, "GetConsoleMode -> FALSE (no console)");
  CHECK(ret & 0x80000u, "SetConsoleCtrlHandler -> FALSE");
  CHECK(ret & 0x100000u, "ReadConsoleW -> FALSE (no console)");
  CHECK(ret & 0x200000u, "WriteConsoleW -> FALSE (no console)");
  // STAGE 7 (OrTMM): the CRT startup gate. PF_FASTFAIL_AVAILABLE (0x17)
  // MUST answer present — a 0 sent _set_Fflag to __fastfail and the EXE
  // died with int 0x29 before WinMain (measured at OrTMM+0xd1d4).
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0x17, 0, 0) == 1,
        "IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE) -> 1 (CRT startup gate)");
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0, 0, 0) == 1,
        "IsProcessorFeaturePresent(PF_COMPARABLE_EXCEPT) -> 1");
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0x30, 0, 0) == 0,
        "IsProcessorFeaturePresent(unknown) -> 0 (honest no)");

  // ---- STAGE 7: GetCPInfo writes EXACTLY sizeof(CPINFO)=18 bytes ----------
  // MAX_LEADBYTES is 12 (winnls.h), not 126. A 132-byte memset here zeroed
  // the MSVC /GS cookie the CRT keeps 0x18 bytes past its frame-local
  // CPINFO — every corpus DLL + OrTMM died in __raise_securityfailure
  // (int 0x29) before WinMain/DllMain finished (pinned under gdb).
  {
    u8 guard_buf[64];
    memset(guard_buf, 0xA5, sizeof(guard_buf));
    // Win64 ABI: rcx=codepage, rdx=cpinfo buffer
    win64_call((void*)&trt_GetCPInfo, 1252, (u64)(uintptr_t)guard_buf, 0);
    int ok = 1;
    for (size_t i = 20; i < sizeof(guard_buf); ++i)
      if (guard_buf[i] != 0xA5) ok = 0;  // anything past 20 = stack overwrite
    CHECK(ok, "GetCPInfo(1252) wrote only sizeof(CPINFO)=20 (no /GS cookie kill)");
    CHECK(((u32*)guard_buf)[0] == 1, "GetCPInfo(1252) MaxCharSize=1 (SBCS)");
  }

  // ---- STAGE 7: FLS surface roundtrip incl. FlsGetValue2 ------------------
  // OrTMM's UCRT resolves FlsGetValue2 BY NAME via GetProcAddress; before the
  // bridge export existed the blanket-0 stub answered NULL forever and the CRT
  // locale/FLS init storm-allocated 968 bytes x716k (2.9GB, watchdog exit 43).
  {
    const u64 fls_idx = win64_call((void*)&trt_FlsAlloc, 0, 0, 0);
    CHECK(fls_idx != 0xFFFFFFFFull, "FlsAlloc returned a valid index");
    const u64 cookie = 0xC0FFEE123456789ull;
    CHECK(win64_call((void*)&trt_FlsSetValue, fls_idx, cookie, 0) == 1,
          "FlsSetValue(idx,cookie) -> nonzero");
    CHECK(win64_call((void*)&trt_FlsGetValue, fls_idx, 0, 0) == cookie,
          "FlsGetValue(idx) == cookie (roundtrip)");
    CHECK(win64_call((void*)&trt_FlsGetValue2, fls_idx, 0, 0) == cookie,
          "FlsGetValue2(idx) == cookie (the OrTMM storm fix)");
    CHECK(win64_call((void*)&trt_FlsGetValue2, 0xFFFFFFFFull, 0, 0) == 0,
          "FlsGetValue2(invalid) -> NULL");
    CHECK(win64_call((void*)&trt_FlsSetValue, 0xFFFFFFFFull, 0, 0) == 0,
          "FlsSetValue(invalid) -> 0");
  }

  // ---- STAGE 7: relative guest paths resolve against the guest cwd drive -
  // PyInstaller extracts its archive via RELATIVE paths (".\_MEIxxxx\entry").
  // The resolver only understood "X:" drives -> every relative CreateFileW
  // failed PATH_NOT_FOUND and extraction aborted ("Failed to extract
  // NoThumbnail.png"). Here only "c:" is mounted, so it becomes the cwd
  // drive; ".\rel.txt" must land under the mount target, not the host cwd.
  {
    char host[1024];
    const u16 dot_rel[] = {'.','\\','r','e','l','.','t','x','t',0};
    CHECK(trt_vfs_resolve_utf16(dot_rel, host, sizeof(host)) == 0,
          "resolve(.\\rel.txt) hits the guest cwd drive");
    CHECK(strstr(host, "/rel.txt") && strstr(host, "drive_c"),
          "resolve(.\\rel.txt) lands under the c: mount (not host cwd)");
    const u16 bare_rel[] = {'s','u','b','\\','f',0};
    CHECK(trt_vfs_resolve_utf16(bare_rel, host, sizeof(host)) == 0,
          "resolve(sub\\f) also resolves relative");
    const u16 drive_rel[] = {'c',':','\\','x',0};
    CHECK(trt_vfs_resolve_utf16(drive_rel, host, sizeof(host)) == 0,
          "resolve(c:\\x) still hits the drive mount");
    const u16 unmounted[] = {'q',':','\\','x',0};
    CHECK(trt_vfs_resolve_utf16(unmounted, host, sizeof(host)) != 0,
          "resolve(q:\\x) fails (unmounted drive, fail-visible)");
  }

  // ---- STAGE 7: GetTempPathW returns a GUEST path; CreateDirectoryW digs --
  // GetTempPathW used to return the HOST path ("/tmp/"): PyInstaller built
  // "/tmp\_MEIxxxx", the resolver mis-read host-absolute paths as relative
  // guest paths, the temp dir never materialised and the candidate probe
  // looped 2.5M opens (watchdog exit 43). The bridge must answer the guest
  // naming world. CreateDirectoryW must create missing parents because the
  // drive_c mount starts empty ("C:\Temp\_MEIxxxx" needs C:\Temp first).
  {
    u16 tbuf[64]; const u32 n =
      (u32)win64_call((void*)&trt_GetTempPathW, 64, (u64)(uintptr_t)tbuf, 0);
    CHECK(n == 8 && tbuf[n] == 0, "GetTempPathW success excludes the terminator");
    u16 short_buf[8]{0x1122};
    CHECK(trt_GetTempPathW(0, nullptr) == 9 &&
              trt_GetTempPathW(8, short_buf) == 9 && short_buf[0] == 0x1122,
          "GetTempPathW query and short buffer return the required capacity");
    char t8[128];
    for (u32 i = 0; i < n && i < 63; ++i) t8[i] = (char)tbuf[i];
    t8[n && n <= 63 ? n : 63] = 0;
    CHECK(t8[1] == ':' && t8[2] == '\\',
          "GetTempPathW returns a GUEST drive path (C:\\Temp\\), not /tmp/");
    // CreateDirectoryW with parents: real dirs must appear under the mount
    const u16 mk_path[] = {'c',':','\\','T','e','m','p','\\','_','M','E','I','t',
                           0};
    CHECK(win64_call((void*)&trt_CreateDirectoryW, (u64)(uintptr_t)mk_path, 0,
                     0) == 1,
          "CreateDirectoryW(C:\\Temp\\_MEIt) -> nonzero");
    struct stat st;
    char want[1024];
    snprintf(want, sizeof(want), "%s/Temp/_MEIt", argv[2]);
    CHECK(stat(want, &st) == 0 && (st.st_mode & S_IFDIR),
          "C:\\Temp\\_MEIt materialised as real dirs under the c: mount");

    const u16 prefix[] = {'e','n','g','i','n','e',0};
    u16 named[260]{}, first[260]{};
    CHECK(trt_GetTempFileNameW(tbuf, prefix, 1, named) == 1 &&
              trt_GetFileAttributesW(named) == 0xffffffffu,
          "a specified temp-file ID returns a path without creating a file");
    const void* invalid = reinterpret_cast<void*>(static_cast<uintptr_t>(-1));
    void* existing = trt_CreateFileW(named, 0x40000000u, 0, nullptr, 1, 0, nullptr);
    CHECK(existing != invalid, "create a conflicting temp filename");
    if (existing != invalid) trt_CloseHandle(existing);
    CHECK(trt_CreateFileW(named, 0x40000000u, 0, nullptr, 1, 0, nullptr) == invalid,
          "CREATE_NEW refuses to reopen or truncate an existing file");
    const u32 first_id = trt_GetTempFileNameW(tbuf, prefix, 0, first);
    CHECK(first_id && first_id != 1 && trt_GetFileAttributesW(first) != 0xffffffffu,
          "GetTempFileNameW skips occupied IDs and creates an empty file");
    char other[260]{};
    const u32 other_id = trt_GetTempFileNameA("C:\\Temp", "engine", 0, other);
    CHECK(other_id && other_id != first_id && strstr(other, "C:\\Temp\\eng") == other,
          "GetTempFileNameA shares the allocator and adds the missing separator");
    const u16 missing[] = {'C',':','\\','n','o','p','e',0};
    CHECK(trt_GetTempFileNameW(missing, prefix, 0, first) == 0,
          "GetTempFileNameW reports a missing directory");
  }

  // ---- STAGE 7: LoadLibraryW bare-name miss falls back to system modules --
  // dstoragecore's obfuscation runtime calls LoadLibraryExW("advapi32");
  // trt_LoadLibraryW resolves the bare name to <gamedir>/advapi32 (which does
  // not exist) and used to return NULL without trying the synthetic system
  // module table — the runtime then aborted via int 0x29(ec=7).
  {
    // UTF-16 by hand: clang on Linux makes L"" literals UTF-32 (4 bytes per
    // unit) — a literal here would hand the bridge a 'a',0x0,... string
    const u16 advapi32_w[] = {'a','d','v','a','p','i','3','2',0};
    const int ll_ok = (int)win64_call((void*)&trt_LoadLibraryW,
                                      (u64)(uintptr_t)advapi32_w, 0, 0);
    CHECK(ll_ok != 0, "LoadLibraryW(\"advapi32\") -> non-NULL (system-module fallback)");
    // and the obfuscation runtime's actual target resolves by name
    const u64 gpa = win64_call((void*)&trt_GetProcAddress, ll_ok,
                               (u64)(uintptr_t)"SystemFunction036", 0);
    CHECK(gpa != 0, "GetProcAddress(advapi32, SystemFunction036) -> non-NULL");
  }


  // ---- STAGE 7: the SEH/unwind guest (OrTMM's CRT startup surface) ----
  if (seh_file) {
    trt_resolver rs2;
    trt_resolver_default(&rs2);
    trt_image* img2 = nullptr;
    const int rc2 = trt_map_pe(&be, &rs2, seh_file, seh_size, &img2);
    CHECK(rc2 == TRT_OK, "SEH guest PE maps");
    if (rc2 == TRT_OK && img2) {
      // the console host registers the main guest image (HMODULE == base);
      // RtlLookupFunctionEntry walks exactly that registry
      trt_images_register_main(img2, "guest_seh.exe");
      CHECK(trt_seal_image(&be, img2) == TRT_OK, "SEH image sealed (W^X)");
      const u64 sret = win64_call(img2->base + img2->entry_rva, 0, 0, 0);
      printf("  SEH guest returned 0x%llx (want 0xf)\n", (unsigned long long)sret);

      CHECK((sret & 0x3u) == 0x3u, "VirtualProtect RW->R dance (honest PAGE_* codes)");
      CHECK(sret & 0x4u, "page writable again after the protect dance");
      CHECK(sret & 0x8u, "VirtualProtect restored RW after R");
      CHECK(sret & 0x10u, "RtlLookupFunctionEntry answered for a live RIP");
    }
    free(seh_file);
  }

  // ---- STAGE 8: directory enumeration (the FindFirstFile family) ----
  // The measured PyInstaller/engine blocker: FindFirstFileExW used to be an
  // INVALID_HANDLE-stub. Prove real enumeration through the bridge surface:
  // seed a dedicated subdirectory, enumerate it via a wildcard, verify names
  // arrive in a real-layout WIN32_FIND_DATAW (cFileName at +44), and that
  // FindClose is honest both ways.
  {
    // seed a dedicated dir host-side (keeps other suites' files out of the count)
    char seeddir[600];
    snprintf(seeddir, sizeof(seeddir), "%s/seedfind", argv[2]);
    mkdir(seeddir, 0755);
    char seed[700];
    snprintf(seed, sizeof(seed), "%s/seed_a.dat", seeddir);
    FILE* f = fopen(seed, "wb"); if (f) { fputs("a", f); fclose(f); }
    snprintf(seed, sizeof(seed), "%s/seed_b.dat", seeddir);
    f = fopen(seed, "wb"); if (f) { fputs("bb", f); fclose(f); }
    snprintf(seed, sizeof(seed), "%s/seed_c.txt", seeddir);
    f = fopen(seed, "wb"); if (f) { fputs("ccc", f); fclose(f); }

    // guest-side calls through the registered bridge (ms_abi on Linux);
    // struct mirrors the REAL x64 WIN32_FIND_DATAW the guest would pass
    // (FILETIMEs are 4-aligned in Win32: attrs +0, times +4/+12/+20,
    // size hi/lo +28/+32, reserved +36/+40, name +44)
    struct find_data_w { u32 attrs; u32 c_lo, c_hi, a_lo, a_hi, w_lo, w_hi;
                         u32 szhi, szlo, r1, r2;
                         u16 name[260]; u16 alt[14]; };
    static_assert(offsetof(find_data_w, name) == 44, "real WIN32_FIND_DATAW cFileName offset");
    static find_data_w fd;
    memset(&fd, 0, sizeof(fd));
    u16 pat[] = { 'C', ':', '\\', 's', 'e', 'e', 'd', 'f', 'i', 'n', 'd', '\\', '*', 0 };
    // real Win64 ABI: 6 args (name, level, data, searchOp, extra, flags)
    void* fh = trt_FindFirstFileExW(pat, 0, &fd, 0, nullptr, 0);
    int found = 0, dat_seen = 0, txt_seen = 0;
    char first_name[64] = {};
    if (fh != (void*)(uintptr_t)-1) {
      for (;;) {
        ++found;
        char n[64] = {};
        for (int i = 0; fd.name[i] && i < 63; ++i) n[i] = (char)fd.name[i];
        if (!first_name[0]) snprintf(first_name, sizeof(first_name), "%s", n);
        if (strstr(n, ".dat")) ++dat_seen;
        if (strstr(n, ".txt")) ++txt_seen;
        memset(&fd, 0, sizeof(fd));
        if (!trt_FindNextFileW(fh, &fd)) break;
      }
      CHECK(trt_FindClose(fh) == 1, "FindClose -> TRUE");
      // re-close is honestly FALSE (invalid handle after free)
      CHECK(trt_FindClose(fh) == 0, "double FindClose -> FALSE (honest)");
    }
    CHECK(fh != (void*)(uintptr_t)-1, "FindFirstFileExW(C:\\seedfind\\*) -> real handle");
    // C:\Temp lives in the drive ROOT, not seedfind — the enumeration is
    // exactly the 3 seeded files
    CHECK(found == 3, "enumerated exactly the 3 seeded files");
    CHECK(dat_seen == 2 && txt_seen == 1, "wildcard matched *.dat x2 + *.txt x1");
    // size arrives at the real offset (seed_c.txt = 3 bytes)
    // targeted wildcard
    u16 pat2[] = { 'C', ':', '\\', 's', 'e', 'e', 'd', 'f', 'i', 'n', 'd', '\\', '*', '.', 't', 'x', 't', 0 };
    memset(&fd, 0, sizeof(fd));
    void* fh2 = trt_FindFirstFileExW(pat2, 0, &fd, 0, nullptr, 0);
    CHECK(fh2 != (void*)(uintptr_t)-1, "FindFirstFileExW(C:\\seedfind\\*.txt) -> handle");
    if (fh2 != (void*)(uintptr_t)-1) {
      char n[64] = {};
      for (int i = 0; fd.name[i] && i < 63; ++i) n[i] = (char)fd.name[i];
      CHECK(strstr(n, "seed_c.txt") != nullptr, "pattern *.txt matched seed_c.txt");
      CHECK(trt_FindNextFileW(fh2, &fd) == 0, "no more *.txt (honest end)");
      trt_FindClose(fh2);
    }
    // missing dir -> INVALID_HANDLE_VALUE + named log (fail-visible)
    u16 pat3[] = { 'C', ':', '\\', 'n', 'o', 'd', 'i', 'r', '\\', '*', 0 };
    CHECK(trt_FindFirstFileExW(pat3, 0, &fd, 0, nullptr, 0) == (void*)(uintptr_t)-1,
          "FindFirstFileExW(missing dir) -> INVALID_HANDLE_VALUE");

    // ---- ANSI wrappers: many apps import FindFirstFileA, which used to fall
    // through to the real host API (no guest C:\ there). The A path must
    // narrow the same 592-byte record to the 320-byte A layout.
    struct find_data_a { u32 attrs; u32 c_lo, c_hi, a_lo, a_hi, w_lo, w_hi;
                         u32 szhi, szlo, r1, r2;
                         char name[260]; char alt[14]; char pad[2]; };
    static_assert(offsetof(find_data_a, name) == 44, "WIN32_FIND_DATAA cFileName offset");
    static_assert(sizeof(find_data_a) == 320, "WIN32_FIND_DATAA is 320 bytes");
    find_data_a fa;
    memset(&fa, 0, sizeof(fa));
    void* ah = trt_FindFirstFileA("C:\\seedfind\\*.txt", &fa);
    CHECK(ah != (void*)(uintptr_t)-1, "FindFirstFileA(C:\\seedfind\\*.txt) -> handle");
    if (ah != (void*)(uintptr_t)-1) {
      CHECK(strstr(fa.name, "seed_c.txt") != nullptr, "A record carries seed_c.txt");
      CHECK(fa.szlo == 3 && fa.szhi == 0, "A record carries the 3-byte size");
      memset(&fa, 0, sizeof(fa));
      CHECK(trt_FindNextFileA(ah, &fa) == 0, "A enumeration ends honestly");
      CHECK(trt_FindClose(ah) == 1, "A handle closes");
    }
    CHECK(trt_FindFirstFileA("C:\\nodir\\*", &fa) == (void*)(uintptr_t)-1,
          "FindFirstFileA(missing dir) -> INVALID_HANDLE_VALUE");
  }

  // ---- STAGE 9: WIN32_FILE_ATTRIBUTE_DATA layout (GetFileAttributesExW) ----
  // Regression: the sizes were written at +36/+40 (one u32 past the real
  // 36-byte x64 record), which zeroed a guest /GS stack cookie in OrTMM and
  // fastfailed the boot. Guard dwords past the record must survive the call.
  {
    char seed[600];
    snprintf(seed, sizeof(seed), "%s/attr_probe.bin", argv[2]);
    FILE* f = fopen(seed, "wb");
    if (f) { fputs("attribute-probe", f); fclose(f); }
    u16 wname[300];
    const char* rel = "attr_probe.bin";
    u32 wi = 0;
    for (; rel[wi]; ++wi) wname[wi] = (u16)(u8)rel[wi];
    wname[wi] = 0;
    struct attr_data_w { u32 attrs, c_lo, c_hi, a_lo, a_hi, w_lo, w_hi, szhi, szlo; };
    static_assert(sizeof(attr_data_w) == 36, "real x64 WIN32_FILE_ATTRIBUTE_DATA");
    struct guarded { u64 g0, g1; attr_data_w d; u64 g2, g3; };
    static guarded gd;
    gd.g0 = 0xA11CE111; gd.g1 = 0xB22CE222; gd.g2 = 0xC33DE333; gd.g3 = 0xD44EE444;
    memset(&gd.d, 0, sizeof(gd.d));
    CHECK(trt_GetFileAttributesExW(wname, 0, &gd.d) == 1,
          "GetFileAttributesExW -> TRUE");
    CHECK(gd.g0 == 0xA11CE111 && gd.g1 == 0xB22CE222 &&
          gd.g2 == 0xC33DE333 && gd.g3 == 0xD44EE444,
          "guard qwords intact (no write past the 36-byte record)");
    CHECK(gd.d.szlo == 15 && gd.d.szhi == 0, "size at the real +32/+28 offsets");
    // Missing path -> FALSE (real Win32). Returning TRUE wedged PyInstaller's
    // unique-temp-dir search into an infinite silent probe loop (OrTMM v0.33).
    u16 wmiss[300];
    const char* relm = "attr_missing_zz.bin";
    u32 mi = 0;
    for (; relm[mi]; ++mi) wmiss[mi] = (u16)(u8)relm[mi];
    wmiss[mi] = 0;
    CHECK(trt_GetFileAttributesExW(wmiss, 0, &gd.d) == 0,
          "GetFileAttributesExW(missing) -> FALSE, not a silent TRUE");
  }

  // ---- G2: directory attributes + Z: write overlay + in-place remount ----
  {
    auto w16 = [](u16* dst, const char* s) {
      while (*s) *dst++ = (u16)(u8)*s++;
      *dst = 0;
    };
    u16 wpath[256];
    w16(wpath, "C:\\seedfind");
    CHECK(trt_GetFileAttributesW(wpath) == 0x10u,
          "GetFileAttributesW(dir) -> FILE_ATTRIBUTE_DIRECTORY (0x10)");

    struct { u32 attrs, c_lo, c_hi, a_lo, a_hi, w_lo, w_hi, szhi, szlo; } ex{};
    CHECK(trt_GetFileAttributesExW(wpath, 0, &ex) == 1 && ex.attrs == 0x10u &&
              ex.szlo == 0 && ex.szhi == 0,
          "GetFileAttributesExW(dir) -> attrs=0x10, size=0");

    char zdir[512], zfile[600], ovfile[600], zdir2[512], zfile2[600];
    snprintf(zdir, sizeof(zdir), "%s_z_test", argv[2]);
    snprintf(zfile, sizeof(zfile), "%s/ro.txt", zdir);
    snprintf(ovfile, sizeof(ovfile), "%s/_z_write/ro.txt", argv[2]);
    snprintf(zdir2, sizeof(zdir2), "%s_z_test2", argv[2]);
    snprintf(zfile2, sizeof(zfile2), "%s/ro2.txt", zdir2);
    mkdir(zdir, 0755);
    mkdir(zdir2, 0755);
    FILE* fz = fopen(zfile, "wb");
    if (fz) { fwrite("orig", 1, 4, fz); fclose(fz); }
    fz = fopen(zfile2, "wb");
    if (fz) { fwrite("two2", 1, 4, fz); fclose(fz); }

    trt_vfs_mount("z:", zdir);

    w16(wpath, "Z:\\ro.txt");
    void* hr0 = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0x80u, nullptr);
    char buf[16] = {};
    u32 nread = 0;
    int r0ok = (hr0 != (void*)(uintptr_t)-1) &&
               trt_ReadFile(hr0, buf, 4, &nread, nullptr) == 1;
    if (hr0 != (void*)(uintptr_t)-1) trt_CloseHandle(hr0);
    CHECK(r0ok && nread == 4 && memcmp(buf, "orig", 4) == 0,
          "Z:\\ro.txt reads original content before write");

    void* hw = trt_CreateFileW(wpath, 0x40000000u, 0u, nullptr, 2u, 0x80u, nullptr);
    u32 nwr = 0;
    int wok = (hw != (void*)(uintptr_t)-1) &&
              trt_WriteFile(hw, "over", 4, &nwr, nullptr) == 1 && nwr == 4;
    if (hw != (void*)(uintptr_t)-1) trt_CloseHandle(hw);
    CHECK(wok, "Z:\\ro.txt write succeeds via _z_write overlay");

    char orig_chk[8] = {}, ov_chk[8] = {};
    FILE* fo = fopen(zfile, "rb");
    if (fo) { fread(orig_chk, 1, 4, fo); fclose(fo); }
    FILE* fv = fopen(ovfile, "rb");
    if (fv) { fread(ov_chk, 1, 4, fv); fclose(fv); }
    CHECK(memcmp(orig_chk, "orig", 4) == 0 && memcmp(ov_chk, "over", 4) == 0,
          "Z: write leaves read-only source untouched and writes C:\\_z_write\\ro.txt");

    memset(buf, 0, sizeof(buf));
    nread = 0;
    void* hr1 = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0x80u, nullptr);
    int r1ok = (hr1 != (void*)(uintptr_t)-1) &&
               trt_ReadFile(hr1, buf, 4, &nread, nullptr) == 1;
    if (hr1 != (void*)(uintptr_t)-1) trt_CloseHandle(hr1);
    CHECK(r1ok && nread == 4 && memcmp(buf, "over", 4) == 0,
          "re-reading Z:\\ro.txt observes overlaid bytes");

    w16(wpath, "Z:\\sub\\deep");
    CHECK(trt_CreateDirectoryW(wpath, nullptr) == 1,
          "CreateDirectoryW(Z:\\sub\\deep) creates overlay parents");
    CHECK(trt_GetFileAttributesW(wpath) == 0x10u,
          "GetFileAttributesW(Z:\\sub\\deep) -> 0x10 directory");

    w16(wpath, "Z:\\sub\\deep\\f.txt");
    void* hs = trt_CreateFileW(wpath, 0x40000000u, 0u, nullptr, 2u, 0x80u, nullptr);
    nwr = 0;
    int sok = (hs != (void*)(uintptr_t)-1) &&
              trt_WriteFile(hs, "hi", 2, &nwr, nullptr) == 1 && nwr == 2;
    if (hs != (void*)(uintptr_t)-1) trt_CloseHandle(hs);
    CHECK(sok, "writing Z:\\sub\\deep\\f.txt succeeds");

    struct find_data_w2 {
      u32 attrs;
      u32 c_lo, c_hi, a_lo, a_hi, w_lo, w_hi;
      u32 szhi, szlo, r1, r2;
      u16 name[260];
      u16 alt[14];
    };
    find_data_w2 fdz{};
    u16 wpat[256];
    w16(wpat, "Z:\\sub\\deep\\*");
    void* fhz = trt_FindFirstFileExW(wpat, 0, &fdz, 0, nullptr, 0);
    bool saw_f = false;
    if (fhz != (void*)(uintptr_t)-1) {
      for (;;) {
        char nm[64] = {};
        for (int i = 0; fdz.name[i] && i < 63; ++i) nm[i] = (char)fdz.name[i];
        if (strcmp(nm, "f.txt") == 0) saw_f = true;
        if (!trt_FindNextFileW(fhz, &fdz)) break;
      }
      trt_FindClose(fhz);
    }
    CHECK(saw_f, "FindFirstFileExW(Z:\\sub\\deep\\*) enumerates f.txt");

    trt_vfs_mount("z:", zdir2);
    w16(wpath, "Z:\\ro2.txt");
    CHECK(trt_GetFileAttributesW(wpath) == 0x80u,
          "trt_vfs_mount(\"z:\") updates existing mount in-place");
  }

  // ---- G3: VirtualAlloc / VirtualQuery / FlushInstructionCache / W->RX JIT ----
  {
    struct mbi64 {
      u64 BaseAddress;
      u64 AllocationBase;
      u32 AllocationProtect;
      u16 PartitionId;
      u16 _pad;
      u64 RegionSize;
      u32 State;
      u32 Protect;
      u32 Type;
      u32 _pad2;
    };
    static_assert(sizeof(mbi64) == 48, "mbi64 layout");

    void* p = trt_VirtualAlloc(nullptr, 8192, 0x2000u /*MEM_RESERVE*/, 0x01u /*PAGE_NOACCESS*/);
    CHECK(p != nullptr, "VirtualAlloc(MEM_RESERVE, 8192) -> non-NULL");

    mbi64 mbi{};
    CHECK(trt_VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi),
          "VirtualQuery(reserved) -> 48");
    CHECK(mbi.State == 0x2000u && mbi.AllocationBase == (u64)(uintptr_t)p &&
              mbi.RegionSize >= 8192u,
          "VirtualQuery reports MEM_RESERVE with matching AllocationBase");

    void* pc = trt_VirtualAlloc(p, 4096, 0x1000u /*MEM_COMMIT*/, 0x04u /*PAGE_READWRITE*/);
    CHECK(pc == p, "VirtualAlloc(p, MEM_COMMIT, RW) commits inside reserved range");

    memset(&mbi, 0, sizeof(mbi));
    CHECK(trt_VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
              mbi.State == 0x1000u && mbi.Protect == 0x04u,
          "VirtualQuery reports MEM_COMMIT + PAGE_READWRITE");

    // Emit Win64/x64 stub: mov eax, 42; ret
    u8* code = (u8*)p;
    code[0] = 0xB8;
    code[1] = 0x2A;
    code[2] = 0x00;
    code[3] = 0x00;
    code[4] = 0x00;
    code[5] = 0xC3;

    u32 oldp = 0;
    CHECK(trt_VirtualProtect(p, 4096, 0x20u /*PAGE_EXECUTE_READ*/, &oldp) == 1,
          "VirtualProtect(RW -> RX) succeeds for JIT code");
    CHECK(trt_FlushInstructionCache((void*)(uintptr_t)-1, p, 4096) == 1,
          "FlushInstructionCache -> TRUE");

    memset(&mbi, 0, sizeof(mbi));
    CHECK(trt_VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Protect == 0x20u,
          "VirtualQuery sees VirtualProtect -> PAGE_EXECUTE_READ (0x20)");
    CHECK(win64_call(p, 0, 0, 0) == 42u,
          "JIT W->RX code executes and returns 42");

    trt_image* qimg = trt_images_first();
    if (!qimg) {
      trt_images_register_main(img, "guest_engine.exe");
      qimg = img;
    }
    memset(&mbi, 0, sizeof(mbi));
    CHECK(trt_VirtualQuery(qimg->base, &mbi, sizeof(mbi)) == sizeof(mbi) &&
              mbi.State == 0x1000u && mbi.Type == 0x1000000u &&
              mbi.AllocationBase == (u64)(uintptr_t)qimg->base,
          "VirtualQuery(PE image base) reports MEM_COMMIT + MEM_IMAGE");

    CHECK(trt_VirtualFree(p, 0, 0x8000u /*MEM_RELEASE*/) == 1,
          "VirtualFree(MEM_RELEASE) -> TRUE");
    memset(&mbi, 0, sizeof(mbi));
    CHECK(trt_VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == 0x10000u,
          "VirtualQuery after VirtualFree reports MEM_FREE (0x10000)");
  }

  // ---- Lane B: GetFinalPathNameByHandleW, DuplicateHandle, WaitOnAddress,
  //             ntdll, bcrypt, advapi32 ANSI/Crypt, VEH/Compare, steam_api64 ----
  {
    auto w16 = [](u16* dst, const char* s) {
      while (*s) *dst++ = (u16)(u8)*s++;
      *dst = 0;
    };
    auto u16_ends_with = [](const u16* s, const char* suffix) -> bool {
      size_t slen = 0;
      while (s[slen]) ++slen;
      const size_t n = strlen(suffix);
      if (slen < n) return false;
      for (size_t i = 0; i < n; ++i) {
        u16 a = s[slen - n + i];
        u16 b = (u16)(u8)suffix[i];
        if (a >= 'A' && a <= 'Z') a = (u16)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (u16)(b + ('a' - 'A'));
        if (a != b) return false;
      }
      return true;
    };
    auto u16_starts_with = [](const u16* s, const char* prefix) -> bool {
      for (size_t i = 0; prefix[i]; ++i) {
        if (s[i] != (u16)(u8)prefix[i]) return false;
      }
      return true;
    };

    u16 wmod[128], wpath[256];
    w16(wmod, "kernel32.dll");
    void* hk32 = trt_LoadLibraryW(wmod);
    CHECK(hk32 != nullptr, "LoadLibraryW(kernel32.dll) -> synthetic handle");

    typedef u32 (TRT_STDCALL *fn_gfp_t)(void*, u16*, u32, u32);
    typedef int (TRT_STDCALL *fn_dup_t)(void*, void*, void*, void**, u32, int, u32);
    typedef int (TRT_STDCALL *fn_woa_t)(volatile void*, void*, u64, u32);
    typedef void (TRT_STDCALL *fn_wake_t)(void*);
    typedef int (TRT_STDCALL *fn_cmpord_t)(const u16*, int, const u16*, int, int);
    typedef void* (TRT_STDCALL *fn_addveh_t)(u32, void*);
    typedef u32 (TRT_STDCALL *fn_remveh_t)(void*);

    auto fn_gfp = (fn_gfp_t)trt_GetProcAddress(hk32, "GetFinalPathNameByHandleW");
    auto fn_dup = (fn_dup_t)trt_GetProcAddress(hk32, "DuplicateHandle");
    auto fn_woa = (fn_woa_t)trt_GetProcAddress(hk32, "WaitOnAddress");
    auto fn_wake = (fn_wake_t)trt_GetProcAddress(hk32, "WakeByAddressSingle");
    auto fn_cmpord = (fn_cmpord_t)trt_GetProcAddress(hk32, "CompareStringOrdinal");
    auto fn_addveh = (fn_addveh_t)trt_GetProcAddress(hk32, "AddVectoredExceptionHandler");
    auto fn_remveh = (fn_remveh_t)trt_GetProcAddress(hk32, "RemoveVectoredExceptionHandler");
    CHECK(fn_gfp && fn_dup && fn_woa && fn_wake && fn_cmpord && fn_addveh && fn_remveh,
          "GetProcAddress resolves new Lane B kernel32 exports");
    CHECK(trt_GetProcAddress(hk32, "DoesNotExist_LaneB_999") == nullptr &&
              trt_GetLastError() == 127u,
          "GetProcAddress missing export -> NULL + ERROR_PROC_NOT_FOUND(127)");

    // Create C:\gfp_test.txt ("hello-world", 11 bytes)
    w16(wpath, "C:\\gfp_test.txt");
    void* hw = trt_CreateFileW(wpath, 0x40000000u, 0, nullptr, 2u, 0x80u, nullptr);
    u32 nw = 0;
    CHECK(hw != (void*)(uintptr_t)-1 &&
              trt_WriteFile(hw, "hello-world", 11, &nw, nullptr) == 1 && nw == 11,
          "write C:\\gfp_test.txt succeeds");
    if (hw != (void*)(uintptr_t)-1) trt_CloseHandle(hw);

    void* hf = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0x80u, nullptr);
    u16 pout[256] = {};
    const u32 dos_len = fn_gfp(hf, pout, 256, 0u);
    CHECK(dos_len > 0 && u16_starts_with(pout, "\\\\?\\C:\\") &&
              u16_ends_with(pout, "\\gfp_test.txt"),
          "GetFinalPathNameByHandleW(C:\\) -> \\\\?\\C:\\...gfp_test.txt");

    u16 nt_out[256] = {};
    const u32 nt_len = fn_gfp(hf, nt_out, 256, 0x2u /*VOLUME_NAME_NT*/);
    CHECK(nt_len > 0 && u16_starts_with(nt_out, "\\Device\\HarddiskVolume1\\") &&
              u16_ends_with(nt_out, "\\gfp_test.txt"),
          "GetFinalPathNameByHandleW(VOLUME_NAME_NT) -> \\Device\\HarddiskVolume1\\...");

    u16 none_out[256] = {};
    const u32 none_len = fn_gfp(hf, none_out, 256, 0x4u /*VOLUME_NAME_NONE*/);
    CHECK(none_len > 0 && u16_starts_with(none_out, "\\gfp_test.txt"),
          "GetFinalPathNameByHandleW(VOLUME_NAME_NONE) -> \\gfp_test.txt");

    u16 tiny[4] = {};
    trt_SetLastError(0);
    const u32 need_len = fn_gfp(hf, tiny, 4, 0u);
    CHECK(need_len == dos_len + 1 && trt_GetLastError() == 122u,
          "GetFinalPathNameByHandleW small buf -> need+1 + ERROR_INSUFFICIENT_BUFFER(122)");

    u16 directory[64], directory_path[256]{};
    w16(directory, "C:\\Temp");
    const auto invalid = reinterpret_cast<void*>(static_cast<uintptr_t>(-1));
    CHECK(trt_CreateFileW(directory, 0, 7, nullptr, 3, 0x80, nullptr) == invalid &&
              trt_GetLastError() == 5,
          "CreateFileW rejects a directory open without backup semantics");
    void* dir = trt_CreateFileW(directory, 0, 7, nullptr, 3, 0x02000080u, nullptr);
    CHECK(dir != invalid && fn_gfp(dir, directory_path, 256, 0) > 0 &&
              u16_starts_with(directory_path, "\\\\?\\C:\\") &&
              u16_ends_with(directory_path, "\\Temp"),
          "GetFinalPathNameByHandleW returns the guest path for a directory");
    if (dir != invalid) {
      size_t written = 99;
      CHECK(!trt_file_pwrite(dir, 0, "x", 1, &written) && written == 0,
            "virtual directory handles reject file writes");
      trt_CloseHandle(dir);
      CHECK(fn_gfp(dir, directory_path, 256, 0) == 0 && trt_GetLastError() == 6,
            "closed directory handles no longer yield a path");
    }

    // DuplicateHandle refcounting keeps FILE* open across CloseHandle(orig)
    void* hf_dup = nullptr;
    CHECK(fn_dup((void*)(uintptr_t)-1, hf, (void*)(uintptr_t)-1, &hf_dup, 0, 0, 0) == 1 &&
              hf_dup != nullptr,
          "DuplicateHandle(0x777 file) -> TRUE");
    trt_CloseHandle(hf);
    memset(pout, 0, sizeof(pout));
    CHECK(fn_gfp(hf_dup, pout, 256, 0u) == dos_len,
          "DuplicateHandle refcount keeps handle valid after closing original");

    // Z:\ handle path check (reuses Z: mount on zdir2 from G2 test)
    u16 wz[64];
    w16(wz, "Z:\\ro2.txt");
    void* hzf = trt_CreateFileW(wz, 0x80000000u, 1u, nullptr, 3u, 0x80u, nullptr);
    u16 zout[256] = {};
    const u32 zlen = (hzf != (void*)(uintptr_t)-1) ? fn_gfp(hzf, zout, 256, 0u) : 0;
    CHECK(zlen > 0 && u16_starts_with(zout, "\\\\?\\Z:\\") &&
              u16_ends_with(zout, "ro2.txt"),
          "GetFinalPathNameByHandleW(Z:\\ro2.txt) -> \\\\?\\Z:\\ro2.txt");
    if (hzf != (void*)(uintptr_t)-1) trt_CloseHandle(hzf);

    // WaitOnAddress + WakeByAddressSingle + ERROR_TIMEOUT(1460)
    volatile u32 wval = 7u;
    u32 wcmp = 7u;
    trt_SetLastError(0);
    CHECK(fn_woa(&wval, &wcmp, 4, 25u) == 0 && trt_GetLastError() == 1460u,
          "WaitOnAddress timeout -> FALSE + ERROR_TIMEOUT(1460)");
    u32 wcmp_miss = 9u;
    CHECK(fn_woa(&wval, &wcmp_miss, 4, 100u) == 1,
          "WaitOnAddress value-mismatch fast path -> TRUE");

    struct wake_worker {
      volatile u32* addr;
      fn_wake_t wake;
      static u32 TRT_STDCALL run(void* arg) {
        wake_worker* w = (wake_worker*)arg;
        usleep(20000);
        __atomic_store_n(w->addr, 42u, __ATOMIC_SEQ_CST);
        w->wake((void*)w->addr);
        return 0;
      }
    };
    wake_worker ww{&wval, fn_wake};
    u64 wth = trt_CreateThread(nullptr, nullptr, (void*)&wake_worker::run, &ww, 0, nullptr);
    CHECK(wth != 0, "spawn WaitOnAddress waker thread");
    int wake_ok = 1;
    while (__atomic_load_n(&wval, __ATOMIC_SEQ_CST) == 7u) {
      if (!fn_woa(&wval, &wcmp, 4, 2000u)) { wake_ok = 0; break; }
    }
    CHECK(wake_ok == 1 && __atomic_load_n(&wval, __ATOMIC_SEQ_CST) == 42u,
          "WaitOnAddress woken by WakeByAddressSingle observes wval==42");
    if (wth) {
      trt_WaitForSingleObject(wth, 5000u);
      trt_CloseHandle((void*)(uintptr_t)wth);
    }

    // CompareStringOrdinal & VEH
    u16 cs1[16], cs2[16];
    w16(cs1, "AbC");
    w16(cs2, "abc");
    CHECK(fn_cmpord(cs1, -1, cs2, 3, 1) == 2 && fn_cmpord(cs1, -1, cs2, -1, 0) == 1,
          "CompareStringOrdinal case-insensitive EQUAL(2) and ordinal LESS(1)");
    void* veh = fn_addveh(1, (void*)&wake_worker::run);
    CHECK(veh != nullptr && fn_remveh(veh) == 1 && fn_remveh(veh) == 0,
          "AddVectoredExceptionHandler / RemoveVectoredExceptionHandler cookie");

    // ntdll: RtlGetVersion + NtQueryInformationFile
    w16(wmod, "ntdll.dll");
    void* hnt = trt_LoadLibraryW(wmod);
    CHECK(hnt != nullptr, "LoadLibraryW(ntdll.dll) -> synthetic handle");
    typedef u32 (TRT_STDCALL *fn_rtlgetver_t)(void*);
    typedef u32 (TRT_STDCALL *fn_ntqif_t)(void*, void*, void*, u32, u32);
    auto fn_rtlver = (fn_rtlgetver_t)trt_GetProcAddress(hnt, "RtlGetVersion");
    auto fn_ntqif = (fn_ntqif_t)trt_GetProcAddress(hnt, "NtQueryInformationFile");
    CHECK(fn_rtlver && fn_ntqif, "ntdll exports RtlGetVersion & NtQueryInformationFile");
    if (fn_rtlver) {
      u8 ovi[284] = {};
      *(u32*)ovi = 284u;
      CHECK(fn_rtlver(ovi) == 0u && ((u32*)ovi)[1] == 10u &&
                ((u32*)ovi)[3] >= 19041u && ovi[282] == 1u,
            "ntdll!RtlGetVersion -> Win10 >=19041 Workstation");
    }
    if (fn_ntqif) {
      u64 iosb[2] = {};
      struct { i64 alloc; i64 eof; u32 links; u8 del; u8 dir; u16 pad; } fsi{};
      CHECK(fn_ntqif(hf_dup, iosb, &fsi, sizeof(fsi), 5u /*FileStandardInformation*/) == 0u &&
                fsi.eof == 11 && fsi.links == 1 && iosb[0] == 0u,
            "ntdll!NtQueryInformationFile(FileStandardInformation) -> eof=11");
      u8 fnbuf[520] = {};
      memset(iosb, 0, sizeof(iosb));
      CHECK(fn_ntqif(hf_dup, iosb, fnbuf, sizeof(fnbuf), 9u /*FileNameInformation*/) == 0u &&
                *(u32*)fnbuf > 0u &&
                u16_ends_with((const u16*)(fnbuf + 4), "gfp_test.txt"),
            "ntdll!NtQueryInformationFile(FileNameInformation) -> \\gfp_test.txt");
    }
    trt_CloseHandle(hf_dup);

    // bcrypt: BCryptGenRandom + BCryptHash(SHA256)
    w16(wmod, "bcrypt.dll");
    void* hbc = trt_LoadLibraryW(wmod);
    CHECK(hbc != nullptr, "LoadLibraryW(bcrypt.dll) -> synthetic handle");
    typedef u32 (TRT_STDCALL *fn_bc_open_t)(void**, const u16*, const u16*, u32);
    typedef u32 (TRT_STDCALL *fn_bc_close_t)(void*, u32);
    typedef u32 (TRT_STDCALL *fn_bc_rand_t)(void*, u8*, u32, u32);
    typedef u32 (TRT_STDCALL *fn_bc_hash_t)(void*, u8*, u32, const u8*, u32, u8*, u32);
    auto fn_bc_open = (fn_bc_open_t)trt_GetProcAddress(hbc, "BCryptOpenAlgorithmProvider");
    auto fn_bc_close = (fn_bc_close_t)trt_GetProcAddress(hbc, "BCryptCloseAlgorithmProvider");
    auto fn_bc_rand = (fn_bc_rand_t)trt_GetProcAddress(hbc, "BCryptGenRandom");
    auto fn_bc_hash = (fn_bc_hash_t)trt_GetProcAddress(hbc, "BCryptHash");
    CHECK(fn_bc_open && fn_bc_close && fn_bc_rand && fn_bc_hash,
          "bcrypt exports open/close/GenRandom/Hash");
    static const u8 k_sha256_abc[32] = {
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    if (fn_bc_rand) {
      u8 rnd[32] = {};
      u32 nz = 0;
      const u32 st = fn_bc_rand(nullptr, rnd, sizeof(rnd), 0x2u /*USE_SYSTEM_PREFERRED_RNG*/);
      for (u32 i = 0; i < 32; ++i) nz |= rnd[i];
      CHECK(st == 0u && nz != 0u, "BCryptGenRandom -> STATUS_SUCCESS + non-zero entropy");
    }
    if (fn_bc_open && fn_bc_hash && fn_bc_close) {
      u16 walgo[16];
      w16(walgo, "SHA256");
      void* halg = nullptr;
      u8 dig[32] = {};
      const u32 o_st = fn_bc_open(&halg, walgo, nullptr, 0);
      const u32 h_st = (o_st == 0u) ? fn_bc_hash(halg, nullptr, 0, (const u8*)"abc", 3, dig, 32) : 1u;
      if (halg) fn_bc_close(halg, 0);
      CHECK(o_st == 0u && h_st == 0u && memcmp(dig, k_sha256_abc, 32) == 0,
            "BCryptHash(SHA256, \"abc\") matches NIST SHA-256 test vector");
    }

    // advapi32: ANSI Registry + CryptoAPI SHA-256 + SystemFunction036
    w16(wmod, "advapi32.dll");
    void* hadv = trt_LoadLibraryW(wmod);
    CHECK(hadv != nullptr, "LoadLibraryW(advapi32.dll) -> synthetic handle");
    typedef int (TRT_STDCALL *fn_regopena_t)(u64, const char*, u32, u32, void**);
    typedef int (TRT_STDCALL *fn_regcreatew_t)(u64, const u16*, void**);
    typedef int (TRT_STDCALL *fn_regsetexa_t)(u64, const char*, u32, u32, const u8*, u32);
    typedef int (TRT_STDCALL *fn_regqexa_t)(u64, const char*, u32*, u32*, u8*, u32*);
    typedef int (TRT_STDCALL *fn_regclose_t)(u64);
    typedef int (TRT_STDCALL *fn_cacq_t)(void**, const char*, const char*, u32, u32);
    typedef int (TRT_STDCALL *fn_crel_t)(void*, u32);
    typedef int (TRT_STDCALL *fn_chash_t)(void*, u32, void*, u32, void**);
    typedef int (TRT_STDCALL *fn_chdata_t)(void*, const u8*, u32, u32);
    typedef int (TRT_STDCALL *fn_chget_t)(void*, u32, u8*, u32*, u32);
    typedef int (TRT_STDCALL *fn_chdest_t)(void*);
    typedef int (TRT_STDCALL *fn_sys036_t)(void*, unsigned long);
    auto fn_ropena = (fn_regopena_t)trt_GetProcAddress(hadv, "RegOpenKeyExA");
    auto fn_rcreatew = (fn_regcreatew_t)trt_GetProcAddress(hadv, "RegCreateKeyW");
    auto fn_rseta = (fn_regsetexa_t)trt_GetProcAddress(hadv, "RegSetValueExA");
    auto fn_rqexa = (fn_regqexa_t)trt_GetProcAddress(hadv, "RegQueryValueExA");
    auto fn_rclose = (fn_regclose_t)trt_GetProcAddress(hadv, "RegCloseKey");
    auto fn_cacq = (fn_cacq_t)trt_GetProcAddress(hadv, "CryptAcquireContextA");
    auto fn_crel = (fn_crel_t)trt_GetProcAddress(hadv, "CryptReleaseContext");
    auto fn_chash = (fn_chash_t)trt_GetProcAddress(hadv, "CryptCreateHash");
    auto fn_chdata = (fn_chdata_t)trt_GetProcAddress(hadv, "CryptHashData");
    auto fn_chget = (fn_chget_t)trt_GetProcAddress(hadv, "CryptGetHashParam");
    auto fn_chdest = (fn_chdest_t)trt_GetProcAddress(hadv, "CryptDestroyHash");
    auto fn_sys036 = (fn_sys036_t)trt_GetProcAddress(hadv, "SystemFunction036");
    CHECK(fn_ropena && fn_rcreatew && fn_rseta && fn_rqexa && fn_rclose &&
              fn_cacq && fn_crel && fn_chash && fn_chdata && fn_chget &&
              fn_chdest && fn_sys036,
          "advapi32 exports ANSI Reg + CryptoAPI + SystemFunction036");
    if (fn_ropena && fn_rcreatew && fn_rseta && fn_rqexa && fn_rclose) {
      void* hmiss = nullptr;
      CHECK(fn_ropena(0x80000001ull, "Software\\TiramisuMissingZZ", 0, 0x20019u, &hmiss) != 0,
            "RegOpenKeyExA(missing) fails non-zero");
      u16 wsub[64];
      w16(wsub, "Software\\TiramisuLaneB");
      void* hk_w = nullptr;
      void* hk_a = nullptr;
      CHECK(fn_rcreatew(0x80000001ull, wsub, &hk_w) == 0 && hk_w != nullptr,
            "RegCreateKeyW creates Software\\TiramisuLaneB");
      if (hk_w) fn_rclose((u64)(uintptr_t)hk_w);
      CHECK(fn_ropena(0x80000001ull, "Software\\TiramisuLaneB", 0, 0xF003Fu, &hk_a) == 0 &&
                hk_a != nullptr,
            "RegOpenKeyExA opens Software\\TiramisuLaneB");
      if (hk_a) {
        const char* val = "ok123";
        CHECK(fn_rseta((u64)(uintptr_t)hk_a, "LaneBVal", 0, 1u /*REG_SZ*/,
                       (const u8*)val, 6u) == 0,
              "RegSetValueExA writes REG_SZ");
        u32 rtype = 0, rsz = 32;
        char rbuf[32] = {};
        CHECK(fn_rqexa((u64)(uintptr_t)hk_a, "LaneBVal", nullptr, &rtype,
                       (u8*)rbuf, &rsz) == 0 &&
                  rtype == 1u && strcmp(rbuf, "ok123") == 0,
              "RegQueryValueExA reads back ok123");
        fn_rclose((u64)(uintptr_t)hk_a);
      }
    }
    if (fn_cacq && fn_chash && fn_chdata && fn_chget && fn_chdest && fn_crel) {
      void* hprov = nullptr;
      void* hhash = nullptr;
      u8 cdig[32] = {};
      u32 csz = 32;
      int ok = fn_cacq(&hprov, nullptr, nullptr, 24u /*PROV_RSA_AES*/, 0xF0000000u) &&
               fn_chash(hprov, 0x800Cu /*CALG_SHA_256*/, nullptr, 0, &hhash) &&
               fn_chdata(hhash, (const u8*)"abc", 3, 0) &&
               fn_chget(hhash, 2u /*HP_HASHVAL*/, cdig, &csz, 0);
      if (hhash) fn_chdest(hhash);
      if (hprov) fn_crel(hprov, 0);
      CHECK(ok && csz == 32u && memcmp(cdig, k_sha256_abc, 32) == 0,
            "advapi32 CryptoAPI SHA-256(\"abc\") matches NIST vector");
    }
    if (fn_sys036) {
      u8 rbuf[32] = {};
      u32 nz = 0;
      const int s_ok = fn_sys036(rbuf, sizeof(rbuf));
      for (u32 i = 0; i < 32; ++i) nz |= rbuf[i];
      CHECK(s_ok == 1 && nz != 0u, "advapi32!SystemFunction036 fills entropy");
    }

    // steam_api64 forced-builtin route (even with full C:\ dir prefix) + honest BIsSubscribedApp==0
    w16(wmod, "C:\\fake_dir\\steam_api64.dll");
    void* hst = trt_LoadLibraryW(wmod);
    CHECK(hst != nullptr, "LoadLibraryW(C:\\fake_dir\\steam_api64.dll) routes to BUILTIN");
    typedef void* (TRT_STDCALL *fn_stctx_t)(void*);
    typedef void* (TRT_STDCALL *fn_stapps_t)(void);
    typedef u8 (TRT_STDCALL *fn_stsub_t)(void*, u32);
    auto fn_stctx = (fn_stctx_t)trt_GetProcAddress(hst, "SteamInternal_ContextInit");
    auto fn_stapps = (fn_stapps_t)trt_GetProcAddress(hst, "SteamAPI_SteamApps_v008");
    auto fn_stsub = (fn_stsub_t)trt_GetProcAddress(hst, "SteamAPI_ISteamApps_BIsSubscribedApp");
    CHECK(fn_stctx && fn_stapps && fn_stsub,
          "steam_api64 exports SteamInternal_ContextInit + SteamApps_v008 + BIsSubscribedApp");
    if (fn_stctx) {
      struct steam_cb {
        static void TRT_STDCALL init(void* ctx) {
          *(u64*)ctx = 0x57EA3001ull;
        }
      };
      struct {
        void (TRT_STDCALL *init_fn)(void*);
        uintptr_t counter;
        u64 storage;
      } blk{&steam_cb::init, 0, 0};
      void* c1 = fn_stctx(&blk);
      void* c2 = fn_stctx(&blk);
      CHECK(c1 == &blk.storage && c2 == &blk.storage &&
                blk.counter == 1 && blk.storage == 0x57EA3001ull,
            "SteamInternal_ContextInit initializes once and caches counter==1");
    }
    if (fn_stapps && fn_stsub) {
      void* apps = fn_stapps();
      CHECK(apps != nullptr && fn_stsub(apps, 480u) == 0u,
            "SteamAPI_ISteamApps_BIsSubscribedApp -> 0 (no DRM bypass)");
    }
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
