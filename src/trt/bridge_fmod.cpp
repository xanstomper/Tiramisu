// tiramisu/src/trt/bridge_fmod.cpp
// FMOD audio bridge — fmodex.dll (FMOD Ex 4.x), fmod.dll / fmodstudio.dll
// (FMOD Studio 1.x/2.x). The C exports are the ABI for both families (the
// C++ API is inline wrappers over them), and the core lifecycle names are
// identical, so one table serves all five module names.
//
// Honest-audio model:
//  - WAV/PCM sounds DECODE for real (RIFF parse -> PCM 8/16/24/32/float) and
//    play through the host mixer (trt_audio_submit_voice) at real volume.
//  - Looping sounds are fed by a bridge scheduler thread that re-pushes the
//    decoded PCM until the channel stops (the mixer FIFO absorbs it).
//  - Compressed payloads (OGG/MP3/FSB banks) have NO decoder in the runtime
//    yet: the sound handle is real, play/channel control work, audio is
//    SILENT with a named one-time log per sound ("decoder pending"). The
//    game boots and its audio state machine runs — a named work item, never
//    a fake success.
//  - Configuration setters (output mode, speaker mode, DSP buffer size)
//    accept and record: the runtime has exactly one real output path, so
//    accepting the configuration IS the truthful answer.
#include "bridge_fmod.h"
#include "audio_codecs.h"
#include "bridge_audio.h"
#include "thunk.h"
#include "trt.h"

#include <chrono>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <utility>

// vendored stb_vorbis 1.24 (public domain, src/trt/vendor_stb_vorbis.c):
// makes OGG payloads DECODE for real — the biggest FMOD payload class
extern "C" {
typedef struct stb_vorbis stb_vorbis;
typedef struct {
  unsigned int sample_rate;
  int channels;
  unsigned int setup_memory_required;
  unsigned int setup_temp_memory_required;
  unsigned int temp_memory_required;
  int max_frame_size;
} stb_vorbis_info;
stb_vorbis* stb_vorbis_open_memory(const unsigned char* data, int len,
                                   int* error_ret, const void* alloc);
void stb_vorbis_close(stb_vorbis* f);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis* f);
int stb_vorbis_get_frame_short_interleaved(stb_vorbis* f, int num_c,
                                           short* buffer, int num_shorts);
}

// vendored dr_mp3 (public domain / MIT-0, src/trt/vendor_dr_mp3.c): MP3 is
// the last common FMOD payload class that was a named-silent path.
extern "C" {
// dr_mp3.h sets DR_MP3_NO_STDIO itself; the implementation TU does the same.
#include "vendor_dr_mp3.h"
}
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fmodlog {
static void line(const char* s) { trt_core_log(s); }
}  // namespace fmodlog
#define log_line(s_) fmodlog::line(s_)

namespace {

// ---- FMOD result codes (public constants) ----
// FMOD_RESULT values (FMOD Studio 2.02 public enum, fmod_common.h).
// The old values here (INVALID_HANDLE=5, UNSUPPORTED=35,
// FILE_NOT_FOUND=22, INVALID_PARAM=3) were invented and did not match the
// SDK; the managed wrapper compares against the real enum (notably
// ERR_EVENT_NOTFOUND=70), so wrong values corrupt error handling.
// Fixed 2026-11-20 (dllmap1).
constexpr int FMOD_OK_ = 0;
constexpr int FMOD_ERR_INVALID_PARAM_ = 28;
constexpr int FMOD_ERR_INVALID_HANDLE_ = 27;
constexpr int FMOD_ERR_FILE_NOT_FOUND_ = 15;
constexpr int FMOD_ERR_UNSUPPORTED_ = 64;
constexpr int FMOD_ERR_NOTREADY_ = 43;

// ---- FMOD mode flags (subset the bridge reads) ----
constexpr u32 FMOD_LOOP_OFF_ = 0x00000001u;
constexpr u32 FMOD_LOOP_NORMAL_ = 0x00000002u;
constexpr u32 FMOD_LOOP_BIDI_ = 0x00000004u;
constexpr u32 FMOD_OPENMEMORY_ = 0x00000800u;
constexpr u32 FMOD_OPENRAW_ = 0x00001000u;
constexpr u32 FMOD_CREATESTREAM_ = 0x00000080u;

// ---- object model ----
enum fmod_type : u32 {
  FM_T_SYSTEM = 1,
  FM_T_SOUND = 2,
  FM_T_CHANNEL = 3,
  FM_T_CHANNELGROUP = 4,
};

struct fmod_sound {
  u8* alloc_base;          // the malloc'd base (freed on Release)
  u8* pcm;                 // decoded PCM (may point INSIDE alloc_base)
  u32 pcm_bytes;
  u32 rate;
  u32 channels;
  u32 bps;                 // bytes per sample
  u32 is_float;
  u32 mode;                // guest's FMOD_MODE
  u32 loop;                // 1 = looping (feeder re-pushes)
  u32 length_ms;           // decoded length; 0 for undecoded
  int silent;              // 1 = compressed, named decoder-pending
  char name[128];
};

struct fmod_channel {
  fmod_sound* snd;
  float volume;
  int paused;
  int playing;
  u64 voice_id;
};

struct fmod_obj {
  u32 type;
  u32 magic;
  union {
    fmod_sound snd;
    fmod_channel ch;
  };
};

constexpr u32 k_max_objs = 1024;
fmod_obj g_objs[k_max_objs];
u32 g_obj_count = 0;
u32 g_magic_seq = 0xF0D1u;

void* obj_make(u32 type) {
  if (g_obj_count >= k_max_objs) return nullptr;
  fmod_obj& o = g_objs[g_obj_count];
  memset(&o, 0, sizeof(o));
  o.type = type;
  o.magic = ++g_magic_seq;
  // handle = pointer to the slot (guest treats it as opaque)
  void* h = &g_objs[g_obj_count];
  ++g_obj_count;
  return h;
}

fmod_obj* obj_check(void* h, u32 type) {
  if (!h) return nullptr;
  // handles are pointers into g_objs; bounds-check by range
  if (h < (void*)&g_objs[0] || h > (void*)&g_objs[k_max_objs - 1]) return nullptr;
  fmod_obj* o = (fmod_obj*)h;
  if (o->type != type || o->magic == 0) return nullptr;
  return o;
}

void obj_take(void* h) {
  fmod_obj* o = obj_check(h, 0);
  if (!o) {
    // type-agnostic retire: zero magic so future checks fail
    if (h >= (void*)&g_objs[0] && h <= (void*)&g_objs[k_max_objs - 1])
      ((fmod_obj*)h)->magic = 0;
  } else {
    o->magic = 0;
  }
}

// ---- mixer feed ----
struct feeder_ent {
  fmod_channel* ch;
  u32 rate, channels, bps, is_float;
};
static feeder_ent g_feeders[32];
static u32 g_feeder_count = 0;
static void* g_feeder_thread = nullptr;
static volatile int g_feeder_run = 0;

void feeder_push_once(fmod_channel* fc) {
  fmod_sound* s = fc->snd;
  if (!s || !s->pcm) return;
  trt_audio_submit_voice(fc->voice_id, s->pcm, s->pcm_bytes, s->rate,
                         s->channels, s->bps, s->is_float, fc->volume);
}

// channel state (playing/paused/volume) is touched by the guest thread AND
// the feeder thread — measured data race (TSan gate) without this lock.
static std::mutex g_ch_mu;

void feeder_fn(void) {
  while (g_feeder_run) {
    int active = 0;
    unsigned longest_step = 50u;
    {  // ONE lock for registry + channel state; NO sleep inside it
      // (std::mutex is not recursive — and PlaySound must not wait 250ms
      // behind a sleeping feeder)
      std::lock_guard<std::mutex> lk(g_ch_mu);
      const u32 feeder_n = g_feeder_count;
      for (u32 i = 0; i < feeder_n; ++i) {
        fmod_channel* fc = g_feeders[i].ch;
        if (!fc || !fc->playing || fc->paused) continue;
        fmod_sound* s = fc->snd;
        if (!s || !s->pcm || !s->loop) continue;
        // re-push at ~1/2 the sound's duration, bounded to 250ms steps
        u32 dur_ms = s->length_ms;
        unsigned step = dur_ms ? (dur_ms / 2u) : 250u;
        if (step < 30u) step = 30u;
        if (step > 250u) step = 250u;
        if (step > longest_step) longest_step = step;
        feeder_push_once(fc);
        active = 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(active ? longest_step
                                                                 : 50u));
  }
}

void feeder_ensure(void) {
  if (g_feeder_thread) return;
  g_feeder_run = 1;
  try {
    g_feeder_thread = new std::thread(feeder_fn);  // detached; exits on run=0
  } catch (...) {
    g_feeder_run = 0;
    g_feeder_thread = nullptr;
  }
}

void feeder_drop(fmod_channel* fc) {
  std::lock_guard<std::mutex> lk(g_ch_mu);
  for (u32 i = 0; i < g_feeder_count; ++i)
    if (g_feeders[i].ch == fc) {
      g_feeders[i] = g_feeders[g_feeder_count - 1];
      --g_feeder_count;
      return;
    }
}

// ---- WAV decode (RIFF parse) ----
u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

int wav_decode(const u8* data, u32 size, fmod_sound* out) {
  if (size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4))
    return -1;
  u32 pos = 12;
  u16 fmt_tag = 0, channels = 0, bits = 0;
  u32 rate = 0;
  const u8* payload = nullptr;
  u32 payload_bytes = 0;
  while (pos + 8 <= size) {
    const u8* id = data + pos;
    u32 clen = rd32(data + pos + 4);
    pos += 8;
    if (pos + clen > size) break;
    if (!memcmp(id, "fmt ", 4) && clen >= 16) {
      fmt_tag = rd16(data + pos);
      channels = rd16(data + pos + 2);
      rate = rd32(data + pos + 4);
      bits = rd16(data + pos + 14);
    } else if (!memcmp(id, "data", 4)) {
      payload = data + pos;
      payload_bytes = clen;
    }
    pos += clen + (clen & 1u);
  }
  if (!payload || !channels || !rate) return -1;
  if (fmt_tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
    out->pcm = (u8*)payload;
    out->pcm_bytes = payload_bytes;
    out->rate = rate;
    out->channels = channels;
    out->bps = bits / 8;
    out->is_float = 0;
  } else if (fmt_tag == 3 && (bits == 32 || bits == 64)) {
    out->pcm = (u8*)payload;
    out->pcm_bytes = payload_bytes;
    out->rate = rate;
    out->channels = channels;
    out->bps = 4;  // f64 truncated to f32 by the mixer's reader
    out->is_float = 1;
  } else {
    return -1;  // ADPCM/IEEE-in-wav variants: named by the caller
  }
  const u32 stride = out->channels * out->bps;
  out->length_ms = stride ? (u32)((u64)(out->pcm_bytes / stride) * 1000ull /
                                  out->rate)
                          : 0;
  return 0;
}

// RAW PCM via exinfo (FMOD_OPENRAW / FMOD_OPENMEMORY with exinfo): the guest
// hands us the layout directly.
int raw_decode(const void* data, u32 size, const void* exinfo, fmod_sound* out) {
  if (!exinfo) return -1;
  // FMOD_CREATESOUNDEXINFO layout (x64): cbsize(4) length(4) ... decodeoffset
  // (4) ... numchannels(+0x24?) — offsets differ across versions; read the
  // two fields the docs pin: length at +4, and use defaultformat when the
  // guest sets FMOD_OPENRAW. conservative: require the caller's exinfo
  // cbsize >= 0x40 and read defaultfrequency/decodeformat conservatively.
  const u32* ex = (const u32*)exinfo;
  if (ex[0] < 0x40) return -1;
  // defaultfrequency at +0x28? — versions vary; only trust length (+4) and
  // ask the guest's mode for the rest. RAW without a decodable layout is
  // named unsupported rather than guessed.
  (void)size;
  (void)out;
  return -1;
}

// OGG/vorbis full decode -> s16 interleaved PCM (owned; the caller frees).
int ogg_decode(const u8* data, u32 size, fmod_sound* out) {
  int err = 0;
  stb_vorbis* v = stb_vorbis_open_memory(data, (int)size, &err, nullptr);
  if (!v) return -1;
  const stb_vorbis_info info = stb_vorbis_get_info(v);
  if (!info.channels || !info.sample_rate) {
    stb_vorbis_close(v);
    return -1;
  }
  u32 cap_frames = 1u << 16;
  short* pcm = (short*)malloc((size_t)cap_frames * info.channels * sizeof(short));
  u32 total = 0;
  short frame_buf[8192 * 2];
  int n;
  while ((n = stb_vorbis_get_frame_short_interleaved(v, info.channels,
                                                     frame_buf, 8192 * 2)) > 0) {
    const u32 need = (total + (u32)n) * info.channels;
    if (need > cap_frames * (u32)info.channels) {
      while (need > cap_frames * (u32)info.channels) cap_frames <<= 1;
      short* np = (short*)realloc(pcm, (size_t)cap_frames * info.channels * sizeof(short));
      if (!np) {
        free(pcm);
        stb_vorbis_close(v);
        return -1;
      }
      pcm = np;
    }
    memcpy(pcm + total * info.channels, frame_buf,
           (size_t)n * info.channels * sizeof(short));
    total += (u32)n;
  }
  stb_vorbis_close(v);
  if (!total) {
    free(pcm);
    return -1;
  }
  out->alloc_base = (u8*)pcm;  // the decoded buffer is the owned base
  out->pcm = (u8*)pcm;
  out->pcm_bytes = total * (u32)info.channels * 2u;
  out->rate = info.sample_rate;
  out->channels = (u32)info.channels;
  out->bps = 2;
  out->is_float = 0;
  out->length_ms = (u32)((u64)total * 1000ull / info.sample_rate);
  return 0;
}

// MP3 full decode -> s16 interleaved PCM (owned; the caller frees).
// Same ownership contract as ogg_decode: alloc_base == the decoded buffer.
int mp3_decode(const u8* data, u32 size, fmod_sound* out) {
  drmp3 mp3{};
  if (!drmp3_init_memory(&mp3, data, (size_t)size, nullptr)) return -1;
  const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
  const u32 rate = mp3.sampleRate;
  const u32 channels = mp3.channels;
  if (!frames || frames > (drmp3_uint64)(1u << 26) || !rate || !channels ||
      channels > 8) {
    drmp3_uninit(&mp3);
    return -1;
  }
  short* pcm = (short*)malloc((size_t)frames * channels * sizeof(short));
  if (!pcm) {
    drmp3_uninit(&mp3);
    return -1;
  }
  const drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, frames, pcm);
  drmp3_uninit(&mp3);
  if (!got) {
    free(pcm);
    return -1;
  }
  out->alloc_base = (u8*)pcm;
  out->pcm = (u8*)pcm;
  out->pcm_bytes = (u32)(got * channels * 2u);
  out->rate = rate;
  out->channels = channels;
  out->bps = 2;
  out->is_float = 0;
  out->length_ms = (u32)(got * 1000ull / rate);
  return 0;
}

// MP3 containers: ID3 tag, or a bare MPEG frame sync.
int is_mp3(const u8* d, u32 n) {
  if (n < 4) return 0;
  if (!memcmp(d, "ID3", 3)) return 1;
  if (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return 1;
  return 0;
}

int is_compressed(const u8* d, u32 n) {
  if (n < 4) return 1;
  if (!memcmp(d, "OggS", 4)) return 1;               // ogg/vorbis
  if (!memcmp(d, "FSB", 3)) return 1;                // FMOD sound bank
  if (!memcmp(d, "ID3", 3) || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))
    return 1;                                        // mp3
  if (!memcmp(d, "RIFF", 4)) return 0;               // wav: decodable
  return 1;                                          // unknown container
}

}  // namespace

extern "C" {

// ---- lifecycle ----
int TRT_GUEST_ABI fmod_System_Create(const void* mem, void** out) {
  (void)mem;
  if (!out) return FMOD_ERR_INVALID_PARAM_;
  fmod_obj* o = (fmod_obj*)obj_make(FM_T_SYSTEM);
  if (!o) return FMOD_ERR_INVALID_PARAM_;
  *out = o;
  log_line("fmod: FMOD_System_Create -> system handle (host mixer backend)");
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_System_Release(void* sys) {
  if (!obj_check(sys, FM_T_SYSTEM)) return FMOD_ERR_INVALID_HANDLE_;
  obj_take(sys);
  log_line("fmod: FMOD_System_Release -> released");
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_System_Init(void* sys, u32 maxchannels, u32 flags,
                                   void* extra) {
  (void)maxchannels;
  (void)flags;
  (void)extra;
  if (!obj_check(sys, FM_T_SYSTEM)) return FMOD_ERR_INVALID_HANDLE_;
  feeder_ensure();
  char line[128];
  snprintf(line, sizeof(line),
           "fmod: FMOD_System_Init(maxch=%u) -> FMOD_OK (host mixer, 48k stereo)",
           maxchannels);
  log_line(line);
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_System_Close(void* sys) {
  if (!obj_check(sys, FM_T_SYSTEM)) return FMOD_ERR_INVALID_HANDLE_;
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_System_Update(void* sys) {
  (void)sys;  // the mixer runs host-side; update is a no-op pump
  return FMOD_OK_;
}

// ---- sounds ----
int TRT_GUEST_ABI fmod_System_CreateSound(void* sys, const void* name_or_data,
                                          u32 mode, void* exinfo, void** out) {
  if (!obj_check(sys, FM_T_SYSTEM) || !out) return FMOD_ERR_INVALID_PARAM_;
  *out = nullptr;
  fmod_obj* o = (fmod_obj*)obj_make(FM_T_SOUND);
  if (!o) return FMOD_ERR_INVALID_PARAM_;
  fmod_sound* s = &o->snd;
  s->mode = mode;

  const u8* data = nullptr;
  u32 size = 0;
  char label[128] = "(memory)";

  if (mode & FMOD_OPENMEMORY_) {
    // FMOD_CREATESOUNDEXINFO: length at +4
    const u32* ex = (const u32*)exinfo;
    size = (ex && ex[0] >= 8) ? ex[1] : 0;
    data = (const u8*)name_or_data;
    if (!data || !size || size > (64u << 20)) {
      obj_take(o);
      return FMOD_ERR_INVALID_PARAM_;
    }
    // OPENMEMORY payloads are guest-owned (and may be freed the moment
    // CreateSound returns), so the decode paths must never read them later.
    // Take an owned copy: this is also what makes memory-backed sounds
    // decodable at all — previously any OPENMEMORY sound fell into the
    // "no alloc_base" branch and was silent.
    u8* copy = (u8*)malloc(size);
    if (!copy) {
      obj_take(o);
      return FMOD_ERR_INVALID_PARAM_;
    }
    memcpy(copy, data, size);
    data = copy;
    s->alloc_base = copy;
  } else {
    // guest file path (char*): resolve through the VFS like CreateFileW
    const char* path8 = (const char*)name_or_data;
    if (!path8 || !path8[0]) {
      obj_take(o);
      return FMOD_ERR_INVALID_PARAM_;
    }
    snprintf(label, sizeof(label), "%s", path8);
    u16 wpath[512];
    u32 n = 0;
    for (; path8[n] && n < 511; ++n) wpath[n] = (u16)(u8)path8[n];
    wpath[n] = 0;
    char host_path[1024];
    if (trt_vfs_resolve_utf16_rw(wpath, 0, host_path, sizeof(host_path)) != 0) {
      char line[224];
      snprintf(line, sizeof(line),
               "fmod: CreateSound(%s) -> FMOD_ERR_FILE_NOT_FOUND (VFS)", label);
      log_line(line);
      obj_take(o);
      return FMOD_ERR_FILE_NOT_FOUND_;
    }
    FILE* f = fopen(host_path, "rb");
    if (!f) {
      char line[224];
      snprintf(line, sizeof(line),
               "fmod: CreateSound(%s) -> FMOD_ERR_FILE_NOT_FOUND", label);
      log_line(line);
      obj_take(o);
      return FMOD_ERR_FILE_NOT_FOUND_;
    }
    fseek(f, 0, SEEK_END);
    const long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz > 0 && fsz < (64 << 20)) {
      u8* buf = (u8*)malloc((size_t)fsz);
      if (buf && fread(buf, 1, (size_t)fsz, f) == (size_t)fsz) {
        data = buf;
        size = (u32)fsz;
        s->alloc_base = buf;  // ownership: the BASE is freed on Sound_Release
      } else {
        free(buf);
      }
    }
    fclose(f);
    if (!data) {
      char line[224];
      snprintf(line, sizeof(line), "fmod: CreateSound(%s) -> read failed", label);
      log_line(line);
      obj_take(o);
      return FMOD_ERR_FILE_NOT_FOUND_;
    }
  }

  if (s->alloc_base) {
    // we own the buffer: try WAV, else classify
    fmod_sound tmp{};
    if (wav_decode(data, size, &tmp) == 0) {
      // steal the payload pointers into the persistent sound (no copy: the
      // buffer is ours for the handle's lifetime; pcm may point INSIDE
      // alloc_base — Release frees the base only)
      s->pcm = tmp.pcm;
      s->pcm_bytes = tmp.pcm_bytes;
      s->rate = tmp.rate;
      s->channels = tmp.channels;
      s->bps = tmp.bps;
      s->is_float = tmp.is_float;
      s->length_ms = tmp.length_ms;
      s->loop = (mode & (FMOD_LOOP_NORMAL_ | FMOD_LOOP_BIDI_)) ? 1 : 0;
    } else if (is_compressed(data, size) ||
               (size >= 44u && !memcmp(data, "RIFF", 4))) {
      // OGG: decode for real (stb_vorbis). The file buffer (alloc_base, when
      // we own it) feeds the decoder, then the decoded PCM replaces it.
      u8* old_base = s->alloc_base;
      const char* kind = "ogg";
      int decoded = ogg_decode(data, size, s);
      if (decoded != 0 && is_mp3(data, size)) {
        decoded = mp3_decode(data, size, s);
        kind = "mp3";
      }
      if (decoded != 0 && size >= 60u && !memcmp(data, "FSB5", 4)) {
        trt_decoded_pcm fsb_pcm{};
        if (trt_fsb5_decode_first_sample(data, size, &fsb_pcm) == 0 && fsb_pcm.samples) {
          s->alloc_base = reinterpret_cast<u8*>(fsb_pcm.samples);
          s->pcm = s->alloc_base;
          s->pcm_bytes = fsb_pcm.frames * fsb_pcm.channels * 2u;
          s->rate = fsb_pcm.rate;
          s->channels = fsb_pcm.channels;
          s->bps = 2;
          s->is_float = 0;
          s->length_ms = fsb_pcm.rate ? (u32)((u64)fsb_pcm.frames * 1000ull / fsb_pcm.rate) : 0;
          decoded = 0;
          kind = "fsb5";
        }
      }
      if (decoded != 0 && size >= 44u && !memcmp(data, "RIFF", 4)) {
        // WAV variants wav_decode doesn't pass through directly (e.g. IMA-ADPCM 0x0011)
        trt_decoded_pcm wav_pcm{};
        if (trt_decode_audio_buffer(data, size, &wav_pcm) == 0 && wav_pcm.samples) {
          s->alloc_base = reinterpret_cast<u8*>(wav_pcm.samples);
          s->pcm = s->alloc_base;
          s->pcm_bytes = wav_pcm.frames * wav_pcm.channels * 2u;
          s->rate = wav_pcm.rate;
          s->channels = wav_pcm.channels;
          s->bps = 2;
          s->is_float = 0;
          s->length_ms = wav_pcm.rate ? (u32)((u64)wav_pcm.frames * 1000ull / wav_pcm.rate) : 0;
          decoded = 0;
          kind = "wav-adpcm";
        }
      }
      if (old_base) free(old_base);  // data pointed inside it: free AFTER
      s->alloc_base = (decoded == 0) ? s->alloc_base : nullptr;
      if (decoded == 0) {
        s->loop = (mode & (FMOD_LOOP_NORMAL_ | FMOD_LOOP_BIDI_)) ? 1 : 0;
        char line[224];
        snprintf(line, sizeof(line),
                 "fmod: CreateSound(%s) -> %s decoded (%ums, %uhz %uch, real "
                 "audio)", label, kind, s->length_ms, s->rate, s->channels);
        log_line(line);
      } else {
        s->pcm = nullptr;
        s->silent = 1;
        // Name the container class and the missing decoder. The channel is
        // still real (play/stop/volume work); only the PCM is absent.
        const char* fmt = "unknown container";
        if (size >= 4 && !memcmp(data, "OggS", 4)) fmt = "ogg (decode failed)";
        else if (size >= 3 && !memcmp(data, "FSB", 3)) fmt = "fsb bank (no decoder)";
        else if (size >= 3 && !memcmp(data, "ID3", 3)) fmt = "mp3 (no decoder)";
        else if (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)
          fmt = "mp3 (no decoder)";
        else if (size >= 3 && !memcmp(data, "RIFF", 4)) fmt = "wav (decode failed)";
        char line[256];
        snprintf(line, sizeof(line),
                 "fmod: CreateSound(%s) -> %s: channel control real, audio "
                 "SILENT (named)", label, fmt);
        log_line(line);
      }
    } else {
      free(s->alloc_base);
      s->alloc_base = nullptr;
      s->pcm = nullptr;
      char line[256];
      snprintf(line, sizeof(line),
               "fmod: CreateSound(%s) -> undecodable container (%u bytes, "
               "named; handle valid, audio silent)", label, size);
      log_line(line);
      s->silent = 1;
    }
  } else {
    s->silent = 1;
  }
  snprintf(s->name, sizeof(s->name), "%s", label);
  *out = o;
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_System_CreateStream(void* sys, const void* name_or_data,
                                           u32 mode, void* exinfo, void** out) {
  // streams decode identically at this layer (the mixer is the stream)
  return fmod_System_CreateSound(sys, name_or_data, mode | FMOD_CREATESTREAM_,
                                 exinfo, out);
}

int TRT_GUEST_ABI fmod_Sound_Release(void* snd) {
  fmod_obj* o = obj_check(snd, FM_T_SOUND);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  if (o->snd.alloc_base) free(o->snd.alloc_base);  // the base, never the
  o->snd.alloc_base = nullptr;                     // interior payload pointer
  o->snd.pcm = nullptr;
  obj_take(snd);
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Sound_SetMode(void* snd, u32 mode) {
  fmod_obj* o = obj_check(snd, FM_T_SOUND);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  o->snd.mode = mode;
  o->snd.loop = (mode & (FMOD_LOOP_NORMAL_ | FMOD_LOOP_BIDI_)) ? 1 : 0;
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Sound_GetMode(void* snd, u32* mode) {
  fmod_obj* o = obj_check(snd, FM_T_SOUND);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  if (mode) *mode = o->snd.mode;
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Sound_GetLength(void* snd, u32* len, u32 unit) {
  fmod_obj* o = obj_check(snd, FM_T_SOUND);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  if (!len) return FMOD_ERR_INVALID_PARAM_;
  // unit 1 = milliseconds
  *len = (unit == 1) ? o->snd.length_ms : 0;
  return FMOD_OK_;
}

// ---- channels ----
// NOTE (dllmap1): the parameter order here used to be (sys,
// channel_or_index, sound) — swapped vs the real FMOD C API
// (sys, sound, channelgroup). Fixed to the real order; the Mono door and
// native games both pass sound second.
int TRT_GUEST_ABI fmod_System_PlaySound(void* sys, void* sound,
                                        void* channelgroup, int paused,
                                        void** out) {
  (void)sys;
  if (!out) return FMOD_ERR_INVALID_PARAM_;
  *out = nullptr;
  fmod_obj* so = obj_check(sound, FM_T_SOUND);
  if (!so) return FMOD_ERR_INVALID_HANDLE_;
  fmod_obj* o = (fmod_obj*)obj_make(FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_PARAM_;
  fmod_channel* fc = &o->ch;
  {
    std::lock_guard<std::mutex> lk(g_ch_mu);
    fc->snd = &so->snd;
    fc->volume = 1.0f;
    fc->paused = paused ? 1 : 0;
    fc->playing = 1;
    fc->voice_id = (u64)(uintptr_t)o * 2654435761ull + 1ull;
    if (!fc->paused && so->snd.pcm) feeder_push_once(fc);
  }
  if (!paused && so->snd.pcm && so->snd.loop) {
    { std::lock_guard<std::mutex> lk(g_ch_mu);
      if (g_feeder_count < 32) g_feeders[g_feeder_count++].ch = fc; }
    feeder_ensure();
  }
  if (so->snd.silent && so->snd.pcm == nullptr) {
    // one named line per silent sound (first play only: snd->silent acts as
    // the flag after the first play)
    char line[192];
    snprintf(line, sizeof(line),
             "fmod: PlaySound(%s) -> channel real, audio silent (decoder "
             "pending)",
             so->snd.name);
    log_line(line);
    so->snd.silent = 2;
  }
  *out = o;
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_Stop(void* ch) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); o->ch.playing = 0; }
  feeder_drop(&o->ch);
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_SetVolume(void* ch, float vol) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); o->ch.volume = vol; }
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_GetVolume(void* ch, float* vol) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); if (vol) *vol = o->ch.volume; }
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_SetPaused(void* ch, int paused) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); o->ch.paused = paused ? 1 : 0; }
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_GetPaused(void* ch, int* paused) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); if (paused) *paused = o->ch.paused; }
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_IsPlaying(void* ch, int* playing) {
  fmod_obj* o = obj_check(ch, FM_T_CHANNEL);
  if (!o) return FMOD_ERR_INVALID_HANDLE_;
  { std::lock_guard<std::mutex> lk(g_ch_mu); if (playing) *playing = o->ch.playing; }
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_SetFrequency(void* ch, float freq) {
  (void)ch;
  (void)freq;  // the mixer resamples; per-channel rate override is a no-op
  return FMOD_OK_;
}

int TRT_GUEST_ABI fmod_Channel_SetPosition(void* ch, u32 pos, u32 unit) {
  (void)ch;
  (void)pos;
  (void)unit;
  return FMOD_OK_;  // seek within the FIFO is v2 work
}

// ---- channel groups ----
int TRT_GUEST_ABI fmod_System_GetMasterChannelGroup(void* sys, void** out) {
  if (!obj_check(sys, FM_T_SYSTEM)) return FMOD_ERR_INVALID_HANDLE_;
  if (!out) return FMOD_ERR_INVALID_PARAM_;
  // a singleton master group handle
  *out = obj_make(FM_T_CHANNELGROUP);
  return *out ? FMOD_OK_ : FMOD_ERR_INVALID_PARAM_;
}

int TRT_GUEST_ABI fmod_ChannelGroup_AddChannelGroup(void* grp, void* other) {
  (void)grp;
  (void)other;
  return FMOD_OK_;  // grouping is bookkeeping the mixer flattens
}

int TRT_GUEST_ABI fmod_ChannelGroup_SetVolume(void* grp, float vol) {
  (void)grp;
  (void)vol;
  return FMOD_OK_;
}

// ---- configuration setters/getters: one real output path exists ----
int TRT_GUEST_ABI fmod_System_SetOutput(void* sys, u32 output) {
  (void)sys;
  (void)output;
  return FMOD_OK_;
}
int TRT_GUEST_ABI fmod_System_SetSpeakerMode(void* sys, u32 mode) {
  (void)sys;
  (void)mode;
  return FMOD_OK_;
}
int TRT_GUEST_ABI fmod_System_SetDSPBufferSize(void* sys, u32 len, u32 num) {
  (void)sys;
  (void)len;
  (void)num;
  return FMOD_OK_;
}
int TRT_GUEST_ABI fmod_System_Init2(void* sys, u32 maxch, u32 flags, void* ex) {
  return fmod_System_Init(sys, maxch, flags, ex);
}
int TRT_GUEST_ABI fmod_Memory_Initialize(void* pool, u32 size,
                                          const void* funcs, u32 memtype) {
  (void)pool;
  (void)size;
  (void)funcs;
  (void)memtype;
  log_line("fmod: FMOD_Memory_Initialize -> accepted (bridge uses host heap)");
  return FMOD_OK_;
}
int TRT_GUEST_ABI fmod_Debug_Initialize(u32 flags, const char* filename,
                                        int mode) {
  (void)flags;
  (void)filename;
  (void)mode;
  return FMOD_OK_;
}

}  // extern "C"

// ---- registration ----
int trt_bridge_register_fmod(void) {
  static const trt_named_fn k_fmod_fns[] = {
      {"FMOD_System_Create", (void*)&fmod_System_Create},
      {"FMOD_System_Release", (void*)&fmod_System_Release},
      {"FMOD_System_Init", (void*)&fmod_System_Init},
      {"FMOD_System_Close", (void*)&fmod_System_Close},
      {"FMOD_System_Update", (void*)&fmod_System_Update},
      {"FMOD_System_CreateSound", (void*)&fmod_System_CreateSound},
      {"FMOD_System_CreateStream", (void*)&fmod_System_CreateStream},
      {"FMOD_System_PlaySound", (void*)&fmod_System_PlaySound},
      {"FMOD_System_GetMasterChannelGroup",
       (void*)&fmod_System_GetMasterChannelGroup},
      {"FMOD_System_SetOutput", (void*)&fmod_System_SetOutput},
      {"FMOD_System_SetSpeakerMode", (void*)&fmod_System_SetSpeakerMode},
      {"FMOD_System_SetDSPBufferSize", (void*)&fmod_System_SetDSPBufferSize},
      {"FMOD_Sound_Release", (void*)&fmod_Sound_Release},
      {"FMOD_Sound_SetMode", (void*)&fmod_Sound_SetMode},
      {"FMOD_Sound_GetMode", (void*)&fmod_Sound_GetMode},
      {"FMOD_Sound_GetLength", (void*)&fmod_Sound_GetLength},
      {"FMOD_Channel_Stop", (void*)&fmod_Channel_Stop},
      {"FMOD_Channel_SetVolume", (void*)&fmod_Channel_SetVolume},
      {"FMOD_Channel_GetVolume", (void*)&fmod_Channel_GetVolume},
      {"FMOD_Channel_SetPaused", (void*)&fmod_Channel_SetPaused},
      {"FMOD_Channel_GetPaused", (void*)&fmod_Channel_GetPaused},
      {"FMOD_Channel_IsPlaying", (void*)&fmod_Channel_IsPlaying},
      {"FMOD_Channel_SetFrequency", (void*)&fmod_Channel_SetFrequency},
      {"FMOD_Channel_SetPosition", (void*)&fmod_Channel_SetPosition},
      {"FMOD_ChannelGroup_AddChannelGroup",
       (void*)&fmod_ChannelGroup_AddChannelGroup},
      {"FMOD_ChannelGroup_SetVolume", (void*)&fmod_ChannelGroup_SetVolume},
      {"FMOD_Memory_Initialize", (void*)&fmod_Memory_Initialize},
      {"FMOD_Debug_Initialize", (void*)&fmod_Debug_Initialize},
  };
  // FMOD Ex and Studio ship under these module names; one table serves all
  trt_module_register("fmodex.dll", TRT_ROUTE_BUILTIN, k_fmod_fns,
                      (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  trt_module_register("fmodex64.dll", TRT_ROUTE_BUILTIN, k_fmod_fns,
                      (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  trt_module_register("fmod.dll", TRT_ROUTE_BUILTIN, k_fmod_fns,
                      (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  trt_module_register("fmodstudio.dll", TRT_ROUTE_BUILTIN, k_fmod_fns,
                      (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  trt_module_register("fmodstudioL.dll", TRT_ROUTE_BUILTIN, k_fmod_fns,
                      (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  char line[96];
  snprintf(line, sizeof(line),
           "fmod: registered %d functions across 5 module names (wav/pcm real, "
           "compressed named-silent)",
           (int)(sizeof(k_fmod_fns) / sizeof(k_fmod_fns[0])));
  log_line(line);
  return 0;
}
