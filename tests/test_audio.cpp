// tiramisu/tests/test_audio.cpp
// XAudio2: the guest creates the engine, voices, submits PCM and polls the
// queue. The host sink (hooked here) counts the PCM bytes it received.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_audio(void);
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

// ---- the host audio sink (what the console would wire to its real output) ----
int g_submit_count = 0;
u32 g_submit_bytes = 0;
u32 g_submit_rate = 0;
u32 g_submit_channels = 0;
u32 g_submit_width = 0;

int sink_submit(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) {
  (void)pcm; (void)user;
  ++g_submit_count;
  g_submit_bytes += bytes;
  g_submit_rate = rate;
  g_submit_channels = channels;
  g_submit_width = bytes_per_sample;
  return 0;
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
    printf("usage: test_audio <guest.exe>\n");
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
  trt_bridge_register_audio();

  // the console host publishes its real output device the same way
  trt_audio_set_hooks(&sink_submit, nullptr);

  printf("tiramisu XAudio2 test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (xaudio2 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "XAudio2Create returned the engine");
  CHECK(ret & 0x2u, "StartEngine accepted");
  CHECK(ret & 0x4u, "CreateMasteringVoice returned a voice");
  CHECK(ret & 0x8u, "CreateSourceVoice (PCM format) returned a voice");
  CHECK(ret & 0x10u, "SubmitSourceBuffer accepted");
  CHECK(ret & 0x20u, "GetState showed the queued buffer");
  CHECK(ret & 0x40u, "FlushSourceBuffers emptied the queue");
  CHECK(ret & 0x80u, "Stop accepted");

  // the host sink actually received the guest's PCM
  CHECK(g_submit_count >= 1, "the host sink received a buffer");
  CHECK(g_submit_bytes >= 512, "the sink got the PCM bytes (>= 512)");
  CHECK(g_submit_rate == 48000, "the sink got the sample rate (48000)");
  CHECK(g_submit_channels == 2, "the sink got the channel count (2)");
  CHECK(g_submit_width == 2, "the sink got 16-bit samples");

  // XAudio 2.7 COM path (CLSID_XAudio2) + X3DAudio1_7 + XAPOFX1_5
  {
    using CoInitEx_t = int(TRT_GUEST_ABI*)(void*, u32);
    using CoCreate_t = int(TRT_GUEST_ABI*)(const u8*, void*, u32, const u8*, void**);
    auto pCoInit = (CoInitEx_t)trt_resolve_module(nullptr, "ole32.dll", "CoInitializeEx", 0);
    auto pCoCreate = (CoCreate_t)trt_resolve_module(nullptr, "ole32.dll", "CoCreateInstance", 0);
    static const u8 k_clsid_xa27[16] = {
        0x85, 0x86, 0x50, 0x5A, 0x54, 0xA2, 0xBA, 0x4F,
        0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF};
    if (pCoInit) pCoInit(nullptr, 0);
    void* xa27 = nullptr;
    const int cchr = pCoCreate ? pCoCreate(k_clsid_xa27, nullptr, 1, k_clsid_xa27, &xa27) : -1;
    u32 dev_cnt = 0;
    u16 dev_name[64] = {};
    u8 dev_details[1100] = {};
    void* mv27 = nullptr;
    int init_hr = -1, gdd_hr = -1, mv_hr = -1;
    if (cchr == 0 && xa27) {
      void** vt = *(void***)xa27;
      using GetCnt_t = int(TRT_GUEST_ABI*)(void*, u32*);
      using GetDet_t = int(TRT_GUEST_ABI*)(void*, u32, void*);
      using Init_t = int(TRT_GUEST_ABI*)(void*, u32, u32);
      using CrtMv_t = int(TRT_GUEST_ABI*)(void*, void**, u32, u32, u32, u32, const void*);
      using Rel_t = u32(TRT_GUEST_ABI*)(void*);
      ((GetCnt_t)vt[3])(xa27, &dev_cnt);
      gdd_hr = ((GetDet_t)vt[4])(xa27, 0, dev_details);
      memcpy(dev_name, dev_details + 4, 64);
      init_hr = ((Init_t)vt[5])(xa27, 0, 1);
      mv_hr = ((CrtMv_t)vt[10])(xa27, &mv27, 2, 48000, 0, 0, nullptr);
      ((Rel_t)vt[2])(xa27);
    }
    CHECK(cchr == 0 && xa27 != nullptr && dev_cnt >= 1 && gdd_hr == 0 &&
              dev_name[0] != 0 && init_hr == 0 && mv_hr == 0 && mv27 != nullptr,
          "XAudio2.7 COM CLSID_XAudio2 Initialize/GetDeviceDetails/CreateMasteringVoice works");

    using X3DInit_t = int(TRT_GUEST_ABI*)(u32, float, u8*);
    using X3DCalc_t = void(TRT_GUEST_ABI*)(const u8*, const void*, const void*, u32, void*);
    using CrtFX_t = int(TRT_GUEST_ABI*)(const u8*, void**, const void*, u32);
    auto pX3Init = (X3DInit_t)trt_resolve_module(nullptr, "x3daudio1_7.dll", "X3DAudioInitialize", 0);
    auto pX3Calc = (X3DCalc_t)trt_resolve_module(nullptr, "x3daudio1_7.dll", "X3DAudioCalculate", 0);
    auto pCrtFX = (CrtFX_t)trt_resolve_module(nullptr, "xapofx1_5.dll", "CreateFX", 0);
    u8 x3d_inst[20] = {};
    struct { float fx, fy, fz, tx, ty, tz, px, py, pz, vx, vy, vz; const void* c; }
        lst{0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, nullptr},
        emt{0, 0, 1, 0, 1, 0, 10, 0, 0, 0, 0, 0, nullptr};
    // Match minimal x3d_emitter layout tail (CurveDistanceScaler near end) -> set full block
    struct {
      const void* pCone;
      float orientF[3], orientT[3], pos[3], vel[3];
      float innerR, innerRAngle; u32 ch; float chRad;
      const float* chAz; const void* curves[5];
      float curve, doppler;
    } em2{};
    em2.pos[0] = 2.0f;
    em2.ch = 1;
    em2.curve = 1.0f;
    float mtx[8] = {};
    struct { float* pMatrixCoefficients; float* pDelayTimes; u32 SrcChannels; u32 DstChannels;
             float LPFDirectCoefficient, LPFReverbCoefficient, ReverbLevel, DopplerFactor,
             EmitterToListenerAngle, EmitterToListenerDistance, EmitterVelocityComponent,
             ListenerVelocityComponent; } dsp{mtx, nullptr, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    (void)emt;
    void* xapo = nullptr;
    const int x3ok = pX3Init ? pX3Init(3, 343.5f, x3d_inst) : -1;
    if (x3ok == 0 && pX3Calc) pX3Calc(x3d_inst, &lst, &em2, 1, &dsp);
    const int fxok = pCrtFX ? pCrtFX(k_clsid_xa27, &xapo, nullptr, 0) : -1;
    if (xapo) {
      void** fvt = *(void***)xapo;
      ((u32(TRT_GUEST_ABI*)(void*))fvt[2])(xapo);
    }
    CHECK(x3ok == 0 && dsp.EmitterToListenerDistance > 1.5f && fxok == 0 && xapo != nullptr,
          "x3daudio1_7.dll + xapofx1_5.dll attenuation and CreateFX succeed");
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
