// tiramisu/tests/test_libload.cpp
// Proof that a game can load the DLLs it ships:
//
//   1. the exe statically imports guestdll.dll - the runtime must find that
//      DLL in the game's own directory, map it, bind ITS imports, seal it
//      and run its DllMain BEFORE binding the exe's import
//   2. the exe calls the DLL's export across the image boundary
//   3. the DLL calls back into the host (its own import) - so imports
//      inside a guest DLL bind too
//   4. LoadLibraryW + GetProcAddress work at runtime
#include "../src/trt/trt.h"
#include "../src/trt/images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

// ---------------- mmap backend ----------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[32];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    defined(__clang__) && __has_feature(address_sanitizer))
  {  // ASan's shadow gap swallows the guests' usual preferred bases
     // (0x140000000 is INSIDE it): let the kernel pick; the loader relocates.
    const uintptr_t _b = (uintptr_t)(size_t)preferred_base;
    if (_b >= 0x00008fff7000ull && _b <= 0x02008fff6fffull) preferred_base = 0;
  }
#endif
  // Windows semantics: a fixed address that is already taken FAILS, it does
  // not overwrite what is there. Without this the loader would map the
  // guest DLL over the exe (both prefer 0x1A00000) and corrupt it.
  if (preferred_base) {
    const uintptr_t lo = (uintptr_t)preferred_base;
    const uintptr_t hi = lo + bytes;
    for (int i = 0; i < g_extent_count; ++i) {
      const uintptr_t a = (uintptr_t)g_extents[i].base;
      const uintptr_t b = a + g_extents[i].len;
      if (lo < b && a < hi) return nullptr;  // occupied
    }
  }
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 32) {
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

char g_log[16384];
size_t g_log_len = 0;

void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  const size_t n = strlen(line);
  if (g_log_len + n + 2 < sizeof(g_log)) {
    memcpy(g_log + g_log_len, line, n);
    g_log_len += n;
    g_log[g_log_len++] = '\n';
    g_log[g_log_len] = 0;
  }
}

bool log_has(const char* needle) { return strstr(g_log, needle) != nullptr; }

// ---------------- the host, as the DLL sees it ----------------
// The DLL calls back into the host with the Win64 ABI, so the recorder is
// hand-written asm (a C function would use the host's SysV ABI).
// It records (rcx, rdx, return address) per call - the return address is
// the guest's call site, which tells us WHO called.
extern "C" u64 g_ring_n, g_ring_rcx[8], g_ring_rdx[8], g_ring_caller[8];
extern "C" u64 g_fake_calls;
extern "C" u64 fake_host_log_u32(void);
asm(
  ".text\n"
  ".globl fake_host_log_u32\n"
  "fake_host_log_u32:\n"
  "  incq g_fake_calls(%rip)\n"
  "  movq g_ring_n(%rip), %rax\n"
  "  cmpq $8, %rax\n"
  "  jae 1f\n"
  "  movq %rcx, g_ring_rcx(,%rax,8)\n"
  "  movq %rdx, g_ring_rdx(,%rax,8)\n"
  "  movq (%rsp), %rcx\n"          // return address = the guest call site
  "  movq %rcx, g_ring_caller(,%rax,8)\n"
  "  incq g_ring_n(%rip)\n"
  "1:\n"
  "  movq %rcx, %rax\n"            // return the tag like the real API would
  "  ret\n"
);
extern "C" u64 g_fake_calls = 0;
extern "C" u64 g_ring_n = 0;
extern "C" u64 g_ring_rcx[8] = {0};
extern "C" u64 g_ring_rdx[8] = {0};
extern "C" u64 g_ring_caller[8] = {0};

void* fake_system_load(const char* module, const char* symbol) {
  if (strcmp(module, "tiramisu_host.dll") == 0 &&
      strcmp(symbol, "TiramisuHostLogU32") == 0)
    return (void*)&fake_host_log_u32;
  return nullptr;
}

// ---- builtin bridges: the bridges are TRT_STDCALL = ms_abi (Win64) on the
// Linux test host, so the guest binds their addresses directly in the IAT
// (on the console both sides are Win64 and this is the exact same path) ----
extern "C" void* TRT_STDCALL trt_LoadLibraryW(const u16*);
extern "C" void* TRT_STDCALL trt_GetProcAddress(void*, const char*);
extern "C" void* TRT_STDCALL trt_GetProcessHeap(void);
extern "C" u32 TRT_STDCALL trt_GetModuleFileNameW(void*, u16*, u32);

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
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
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
    printf("usage: test_libload <guest.exe> <search_dir> [content_root]\n");
    return 2;
  }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};

  // the runtime knows where the game's own DLLs live
  trt_runtime_init(&be, argv[2]);
  if (argc > 3) trt_runtime_set_content_root(argv[3]);
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&fake_system_load);

  // the kernel32 builtins the guest calls need Win64->SysV trampolines in
  // this harness (on the console both sides are Win64 and none are needed)
  {
    static const trt_named_fn k_test_k32[] = {
        {"GetProcessHeap", (void*)&trt_GetProcessHeap},
        {"LoadLibraryW", (void*)&trt_LoadLibraryW},
        {"GetProcAddress", (void*)&trt_GetProcAddress},
    };
    trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_test_k32, 3);
  }

  printf("tiramisu guest DLL loading test\n");

  // the exe goes through the same path the console host uses
  trt_image* img = trt_load_pe(argv[1], "guest_libload.exe", 0);
  CHECK(img != nullptr, "exe mapped (its DLL dependency loaded on demand)");
  if (!img) {
    printf("\nRESULT: FAILURES\n");
    return 1;
  }

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx\n", (unsigned long long)ret);
  for (unsigned i = 0; i < g_ring_n && i < 8; ++i)
    printf("  host call %u: tag=0x%llx value=0x%llx from 0x%llx\n", i,
           (unsigned long long)g_ring_rcx[i], (unsigned long long)g_ring_rdx[i],
           (unsigned long long)g_ring_caller[i]);
  // bits 0xF (static import, LoadLibrary, GetProcAddress, indirect call);
  // the raw dll_add sum (42) rides in the high byte per guest_libload.c.
  CHECK(ret == 0x6B002A0Full, "static cross-image import + LoadLibrary + GetProcAddress");

  CHECK(log_has("loading guest DLL guestdll.dll"),
        "runtime loaded the DLL the game ships");
  CHECK(log_has("guestdll.dll entry returned"),
        "the DLL's entry point (DllMain) ran");
  // the DllMain's call into the host must carry the arguments the DLL set:
  // tag 0xD10, value 0x1111 - and come from the DLL's own code
  bool dll_main_called_host = false;
  trt_image* dll = trt_images_find("guestdll.dll");
  const u64 dll_lo = dll ? (u64)(uintptr_t)dll->base : 0;
  const u64 dll_hi = dll ? dll_lo + dll->size_of_image : 0;
  for (unsigned i = 0; i < g_ring_n && i < 8; ++i)
    if (g_ring_rcx[i] == 0xD10u && g_ring_rdx[i] == 0x1111u &&
        g_ring_caller[i] >= dll_lo && g_ring_caller[i] < dll_hi)
      dll_main_called_host = true;
  CHECK(dll_main_called_host, "DllMain reached the host with the right arguments");
  CHECK(g_fake_calls >= 3, "cross-image calls landed (DllMain + 2 exports)");

  if (argc > 4) {
    trt_vfs_mount("z:", argv[3]);
    trt_image* nested = trt_load_pe(argv[4], "nested.dll", 0);
    CHECK(nested != nullptr, "nested DLL mapped");
    if (nested) {
      u16 actual[128] = {};
      const char16_t expected[] = u"Z:\\sub\\nested.dll";
      const u32 length = trt_GetModuleFileNameW(trt_images_handle(nested), actual, 128);
      CHECK(length == sizeof(expected) / sizeof(expected[0]) - 1 &&
            memcmp(actual, expected, sizeof(expected)) == 0,
            "GetModuleFileNameW preserves the loaded DLL's subdirectory");
    }
    char path[128];
    trt_vfs_mount("x:", "/");
    trt_vfs_mount("y:", "/games/example/");
    CHECK(trt_vfs_host_to_guest("/games/example/sub/a.dll", path, sizeof(path)) == 0 &&
          strcmp(path, "Y:\\sub\\a.dll") == 0, "most specific mount wins");
    CHECK(trt_vfs_host_to_guest("/games/example2/a.dll", path, sizeof(path)) == 0 &&
          strcmp(path, "X:\\games\\example2\\a.dll") == 0,
          "a partial directory name does not shadow the root mount");
    CHECK(trt_vfs_host_to_guest("/games/example/sub/a.dll", path, 4) != 0,
          "short mounted-path buffer is rejected");
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
