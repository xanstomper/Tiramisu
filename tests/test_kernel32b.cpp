// tiramisu/tests/test_kernel32b.cpp
// Work-order item 12 remainder: job objects, console attach, the toolhelp
// MODULE32 walk over the REAL guest image registry, the mitigation-policy /
// stack-guarantee surface, and the nvapi/nvml/atiadl honest-stub modules.
// The guest PE (tests/guest_kernel32b.c) runs the whole sequence through the
// IAT + LoadLibraryW/GetProcAddress and writes its checksum to C:\results.txt
// through the VFS; this host verifies every bit and the on-disk bytes.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_nvapi(void);
int TRT_STDCALL trt_GetLocaleInfoEx(const u16* locale, u32 lctype, u16* out, int cch);
int TRT_STDCALL trt_ResolveLocaleName(const u16* name, u16* out, int cch);
int TRT_STDCALL trt_EnumSystemLocalesEx(void* cb, u32 flags, i64 lparam, void* reserved);
int TRT_STDCALL trt_IsValidLocaleName(const u16* name);
// ktmw32 bridge (discord_game_sdk.dll imports CreateTransaction/Commit)
void* TRT_STDCALL trt_CreateTransaction(void* attrs, const void* uow,
                                        u32 create_options, u32 isolation_level,
                                        u32 isolation_flags, u32 timeout,
                                        const u16* description);
int TRT_STDCALL trt_CommitTransaction(void* handle);
int TRT_STDCALL trt_RollbackTransaction(void* handle);
int TRT_STDCALL trt_GetTransactionId(void* handle, void* guid_out);
u32 TRT_STDCALL trt_GetLastError(void);
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

// call a Win64 guest: shadow space + rcx/rdx/r8, result in rax
// (arguments read through MEMORY constraints BEFORE %rsp moves — see
// test_game_boot.cpp for why)
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
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("usage: test_kernel32b <guest_kernel32b.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_nvapi();

  printf("tiramisu kernel32b (jobs/console/toolhelp/mitigation + GPU stubs) "
         "test\n");

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-k32b.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_kernel32b.exe");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (kernel32 gap surface bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");
  // the module list the toolhelp snapshot walks: entry 0 = the main exe
  trt_images_register_main(img, "guest_kernel32b.exe");

  const u64 ret = win64_call(img->base + img->entry_rva,
                             (u64)(uintptr_t)img->base, 1, 0);
  char det[96];
  snprintf(det, sizeof(det), "guest returned 0x%08llx",
           (unsigned long long)ret);
  CHECK(ret == 0x3FFFu, det);

  CHECK(ret & 0x0001u, "job: CreateJobObjectW + Assign(self) + SetInfo(EXTENDED, PROCESS_MEMORY) + Terminate + IsProcessInJob");
  CHECK(ret & 0x0002u, "job: OpenJobObjectW honest-fails NULL + ERROR_FILE_NOT_FOUND");
  CHECK(ret & 0x0004u, "console: AllocConsole -> TRUE");
  CHECK(ret & 0x0008u, "console: GetConsoleCP -> 437");
  CHECK(ret & 0x0010u, "console: FreeConsole -> TRUE");
  CHECK(ret & 0x0020u, "toolhelp: CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) -> valid handle");
  CHECK(ret & 0x0040u, "toolhelp: Module32FirstW -> exe module with nonzero base/size/name");
  CHECK(ret & 0x0080u, "toolhelp: Module32NextW terminates FALSE + ERROR_NO_MORE_FILES");
  CHECK(ret & 0x0100u, "toolhelp: CloseToolhelp32Snapshot -> TRUE");
  CHECK(ret & 0x0200u, "mitigation: GetProcessMitigationPolicy -> TRUE, all-zero buffer");
  CHECK(ret & 0x0400u, "nvapi: NvAPI_Initialize -> -160 + QueryInterface -> NULL");
  CHECK(ret & 0x0800u, "nvml: nvmlInit_v2 -> NVML_ERROR_NOT_SUPPORTED (9)");
  CHECK(ret & 0x1000u, "atiadl: ADL2_Main_Control_Create -> ADL_ERR_NOT_SUPPORTED (-9)");
  CHECK(ret & 0x2000u, "threading: Set/GetThreadStackGuarantee round-trip");

  // the guest wrote its checksum through the VFS — verify ON DISK
  std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  CHECK(rf != nullptr, "guest created C:\\results.txt through the VFS");
  if (rf) {
    u32 stored = 0;
    if (fread(&stored, 4, 1, rf) == 1) {}
    fclose(rf);
    char d2[96];
    snprintf(d2, sizeof(d2), "stored checksum == returned (0x%08x)", stored);
    CHECK(stored == (u32)ret, d2);
  }

  trt_unmap(&be, img);
  free(file);

  // verify locale resolution surface (Sekiro UCRT setlocale unblocker)
  {
    u16 out_buf[128] = {};
    const u16 q_jp[] = {'J','a','p','a','n','e','s','e',0};
    int r = trt_ResolveLocaleName(q_jp, out_buf, 128);
    CHECK(r == 6 && out_buf[0] == 'j' && out_buf[1] == 'a' && out_buf[2] == '-' &&
          out_buf[3] == 'J' && out_buf[4] == 'P' && out_buf[5] == 0,
          "locale: ResolveLocaleName(Japanese) -> ja-JP");

    const u16 ja_jp[] = {'j','a','-','J','P',0};
    r = trt_GetLocaleInfoEx(ja_jp, 0x1001 /*LOCALE_SENGLISHLANGUAGENAME*/, out_buf, 128);
    CHECK(r == 9 && out_buf[0] == 'J' && out_buf[1] == 'a' && out_buf[2] == 'p' &&
          out_buf[3] == 'a' && out_buf[4] == 'n' && out_buf[5] == 'e' &&
          out_buf[6] == 's' && out_buf[7] == 'e' && out_buf[8] == 0,
          "locale: GetLocaleInfoEx(ja-JP, LOCALE_SENGLISHLANGUAGENAME) -> Japanese");

    u32 cp = 0;
    r = trt_GetLocaleInfoEx(ja_jp, 0x1004 | 0x20000000u /*LOCALE_IDEFAULTANSICODEPAGE numeric*/, (u16*)&cp, 2);
    CHECK(r == 2 && cp == 932, "locale: GetLocaleInfoEx(ja-JP, ANSI_CP) -> 932");

    static bool s_saw_jp = false;
    s_saw_jp = false;
    auto enum_cb = [](const u16* loc, u32 flags, i64 lp) TRT_STDCALL -> int {
      (void)flags;
      (void)lp;
      if (loc && loc[0] == 'j' && loc[1] == 'a' && loc[2] == '-' && loc[3] == 'J' && loc[4] == 'P') {
        s_saw_jp = true;
      }
      return 1;
    };
    trt_EnumSystemLocalesEx((void*)(intptr_t)+enum_cb, 0, 0, nullptr);
    CHECK(s_saw_jp, "locale: EnumSystemLocalesEx yields ja-JP");
  }

  // verify wldap32 fallback resolving (Sekiro LDAP unblocker)
  {
    void* fn_ord27 = trt_resolve_module(nullptr, "wldap32.dll", nullptr, 27);
    CHECK(fn_ord27 != nullptr, "wldap32: resolve by ordinal #27 (ldap_next_entry)");
    void* fn_name = trt_resolve_module(nullptr, "wldap32.dll", "ldap_init", 0);
    CHECK(fn_name != nullptr, "wldap32: resolve by name ldap_init");
    void* fn_vol = trt_resolve_module(nullptr, "winmm.dll", "waveOutSetVolume", 0);
    CHECK(fn_vol != nullptr, "winmm: resolve waveOutSetVolume");
  }

  // ktmw32: discord_game_sdk.dll imports 2 transaction exports at LOAD time
  {
    CHECK(trt_resolve_module(nullptr, "ktmw32.dll", "CreateTransaction", 0) !=
                  nullptr &&
              trt_resolve_module(nullptr, "ktmw32.dll", "CommitTransaction", 0) !=
                  nullptr,
          "ktmw32: CreateTransaction/CommitTransaction resolve");
    void* tx = trt_CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr);
    CHECK(tx != nullptr && tx != (void*)(uintptr_t)~0ull,
          "ktmw32: CreateTransaction -> real handle");
    u8 guid[16] = {};
    CHECK(trt_GetTransactionId(tx, guid) == 1,
          "ktmw32: GetTransactionId fills a UOW");
    bool guid_nonzero = false;
    for (int i = 0; i < 16; ++i)
      if (guid[i]) guid_nonzero = true;
    CHECK(guid_nonzero, "ktmw32: transaction id is non-zero/stable");
    CHECK(trt_CommitTransaction(tx) == 1,
          "ktmw32: CommitTransaction -> TRUE (writes already durable)");
    CHECK(trt_CommitTransaction((void*)(uintptr_t)0xdeadbeef) == 0 &&
              trt_GetLastError() == 6,
          "ktmw32: commit on a garbage handle -> FALSE + ERROR_INVALID_HANDLE");
    CHECK(trt_RollbackTransaction(tx) == 0 && trt_GetLastError() == 120,
          "ktmw32: rollback -> FALSE + ERROR_CALL_NOT_IMPLEMENTED (named)");
  }

  if (failures) {
    printf("\nRESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("\nRESULT: ALL PASS\n");
  return 0;
}
