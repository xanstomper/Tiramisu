// tiramisu/tests/test_pe_loader.cpp
// Offline proof for the trt PE64 loader on a Linux host:
//   1. map the real guest PE (clang-built) at its preferred base (no relocs)
//   2. map it again anywhere (delta != 0 -> DIR64/HIGHLOW relocation pass)
//   3. bind its imports through the module/resolver path
//   4. seal (W^X) and EXECUTE the guest entry under the Win64 ABI
//   5. verify the guest's import call landed in the host and the checksum
//      matches the same arithmetic computed in C
//   6. negative: corrupt DOS header -> TRT_ERR_BAD_DOS, no crash
//   7. negative: managed (.NET) PE64 (COM descriptor set) -> TRT_ERR_MANAGED
//      with the CLR gap named in the log; the boot audit agrees
//   8. negative: 32-bit PE fixture (Celeste.exe's shape, but SizeOfImage=0,
//      so genuinely malformed) -> TRT_ERR_SIZE_OVERFLOW. M7 supersedes the
//      old PE32 rejection: PE32 now routes to trt_map_pe32 and maps
//      (managed PE32 load-only via the mscoree STUB); the fixture fails on
//      its malformed SizeOfImage, not on bitness.
// Console runs the identical loader through the UWP backend.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// ---------------- mmap backend ----------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;
bool g_ignore_preferred = false;  // reloc-path test flips this

void* linux_alloc(size_t bytes, u64 preferred_base) {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    defined(__clang__) && __has_feature(address_sanitizer))
  {  // ASan's shadow gap swallows the guests' usual preferred bases
     // (0x140000000 is INSIDE it): let the kernel pick; the loader relocates.
    const uintptr_t _b = (uintptr_t)(size_t)preferred_base;
    if (_b >= 0x00008fff7000ull && _b <= 0x02008fff6fffull) preferred_base = 0;
  }
#endif
  if (g_ignore_preferred) preferred_base = 0;
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
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
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}

void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
  (void)bytes;
}

void linux_log(const char* line) { printf("  [trt] %s\n", line); }

// capturing log: the negative cases assert the diagnostic text, not just
// the error code (a code without a message is how the mscoree failure hid)
char g_cap_log[8192];
void capture_log(const char* line) {
  const size_t n = strlen(g_cap_log);
  if (n < sizeof(g_cap_log) - 2)
    snprintf(g_cap_log + n, sizeof(g_cap_log) - n, "%s\n", line);
  printf("  [trt] %s\n", line);
}

// ---------------- import plumbing ----------------
// SysV implementations the guest's imports forward into
u32 g_build_value = 0x0042u;
u32 g_log_tag = 0, g_log_value = 0;
bool g_log_called = false;

extern "C" u32 TRT_STDCALL sysv_query_build() { return g_build_value; }
extern "C" void TRT_STDCALL sysv_log_u32(u32 tag, u32 value) {
  g_log_tag = tag;
  g_log_value = value;
  g_log_called = true;
}

// The test fakes carry TRT_STDCALL = ms_abi (Win64) so the guest binds their
// addresses directly — the console-exact path (host and guest share Win64,
// no trampoline needed).

void* test_resolve(void* user, const char* module, const char* symbol,
                   u16 ordinal) {
  (void)user; (void)ordinal;
  if (strcmp(module, "tiramisu_host.dll") == 0) {
    if (strcmp(symbol, "TiramisuHostQueryBuild") == 0) return (void*)&sysv_query_build;
    if (strcmp(symbol, "TiramisuHostLogU32") == 0) return (void*)&sysv_log_u32;
  }
  return nullptr;
}

// call a Win64 function: shadow space + rcx/rdx/r8 args, result in rax
u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 ret;
  asm volatile(
    "subq $48, %%rsp\n"       // keeps rsp 16-aligned; 40 bytes shadow+pad
    "movq %1, %%rcx\n"
    "movq %2, %%rdx\n"
    "movq %3, %%r8\n"
    "call *%4\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : "r"(a), "r"(b), "r"(c), "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) { printf("  PASS %s\n", msg); }                          \
    else { printf("  FAIL %s\n", msg); ++failures; }                   \
  } while (0)

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// same arithmetic as the guest, computed host-side; the +5 is the pointer
// table read (3+2) — only correct when the DIR64 relocation pass ran
u32 expected_checksum(u32 build, u64 a, u64 b) {
  u32 x = build ^ 0x1DEAu;
  x += (u32)(a & 0xF) + (u32)(b & 0xF);
  x += 5u;
  for (volatile int i = 0; i < 16; ++i) x = x * 16777619u + 1u;
  return x;
}

int run_case(const char* label, const u8* file, size_t size, bool ignore_preferred) {
  printf("case: %s\n", label);
  g_ignore_preferred = ignore_preferred;
  trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
  trt_resolver rs = {nullptr, &test_resolve};
  trt_image* img = nullptr;
  int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "map returns OK");
  if (rc != TRT_OK) return failures;
  CHECK(img->base != nullptr, "image base non-null");
  CHECK(img->entry_rva != 0, "entry rva non-zero");

  g_log_called = false; g_log_tag = 0; g_log_value = 0;
  CHECK(trt_seal_image(&be, img) == TRT_OK, "seal OK (W^X flip)");

  const u64 a = 0, b = 0;
  printf("  info exec at %llx (text+%llx)\n",
         (unsigned long long)(uintptr_t)(img->base + img->entry_rva),
         (unsigned long long)img->entry_rva);
  const u64 ret = win64_call(img->base + img->entry_rva, a, b, 0);
  CHECK(g_log_called, "guest import call reached the host");
  CHECK(g_log_tag == 0x1DEAu, "log tag == 0x1DEA");
  CHECK(g_log_value == g_build_value, "log value == build constant");
  CHECK((u32)ret == expected_checksum(g_build_value, a, b),
        "guest checksum matches host-side computation");
  char det[128];
  snprintf(det, sizeof(det), "entry returned 0x%llx", (unsigned long long)ret);
  printf("  info %s (base=%llx)\n", det, (unsigned long long)(uintptr_t)img->base);
  trt_unmap(&be, img);
  return failures;
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);  // survive SIGSEGV with logs intact
  if (argc < 3) {
    fprintf(stderr, "usage: %s <guest_hello.exe> <guest_hello_reloc.exe>\n", argv[0]);
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  printf("tiramisu offline loader test — guest %s (%zu bytes)\n", argv[1], size);

  size_t size2 = 0;
  u8* file2 = read_file(argv[2], &size2);
  if (!file2) {
    fprintf(stderr, "cannot read %s\n", argv[2]);
    return 2;
  }
  run_case("preferred base (delta == 0, no reloc pass)", file, size, false);
  // reloc guest: linked /BASE:0x1A00000 (internally consistent fixups);
  // backend ignores preferred -> delta != 0 -> DIR64 pass must run
  run_case("relocated base (delta != 0, full reloc pass)", file2, size2, true);

  // negative: corrupt MZ
  {
    trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
    trt_resolver rs = {nullptr, &test_resolve};
    trt_image* img = nullptr;
    file2[0] = 'X';
    const int rc = trt_map_pe(&be, &rs, file2, size2, &img);
    CHECK(rc == TRT_ERR_BAD_DOS, "corrupt MZ -> TRT_ERR_BAD_DOS");
    CHECK(img == nullptr, "no image leaked on error");
  }

  // negative: managed (.NET) PE64 — the guest bytes with only the COM
  // descriptor directory (14) patched in, exactly what a .NET assembly
  // carries. Must fail fast with the dedicated code, not at import bind.
  {
    u8* managed = (u8*)malloc(size);
    memcpy(managed, file, size);
    u32 pe_off = 0;
    memcpy(&pe_off, managed + 0x3c, 4);
    const size_t com_dir = (size_t)pe_off + 24 + 112 + 14 * 8;
    const u32 com_va = 0x1000u, com_sz = 72u;
    memcpy(managed + com_dir, &com_va, 4);
    memcpy(managed + com_dir + 4, &com_sz, 4);
    g_cap_log[0] = 0;
    trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &capture_log, nullptr};
    trt_resolver rs = {nullptr, &test_resolve};
    trt_image* img = nullptr;
    const int rc = trt_map_pe(&be, &rs, managed, size, &img);
    CHECK(rc == TRT_ERR_MANAGED, "managed PE64 (COM descriptor) -> TRT_ERR_MANAGED");
    CHECK(img == nullptr, "no image leaked on managed error");
    CHECK(strstr(g_cap_log, "managed .NET assembly") != nullptr,
          "managed failure names the CLR gap in the log");
    CHECK(strstr(trt_errstr(rc), "MANAGED") != nullptr,
          "trt_errstr names TRT_ERR_MANAGED");
    // the boot audit agrees without mapping anything
    std::vector<std::string> missing;
    CHECK(trt_audit_imports(managed, size, &missing) == TRT_ERR_MANAGED,
          "audit_imports on managed PE64 -> TRT_ERR_MANAGED");
    free(managed);
  }

  // negative: 32-bit PE — Celeste.exe's shape (PE32, COM descriptor, sole
  // import mscoree.dll!_CorExeMain). M7 supersedes the old PE32 rejection
  // by design: PE32 now routes to trt_map_pe32 (managed PE32 maps load-only
  // via the mscoree STUB), so this fixture — SizeOfImage=0, genuinely
  // malformed — fails on its size, not on its bitness.
  {
    u8 pe32[512];
    memset(pe32, 0, sizeof(pe32));
    pe32[0] = 'M'; pe32[1] = 'Z';
    const u32 pe_off = 0x80u;
    memcpy(pe32 + 0x3c, &pe_off, 4);
    memcpy(pe32 + 0x80, "PE\0\0", 4);
    pe32[0x84] = 0x4c; pe32[0x85] = 0x01;  // machine i386
    pe32[0x86] = 1;                          // number of sections
    pe32[0x94] = 224;                        // size of optional header
    pe32[0x98] = 0x0b; pe32[0x99] = 0x01;   // magic PE32
    const u32 n_rva = 16u;
    memcpy(pe32 + 0x98 + 92, &n_rva, 4);
    const u32 com_va = 0x2008u, com_sz = 72u;
    memcpy(pe32 + 0x98 + 96 + 14 * 8, &com_va, 4);
    memcpy(pe32 + 0x98 + 96 + 14 * 8 + 4, &com_sz, 4);
    g_cap_log[0] = 0;
    trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &capture_log, nullptr};
    trt_resolver rs = {nullptr, &test_resolve};
    trt_image* img = nullptr;
    const int rc = trt_map_pe(&be, &rs, pe32, sizeof(pe32), &img);
    CHECK(rc == TRT_ERR_SIZE_OVERFLOW, "PE32 managed -> TRT_ERR_SIZE_OVERFLOW (malformed fixture: SizeOfImage=0)");
    CHECK(img == nullptr, "no image leaked on PE32 error");
    CHECK(strstr(g_cap_log, "SIZE_OVERFLOW") != nullptr,
          "PE32 managed failure names the size problem in the log");
    // same fixture without the COM descriptor: same size failure
    const u32 zero = 0;
    memcpy(pe32 + 0x98 + 96 + 14 * 8, &zero, 4);
    g_cap_log[0] = 0;
    const int rc2 = trt_map_pe(&be, &rs, pe32, sizeof(pe32), &img);
    CHECK(rc2 == TRT_ERR_SIZE_OVERFLOW, "plain PE32 -> TRT_ERR_SIZE_OVERFLOW");
    CHECK(strstr(g_cap_log, "SizeOfImage/alignment insane") != nullptr,
          "plain PE32 failure keeps the size detail");
  }

  free(file2);
  free(file);
  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
