// tiramisu/tests/test_openal.cpp
// The OpenAL bridge: a guest runs the streaming-audio loop through the IAT
// and the host audio sink observes every PCM submission.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_openal(void);
void trt_audio_set_hooks(int (*submit)(const void*, u32, u32, u32, u32,
                                       void*),
                         void* user);
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

// ---- host sink observation ----
u32 g_submit_count = 0;
u32 g_last_bytes = 0, g_last_rate = 0, g_last_channels = 0, g_last_bps = 0;
u32 g_total_bytes = 0;
int sink_submit(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) {
  (void)pcm; (void)user;
  ++g_submit_count;
  g_last_bytes = bytes;
  g_last_rate = rate;
  g_last_channels = channels;
  g_last_bps = bytes_per_sample;
  g_total_bytes += bytes;
  return 0;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_openal <guest.exe>\n");
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
  trt_bridge_register_openal();

  // the console's audio sink (same shape the audio bridge test uses)
  trt_audio_set_hooks(&sink_submit, nullptr);

  printf("tiramisu OpenAL bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (openal bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xfff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "alcOpenDevice -> device");
  CHECK(ret & 0x002u, "context created + made current");
  CHECK(ret & 0x004u, "alGenBuffers filled 4 handles");
  CHECK(ret & 0x008u, "alBufferData accepted PCM (AL_NO_ERROR)");
  CHECK(ret & 0x010u, "alGenSources filled a handle");
  CHECK(ret & 0x020u, "alSourceQueueBuffers queued 4");
  CHECK(ret & 0x040u, "alSourcePlay -> AL_PLAYING");
  CHECK(ret & 0x080u, "AL_BUFFERS_QUEUED == 4");
  CHECK(ret & 0x100u, "pump drained: AL_BUFFERS_PROCESSED >= 4");
  CHECK(ret & 0x200u, "alSourceUnqueueBuffers retired 4");
  CHECK(ret & 0x400u, "AL_BUFFERS_QUEUED back to 0");
  CHECK(ret & 0x800u, "alSourceStop + alcCloseDevice OK");

  // host-side observation: the sink actually received the PCM
  CHECK(g_submit_count >= 4, "sink: >= 4 PCM submissions");
  CHECK(g_last_bytes == 22050 * 2, "sink: last buffer = 44100 bytes");
  CHECK(g_last_rate == 22050, "sink: rate 22050 preserved");
  CHECK(g_last_channels == 1, "sink: mono (AL_FORMAT_MONO16)");
  CHECK(g_last_bps == 2, "sink: 16-bit samples");
  CHECK(g_total_bytes >= 4 * 22050 * 2, "sink: >= 176400 bytes total");

  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
