// tiramisu/src/trt/audio_codecs.cpp
// Real audio codec implementations shared across FMOD Core, FMOD Studio, and Wwise:
//   - FMOD FADPCM (publicly documented XA/PSX-derived 4-bit ADPCM, 140B -> 256 samples/ch)
//   - IMA-ADPCM (WAV 0x0011, Wwise ADPCM, FSB5 IMA-ADPCM)
//   - FSB5 container & non-Vorbis sample decode (PCM8/16/24/32/Float, IMA-ADPCM, MP3, FADPCM)
//   - RIFF/WAVE + Wwise .wem + Ogg Vorbis + MP3 buffer decode
#include "audio_codecs.h"
#include "bridge_fmod_studio.h"

#include <cstdlib>
#include <cstring>
#include <vector>

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
#include "vendor_dr_mp3.h"
}

namespace {

inline u16 rd16(const u8* p) {
  u16 v = 0;
  memcpy(&v, p, 2);
  return v;
}

inline int16_t rds16(const u8* p) {
  int16_t v = 0;
  memcpy(&v, p, 2);
  return v;
}

inline u32 rd32(const u8* p) {
  u32 v = 0;
  memcpy(&v, p, 4);
  return v;
}

inline u64 rd64(const u8* p) {
  u64 v = 0;
  memcpy(&v, p, 8);
  return v;
}

inline int16_t clamp_s16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}

// FADPCM predictor coefficients (XA/PSX-derived, fixed-point << 6, 7-entry modulo).
static constexpr int8_t k_fadpcm_coefs[8][2] = {
    {  0,  0},
    { 60,  0},
    {122, 60},
    {115, 52},
    { 98, 55},
    {  0,  0},
    {  0,  0},
    {  0,  0},
};

// Standard IMA-ADPCM step and index tables (ITU/IMA spec).
static constexpr int8_t k_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static constexpr int16_t k_ima_step_table[89] = {
       7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
      19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
      50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
     130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
     337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
     876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
   15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

inline int16_t ima_step(u8 nibble, int32_t& predictor, int32_t& step_idx) {
  int32_t step = k_ima_step_table[step_idx];
  int32_t diff = step >> 3;
  if (nibble & 1) diff += step >> 2;
  if (nibble & 2) diff += step >> 1;
  if (nibble & 4) diff += step;
  if (nibble & 8) predictor -= diff;
  else predictor += diff;
  if (predictor > 32767) predictor = 32767;
  if (predictor < -32768) predictor = -32768;
  step_idx += k_ima_index_table[nibble & 0x0F];
  if (step_idx < 0) step_idx = 0;
  if (step_idx > 88) step_idx = 88;
  return static_cast<int16_t>(predictor);
}

int decode_mp3_to_s16(const u8* data, size_t len, trt_decoded_pcm* out) {
  drmp3 mp3{};
  if (!drmp3_init_memory(&mp3, data, len, nullptr)) return -1;
  const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
  const u32 rate = mp3.sampleRate;
  const u32 channels = mp3.channels;
  if (!frames || frames > 20000000ull || !rate || !channels || channels > 8) {
    drmp3_uninit(&mp3);
    return -1;
  }
  int16_t* pcm = static_cast<int16_t*>(
      malloc(static_cast<size_t>(frames) * channels * sizeof(int16_t)));
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
  out->samples = pcm;
  out->frames = static_cast<u32>(got);
  out->rate = rate;
  out->channels = channels;
  return 0;
}

int decode_ogg_to_s16(const u8* data, size_t len, trt_decoded_pcm* out) {
  int err = 0;
  stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(len), &err, nullptr);
  if (!v) return -1;
  const stb_vorbis_info info = stb_vorbis_get_info(v);
  if (!info.channels || info.channels > 8 || !info.sample_rate) {
    stb_vorbis_close(v);
    return -1;
  }
  std::vector<int16_t> pcm;
  pcm.reserve(65536u * static_cast<size_t>(info.channels));
  int16_t frame_buf[4096 * 8];
  int n = 0;
  while ((n = stb_vorbis_get_frame_short_interleaved(
              v, info.channels, frame_buf, 4096 * info.channels)) > 0) {
    const size_t total_frames = pcm.size() / static_cast<size_t>(info.channels);
    if (total_frames + static_cast<size_t>(n) > 20000000u) break;
    pcm.insert(pcm.end(), frame_buf, frame_buf + static_cast<size_t>(n) * info.channels);
  }
  stb_vorbis_close(v);
  if (pcm.empty()) return -1;
  int16_t* buf = static_cast<int16_t*>(malloc(pcm.size() * sizeof(int16_t)));
  if (!buf) return -1;
  memcpy(buf, pcm.data(), pcm.size() * sizeof(int16_t));
  out->samples = buf;
  out->frames = static_cast<u32>(pcm.size() / static_cast<size_t>(info.channels));
  out->rate = info.sample_rate;
  out->channels = static_cast<u32>(info.channels);
  return 0;
}

}  // namespace

extern "C" {

int trt_decode_fadpcm(const u8* data, size_t data_len, u32 channels,
                      u32 num_frames, int16_t** out_pcm, u32* out_frames) {
  if (!data || !out_pcm || !out_frames) return -1;
  *out_pcm = nullptr;
  *out_frames = 0;
  if (channels == 0 || channels > 8) return -1;

  constexpr size_t k_frame_bytes = 0x8Cu;     // 140 bytes per channel block
  constexpr u32 k_samples_per_block = 256u;   // (140 - 12) * 2 nibbles
  const size_t block_stride = k_frame_bytes * static_cast<size_t>(channels);
  if (data_len < block_stride) return -1;

  const size_t total_blocks = data_len / block_stride;
  u64 max_decoded = static_cast<u64>(total_blocks) * k_samples_per_block;
  if (num_frames > 0 && num_frames < max_decoded) max_decoded = num_frames;
  if (max_decoded == 0 || max_decoded > 20000000ull) return -1;

  const u32 target_frames = static_cast<u32>(max_decoded);
  int16_t* pcm = static_cast<int16_t*>(
      malloc(static_cast<size_t>(target_frames) * channels * sizeof(int16_t)));
  if (!pcm) return -1;

  u32 produced_frames = 0;
  for (size_t blk = 0; blk < total_blocks && produced_frames < target_frames; ++blk) {
    const u32 frames_this_blk =
        (target_frames - produced_frames < k_samples_per_block)
            ? (target_frames - produced_frames)
            : k_samples_per_block;
    for (u32 ch = 0; ch < channels; ++ch) {
      const u8* fr = data + blk * block_stride + ch * k_frame_bytes;
      const u32 coefs = rd32(fr + 0x00);
      const u32 shifts = rd32(fr + 0x04);
      int32_t hist1 = rds16(fr + 0x08);
      int32_t hist2 = rds16(fr + 0x0A);

      u32 local_sample = 0;
      for (int i = 0; i < 8 && local_sample < frames_this_blk; ++i) {
        const int idx = static_cast<int>(((coefs >> (i * 4)) & 0x0Fu) % 7u);
        const int raw_shift = static_cast<int>((shifts >> (i * 4)) & 0x0Fu);
        const int shift = 22 - raw_shift;
        const int32_t coef1 = k_fadpcm_coefs[idx][0];
        const int32_t coef2 = k_fadpcm_coefs[idx][1];

        for (int j = 0; j < 4 && local_sample < frames_this_blk; ++j) {
          const u32 nibbles = rd32(fr + 0x0C + i * 16 + j * 4);
          for (int k = 0; k < 8 && local_sample < frames_this_blk; ++k) {
            int32_t sample = static_cast<int32_t>((nibbles >> (k * 4)) & 0x0Fu);
            // Sign-extend 4-bit nibble in top 4 bits of 32-bit int, then arithmetic shift.
            const int32_t signed_nib = static_cast<int32_t>(static_cast<u32>(sample) << 28);
            sample = (shift >= 0 && shift < 32) ? (signed_nib >> shift) : 0;
            sample = (sample - hist2 * coef2 + hist1 * coef1) >> 6;
            const int16_t clamped = clamp_s16(sample);

            pcm[(static_cast<size_t>(produced_frames) + local_sample) * channels + ch] = clamped;
            hist2 = hist1;
            hist1 = clamped;
            ++local_sample;
          }
        }
      }
    }
    produced_frames += frames_this_blk;
  }

  *out_pcm = pcm;
  *out_frames = produced_frames;
  return 0;
}

int trt_decode_ima_adpcm(const u8* data, size_t data_len, u32 channels,
                         u32 block_align, u32 max_frames,
                         int16_t** out_pcm, u32* out_frames) {
  if (!data || !out_pcm || !out_frames) return -1;
  *out_pcm = nullptr;
  *out_frames = 0;
  if (channels == 0 || channels > 8) return -1;
  const u32 hdr_bytes = channels * 4u;
  if (block_align < hdr_bytes || (block_align - hdr_bytes) % (channels * 4u) != 0)
    return -1;
  if (data_len < block_align) return -1;

  const size_t total_blocks = data_len / block_align;
  const u32 word_groups = (block_align - hdr_bytes) / (channels * 4u);
  const u32 samples_per_block = 1u + word_groups * 8u;
  u64 total_frames = static_cast<u64>(total_blocks) * samples_per_block;
  if (max_frames > 0 && max_frames < total_frames) total_frames = max_frames;
  if (total_frames == 0 || total_frames > 20000000ull) return -1;

  const u32 target = static_cast<u32>(total_frames);
  int16_t* pcm = static_cast<int16_t*>(
      malloc(static_cast<size_t>(target) * channels * sizeof(int16_t)));
  if (!pcm) return -1;

  u32 out_pos = 0;
  for (size_t blk = 0; blk < total_blocks && out_pos < target; ++blk) {
    const u8* bp = data + blk * block_align;
    int32_t pred[8] = {};
    int32_t sidx[8] = {};
    for (u32 ch = 0; ch < channels; ++ch) {
      pred[ch] = rds16(bp + ch * 4u);
      sidx[ch] = bp[ch * 4u + 2u];
      if (sidx[ch] > 88) sidx[ch] = 88;
      pcm[static_cast<size_t>(out_pos) * channels + ch] = static_cast<int16_t>(pred[ch]);
    }
    ++out_pos;
    const u8* nib = bp + hdr_bytes;
    for (u32 g = 0; g < word_groups && out_pos < target; ++g) {
      for (u32 ch = 0; ch < channels; ++ch) {
        const u8* cw = nib + (g * channels + ch) * 4u;
        for (u32 b = 0; b < 4u; ++b) {
          const u8 byte = cw[b];
          const u32 f0 = out_pos + b * 2u;
          const u32 f1 = f0 + 1u;
          const int16_t s0 = ima_step(byte & 0x0Fu, pred[ch], sidx[ch]);
          const int16_t s1 = ima_step((byte >> 4) & 0x0Fu, pred[ch], sidx[ch]);
          if (f0 < target) pcm[static_cast<size_t>(f0) * channels + ch] = s0;
          if (f1 < target) pcm[static_cast<size_t>(f1) * channels + ch] = s1;
        }
      }
      out_pos = (out_pos + 8u <= target) ? (out_pos + 8u) : target;
    }
  }

  *out_pcm = pcm;
  *out_frames = out_pos;
  return 0;
}

int trt_fsb5_decode_payload(u32 fsb_mode, const u8* payload, size_t payload_len,
                            u32 rate, u32 channels, u32 expected_frames,
                            trt_decoded_pcm* out) {
  if (!payload || !payload_len || !out || channels == 0 || channels > 8) return -1;
  memset(out, 0, sizeof(*out));
  if (!rate) rate = 44100u;

  if (fsb_mode == TRT_FSB5_FADPCM) {
    int16_t* pcm = nullptr;
    u32 got = 0;
    if (trt_decode_fadpcm(payload, payload_len, channels, expected_frames, &pcm, &got) != 0)
      return -1;
    out->samples = pcm;
    out->frames = got;
    out->rate = rate;
    out->channels = channels;
    return 0;
  }

  if (fsb_mode == TRT_FSB5_IMAADPCM) {
    int16_t* pcm = nullptr;
    u32 got = 0;
    // FSB5 IMA-ADPCM uses 36 bytes per channel per block (4B header + 32B nibbles = 65 samples)
    if (trt_decode_ima_adpcm(payload, payload_len, channels, 36u * channels,
                             expected_frames, &pcm, &got) != 0)
      return -1;
    out->samples = pcm;
    out->frames = got;
    out->rate = rate;
    out->channels = channels;
    return 0;
  }

  if (fsb_mode == TRT_FSB5_MPEG) {
    return decode_mp3_to_s16(payload, payload_len, out);
  }

  // Raw PCM variants: PCM8, PCM16, PCM24, PCM32, PCMFLOAT
  u32 bps = 0;
  if (fsb_mode == TRT_FSB5_PCM8) bps = 1;
  else if (fsb_mode == TRT_FSB5_PCM16) bps = 2;
  else if (fsb_mode == TRT_FSB5_PCM24) bps = 3;
  else if (fsb_mode == TRT_FSB5_PCM32 || fsb_mode == TRT_FSB5_PCMFLOAT) bps = 4;
  else return -1;

  const size_t stride = static_cast<size_t>(channels) * bps;
  u64 avail_frames = payload_len / stride;
  if (expected_frames > 0 && expected_frames < avail_frames)
    avail_frames = expected_frames;
  if (avail_frames == 0 || avail_frames > 20000000ull) return -1;

  const u32 frames = static_cast<u32>(avail_frames);
  const size_t total_samples = static_cast<size_t>(frames) * channels;
  int16_t* pcm = static_cast<int16_t*>(malloc(total_samples * sizeof(int16_t)));
  if (!pcm) return -1;

  if (fsb_mode == TRT_FSB5_PCM8) {
    for (size_t i = 0; i < total_samples; ++i)
      pcm[i] = static_cast<int16_t>((static_cast<int32_t>(payload[i]) - 128) << 8);
  } else if (fsb_mode == TRT_FSB5_PCM16) {
    memcpy(pcm, payload, total_samples * 2u);
  } else if (fsb_mode == TRT_FSB5_PCM24) {
    for (size_t i = 0; i < total_samples; ++i) {
      const u8* p = payload + i * 3u;
      pcm[i] = static_cast<int16_t>(static_cast<u16>(p[1]) | (static_cast<u16>(p[2]) << 8));
    }
  } else if (fsb_mode == TRT_FSB5_PCM32) {
    for (size_t i = 0; i < total_samples; ++i) {
      int32_t v = static_cast<int32_t>(rd32(payload + i * 4u));
      pcm[i] = static_cast<int16_t>(v >> 16);
    }
  } else if (fsb_mode == TRT_FSB5_PCMFLOAT) {
    for (size_t i = 0; i < total_samples; ++i) {
      float f = 0.0f;
      memcpy(&f, payload + i * 4u, 4);
      if (f > 1.0f) f = 1.0f;
      else if (f < -1.0f) f = -1.0f;
      pcm[i] = clamp_s16(static_cast<int32_t>(f * 32767.0f));
    }
  }

  out->samples = pcm;
  out->frames = frames;
  out->rate = rate;
  out->channels = channels;
  return 0;
}

int trt_fsb5_decode_first_sample(const u8* fsb_data, size_t fsb_len,
                                 trt_decoded_pcm* out) {
  if (!fsb_data || fsb_len < 60u || !out) return -1;
  if (memcmp(fsb_data, "FSB5", 4) != 0) return -1;
  const u32 num_samples = rd32(fsb_data + 8);
  const u32 shdr_size = rd32(fsb_data + 12);
  const u32 name_size = rd32(fsb_data + 16);
  const u32 data_size = rd32(fsb_data + 20);
  const u32 mode = rd32(fsb_data + 24);
  const char* why = nullptr;
  if (!trt_fsb5_header_sane(num_samples, shdr_size, name_size, data_size,
                            static_cast<u64>(fsb_len), &why))
    return -1;

  static constexpr u32 k_freq_table[10] = {
      0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
  const u8* sh = fsb_data + 60u;
  size_t pos = 0;
  if (pos + 8u > shdr_size) return -1;
  const u64 raw0 = rd64(sh + pos);
  pos += 8u;
  int next_chunk = static_cast<int>(raw0 & 1ull);
  const u32 freq_idx = static_cast<u32>((raw0 >> 1) & 0xFull);
  u32 channels = static_cast<u32>(((raw0 >> 5) & 1ull) + 1u);
  const u32 data_off0 = static_cast<u32>(((raw0 >> 6) & 0xFFFFFFFull) * 16u);
  const u32 samples0 = static_cast<u32>((raw0 >> 34) & 0x3FFFFFFFull);
  u32 rate = freq_idx < 10 ? k_freq_table[freq_idx] : 44100u;
  u32 loop_start = 0, loop_end = 0, has_loop = 0;
  while (next_chunk) {
    if (pos + 4u > shdr_size) return -1;
    const u32 craw = rd32(sh + pos);
    pos += 4u;
    next_chunk = static_cast<int>(craw & 1u);
    const u32 csz = (craw >> 1) & 0xFFFFFFu;
    const u32 ctype = (craw >> 25) & 0x7Fu;
    if (pos + csz > shdr_size) return -1;
    if (ctype == 1 && csz >= 1) channels = sh[pos];
    else if (ctype == 2 && csz >= 4) rate = rd32(sh + pos);
    else if (ctype == 3 && csz >= 8) {
      loop_start = rd32(sh + pos);
      loop_end = rd32(sh + pos + 4u);
      has_loop = 1;
    }
    pos += csz;
  }
  u32 data_end = data_size;
  if (num_samples > 1 && pos + 8u <= shdr_size) {
    const u64 raw1 = rd64(sh + pos);
    const u32 off1 = static_cast<u32>(((raw1 >> 6) & 0xFFFFFFFull) * 16u);
    if (off1 > data_off0 && off1 <= data_size) data_end = off1;
  }
  if (!trt_fsb5_sample_sane(samples0, channels ? channels : 1u, &why)) return -1;
  const size_t data_base = 60u + shdr_size + name_size;
  if (data_base + data_end > fsb_len || data_end <= data_off0) return -1;

  if (trt_fsb5_decode_payload(mode, fsb_data + data_base + data_off0,
                              data_end - data_off0, rate, channels, samples0, out) != 0)
    return -1;
  out->loop = has_loop;
  out->loop_start = loop_start;
  out->loop_end = loop_end;
  return 0;
}

int trt_decode_audio_buffer(const u8* data, size_t len, trt_decoded_pcm* out) {
  if (!data || len < 12u || !out) return -1;
  memset(out, 0, sizeof(*out));

  // Ogg Vorbis
  if (memcmp(data, "OggS", 4) == 0) {
    return decode_ogg_to_s16(data, len, out);
  }
  // FSB5
  if (memcmp(data, "FSB5", 4) == 0) {
    return trt_fsb5_decode_first_sample(data, len, out);
  }
  // MP3 (ID3 or MPEG frame sync)
  if (memcmp(data, "ID3", 3) == 0 || (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)) {
    return decode_mp3_to_s16(data, len, out);
  }
  // RIFF / WAVE (standard WAV and Wwise .wem)
  if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) {
    size_t pos = 12u;
    u16 fmt_tag = 0, channels = 0, block_align = 0, bits = 0;
    u32 rate = 0;
    const u8* payload = nullptr;
    u32 payload_len = 0;
    while (pos + 8u <= len) {
      const u8* cid = data + pos;
      const u32 clen = rd32(data + pos + 4u);
      pos += 8u;
      if (pos + clen > len) break;
      if (memcmp(cid, "fmt ", 4) == 0 && clen >= 16u) {
        fmt_tag = rd16(data + pos);
        channels = rd16(data + pos + 2u);
        rate = rd32(data + pos + 4u);
        block_align = rd16(data + pos + 12u);
        bits = rd16(data + pos + 14u);
        if (fmt_tag == 0xFFFEu && clen >= 40u) {
          // WAVEFORMATEXTENSIBLE: SubFormat first u16 gives PCM/Float/IMA tag
          fmt_tag = rd16(data + pos + 24u);
        }
      } else if (memcmp(cid, "data", 4) == 0) {
        payload = data + pos;
        payload_len = clen;
      }
      pos += clen + (clen & 1u);
    }
    if (!payload || !payload_len || !channels || channels > 8 || !rate) return -1;
    if (fmt_tag == 1u) {
      u32 mode = 0;
      if (bits == 8) mode = TRT_FSB5_PCM8;
      else if (bits == 16) mode = TRT_FSB5_PCM16;
      else if (bits == 24) mode = TRT_FSB5_PCM24;
      else if (bits == 32) mode = TRT_FSB5_PCM32;
      else return -1;
      return trt_fsb5_decode_payload(mode, payload, payload_len, rate, channels, 0, out);
    }
    if (fmt_tag == 3u && bits == 32) {
      return trt_fsb5_decode_payload(TRT_FSB5_PCMFLOAT, payload, payload_len,
                                     rate, channels, 0, out);
    }
    if (fmt_tag == 0x0011u || fmt_tag == 0x0002u) {
      // WAV IMA-ADPCM (0x0011) or Wwise ADPCM (0x0002)
      const u32 ba = block_align ? block_align : (36u * channels);
      int16_t* pcm = nullptr;
      u32 got = 0;
      if (trt_decode_ima_adpcm(payload, payload_len, channels, ba, 0, &pcm, &got) != 0)
        return -1;
      out->samples = pcm;
      out->frames = got;
      out->rate = rate;
      out->channels = channels;
      return 0;
    }
  }
  return -1;
}

}  // extern "C"
