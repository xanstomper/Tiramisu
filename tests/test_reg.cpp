// tiramisu/tests/test_reg.cpp
// The registry bridge, proven offline.
//
// A guest (compiled like a PC game) opens/creates HKCU and HKLM keys,
// writes a string, reads it back, asks for a value that is not there (must
// fail VISIBLY) and enumerates subkeys. The test then checks the hive on
// disk: the value must have been persisted, and a second run must see it.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_reg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
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
  if (argc < 3) {
    printf("usage: test_reg <guest.exe> <drive_c_dir>\n");
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

  // the guest's C: drive (the hive lives there, like a game's saves)
  trt_vfs_mount("c:", argv[2]);
  const u16 hive_guest_path[] = {'C', ':', '\\', 'r', 'e', 'g', 'i', 's',
                                 't', 'r', 'y', '.', 'h', 'i', 'v', 0};
  trt_registry_set_hive_path(hive_guest_path);

  printf("tiramisu registry bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (advapi32 imports bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (bits want 0x3fff)\n", (unsigned long long)ret);
  // the high bytes carry diagnostics (the value type/size/first byte)
  CHECK((ret & 0x7Full) == 0x7Full, "guest did the whole registry dance");
  CHECK((ret & 0x80ull) != 0, "OpenProcessToken -> real pseudo token handle");
  CHECK((ret & 0x100ull) != 0, "GetTokenInformation(TokenUser) -> SID record");
  CHECK((ret & 0x200ull) != 0,
        "ConvertSidToStringSidW -> \"S-1-5-88-1-1001\"");
  CHECK((ret & 0x400ull) != 0,
        "GetTokenInformation(TokenIntegrityLevel) -> medium-integrity SID");
  CHECK((ret & 0x800ull) != 0,
        "GetTokenInformation(unsupported class) fails named");
  CHECK((ret & 0x1000ull) != 0,
        "SDDL conversion -> real self-relative SD (NULL DACL, logged "
        "no-security-model)");
  CHECK((ret & 0x2000ull) != 0, "garbage token handle fails (INVALID_HANDLE)");

  // the hive must exist on disk and carry the value
  char hive_path[512];
  snprintf(hive_path, sizeof(hive_path), "%s/registry.hiv", argv[2]);
  size_t hsize = 0;
  u8* hive = read_file(hive_path, &hsize);
  CHECK(hive != nullptr && hsize > 0, "hive written to the guest's C: drive");
  if (hive) {
    // the hive stores strings hex-encoded (UTF-16 survives the round trip)
    const char* needle = "73006800610064006f007700";
    bool found = false;
    for (size_t i = 0; i + strlen(needle) <= hsize; ++i)
      if (memcmp(hive + i, needle, strlen(needle)) == 0) { found = true; break; }
    CHECK(found, "the written value is persisted in the hive");
    free(hive);
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
