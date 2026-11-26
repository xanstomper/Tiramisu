// tiramisu/src/trt/bridge_audio.cpp
// XAudio2 for the guest.
//
// Games output sound through XAudio2: they create the engine, a mastering
// voice, source voices per sound, submit buffers and poll GetState. Without
// this bridge a game either crashes at startup or runs muted.
//
// The COM objects use the SDK's EXACT vtable layouts:
//   IXAudio2             3 + 8  slots
//   IXAudio2SourceVoice  3 + 30 slots (22..32 are the source-only ones)
// On the console the host publishes a real audio sink through
// trt_audio_set_hooks; the offline harness counts bytes.
#include "bridge_audio.h"
#include "bridge_ole.h"
#include "trt.h"

#include <atomic>
#include <deque>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int E_NOTIMPL_ = 0x80004001;
constexpr int S_OK_ = 0;

// ---- the host audio sink (the console fills this in) ----
struct audio_hooks {
  // a PCM buffer was submitted (pcm bytes, sample rate, channels, bytes/sample)
  int (*submit)(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user);
  void* user;
};
audio_hooks g_hooks = {nullptr, nullptr};

trt_audio_voice_submit_fn g_voice_hook = nullptr;
void* g_voice_user = nullptr;

} // namespace

extern "C" void trt_audio_set_hooks(int (*submit)(const void*, u32, u32, u32,
                                                  u32, void*),
                                    void* user) {
  g_hooks.submit = submit;
  g_hooks.user = user;
}

// getter for sibling bridges (openal drives the same sink)
extern "C" void trt_audio_get_hooks(void (**submit)(const void*, u32, u32, u32,
                                                    u32, void*),
                                    void** user) {
  if (submit) *submit = (void (*)(const void*, u32, u32, u32, u32, void*))g_hooks.submit;
  if (user) *user = g_hooks.user;
}

extern "C" void trt_audio_set_voice_hook(trt_audio_voice_submit_fn fn,
                                         void* user) {
  g_voice_hook = fn;
  g_voice_user = user;
}

extern "C" int trt_audio_submit_voice(u64 voice_id, const void* pcm, u32 bytes,
                                      u32 rate, u32 channels,
                                      u32 bytes_per_sample, u32 is_float,
                                      float volume) {
  if (!bytes) return 0;
  if (g_voice_hook) {
    g_voice_hook(voice_id, pcm, bytes, rate, channels, bytes_per_sample,
                 is_float, volume, g_voice_user);
    return 1;
  }
  return 0;
}

namespace {

// ---- objects ----
enum voice_kind { VK_MASTER = 1, VK_SOURCE = 2, VK_SUBMIX = 3 };

struct xaudio_voice {
  const void** vtbl;
  u32 refcount;
  u32 kind;              // voice_kind
  u32 started;
  u32 stopped;
  u32 buffers_queued;
  u64 samples_played;
  // the source format (WAVEFORMATEX)
  u16 format_tag;
  u16 channels;
  u32 sample_rate;
  u16 bits_per_sample;
  u32 is_float;
  float volume;
  u32 bytes_total;
};

struct xaudio_engine {
  const void** vtbl;
  u32 refcount;
  u32 started;
};

std::mutex g_voices_mu;
std::deque<xaudio_voice> g_voices;  // uncapped, pointer-stable across push_back
xaudio_engine g_engine_obj;
xaudio_engine g_engine27_obj;

xaudio_voice* alloc_voice() {
  std::lock_guard<std::mutex> lk(g_voices_mu);
  g_voices.emplace_back();
  xaudio_voice* v = &g_voices.back();
  memset(v, 0, sizeof(*v));
  v->volume = 1.0f;
  return v;
}

// forward decls for the vtables
extern const void* g_xaudio2_vtbl[13];
extern const void* g_xaudio27_vtbl[16];
extern const void* g_srcvoice_vtbl[33];

// ---- IUnknown on every object ----
u32 TRT_GUEST_ABI xq_AddRef(void* self) {
  if (!self) return 0;
  u32* r = (u32*)self;
  return ++r[2];
}
u32 TRT_GUEST_ABI xq_Release(void* self) {
  if (!self) return 0;
  u32* r = (u32*)self;
  return r[2] ? --r[2] : 0;
}
int TRT_GUEST_ABI xq_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (out) *out = self;
  return S_OK_;
}

// ---- IXAudio2 (slots 3..12 for 2.8/2.9, slots 3..15 for 2.7 COM) ----
int TRT_GUEST_ABI xe_RegisterForCallbacks(void*, void*) { return S_OK_; }
void TRT_GUEST_ABI xe_UnregisterForCallbacks(void*, void*) {}
int TRT_GUEST_ABI xe_CreateSourceVoice(void* self, void** out_voice,
                                       const void* format, u32 flags,
                                       float max_ratio, void* callback,
                                       const void* sends, u32 opset);
int TRT_GUEST_ABI xe_CreateSubmixVoice(void* self, void** out_voice,
                                       u32 channels, u32 rate, u32 flags,
                                       u32 processor, const void* sends,
                                       u32 opset) {
  (void)self; (void)flags; (void)processor; (void)sends; (void)opset;
  if (!out_voice) return E_NOTIMPL_;
  xaudio_voice* v = alloc_voice();
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_SUBMIX;
  v->channels = (u16)(channels ? channels : 2);
  v->sample_rate = rate ? rate : 48000;
  v->bits_per_sample = 32;
  v->is_float = 1;
  *out_voice = v;
  return S_OK_;
}
int TRT_GUEST_ABI xe_CreateMasteringVoice(void* self, void** out_voice,
                                          u32 channels, u32 rate, u32 flags,
                                          const u16* device_id,
                                          const void* effects,
                                          u8 category, u8 pipeline) {
  (void)self; (void)flags; (void)device_id; (void)effects;
  (void)category; (void)pipeline;
  if (!out_voice) return E_NOTIMPL_;
  xaudio_voice* v = alloc_voice();
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_MASTER;
  v->channels = (u16)(channels ? channels : 2);
  v->sample_rate = rate ? rate : 48000;
  v->bits_per_sample = 32;
  v->is_float = 1;
  *out_voice = v;
  return S_OK_;
}
int TRT_GUEST_ABI xe27_CreateMasteringVoice(void* self, void** out_voice,
                                            u32 channels, u32 rate, u32 flags,
                                            u32 dev_index, const void* effects) {
  (void)dev_index;
  return xe_CreateMasteringVoice(self, out_voice, channels, rate, flags,
                                 nullptr, effects, 0, 0);
}
int TRT_GUEST_ABI xe27_GetDeviceCount(void*, u32* count) {
  if (!count) return E_NOTIMPL_;
  *count = 1u;
  return S_OK_;
}
int TRT_GUEST_ABI xe27_GetDeviceDetails(void*, u32 index, void* details) {
  if (index != 0 || !details) return E_NOTIMPL_;
  // XAUDIO2_DEVICE_DETAILS: DeviceID[256] (512B), DisplayName[256] (512B),
  // Role (u32 @1024), WAVEFORMATEXTENSIBLE OutputFormat (40B @1028)
  memset(details, 0, 1068);
  const char* id = "default";
  const char* name = "Tiramisu Audio";
  u16* wid = (u16*)details;
  u16* wname = (u16*)((u8*)details + 512);
  for (int i = 0; id[i]; ++i) wid[i] = (u16)(u8)id[i];
  for (int i = 0; name[i]; ++i) wname[i] = (u16)(u8)name[i];
  *(u32*)((u8*)details + 1024) = 0xFu;  // GlobalDefaultDevice
  u16* wf = (u16*)((u8*)details + 1028);
  wf[0] = 0xFFFEu;  // WAVE_FORMAT_EXTENSIBLE
  wf[1] = 2u;       // channels
  *(u32*)((u8*)details + 1032) = 48000u;
  *(u32*)((u8*)details + 1036) = 48000u * 8u;
  wf[6] = 8u;       // nBlockAlign
  wf[7] = 32u;      // wBitsPerSample
  wf[8] = 22u;      // cbSize
  *(u32*)((u8*)details + 1048) = 0x3u;  // SPEAKER_FRONT_LEFT | RIGHT
  return S_OK_;
}
int TRT_GUEST_ABI xe27_Initialize(void*, u32, u32) { return S_OK_; }
int TRT_GUEST_ABI xe_StartEngine(void* self) {
  ((xaudio_engine*)self)->started = 1;
  return S_OK_;
}
void TRT_GUEST_ABI xe_StopEngine(void* self) {
  ((xaudio_engine*)self)->started = 0;
}
int TRT_GUEST_ABI xe_CommitChanges(void*, u32) { return S_OK_; }
void TRT_GUEST_ABI xe_GetPerformanceData(void*, void* perf) {
  if (perf) memset(perf, 0, 64);
}
void TRT_GUEST_ABI xe_SetDebugConfiguration(void*, const void*, void*) {}

const void* g_xaudio2_vtbl[13] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xq_Release,
    (const void*)&xe_RegisterForCallbacks,
    (const void*)&xe_UnregisterForCallbacks,
    (const void*)&xe_CreateSourceVoice,
    (const void*)&xe_CreateSubmixVoice,
    (const void*)&xe_CreateMasteringVoice,
    (const void*)&xe_StartEngine,
    (const void*)&xe_StopEngine,
    (const void*)&xe_CommitChanges,
    (const void*)&xe_GetPerformanceData,
    (const void*)&xe_SetDebugConfiguration,
};

const void* g_xaudio27_vtbl[16] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xq_Release,
    (const void*)&xe27_GetDeviceCount,
    (const void*)&xe27_GetDeviceDetails,
    (const void*)&xe27_Initialize,
    (const void*)&xe_RegisterForCallbacks,
    (const void*)&xe_UnregisterForCallbacks,
    (const void*)&xe_CreateSourceVoice,
    (const void*)&xe_CreateSubmixVoice,
    (const void*)&xe27_CreateMasteringVoice,
    (const void*)&xe_StartEngine,
    (const void*)&xe_StopEngine,
    (const void*)&xe_CommitChanges,
    (const void*)&xe_GetPerformanceData,
    (const void*)&xe_SetDebugConfiguration,
};

// ---- IXAudio2Voice base (slots 3..21 shared by every voice) ----
int TRT_GUEST_ABI xv_GetVoiceDetails(void* self, void* details) {
  (void)self;
  if (!details) return E_NOTIMPL_;
  u32* d = (u32*)details;
  d[0] = 20;  // SizeOf
  d[1] = 1;   // InputChannels
  d[2] = 48000;
  d[3] = 64;   // CreationFlags
  d[4] = 0;    // Flags
  return S_OK_;
}
int TRT_GUEST_ABI xv_SetOutputVoices(void*, const void*) { return S_OK_; }
int TRT_GUEST_ABI xv_SetEffectChain(void*, const void*) { return S_OK_; }
int TRT_GUEST_ABI xv_EnableEffect(void*, u32, u32, u32) { return S_OK_; }
int TRT_GUEST_ABI xv_DisableEffect(void*, u32, u32, u32) { return S_OK_; }
void TRT_GUEST_ABI xv_GetEffectState(void*, u32, u32* enabled) {
  if (enabled) *enabled = 0;
}
int TRT_GUEST_ABI xv_SetEffectParameters(void*, u32, const void*, u32, u32) {
  return S_OK_;
}
int TRT_GUEST_ABI xv_GetEffectParameters(void*, u32, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI xv_SetFilterParameters(void*, const void*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetFilterParameters(void*, void*) {}
int TRT_GUEST_ABI xv_SetOutputFilterParameters(void*, void*, const void*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetOutputFilterParameters(void*, void*, void*) {}
int TRT_GUEST_ABI xv_SetVolume(void* self, float volume, u32 opset) {
  (void)opset;
  if (self) ((xaudio_voice*)self)->volume = volume;
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetVolume(void* self, float* out) {
  if (out) *out = self ? ((xaudio_voice*)self)->volume : 1.0f;
}
int TRT_GUEST_ABI xv_SetChannelVolumes(void*, u32, const float*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetChannelVolumes(void*, u32, float* out) {
  if (out) out[0] = 1.0f;
}
int TRT_GUEST_ABI xv_SetOutputMatrix(void*, void*, u32, u32, const float*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetOutputMatrix(void*, void*, u32, u32, float* out) {
  if (out) out[0] = 1.0f;
}
void TRT_GUEST_ABI xv_DestroyVoice(void*) {}

// ---- IXAudio2SourceVoice (slots 22..32) ----
int TRT_GUEST_ABI xv_Start(void* self, u32 flags, u32 opset) {
  (void)flags; (void)opset;
  xaudio_voice* v = (xaudio_voice*)self;
  v->started = 1;
  v->stopped = 0;
  static std::atomic<unsigned> starts{0};
  if (starts.fetch_add(1, std::memory_order_relaxed) < 16) {
    char line[128];
    snprintf(line, sizeof(line), "audio: guest XAudio Start rate=%u ch=%u",
             v->sample_rate, (unsigned)v->channels);
    trt_core_log(line);
  }
  return S_OK_;
}
int TRT_GUEST_ABI xv_Stop(void* self, u32 flags, u32 opset) {
  (void)flags; (void)opset;
  ((xaudio_voice*)self)->stopped = 1;
  return S_OK_;
}
// XAUDIO2_BUFFER: Flags, AudioBytes, pAudioData, PlayBegin, PlayLength,
//                LoopBegin, LoopLength, LoopCount, pContext
struct x2_buffer {
  u32 flags;
  u32 audio_bytes;
  const void* audio_data;
  u32 play_begin;
  u32 play_length;
  u32 loop_begin;
  u32 loop_length;
  u32 loop_count;
  void* context;
};
int TRT_GUEST_ABI xv_SubmitSourceBuffer(void* self, const x2_buffer* buf,
                                        const void* wma) {
  (void)wma;
  xaudio_voice* v = (xaudio_voice*)self;
  if (!buf) return E_NOTIMPL_;
  ++v->buffers_queued;
  v->bytes_total += buf->audio_bytes;
  // hand the PCM to the host's multi-voice mixer (or legacy hook for offline tests)
  if (buf->audio_data && buf->audio_bytes > 0) {
    static std::atomic<unsigned> submits{0};
    if (submits.fetch_add(1, std::memory_order_relaxed) < 16) {
      char line[144];
      snprintf(line, sizeof(line),
               "audio: guest XAudio Submit bytes=%u rate=%u ch=%u started=%u",
               buf->audio_bytes, v->sample_rate, (unsigned)v->channels,
               (unsigned)v->started);
      trt_core_log(line);
    }
    const u32 bps = v->bits_per_sample >= 8 ? (v->bits_per_sample / 8) : 2;
    if (!trt_audio_submit_voice((u64)(uintptr_t)v, buf->audio_data,
                                buf->audio_bytes,
                                v->sample_rate ? v->sample_rate : 48000,
                                v->channels ? v->channels : 2, bps,
                                v->is_float, v->volume)) {
      if (g_hooks.submit) {
        g_hooks.submit(buf->audio_data, buf->audio_bytes, v->sample_rate,
                       v->channels, bps, g_hooks.user);
      }
    }
    const u32 frame_bytes = (v->channels ? v->channels : 2) * bps;
    if (frame_bytes) v->samples_played += buf->audio_bytes / frame_bytes;
  }
  return S_OK_;
}
int TRT_GUEST_ABI xv_FlushSourceBuffers(void* self) {
  ((xaudio_voice*)self)->buffers_queued = 0;
  return S_OK_;
}
int TRT_GUEST_ABI xv_Discontinuity(void*) { return S_OK_; }
int TRT_GUEST_ABI xv_ExitLoop(void*, u32) { return S_OK_; }
// XAUDIO2_VOICE_STATE: pCurrentBufferContext, BuffersQueued, SamplesPlayed
void TRT_GUEST_ABI xv_GetState(void* self, void* state, u32 flags) {
  (void)flags;
  xaudio_voice* v = (xaudio_voice*)self;
  if (!state) return;
  void** s = (void**)state;
  s[0] = nullptr;                    // pCurrentBufferContext
  u32* q = (u32*)state;
  q[2] = v->buffers_queued;          // BuffersQueued (offset 8)
  u64* sp = (u64*)state;
  sp[2] = v->samples_played;         // SamplesPlayed (offset 16)
}
int TRT_GUEST_ABI xv_SetFrequencyRatio(void*, float, u32) { return S_OK_; }
void TRT_GUEST_ABI xv_GetFrequencyRatio(void*, float* out) {
  if (out) *out = 1.0f;
}
int TRT_GUEST_ABI xv_SetSourceSampleRate(void* self, u32 rate) {
  ((xaudio_voice*)self)->sample_rate = rate;
  return S_OK_;
}
int TRT_GUEST_ABI xv_GetChannelMask(void* self, u32* mask) {
  (void)self;
  if (mask) *mask = 0x3;  // FRONT_LEFT | FRONT_RIGHT
  return S_OK_;
}

const void* g_srcvoice_vtbl[33] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xq_Release,
    (const void*)&xv_GetVoiceDetails,
    (const void*)&xv_SetOutputVoices,
    (const void*)&xv_SetEffectChain,
    (const void*)&xv_EnableEffect,
    (const void*)&xv_DisableEffect,
    (const void*)&xv_GetEffectState,
    (const void*)&xv_SetEffectParameters,
    (const void*)&xv_GetEffectParameters,
    (const void*)&xv_SetFilterParameters,
    (const void*)&xv_GetFilterParameters,
    (const void*)&xv_SetOutputFilterParameters,
    (const void*)&xv_GetOutputFilterParameters,
    (const void*)&xv_SetVolume,
    (const void*)&xv_GetVolume,
    (const void*)&xv_SetChannelVolumes,
    (const void*)&xv_GetChannelVolumes,
    (const void*)&xv_SetOutputMatrix,
    (const void*)&xv_GetOutputMatrix,
    (const void*)&xv_DestroyVoice,
    (const void*)&xv_Start,
    (const void*)&xv_Stop,
    (const void*)&xv_SubmitSourceBuffer,
    (const void*)&xv_FlushSourceBuffers,
    (const void*)&xv_Discontinuity,
    (const void*)&xv_ExitLoop,
    (const void*)&xv_GetState,
    (const void*)&xv_SetFrequencyRatio,
    (const void*)&xv_GetFrequencyRatio,
    (const void*)&xv_SetSourceSampleRate,
    (const void*)&xv_GetChannelMask,
};

int TRT_GUEST_ABI xe_CreateSourceVoice(void* self, void** out_voice,
                                       const void* format, u32 flags,
                                       float max_ratio, void* callback,
                                       const void* sends, u32 opset) {
  (void)self; (void)flags; (void)max_ratio; (void)callback; (void)sends;
  (void)opset;
  if (!out_voice) return E_NOTIMPL_;
  xaudio_voice* v = alloc_voice();
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_SOURCE;
  if (format) {
    // WAVEFORMATEX: tag, channels, rate, avg, align, bits, cbSize
    const u16* w = (const u16*)format;
    v->format_tag = w[0];
    v->channels = w[1] ? w[1] : 2;
    v->sample_rate = *(const u32*)(const void*)(w + 2);
    if (!v->sample_rate) v->sample_rate = 48000;
    v->bits_per_sample = w[5] ? w[5] : 16;
    if (v->format_tag == 3 /*WAVE_FORMAT_IEEE_FLOAT*/) {
      v->is_float = 1;
    } else if (v->format_tag == 0xFFFEu && w[8] >= 22) {
      const u8* subfmt = (const u8*)format + 24;
      v->is_float = (subfmt[0] == 3 && subfmt[1] == 0 && subfmt[2] == 0 &&
                     subfmt[3] == 0)
                        ? 1u
                        : 0u;
    }
  } else {
    v->channels = 2;
    v->sample_rate = 48000;
    v->bits_per_sample = 16;
  }
  *out_voice = v;
  static std::atomic<unsigned> creates{0};
  if (creates.fetch_add(1, std::memory_order_relaxed) < 16) {
    char line[144];
    snprintf(line, sizeof(line),
             "audio: guest XAudio CreateSourceVoice rate=%u ch=%u bits=%u tag=%u",
             v->sample_rate, (unsigned)v->channels, (unsigned)v->bits_per_sample,
             (unsigned)v->format_tag);
    trt_core_log(line);
  }
  return S_OK_;
}

// ---- CLSID_XAudio2 COM factory (XAudio 2.7 / DirectX June 2010) ----
int xaudio27_com_factory(const void* iid, void** out, void* user) {
  (void)iid; (void)user;
  if (!out) return E_NOTIMPL_;
  memset(&g_engine27_obj, 0, sizeof(g_engine27_obj));
  g_engine27_obj.vtbl = g_xaudio27_vtbl;
  g_engine27_obj.refcount = 1;
  *out = &g_engine27_obj;
  return S_OK_;
}

// ---- X3DAudio1_7 (stereo attenuation + pan + Doppler) ----
struct x3d_vec { float x, y, z; };
struct x3d_listener {
  x3d_vec front;
  x3d_vec top;
  x3d_vec pos;
  x3d_vec vel;
  const void* cone;
};
struct x3d_emitter {
  const void* cone;
  x3d_vec front;
  x3d_vec top;
  x3d_vec pos;
  x3d_vec vel;
  float inner_radius;
  float inner_angle;
  u32 channel_count;
  float channel_radius;
  const float* channel_azimuths;
  const void* vol_curve;
  const void* lfe_curve;
  const void* lpf_direct;
  const void* lpf_reverb;
  const void* reverb_curve;
  float curve_dist_scaler;
  float doppler_scaler;
};
struct x3d_dsp {
  float* matrix_coeffs;
  float* delay_times;
  u32 src_channels;
  u32 dst_channels;
  float lpf_direct;
  float lpf_reverb;
  float reverb_level;
  float doppler_factor;
  float angle;
  float dist;
};

int TRT_GUEST_ABI trt_X3DAudioInitialize(u32 channel_mask, float speed_of_sound,
                                         u8* instance) {
  if (!instance) return E_NOTIMPL_;
  memset(instance, 0, 20);
  memcpy(instance + 0, &channel_mask, 4);
  memcpy(instance + 4, &speed_of_sound, 4);
  return S_OK_;
}

void TRT_GUEST_ABI trt_X3DAudioCalculate(const u8*, const x3d_listener* l,
                                         const x3d_emitter* e, u32,
                                         x3d_dsp* dsp) {
  if (!dsp) return;
  float dx = 0, dy = 0, dz = 0;
  if (l && e) {
    dx = e->pos.x - l->pos.x;
    dy = e->pos.y - l->pos.y;
    dz = e->pos.z - l->pos.z;
  }
  const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
  const float scale = (e && e->curve_dist_scaler > 0.0f) ? e->curve_dist_scaler : 1.0f;
  const float atten = (dist <= scale) ? 1.0f : (scale / dist);
  // Right vector = OrientTop x OrientFront (LH) or cross sign clamped to [-1,1]
  float pan = 0.0f;
  if (l && dist > 1e-4f) {
    const float rx = l->top.y * l->front.z - l->top.z * l->front.y;
    const float ry = l->top.z * l->front.x - l->top.x * l->front.z;
    const float rz = l->top.x * l->front.y - l->top.y * l->front.x;
    pan = (rx * dx + ry * dy + rz * dz) / dist;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
  }
  const float g_l = atten * sqrtf(0.5f * (1.0f - pan));
  const float g_r = atten * sqrtf(0.5f * (1.0f + pan));
  dsp->dist = dist;
  dsp->angle = 0.0f;
  dsp->doppler_factor = 1.0f;
  dsp->lpf_direct = 1.0f;
  dsp->lpf_reverb = 1.0f;
  dsp->reverb_level = 0.0f;
  if (dsp->matrix_coeffs && dsp->src_channels > 0 && dsp->dst_channels > 0) {
    for (u32 dst = 0; dst < dsp->dst_channels; ++dst) {
      for (u32 src = 0; src < dsp->src_channels; ++src) {
        float g = (dst == 0) ? g_l : ((dst == 1) ? g_r : atten * 0.5f);
        dsp->matrix_coeffs[dst * dsp->src_channels + src] = g;
      }
    }
  }
}

// ---- XAPOFX1_5 (CreateFX minimal IXAPO COM effect) ----
struct xapo_obj {
  const void** vtbl;
  u32 refcount;
};
extern const void* g_xapo_vtbl[14];
u32 TRT_GUEST_ABI xapo_Release(void* self) {
  auto* x = (xapo_obj*)self;
  if (!x) return 0;
  if (x->refcount && --x->refcount == 0) { delete x; return 0; }
  return x->refcount;
}
int TRT_GUEST_ABI xapo_GetRegProps(void*, void** props) {
  if (props) *props = nullptr;
  return S_OK_;
}
int TRT_GUEST_ABI xapo_IsInputSupported(void*, const void*, const void*, int* sup) {
  if (sup) *sup = 1;
  return S_OK_;
}
int TRT_GUEST_ABI xapo_IsOutputSupported(void*, const void*, const void*, int* sup) {
  if (sup) *sup = 1;
  return S_OK_;
}
int TRT_GUEST_ABI xapo_Initialize(void*, const void*, u32) { return S_OK_; }
void TRT_GUEST_ABI xapo_Reset(void*) {}
int TRT_GUEST_ABI xapo_LockForProcess(void*, u32, const void*, u32, const void*) {
  return S_OK_;
}
void TRT_GUEST_ABI xapo_UnlockForProcess(void*) {}
void TRT_GUEST_ABI xapo_Process(void*, u32, const void*, u32, void*, int) {}
u32 TRT_GUEST_ABI xapo_CalcFrames(void*, u32 n) { return n; }
const void* g_xapo_vtbl[14] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xapo_Release,
    (const void*)&xapo_GetRegProps,
    (const void*)&xapo_IsInputSupported,
    (const void*)&xapo_IsOutputSupported,
    (const void*)&xapo_Initialize,
    (const void*)&xapo_Reset,
    (const void*)&xapo_LockForProcess,
    (const void*)&xapo_UnlockForProcess,
    (const void*)&xapo_Process,
    (const void*)&xapo_CalcFrames,
    (const void*)&xapo_CalcFrames,
    (const void*)&xapo_Initialize,
};

int TRT_GUEST_ABI trt_CreateFX(const u8*, void** out, const void*, u32) {
  if (!out) return E_NOTIMPL_;
  auto* x = new xapo_obj{g_xapo_vtbl, 1};
  *out = x;
  return S_OK_;
}

} // namespace

extern "C" {

// XAudio2Create(out_engine, flags, processor)
int TRT_GUEST_ABI trt_XAudio2Create(void** out_engine, u32 flags, u32 processor) {
  (void)flags; (void)processor;
  if (!out_engine) return E_NOTIMPL_;
  memset(&g_engine_obj, 0, sizeof(g_engine_obj));
  g_engine_obj.vtbl = g_xaudio2_vtbl;
  g_engine_obj.refcount = 1;
  {
    std::lock_guard<std::mutex> lk(g_voices_mu);
    g_voices.clear();
  }
  *out_engine = &g_engine_obj;
  return 0;
}

// ---- test hooks: the guest proves its PCM reached the sink ----
u32 TRT_GUEST_ABI trt_audio_note_submit(u32 bytes) {
  if (g_hooks.submit) {
    g_hooks.submit(nullptr, bytes, 48000, 2, 2, g_hooks.user);
  }
  return bytes;
}
void TRT_GUEST_ABI trt_audio_note_start(void) {}

int trt_bridge_register_audio(void) {
  // XAudio 2.7 COM CLSIDs ({5a508685-a254-4fba-9b82-9a24b00306af} & debug)
  static const u8 k_clsid_xaudio2[16] = {
      0x85, 0x86, 0x50, 0x5A, 0x54, 0xA2, 0xBA, 0x4F,
      0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF};
  static const u8 k_clsid_xaudio2_dbg[16] = {
      0x35, 0xEA, 0x05, 0xDB, 0x29, 0x03, 0x4B, 0x4D,
      0xA5, 0x3A, 0x6D, 0xEA, 0xD0, 0x3D, 0x38, 0x52};
  trt_ole_register_class(k_clsid_xaudio2, &xaudio27_com_factory, nullptr);
  trt_ole_register_class(k_clsid_xaudio2_dbg, &xaudio27_com_factory, nullptr);

  static const trt_named_fn k_fns[] = {
      {"XAudio2Create", (void*)&trt_XAudio2Create},
      {"XAudio2CreateWithVersionInfo", (void*)&trt_XAudio2Create},
      {"X3DAudioInitialize", (void*)&trt_X3DAudioInitialize},
      {"X3DAudioCalculate", (void*)&trt_X3DAudioCalculate},
      {"CreateFX", (void*)&trt_CreateFX},
      {"trt_audio_note_submit", (void*)&trt_audio_note_submit},
      {"trt_audio_note_start", (void*)&trt_audio_note_start},
  };
  static const char* const k_xa_dlls[] = {
      "xaudio2_9.dll", "xaudio2_8.dll", "xaudio2_7.dll", "xaudio2_6.dll",
      "xaudio2_5.dll", "xaudio2_4.dll", "xaudio2_3.dll", "xaudio2_2.dll",
      "xaudio2_1.dll", "xaudio2_0.dll", "xaudio2.dll",
  };
  for (const char* name : k_xa_dlls) {
    trt_module_register(name, TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  }

  static const trt_named_fn k_x3d_fns[] = {
      {"X3DAudioInitialize", (void*)&trt_X3DAudioInitialize},
      {"X3DAudioCalculate", (void*)&trt_X3DAudioCalculate},
  };
  static const char* const k_x3d_dlls[] = {
      "x3daudio1_7.dll", "x3daudio1_6.dll", "x3daudio1_5.dll", "x3daudio1_4.dll",
  };
  for (const char* name : k_x3d_dlls) {
    trt_module_register(name, TRT_ROUTE_BUILTIN, k_x3d_fns,
                        (int)(sizeof(k_x3d_fns) / sizeof(k_x3d_fns[0])));
  }

  static const trt_named_fn k_fx_fns[] = {
      {"CreateFX", (void*)&trt_CreateFX},
  };
  static const char* const k_fx_dlls[] = {
      "xapofx1_5.dll", "xapofx1_4.dll", "xapofx1_3.dll",
      "xapofx1_2.dll", "xapofx1_1.dll",
  };
  for (const char* name : k_fx_dlls) {
    trt_module_register(name, TRT_ROUTE_BUILTIN, k_fx_fns,
                        (int)(sizeof(k_fx_fns) / sizeof(k_fx_fns[0])));
  }
  return 0;
}

} // extern "C"
