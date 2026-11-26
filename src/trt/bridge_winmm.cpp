// tiramisu/src/trt/bridge_winmm.cpp
// winmm.dll for the guest — the multimedia clock.
//
// Nearly every game pulls its frame timing from winmm!timeGetTime (even
// XAudio2-era engines keep it as the scheduler clock), and startup often
// calls timeBeginPeriod to raise the timer resolution. Without this bridge
// those calls hit a fail-visible STUB and the game either spins at 100%
// waiting on a clock that never moves or aborts with a named error.
//
// waveOut and waveIn are each one real device over the console's audio path;
// midiOut* still answers "no legacy device" instead of failing, because there
// is no MIDI synthesizer to feed and pretending otherwise would be a lie.
#include "bridge_winmm.h"
#include "trt.h"
#include "thunk.h"
#include "images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Signalling a kernel event from a bridge worker thread is the established
// pattern (WASAPI, APCs, the timer queue all do it).
extern "C" int TRT_STDCALL trt_SetEvent(void* h);

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

// ---- multimedia timers ---------------------------------------------------
// timeSetEvent used to return 0 ("no timer exists") because entering guest
// code from a host thread looked unsafe. It is not: trt_call_guest is the same
// host->guest boundary the APC, FLS and timer-queue paths already cross, and an
// event callback is trt_SetEvent, which every other bridge worker thread uses.
// So each timer is a real thread that sleeps to its deadline and fires.
//
// Ownership is shared_ptr because a timer may be killed from inside its own
// callback (TIME_KILL_SYNCHRONOUS from the firing thread); the thread then
// detaches and the last reference dies with the callback frame.
struct mm_timer {
  u32 id = 0;
  std::thread th;
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
};

std::mutex g_timers_mu;
std::vector<std::shared_ptr<mm_timer>> g_timers;
std::atomic<u32> g_next_timer_id{1};
std::atomic<unsigned long long> g_timer_fires{0};

// gs:0x58 / TEB cleanup is irrelevant here; the callback runs on this host
// thread exactly like every other bridge-initiated guest call.
u64 mm_timer_fire(void* cb, u64 user, u32 flags) {
  const bool use_event = (flags & 0x0030u) != 0;  // EVENT_SET or EVENT_PULSE
  ++g_timer_fires;
  if (use_event) {
    if (cb) trt_SetEvent(cb);
    return 0;
  }
  return trt_call_guest(cb, user, 0, 0, 0);
}

void mm_timer_body(std::shared_ptr<mm_timer> t, u32 delay, u32 res, void* cb,
                   u64 user, u32 flags) {
  const bool periodic = (flags & 0x0001u) != 0;  // TIME_PERIODIC
  const u32 period = delay ? delay : (res ? res : 1);
  // Wake early only to observe a kill; a 1ms slice keeps a 500ms timer no
  // more responsive than it needs to be while a kill still lands promptly.
  const u32 slice_ms = period < 5 ? period : 5;
  std::unique_lock<std::mutex> lk(t->mu);
  for (;;) {
    u32 remaining = period;
    while (remaining) {
      const u32 slice = remaining < slice_ms ? remaining : slice_ms;
      if (t->cv.wait_for(lk, std::chrono::milliseconds(slice),
                         [&t] { return t->stop; }))
        return;
      remaining -= slice;
    }
    if (t->stop) return;
    lk.unlock();
    mm_timer_fire(cb, user, flags);
    lk.lock();
    if (!periodic) return;
  }
}

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

#include "bridge_audio.h"

// ---- waveOut: one REAL output device ------------------------------------
// This used to answer "0 devices", which makes a title disable audio entirely.
// It was the most common gap in the whole corpus (7 PEs import these). The
// console has exactly one real output path - the XAudio2 sink that the WASAPI
// bridge already feeds - so waveOut is a thin, honest adapter over it: one
// device (id 0), real device caps, and waveOutWrite handing PCM to the sink.
namespace {
struct waveout_state {
  bool open = false;
  u32 rate = 48000;
  u32 channels = 2;
  u32 bits = 16;
  u32 volume = 0xFFFFFFFFu;
  u64 frames = 0;
};
waveout_state g_waveout;
// Never 0 and never a host pointer: a guest cannot mistake it for memory.
constexpr u64 kWaveOutHandle = 0x574F000000000001ull;
constexpr u32 kWhdrDone = 0x01;
constexpr u32 kWhdrPrepared = 0x02;
constexpr u32 kWhdrInQueue = 0x10;

// WAVEHDR layout depends on the guest's bitness and the cbWH argument is the
// only reliable discriminator. Only the 64-bit layout is parsed: a PE32 lpData
// is a 32-bit guest VA, and dereferencing it on a guess risks reading host
// memory that is not the buffer. Wrong silence beats wrong memory.
bool wavehdr64(const void* hdr, u32 cb, const void** data, u32* bytes, u32* flags) {
  if (!hdr || cb != 48) return false;
  const u8* p = (const u8*)hdr;
  *data = *(const void* const*)p;
  *bytes = *(const u32*)(p + 8);
  *flags = *(const u32*)(p + 24);
  return true;
}
void wavehdr_set_flags(void* hdr, u32 cb, u32 flags) {
  if (!hdr || cb != 48) return;
  *(u32*)((u8*)hdr + 24) = flags;
}
void waveout_submit(const void* pcm, u32 bytes) {
  if (!pcm || bytes == 0) return;
  // Position accounting tracks what waveOut was HANDED, not whether a sink
  // happened to be installed: getPosition must keep advancing offline and in
  // tests, exactly as a real device's sample counter would.
  const u32 frame_bytes = g_waveout.channels * (g_waveout.bits / 8);
  if (frame_bytes) g_waveout.frames += bytes / frame_bytes;
  void (*submit)(const void*, u32, u32, u32, u32, void*) = nullptr;
  void* user = nullptr;
  trt_audio_get_hooks(&submit, &user);
  if (!submit) return;
  submit(pcm, bytes, g_waveout.rate, g_waveout.channels, g_waveout.bits, user);
}
}  // namespace

u32 TRT_GUEST_ABI trt_waveOutGetNumDevs(void) { return 1; }

u32 TRT_GUEST_ABI trt_waveOutGetDevCapsW(u64 dev, void* caps, u32 size) {
  // Device 0 is us; WAVE_MAPPER ((UINT)-1) also resolves to the only device.
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;  // MMSYSERR_BADDEVICEID
  if (!caps || size < 84) return 11;               // MMSYSERR_INVALPARAM
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;       // wMid
  *(u16*)(p + 2) = 1;       // wPid
  *(u32*)(p + 4) = 0x0100;  // vDriverVersion 1.0
  const char* name = "Tiramisu WaveOut (XAudio2 sink)";
  u16* nm = (u16*)(p + 8);  // szPname[MAXPNAMELEN=32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = (u16)(u8)name[i];
  *(u32*)(p + 72) = 0x880;  // WAVE_FORMAT_2S16 | WAVE_FORMAT_4S16
  *(u16*)(p + 76) = 2;      // wChannels
  *(u32*)(p + 80) = 0;      // dwSupport
  return 0;                 // MMSYSERR_NOERROR
}

u32 TRT_GUEST_ABI trt_waveOutGetDevCapsA(u64 dev, void* caps, u32 size) {
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;
  if (!caps || size < 52) return 11;
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;
  *(u16*)(p + 2) = 1;
  *(u32*)(p + 4) = 0x0100;
  const char* name = "Tiramisu WaveOut (XAudio2 sink)";
  char* nm = (char*)(p + 8);  // szPname[32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = name[i];
  *(u32*)(p + 40) = 0x880;
  *(u16*)(p + 44) = 2;
  *(u32*)(p + 48) = 0;
  return 0;
}

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

u32 TRT_GUEST_ABI trt_mciSendCommandA(u32 dev, u32 msg, u64 p1, u64 p2) {
  (void)dev; (void)msg; (void)p1; (void)p2;
  return 263;  // MCIERR_INVALID_DEVICE_NAME
}

u32 TRT_GUEST_ABI trt_mciSendCommandW(u32 dev, u32 msg, u64 p1, u64 p2) {
  (void)dev; (void)msg; (void)p1; (void)p2;
  return 263;  // MCIERR_INVALID_DEVICE_NAME
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

// ---- waveIn: one REAL capture device ------------------------------------
// waveInGetNumDevs()==0 was the most-imported winmm gap in the whole corpus
// (7 PEs) and it makes engines disable voice capture entirely - a mic-driven
// game then loses a feature that the console itself supports through a
// headset. So there is one device, with real caps and the real WAVEHDR
// contract: prepare -> addBuffer -> the buffer comes back WHDR_DONE carrying
// dwBytesRecorded bytes.
//
// Audio comes from the host when it installs a capture source
// (trt_audio_capture_read). Without one the device records silence, which is
// what a machine with no microphone attached produces:
// a completed buffer, not a missing device.
namespace {
struct wavein_state {
  bool open = false;
  u32 rate = 22050;
  u32 channels = 1;
  u32 bits = 16;
  u64 frames = 0;
};
wavein_state g_wavein;
constexpr u64 kWaveInHandle = 0x5749000000000001ull;

// Weak so hosts and test binaries without a capture backend resolve it to null
// and the device stays silent instead of failing to link.
extern "C" u32 trt_audio_capture_read(void* buf, u32 frames, u32 rate,
                                      u32 channels, u32 bits)
    __attribute__((weak));

void wavein_fill(void* buf, u32 frames) {
  if (!buf || !frames) return;
  const u32 frame_bytes = g_wavein.channels * (g_wavein.bits / 8);
  memset(buf, 0, (size_t)frames * frame_bytes);
  if (trt_audio_capture_read)
    trt_audio_capture_read(buf, frames, g_wavein.rate, g_wavein.channels,
                           g_wavein.bits);
}
}  // namespace

u32 TRT_GUEST_ABI trt_waveInGetNumDevs(void) { return 1; }
u32 TRT_GUEST_ABI trt_waveInGetDevCapsW(u64 dev, void* caps, u32 size) {
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;  // MMSYSERR_BADDEVICEID
  if (!caps || size < 80) return 11;               // MMSYSERR_INVALPARAM
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;       // wMid
  *(u16*)(p + 2) = 1;       // wPid
  *(u32*)(p + 4) = 0x0100;  // vDriverVersion 1.0
  const char* name = "Tiramisu WaveIn (capture)";
  u16* nm = (u16*)(p + 8);  // szPname[MAXPNAMELEN=32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = (u16)(u8)name[i];
  *(u32*)(p + 72) = 0x880;  // WAVE_FORMAT_2M16 | WAVE_FORMAT_4M16
  *(u16*)(p + 76) = 1;      // wChannels
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInGetDevCapsA(u64 dev, void* caps, u32 size) {
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;
  if (!caps || size < 48) return 11;
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;
  *(u16*)(p + 2) = 1;
  *(u32*)(p + 4) = 0x0100;
  const char* name = "Tiramisu WaveIn (capture)";
  char* nm = (char*)(p + 8);  // szPname[32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = name[i];
  *(u32*)(p + 40) = 0x880;
  *(u16*)(p + 44) = 1;
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInOpen(u64* handle, u32 dev, u64 fmt, u64 cb,
                                 u64 inst, u32 flags) {
  (void)cb; (void)inst; (void)flags;
  if (!handle) return 11;  // MMSYSERR_INVALPARAM
  *handle = 0;
  if (dev != 0 && dev != 0xFFFFFFFFu) return 2;
  if (fmt) {
    const u8* f = (const u8*)fmt;
    const u16 tag = *(const u16*)f;
    const u16 ch = *(const u16*)(f + 2);
    const u32 rate = *(const u32*)(f + 4);
    const u16 bits = *(const u16*)(f + 14);
    if (tag != 1) {  // the capture path is PCM, exactly like the sink
      char line[128];
      snprintf(line, sizeof(line),
               "winmm: waveInOpen(format tag 0x%x) -> MMSYSERR_NOTSUPPORTED "
               "(PCM only)", tag);
      trt_core_log(line);
      return 32;
    }
    if (ch == 0 || ch > 2) return 11;
    g_wavein.channels = ch;
    if (rate >= 8000 && rate <= 192000) g_wavein.rate = rate;
    g_wavein.bits = (bits == 8 || bits == 16) ? bits : 16;
  }
  g_wavein.open = true;
  g_wavein.frames = 0;
  *handle = kWaveInHandle;
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInClose(u64 hwi) {
  if (hwi != kWaveInHandle) return 4;   // MMSYSERR_INVALHANDLE
  if (!g_wavein.open) return 4;         // already closed
  g_wavein.open = false;
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInPrepareHeader(u64 hwi, void* hdr, u32 cb) {
  if (hwi != kWaveInHandle || !g_wavein.open) return 4;
  u32 flags = 0;
  if (hdr && cb == 48) flags = *(u32*)((u8*)hdr + 24);
  wavehdr_set_flags(hdr, cb, flags | kWhdrPrepared);
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInUnprepareHeader(u64 hwi, void* hdr, u32 cb) {
  if (hwi != kWaveInHandle) return 4;
  u32 flags = 0;
  if (hdr && cb == 48) flags = *(u32*)((u8*)hdr + 24);
  wavehdr_set_flags(hdr, cb, flags & ~(kWhdrPrepared | kWhdrDone | kWhdrInQueue));
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInAddBuffer(u64 hwi, void* hdr, u32 cb) {
  if (hwi != kWaveInHandle || !g_wavein.open) return 4;
  const void* data = nullptr;
  u32 bytes = 0, flags = 0;
  if (!wavehdr64(hdr, cb, &data, &bytes, &flags)) {
    static unsigned s_logged = 0;
    if (++s_logged <= 3)
      trt_core_log("winmm: waveInAddBuffer: WAVEHDR not parsed (cbwh != 48); "
                   "refused rather than guessed");
    return 11;  // MMSYSERR_INVALPARAM
  }
  if (!data || !bytes) return 11;
  const u32 frame_bytes = g_wavein.channels * (g_wavein.bits / 8);
  const u32 frames = frame_bytes ? bytes / frame_bytes : 0;
  wavein_fill((void*)data, frames);
  g_wavein.frames += frames;
  // Full-buffer capture: the guest is told it has `bytes` of recorded audio,
  // which is what a real driver does when the buffer was filled completely.
  if (cb == 48) *(u32*)((u8*)hdr + 12) = frames * frame_bytes;  // dwBytesRecorded
  wavehdr_set_flags(hdr, cb, (flags | kWhdrDone) & ~kWhdrInQueue);
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInReset(u64 hwi) {
  if (hwi != kWaveInHandle) return 4;
  g_wavein.frames = 0;
  return 0;
}
u32 TRT_GUEST_ABI trt_waveInStart(u64 hwi) {
  return (hwi == kWaveInHandle && g_wavein.open) ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveInStop(u64 hwi) {
  return (hwi == kWaveInHandle && g_wavein.open) ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveOutGetPosition(u64 hwo, void* time, u32 size) {
  if (hwo != kWaveOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  if (time && size >= 12) {
    // MMTIME: wType@0, then the union. Report TIME_SAMPLES (0x2) played so far.
    memset(time, 0, size);
    *(u32*)((u8*)time + 0) = 0x2;
    *(u32*)((u8*)time + 4) = (u32)g_waveout.frames;
  }
  return 0;
}
u32 TRT_GUEST_ABI trt_waveOutReset(u64 hwo) {
  return hwo == kWaveOutHandle ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveOutWrite(u64 hwo, void* hdr, u32 cb) {
  if (hwo != kWaveOutHandle || !g_waveout.open) return 4;
  const void* data = nullptr;
  u32 bytes = 0, flags = 0;
  if (!wavehdr64(hdr, cb, &data, &bytes, &flags)) {
    static unsigned s_logged = 0;
    if (++s_logged <= 3)
      trt_core_log("winmm: waveOutWrite: WAVEHDR not parsed (cbwh != 48); "
                   "counted as silence rather than guessed");
    return 0;
  }
  waveout_submit(data, bytes);
  wavehdr_set_flags(hdr, cb, (flags | kWhdrDone) & ~kWhdrInQueue);
  return 0;
}
u32 TRT_GUEST_ABI trt_waveOutClose(u64 hwo) {
  if (hwo != kWaveOutHandle) return 4;
  if (!g_waveout.open) return 4;  // MMSYSERR_INVALHANDLE: already closed
  g_waveout.open = false;
  return 0;
}
u32 TRT_GUEST_ABI trt_waveOutOpen(u64* handle, u32 dev, u64 fmt, u64 cb,
                                  u64 inst, u32 flags) {
  (void)cb; (void)inst; (void)flags;
  if (!handle) return 11;  // MMSYSERR_INVALPARAM
  *handle = 0;
  if (dev != 0 && dev != 0xFFFFFFFFu) return 2;  // MMSYSERR_BADDEVICEID
  if (fmt) {
    // WAVEFORMATEX: wFormatTag@0, nChannels@2, nSamplesPerSec@4,
    // nAvgBytesPerSec@8, nBlockAlign@12, wBitsPerSample@14, cbSize@16
    const u8* f = (const u8*)fmt;
    const u16 tag = *(const u16*)f;
    const u16 ch = *(const u16*)(f + 2);
    const u32 rate = *(const u32*)(f + 4);
    const u16 bits = *(const u16*)(f + 14);
    if (tag != 1) {  // WAVE_FORMAT_PCM is the only thing the sink takes
      char line[128];
      snprintf(line, sizeof(line),
               "winmm: waveOutOpen(format tag 0x%x) -> MMSYSERR_NOTSUPPORTED "
               "(PCM only)", tag);
      trt_core_log(line);
      return 32;
    }
    if (ch == 1 || ch == 2) g_waveout.channels = ch;
    if (rate >= 8000 && rate <= 192000) g_waveout.rate = rate;
    if (bits == 8 || bits == 16) g_waveout.bits = bits;
  }
  g_waveout.open = true;
  *handle = kWaveOutHandle;
  return 0;  // MMSYSERR_NOERROR
}
u32 TRT_GUEST_ABI trt_waveOutUnprepareHeader(u64 hwo, void* hdr, u32 cb) {
  if (hwo != kWaveOutHandle) return 4;
  u32 flags = 0;
  if (hdr && cb == 48) flags = *(u32*)((u8*)hdr + 24);
  wavehdr_set_flags(hdr, cb, flags & ~(kWhdrPrepared | kWhdrDone | kWhdrInQueue));
  return 0;
}
u32 TRT_GUEST_ABI trt_waveOutPrepareHeader(u64 hwo, void* hdr, u32 cb) {
  if (hwo != kWaveOutHandle) return 4;
  u32 flags = 0;
  if (hdr && cb == 48) flags = *(u32*)((u8*)hdr + 24);
  wavehdr_set_flags(hdr, cb, flags | kWhdrPrepared);
  return 0;
}
// waveOutMessage: device-specific driver message. With no legacy wave
// devices there is no driver to receive it — MMSYSERR_BADDEVICEID (4),
// the API's own named failure, same as the rest of the waveOut surface.
u32 TRT_GUEST_ABI trt_waveOutMessage(u64, u32, u64, u64) { return 4; }
u32 TRT_GUEST_ABI trt_waveOutBreakLoop(u64 hwo) {
  return hwo == kWaveOutHandle ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveOutRestart(u64 hwo) {
  return hwo == kWaveOutHandle ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveOutPause(u64 hwo) {
  return hwo == kWaveOutHandle ? 0 : 4;
}
u32 TRT_GUEST_ABI trt_waveOutSetVolume(u64 dev, u32 vol) {
  if (dev != kWaveOutHandle && dev != 0 && dev != 0xFFFFFFFFull) return 4;
  g_waveout.volume = vol;
  return 0;  // MMSYSERR_NOERROR
}
u32 TRT_GUEST_ABI trt_waveOutGetVolume(u64 dev, u32* vol) {
  if (dev != kWaveOutHandle && dev != 0 && dev != 0xFFFFFFFFull) return 4;
  if (vol) *vol = g_waveout.volume;
  return 0;  // MMSYSERR_NOERROR
}
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

// ---- MIDI output: one REAL virtual synthesizer device ---------------------
// Many engines and ports probe midiOutGetNumDevs(). If it returns 0, they
// disable audio or fail to start. We virtualize device 0 as a software
// synthesizer ("Tiramisu Virtual MIDI Synth"): device caps, handle lifecycle,
// volume control, and message dispatch all succeed with standard WinMM codes.

namespace {
struct midiout_state {
  bool open = false;
  u32 volume = 0xFFFFFFFFu;
  void* cb = nullptr;
  u64 inst = 0;
  u32 flags = 0;
};
midiout_state g_midiout;
constexpr u64 kMidiOutHandle = 0x4D49000000000001ull;
}  // namespace

u32 TRT_GUEST_ABI trt_midiOutGetNumDevs(void) { return 1; }

u32 TRT_GUEST_ABI trt_midiOutGetDevCapsW(u64 dev, void* caps, u32 size) {
  // Device 0 is us; MIDI_MAPPER ((UINT)-1) also resolves to the only device.
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;  // MMSYSERR_BADDEVICEID
  if (!caps || size < 84) return 11;               // MMSYSERR_INVALPARAM
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;       // wMid (MM_MICROSOFT)
  *(u16*)(p + 2) = 1;       // wPid (MOD_SWSYNTH)
  *(u32*)(p + 4) = 0x0100;  // vDriverVersion 1.0
  const char* name = "Tiramisu Virtual MIDI Synth";
  u16* nm = (u16*)(p + 8);  // szPname[32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = (u16)(u8)name[i];
  *(u16*)(p + 72) = 7;      // wTechnology = MOD_SWSYNTH
  *(u16*)(p + 74) = 32;     // wVoices
  *(u16*)(p + 76) = 32;     // wNotes
  *(u16*)(p + 78) = 0xFFFF; // wChannelMask
  *(u32*)(p + 80) = 3;      // dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME
  return 0;                 // MMSYSERR_NOERROR
}

u32 TRT_GUEST_ABI trt_midiOutGetDevCapsA(u64 dev, void* caps, u32 size) {
  if (dev != 0 && dev != 0xFFFFFFFFull) return 2;  // MMSYSERR_BADDEVICEID
  if (!caps || size < 52) return 11;               // MMSYSERR_INVALPARAM
  u8* p = (u8*)caps;
  memset(p, 0, size);
  *(u16*)(p + 0) = 1;       // wMid
  *(u16*)(p + 2) = 1;       // wPid
  *(u32*)(p + 4) = 0x0100;  // vDriverVersion 1.0
  const char* name = "Tiramisu Virtual MIDI Synth";
  char* nm = (char*)(p + 8);  // szPname[32]
  for (int i = 0; name[i] && i < 31; ++i) nm[i] = name[i];
  *(u16*)(p + 40) = 7;      // wTechnology = MOD_SWSYNTH
  *(u16*)(p + 42) = 32;     // wVoices
  *(u16*)(p + 44) = 32;     // wNotes
  *(u16*)(p + 46) = 0xFFFF; // wChannelMask
  *(u32*)(p + 48) = 3;      // dwSupport
  return 0;                 // MMSYSERR_NOERROR
}

u32 TRT_GUEST_ABI trt_midiOutOpen(void** ph, u32 dev, u64 cb, u64 inst,
                                  u32 flags) {
  if (dev != 0 && dev != 0xFFFFFFFFu) return 2;  // MMSYSERR_BADDEVICEID
  if (!ph) return 11;                            // MMSYSERR_INVALPARAM
  g_midiout.open = true;
  g_midiout.cb = (void*)cb;
  g_midiout.inst = inst;
  g_midiout.flags = flags;
  *ph = (void*)(uintptr_t)kMidiOutHandle;
  if ((flags & 0x00070000) == 0x00050000 && cb) {
    trt_SetEvent((void*)cb);
  }
  return 0;  // MMSYSERR_NOERROR
}

u32 TRT_GUEST_ABI trt_midiOutClose(void* hmo) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  g_midiout.open = false;
  if ((g_midiout.flags & 0x00070000) == 0x00050000 && g_midiout.cb) {
    trt_SetEvent(g_midiout.cb);
  }
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutShortMsg(void* hmo, u32 msg) {
  (void)msg;
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  return 0;  // MMSYSERR_NOERROR
}

u32 TRT_GUEST_ABI trt_midiOutLongMsg(void* hmo, void* lpMidiHdr, u32 cbMidiHdr) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  if (!lpMidiHdr || cbMidiHdr < 32) return 11;          // MMSYSERR_INVALPARAM
  *(u32*)((u8*)lpMidiHdr + 24) |= 0x00000001u;          // MHDR_DONE
  if ((g_midiout.flags & 0x00070000) == 0x00050000 && g_midiout.cb) {
    trt_SetEvent(g_midiout.cb);
  }
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutPrepareHeader(void* hmo, void* lpMidiHdr, u32 cbMidiHdr) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  if (!lpMidiHdr || cbMidiHdr < 32) return 11;          // MMSYSERR_INVALPARAM
  *(u32*)((u8*)lpMidiHdr + 24) |= 0x00000002u;          // MHDR_PREPARED
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutUnprepareHeader(void* hmo, void* lpMidiHdr, u32 cbMidiHdr) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  if (!lpMidiHdr || cbMidiHdr < 32) return 11;          // MMSYSERR_INVALPARAM
  *(u32*)((u8*)lpMidiHdr + 24) &= ~0x00000002u;         // clear MHDR_PREPARED
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutReset(void* hmo) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutGetVolume(void* hmo, u32* vol) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  if (!vol) return 11;                                  // MMSYSERR_INVALPARAM
  *vol = g_midiout.volume;
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutSetVolume(void* hmo, u32 vol) {
  if ((u64)(uintptr_t)hmo != kMidiOutHandle) return 4;  // MMSYSERR_INVALHANDLE
  g_midiout.volume = vol;
  return 0;
}

// ---- multimedia timers (real) --------------------------------------------
// TIME_ONESHOT/TIME_PERIODIC and TIME_CALLBACK_FUNCTION/EVENT_SET all work;
// the id returned is the timer's handle for timeKillEvent. A null callback is
// the one case the API itself rejects.
u32 TRT_GUEST_ABI trt_timeSetEvent(u32 delay, u32 res, void* cb, u64 user,
                                   u32 flags) {
  if (!cb) return 0;
  auto t = std::make_shared<mm_timer>();
  t->id = g_next_timer_id.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(g_timers_mu);
    g_timers.push_back(t);
  }
  t->th = std::thread(mm_timer_body, t, delay, res, cb, user, flags);
  static unsigned s_logged = 0;
  if (++s_logged <= 3) {
    char line[160];
    snprintf(line, sizeof(line),
             "winmm: timeSetEvent(delay=%u, flags=0x%x) -> id=%u (real timer "
             "thread)", delay, flags, t->id);
    trt_core_log(line);
  }
  return t->id;
}

u32 TRT_GUEST_ABI trt_timeKillEvent(u32 id) {
  std::shared_ptr<mm_timer> t;
  {
    std::lock_guard<std::mutex> lk(g_timers_mu);
    for (size_t i = 0; i < g_timers.size(); ++i) {
      if (g_timers[i]->id == id) {
        t = g_timers[i];
        g_timers.erase(g_timers.begin() + (std::ptrdiff_t)i);
        break;
      }
    }
  }
  if (!t) return 97;  // TIMERR_NOCANDO: no such timer
  {
    std::lock_guard<std::mutex> lk(t->mu);
    t->stop = true;
  }
  t->cv.notify_all();
  if (t->th.joinable()) {
    if (t->th.get_id() == std::this_thread::get_id())
      t->th.detach();  // killed from inside its own callback
    else
      t->th.join();
  }
  return 0;  // TIMERR_NOERROR
}

// Diagnostics for the gate: no guest code needed to prove the thread fired.
extern "C" unsigned trt_timer_live(void) {
  std::lock_guard<std::mutex> lk(g_timers_mu);
  return (unsigned)g_timers.size();
}
extern "C" unsigned long long trt_timer_fires(void) {
  return g_timer_fires.load();
}

namespace {
const char* midi_error_text(u32 error) {
  switch (error) {
    case 0: return "No error.";
    case 2: return "Invalid device ID.";
    case 4: return "Invalid device handle.";
    case 11: return "Invalid parameter.";
    default: return nullptr;
  }
}
}

u32 TRT_GUEST_ABI trt_midiOutGetErrorTextA(u32 error, char* buf, u32 len) {
  if (!len) return 0;
  if (!buf) return 11;
  const char* text = midi_error_text(error);
  if (!text) return 9;  // MMSYSERR_BADERRNUM
  snprintf(buf, len, "%s", text);
  return 0;
}

u32 TRT_GUEST_ABI trt_midiOutGetErrorTextW(u32 error, u16* buf, u32 len) {
  if (!len) return 0;
  if (!buf) return 11;
  const char* text = midi_error_text(error);
  if (!text) return 9;  // MMSYSERR_BADERRNUM
  u32 i = 0;
  for (; i < len - 1 && text[i]; ++i) buf[i] = static_cast<u16>(text[i]);
  buf[i] = 0;
  return 0;
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
      {"waveInStop", (void*)&trt_waveInStop},
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
      {"waveOutGetVolume", (void*)&trt_waveOutGetVolume},
      {"waveInMessage", (void*)&trt_waveInMessage},
      {"waveOutGetErrorTextW", (void*)&trt_waveOutGetErrorTextW},
      {"waveOutGetErrorTextA", (void*)&trt_waveOutGetErrorTextW},
      {"waveInGetErrorTextW", (void*)&trt_waveInGetErrorTextW},
      {"waveInGetErrorTextA", (void*)&trt_waveInGetErrorTextW},
      {"midiOutGetNumDevs", (void*)&trt_midiOutGetNumDevs},
      {"midiOutOpen", (void*)&trt_midiOutOpen},
      {"midiOutClose", (void*)&trt_midiOutClose},
      {"midiOutShortMsg", (void*)&trt_midiOutShortMsg},
      {"midiOutLongMsg", (void*)&trt_midiOutLongMsg},
      {"midiOutPrepareHeader", (void*)&trt_midiOutPrepareHeader},
      {"midiOutUnprepareHeader", (void*)&trt_midiOutUnprepareHeader},
      {"midiOutReset", (void*)&trt_midiOutReset},
      {"midiOutGetDevCapsW", (void*)&trt_midiOutGetDevCapsW},
      {"midiOutGetDevCapsA", (void*)&trt_midiOutGetDevCapsA},
      {"midiOutGetErrorTextA", (void*)&trt_midiOutGetErrorTextA},
      {"midiOutGetErrorTextW", (void*)&trt_midiOutGetErrorTextW},
      {"midiOutGetVolume", (void*)&trt_midiOutGetVolume},
      {"midiOutSetVolume", (void*)&trt_midiOutSetVolume},
      // Real now: a thread per timer fires the guest callback (or sets the
      // caller's event) at the requested interval until timeKillEvent.
      {"timeSetEvent", (void*)&trt_timeSetEvent},
      {"timeKillEvent", (void*)&trt_timeKillEvent},
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
