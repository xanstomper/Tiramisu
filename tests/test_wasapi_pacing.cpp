// tiramisu/tests/test_wasapi_pacing.cpp
// Regression tests for the Ultra Pool boot wedge (measured live, tid=11 at
// 97% CPU holding a CS while Godot's WASAPI thread mixed forever):
//
//  1. WASAPI pacing contract — Godot's audio thread (4.3
//     audio_driver_wasapi.cpp) paces itself SOLELY on GetCurrentPadding:
//     write only while buffer_size - padding > 0, sleep 1ms only when it
//     wrote nothing. A padding that collapsed to 0 on read made the guest
//     write full buffers back-to-back forever. Padding must (a) reflect
//     submitted-but-not-consumed frames immediately after ReleaseBuffer,
//     (b) decay to 0 in real time, (c) never be consumed by being read.
//  2. WASAPI period event — the pump thread must signal the guest's event
//     at the device period cadence (Godot relies on it for wakeups).
//  3. Condition-variable wake atomicity — SleepConditionVariableSRW must
//     make "release the SRW" and "register as waiter" atomic against
//     WakeConditionVariable/WakeAllConditionVariable, or a producer that
//     acquires the just-released lock, updates the predicate and wakes
//     BEFORE the consumer registers loses the wakeup and the consumer
//     parks forever (measured: tid=11 parked in SleepConditionVariableSRW
//     while holding a critical section, wedging the whole boot).
//  4. Huge-allocation probe — msvcrt malloc must return NULL (with a named
//     log line) for absurd sizes instead of forwarding them to the host.
//
// Host-driven: each bridge fn is invoked through its real Win64 entry
// point the same way the guest calls it (test_audio.cpp pattern).
#include "../src/trt/trt.h"
#include "../src/trt/bridge_wasapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_wasapi(void);
int trt_bridge_register_msvcrt(void);
}

namespace {

struct extent {
  void* base;
  size_t len;
};
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

// Named-line capture for the huge-malloc probe check.
char g_log_lines[256][128];
int g_log_count = 0;
void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  if (g_log_count < 256) {
    strncpy(g_log_lines[g_log_count], line, 127);
    g_log_lines[g_log_count][127] = 0;
    ++g_log_count;
  }
}

int failures = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) printf("  PASS %s\n", msg);                                      \
    else { printf("  FAIL %s\n", msg); ++failures; }                           \
  } while (0)

u64 now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000u + (u64)ts.tv_nsec / 1000000u;
}

} // namespace

// ---- bridge fn types (Win64 ABI as the guest sees them) ----
using GetPadding_t = i32(TRT_GUEST_ABI*)(void*, u32*);
using GetBufferSize_t = i32(TRT_GUEST_ABI*)(void*, u32*);
using GetService_t = i32(TRT_GUEST_ABI*)(void*, const void*, void**);
using RenderGetBuffer_t = i32(TRT_GUEST_ABI*)(void*, u32, u8**);
using RenderReleaseBuffer_t = i32(TRT_GUEST_ABI*)(void*, u32, u32);
using CreateEventW_t = u64(TRT_STDCALL*)(void*, bool, bool, const void*);
using WaitForSingleObject_t = u32(TRT_STDCALL*)(u64, u32);
using SleepCVSRW_t = int(TRT_STDCALL*)(void*, void*, u32, u32);
using WakeCV_t = void(TRT_STDCALL*)(void*);
using SRWVoid_t = void(TRT_STDCALL*)(void*);
using Malloc_t = void*(TRT_GUEST_ABI*)(u64);

int main() {
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_wasapi();
  trt_bridge_register_msvcrt();

  printf("tiramisu WASAPI pacing + condition-variable wake tests\n");

  // ---- build the WASAPI object chain the way Godot does ----
  void* enumerator = nullptr;
  const int enum_hr = trt_wasapi_create_enumerator(nullptr, &enumerator, nullptr);
  CHECK(enum_hr == 0 && enumerator != nullptr,
        "IMMDeviceEnumerator created (CoCreateInstance path)");
  if (enum_hr != 0 || !enumerator) return 1;

  void** enum_vt = *(void***)enumerator;
  void* dev = nullptr;
  // IMMDeviceEnumerator vtbl: QI, AddRef, Release, EnumAudioEndpoints,
  // GetDefaultAudioEndpoint(dataFlow, role, out)
  ((i32(TRT_GUEST_ABI*)(void*, u32, u32, void**))enum_vt[4])(
      enumerator, 0 /*eRender*/, 0, &dev);
  CHECK(dev != nullptr, "GetDefaultAudioEndpoint(eRender) -> device");

  void* client = nullptr;
  // IMMDevice vtbl: QI, AddRef, Release, Activate(riid, clsctx, params, out)
  const int act_hr =
      ((i32(TRT_GUEST_ABI*)(void*, const void*, u32, void*, void**))(
          *(void***)dev)[3])(dev, nullptr, 0, nullptr, &client);
  CHECK(act_hr == 0 && client != nullptr,
        "IMMDevice::Activate -> IAudioClient");
  if (act_hr != 0 || !client) return 1;

  void** cvt = *(void***)client;
  // Initialize(share, flags, 100ms buffer, 0 periodicity, no fmt, no guid):
  // format null keeps the defaults (48000 Hz, stereo, float32).
  const int init_hr = ((i32(TRT_GUEST_ABI*)(void*, u32, u32, i64, i64,
                                            const void*, const void*))
                           cvt[3])(client, 0, 0, 1000000, 0, nullptr, nullptr);
  CHECK(init_hr == 0, "IAudioClient::Initialize accepted");

  u32 buffer_frames = 0;
  ((GetBufferSize_t)cvt[4])(client, &buffer_frames);
  CHECK(buffer_frames >= 4800, "GetBufferSize reports the 100ms buffer");
  using initialize_t = i32(TRT_GUEST_ABI*)(void*, u32, u32, i64, i64,
                                           const void*, const void*);
  const auto initialize = reinterpret_cast<initialize_t>(cvt[3]);
  const i64 very_long_duration = 0x7fffffffffffffffLL;
  const int bounded_hr = initialize(client, 0, 0, very_long_duration, 0,
                                    nullptr, nullptr);
  ((GetBufferSize_t)cvt[4])(client, &buffer_frames);
  CHECK(bounded_hr == 0 && buffer_frames == 96000,
        "Oversized audio duration is limited to two seconds");
  CHECK(initialize(client, 0, 0, 1000000, 0, nullptr, nullptr) == 0,
        "Normal buffer size can be restored");
  ((GetBufferSize_t)cvt[4])(client, &buffer_frames);

  // ---- test 1: padding reflects the submit immediately and decays in
  // real time; reading padding does not consume it ----
  void* render = nullptr;
  // IID_IAudioRenderClient {F294ACFC-3146-4483-A7BF-ADDCA7C260E2}
  const u8 iid_render[16] = {0xFC, 0xAC, 0x94, 0xF2, 0x46, 0x31, 0x83, 0x44,
                             0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2};
  const int svc_hr = ((GetService_t)cvt[14])(client, iid_render, &render);
  CHECK(svc_hr == 0 && render != nullptr,
        "GetService(IAudioRenderClient) -> render client");

  if (render) {
    void** rvt = *(void***)render;
    u8* data = nullptr;
    const u32 half = buffer_frames / 2;
    const int gb_hr = ((RenderGetBuffer_t)rvt[3])(render, half, &data);
    CHECK(gb_hr == 0 && data != nullptr, "render GetBuffer(half) accepted");
    const int rb_hr = ((RenderReleaseBuffer_t)rvt[4])(render, half, 0);
    CHECK(rb_hr == 0, "render ReleaseBuffer(half) accepted");

    u32 padding = 0;
    ((GetPadding_t)cvt[6])(client, &padding);
    CHECK(padding >= half / 2 && padding <= buffer_frames,
          "padding reflects the just-submitted frames immediately");
    u32 padding_again = 0;
    ((GetPadding_t)cvt[6])(client, &padding_again);
    CHECK(padding_again >= half / 2,
          "reading padding does not consume it (no destructive read)");

    // decay: within 3 device-seconds the buffer must drain (real time)
    const u64 deadline = now_ms() + 3000;
    u32 drained = 0;
    while (now_ms() < deadline) {
      u32 p = 0;
      ((GetPadding_t)cvt[6])(client, &p);
      if (p == 0) {
        drained = 1;
        break;
      }
      usleep(20000);
    }
    CHECK(drained, "padding decays to 0 in real time (device consumption)");
  }

  // ---- test 2: the pump signals the guest's period event ----
  {
    auto pCreateEvent =
        (CreateEventW_t)trt_resolve_module(nullptr, "kernel32.dll",
                                           "CreateEventW", 0);
    auto pWait = (WaitForSingleObject_t)trt_resolve_module(
        nullptr, "kernel32.dll", "WaitForSingleObject", 0);
    if (pCreateEvent && pWait) {
      const u64 ev = pCreateEvent(nullptr, false /*auto-reset*/, false, nullptr);
      CHECK(ev != 0, "CreateEventW for the WASAPI period event");
      ((i32(TRT_GUEST_ABI*)(void*, void*))cvt[13])(client, (void*)(uintptr_t)ev);
      ((i32(TRT_GUEST_ABI*)(void*))cvt[10])(client);  // Start

      // the pump fires every ~10ms; over 120ms an auto-reset event must be
      // signalled several times (each successful 0-timeout wait consumes it)
      int wakes = 0;
      const u64 deadline = now_ms() + 120;
      while (now_ms() < deadline) {
        if (pWait(ev, 0) == 0) ++wakes;
        usleep(1000);
      }
      CHECK(wakes >= 3, "the pump signals the period event at device cadence");
    } else {
      CHECK(false, "event fns resolvable (kernel32)");
    }
  }

  // ---- test 3: CV wake atomicity (the lost-wakeup wedge) ----
  {
    auto pInitSRW = (SRWVoid_t)trt_resolve_module(nullptr, "kernel32.dll",
                                                  "InitializeSRWLock", 0);
    auto pAcqEx = (SRWVoid_t)trt_resolve_module(
        nullptr, "kernel32.dll", "AcquireSRWLockExclusive", 0);
    auto pRelEx = (SRWVoid_t)trt_resolve_module(
        nullptr, "kernel32.dll", "ReleaseSRWLockExclusive", 0);
    auto pSleep = (SleepCVSRW_t)trt_resolve_module(
        nullptr, "kernel32.dll", "SleepConditionVariableSRW", 0);
    auto pWake = (WakeCV_t)trt_resolve_module(nullptr, "kernel32.dll",
                                              "WakeConditionVariable", 0);
    auto pWakeAll = (WakeCV_t)trt_resolve_module(nullptr, "kernel32.dll",
                                                 "WakeAllConditionVariable", 0);
    CHECK(pInitSRW && pAcqEx && pRelEx && pSleep && pWake && pWakeAll,
          "SRW/condition-variable fns resolvable");

    if (pInitSRW && pAcqEx && pRelEx && pSleep && pWake && pWakeAll) {
      int lost = 0;
      for (int round = 0; round < 50; ++round) {
        // canonical producer/consumer shape: consumer waits on the CV while
        // the producer sets the flag under the just-released lock and wakes.
        void* srw = malloc(64);
        void* cv = malloc(64);
        memset(srw, 0, 64);
        memset(cv, 0, 64);
        pInitSRW(srw);

        std::atomic<int> flag{0};
        std::atomic<int> woke{0};

        std::thread consumer([&]() {
          // canonical Win32 CV loop: the lock is held across the predicate
          // check; SleepConditionVariableSRW returns with the lock
          // RE-ACQUIRED (SRW is not recursive — never re-acquire in the
          // loop). INFINITE: a lost wakeup must surface as a parked thread,
          // which the unstick below resolves for a clean exit.
          pAcqEx(srw);
          while (!flag.load(std::memory_order_acquire)) {
            pSleep(cv, srw, 0xFFFFFFFFu, 0);
          }
          pRelEx(srw);
          woke.store(1, std::memory_order_release);
        });

        // let the consumer acquire + park first
        usleep(2000);
        pAcqEx(srw);
        flag.store(1, std::memory_order_release);
        if (round % 2) pWakeAll(cv);
        else pWake(cv);
        pRelEx(srw);

        // the consumer must wake and exit on its own; a lost wakeup parks
        // it forever (the measured boot wedge)
        const u64 deadline = now_ms() + 4000;
        while (now_ms() < deadline && !woke.load(std::memory_order_acquire)) {
          usleep(1000);
        }
        if (!woke.load(std::memory_order_acquire)) {
          ++lost;
          fprintf(stderr, "  [test] LOST WAKEUP on round %d\n", round);
          // unstick: set the flag and wake-all so the parked consumer's
          // INFINITE wait releases and it exits (keeps the process clean)
          pAcqEx(srw);
          flag.store(1, std::memory_order_release);
          pWakeAll(cv);
          pRelEx(srw);
        }
        consumer.join();
        free(srw);
        free(cv);
      }
      CHECK(lost == 0, "50 producer/consumer rounds: every waiter woke");
    }
  }

  // ---- test 4: huge-allocation probe names and refuses absurd sizes ----
  {
    auto pm = (Malloc_t)trt_resolve_module(nullptr, "msvcrt.dll", "malloc", 0);
    CHECK(pm != nullptr, "msvcrt.dll!malloc resolvable");
    if (pm) {
      void* small = pm(1024);
      CHECK(small != nullptr, "malloc(1024) succeeds");
      void* huge = pm(1073741824u /*1 GB, absurd for a game alloc*/);
      CHECK(huge == nullptr, "malloc(1GB) refused -> NULL (fail-visible)");
      int named = 0;
      for (int i = 0; i < g_log_count; ++i)
        if (strstr(g_log_lines[i], "msvcrt: malloc(sz=")) named = 1;
      CHECK(named, "the refused allocation is named in the log");
    }
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
