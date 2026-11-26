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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
// timeBeginPeriod/timeEndPeriod are desktop-partition in the SDK headers
// (not declared under WINAPI_FAMILY_APP), but the functions are callable
// from the AppContainer at runtime — declare them directly.
extern "C" unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);
extern "C" unsigned int __stdcall timeEndPeriod(unsigned int uPeriod);
#  pragma comment(lib, "winmm.lib")
#endif

namespace {

// timeGetTime: ms since boot, wrapping at 2^32 like Windows.
u32 now_ms() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);  // portable across Windows/UWP and Linux
  return (u32)((u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000);
}

// the requested period (ms), for visibility in logs
u32 g_min_period = 0;

} // namespace

extern "C" {

// ---- timers ----

u32 TRT_GUEST_ABI trt_timeGetTime(void) { return now_ms(); }

u32 TRT_GUEST_ABI trt_timeBeginPeriod(u32 period_ms) {
  if (period_ms && (g_min_period == 0 || period_ms < g_min_period))
    g_min_period = period_ms;
#if defined(_WIN32)
  // forward to the real host API: the guest's Sleep(1) / timer granularity
  // actually becomes 1ms instead of the default ~15.6ms
  return (u32)::timeBeginPeriod(period_ms);  // TIMERR_NOERROR == 0
#else
  // no host timer API on the Linux harness: record-and-return
  return 0;  // TIMERR_NOERROR
#endif
}

u32 TRT_GUEST_ABI trt_timeEndPeriod(u32 period_ms) {
#if defined(_WIN32)
  return (u32)::timeEndPeriod(period_ms);
#else
  (void)period_ms;
  return 0;  // TIMERR_NOERROR
#endif
}

u32 TRT_GUEST_ABI trt_timeGetDevCaps(void* caps, u32 size) {
  if (!caps || size < 8) return 2 /* TIMERR_STRUCT */;
  u8* p = (u8*)caps;
  memset(p, 0, size);
  // TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; }
  *(u32*)(p + 0) = 1;      // wPeriodMin
  *(u32*)(p + 4) = 1000;   // wPeriodMax
  return 0;
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

// ---- MCI command interface (.154): fmodex64.dll imports
// mciSendCommandA; without it the DLL fails to map and the game
// cannot load. No MCI devices exist on the console, so every call
// answers with the API's own named failure and the engine falls back.

u32 TRT_GUEST_ABI trt_mciSendCommandA(u32 mci_id, u32 msg, u64 p1, u64 p2) {
  char line[192];
  snprintf(line, sizeof(line),
           "winmm: mciSendCommandA(id=%u msg=0x%x) -> MCIERR_INVALID_DEVICE_ID",
           mci_id, msg);
  trt_core_log(line);
  (void)p1; (void)p2;
  return 257;  // MCIERR_INVALID_DEVICE_ID
}

u32 TRT_GUEST_ABI trt_mciSendCommandW(u32 mci_id, u32 msg, u64 p1, u64 p2) {
  char line[192];
  snprintf(line, sizeof(line),
           "winmm: mciSendCommandW(id=%u msg=0x%x) -> MCIERR_INVALID_DEVICE_ID",
           mci_id, msg);
  trt_core_log(line);
  (void)p1; (void)p2;
  return 257;  // MCIERR_INVALID_DEVICE_ID
}

u32 TRT_GUEST_ABI trt_mciSendStringA(const char* cmd, char* ret, u32 ret_len,
                                     void* hwnd) {
  char line[192];
  snprintf(line, sizeof(line), "winmm: mciSendStringA(\"%.80s\") -> "
           "MCIERR_INVALID_DEVICE_NAME", cmd ? cmd : "");
  trt_core_log(line);
  (void)ret; (void)ret_len; (void)hwnd;
  return 263;  // MCIERR_INVALID_DEVICE_NAME
}

u32 TRT_GUEST_ABI trt_mciGetErrorStringA(u32 err, char* buf, u32 len) {
  (void)err;
  if (buf && len > 0) buf[0] = 0;
  return 0;  // FALSE: no error text available
}

u32 TRT_GUEST_ABI trt_mciGetErrorStringW(u32 err, u16* buf, u32 len) {
  (void)err;
  if (buf && len > 0) buf[0] = 0;
  return 0;  // FALSE: no error text available
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
// waveOutSetVolume: no legacy wave device exists, so there is no volume
// to set; answer the API own named failure like the rest of the
// waveOut surface (Sekiro audit: bink2w64.dll imports it).
u32 TRT_GUEST_ABI trt_waveOutSetVolume(u64, u32) { return 4; }
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

int trt_bridge_register_winmm(void) {
  static const trt_named_fn k_fns[] = {
      {"timeGetTime", (void*)&trt_timeGetTime},
      {"timeBeginPeriod", (void*)&trt_timeBeginPeriod},
      {"timeEndPeriod", (void*)&trt_timeEndPeriod},
      {"timeGetDevCaps", (void*)&trt_timeGetDevCaps},
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
      {"waveOutSetVolume", (void*)&trt_waveOutSetVolume},
      {"waveInMessage", (void*)&trt_waveInMessage},
      {"waveOutGetErrorTextW", (void*)&trt_waveOutGetErrorTextW},
      {"waveOutGetErrorTextA", (void*)&trt_waveOutGetErrorTextW},
      {"waveInGetErrorTextW", (void*)&trt_waveInGetErrorTextW},
      {"waveInGetErrorTextA", (void*)&trt_waveInGetErrorTextW},
      {"midiOutGetNumDevs", (void*)&trt_midiOutGetNumDevs},
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
      {"mciSendCommandA", (void*)&trt_mciSendCommandA},
      {"mciSendCommandW", (void*)&trt_mciSendCommandW},
      {"mciSendStringA", (void*)&trt_mciSendStringA},
      {"mciGetErrorStringA", (void*)&trt_mciGetErrorStringA},
      {"mciGetErrorStringW", (void*)&trt_mciGetErrorStringW},
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
