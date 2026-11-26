// tiramisu/tests/test_ddraw.cpp
// DirectDraw: the guest locks the primary surface, writes a gradient,
// blits, flips — the host backend's present hook fires.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_ddraw.h"
#include "../src/trt/bridge_d3d9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_ddraw(void);
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

// ---- backend observation (the console GPU path) ----
int g_present_count = 0;

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_ddraw <guest.exe>\n");
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
  trt_bridge_register_ddraw();

  // observe the presentation path the host backend binds on console
  d3d9_hooks hk{};
  hk.present = [](void*) { ++g_present_count; };
  hk.user = nullptr;
  trt_d3d9_set_hooks(&hk);

  printf("tiramisu DirectDraw bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (ddraw bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xfff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "DirectDrawCreateEx -> IDirectDraw7");
  CHECK(ret & 0x002u, "SetCooperativeLevel accepted");
  CHECK(ret & 0x004u, "SetDisplayMode accepted");
  CHECK(ret & 0x008u, "CreateSurface (primary) -> surface");
  CHECK(ret & 0x010u, "Lock handed back pixels + sane pitch");
  CHECK(ret & 0x020u, "pixels written, Unlock accepted");
  CHECK(ret & 0x040u, "GetPixelFormat reports 32bpp RGB");
  CHECK(ret & 0x080u, "offscreen surface created");
  CHECK(ret & 0x100u, "BltFast offscreen -> primary");
  CHECK(ret & 0x200u, "Flip accepted");
  CHECK(ret & 0x400u, "GetCaps reports blit capability");
  CHECK(ret & 0x800u, "GetDisplayMode round-trips");

  // host-side: the frame presentation actually happened
  CHECK(g_present_count >= 2,
        "backend: present fired (Unlock + Flip on primary)");

  free(file);
  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", failures);
  return 1;
}
