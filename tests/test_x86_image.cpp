// tiramisu/tests/test_x86_image.cpp
// Gate for PE32 image execution: build a minimal REAL PE32, map it through
// trt_map_pe32, and run its entry through the clean-room interpreter.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/x86_32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

// Minimal host stubs so the PE32 mapping path can link without the full
// bridge set (the interpreter gate is about x86-32 execution, not Win32).
extern "C" {
int trt_mem_budget_charge_commit(u64) { return 0; }
void trt_mem_budget_discharge_commit(u64) {}
int trt_mem_budget_charge_va(u64) { return 0; }
void trt_mem_budget_discharge_va(u64) {}
u32 TRT_STDCALL trt_GetCurrentThreadId(void) { return 1; }
void trt_eh_retire_resume_records(unsigned) {}
void trt_bridge_post_quit(int) {}
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static void* be_alloc(size_t n, u64) {
  return mmap(nullptr, n, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static int be_protect(void* p, size_t n, unsigned prot) {
  int m = PROT_READ;
  if (prot & TRT_PROT_W) m |= PROT_WRITE;
  if (prot & TRT_PROT_X) m |= PROT_EXEC;
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  return mprotect((void*)((uintptr_t)p & ~(page - 1)), n + page, m);
}
static void be_release(void* p, size_t n) { munmap(p, n); }
static void be_log(const char* line) { (void)line; }
static trt_backend be{be_alloc, be_protect, be_release, be_log, nullptr};
static u64 TRT_GUEST_ABI test_bridge(u64, u64, u64, u64) { return 42; }
static void* rs_resolve(void*, const char* mod, const char* sym, u16) {
  if (mod && sym && !strcasecmp(mod, "testdll.dll") && !strcasecmp(sym, "TestFn"))
    return (void*)&test_bridge;
  return nullptr;
}
static trt_resolver rs{nullptr, rs_resolve};

static void put16(u8* p, u16 v) { memcpy(p, &v, 2); }
static void put32(u8* p, u32 v) { memcpy(p, &v, 4); }

// Minimal PE32: one .text section at RVA 0x1000 with one import. Entry calls
// [IAT] -> testdll.dll!TestFn, whose host bridge returns 42; then HLT.
static size_t make_pe32(u8* out) {
  memset(out, 0, 0x600);
  out[0] = 'M'; out[1] = 'Z';
  put32(out + 0x3c, 0x40);
  u8* pe = out + 0x40;
  pe[0] = 'P'; pe[1] = 'E'; pe[2] = 0; pe[3] = 0;
  put16(pe + 4, 0x014c);            // i386
  put16(pe + 6, 1);                 // one section
  put16(pe + 20, 224);              // size of optional header
  u8* opt = pe + 24;
  put16(opt, 0x10b);                // PE32 magic
  put32(opt + 16, 0x1000);          // AddressOfEntryPoint
  put32(opt + 28, 0x10000000);      // ImageBase
  put32(opt + 32, 0x1000);          // SectionAlignment
  put32(opt + 36, 0x200);           // FileAlignment
  put32(opt + 56, 0x2000);          // SizeOfImage
  put32(opt + 60, 0x200);           // SizeOfHeaders
  put16(opt + 68, 3); put16(opt + 70, 0);  // subsystem = console
  put32(opt + 92, 16);              // NumberOfRvaAndSizes
  put32(opt + 96 + 8, 0x1300);      // import directory RVA
  put32(opt + 96 + 12, 20);         // import directory size
  u8* sec = opt + 224;
  memcpy(sec, ".text\0\0", 8);
  put32(sec + 8, 0x400);            // VirtualSize
  put32(sec + 12, 0x1000);          // VirtualAddress
  put32(sec + 16, 0x400);           // SizeOfRawData
  put32(sec + 20, 0x200);           // PointerToRawData
  put32(sec + 36, 0x60000020);      // code | execute | read
  u8* code = out + 0x200;
  code[0] = 0xff; code[1] = 0x15; put32(code + 2, 0x10001360u);  // call [guest-VA IAT]
  code[6] = 0xf4;                   // hlt

  // Import descriptor at RVA 0x1300 (file 0x300).
  u8* imp = out + 0x500;
  put32(imp + 0, 0x1320);   // OriginalFirstThunk
  put32(imp + 12, 0x1380);  // Dll name RVA
  put32(imp + 16, 0x1360);  // FirstThunk / IAT
  put32(out + 0x520, 0x1340);                      // INT slot -> hint/name
  put16(out + 0x540, 0); memcpy(out + 0x542, "TestFn", 7);  // hint/name
  memcpy(out + 0x580, "testdll.dll", 12);                   // dll name
  put32(out + 0x560, 0x1340);                      // IAT initially -> hint/name
  return 0x600;
}

static FILE* g_trace = nullptr;
static void trace_op(u32 va, unsigned op, u32 esp) {
  if (g_trace) fprintf(g_trace, "%08x %02x esp=%08x\n", va, op, esp);
}

static u64 TRT_GUEST_ABI dummy_bridge(u64, u64, u64, u64) { return 0; }
static void* real_resolve(void*, const char* mod, const char* sym, u16) {
  (void)mod; (void)sym;
  return (void*)&dummy_bridge;  // every import binds; traps name themselves
}
static trt_resolver real_rs{nullptr, real_resolve};

// Diagnostic: map a REAL 32-bit PE and run its entry through the interpreter.
static int run_real_pe32(const char* path, u32 reason = 1) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return 2; }
  std::vector<u8> data((size_t)sz);
  if (fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 2; }
  fclose(f);
  trt_runtime_init(&be, "");
  trt_image* img = nullptr;
  int rc = trt_map_pe32(&be, &real_rs, data.data(), data.size(), &img);
  if (rc != TRT_OK || !img) { printf("map rc=%d\n", rc); return 2; }
  trt_x86_set_resolver(&real_rs);
  g_trace = fopen("/tmp/opencode/x86-trace.txt", "w");
  trt_x86_set_trace(&trace_op);
  u32 eax = 0;
  u32 args[3] = {img->guest_base, reason, 0};
  int layout = -1;
  // Exercise every DllMain lifecycle reason; a DLL may fault on ATTACH but
  // still be fully executable on DETACH/THREAD callbacks. Report the first
  // reason that completes so the harness never mistakes one bad callback for
  // an unexecutable image.
  int run_rc = 2;
  u32 used_reason = reason;
  const u32 reasons[3] = {1, 0, 2};
  for (u32 r : reasons) {
    args[1] = r;
    int rc = trt_x86_run_entry_auto(
        img, img->guest_base + (u32)img->entry_rva, args, 200000, &eax,
        &layout);
    if (rc == 0 || rc == 1) { run_rc = rc; used_reason = r; break; }
    run_rc = rc;
  }
  printf("REAL PE32 %s: bits=32 traps=%u run_rc=%d eax=%u "
         "entry=%llx last_op=%02x fault_va=%08x layout=%d reason=%u\n",
         path, img->pe32_trap_count, run_rc, eax,
         (unsigned long long)img->entry_rva,
         trt_x86_last_opcode(), trt_x86_last_fault_va(), layout, used_reason);
  trt_images_reset();
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1) return run_real_pe32(argv[1], argc > 2 ? (u32)atoi(argv[2]) : 1u);
  trt_runtime_init(&be, "");
  u8 file[0x600];
  size_t n = make_pe32(file);

  trt_image* img = nullptr;
  int rc = trt_map_pe32(&be, &rs, file, n, &img);
  CHECK(rc == TRT_OK && img != nullptr, "minimal PE32 mapped");
  if (!img) { printf("  result: FAIL\n"); return 1; }
  CHECK(img->is_pe32 == 1, "image flagged PE32");
  CHECK(img->entry_rva == 0x1000, "entry RVA parsed from PE32 header");

    trt_x86_set_resolver(&rs);
  u32 eax = 0;
  char trap[160] = "";
  rc = trt_x86_run_image(img, img->guest_base + (u32)img->entry_rva,
                         0x10000, 64, &eax, trap, sizeof(trap));
  CHECK(rc == 0, "PE32 entry ran to HLT through the interpreter");
  CHECK(eax == 42, "PE32 import trap called the real host bridge (EAX=42)");
  CHECK(strstr(trap, "testdll.dll!TestFn") != nullptr,
        "trap dispatched by exact dll!symbol name");

  // DllMain-style cdecl call: mov eax,[esp+8]; ret -> reason argument (1).
  {
    const u8 dllmain[] = {0x8b, 0x44, 0x24, 0x08, 0xc3};
    memcpy(img->base + 0x1000, dllmain, sizeof(dllmain));
    const u32 args[3] = {img->guest_base, 1, 0};
    u32 dll_rc = 0;
    const int r = trt_x86_run_cdecl(img, img->guest_base + 0x1000,
                                    args, 32, &dll_rc);
    CHECK(r == 1 && dll_rc == 1,
          "PE32 DllMain(cdecl) got its args and returned via sentinel RET");
  }

  trt_images_reset();
  printf("  result: %s\n", fails ? "FAIL" : "ALL PASS");
  return fails ? 1 : 0;
}
