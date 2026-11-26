// tiramisu/tests/test_guest_eh.cpp
// The exception-dispatch gate (W1). Boots a real freestanding guest whose
// .pdata carries EHANDLER+UHANDLER, whose scope table is compiler-generated,
// and whose personality is a faithful __C_specific_handler. Then it raises
// real exceptions through the real bridge:
//
//   1. throw in the establishing frame        -> caught by that frame
//   2. exception record intact at the filter  -> code/params survive
//   3. throw four frames down                 -> unwinder walks back
//   4. inner non-matching scope               -> does NOT swallow it
//   5. execution continues after the handler  -> no return-address damage
//
// This is the test GAP-MAP said would have caught RACCOIN's wall.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/pe_exports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>

#include <sys/mman.h>
#include <unistd.h>

#include <vector>

extern "C" int trt_dispatch_hw_fault(int, void*, void*);
extern "C" void* TRT_STDCALL trt_AddVectoredExceptionHandler(u32, void*);
extern "C" u32 TRT_STDCALL trt_RemoveVectoredExceptionHandler(void*);

namespace {

i32 TRT_STDCALL retry_unchanged_fault(void*) { return -1; }

int failures = 0;
#define CHECK(cond, msg)                                     \
  do {                                                       \
    if (cond) {                                              \
      printf("  PASS %s\n", msg);                            \
    } else {                                                 \
      printf("  FAIL %s\n", msg);                            \
      ++failures;                                            \
    }                                                        \
  } while (0)

struct extent {
  void* base;
  size_t len;
};
extent g_extents[64];
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
  if (g_extent_count < 64) {
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
  const size_t len =
      ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
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

// The console host registers .pdata with the real RtlAddFunctionTable; the
// offline runtime scans img->pdata directly, so a no-op success keeps the
// backend contract honest.
int linux_register_unwind(void* image_base, const void* pdata, u32 count) {
  (void)image_base;
  (void)pdata;
  (void)count;
  return 0;
}

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

// bridge entry points (C linkage, declared here like the other harnesses do)
extern "C" void* TRT_STDCALL trt_RtlLookupFunctionEntry(u64 rip, u64* base_out,
                                                        void* history);
extern "C" u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d);

void* eh_lookup(u64 rip, u64* base_out) {
  return trt_RtlLookupFunctionEntry(rip, base_out, nullptr);
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
      : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]), [fn] "r"(fn)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
        "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_guest_eh <guest_eh.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();

  printf("== tiramisu exception-dispatch gate ==\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE with EHANDLER .pdata maps");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");
  // the runtime's image registry is what RtlLookupFunctionEntry walks
  trt_images_register_main(img, "guest_eh.exe");
  CHECK(img->pdata != nullptr && img->pdata_count > 0,
        "the image carries .pdata");

  // the runtime's own table walk must find the function that owns the entry
  const u64 entry_va = (u64)(uintptr_t)img->base + img->entry_rva;
  u64 base_out = 0;
  void* fe = eh_lookup(entry_va, &base_out);
  CHECK(fe != nullptr && base_out != 0,
        "RtlLookupFunctionEntry answers for a live guest RIP");

  const u64 ret = win64_call((void*)(uintptr_t)entry_va, 0, 0, 0);
  printf("  guest returned 0x%llx\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "throw in the establishing frame -> caught there");
  CHECK(ret & 0x2u, "the exception record reached the filter intact");
  CHECK(ret & 0x4u, "throw four frames down -> unwinder walked back");
  CHECK(ret & 0x8u, "a non-matching inner scope did not swallow it");
  CHECK(ret & 0x10u, "execution continued after the handler");
  CHECK((ret & (1ull << 40)) == 0, "the raising path did not fall through");
  CHECK((ret & (1ull << 41)) == 0, "the non-matching handler never ran");
  CHECK((ret & (1ull << 42)) == 0,
        "control never continued past a block that threw");

  // --- funclet rethrow: catch handler throws, outer frame catches ----------
  // Locks resume-record push (pass 2), the pass-1 splice rescue, and the
  // outward crossing to the pre-existing caller chain (RACCOIN's wall).
  trt_resolver rs2;
  trt_resolver_default(&rs2);
  void* rethrow_fn = trt_pe_find_export(&be, &rs2, img,
                                        "guest_eh_funclet_rethrow", 0);
  CHECK(rethrow_fn != nullptr, "funclet-rethrow fixture exported");
  if (rethrow_fn) {
    const u64 r = win64_call(rethrow_fn, 0, 0, 0);
    printf("  funclet rethrow returned 0x%llx\n", (unsigned long long)r);
    CHECK(r & 0x20u, "rethrow in the catch funclet -> outer frame caught it");
    CHECK(r & 0x40u, "execution continued after the rethrow saga");
    CHECK((r & (1ull << 40)) == 0, "the rethrowing handler never fell through");
    CHECK((r & (1ull << 41)) == 0, "the inner frame never swallowed the rethrow");
  }

  // --- funclet rethrow that escapes all guest frames -----------------------
  // Locks the boundary swallow: the host->guest call must report a synthetic
  // 0 return (UnityPlayer native wrapper-catch emulation) and the process
  // must live. The call goes through trt_call_guest — that is where the
  // per-thread escape pad (setjmp landing pad) is armed; the raw-asm
  // win64_call deliberately bypasses it and would terminate.
  void* esc_fn = trt_pe_find_export(&be, &rs2, img,
                                    "guest_eh_funclet_rethrow_escape", 0);
  CHECK(esc_fn != nullptr, "rethrow-escape fixture exported");
  if (esc_fn) {
    const u64 r = trt_call_guest(esc_fn, 0, 0, 0, 0);
    printf("  rethrow escape returned 0x%llx (0 = swallowed at boundary)\n",
           (unsigned long long)r);
    CHECK(r == 0, "escaped rethrow -> boundary reports synthetic 0 return");
  }

#if defined(__x86_64__)
  void* retry_handler =
      trt_AddVectoredExceptionHandler(1, reinterpret_cast<void*>(&retry_unchanged_fault));
  CHECK(retry_handler != nullptr, "register a continuing vectored exception handler");
  if (retry_handler) {
    siginfo_t fault{};
    fault.si_addr = reinterpret_cast<void*>(1);
    ucontext_t original{};
    original.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(entry_va);
    original.uc_mcontext.gregs[REG_RSP] = reinterpret_cast<greg_t>(&original);
    ucontext_t context = original;
    CHECK(trt_dispatch_hw_fault(SIGSEGV, &fault, &context) == 1,
          "allow a handler to repair memory and retry the instruction");
    int last_result = 1;
    for (int i = 0; i < 8; ++i) {
      context = original;
      last_result = trt_dispatch_hw_fault(SIGSEGV, &fault, &context);
    }
    CHECK(last_result == 0, "stop retries when the same guest memory fault keeps recurring");
    context = original;
    fault.si_addr = reinterpret_cast<void*>(2);
    CHECK(trt_dispatch_hw_fault(SIGSEGV, &fault, &context) == 1,
          "a different fault has its own retry budget");
    trt_RemoveVectoredExceptionHandler(retry_handler);
  }
#endif

  free(file);
  printf("%s (%d failed)\n", failures ? "RESULT: FAIL" : "RESULT: ALL PASS",
         failures);
  return failures ? 1 : 0;
}
