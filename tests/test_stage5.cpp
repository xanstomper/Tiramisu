// tiramisu/tests/test_stage5.cpp
// STAGE 5 boot-determinism bridges: wininet / psapi / iphlpapi / imm32 /
// crypt32 / dbghelp+dbgcore, converted from UNCERTAIN SYSTEM(C) whitelist
// entries to BUILTIN honest-runtime. Each guest PE runs the import surface
// through the IAT and returns a bitmask this test checks:
//
//   guest_wininet.exe  — handle records real, send fails NAMED, read = EOF
//   guest_psapi.exe    — module list = the REAL guest image registry
//   guest_iphlpapi.exe — honest no-data + ICMP records
//   guest_imm32.exe    — no IME, NULL context, honest verbs
//   guest_crypt32.exe  — no cert store, honest open-fail / free-ok
//   guest_dbghelp.exe  — symbol record real, MiniDump = real text report
//
// Usage: test_stage5 <guest.exe> <expected_mask> [<game_dir>]
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/trt/images.h"
#include <ctime>

void trt_vfs_mount(const char* drive_prefix, const char* host_path);

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
#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (cond) printf("  PASS %s\n", msg);                                    \
    else { printf("  FAIL %s\n", msg); ++failures; }                         \
  } while (0)

// named bit meaning, from the guest file's header comment
const char* bit_name(const char* guest, u32 bit) {
  if (strstr(guest, "wininet")) {
    switch (bit) {
      case 0x001: return "InternetOpenA -> real handle record";
      case 0x002: return "InternetOpenW -> real handle record";
      case 0x004: return "InternetConnectA -> connect record";
      case 0x008: return "HttpOpenRequestA -> request record";
      case 0x010: return "HttpSendRequestA -> FALSE (cannot connect)";
      case 0x020: return "HttpQueryInfoA -> FALSE (no data)";
      case 0x040: return "InternetCrackUrlA -> FALSE (named)";
      case 0x080: return "InternetReadFile -> TRUE, 0 bytes (EOF)";
      case 0x100: return "InternetCloseHandle -> TRUE (freed)";
      case 0x200: return "InternetCloseHandle(bogus) -> FALSE (honest)";
    }
  } else if (strstr(guest, "psapi")) {
    switch (bit) {
      case 0x001: return "EnumProcesses -> 1 pid (the guest)";
      case 0x002: return "K32EnumProcessModules -> real image table";
      case 0x004: return "K32GetModuleFileNameExW -> image name";
      case 0x010: return "GetProcessMemoryInfo -> FALSE (honest)";
    }
  } else if (strstr(guest, "iphlpapi")) {
    switch (bit) {
      case 0x001: return "GetAdaptersInfo -> ERROR_NO_DATA";
      case 0x002: return "GetAdaptersAddresses -> ERROR_NO_DATA";
      case 0x004: return "GetIfTable -> 0 rows (honest empty)";
      case 0x008: return "IcmpCreateFile -> real handle record";
      case 0x010: return "IcmpSendEcho -> 0 (no raw sockets)";
      case 0x020: return "IcmpCloseHandle -> TRUE (freed)";
    }
  } else if (strstr(guest, "imm32")) {
    switch (bit) {
      case 0x001: return "ImmGetContext -> NULL (no IME)";
      case 0x002: return "ImmAssociateContext -> 0";
      case 0x004: return "ImmAssociateContextEx -> FALSE";
      case 0x008: return "ImmGetCompositionStringW -> -1";
      case 0x010: return "ImmReleaseContext -> TRUE (nothing held)";
      case 0x020: return "ImmNotifyIME -> FALSE";
    }
  } else if (strstr(guest, "crypt32")) {
    switch (bit) {
      case 0x001: return "CertOpenSystemStoreW -> NULL (no store)";
      case 0x002: return "CertOpenSystemStoreA -> NULL";
      case 0x004: return "CertOpenStore -> NULL";
      case 0x008: return "CertCloseStore -> TRUE (nothing open)";
      case 0x010: return "CertGetCertificateChain -> FALSE (no engine)";
      case 0x020: return "CertVerifyCertificateChainPolicy -> FALSE";
      case 0x040: return "CertFree* -> defensive no-crash";
    }
  } else if (strstr(guest, "dbghelp")) {
    switch (bit) {
      case 0x001: return "SymInitialize -> TRUE (record created)";
      case 0x002: return "SymFromAddr -> FALSE (no PDBs, named)";
      case 0x004: return "SymLoadModuleEx -> returns the base";
      case 0x008: return "MiniDumpWriteDump -> TRUE (real text report)";
    }
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: test_stage5 <guest.exe> <expected_mask> [<game_dir>]\n");
    return 2;
  }
  const char* path = argv[1];
  const char* guest = path;
  const char* slash = strrchr(path, '/');
  if (slash) guest = slash + 1;
  const u32 expected = (u32)strtoul(argv[2], nullptr, 0);
  const char* game_dir = argc > 3 ? argv[3] : ".";

  size_t size = 0;
  u8* file = read_file(path, &size);
  if (!file) { printf("cannot read %s\n", path); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  printf("tiramisu STAGE 5 boot-determinism bridge test: %s\n", guest);

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (BUILTIN bridge bound, no SYSTEM(C))");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");
  // the main exe image joins the registry (psapi's module list = the real
  // images the guest can see), then the runtime gets its game dir and C:
  // is mounted there (the crash-report dump lands at C:\crash-<time>.txt)
  trt_images_register_main(img, guest);
  trt_runtime_init(&be, game_dir);
  trt_vfs_mount("c:", game_dir);

  const u32 bits = (u32)win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%x (want 0x%x)\n", bits, expected);

  for (u32 bit = 1; bit; bit <<= 1) {
    if (expected & bit) {
      char msg[160];
      snprintf(msg, sizeof(msg), "%s", bit_name(guest, bit));
      CHECK((bits & bit) != 0, msg);
    }
  }
  // every bit the guest returned must have been requested (no surprises)
  CHECK((bits & ~expected) == 0, "no unexpected bits set");

  // dbghelp: verify the text crash report actually landed in the game dir
  if (strstr(guest, "dbghelp") && (bits & 0x008u)) {
    // the bridge named the dump in its log; the file must exist with the
    // exception address inside — scan the last few seconds' timestamps
    bool found = false;
    struct stat st;
    char probe[1024];
    time_t t = time(nullptr);
    for (time_t back = 0; back < 16 && !found; ++back) {
      snprintf(probe, sizeof(probe), "%s/crash-%lld.txt", game_dir,
               (long long)(t - back));
      if (stat(probe, &st) == 0 && st.st_size > 0) {
        found = true;
        printf("  PASS MiniDump artifact exists: %s (%lld bytes)\n", probe,
               (long long)st.st_size);
        // prove it is the real diagnostic: exception address + module list
        FILE* f = fopen(probe, "rb");
        if (f) {
          char line[256];
          bool have_addr = false, have_modules = false;
          while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "exception address")) have_addr = true;
            if (strstr(line, "modules:")) have_modules = true;
          }
          fclose(f);
          CHECK(have_addr, "crash report names the exception address");
          CHECK(have_modules, "crash report carries the module list");
        }
      }
    }
    CHECK(found, "MiniDumpWriteDump produced a real file in the game dir");
  }

  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
