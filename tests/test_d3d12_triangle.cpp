// tiramisu/tests/test_d3d12_triangle.cpp
// THE D3D12 TRIANGLE GATE (Sprint-2 item 21) — the acceptance fixture that
// would have caught BUG-1. A synthetic freestanding game (guest_triangle.c,
// cross-built as a real PE64) walks the FULL D3D12 render path through the
// loader + bridges on this host:
//
//   device -> queue -> allocator -> list -> rootsig -> pso -> vertex buffer
//   -> state/draw -> Close -> ExecuteCommandLists (slot 8!) -> fence signal
//   -> swapchain -> Present.
//
// The host prints the 12 ladder markers (same pattern as the EXE ladder),
// asserts the guest's checksum bit-for-bit, asserts the named bridge log
// lines for the steps the guest cannot observe itself (DRAW RECORDED, ECL
// EXECUTED), and — the strongest link — reads the queue pointer the GUEST
// created out of C:\results.txt and asserts host-side that the submission
// RECORDED on that very object (trt_d3d12_queue_recorded_count).
//
// Every guest-ABI function pointer is called through typedefs carrying
// TRT_GUEST_ABI (ms_abi on x86_64 Linux) — SysV callers would corrupt the
// callee's argument registers otherwise.
#include "../src/trt/bridge_d3d12.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" {
int trt_bridge_register_d3d12(void);
int trt_bridge_register_d3d(void);
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
// bridge entry points the guest's IAT binds to (STAGE 6: ms_abi on both
// sides — the guest IAT points at them DIRECTLY)
int TRT_GUEST_ABI trt_D3D12CreateDevice(void*, u32, const void*, void**);
int TRT_GUEST_ABI trt_CreateDXGIFactory1(const void*, void**);
void* TRT_STDCALL trt_CreateFileW(const u16*, u32, u32, void*, u32, u32,
                                  void*);
int TRT_STDCALL trt_WriteFile(void*, const void*, u32, u32*, void*);
int TRT_STDCALL trt_CloseHandle(void*);
void* TRT_STDCALL trt_CreateEventW(void*, int, int, const u16*);
u32 TRT_STDCALL trt_WaitForSingleObject(void*, u32);
}

// ---- mmap backend + log capture (same shape as test_game_boot) ------------
namespace {
struct extent { void* base; size_t len; };
extent g_ext[16];
int g_extn = 0;
std::vector<std::string> g_log;

void* l_alloc(size_t bytes, u64 pref) {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    defined(__clang__) && __has_feature(address_sanitizer))
  {  // ASan's shadow gap swallows the guests' usual preferred bases
     // (0x140000000 is INSIDE it): let the kernel pick; the loader relocates.
    const uintptr_t _b = (uintptr_t)(size_t)pref;
    if (_b >= 0x00008fff7000ull && _b <= 0x02008fff6fffull) pref = 0;
  }
#endif
  void* p = mmap(pref ? (void*)(uintptr_t)pref : nullptr, bytes,
                 PROT_READ | PROT_WRITE,
                 pref ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                      : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extn < 16) { g_ext[g_extn] = {p, bytes}; ++g_extn; }
  return p;
}
int l_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = 4096;
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}
void l_release(void* base, size_t) {
  for (int i = 0; i < g_extn; ++i)
    if (g_ext[i].base == base) {
      munmap(base, g_ext[i].len);
      g_ext[i].base = nullptr;
      return;
    }
}
void l_log(const char* line) {
  g_log.emplace_back(line);
  printf("  [trt] %s\n", line);
}

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return nullptr;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// call a Win64 guest entry: shadow space + rcx/rdx/r8, result in rax
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
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
        "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)
// ladder marker: prints the marker on success, names the failure otherwise
#define MARKER(cond, marker_name, fail_msg)                          \
  do {                                                               \
    if (cond) printf("  %s\n", marker_name);                         \
    else { printf("  FAIL %s\n", fail_msg); ++failures; }            \
  } while (0)

bool log_has(const char* needle) {
  for (const std::string& s : g_log)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

static u32 g_host_submit_calls = 0;
static u32 g_host_submit_draws = 0;
static int TRT_GUEST_ABI test_host_submit(const trt_d3d12_submit_desc* sd) {
  ++g_host_submit_calls;
  if (sd) g_host_submit_draws += sd->count;
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: %s <guest_triangle.exe>\n", argv[0]);
    return 2;
  }

  printf("tiramisu D3D12 TRIANGLE GATE — %s\n", argv[1]);

  // runtime boot: builtin bridges; the guest imports d3d12/dxgi/kernel32
  trt_backend be{&l_alloc, &l_protect, &l_release, &l_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_d3d();    // dxgi.dll factory/swapchain shim
  if (trt_bridge_register_d3d12() != 0) {
    printf("  FAIL d3d12 registration (vtbl selfcheck refused)\n");
    return 1;
  }
  {
    trt_d3d12_hooks h{};
    h.submit = &test_host_submit;
    trt_d3d12_set_hooks(&h);
  }
  trt_resolver def;
  trt_resolver_default(&def);
  struct sym { const char* n; void* f; };
  static const sym k_thunks[] = {
      {"D3D12CreateDevice", (void*)&trt_D3D12CreateDevice},
      {"CreateDXGIFactory1", (void*)&trt_CreateDXGIFactory1},
      {"CreateFileW", (void*)&trt_CreateFileW},
      {"WriteFile", (void*)&trt_WriteFile},
      {"CloseHandle", (void*)&trt_CloseHandle},
      {"CreateEventW", (void*)&trt_CreateEventW},
      {"WaitForSingleObject", (void*)&trt_WaitForSingleObject},
  };
  static trt_resolver rs;
  rs.user = &def;
  rs.resolve = [](void* user, const char* module, const char* symbol,
                  u16 ordinal) -> void* {
    if (symbol) {
      for (const sym& s : k_thunks)
        if (strcmp(s.n, symbol) == 0) return s.f;
    }
    (void)module; (void)ordinal;
    const trt_resolver* d = (const trt_resolver*)user;
    return d->resolve(d->user, module, symbol, ordinal);
  };

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-tri.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_triangle.exe");

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) {
    fprintf(stderr, "cannot read guest\n");
    return 2;
  }

  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  if (rc != TRT_OK) {
    printf("  FAIL map guest_triangle.exe (rc=%d)\n", rc);
    return 1;
  }
  trt_seal_image(&be, img);

  const u64 ret = win64_call(img->base + img->entry_rva,
                             (u64)(uintptr_t)img->base, 1, 0);
  char det[96];
  snprintf(det, sizeof(det), "guest returned 0x%08llx",
           (unsigned long long)ret);

  // every ladder step is a distinct bit — exact match proves no step faked
  const u32 k_expected = 0x40000000u | 0x1FFFu;
  MARKER((ret & 0xFFFFFFFFu) == k_expected, "", det);

  // ---- the 12 ladder markers ------------------------------------------------
  MARKER(ret & 0x01u, "D3D12 DEVICE OK", "D3D12CreateDevice failed in guest");
  MARKER(ret & 0x02u, "QUEUE OK", "CreateCommandQueue failed in guest");
  MARKER(ret & 0x04u, "ALLOC OK", "CreateCommandAllocator failed in guest");
  MARKER(ret & 0x08u, "LIST OK", "CreateCommandList failed in guest");
  MARKER(ret & 0x10u, "ROOTSIG OK", "CreateRootSignature failed in guest");
  MARKER(ret & 0x20u, "PSO OK",
         "CreateGraphicsPipelineState failed in guest");
  MARKER(ret & 0x40u, "RESOURCE OK", "CreateCommittedResource failed in guest");

  // DRAW RECORDED: the guest cannot observe the bridge's draw counter, so
  // the bridge NAMES it in the log (fail-visible) and the host asserts it.
  MARKER((ret & 0x80u) && log_has("DrawInstanced(3,1,0,0) recorded"),
         "DRAW RECORDED",
         "DrawInstanced(3,1,0,0) not recorded in the command list");

  MARKER(ret & 0x100u, "LIST CLOSED", "list->Close failed in guest");

  // ECL EXECUTED: log line + the host-side cross-check on the very queue
  // object the guest created (pointer read back from C:\results.txt).
  u64 queue_ptr = 0;
  u32 stored = 0;
  const std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  if (rf) {
    if (fread(&stored, 4, 1, rf) == 1) {}
    if (fread(&queue_ptr, 8, 1, rf) == 1) {}
    fclose(rf);
  }
  MARKER(stored == (u32)ret && queue_ptr != 0,
         "", "guest results file missing/incomplete (C:\\results.txt)");
  const int recorded = queue_ptr ? trt_d3d12_queue_recorded_count(
                                      (void*)(uintptr_t)queue_ptr)
                                 : -1;
  MARKER((ret & 0x200u) && log_has("ExecuteCommandLists(1) recorded") &&
             recorded == 1,
         "ECL EXECUTED",
         "ExecuteCommandLists did not land on the guest's queue (BUG-1 "
         "regression?)");
  char rec_det[96];
  snprintf(rec_det, sizeof(rec_det),
           "queue 0x%llx recorded %d command list(s) host-side",
           (unsigned long long)queue_ptr, recorded);
  CHECK(recorded == 1, rec_det);

  // Submission replay: the ECL must snapshot the guest's real draw parameters
  // into a D3D11-equivalent descriptor, not just remember a list pointer.
  {
    const int subs = trt_d3d12_submission_count((void*)(uintptr_t)queue_ptr);
    const int sub_draws = trt_d3d12_submission_draws(
        (void*)(uintptr_t)queue_ptr, 0);
    const unsigned verts = trt_d3d12_submission_vertices(
        (void*)(uintptr_t)queue_ptr, 0, 0);
    const unsigned insts = trt_d3d12_submission_instances(
        (void*)(uintptr_t)queue_ptr, 0, 0);
    MARKER(subs == 1 && sub_draws == 1 && verts == 3 && insts == 1,
           "SUBMISSION REPLAY",
           "recorded draw did not become a D3D11-equivalent submission "
           "descriptor (verts/instances lost)");
    char sdet[128];
    snprintf(sdet, sizeof(sdet),
             "submissions=%d draws=%d verts=%u insts=%u", subs, sub_draws,
             verts, insts);
    CHECK(subs == 1 && sub_draws == 1 && verts == 3 && insts == 1, sdet);

    MARKER(g_host_submit_calls >= 1 && g_host_submit_draws >= 1,
           "HOST SUBMIT",
           "ExecuteCommandLists did not reach the host submit hook");
    char hdet[96];
    snprintf(hdet, sizeof(hdet),
             "host submit calls=%u draws=%u", g_host_submit_calls,
             g_host_submit_draws);
    CHECK(g_host_submit_calls >= 1 && g_host_submit_draws >= 1, hdet);
  }

  MARKER(ret & 0x400u, "FENCE SIGNALED",
         "fence SetEventOnCompletion/Signal chain failed in guest");
  MARKER(ret & 0x800u, "", "CreateSwapChainForCoreWindow failed in guest");
  MARKER(ret & 0x1000u, "PRESENTED", "swapchain->Present failed in guest");

  trt_unmap(&be, img);
  free(file);

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
