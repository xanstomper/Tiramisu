// tiramisu/tests/test_game_boot.cpp
// THE launch-path proof: a game-shaped PE (window class, WndProc, message
// pump, async keys, cursor, heap, QPC, env strings, module path, VFS file
// writes) is cross-built as a real PE64, mapped by the trt loader with the
// BUILTIN bridge resolver, executed on this host, and its on-disk results
// are verified. This is "games will actually launch" offline.
#include "../src/trt/trt.h"
#include "../src/trt/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

extern "C" {
int trt_RegisterRawInputDevices(const void*, u32, u32);
}

// ---- mmap backend (same as the loader test) ----
namespace {
struct extent { void* base; size_t len; };
extent g_ext[16];
int g_extn = 0;

void* l_alloc(size_t bytes, u64 pref) {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    defined(__clang__) && __has_feature(address_sanitizer))
  {  // ASan's shadow gap swallows the guests' usual preferred bases
     // (0x140000000 is INSIDE it): let the kernel pick; the loader relocates.
    const uintptr_t _b = (uintptr_t)(size_t)pref;
    if (_b >= 0x00008fff7000ull && _b <= 0x02008fff6fffull) pref = 0;
  }
#endif
  void* p = mmap(pref ? (void*)(uintptr_t)pref : nullptr, bytes,
                 PROT_READ | PROT_WRITE,
                 pref ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                      : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extn < 16) { g_ext[g_extn] = {p, bytes}; ++g_extn; }
  return p;
}
int l_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = 4096;
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}
void l_release(void* base, size_t) {
  for (int i = 0; i < g_extn; ++i)
    if (g_ext[i].base == base) { munmap(base, g_ext[i].len); g_ext[i].base = nullptr; return; }
}
void l_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// call a Win64 guest: shadow space + rcx/rdx/r8, result in rax
// NOTE: the arguments are read through MEMORY constraints and BEFORE %rsp
// is adjusted. With register constraints the compiler may pick a register
// that a later mov overwrites; and if %rsp moves first, the compiler's
// memory addresses become stale (the callee then gets garbage).
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
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

// ---- bridge entry points (STAGE 6) ----
// The bridges are TRT_STDCALL = ms_abi (Win64) on the Linux test host — the
// same ABI the guest uses — so the guest IAT can point at them DIRECTLY.
// The old hand-written Win64->SysV asm trampolines are gone: they translated
// arguments into SysV registers, corrupting every call after the ms_abi
// change (GetModuleFileNameW got buf/len in the wrong registers -> SIGSEGV).
extern "C" {
void* TRT_STDCALL trt_GetProcessHeap(void);
void* TRT_STDCALL trt_HeapAlloc(void*, u32, u64);
int TRT_STDCALL trt_QueryPerformanceCounter(i64*);
int TRT_STDCALL trt_CloseHandle(void*);
const u16* TRT_STDCALL trt_GetEnvironmentStringsW();
u32 TRT_STDCALL trt_GetModuleFileNameW(void*, u16*, u32);
u16 TRT_STDCALL trt_RegisterClassExW(const void*);
i16 TRT_STDCALL trt_GetAsyncKeyState(int);
int TRT_STDCALL trt_GetCursorPos(void*);
int TRT_STDCALL trt_TranslateMessage(const void*);
i64 TRT_STDCALL trt_DispatchMessageW(const void*);
i64 TRT_STDCALL trt_SendMessageW(void*, u32, u64, i64);
int TRT_STDCALL trt_WriteFile(void*, const void*, u32, u32*, void*);
int TRT_STDCALL trt_PeekMessageW(void*, void*, u32, u32, u32);
void* TRT_STDCALL trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
void* TRT_STDCALL trt_CreateWindowExW(u32, const wchar_t*, const wchar_t*,
                                      u32, int, int, int, int, void*,
                                      void*, void*, void*);
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) { fprintf(stderr, "usage: %s <guest_game.exe>\n", argv[0]); return 2; }

  printf("tiramisu GAME-SHAPED boot test — %s\n", argv[1]);

  // runtime boot: builtin bridges + Win64 trampolines as the resolver
  // (console needs none of this: one ABI everywhere — see trt.h note)
  trt_backend be{&l_alloc, &l_protect, &l_release, &l_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_resolver def;
  trt_resolver_default(&def);
  struct sym { const char* n; void* f; };
  static const sym k_thunks[] = {
      {"GetProcessHeap", (void*)&trt_GetProcessHeap},
      {"HeapAlloc", (void*)&trt_HeapAlloc},
      {"QueryPerformanceCounter", (void*)&trt_QueryPerformanceCounter},
      {"CloseHandle", (void*)&trt_CloseHandle},
      {"GetEnvironmentStringsW", (void*)&trt_GetEnvironmentStringsW},
      {"GetModuleFileNameW", (void*)&trt_GetModuleFileNameW},
      {"RegisterClassExW", (void*)&trt_RegisterClassExW},
      {"GetAsyncKeyState", (void*)&trt_GetAsyncKeyState},
      {"GetCursorPos", (void*)&trt_GetCursorPos},
      {"TranslateMessage", (void*)&trt_TranslateMessage},
      {"DispatchMessageW", (void*)&trt_DispatchMessageW},
      {"SendMessageW", (void*)&trt_SendMessageW},
      {"WriteFile", (void*)&trt_WriteFile},
      {"PeekMessageW", (void*)&trt_PeekMessageW},
      {"CreateFileW", (void*)&trt_CreateFileW},
      {"CreateWindowExW", (void*)&trt_CreateWindowExW},
  };
  static trt_resolver rs;
  rs.user = &def;
  rs.resolve = [](void* user, const char* module, const char* symbol,
                  u16 ordinal) -> void* {
    const bool bridge = strcmp(module, "kernel32.dll") == 0 ||
                        strcmp(module, "user32.dll") == 0;
    if (bridge && symbol) {
      for (const sym& s : k_thunks)
        if (strcmp(s.n, symbol) == 0) return s.f;
    }
    const trt_resolver* d = (const trt_resolver*)user;
    return d->resolve(d->user, module, symbol, ordinal);
  };

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-drivec.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_game.exe");

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { fprintf(stderr, "cannot read guest\n"); return 2; }
  printf("  guest: %zu bytes, %d sections worth of real game-shaped code\n",
         size, 4);

  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "map game-shaped PE");
  if (rc != TRT_OK) return 1;
  CHECK(trt_seal_image(&be, img) == TRT_OK, "seal (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, (u64)(uintptr_t)img->base, 1, 0);
  char det[96];
  snprintf(det, sizeof(det), "guest returned 0x%08llx",
           (unsigned long long)ret);
  CHECK((ret & 0x40000000u) == 0x40000000u, det);
  // exact: every term is a distinct bit (pumped=2 => +0x20)
  CHECK(ret == 0x43F19120u, "checksum exact-match (all bridges verified)");
  CHECK(ret & 0x00000100, "window created");
  CHECK(ret & 0x00001000, "WndProc invoked via DispatchMessage");
  CHECK(ret & 0x02000000, "WndProc Win64 ABI 4-arg + 64-bit return");
  CHECK(ret & 0x00010000, "GetAsyncKeyState ran (no key = clean)");
  CHECK(ret & 0x000100000, "HeapAlloc through bridge");
  CHECK(ret & 0x00200000, "QueryPerformanceCounter through bridge");
  CHECK(ret & 0x00400000, "environment strings (COMPUTERNAME=TIRAMISU)");
  CHECK(ret & 0x00800000, "module path on Z:\\");

  // the guest wrote its results through the VFS — verify ON DISK
  std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  CHECK(rf != nullptr, "guest created C:\\results.txt through the VFS");
  if (rf) {
    u32 stored = 0, calls = 0;
    if (fread(&stored, 4, 1, rf) == 1) {}
    if (fread(&calls, 4, 1, rf) == 1) {}
    fclose(rf);
    char d2[96];
    snprintf(d2, sizeof(d2), "stored checksum == returned (0x%08x)", stored);
    CHECK(stored == (u32)ret, d2);
    CHECK(calls >= 2, "WndProc ran >= 2x (WM_CREATE + WM_SHOWWINDOW)");
  }

  trt_unmap(&be, img);
  free(file);

  static int pump_calls = 0;
  pump_calls = 0;
  trt_set_pump([]() { ++pump_calls; });
  u8 dummy_msg[48] = {};
  trt_PeekMessageW(dummy_msg, nullptr, 0, 0, 1);
  trt_set_pump(nullptr);
  CHECK(pump_calls == 1, "PeekMessageW invokes host pump when queue empties");

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
