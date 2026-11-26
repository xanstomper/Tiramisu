// tiramisu/tests/test_real_load.cpp
// The load path a REAL PC game walks, proven offline:
//
//   1. api-set contract imports (api-ms-win-core-synch-l1-2-0,
//      api-ms-win-core-winrt-l1-1-0) redirect to the DLL that implements
//      them and the call REACHES the real function.
//   2. a module the console does not have (advapi32) still binds, and its
//      call site names itself instead of silently succeeding.
//   3. a DELAY-loaded DLL (d3dcompiler_47) does not kill the load when it
//      cannot be resolved; the call site names itself.
//
// The guest is a real PE64 built by the Windows-target clang, imported
// through the same module table the console host uses.
#include "../src/trt/trt.h"

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

// log capture: the assertions read what the runtime actually logged
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

bool log_has(const char* needle) {
  return strstr(g_log, needle) != nullptr;
}

// ---------------- the console's real DLLs (faked) ----------------
// On the console the host binds these from the OS. Here they are fakes
// with the Win64 ABI: the guest calls them through the IAT directly.
extern "C" u64 g_fake_calls;
extern "C" u64 fake_wait_on_address(void);
extern "C" u64 fake_winrt_create_string(void);
extern "C" u64 fake_delay_helper(void);
asm(
  ".text\n"
  ".globl fake_wait_on_address\n"
  "fake_wait_on_address:\n"
  "  incq g_fake_calls(%rip)\n"
  "  xorl %eax, %eax\n"          // WAIT_OK
  "  ret\n"
  ".globl fake_winrt_create_string\n"
  "fake_winrt_create_string:\n"
  "  incq g_fake_calls(%rip)\n"
  "  movq %rcx, %rax\n"          // echo the source pointer back
  "  ret\n"
  ".globl fake_delay_helper\n"
  "fake_delay_helper:\n"
  "  incq g_fake_calls(%rip)\n"
  "  xorl %eax, %eax\n"
  "  ret\n"
);
extern "C" u64 g_fake_calls = 0;

// the host's SYSTEM-route resolver: "the console has this DLL and symbol"
void* fake_system_load(const char* module, const char* symbol) {
  if (!module || !symbol) return nullptr;
  if (strcmp(module, "kernel32.dll") == 0 && strcmp(symbol, "WaitOnAddress") == 0)
    return (void*)&fake_wait_on_address;
  if (strcmp(module, "combase.dll") == 0 &&
      strcmp(symbol, "WindowsCreateString") == 0)
    return (void*)&fake_winrt_create_string;
  // the delay-load helper the linker emits: never called, because we bind
  // delay imports eagerly at load time
  if (strcmp(module, "delayimp.dll") == 0 && strcmp(symbol, "__delayLoadHelper2") == 0)
    return (void*)&fake_delay_helper;
  return nullptr;
}

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

// call a Win64 function: shadow space + rcx/rdx/r8 args, result in rax
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
  if (argc < 2) {
    printf("usage: test_real_load <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};

  // the runtime boots exactly like the console host does
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&fake_system_load);

  trt_resolver rs;
  trt_resolver_default(&rs);

  printf("tiramisu real-game load path test\n");
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (contracts + delay import resolve)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }

  const int seal_rc = trt_seal_image(&be, img);
  CHECK(seal_rc == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx\n", (unsigned long long)ret);
  CHECK(ret == 0x5A00000Full, "guest ran the full contract/stub/delay path");

  // evidence, not vibes:
  CHECK(log_has("apiset api-ms-win-core-synch-l1-2-0.dll -> kernel32.dll"),
        "api-set contract redirected (synch -> kernel32)");
  CHECK(log_has("apiset api-ms-win-core-winrt-l1-1-0.dll -> combase.dll"),
        "api-set contract redirected (winrt -> combase)");
  CHECK(log_has("DELAY import d3dcompiler_47.dll"),
        "delay import bound and logged");
  CHECK(log_has("advapi32.dll"),
        "advapi32 is a real module now (the registry bridge)");
  CHECK(log_has("d3dcompiler_47.dll!D3DCompile called (unimplemented)"),
        "delay-import call site names itself");
  // HISTORY: originally both synch and winrt contracts fell through to
  // fake_system_load (>= 2 fake calls). Then kernel32's WaitOnAddress moved
  // to the REAL bridge (g_fake_calls == 1). Now combase.dll is ALSO a real
  // BUILTIN bridge (bridge_winrt), so WindowsCreateString/WindowsDeleteString
  // run on the real HSTRING implementation and never fall through to the fake
  // loader (honest count == 0).
  CHECK(g_fake_calls == 0,
        "synch + winrt contracts bind REAL bridges (fake count == 0)");

  trt_unmap(&be, img);
  free(file);
  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
