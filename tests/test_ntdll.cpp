// tiramisu/tests/test_ntdll.cpp
// The ntdll native surface (work-order item 16 / spec 1K): a guest PE64
// opens and reads a host-written file through NtCreateFile/NtReadFile on the
// VFS, round-trips a write, exercises the Nt event surface (which must share
// kernel32's sync objects), writes PASS/FAIL lines to C:\results.txt, and
// finally calls RaiseFailFastException — honored via the boot exit trap
// (longjmp back here), the same fatal path ExitProcess uses.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_ntdll(void);
int trt_bridge_register_xinput(void);
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

// ---- fail-fast trap: RaiseFailFastException must longjmp back here, the
// same way the offline boot harness catches ExitProcess ----
jmp_buf g_ff_back;
volatile int g_ff_trapped = 0;
volatile int g_ff_code = -1;

void ff_trap(int code) {
  g_ff_trapped = 1;
  g_ff_code = code;
  longjmp(g_ff_back, 1);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("usage: test_ntdll <guest.exe>\n");
    return 2;
  }

  printf("tiramisu ntdll native surface test — %s\n", argv[1]);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_ntdll();

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-ntdll.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_ntdll.exe");

  // the probe file the guest must read through NtCreateFile/NtReadFile:
  // 32 bytes, byte i == 0x40 + i
  {
    std::string probe = std::string(drive_c) + "/ntprobe.txt";
    FILE* pf = fopen(probe.c_str(), "wb");
    if (!pf) { printf("cannot write probe file\n"); return 2; }
    for (u32 i = 0; i < 32; ++i) fputc((int)(0x40u + i), pf);
    fclose(pf);
  }

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (kernel32+ntdll bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  // RaiseFailFastException is called LAST by the guest: arm the trap, run,
  // and expect the longjmp — a normal return means fail-fast was ignored.
  // The guest's return value never reaches the host in that flow, so the
  // guest writes "BITS 0x????????\n" into results.txt before the fail-fast.
  trt_boot_exit_trap = &ff_trap;
  u64 ret = 0;
  if (setjmp(g_ff_back) == 0) {
    ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  }
  trt_boot_exit_trap = nullptr;

  CHECK(g_ff_trapped == 1,
        "RaiseFailFastException -> fail-fast honored (exit trap fired)");
  CHECK(g_ff_code == 0x09, "fail-fast exit code 0x09 (0xC0000409 low byte)");
  if (g_ff_trapped != 1) {
    char d[96];
    snprintf(d, sizeof(d), "guest RETURNED 0x%08llx (fail-fast ignored!)",
             (unsigned long long)ret);
    CHECK((ret & 0x80000000u) == 0, d);
  }

  // the guest wrote PASS/FAIL lines through the VFS — verify ON DISK
  std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  CHECK(rf != nullptr, "guest created C:\\results.txt through the VFS");
  if (rf) {
    fseek(rf, 0, SEEK_END);
    const long n = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    std::string text((size_t)n, 0);
    const size_t got = fread(&text[0], 1, (size_t)n, rf);
    fclose(rf);
    text.resize(got);

    // recover the guest's verdict bitmask ("BITS 0x????????\n")
    u32 bits = 0;
    const size_t bp = text.find("BITS 0x");
    if (bp != std::string::npos && bp + 7 + 8 <= text.size()) {
      for (int i = 0; i < 8; ++i) {
        const char c = text[bp + 7 + i];
        const u32 v = (c >= '0' && c <= '9') ? (u32)(c - '0')
                                            : (u32)(c - 'A' + 10);
        bits = (bits << 4) | (v & 0xFu);
      }
    }
    printf("  guest bits (from results.txt): 0x%08x (want 0x00001fff)\n",
           bits);
    CHECK(bits == 0x1FFFu, "guest bitmask exact-match (all ntdll checks)");

    CHECK(bits & 0x0001u, "NtCreateFile(\\??\\C:\\ntprobe.txt FILE_OPEN) -> STATUS_SUCCESS");
    CHECK(bits & 0x0002u, "NtReadFile -> exact host-written bytes, IOSB info=32");
    CHECK(bits & 0x0004u, "NtReadFile at EOF -> STATUS_END_OF_FILE");
    CHECK(bits & 0x0008u, "NtClose -> STATUS_SUCCESS");
    CHECK(bits & 0x0010u, "NtCreateFile FILE_CREATE existing -> STATUS_OBJECT_NAME_COLLISION");
    CHECK(bits & 0x0020u, "NtCreateFile FILE_OPEN missing -> STATUS_OBJECT_NAME_NOT_FOUND");
    CHECK(bits & 0x0040u, "NtCreateFile+NtWriteFile+NtReadFile round-trip");
    CHECK(bits & 0x0080u, "NtQueryInformationFile(FileStandardInformation) on Nt handle");
    CHECK(bits & 0x0100u, "NtCreateEvent (nameless) -> STATUS_SUCCESS");
    CHECK(bits & 0x0200u, "NtWaitForSingleObject 50ms unsignaled -> STATUS_TIMEOUT");
    CHECK(bits & 0x0400u, "NtSetEvent -> previous state 0, wait then succeeds");
    CHECK(bits & 0x0800u, "NtSignalAndWaitForSingleObject -> STATUS_SUCCESS");
    CHECK(bits & 0x1000u, "NtWaitForSingleObject bad handle -> STATUS_INVALID_HANDLE");

    static const char* k_lines[] = {
        "nt-create-open",   "nt-read-bytes",    "nt-read-eof",
        "nt-query-std-info", "nt-close",        "nt-create-collision",
        "nt-create-missing", "nt-write-roundtrip", "nt-create-event",
        "nt-wait-timeout",  "nt-set-event",     "nt-signal-and-wait",
        "nt-wait-bad-handle",
    };
    for (const char* name : k_lines) {
      std::string want = std::string("PASS ") + name + "\n";
      char d[128];
      snprintf(d, sizeof(d), "results.txt: PASS %s", name);
      CHECK(text.find(want) != std::string::npos, d);
    }
    CHECK(text.find("FAIL ") == std::string::npos,
          "results.txt: zero FAIL lines");
    CHECK(text.find("DONE\n") != std::string::npos,
          "results.txt: DONE written before the fail-fast");
  }

  trt_unmap(&be, img);
  free(file);

  // ---- Host-side module-table checks for NtQuery*, SetFileCompletion*, XInputGetStateEx
  trt_bridge_register_xinput();
  typedef u32(TRT_GUEST_ABI * nt_qsys_fn)(u32, void*, u32, u32*);
  typedef u32(TRT_GUEST_ABI * nt_qproc_fn)(void*, u32, void*, u32, u32*);
  typedef u32(TRT_GUEST_ABI * nt_qvm_fn)(void*, const void*, u32, void*, u64, u64*);
  typedef int(TRT_STDCALL * set_fcnm_fn)(void*, u8);
  typedef void*(TRT_STDCALL * valloc_fn)(void*, u64, u32, u32);
  typedef int(TRT_STDCALL * vfree_fn)(void*, u64, u32);

  auto* qsys = (nt_qsys_fn)trt_resolve_module(nullptr, "ntdll.dll", "NtQuerySystemInformation", 0);
  auto* qproc = (nt_qproc_fn)trt_resolve_module(nullptr, "ntdll.dll", "NtQueryInformationProcess", 0);
  auto* qvm = (nt_qvm_fn)trt_resolve_module(nullptr, "ntdll.dll", "NtQueryVirtualMemory", 0);
  auto* fcnm = (set_fcnm_fn)trt_resolve_module(nullptr, "kernel32.dll", "SetFileCompletionNotificationModes", 0);
  auto* va_fn = (valloc_fn)trt_resolve_module(nullptr, "kernel32.dll", "VirtualAlloc", 0);
  auto* vf_fn = (vfree_fn)trt_resolve_module(nullptr, "kernel32.dll", "VirtualFree", 0);
  CHECK(qsys && qproc && qvm, "NtQuerySystem/Process/VirtualMemory registered in ntdll.dll");
  CHECK(fcnm != nullptr, "SetFileCompletionNotificationModes registered in kernel32.dll");
  CHECK(trt_resolve_module(nullptr, "xinput1_4.dll", "XInputGetStateEx", 0) != nullptr &&
            trt_resolve_module(nullptr, "xinput1_3.dll", "#100", 0) != nullptr &&
            trt_resolve_module(nullptr, "xinput1_4.dll", "#101", 0) != nullptr,
        "XInputGetStateEx + #100/#101 registered in xinput");

  if (qsys) {
    u8 sbi[64]{};
    u32 rlen = 0;
    CHECK(qsys(0, sbi, sizeof(sbi), &rlen) == 0 && rlen == 64 && *(u32*)(sbi + 8) == 4096u,
          "NtQuerySystemInformation(SystemBasicInformation) -> page_size=4096");
    u32 need = 0;
    CHECK(qsys(73, nullptr, 0, &need) == 0xC0000004u && need > 0,
          "NtQuerySystemInformation(LogicalProcInfo, 0) -> STATUS_INFO_LENGTH_MISMATCH + need");
    u8 lpi[64]{};
    CHECK(qsys(73, lpi, need, &rlen) == 0 && *(u64*)lpi != 0,
          "NtQuerySystemInformation(LogicalProcInfo) -> non-zero mask");
    CHECK(qsys(9999, sbi, sizeof(sbi), &rlen) == 0xC0000002u,
          "NtQuerySystemInformation(unknown) -> STATUS_NOT_IMPLEMENTED");
  }
  if (qproc) {
    u64 dbg_port = 0xDEADBEEFULL;
    u32 rlen = 0;
    CHECK(qproc(nullptr, 7, &dbg_port, sizeof(dbg_port), &rlen) == 0 && dbg_port == 0,
          "NtQueryInformationProcess(DebugPort) -> 0");
    u64 dbg_obj = 1;
    CHECK(qproc(nullptr, 30, &dbg_obj, sizeof(dbg_obj), &rlen) == 0xC0000353u && dbg_obj == 0,
          "NtQueryInformationProcess(DebugObjectHandle) -> STATUS_PORT_NOT_SET");
    u32 dbg_flags = 0;
    CHECK(qproc(nullptr, 31, &dbg_flags, sizeof(dbg_flags), &rlen) == 0 && dbg_flags == 1,
          "NtQueryInformationProcess(DebugFlags) -> 1");
    u8 img_buf[128]{};
    CHECK(qproc(nullptr, 43, img_buf, sizeof(img_buf), &rlen) == 0 && rlen > 16,
          "NtQueryInformationProcess(ImageFileNameWin32) -> Z:\\game.exe");
  }
  if (qvm && va_fn && vf_fn) {
    void* p = va_fn(nullptr, 4096, 0x3000u, 0x04u);
    u8 mbi[48]{};
    u64 rlen = 0;
    CHECK(p != nullptr && qvm(nullptr, p, 0, mbi, sizeof(mbi), &rlen) == 0 && rlen == 48,
          "NtQueryVirtualMemory(MemoryBasicInformation) on VirtualAlloc page");
    if (p) vf_fn(p, 0, 0x8000u);
  }
  if (fcnm) {
    // Real Windows semantics (the modes are STORED per handle since the
    // ok-stub became real): a REAL file handle accepts valid mode flags,
    // an unknown handle and null fail with ERROR_INVALID_HANDLE, and an
    // invalid flag combination fails with ERROR_INVALID_PARAMETER.
    typedef void*(TRT_STDCALL * create_file_fn)(const u16*, u32, u32, void*,
                                                u32, u32, void*);
    typedef int(TRT_STDCALL * close_handle_fn)(void*);
    typedef u32(TRT_STDCALL * get_last_err_fn)(void);
    auto* cf = (create_file_fn)trt_resolve_module(nullptr, "kernel32.dll",
                                                  "CreateFileW", 0);
    auto* ch = (close_handle_fn)trt_resolve_module(nullptr, "kernel32.dll",
                                                   "CloseHandle", 0);
    auto* gle = (get_last_err_fn)trt_resolve_module(nullptr, "kernel32.dll",
                                                    "GetLastError", 0);
    CHECK(fcnm(nullptr, 1) == 0,
          "SetFileCompletionNotificationModes(null) fails");
    CHECK(fcnm((void*)(uintptr_t)0x1234, 1) == 0 &&
              (gle ? gle() : 6u) == 6u,
          "SetFileCompletionNotificationModes(unknown handle) -> ERROR_INVALID_HANDLE");
    if (cf && ch) {
      const u16 skip_path[] = {L'c', L':', L'\\', L's', L'k', L'i', L'p',
                               L'.',  L't', L'x',  L't', 0};
      void* sf = cf(skip_path, 0x40000000u /*GENERIC_WRITE*/, 0, nullptr,
                    2 /*CREATE_ALWAYS*/, 0x80u, nullptr);
      CHECK(sf != nullptr, "skip-mode probe file created");
      CHECK(sf && fcnm(sf, 1) == 1,
            "real handle accepts FILE_SKIP_COMPLETION_PORT_ON_SUCCESS");
      CHECK(sf && fcnm(sf, 0x4) == 0 && (gle ? gle() : 87u) == 87u,
            "invalid flag combination -> ERROR_INVALID_PARAMETER");
      CHECK(sf && fcnm(sf, 0) == 1, "modes reset accepted");
      ch(sf);
    }
  }

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
