// tiramisu/src/trt/bridge_wasapi.cpp
// G4 — WASAPI COM audio stack + avrt.dll bridge. See bridge_wasapi.h.
#include "bridge_wasapi.h"
#include "bridge_audio.h"
#include "bridge_ole.h"
#include "images.h"
#include "thunk.h"
#include "trt.h"
// Settings knob (audio_buffer_ms); weak link keeps offline tests green when
// src/host/settings.cpp is not in the test binary.
extern "C" int trt_settings_audio_buffer_ms() __attribute__((weak));

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

namespace wasapilog {
static void line(const char* s) { trt_core_log(s); }
}  // namespace wasapilog

extern "C" int TRT_GUEST_ABI trt_SetEvent(void* h);

namespace {

constexpr i32 k_s_ok = 0;
constexpr i32 k_e_invalidarg = (i32)0x80070057u;
constexpr i32 k_e_outofmemory = (i32)0x8007000Eu;
constexpr i32 k_e_nointerface = (i32)0x80004002u;
constexpr i32 k_e_notfound = (i32)0x80070490u;

// CLSID_MMDeviceEnumerator: {BCDE0395-E52F-467C-8E3D-C4579291692E}
constexpr u8 k_clsid_mmdevice_enumerator[16] = {
    0x95, 0x03, 0xDE, 0xBC, 0x2F, 0xE5, 0x7C, 0x46,
    0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E};

// IID_IMMEndpoint: {1BE09788-6894-4089-8586-9A2A6C265AC5}
constexpr u8 k_iid_immendpoint[16] = {0x88, 0x97, 0xE0, 0x1B, 0x94, 0x68,
                                      0x89, 0x40, 0x85, 0x86, 0x9A, 0x2A,
                                      0x6C, 0x26, 0x5A, 0xC5};

// IID_IAudioRenderClient: {F294ACFC-3146-4483-A7BF-ADDCA7C260E2}
constexpr u8 k_iid_render_client[16] = {0xFC, 0xAC, 0x94, 0xF2, 0x46, 0x31,
                                        0x83, 0x44, 0xA7, 0xBF, 0xAD, 0xDC,
                                        0xA7, 0xC2, 0x60, 0xE2};

// IID_IAudioClock: {CD63314F-3FBA-4A1B-812C-EF96358728E7}
constexpr u8 k_iid_audio_clock[16] = {0x4F, 0x31, 0x63, 0xCD, 0xBA, 0x3F,
                                      0x1B, 0x4A, 0x81, 0x2C, 0xEF, 0x96,
                                      0x35, 0x87, 0x28, 0xE7};

// IID_ISimpleAudioVolume: {87CE5498-68D6-44E5-9215-6DA47EF883D8}
constexpr u8 k_iid_simple_volume[16] = {0x98, 0x54, 0xCE, 0x87, 0xD6, 0x68,
                                        0xE5, 0x44, 0x92, 0x15, 0x6D, 0xA4,
                                        0x7E, 0xF8, 0x83, 0xD8};

// IID_IAudioStreamVolume: {93014887-242D-4068-8A15-CF5E93B90FE3}
constexpr u8 k_iid_stream_volume[16] = {0x87, 0x48, 0x01, 0x93, 0x2D, 0x24,
                                        0x68, 0x40, 0x8A, 0x15, 0xCF, 0x5E,
                                        0x93, 0xB9, 0x0F, 0xE3};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: {00000003-0000-0010-8000-00AA00389B71}
constexpr u8 k_subtype_ieee_float[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                         0x00, 0x38, 0x9B, 0x71};

// KSDATAFORMAT_SUBTYPE_PCM: {00000001-0000-0010-8000-00AA00389B71}
constexpr u8 k_subtype_pcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                  0x00, 0x38, 0x9B, 0x71};

#pragma pack(push, 1)
struct wave_format_extensible {
  u16 wFormatTag;       // 0xFFFE = WAVE_FORMAT_EXTENSIBLE
  u16 nChannels;        // 2
  u32 nSamplesPerSec;   // 48000
  u32 nAvgBytesPerSec;  // 48000 * 8 = 384000
  u16 nBlockAlign;      // 8
  u16 wBitsPerSample;   // 32
  u16 cbSize;           // 22
  u16 wValidBitsPerSample;
  u32 dwChannelMask;    // 3 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
  u8 SubFormat[16];
};
#pragma pack(pop)
static_assert(sizeof(wave_format_extensible) == 40,
              "WAVEFORMATEXTENSIBLE size mismatch");

wave_format_extensible* alloc_mix_format(void) {
  wave_format_extensible* fmt =
      (wave_format_extensible*)malloc(sizeof(wave_format_extensible));
  if (!fmt) return nullptr;
  fmt->wFormatTag = 0xFFFEu;
  fmt->nChannels = 2;
  fmt->nSamplesPerSec = 48000;
  fmt->nAvgBytesPerSec = 48000 * 8;
  fmt->nBlockAlign = 8;
  fmt->wBitsPerSample = 32;
  fmt->cbSize = 22;
  fmt->wValidBitsPerSample = 32;
  fmt->dwChannelMask = 3;
  memcpy(fmt->SubFormat, k_subtype_ieee_float, 16);
  return fmt;
}

// ===========================================================================
// IAudioRenderClient / IAudioClock / ISimpleAudioVolume / IAudioStreamVolume
// ===========================================================================

struct wasapi_client;

struct wasapi_render {
  void** vtbl;
  wasapi_client* parent;
};

struct wasapi_clock {
  void** vtbl;
  wasapi_client* parent;
};

struct wasapi_volume {
  void** vtbl;
  wasapi_client* parent;
};

struct wasapi_stream_vol {
  void** vtbl;
  wasapi_client* parent;
};

struct wasapi_client {
  void** vtbl = nullptr;
  i32 refs = 1;
  u32 rate = 48000;
  u32 channels = 2;
  u32 bits_per_sample = 32;
  u32 block_align = 8;
  bool is_float = true;
  u32 buffer_frames = 4800;
  u32 period_ms = 10;
  u32 in_flight_frames = 0;
  u64 last_submit_ns = 0;  // steady-clock timestamp of last ReleaseBuffer
  bool just_submitted = false;
  bool started = false;
  u64 total_frames_written = 0;
  void* event_handle = nullptr;
  u8* mix_buf = nullptr;
  size_t mix_buf_cap = 0;
  i16* pcm16_buf = nullptr;
  size_t pcm16_cap = 0;
  float master_volume = 1.0f;
  int muted = 0;
  wasapi_render render{};
  wasapi_clock clock{};
  wasapi_volume volume{};
  wasapi_stream_vol stream_vol{};
  std::mutex state_mu;
  std::atomic<bool> pump_stop{false};
  std::thread pump_thread;
};

void stop_wasapi_pump(wasapi_client* c) {
  if (!c) return;
  {
    std::lock_guard<std::mutex> lk(c->state_mu);
    c->started = false;
  }
  c->pump_stop.store(true, std::memory_order_release);
  if (c->pump_thread.joinable()) c->pump_thread.join();
}

void start_wasapi_pump(wasapi_client* c) {
  if (!c) return;
  stop_wasapi_pump(c);
  {
    std::lock_guard<std::mutex> lk(c->state_mu);
    c->started = true;
    c->pump_stop.store(false, std::memory_order_release);
  }
  c->pump_thread = std::thread([c]() {
    while (!c->pump_stop.load(std::memory_order_acquire)) {
      u32 step_ms = 10;
      void* ev = nullptr;
      {
        std::lock_guard<std::mutex> lk(c->state_mu);
        if (!c->started) break;
        step_ms = c->period_ms ? c->period_ms : 10u;
        // NOTE: padding decay lives in client_GetCurrentPadding (timestamp-
        // based, consumed on read). The pump only signals the period event;
        // decrementing in_flight_frames here as well double-consumed the
        // queue and collapsed padding to 0 between submits.
        ev = c->event_handle;
      }
      if (ev) trt_SetEvent(ev);
      std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
  });
}

i32 TRT_GUEST_ABI client_QueryInterface(void* self, const void* iid,
                                        void** out) {
  (void)iid;
  if (!out) return k_e_invalidarg;
  wasapi_client* c = (wasapi_client*)self;
  ++c->refs;
  *out = self;
  return k_s_ok;
}

u32 TRT_GUEST_ABI client_AddRef(void* self) {
  wasapi_client* c = (wasapi_client*)self;
  return (u32)++c->refs;
}

u32 TRT_GUEST_ABI client_Release(void* self) {
  wasapi_client* c = (wasapi_client*)self;
  const i32 left = --c->refs;
  if (left <= 0) {
    stop_wasapi_pump(c);
    free(c->mix_buf);
    free(c->pcm16_buf);
    delete c;
    return 0;
  }
  return (u32)left;
}

// Sub-object IUnknown forwarding to parent wasapi_client
i32 TRT_GUEST_ABI render_QI(void* self, const void* iid, void** out) {
  (void)iid;
  if (!out) return k_e_invalidarg;
  wasapi_render* r = (wasapi_render*)self;
  ++r->parent->refs;
  *out = self;
  return k_s_ok;
}
u32 TRT_GUEST_ABI render_AddRef(void* self) {
  return client_AddRef(((wasapi_render*)self)->parent);
}
u32 TRT_GUEST_ABI render_Release(void* self) {
  return client_Release(((wasapi_render*)self)->parent);
}

i32 TRT_GUEST_ABI render_GetBuffer(void* self, u32 num_frames, u8** data_out) {
  if (!data_out) return k_e_invalidarg;
  wasapi_client* c = ((wasapi_render*)self)->parent;
  const u32 frames = num_frames ? num_frames : c->buffer_frames;
  const size_t bytes = (size_t)frames * (size_t)c->block_align;
  if (bytes > c->mix_buf_cap) {
    u8* nb = (u8*)realloc(c->mix_buf, bytes ? bytes : 64);
    if (!nb) {
      *data_out = nullptr;
      return k_e_outofmemory;
    }
    c->mix_buf = nb;
    c->mix_buf_cap = bytes ? bytes : 64;
  }
  if (bytes) memset(c->mix_buf, 0, bytes);
  *data_out = c->mix_buf;
  return k_s_ok;
}

i32 TRT_GUEST_ABI render_ReleaseBuffer(void* self, u32 num_frames_written,
                                       u32 flags) {
  wasapi_client* c = ((wasapi_render*)self)->parent;
  if (num_frames_written > 0 && c->mix_buf) {
    const u32 samples = num_frames_written * c->channels;
    const size_t pcm_bytes = (size_t)samples * sizeof(i16);
    if (pcm_bytes > c->pcm16_cap) {
      i16* np = (i16*)realloc(c->pcm16_buf, pcm_bytes);
      if (np) {
        c->pcm16_buf = np;
        c->pcm16_cap = pcm_bytes;
      }
    }
    if (c->pcm16_buf && pcm_bytes <= c->pcm16_cap) {
      const bool silent = (flags & 0x2u /*AUDCLNT_BUFFERFLAGS_SILENT*/) != 0 ||
                          c->muted || c->master_volume <= 0.0f;
      const float vol = c->master_volume;
      if (silent) {
        memset(c->pcm16_buf, 0, pcm_bytes);
      } else if (c->bits_per_sample == 32 && c->is_float) {
        const float* src = (const float*)c->mix_buf;
        for (u32 i = 0; i < samples; ++i) {
          float s = src[i] * vol;
          if (s > 1.0f) s = 1.0f;
          if (s < -1.0f) s = -1.0f;
          c->pcm16_buf[i] = (i16)(s * 32767.0f);
        }
      } else if (c->bits_per_sample == 16) {
        const i16* src = (const i16*)c->mix_buf;
        if (vol >= 0.999f) {
          memcpy(c->pcm16_buf, src, pcm_bytes);
        } else {
          for (u32 i = 0; i < samples; ++i)
            c->pcm16_buf[i] = (i16)((float)src[i] * vol);
        }
      } else if (c->bits_per_sample == 32 && !c->is_float) {
        const i32* src = (const i32*)c->mix_buf;
        for (u32 i = 0; i < samples; ++i)
          c->pcm16_buf[i] = (i16)((float)(src[i] >> 16) * vol);
      } else {
        memset(c->pcm16_buf, 0, pcm_bytes);
      }

      if (!trt_audio_submit_voice((u64)(uintptr_t)c | 0x5741000000000000ULL,
                                  c->pcm16_buf, (u32)pcm_bytes, c->rate,
                                  c->channels, 2, 0, 1.0f)) {
        void (*submit)(const void*, u32, u32, u32, u32, void*) = nullptr;
        void* user = nullptr;
        trt_audio_get_hooks(&submit, &user);
        if (submit) {
          submit(c->pcm16_buf, (u32)pcm_bytes, c->rate, c->channels, 2, user);
        }
      }
    }
    std::lock_guard<std::mutex> lk(c->state_mu);
    c->total_frames_written += num_frames_written;
    const u32 sum = c->in_flight_frames + num_frames_written;
    c->in_flight_frames = sum <= c->buffer_frames ? sum : c->buffer_frames;
    c->last_submit_ns =
        (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    c->just_submitted = true;
  }
  void* ev = nullptr;
  {
    std::lock_guard<std::mutex> lk(c->state_mu);
    ev = c->event_handle;
  }
  if (ev) trt_SetEvent(ev);
  return k_s_ok;
}

void* k_render_vtbl[5] = {
    (void*)&render_QI,        (void*)&render_AddRef,
    (void*)&render_Release,   (void*)&render_GetBuffer,
    (void*)&render_ReleaseBuffer,
};

// IAudioClock
i32 TRT_GUEST_ABI clock_GetFrequency(void* self, u64* freq) {
  if (!freq) return k_e_invalidarg;
  wasapi_client* c = ((wasapi_clock*)self)->parent;
  *freq = (u64)c->rate * (u64)c->block_align;
  return k_s_ok;
}
i32 TRT_GUEST_ABI clock_GetPosition(void* self, u64* pos, u64* qpc_pos) {
  if (!pos) return k_e_invalidarg;
  wasapi_client* c = ((wasapi_clock*)self)->parent;
  *pos = c->total_frames_written * (u64)c->block_align;
  if (qpc_pos) *qpc_pos = 0;
  return k_s_ok;
}
i32 TRT_GUEST_ABI clock_GetCharacteristics(void* self, u32* chars) {
  (void)self;
  if (chars) *chars = 0;
  return k_s_ok;
}

void* k_clock_vtbl[6] = {
    (void*)&render_QI,          (void*)&render_AddRef,
    (void*)&render_Release,     (void*)&clock_GetFrequency,
    (void*)&clock_GetPosition,  (void*)&clock_GetCharacteristics,
};

// ISimpleAudioVolume
i32 TRT_GUEST_ABI vol_SetMasterVolume(void* self, float level,
                                      const void* ctx) {
  (void)ctx;
  wasapi_client* c = ((wasapi_volume*)self)->parent;
  c->master_volume = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
  return k_s_ok;
}
i32 TRT_GUEST_ABI vol_GetMasterVolume(void* self, float* level) {
  if (!level) return k_e_invalidarg;
  *level = ((wasapi_volume*)self)->parent->master_volume;
  return k_s_ok;
}
i32 TRT_GUEST_ABI vol_SetMute(void* self, int mute, const void* ctx) {
  (void)ctx;
  ((wasapi_volume*)self)->parent->muted = mute;
  return k_s_ok;
}
i32 TRT_GUEST_ABI vol_GetMute(void* self, int* mute) {
  if (!mute) return k_e_invalidarg;
  *mute = ((wasapi_volume*)self)->parent->muted;
  return k_s_ok;
}

void* k_vol_vtbl[7] = {
    (void*)&render_QI,           (void*)&render_AddRef,
    (void*)&render_Release,      (void*)&vol_SetMasterVolume,
    (void*)&vol_GetMasterVolume, (void*)&vol_SetMute,
    (void*)&vol_GetMute,
};

// IAudioStreamVolume
i32 TRT_GUEST_ABI svol_GetChannelCount(void* self, u32* count) {
  if (!count) return k_e_invalidarg;
  *count = ((wasapi_stream_vol*)self)->parent->channels;
  return k_s_ok;
}
i32 TRT_GUEST_ABI svol_SetChannelVolume(void* self, u32 idx, float level) {
  (void)self;
  (void)idx;
  (void)level;
  return k_s_ok;
}
i32 TRT_GUEST_ABI svol_GetChannelVolume(void* self, u32 idx, float* level) {
  (void)self;
  (void)idx;
  if (!level) return k_e_invalidarg;
  *level = 1.0f;
  return k_s_ok;
}
i32 TRT_GUEST_ABI svol_SetAllVolumes(void* self, u32 count,
                                     const float* volumes) {
  (void)self;
  (void)count;
  (void)volumes;
  return k_s_ok;
}
i32 TRT_GUEST_ABI svol_GetAllVolumes(void* self, u32 count, float* volumes) {
  (void)self;
  if (!volumes) return k_e_invalidarg;
  for (u32 i = 0; i < count; ++i) volumes[i] = 1.0f;
  return k_s_ok;
}

void* k_svol_vtbl[8] = {
    (void*)&render_QI,             (void*)&render_AddRef,
    (void*)&render_Release,        (void*)&svol_GetChannelCount,
    (void*)&svol_SetChannelVolume, (void*)&svol_GetChannelVolume,
    (void*)&svol_SetAllVolumes,    (void*)&svol_GetAllVolumes,
};

// IAudioClient methods
i32 TRT_GUEST_ABI client_Initialize(void* self, u32 share_mode,
                                    u32 stream_flags, i64 hns_buffer_duration,
                                    i64 hns_periodicity, const void* format,
                                    const void* session_guid) {
  (void)share_mode;
  (void)stream_flags;
  (void)hns_periodicity;
  (void)session_guid;
  wasapi_client* c = (wasapi_client*)self;
  if (format) {
    const wave_format_extensible* wf = (const wave_format_extensible*)format;
    if (wf->nSamplesPerSec >= 8000 && wf->nSamplesPerSec <= 192000)
      c->rate = wf->nSamplesPerSec;
    if (wf->nChannels >= 1 && wf->nChannels <= 8) c->channels = wf->nChannels;
    if (wf->wBitsPerSample == 16 || wf->wBitsPerSample == 32)
      c->bits_per_sample = wf->wBitsPerSample;
    if (wf->wFormatTag == 3 /*WAVE_FORMAT_IEEE_FLOAT*/) {
      c->is_float = true;
    } else if (wf->wFormatTag == 1 /*WAVE_FORMAT_PCM*/) {
      c->is_float = false;
    } else if (wf->wFormatTag == 0xFFFEu && wf->cbSize >= 22) {
      c->is_float = (memcmp(wf->SubFormat, k_subtype_ieee_float, 16) == 0);
      if (memcmp(wf->SubFormat, k_subtype_pcm, 16) == 0) c->is_float = false;
    }
    c->block_align = c->channels * (c->bits_per_sample / 8);
    if (c->block_align == 0) c->block_align = 8;
  }
  if (hns_buffer_duration > 0) {
    const u64 frames =
        ((u64)hns_buffer_duration * (u64)c->rate) / 10000000ULL;
    c->buffer_frames = frames >= 1024 ? (u32)frames : 4800u;
  } else {
    // Settings knob: audio_buffer_ms (default 100). Games that never pass
    // hnsBufferDuration get the user's tuning instead of a hard-coded 100ms.
    const int ms = trt_settings_audio_buffer_ms ? trt_settings_audio_buffer_ms() : 0;
    const u64 frames =
        (u64)c->rate * (u64)(ms > 0 ? ms : 100) / 1000ull;
    c->buffer_frames = frames >= 1024 ? (u32)frames : 4800u;
  }
  const size_t bytes = (size_t)c->buffer_frames * (size_t)c->block_align;
  free(c->mix_buf);
  c->mix_buf = (u8*)calloc(1, bytes ? bytes : 64);
  c->mix_buf_cap = bytes ? bytes : 64;
  c->in_flight_frames = 0;
  c->just_submitted = false;
  if (c->event_handle) trt_SetEvent(c->event_handle);
  char buf[160];
  snprintf(buf, sizeof(buf),
           "wasapi: IAudioClient::Initialize(rate=%u ch=%u bits=%u float=%d "
           "frames=%u) -> S_OK",
           c->rate, c->channels, c->bits_per_sample, c->is_float ? 1 : 0,
           c->buffer_frames);
  wasapilog::line(buf);
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetBufferSize(void* self, u32* out_frames) {
  if (!out_frames) return k_e_invalidarg;
  *out_frames = ((wasapi_client*)self)->buffer_frames;
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetStreamLatency(void* self, i64* out_lat) {
  (void)self;
  if (!out_lat) return k_e_invalidarg;
  *out_lat = 100000;  // 10 ms in 100-ns units
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetCurrentPadding(void* self, u32* out_padding) {
  if (!out_padding) return k_e_invalidarg;
  wasapi_client* c = (wasapi_client*)self;
  // Real-time consumption model: padding is the frames currently queued in
  // the (virtual) device, decaying at the render rate — exactly what the
  // real device does. Godot's WASAPI thread paces itself SOLELY on this
  // value (4.3 audio_driver_wasapi.cpp: write only when
  // buffer_size - padding > 0, sleep 1ms only when it wrote nothing), so a
  // padding that collapses to 0 on read made the guest write full-buffers
  // back-to-back forever: the 97%-CPU mix storm that held the CS and wedged
  // the boot. Reading padding must not consume state (real WASAPI is not
  // destructive here either); the pump thread's decay + a timestamp-based
  // floor keep it honest between reads.
  std::lock_guard<std::mutex> lk(c->state_mu);
  u32 padding = c->in_flight_frames;
  if (padding > 0 && c->last_submit_ns != 0) {
    const u64 now_ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const u64 since_ns = now_ns > c->last_submit_ns ? now_ns - c->last_submit_ns : 0;
    const u64 consumed = ((u64)c->rate * since_ns) / 1000000000ULL;
    padding = (u64)padding > consumed ? (u32)((u64)padding - consumed) : 0u;
  }
  *out_padding = padding;
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_IsFormatSupported(void* self, u32 share_mode,
                                           const void* format,
                                           void** closest_match) {
  (void)self;
  (void)share_mode;
  if (closest_match) *closest_match = nullptr;
  if (!format) return k_e_invalidarg;
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetMixFormat(void* self, void** out_format) {
  (void)self;
  if (!out_format) return k_e_invalidarg;
  wave_format_extensible* fmt = alloc_mix_format();
  if (!fmt) return k_e_outofmemory;
  *out_format = fmt;
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetDevicePeriod(void* self, i64* def_period,
                                         i64* min_period) {
  (void)self;
  if (def_period) *def_period = 100000;  // 10 ms
  if (min_period) *min_period = 30000;   // 3 ms
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_Start(void* self) {
  wasapi_client* c = (wasapi_client*)self;
  start_wasapi_pump(c);
  if (c->event_handle) trt_SetEvent(c->event_handle);
  wasapilog::line("wasapi: IAudioClient::Start -> S_OK");
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_Stop(void* self) {
  wasapi_client* c = (wasapi_client*)self;
  stop_wasapi_pump(c);
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_Reset(void* self) {
  wasapi_client* c = (wasapi_client*)self;
  std::lock_guard<std::mutex> lk(c->state_mu);
  c->in_flight_frames = 0;
  c->just_submitted = false;
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_SetEventHandle(void* self, void* event_handle) {
  wasapi_client* c = (wasapi_client*)self;
  {
    std::lock_guard<std::mutex> lk(c->state_mu);
    c->event_handle = event_handle;
  }
  if (event_handle) trt_SetEvent(event_handle);
  return k_s_ok;
}

i32 TRT_GUEST_ABI client_GetService(void* self, const void* riid, void** ppv) {
  if (!riid || !ppv) return k_e_invalidarg;
  *ppv = nullptr;
  wasapi_client* c = (wasapi_client*)self;
  if (memcmp(riid, k_iid_render_client, 16) == 0) {
    ++c->refs;
    *ppv = &c->render;
    return k_s_ok;
  }
  if (memcmp(riid, k_iid_audio_clock, 16) == 0) {
    ++c->refs;
    *ppv = &c->clock;
    return k_s_ok;
  }
  if (memcmp(riid, k_iid_simple_volume, 16) == 0) {
    ++c->refs;
    *ppv = &c->volume;
    return k_s_ok;
  }
  if (memcmp(riid, k_iid_stream_volume, 16) == 0) {
    ++c->refs;
    *ppv = &c->stream_vol;
    return k_s_ok;
  }
  wasapilog::line("wasapi: IAudioClient::GetService unknown IID -> E_NOINTERFACE");
  return k_e_nointerface;
}

// IAudioClient2 / IAudioClient3 extra methods
i32 TRT_GUEST_ABI client_IsOffloadCapable(void* self, int category,
                                          int* offload) {
  (void)self;
  (void)category;
  if (offload) *offload = 0;
  return k_s_ok;
}
i32 TRT_GUEST_ABI client_SetClientProperties(void* self, const void* props) {
  (void)self;
  (void)props;
  return k_s_ok;
}
i32 TRT_GUEST_ABI client_GetBufferSizeLimits(void* self, const void* fmt,
                                             int event_driven, i64* min_dur,
                                             i64* max_dur) {
  (void)self;
  (void)fmt;
  (void)event_driven;
  if (min_dur) *min_dur = 30000;
  if (max_dur) *max_dur = 1000000;
  return k_s_ok;
}
i32 TRT_GUEST_ABI client_GetSharedModeEnginePeriod(void* self, const void* fmt,
                                                   u32* def_frames,
                                                   u32* fund_frames,
                                                   u32* min_frames,
                                                   u32* max_frames) {
  (void)self;
  (void)fmt;
  if (def_frames) *def_frames = 480;
  if (fund_frames) *fund_frames = 48;
  if (min_frames) *min_frames = 480;
  if (max_frames) *max_frames = 480;
  return k_s_ok;
}
i32 TRT_GUEST_ABI client_GetCurrentSharedModeEnginePeriod(void* self,
                                                          void** fmt,
                                                          u32* cur_frames) {
  (void)self;
  if (fmt) *fmt = alloc_mix_format();
  if (cur_frames) *cur_frames = 480;
  return k_s_ok;
}
i32 TRT_GUEST_ABI client_InitializeSharedAudioStream(void* self, u32 flags,
                                                     u32 period_frames,
                                                     const void* fmt,
                                                     const void* session_guid) {
  (void)period_frames;
  return client_Initialize(self, 0, flags, 100000, 0, fmt, session_guid);
}

void* k_client_vtbl[21] = {
    (void*)&client_QueryInterface,
    (void*)&client_AddRef,
    (void*)&client_Release,
    (void*)&client_Initialize,
    (void*)&client_GetBufferSize,
    (void*)&client_GetStreamLatency,
    (void*)&client_GetCurrentPadding,
    (void*)&client_IsFormatSupported,
    (void*)&client_GetMixFormat,
    (void*)&client_GetDevicePeriod,
    (void*)&client_Start,
    (void*)&client_Stop,
    (void*)&client_Reset,
    (void*)&client_SetEventHandle,
    (void*)&client_GetService,
    (void*)&client_IsOffloadCapable,
    (void*)&client_SetClientProperties,
    (void*)&client_GetBufferSizeLimits,
    (void*)&client_GetSharedModeEnginePeriod,
    (void*)&client_GetCurrentSharedModeEnginePeriod,
    (void*)&client_InitializeSharedAudioStream,
};

wasapi_client* alloc_client(void) {
  wasapi_client* c = new wasapi_client();
  c->vtbl = k_client_vtbl;
  c->refs = 1;
  c->rate = 48000;
  c->channels = 2;
  c->bits_per_sample = 32;
  c->block_align = 8;
  c->is_float = true;
  c->buffer_frames = 4800;
  c->period_ms = 10;
  c->master_volume = 1.0f;
  c->render = {k_render_vtbl, c};
  c->clock = {k_clock_vtbl, c};
  c->volume = {k_vol_vtbl, c};
  c->stream_vol = {k_svol_vtbl, c};
  return c;
}

// ===========================================================================
// IPropertyStore / IMMEndpoint / IMMDevice / IMMDeviceCollection / Enumerator
// ===========================================================================

i32 TRT_GUEST_ABI static_QI(void* self, const void* iid, void** out) {
  (void)iid;
  if (!out) return k_e_invalidarg;
  *out = self;
  return k_s_ok;
}
u32 TRT_GUEST_ABI static_AddRef(void* self) {
  (void)self;
  return 2;
}
u32 TRT_GUEST_ABI static_Release(void* self) {
  (void)self;
  return 1;
}

i32 TRT_GUEST_ABI prop_GetCount(void* self, u32* count) {
  (void)self;
  if (!count) return k_e_invalidarg;
  *count = 2;
  return k_s_ok;
}
i32 TRT_GUEST_ABI prop_GetAt(void* self, u32 idx, void* pkey) {
  (void)self;
  (void)idx;
  if (!pkey) return k_e_invalidarg;
  memset(pkey, 0, 20);
  return k_s_ok;
}
i32 TRT_GUEST_ABI prop_GetValue(void* self, const void* key, void* pv) {
  (void)self;
  if (!key || !pv) return k_e_invalidarg;
  memset(pv, 0, 24);
  const u32 d1 = *(const u32*)key;
  // PKEY_AudioEngine_DeviceFormat (0xF19F064D) or OEMFormat (0xE4870E26)
  if (d1 == 0xF19F064Du || d1 == 0xE4870E26u) {
    wave_format_extensible* fmt = alloc_mix_format();
    if (!fmt) return k_e_outofmemory;
    *(u16*)pv = 65u;  // VT_BLOB
    *(u32*)((u8*)pv + 8) = (u32)sizeof(wave_format_extensible);
    *(void**)((u8*)pv + 16) = fmt;
    return k_s_ok;
  }
  // FriendlyName or any string property -> VT_LPWSTR (31)
  static const char k_name[] = "Tiramisu Xbox Audio";
  u16* wstr = (u16*)malloc(sizeof(k_name) * 2);
  if (!wstr) return k_e_outofmemory;
  for (size_t i = 0; i < sizeof(k_name); ++i) wstr[i] = (u16)(u8)k_name[i];
  *(u16*)pv = 31u;  // VT_LPWSTR
  *(void**)((u8*)pv + 8) = wstr;
  return k_s_ok;
}
i32 TRT_GUEST_ABI prop_SetValue(void* self, const void* key,
                                const void* propvar) {
  (void)self;
  (void)key;
  (void)propvar;
  return k_s_ok;
}
i32 TRT_GUEST_ABI prop_Commit(void* self) {
  (void)self;
  return k_s_ok;
}

void* k_prop_vtbl[8] = {
    (void*)&static_QI,     (void*)&static_AddRef, (void*)&static_Release,
    (void*)&prop_GetCount, (void*)&prop_GetAt,    (void*)&prop_GetValue,
    (void*)&prop_SetValue, (void*)&prop_Commit,
};
struct static_com_obj {
  void** vtbl;
};
static_com_obj g_prop_store = {k_prop_vtbl};

// IMMEndpoint
i32 TRT_GUEST_ABI endpoint_GetDataFlow(void* self, u32* data_flow) {
  (void)self;
  if (!data_flow) return k_e_invalidarg;
  *data_flow = 0;  // eRender
  return k_s_ok;
}
void* k_endpoint_vtbl[4] = {
    (void*)&static_QI,
    (void*)&static_AddRef,
    (void*)&static_Release,
    (void*)&endpoint_GetDataFlow,
};
static_com_obj g_endpoint = {k_endpoint_vtbl};

// IMMDevice
i32 TRT_GUEST_ABI dev_QueryInterface(void* self, const void* iid, void** out) {
  if (!out) return k_e_invalidarg;
  if (iid && memcmp(iid, k_iid_immendpoint, 16) == 0) {
    *out = &g_endpoint;
    return k_s_ok;
  }
  *out = self;
  return k_s_ok;
}
i32 TRT_GUEST_ABI dev_Activate(void* self, const void* iid, u32 cls_ctx,
                               void* params, void** out) {
  (void)self;
  (void)iid;
  (void)cls_ctx;
  (void)params;
  if (!out) return k_e_invalidarg;
  wasapi_client* c = alloc_client();
  if (!c) {
    *out = nullptr;
    return k_e_outofmemory;
  }
  *out = c;
  wasapilog::line("wasapi: IMMDevice::Activate -> IAudioClient");
  return k_s_ok;
}
i32 TRT_GUEST_ABI dev_OpenPropertyStore(void* self, u32 access, void** out) {
  (void)self;
  (void)access;
  if (!out) return k_e_invalidarg;
  *out = &g_prop_store;
  return k_s_ok;
}
i32 TRT_GUEST_ABI dev_GetId(void* self, u16** out_id) {
  (void)self;
  if (!out_id) return k_e_invalidarg;
  static const char k_id[] = "{0.0.0.00000000}.{tiramisu-xbox-render}";
  u16* s = (u16*)malloc(sizeof(k_id) * 2);
  if (!s) return k_e_outofmemory;
  for (size_t i = 0; i < sizeof(k_id); ++i) s[i] = (u16)(u8)k_id[i];
  *out_id = s;
  return k_s_ok;
}
i32 TRT_GUEST_ABI dev_GetState(void* self, u32* out_state) {
  (void)self;
  if (!out_state) return k_e_invalidarg;
  *out_state = 1u;  // DEVICE_STATE_ACTIVE
  return k_s_ok;
}

void* k_dev_vtbl[7] = {
    (void*)&dev_QueryInterface,    (void*)&static_AddRef,
    (void*)&static_Release,        (void*)&dev_Activate,
    (void*)&dev_OpenPropertyStore, (void*)&dev_GetId,
    (void*)&dev_GetState,
};
static_com_obj g_default_dev = {k_dev_vtbl};

// IMMDeviceCollection
struct dev_collection {
  void** vtbl;
  u32 count;
};
i32 TRT_GUEST_ABI coll_GetCount(void* self, u32* count) {
  if (!count) return k_e_invalidarg;
  *count = ((dev_collection*)self)->count;
  return k_s_ok;
}
i32 TRT_GUEST_ABI coll_Item(void* self, u32 idx, void** out_dev) {
  if (!out_dev) return k_e_invalidarg;
  dev_collection* c = (dev_collection*)self;
  if (idx >= c->count) {
    *out_dev = nullptr;
    return k_e_invalidarg;
  }
  *out_dev = &g_default_dev;
  return k_s_ok;
}
void* k_coll_vtbl[5] = {
    (void*)&static_QI,     (void*)&static_AddRef, (void*)&static_Release,
    (void*)&coll_GetCount, (void*)&coll_Item,
};
dev_collection g_render_coll = {k_coll_vtbl, 1};
dev_collection g_empty_coll = {k_coll_vtbl, 0};

// IMMDeviceEnumerator
i32 TRT_GUEST_ABI enum_EnumAudioEndpoints(void* self, u32 data_flow,
                                          u32 state_mask, void** out_coll) {
  (void)self;
  (void)state_mask;
  if (!out_coll) return k_e_invalidarg;
  // data_flow: 0 = eRender, 1 = eCapture, 2 = eAll
  *out_coll = (data_flow == 1u) ? &g_empty_coll : &g_render_coll;
  return k_s_ok;
}
i32 TRT_GUEST_ABI enum_GetDefaultAudioEndpoint(void* self, u32 data_flow,
                                               u32 role, void** out_dev) {
  (void)self;
  (void)role;
  if (!out_dev) return k_e_invalidarg;
  if (data_flow == 1u /*eCapture*/) {
    *out_dev = nullptr;
    wasapilog::line(
        "wasapi: GetDefaultAudioEndpoint(eCapture) -> E_NOTFOUND (no mic)");
    return k_e_notfound;
  }
  *out_dev = &g_default_dev;
  wasapilog::line(
      "wasapi: GetDefaultAudioEndpoint(eRender) -> default Xbox audio device");
  return k_s_ok;
}
i32 TRT_GUEST_ABI enum_GetDevice(void* self, const u16* id, void** out_dev) {
  (void)self;
  (void)id;
  if (!out_dev) return k_e_invalidarg;
  *out_dev = &g_default_dev;
  return k_s_ok;
}
i32 TRT_GUEST_ABI enum_RegisterNotification(void* self, void* client) {
  (void)self;
  (void)client;
  return k_s_ok;
}
i32 TRT_GUEST_ABI enum_UnregisterNotification(void* self, void* client) {
  (void)self;
  (void)client;
  return k_s_ok;
}

void* k_enum_vtbl[8] = {
    (void*)&static_QI,
    (void*)&static_AddRef,
    (void*)&static_Release,
    (void*)&enum_EnumAudioEndpoints,
    (void*)&enum_GetDefaultAudioEndpoint,
    (void*)&enum_GetDevice,
    (void*)&enum_RegisterNotification,
    (void*)&enum_UnregisterNotification,
};
static_com_obj g_enumerator = {k_enum_vtbl};

}  // namespace

extern "C" {

// ---- mmdevapi.dll: the UWP-side audio activation entry point -------------
// Windows store titles (and Rust/wgpu games like Veloren) never open a
// WASAPI device through the COM enumerator; they call mmdevapi!
// ActivateAudioInterfaceAsync(VID, IID_IAudioClient, pfn, handler) and the
// OS asynchronously delivers IAudioClient through the completion handler's
// ActivateCompleted (IActivateAudioInterfaceCompletionHandler, slot 3).
// We activate synchronously against the real wasapi_client implementation
// and fire the handler's ActivateCompleted through trt_call_guest (same
// gate as the MF bridge's IMFAsyncCallback::Invoke) before returning S_OK.

u64 TRT_GUEST_ABI wasapi_GetActivationFactory(const u16* vid, void** out) {
  (void)vid;
  if (out) *out = nullptr;
  return (u64)k_e_nointerface;
}

i32 TRT_GUEST_ABI wasapi_ActivateAudioInterfaceAsync(
    const u16* device_interface_path, const void* riid, void* pfn,
    void* completion_handler, void** out_async_op) {
  (void)device_interface_path;
  (void)pfn;
  if (out_async_op) *out_async_op = nullptr;
  if (!riid || !completion_handler) return k_e_invalidarg;

  // Create the real client (the same object IMMDevice::Activate hands out)
  wasapi_client* c = alloc_client();
  if (!c) return k_e_outofmemory;

  // Deliver synchronously: ActivateCompleted(this, IAsyncOperation*) is
  // slot 3 of IActivateAudioInterfaceCompletionHandler. The async op is
  // null; games only test it for non-null before reading results, and the
  // GetResults path lives on the handler's caller side.
  bool delivered = false;
  const void** vt = *(const void***)completion_handler;
  if (vt && vt[3]) {
    trt_call_guest((void*)vt[3], (u64)(uintptr_t)completion_handler, 0, 0, 0);
    delivered = true;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "wasapi: ActivateAudioInterfaceAsync -> IAudioClient%s",
           delivered ? " (ActivateCompleted fired)" : " (no handler slot)");
  wasapilog::line(line);

  // S_OK even when the handler slot is absent: the client object exists
  // and stays referenced; a handler-less caller is a probing game.
  (void)delivered;
  return k_s_ok;
}

int trt_wasapi_create_enumerator(const void* iid, void** out, void* user) {
  (void)iid;
  (void)user;
  if (!out) return k_e_invalidarg;
  *out = &g_enumerator;
  wasapilog::line("wasapi: CoCreateInstance(CLSID_MMDeviceEnumerator) -> S_OK");
  return k_s_ok;
}

// ===========================================================================
// avrt.dll (Multimedia Class Scheduler Service — used by WASAPI audio threads)
// ===========================================================================

void* TRT_GUEST_ABI trt_AvSetMmThreadCharacteristicsW(const u16* task_name,
                                                      u32* task_index) {
  (void)task_name;
  if (task_index) *task_index = 1u;
  return (void*)(uintptr_t)0x41560001u;
}

void* TRT_GUEST_ABI trt_AvSetMmThreadCharacteristicsA(const char* task_name,
                                                      u32* task_index) {
  (void)task_name;
  if (task_index) *task_index = 1u;
  return (void*)(uintptr_t)0x41560001u;
}

int TRT_GUEST_ABI trt_AvSetMmThreadPriority(void* avrt_handle, int priority) {
  (void)avrt_handle;
  (void)priority;
  return 1;
}

int TRT_GUEST_ABI trt_AvRevertMmThreadCharacteristics(void* avrt_handle) {
  (void)avrt_handle;
  return 1;
}

int trt_bridge_register_wasapi(void) {
  trt_ole_register_class(k_clsid_mmdevice_enumerator,
                         &trt_wasapi_create_enumerator, nullptr);
  static const trt_named_fn k_avrt_fns[] = {
      {"AvSetMmThreadCharacteristicsW",
       (void*)&trt_AvSetMmThreadCharacteristicsW},
      {"AvSetMmThreadCharacteristicsA",
       (void*)&trt_AvSetMmThreadCharacteristicsA},
      {"AvSetMmThreadPriority", (void*)&trt_AvSetMmThreadPriority},
      {"AvRevertMmThreadCharacteristics",
       (void*)&trt_AvRevertMmThreadCharacteristics},
  };
  trt_module_register("avrt.dll", TRT_ROUTE_BUILTIN, k_avrt_fns,
                      (int)(sizeof(k_avrt_fns) / sizeof(k_avrt_fns[0])));
  // mmdevapi.dll: UWP audio activation (Veloren/wgpu path)
  static const trt_named_fn k_mmdevapi_fns[] = {
      {"ActivateAudioInterfaceAsync",
       (void*)&wasapi_ActivateAudioInterfaceAsync},
      {"GetActivationFactory", (void*)&wasapi_GetActivationFactory},
  };
  trt_module_register("mmdevapi.dll", TRT_ROUTE_BUILTIN, k_mmdevapi_fns,
                      (int)(sizeof(k_mmdevapi_fns) / sizeof(k_mmdevapi_fns[0])));
  return 0;
}

}  // extern "C"
