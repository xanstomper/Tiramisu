// tiramisu/tests/test_ole.cpp
// The minimal COM boot + OLEAUT bridge: a guest runs the boot sequence a
// COM-heavy engine runs (CoInitializeEx, task memory, CoCreateInstance,
// BSTRs, variants, safe arrays) through the IAT.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_ole(void);

// sync primitive shared with the kernel32 bridge (bridge_thread.cpp) —
// used to create the events CoWaitForMultipleHandles waits on
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                 const u16* name);

// the STA wait under test (bridge_ole.cpp, declared in bridge_ole.h)
int TRT_GUEST_ABI ole_CoWaitForMultipleHandles(u32 flags, u32 timeout_ms,
                                               u32 count, const void* handles,
                                               u32* index_out);
}

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
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
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

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_ole <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_ole();

  printf("tiramisu OLE (COM boot + OLEAUT) bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (ole32+oleaut32 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x3ff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "CoInitializeEx -> S_OK");
  CHECK(ret & 0x002u, "CoInitializeEx nesting -> S_FALSE");
  CHECK(ret & 0x004u, "CoTaskMemAlloc/Free round-trip");
  CHECK(ret & 0x008u, "CoCreateInstance unregistered -> REGDB_E_CLASSNOTREG (fail-visible)");
  CHECK(ret & 0x010u, "SysAllocString/SysStringLen/SysFreeString round-trip");
  CHECK(ret & 0x020u, "VariantInit -> VT_EMPTY + VariantClear");
  CHECK(ret & 0x040u, "SafeArray bounds + element put/get round-trip");
  CHECK(ret & 0x080u, "SafeArrayDestroy");
  CHECK(ret & 0x100u, "OleInitialize (already-initialized S_FALSE)");
  CHECK(ret & 0x200u, "CoUninitialize nesting unwind (no crash)");

  // ---- CoWaitForMultipleHandles: the STA wait (work-order item 16) ----
  // Called directly on the host — the bridge is TRT_GUEST_ABI (ms_abi), the
  // same ABI the guest uses, so the host can exercise it without a guest PE.
  {
    u64 ev_sig = trt_CreateEventW(nullptr, true, true, nullptr);
    u64 ev_uns = trt_CreateEventW(nullptr, true, false, nullptr);
    u32 idx = 0xDEADBEEFu;

    const int hr_ok =
        ole_CoWaitForMultipleHandles(0, 1000, 1, &ev_sig, &idx);
    CHECK(hr_ok == 0 && idx == 0,
          "CoWaitForMultipleHandles signaled event -> S_OK index 0");

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    const int hr_pend =
        ole_CoWaitForMultipleHandles(0, 50, 1, &ev_uns, &idx);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    const long dt_ms = (ts1.tv_sec - ts0.tv_sec) * 1000 +
                       (ts1.tv_nsec - ts0.tv_nsec) / 1000000;
    CHECK(hr_pend == (int)0x80010127,
          "CoWaitForMultipleHandles 50ms unsignaled -> RPC_S_CALLPENDING");
    CHECK(dt_ms >= 45 && dt_ms < 2000,
          "CoWaitForMultipleHandles timeout honored (~50ms wall)");

    const u64 bad = 0;
    CHECK(ole_CoWaitForMultipleHandles(0, 10, 1, &bad, &idx) == (int)0x80070006,
          "CoWaitForMultipleHandles invalid handle -> E_HANDLE");
    CHECK(ole_CoWaitForMultipleHandles(0, 10, 0, &ev_sig, &idx) ==
              (int)0x80070057,
          "CoWaitForMultipleHandles zero handles -> E_INVALIDARG");

    // COWAIT_WAITALL on two signaled events -> S_OK
    u64 both[2] = {ev_sig, ev_sig};
    CHECK(ole_CoWaitForMultipleHandles(1, 100, 2, both, &idx) == 0,
          "CoWaitForMultipleHandles WAITALL on signaled pair -> S_OK");
  }

  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
