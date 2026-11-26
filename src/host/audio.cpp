// tiramisu/src/host/audio.cpp
// Multi-voice real-time software audio mixer feeding XAudio2 2.8 on Xbox.
//
// Mixes concurrent XAudio2, DirectSound, WASAPI, and OpenAL streams of
// arbitrary sample rate (8kHz..192kHz), channel count (1..8), and bit format
// (u8, i16, i24, i32, f32) into a single persistent 48 kHz stereo 16-bit
// mastering XAudio2 source voice pumped at 10ms intervals.
#include "audio.h"
#include "host_log.h"

#include "../trt/trt.h"
#include "../trt/bridge_audio.h"
#include "../trt/bridge_dsound.h"

#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "xaudio2_8.lib")
#pragma comment(lib, "ole32.lib")

namespace longan {

namespace {

constexpr u32 k_mix_rate = 48000u;
constexpr u32 k_mix_channels = 2u;
constexpr u32 k_chunk_frames = 480u;  // 10 ms at 48 kHz
constexpr int k_slots = 8;
constexpr size_t k_max_voice_fifo_samples = k_mix_rate * k_mix_channels * 2u;

struct pcm_slot {
  std::vector<uint8_t> pcm;
  XAUDIO2_BUFFER buf{};
  bool busy = false;
  u64 submit_seq = 0;
};

struct voice_stream {
  std::deque<float> fifo;  // stereo float interleaved @ 48 kHz
  float prev_l = 0.0f;
  float prev_r = 0.0f;
  double phase = 0.0;
  u32 idle_ticks = 0;
};

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_src = nullptr;

std::mutex g_mix_mu;
std::unordered_map<u64, voice_stream> g_voices;
pcm_slot g_slots[k_slots];
u64 g_seq = 0;
u32 g_submitted = 0;
u32 g_dropped = 0;

std::atomic<bool> g_mix_stop{false};
std::thread g_mix_thread;

float read_sample(const uint8_t* p, u32 bps, u32 is_float) {
  if (is_float && bps == 4) {
    float f = 0.0f;
    memcpy(&f, p, 4);
    return f;
  }
  if (bps == 1) {
    return ((float)p[0] - 128.0f) / 128.0f;
  }
  if (bps == 2) {
    int16_t s = 0;
    memcpy(&s, p, 2);
    return (float)s / 32768.0f;
  }
  if (bps == 3) {
    int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16));
    if (s & 0x800000) s |= 0xFF000000;
    return (float)s / 8388608.0f;
  }
  if (bps == 4) {
    int32_t s = 0;
    memcpy(&s, p, 4);
    return (float)s / 2147483648.0f;
  }
  return 0.0f;
}

void push_voice_pcm(u64 voice_id, const void* pcm, u32 bytes, u32 rate,
                    u32 channels, u32 bps, u32 is_float, float volume) {
  if (!pcm || bytes == 0 || rate == 0 || channels == 0 || bps == 0) return;
  const u32 stride = channels * bps;
  const u32 src_frames = bytes / stride;
  if (src_frames == 0) return;

  const uint8_t* raw = (const uint8_t*)pcm;
  const double step = (double)rate / (double)k_mix_rate;

  std::lock_guard<std::mutex> lk(g_mix_mu);
  if (!g_xa) return;

  // Cap active stream count safely to bound CPU/RAM on pathological guests
  if (g_voices.find(voice_id) == g_voices.end() && g_voices.size() >= 64) {
    auto victim = g_voices.begin();
    for (auto it = g_voices.begin(); it != g_voices.end(); ++it) {
      if (it->second.fifo.size() < victim->second.fifo.size()) victim = it;
    }
    g_voices.erase(victim);
    ++g_dropped;
  }

  voice_stream& vs = g_voices[voice_id];
  vs.idle_ticks = 0;

  double pos = vs.phase;
  float last_l = vs.prev_l;
  float last_r = vs.prev_r;

  auto decode_frame = [&](u32 fi, float* out_l, float* out_r) {
    const uint8_t* f = raw + (size_t)fi * stride;
    const float s0 = read_sample(f, bps, is_float);
    float l = s0, r = s0;
    if (channels >= 2) {
      r = read_sample(f + bps, bps, is_float);
    }
    if (channels >= 5) {
      // Fold center (ch2) and surround L/R (ch4, ch5) into stereo mix
      const float c = read_sample(f + 2 * bps, bps, is_float) * 0.707f;
      l += c;
      r += c;
      if (channels >= 6) {
        l += read_sample(f + 4 * bps, bps, is_float) * 0.5f;
        r += read_sample(f + 5 * bps, bps, is_float) * 0.5f;
      }
    }
    *out_l = l * volume;
    *out_r = r * volume;
  };

  while (pos < (double)src_frames) {
    const int i0 = (int)floor(pos);
    const float frac = (float)(pos - (double)i0);
    float l0 = last_l, r0 = last_r;
    if (i0 >= 0) decode_frame((u32)i0, &l0, &r0);
    const u32 i1 = (u32)(i0 + 1);
    float l1 = l0, r1 = r0;
    if (i1 < src_frames) decode_frame(i1, &l1, &r1);

    if (vs.fifo.size() + 2 <= k_max_voice_fifo_samples) {
      vs.fifo.push_back(l0 + (l1 - l0) * frac);
      vs.fifo.push_back(r0 + (r1 - r0) * frac);
    } else {
      ++g_dropped;
      break;
    }
    pos += step;
  }

  decode_frame(src_frames - 1, &vs.prev_l, &vs.prev_r);
  vs.phase = pos - (double)src_frames;
  if (vs.phase < -1.0 || vs.phase >= 1.0) vs.phase = 0.0;
}

int sink_submit_voice(u64 voice_id, const void* pcm, u32 bytes, u32 rate,
                      u32 channels, u32 bps, u32 is_float, float vol,
                      void* user) {
  (void)user;
  static std::atomic<unsigned> submits{0};
  const unsigned submit_no = submits.fetch_add(1, std::memory_order_relaxed);
  if (submit_no < 16) {
    const uint8_t* data = static_cast<const uint8_t*>(pcm);
    const u32 count = bytes / bps;
    double sum_sq = 0.0;
    float peak = 0.0f;
    for (u32 i = 0; i < count; ++i) {
      const float sample = read_sample(data + (size_t)i * bps, bps, is_float);
      const float magnitude = fabsf(sample);
      if (magnitude > peak) peak = magnitude;
      sum_sq += (double)sample * (double)sample;
    }
    const float rms = count ? (float)sqrt(sum_sq / count) : 0.0f;
    char line[192];
    snprintf(line, sizeof(line),
             "audio: input voice=%llu bytes=%u rate=%u ch=%u bps=%u vol=%.3f rms=%.5f peak=%.5f",
             (unsigned long long)voice_id, bytes, rate, channels, bps,
             vol, rms, peak);
    log_line(line);
  }
  push_voice_pcm(voice_id, pcm, bytes, rate, channels, bps, is_float, vol);
  return 0;
}

int sink_submit(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) {
  (void)user;
  if (!pcm || bytes == 0) return 0;
  // Derive format-grouped voice stream for legacy callers without voice IDs
  const u64 fallback_id = ((u64)rate << 16) ^ ((u64)channels << 8) ^
                          (u64)bytes_per_sample;
  push_voice_pcm(fallback_id, pcm, bytes, rate, channels, bytes_per_sample, 0,
                 1.0f);
  return 0;
}

void mixer_tick() {
  std::vector<int16_t> out_s16;
  bool any_audio = false;
  {
    std::lock_guard<std::mutex> lk(g_mix_mu);
    if (!g_xa || !g_src) return;

    float mix[k_chunk_frames * k_mix_channels] = {};
    for (auto it = g_voices.begin(); it != g_voices.end();) {
      voice_stream& vs = it->second;
      if (vs.fifo.empty()) {
        ++vs.idle_ticks;
        if (vs.idle_ticks > 300) {  // 3 seconds idle -> reap stream
          it = g_voices.erase(it);
          continue;
        }
        ++it;
        continue;
      }
      vs.idle_ticks = 0;
      any_audio = true;
      const size_t avail = vs.fifo.size();
      const size_t take =
          avail < (size_t)(k_chunk_frames * k_mix_channels)
              ? avail
              : (size_t)(k_chunk_frames * k_mix_channels);
      for (size_t i = 0; i < take; ++i) {
        mix[i] += vs.fifo.front();
        vs.fifo.pop_front();
      }
      ++it;
    }

    XAUDIO2_VOICE_STATE st{};
    g_src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (!any_audio && st.BuffersQueued > 0) return;

    // Wait on busy slots safely vs XAudio2 queued count
    int pick = -1;
    u64 oldest = ~0ull;
    int steal = -1;
    for (int i = 0; i < k_slots; ++i) {
      if (g_slots[i].busy && st.BuffersQueued < (UINT32)k_slots) {
        g_slots[i].busy = false;
      }
      if (!g_slots[i].busy && pick < 0) pick = i;
      if (g_slots[i].busy && g_slots[i].submit_seq < oldest) {
        oldest = g_slots[i].submit_seq;
        steal = i;
      }
    }
    if (pick < 0) {
      if (steal < 0) return;
      g_slots[steal].busy = false;
      pick = steal;
      ++g_dropped;
    }

    if (!any_audio) return;

    const u32 out_bytes = k_chunk_frames * k_mix_channels * sizeof(int16_t);
    pcm_slot& s = g_slots[pick];
    s.pcm.resize(out_bytes);
    int16_t* dst = (int16_t*)s.pcm.data();
    for (u32 i = 0; i < k_chunk_frames * k_mix_channels; ++i) {
      // Soft-clipping tanh keeps concurrent explosions/music clean without wrap
      const float clamped = std::tanh(mix[i]);
      dst[i] = (int16_t)(clamped * 32767.0f);
    }
    memset(&s.buf, 0, sizeof(s.buf));
    s.buf.AudioBytes = out_bytes;
    s.buf.pAudioData = s.pcm.data();
    s.buf.Flags = 0;
    const HRESULT hr = g_src->SubmitSourceBuffer(&s.buf);
    static std::atomic<unsigned> output_logs{0};
    const unsigned output_no = output_logs.fetch_add(1, std::memory_order_relaxed);
    if (output_no < 8) {
      double sum_sq = 0.0;
      int peak = 0;
      for (u32 i = 0; i < k_chunk_frames * k_mix_channels; ++i) {
        const int v = dst[i];
        const int magnitude = v < 0 ? -v : v;
        if (magnitude > peak) peak = magnitude;
        sum_sq += (double)v * (double)v;
      }
      const double rms = sqrt(sum_sq / (k_chunk_frames * k_mix_channels));
      char line[128];
      snprintf(line, sizeof(line),
               "audio: output submit=%u hr=0x%08lx rms=%.1f peak=%d queued=%u",
               output_no + 1, (unsigned long)hr, rms, peak, st.BuffersQueued);
      log_line(line);
    }
    if (FAILED(hr)) {
      ++g_dropped;
      return;
    }
    s.busy = true;
    s.submit_seq = ++g_seq;
    ++g_submitted;
  }
}

void mixer_thread_main() {
  auto next = std::chrono::steady_clock::now();
  while (!g_mix_stop.load(std::memory_order_acquire)) {
    mixer_tick();
    next += std::chrono::milliseconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;
    std::this_thread::sleep_until(next);
  }
}

} // namespace

void audio_init() {
  const HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line),
             "audio: XAudio2Create hr=0x%08lx (guest will run silent)",
             (unsigned long)hr);
    longan::log_line(line);
    g_xa = nullptr;
    return;
  }
  const HRESULT hr2 = g_xa->CreateMasteringVoice(&g_master);
  if (FAILED(hr2)) {
    char line[128];
    snprintf(line, sizeof(line),
             "audio: CreateMasteringVoice hr=0x%08lx (silent)",
             (unsigned long)hr2);
    longan::log_line(line);
    g_xa = nullptr;
    return;
  }

  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = (WORD)k_mix_channels;
  wf.nSamplesPerSec = k_mix_rate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = (WORD)(k_mix_channels * 2);
  wf.nAvgBytesPerSec = k_mix_rate * wf.nBlockAlign;
  const HRESULT hr3 =
      g_xa->CreateSourceVoice(&g_src, &wf, 0, 2.0f, nullptr, nullptr, nullptr);
  if (SUCCEEDED(hr3) && g_src) {
    g_src->Start(0);
    longan::log_line(
        "audio: XAudio2 2.8 multi-voice mixer OK (48000 Hz stereo)");
  } else {
    char line[128];
    snprintf(line, sizeof(line), "audio: CreateSourceVoice hr=0x%08lx",
             (unsigned long)hr3);
    longan::log_line(line);
    g_src = nullptr;
  }

  g_mix_stop.store(false, std::memory_order_release);
  g_mix_thread = std::thread(&mixer_thread_main);

  trt_audio_set_hooks(&sink_submit, nullptr);
  trt_audio_set_voice_hook(&sink_submit_voice, nullptr);
  trt_dsound_bind_sink(&sink_submit, nullptr);
}

void audio_status(char* out, unsigned cap) {
  std::lock_guard<std::mutex> lk(g_mix_mu);
  if (!g_xa) { snprintf(out, cap, "audio: no device"); return; }
  snprintf(out, cap, "audio: %zu voices, %u bufs, %u dropped, 48000 Hz stereo",
           g_voices.size(), g_submitted, g_dropped);
}

void audio_play_pcm_chunk(uint64_t voice_id, const void* pcm, uint32_t bytes,
                          uint32_t rate, uint32_t channels, uint32_t bps,
                          float volume) {
  push_voice_pcm(voice_id, pcm, bytes, rate, channels, bps, 0u, volume);
}

void audio_stop_voice(uint64_t voice_id) {
  std::lock_guard<std::mutex> lk(g_mix_mu);
  g_voices.erase(voice_id);
}

void audio_shutdown() {
  g_mix_stop.store(true, std::memory_order_release);
  if (g_mix_thread.joinable()) g_mix_thread.join();
  std::lock_guard<std::mutex> lk(g_mix_mu);
  g_voices.clear();
  if (g_src) { g_src->Stop(0); g_src->DestroyVoice(); g_src = nullptr; }
  if (g_master) g_master = nullptr;
  if (g_xa) g_xa = nullptr;
}

} // namespace longan
