// tiramisu/tests/test_winmm.cpp
// winmm: the frame-timing clock. The guest busy-spins and requires the
// clock to advance — a frozen or backwards timeGetTime deadlocks games.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_winmm(void);
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

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_winmm <guest.exe>\n");
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
  trt_bridge_register_winmm();

  printf("tiramisu winmm timing test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (winmm bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x1ff)\n", (unsigned long long)ret);

  CHECK(ret & 0x01u, "timeBeginPeriod(1) accepted");
  CHECK(ret & 0x02u, "clock nondecreasing");
  CHECK(ret & 0x04u, "clock advances during spin");
  CHECK(ret & 0x08u, "waveOutGetNumDevs -> 1 real output device");
  CHECK(ret & 0x10u, "timeGetDevCaps sane");
  CHECK(ret & 0x20u, "mciSendStringW reports an unavailable device");
  CHECK(ret & 0x40u, "mciSendStringA reports the same device error");
  CHECK(ret & 0x80u, "MCI error strings respect ANSI and UTF-16 buffer bounds");
  CHECK(ret & 0x100u, "unknown errors and zero-length buffers fail safely");
  CHECK(rs.resolve(rs.user, "winmmbase.dll", "mciGetErrorStringA", 0) != nullptr,
        "the alternate WinMM module exposes the recovered entry points");

  // Issue #6 regression: joyGetPos and the WinMM joystick API must resolve
  // cleanly for games like The Spike Cross on both winmm.dll and winmmbase.dll.
  {
    void* p_num = rs.resolve(rs.user, "winmm.dll", "joyGetNumDevs", 0);
    void* p_pos = rs.resolve(rs.user, "winmm.dll", "joyGetPos", 0);
    void* p_base_pos = rs.resolve(rs.user, "winmmbase.dll", "joyGetPos", 0);
    void* p_posex = rs.resolve(rs.user, "winmm.dll", "joyGetPosEx", 0);
    void* p_caps_a = rs.resolve(rs.user, "winmm.dll", "joyGetDevCapsA", 0);
    void* p_caps_w = rs.resolve(rs.user, "winmm.dll", "joyGetDevCapsW", 0);
    void* p_gth = rs.resolve(rs.user, "winmm.dll", "joyGetThreshold", 0);
    void* p_sth = rs.resolve(rs.user, "winmm.dll", "joySetThreshold", 0);
    void* p_scap = rs.resolve(rs.user, "winmm.dll", "joySetCapture", 0);
    void* p_rcap = rs.resolve(rs.user, "winmm.dll", "joyReleaseCapture", 0);
    void* p_cfg = rs.resolve(rs.user, "winmm.dll", "joyConfigChanged", 0);
    CHECK(p_num != nullptr, "winmm.dll!joyGetNumDevs resolves");
    CHECK(p_pos != nullptr, "winmm.dll!joyGetPos resolves");
    CHECK(p_base_pos == p_pos && p_base_pos != nullptr,
          "winmmbase.dll!joyGetPos resolves");
    CHECK(p_posex != nullptr, "winmm.dll!joyGetPosEx resolves");
    CHECK(p_caps_a != nullptr, "winmm.dll!joyGetDevCapsA resolves");
    CHECK(p_caps_w != nullptr, "winmm.dll!joyGetDevCapsW resolves");
    CHECK(p_gth != nullptr, "winmm.dll!joyGetThreshold resolves");
    CHECK(p_sth != nullptr, "winmm.dll!joySetThreshold resolves");
    CHECK(p_scap != nullptr, "winmm.dll!joySetCapture resolves");
    CHECK(p_rcap != nullptr, "winmm.dll!joyReleaseCapture resolves");
    CHECK(p_cfg != nullptr, "winmm.dll!joyConfigChanged resolves");

    u32 joyinfo[4] = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    const u64 pos_rc = p_pos ? win64_call(p_pos, 0, (u64)(uintptr_t)joyinfo, 0) : 0;
    CHECK(pos_rc == 167 && joyinfo[0] == 0 && joyinfo[1] == 0 && joyinfo[2] == 0 && joyinfo[3] == 0,
          "joyGetPos zeroes JOYINFO and returns JOYERR_UNPLUGGED (167)");
    const u64 pos_null = p_pos ? win64_call(p_pos, 0, 0, 0) : 0;
    CHECK(pos_null == 11, "joyGetPos(NULL) returns MMSYSERR_INVALPARAM (11)");

    u32 thresh = 999;
    const u64 gth_rc = p_gth ? win64_call(p_gth, 0, (u64)(uintptr_t)&thresh, 0) : 0;
    CHECK(gth_rc == 167 && thresh == 0, "joyGetThreshold returns JOYERR_UNPLUGGED and clears threshold");

    u8 caps[64];
    memset(caps, 0x5A, sizeof(caps));
    const u64 cap_rc = p_caps_w ? win64_call(p_caps_w, 0, (u64)(uintptr_t)caps, sizeof(caps)) : 0;
    CHECK(cap_rc == 165 && caps[0] == 0 && caps[63] == 0,
          "joyGetDevCapsW zeroes caps buffer and returns JOYERR_PARMS (165)");

    const u64 cfg_rc = p_cfg ? win64_call(p_cfg, 0, 0, 0) : 999;
    CHECK(cfg_rc == 0, "joyConfigChanged returns JOYERR_NOERROR (0)");
  }

  free(file);
  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", failures);
  return 1;
}
