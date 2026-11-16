// tiramisu/src/trt/bridge_winmm.cpp
// winmm.dll for the guest — the multimedia clock.
//
// Nearly every game pulls its frame timing from winmm!timeGetTime (even
// XAudio2-era engines keep it as the scheduler clock), and startup often
// calls timeBeginPeriod to raise the timer resolution. Without this bridge
// those calls hit a fail-visible STUB and the game either spins at 100%
// waiting on a clock that never moves or aborts with a named error.
//
// waveOut*/midiOut* answer "no legacy devices" (0 devices) instead of
// failing: engines probe them and fall back to XAudio2, which is the real
// path this runtime serves.
#include "bridge_winmm.h"
#include "trt.h"
#include "thunk.h"
#include "images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cerrno>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

namespace {

// timeGetTime: ms since boot, wrapping at 2^32 like Windows.
u32 now_ms() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);  // portable across Windows/UWP and Linux
  return (u32)((u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000);
}

// the requested period (ms), for visibility in logs
u32 g_min_period = 0;

// ---- multimedia timers (timeSetEvent/timeKillEvent): OpenTTD uses these
// as its network/scheduler tick. Real contract: a callback fired on a host
// thread every `period` ms (periodic) or once (TIME_ONESHOT). We run one
// std::thread per timer with a monotonic steady_clock schedule and an
// atomic kill flag — no polling loops at coarse granularity. ----
struct mm_timer {
  std::atomic<u32> dead{0};
  u32 id = 0;     // the opaque id we handed out (== callback arg0)
  u64 cb = 0;     // guest function pointer
  u64 user = 0;   // guest user data
  std::thread th;
};
std::mutex g_mm_mu;
std::vector<mm_timer*> g_mm_timers;

void mm_timer_fire(u32 id, u64 cb, u64 user) {
  // Guest callback: Win64 ABI TimeProc(UINT uTimerID, UINT uMsg,
  // DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2). Call through
  // trt_call_guest (the same gate CreateThread uses) so the call gets a
  // guest image context and exceptions stay inside guest dispatch.
  trt_call_guest((void*)(uintptr_t)cb, id, 0, user, 0);
}

void mm_timer_thread(mm_timer* t, u32 period_ms, bool periodic) {
  auto deadline = std::chrono::steady_clock::now();
  const auto period = std::chrono::milliseconds(period_ms);
  for (;;) {
    deadline += period;
    // sleep_until the absolute steady-clock deadline: keeps the drift-free
    // accumulate-to-deadline schedule AND works under the UCRT (which has
    // no POSIX nanosleep — the clang-cl UWP host build failed to compile
    // the old pair). History: the first draft passed the raw epoch-ns
    // timestamp to nanosleep as its RELATIVE duration and the timer slept
    // ~1.7 billion seconds (caught by the guest_winmm 0x40/0x80 contract).
    std::this_thread::sleep_until(deadline);
    if (t->dead.load(std::memory_order_acquire)) return;
    mm_timer_fire(t->id, t->cb, t->user);
    if (t->dead.load(std::memory_order_acquire) || !periodic) return;
  }
}

u32 mm_timer_set(u32 period_ms, u64 cb, u64 user, u32 flags) {
  if (!period_ms || !cb) return 0;  // TIMERR_NOCANDO
  const bool periodic = (flags & 0x0001) != 0;  // TIME_PERIODIC
  auto* t = new mm_timer();
  t->cb = cb;
  t->user = user;
  t->id = (u32)(uintptr_t)t | 1u;  // opaque nonzero id; the pointer is ours
  {
    std::lock_guard<std::mutex> lk(g_mm_mu);
    g_mm_timers.push_back(t);
  }
  char line[128];
  snprintf(line, sizeof(line),
           "winmm: timeSetEvent(period=%ums %s cb=%llx) -> host timer live",
           period_ms, periodic ? "periodic" : "oneshot", (unsigned long long)cb);
  trt_core_log(line);
  try {
    t->th = std::thread(mm_timer_thread, t, period_ms, periodic);
  } catch (...) {
    t->dead.store(1, std::memory_order_release);
    char err[96];
    snprintf(err, sizeof(err),
             "winmm: timeSetEvent thread creation FAILED (errno=%d) -> 0",
             errno);
    trt_core_log(err);
    return 0;
  }
  // Win32 returns the opaque nonzero timer id (== TimeProc arg0).
  return t->id;
}

void mm_timer_kill(u32 id) {
  if (!id) return;
  mm_timer* t = (mm_timer*)(uintptr_t)(id & ~1u);
  {
    std::lock_guard<std::mutex> lk(g_mm_mu);
    for (mm_timer* x : g_mm_timers)
      if (x == t) { t->dead.store(1, std::memory_order_release); return; }
  }
}

void mm_timer_teardown_all() {
  std::lock_guard<std::mutex> lk(g_mm_mu);
  for (mm_timer* t : g_mm_timers) {
    if (!t) continue;
    t->dead.store(1, std::memory_order_release);
    if (t->th.joinable()) t->th.detach();
  }
  g_mm_timers.clear();
}

} // namespace

extern "C" {

// ---- timers ----

u32 TRT_GUEST_ABI trt_timeGetTime(void) { return now_ms(); }

u32 TRT_GUEST_ABI trt_timeBeginPeriod(u32 period_ms) {
  if (period_ms && (g_min_period == 0 || period_ms < g_min_period))
    g_min_period = period_ms;
  // TIMERR_NOERROR
  return 0;
}

u32 TRT_GUEST_ABI trt_timeEndPeriod(u32) { return 0; }

u32 TRT_GUEST_ABI trt_timeGetDevCaps(void* caps, u32 size) {
  if (!caps || size < 8) return 2 /* TIMERR_STRUCT */;
  u8* p = (u8*)caps;
  memset(p, 0, size);
  // TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; }
  *(u32*)(p + 0) = 1;      // wPeriodMin
  *(u32*)(p + 4) = 1000;   // wPeriodMax
  return 0;
}

// timeSetEvent/timeKillEvent/timeGetSystemTime: the multimedia timer queue
// (OpenTTD's scheduler tick). Real host threads, see mm_timer_* above.
u32 TRT_GUEST_ABI trt_timeSetEvent(u32 period_ms, u32 resolution, u64 cb,
                                   u64 user, u32 flags) {
  (void)resolution;
  return mm_timer_set(period_ms, cb, user, flags);
}

u32 TRT_GUEST_ABI trt_timeKillEvent(u32 id) {
  mm_timer_kill(id);
  return 0;  // TIMERR_NOERROR
}

// ---- legacy audio device probes: "no devices", never a crash ----

u32 TRT_GUEST_ABI trt_waveOutGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_waveOutGetDevCapsW(u64, void*, u32) { return 4 /* MMSYSERR_BADDEVICEID */; }

u32 TRT_GUEST_ABI trt_midiOutGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_joyGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_joyGetPosEx(u32, void*) { return 167 /* JOYERR_UNPLUGGED */; }

// ---- mciSendStringW: engines probe codecs with it; answer "cannot find
// the device" (MCIERR_INVALID_DEVICE_NAME = 263) visibly rather than crash ----

u32 TRT_GUEST_ABI trt_mciSendStringW(const u16* cmd, u16* ret, u32 ret_len, void*) {
  char line[192];
  char u8[96];
  size_t n = 0;
  for (size_t i = 0; cmd && cmd[i] && n + 1 < sizeof(u8); ++i)
    u8[n++] = (cmd[i] >= 0x20 && cmd[i] < 0x7F) ? (char)cmd[i] : '?';
  u8[n] = 0;
  snprintf(line, sizeof(line), "winmm: mciSendStringW(\"%s\") -> MCIERR_INVALID_DEVICE_NAME", u8);
  trt_core_log(line);
  (void)ret; (void)ret_len;
  return 263;
}

// ---- media key helpers some engines import ----

u32 TRT_GUEST_ABI trt_GetProfileStringW(const u16* app, const u16* key,
                                      const u16* def, u16* out, u32 out_len) {
  if (!out || out_len == 0) return 0;
  out[0] = 0;
  (void)app; (void)key;
  // return the caller's default (copy min(def, out_len-1) UTF-16 chars)
  if (!def) return 0;
  u32 n = 0;
  while (def[n] && n + 1 < out_len) { out[n] = def[n]; ++n; }
  out[n] = 0;
  return n;
}

u32 TRT_GUEST_ABI trt_PlaySoundW(const u16* name, void*, u32 flags) {
  char line[128];
  snprintf(line, sizeof(line), "winmm: PlaySoundW(flags=%u) -> no legacy audio, ignored", flags);
  trt_core_log(line);
  (void)name;
  return 1;  // pretend handled so the engine does not retry forever
}

// ---- waveIn/waveOut full surface (UnityPlayer imports it; the console
// has NO legacy wave devices — every call answers "no devices" honestly:
// MMSYSERR_BADDEVICEID (4) / NDRIVER (6), the API's own named failure) ----
u32 TRT_GUEST_ABI trt_waveInGetNumDevs(void) { return 0; }
u32 TRT_GUEST_ABI trt_waveInGetDevCapsW(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveInGetDevCapsA(u64 h, void* caps, u32 c) {
  return trt_waveInGetDevCapsW(h, caps, c);  // ANSI twin, same "no devices"
}
u32 TRT_GUEST_ABI trt_waveInOpen(u64* handle, u32, u64, u64, u64, u32) {
  if (handle) *handle = 0;
  return 6;  // MMSYSERR_NODRIVER
}
u32 TRT_GUEST_ABI trt_waveInClose(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveInPrepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveInUnprepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveInAddBuffer(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveInReset(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveInStart(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutGetPosition(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutReset(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutWrite(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutClose(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutOpen(u64* handle, u32, u64, u64, u64, u32) {
  if (handle) *handle = 0;
  return 6;  // MMSYSERR_NODRIVER
}
u32 TRT_GUEST_ABI trt_waveOutUnprepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutPrepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutGetDevCapsA(u64, void*, u32) { return 4; }
// waveOutMessage: device-specific driver message. With no legacy wave
// devices there is no driver to receive it — MMSYSERR_BADDEVICEID (4),
// the API's own named failure, same as the rest of the waveOut surface.
u32 TRT_GUEST_ABI trt_waveOutMessage(u64, u32, u64, u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutBreakLoop(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutRestart(u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutPause(u64) { return 4; }
// waveInMessage / waveOutMessage twins; GetErrorText* answer "no such
// error for an invalid device" with the API's own bad-device code.
u32 TRT_GUEST_ABI trt_waveInMessage(u64, u32, u64, u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutGetErrorTextW(u32 err, u16* buf, u32 len) {
  (void)err; (void)buf; (void)len;
  return 4;  // MMSYSERR_BADDEVICEID
}
u32 TRT_GUEST_ABI trt_waveInGetErrorTextW(u32 err, u16* buf, u32 len) {
  (void)err; (void)buf; (void)len;
  return 4;
}
u32 TRT_GUEST_ABI trt_midiInGetNumDevs(void) { return 0; }
u32 TRT_GUEST_ABI trt_midiInOpen(void** ph, u32 dev, u64 cb, u64 inst, u32 fl) {
  (void)dev; (void)cb; (void)inst; (void)fl;
  if (ph) *ph = nullptr;
  return 6;  // MMSYSERR_NODRIVER
}
u32 TRT_GUEST_ABI trt_midiInHandleOp(void*) { return 4; /*MMSYSERR_INVALHANDLE*/ }
u32 TRT_GUEST_ABI trt_midiInGetDevCapsA(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_midiInGetErrorTextA(u32 err, char* buf, u32 len) {
  (void)err;
  if (buf && len > 0) buf[0] = 0;
  return 8;  // MMSYSERR_BADERRNUM
}

// midiOut* device-cap probes: 0 devices (MIDIOUTCAPS untouched), the same
// "no legacy MIDI on console" answer as midiOutGetNumDevs. OpenTTD probes
// midiOutGetDevCapsW during its sound-backend sweep.
u32 TRT_GUEST_ABI trt_midiOutGetDevCapsA(u64, void*, u32) { return 4; }  // MMSYSERR_BADDEVICEID
u32 TRT_GUEST_ABI trt_midiOutGetDevCapsW(u64, void*, u32) { return 4; }

// midiOutOpen: with 0 MIDI devices Windows returns MMSYSERR_BADDEVICEID (4)
// and leaves the handle untouched — OpenTTD's device sweep treats that as
// "no MIDI, use the next backend", exactly the console answer.
u32 TRT_GUEST_ABI trt_midiOutOpen(u64* handle, u64, u64, u64, u32) {
  if (handle) *handle = 0;
  return 4;  // MMSYSERR_BADDEVICEID
}
// The rest of the midiOut family on a no-device host: every handle-based
// call answers MMSYSERR_BADDEVICEID (MMSYSERR_NOCANDO for prepare/unprepare
// is 6; BADDEVICEID 4 is what a NULL/closed handle produces and is what
// callers test for after a failed open).
u32 TRT_GUEST_ABI trt_midiOutClose(u64) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutReset(u64) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutShortMsg(u64, u32) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutLongMsg(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutPrepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutUnprepareHeader(u64, void*, u32) { return 4; }
u32 TRT_GUEST_ABI trt_midiOutGetErrorTextA(u32 err, char* buf, u32 len) {
  (void)err;
  if (buf && len > 0) buf[0] = 0;
  return 8;  // MMSYSERR_BADERRNUM
}
u32 TRT_GUEST_ABI trt_midiOutGetErrorTextW(u32 err, u16* buf, u32 len) {
  (void)err;
  if (buf && len > 0) buf[0] = 0;
  return 8;
}

int trt_bridge_register_winmm(void) {
  static const trt_named_fn k_fns[] = {
      {"timeGetTime", (void*)&trt_timeGetTime},
      {"timeBeginPeriod", (void*)&trt_timeBeginPeriod},
      {"timeEndPeriod", (void*)&trt_timeEndPeriod},
      {"timeGetDevCaps", (void*)&trt_timeGetDevCaps},
      {"timeSetEvent", (void*)&trt_timeSetEvent},
      {"timeKillEvent", (void*)&trt_timeKillEvent},
      {"waveOutGetNumDevs", (void*)&trt_waveOutGetNumDevs},
      {"waveOutGetDevCapsW", (void*)&trt_waveOutGetDevCapsW},
      {"waveInGetNumDevs", (void*)&trt_waveInGetNumDevs},
      {"waveInGetDevCapsW", (void*)&trt_waveInGetDevCapsW},
      {"waveInGetDevCapsA", (void*)&trt_waveInGetDevCapsA},
      {"waveInOpen", (void*)&trt_waveInOpen},
      {"waveInClose", (void*)&trt_waveInClose},
      {"waveInPrepareHeader", (void*)&trt_waveInPrepareHeader},
      {"waveInUnprepareHeader", (void*)&trt_waveInUnprepareHeader},
      {"waveInAddBuffer", (void*)&trt_waveInAddBuffer},
      {"waveInReset", (void*)&trt_waveInReset},
      {"waveInStart", (void*)&trt_waveInStart},
      {"waveOutGetPosition", (void*)&trt_waveOutGetPosition},
      {"waveOutReset", (void*)&trt_waveOutReset},
      {"waveOutWrite", (void*)&trt_waveOutWrite},
      {"waveOutClose", (void*)&trt_waveOutClose},
      {"waveOutOpen", (void*)&trt_waveOutOpen},
      {"waveOutPrepareHeader", (void*)&trt_waveOutPrepareHeader},
      {"waveOutUnprepareHeader", (void*)&trt_waveOutUnprepareHeader},
      {"waveOutGetDevCapsA", (void*)&trt_waveOutGetDevCapsA},
      {"waveOutMessage", (void*)&trt_waveOutMessage},
      {"waveOutBreakLoop", (void*)&trt_waveOutBreakLoop},
      {"waveOutRestart", (void*)&trt_waveOutRestart},
      {"waveOutPause", (void*)&trt_waveOutPause},
      {"waveInMessage", (void*)&trt_waveInMessage},
      {"waveOutGetErrorTextW", (void*)&trt_waveOutGetErrorTextW},
      {"waveOutGetErrorTextA", (void*)&trt_waveOutGetErrorTextW},
      {"waveInGetErrorTextW", (void*)&trt_waveInGetErrorTextW},
      {"waveInGetErrorTextA", (void*)&trt_waveInGetErrorTextW},
      {"midiOutGetNumDevs", (void*)&trt_midiOutGetNumDevs},
      {"midiOutGetDevCapsA", (void*)&trt_midiOutGetDevCapsA},
      {"midiOutGetDevCapsW", (void*)&trt_midiOutGetDevCapsW},
      {"midiOutGetErrorTextA", (void*)&trt_midiOutGetErrorTextA},
      {"midiOutGetErrorTextW", (void*)&trt_midiOutGetErrorTextW},
      {"midiOutOpen", (void*)&trt_midiOutOpen},
      {"midiOutClose", (void*)&trt_midiOutClose},
      {"midiOutReset", (void*)&trt_midiOutReset},
      {"midiOutShortMsg", (void*)&trt_midiOutShortMsg},
      {"midiOutLongMsg", (void*)&trt_midiOutLongMsg},
      {"midiOutPrepareHeader", (void*)&trt_midiOutPrepareHeader},
      {"midiOutUnprepareHeader", (void*)&trt_midiOutUnprepareHeader},
      {"midiInGetNumDevs", (void*)&trt_midiInGetNumDevs},
      {"midiInOpen", (void*)&trt_midiInOpen},
      {"midiInClose", (void*)&trt_midiInHandleOp},
      {"midiInStart", (void*)&trt_midiInHandleOp},
      {"midiInStop", (void*)&trt_midiInHandleOp},
      {"midiInReset", (void*)&trt_midiInHandleOp},
      {"midiInGetDevCapsA", (void*)&trt_midiInGetDevCapsA},
      {"midiInGetDevCapsW", (void*)&trt_midiInGetDevCapsA},
      {"midiInGetErrorTextA", (void*)&trt_midiInGetErrorTextA},
      {"joyGetNumDevs", (void*)&trt_joyGetNumDevs},
      {"joyGetPosEx", (void*)&trt_joyGetPosEx},
      {"mciSendStringW", (void*)&trt_mciSendStringW},
      {"GetProfileStringW", (void*)&trt_GetProfileStringW},
      {"PlaySoundW", (void*)&trt_PlaySoundW},
  };
  trt_module_register("winmm.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  // winmm is served through the api-ms-win-mm apiset too (apiset.cpp maps
  // the contract here; registering the concrete name is the load path)
  trt_module_register("winmmbase.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
