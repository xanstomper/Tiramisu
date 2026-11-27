// tiramisu/src/trt/bridge_fmod_studio.cpp
// FMOD Studio engine: real bank parsing + event playback.
//
// Layout references (all verified against Celeste's actual banks with a
// Python prototype before this C++ was written):
//   - Bank format: FMOD Studio bank v0x67 (RIFF/FEV). Chunk walk + node
//     layouts translated from FModBankParser (MIT,
//     github.com/Masusder/FModBankParser): EventNode, TimelineNode,
//     ParameterLayoutNode, Waveform/Multi/Scatterer instruments,
//     PlaylistNode, WaveformResourceNode, ParameterNode, VCANode, BusNode,
//     and the STDT radix-tree string table.
//   - Sample data: FSB5 (FMOD Sample Bank 5). Sample headers, metadata
//     chunks, name table; Vorbis samples are u16-length-prefixed raw
//     packets that must be re-muxed into Ogg (setup packet looked up by
//     the VORBISDATA CRC32) before stb_vorbis can decode them.
//   - Vorbis setup packets: vendored from TriMO-FSB5 0.1.1
//     (Apache 2.0, see fsb_vorbis_setups.h); only the two CRCs Celeste's
//     banks use (0xA4F666FC, 0xC55EFA16).
//
// Verified ground truth (prototype, 2026-11-20): worldmap event ->
// env_amb_worldmap, 48 kHz stereo, 1,617,920 samples, decoded EXACTLY
// (stb_vorbis, RMS 0.0635, non-silent). Event counts: sfx 587 (527
// top-level), music 94 (70), ui 105, Master Bank 69, dlc_sfx 152 (135),
// dlc_music 24 (16); strings bank 996 paths (810 events, 118 buses,
// 58 snapshots, 3 VCAs, 7 banks).
//
// What this engine does NOT do (named gaps, not silent lies):
//   - 3D detection: Is3D always returns false (the spatializer data that
//     decides it lives in the effect chain, not parsed yet). Listener and
//     instance 3D attributes ARE stored for real, and distance attenuation
//     IS applied when attributes are set — but Celeste checks Is3D first,
//     so positioned sounds play centered until 3D detection lands.
//   - Parameter automation: parameter VALUES are real state (set/get
//     round-trips; parameter-gated instruments start/stop on value
//     changes), but volume/pitch automation curves driven by parameters
//     are not applied to the mix.
//   - Snapshots: snapshot events start/stop as state but their mixer
//     changes are not applied (SnapshotNode data not parsed yet).
//   - Per-voice stop/pan: the host mixer (trt_audio_submit_voice) queues
//     PCM per voice with no stop or pan control, so Stop() stops future
//     submits (already-queued ~1s plays out) and 3D pan is attenuation
//     only. Same limitation as the core FMOD bridge.
#include "bridge_fmod_studio.h"
#include "trt_trace.h"

#if defined(_WIN32)
// WinRT MemoryManager for the bank-load memory log (patch 07). UWP
// only: the Linux harness has no WinRT (its branch logs PCM bytes
// only, below).
#include <winrt/base.h>  // winrt::hresult_error
#include <winrt/Windows.System.h>  // MemoryManager::AppMemoryUsage
#endif

#include "audio_codecs.h"
#include "bridge_audio.h"
#include "bridge_fmod.h"
#include "fsb_vorbis_setups.h"
#include "thunk.h"
#include "trt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
// stb_vorbis (vendored src/trt/vendor_stb_vorbis.c). NOTE: the real
// signature takes a caller buffer + size (4 args); bridge_fmod.cpp's old
// 3-arg short** declaration was ABI-wrong and is fixed there too.
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

// Console decode gate, set by the host from the `fsb_decode` setting. Only the
// Windows build consults it (the Linux gate always decodes, because
// test_fmod_studio asserts real PCM).
std::atomic<bool> g_fsb5_console_decode{false};

void trt_fsb5_set_console_decode(int on) {
  g_fsb5_console_decode.store(on != 0);
}
}

namespace {

// Portable 64-bit seek (fseeko is POSIX-only; clang-cl needs _fseeki64).
int file_seek(FILE* f, u64 off) {
#if defined(_WIN32)
  return _fseeki64(f, (long long)off, SEEK_SET);
#else
  return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
u64 file_tell(FILE* f) {
#if defined(_WIN32)
  return (u64)_ftelli64(f);
#else
  return (u64)ftello(f);
#endif
}

// ---- logging (trt_core_log; one-time named lines for unimplemented paths) ----
void log_line(const char* s) { trt_core_log(s); }
void logf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  trt_core_log(buf);
}
void log_once(std::atomic<int>* flag, const char* s) {
  int expect = 0;
  if (flag->compare_exchange_strong(expect, 1)) trt_core_log(s);
}

// ---- little-endian readers over a memory buffer ----
struct reader {
  const u8* p;
  size_t size;
  size_t pos = 0;
  bool ok(size_t n) const { return pos + n <= size; }
  u8 u8v() { return ok(1) ? p[pos++] : 0; }
  u16 u16v() {
    u16 v = 0;
    if (ok(2)) memcpy(&v, p + pos, 2), pos += 2;
    return v;
  }
  u32 u32v() {
    u32 v = 0;
    if (ok(4)) memcpy(&v, p + pos, 4), pos += 4;
    return v;
  }
  i32 i32v() { return (i32)u32v(); }
  u64 u64v() {
    u64 v = 0;
    if (ok(8)) memcpy(&v, p + pos, 8), pos += 8;
    return v;
  }
  float f32v() {
    float v = 0;
    if (ok(4)) memcpy(&v, p + pos, 4), pos += 4;
    return v;
  }
  // FModBankParser ReadX16: u16 low; if the high bit is set, another u16
  // extends it: value = (low & 0x7FFF) | (high << 15).
  u32 x16() {
    u16 lo = u16v();
    if (lo & 0x8000u) {
      u16 hi = u16v();
      return (u32)(lo & 0x7FFFu) | ((u32)hi << 15);
    }
    return lo;
  }
  // Element list: ReadX16 count>>1, one u16 payload size, then count
  // elements. Returns false if the list header is malformed.
  bool elem_list_begin(u32* count_out) {
    u32 raw = x16();
    u32 count = raw >> 1;
    *count_out = count;
    if (count == 0) return true;
    if (!ok(2)) return false;
    pos += 2;  // payload size for the whole list
    return true;
  }
  // Versioned element list: ReadX16 count>>1, then per element a u16
  // payload size + that many bytes. Skips the whole list.
  bool skip_versioned_list() {
    u32 raw = x16();
    u32 count = raw >> 1;
    for (u32 i = 0; i < count; ++i) {
      if (!ok(2)) return false;
      u16 sz = u16v();
      if (!ok(sz)) return false;
      pos += sz;
    }
    return true;
  }
  // FModBankParser ReadString: ReadX16 length + UTF-8 bytes (no NUL).
  std::string str() {
    u32 len = x16();
    if (len == 0 || !ok(len)) return std::string();
    std::string s((const char*)p + pos, len);
    pos += len;
    return s;
  }
  void guid(u8 out[16]) {
    if (ok(16)) memcpy(out, p + pos, 16), pos += 16;
    else memset(out, 0, 16);
  }
};

struct guid16 {
  u8 b[16];
  bool operator==(const guid16& o) const {
    return memcmp(b, o.b, 16) == 0;
  }
  bool is_zero() const {
    for (int i = 0; i < 16; ++i)
      if (b[i]) return false;
    return true;
  }
};
struct guid_hash {
  size_t operator()(const guid16& g) const {
    u64 a, b;
    memcpy(&a, g.b, 8);
    memcpy(&b, g.b + 8, 8);
    return (size_t)(a ^ (b * 0x9e3779b97f4a7c15ull));
  }
};
guid16 guid_from(const u8 in[16]) {
  guid16 g;
  memcpy(g.b, in, 16);
  return g;
}
bool guid_less(const guid16& a, const guid16& b) {
  return memcmp(a.b, b.b, 16) < 0;
}

// dB -> linear (FMOD bank mixer strips store volume in dB; 0 dB = unity).
float db_to_linear(float db) {
  if (db <= -80.0f) return 0.0f;
  return powf(10.0f, db / 20.0f);
}

// ---- Ogg muxer (minimal; one page per packet, as TriMO-FSB5 does) ----
// Ogg CRC-32: polynomial 0x04C11DB7, init 0, no reflection, no final xor.
u32 ogg_crc_table[256];
bool ogg_crc_table_ready = false;
void ogg_crc_init() {
  if (ogg_crc_table_ready) return;
  for (u32 i = 0; i < 256; ++i) {
    u32 r = i << 24;
    for (int j = 0; j < 8; ++j)
      r = (r & 0x80000000u) ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
    ogg_crc_table[i] = r;
  }
  ogg_crc_table_ready = true;
}
u32 ogg_crc(const u8* data, size_t len) {
  ogg_crc_init();
  u32 crc = 0;
  for (size_t i = 0; i < len; ++i)
    crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) ^ data[i]) & 0xFF];
  return crc;
}

// Append one Ogg page holding a single packet. header_type: 0x02 BOS on
// the first page, 0x04 EOS on the last; granulepos -1 unless known.
// Packets larger than a page (255 segments = 65025 bytes) span multiple
// pages; only the last carries the granulepos / EOS flag.
void ogg_page_append(std::vector<u8>* out, u32* seq_io, const u8* packet,
                     size_t packet_len, i64 granulepos, u8 header_type) {
  const u32 serial = 1;
  size_t offset = 0;
  bool first = true;
  while (first || offset < packet_len) {
    first = false;
    // This page takes up to 255 segments (65025 bytes); a packet that
    // ends exactly on a segment boundary needs a trailing 0-length
    // segment on the final page so the packet end is visible.
    size_t take = packet_len - offset;
    bool last_page = take <= 65025;
    if (!last_page) take = 65025;
    u8 segs[256];
    size_t nseg = 0;
    size_t remaining = take;
    do {
      u8 s = remaining >= 255 ? 255 : (u8)remaining;
      segs[nseg++] = s;
      remaining -= s;
    } while (remaining > 0);
    if (last_page && take > 0 && (take % 255) == 0) segs[nseg++] = 0;
    if (last_page && take == 0) segs[nseg++] = 0;
    const size_t header_len = 27 + nseg;
    const size_t page_start = out->size();
    out->resize(page_start + header_len + take);
    u8* h = out->data() + page_start;
    memcpy(h, "OggS", 4);
    h[4] = 0;  // version
    h[5] = (u8)(header_type & (last_page ? 0xFF : 0x02));
    // Continued packets: header_type bit 0x01 on non-first pages.
    if (offset > 0) h[5] |= 0x01;
    i64 gp = last_page ? granulepos : -1;
    memcpy(h + 6, &gp, 8);
    memcpy(h + 14, &serial, 4);
    u32 seq = (*seq_io)++;
    memcpy(h + 18, &seq, 4);
    memset(h + 22, 0, 4);  // crc placeholder
    h[26] = (u8)nseg;
    memcpy(h + 27, segs, nseg);
    if (take) memcpy(h + header_len, packet + offset, take);
    u32 crc = ogg_crc(out->data() + page_start, header_len + take);
    memcpy(h + 22, &crc, 4);
    offset += take;
  }
}

// Vorbis id header (30 bytes), rebuilt from channels/rate like TriMO does:
// blocksizes 256/2048 -> blocksize byte (11<<4)|8 = 0xB8 (long 2^11,
// short 2^8). NOTE: 0x74 is wrong (that would be 128/16); fixed 2026-11-20
// after stb_vorbis rejected the stream (err=20).
void vorbis_id_header(u8 out[30], int channels, u32 rate) {
  memset(out, 0, 30);
  out[0] = 0x01;
  memcpy(out + 1, "vorbis", 6);
  // version u32 = 0 at +7
  out[11] = (u8)channels;
  memcpy(out + 12, &rate, 4);
  // bitrate upper/nominal/lower u32 = 0 at +16/+20/+24
  out[28] = 0xB8;  // short 256 (2^8), long 2048 (2^11)
  out[29] = 0x01;  // framing bit
}

// Minimal comment header: empty vendor, zero comments.
const u8 k_vorbis_comment_header[] = {0x03, 'v', 'o', 'r', 'b', 'i', 's',
                                      0x00, 0x00, 0x00, 0x00,  // vendor len 0
                                      0x00, 0x00, 0x00, 0x00,  // 0 comments
                                      0x01};                   // framing bit

const u8* find_vorbis_setup(u32 crc, size_t* len_out) {
  for (size_t i = 0; i < sizeof(kFsbVorbisSetups) / sizeof(kFsbVorbisSetups[0]);
       ++i) {
    if (kFsbVorbisSetups[i].crc == crc) {
      *len_out = kFsbVorbisSetups[i].len;
      return kFsbVorbisSetups[i].data;
    }
  }
  return nullptr;
}

// ---- FSB5 sample table (index only; sample data stays on disk) ----
struct fsb_sample {
  std::string name;
  u32 frequency = 0;
  u32 channels = 0;
  u32 samples = 0;       // total PCM frames in the sample
  u64 file_offset = 0;   // absolute file offset of the sample data
  u32 data_len = 0;      // compressed bytes
  u32 vorbis_crc = 0;    // 0 = no VORBISDATA chunk (not Vorbis)
  bool has_loop = false;
  u32 loop_start = 0;
  u32 loop_end = 0;
};

struct fsb_bank {
  u32 mode = 0;  // 15 = Vorbis for all of Celeste's banks
  std::vector<fsb_sample> samples;
};

// Parse one FSB5 blob at absolute file offset `fsb_off` in `f`.
// Reads only the header + sample headers + name table (not sample data).
bool fsb_parse(FILE* f, u64 fsb_off, u64 file_bytes, fsb_bank* out) {
  if (file_seek(f, fsb_off) != 0) return false;
  u8 hdr[60];
  if (fread(hdr, 1, 60, f) != 60) return false;
  if (memcmp(hdr, "FSB5", 4) != 0) return false;
  reader r{hdr, 60};
  r.pos = 4;
  u32 version = r.u32v();
  (void)version;
  u32 num_samples = r.u32v();
  u32 shdr_size = r.u32v();
  u32 name_size = r.u32v();
  u32 data_size = r.u32v();
  u32 mode = r.u32v();
  // A corrupt header used to reach reserve()/vector<> with whatever the file
  // said, which is the shape of the console crash that disabled decode. Every
  // refusal below is named and happens before a byte is allocated or read.
  const char* why = nullptr;
  if (!trt_fsb5_header_sane(num_samples, shdr_size, name_size, data_size,
                            file_bytes > fsb_off ? file_bytes - fsb_off : 0,
                            &why)) {
    logf("fmod-studio: FSB5 at 0x%llx rejected: %s (samples=%u shdr=%u "
         "names=%u data=%u filesize=%llu)",
         (unsigned long long)fsb_off, why ? why : "unspecified", num_samples,
         shdr_size, name_size, data_size, (unsigned long long)file_bytes);
    return false;
  }
  out->mode = mode;
  out->samples.clear();
  out->samples.reserve(num_samples);
  // Sample headers follow the 60-byte header.
  std::vector<u8> shdr(shdr_size);
  if (shdr_size && fread(shdr.data(), 1, shdr_size, f) != shdr_size)
    return false;
  reader sr{shdr.data(), shdr.size()};
  const u32 k_freq_table[10] = {0, 8000, 11000, 11025, 16000,
                                22050, 24000, 32000, 44100, 48000};
  std::vector<u32> data_offsets;
  data_offsets.reserve(num_samples);
  for (u32 i = 0; i < num_samples; ++i) {
    u64 raw = sr.u64v();
    int next_chunk = (int)(raw & 1ull);
    u32 freq_idx = (u32)((raw >> 1) & 0xFull);
    u32 channels = (u32)(((raw >> 5) & 1ull) + 1);
    u32 data_off = (u32)(((raw >> 6) & 0xFFFFFFFull) * 16u);
    u32 samples = (u32)((raw >> 34) & 0x3FFFFFFFull);
    fsb_sample s;
    s.frequency = freq_idx < 10 ? k_freq_table[freq_idx] : 0;
    s.channels = channels;
    s.samples = samples;
    data_offsets.push_back(data_off);
    while (next_chunk) {
      u32 craw = sr.u32v();
      next_chunk = (int)(craw & 1u);
      u32 chunk_size = (craw >> 1) & 0xFFFFFFu;
      u32 chunk_type = (craw >> 25) & 0x7Fu;
      if (!sr.ok(chunk_size)) return false;
      const u8* cd = sr.p + sr.pos;
      if (chunk_type == 1) {  // CHANNELS
        if (chunk_size >= 1) s.channels = cd[0];
      } else if (chunk_type == 2) {  // FREQUENCY
        if (chunk_size >= 4) memcpy(&s.frequency, cd, 4);
      } else if (chunk_type == 3) {  // LOOP
        if (chunk_size >= 8) {
          memcpy(&s.loop_start, cd, 4);
          memcpy(&s.loop_end, cd + 4, 4);
          s.has_loop = true;
        }
      } else if (chunk_type == 11) {  // VORBISDATA
        if (chunk_size >= 4) memcpy(&s.vorbis_crc, cd, 4);
      }
      sr.pos += chunk_size;
    }
    out->samples.push_back(std::move(s));
  }
  // Name table: num_samples u32 offsets, then NUL-terminated names.
  std::vector<u8> names(name_size);
  if (name_size && fread(names.data(), 1, name_size, f) != name_size)
    return false;
  if ((u64)name_size >= (u64)num_samples * 4ull) {
    for (u32 i = 0; i < num_samples; ++i) {
      u32 off = 0;
      memcpy(&off, names.data() + i * 4, 4);
      if (off < name_size) {
        const char* n = (const char*)names.data() + off;
        size_t maxlen = name_size - off;
        size_t len = 0;
        while (len < maxlen && n[len]) ++len;
        out->samples[i].name.assign(n, len);
      }
    }
  }
  // Absolute file offsets: data starts after header + sample headers +
  // name table. Each sample spans to the next sample's offset (or the
  // data size for the last).
  u64 data_base = fsb_off + 60ull + shdr_size + name_size;
  for (u32 i = 0; i < num_samples; ++i) {
    u32 end = (i + 1 < num_samples) ? data_offsets[i + 1] : data_size;
    out->samples[i].file_offset = data_base + data_offsets[i];
    out->samples[i].data_len =
        end > data_offsets[i] ? end - data_offsets[i] : 0;
  }
  return true;
}

}  // namespace

// ---- FSB5 envelope checks ----
extern "C" {
// Limits, and where they come from:
//   * a Studio bank holds at most a few thousand samples (Celeste's largest is
//     587 events / ~700 samples), so 65536 is already absurd;
//   * every sample header is at least 8 bytes, every name index 4;
//   * 1 MiB of sample headers and 4 MiB of names is larger than any shipping
//     bank, and small enough that a corrupt size cannot exhaust the console;
//   * the declared blocks must fit in the file that was actually opened.
// The point is not the exact number: it is that a bad bank fails with a name
// instead of an allocation or a read past the end.
int trt_fsb5_header_sane(u32 num_samples, u32 shdr_size, u32 name_size,
                         u32 data_size, u64 file_bytes, const char** why) {
  const char* ignored = "";
  if (!why) why = &ignored;
  *why = "";
  if (file_bytes < 60) {
    *why = "file shorter than the 60-byte FSB5 header";
    return 0;
  }
  if (num_samples == 0) {
    *why = "header declares no samples";
    return 0;
  }
  if (num_samples > 65536u) {
    *why = "sample count above the 65536 sanity limit";
    return 0;
  }
  if (shdr_size > (1u << 20)) {
    *why = "sample-header table above the 1 MiB sanity limit";
    return 0;
  }
  if ((u64)shdr_size < (u64)num_samples * 8ull) {
    *why = "sample-header table shorter than the sample count";
    return 0;
  }
  if (name_size > (4u << 20)) {
    *why = "name table above the 4 MiB sanity limit";
    return 0;
  }
  if (name_size != 0 && (u64)name_size < (u64)num_samples * 4ull) {
    *why = "name table shorter than its own index";
    return 0;
  }
  const u64 declared = 60ull + (u64)shdr_size + (u64)name_size +
                       (u64)data_size;
  if (declared > file_bytes) {
    *why = "declared header+names+data exceeds the file size";
    return 0;
  }
  return 1;
}

// A decoded sample becomes a heap allocation of frames*channels*2 bytes, so a
// corrupt frame count is a memory bomb. 20M frames is ~7 minutes at 48 kHz
// (80 MB stereo); longer than any single FMOD sample in Celeste's banks.
int trt_fsb5_sample_sane(u32 frames, u32 channels, const char** why) {
  const char* ignored = "";
  if (!why) why = &ignored;
  *why = "";
  if (frames == 0) {
    *why = "sample declares zero frames";
    return 0;
  }
  if (channels == 0 || channels > 8) {
    *why = "channel count out of range";
    return 0;
  }
  if (frames > 20000000u) {
    *why = "frame count above the 20M decode limit";
    return 0;
  }
  return 1;
}
}  // extern "C"

// ---- bank data (parsed metadata; sample data stays on disk) ----
struct trigger_box {
  guid16 instrument;
  u32 start_ms = 0;
  u32 length_ms = 0;
};
struct layout_box {
  guid16 instrument;
  float range_lo = 0;
  float range_hi = 0;
  bool include_end = false;
};
struct playlist_entry {
  guid16 instrument;
  float weight = 1.0f;
};

struct bank_data {
  std::string path;
  FILE* f = nullptr;
  u64 file_size = 0;
  int version = 0;
  guid16 bank_guid{};
  int top_level_event_count = 0;
  bool is_strings_bank = false;

  struct event_node {
    guid16 timeline;
    guid16 snapshot;
    guid16 input_bus;
    std::vector<guid16> layouts;
  };
  std::unordered_map<guid16, event_node, guid_hash> events;
  // timeline guid -> trigger boxes (time-based) + named-marker flag
  std::unordered_map<guid16, std::vector<trigger_box>, guid_hash> timelines;
  std::unordered_map<guid16, bool, guid_hash> timeline_has_markers;
  struct layout_node {
    guid16 param;
    std::vector<layout_box> boxes;
  };
  std::unordered_map<guid16, layout_node, guid_hash> layouts;
  struct instrument {
    int kind = 3;  // 0=wav, 1=multi, 2=scatterer, 3=other
    guid16 wav;
    guid16 timeline;  // from the following INST chunk
    std::vector<playlist_entry> playlist;
    int play_mode = 0;
    int selection_mode = 1;
  };
  std::unordered_map<guid16, instrument, guid_hash> instruments;
  struct wav_res {
    int fsb_index = 0;
    int subsound = 0;
  };
  std::unordered_map<guid16, wav_res, guid_hash> wavs;
  struct param_node {
    std::string name;
    float min_v = 0, max_v = 1, def_v = 0;
    int type = 0;
  };
  std::unordered_map<guid16, param_node, guid_hash> params;
  struct vca_node {
    std::vector<guid16> strips;
    float volume_db = 0.0f;
  };
  std::unordered_map<guid16, vca_node, guid_hash> vcas;
  struct bus_node {
    float volume_db = 0.0f;
  };
  std::unordered_map<guid16, bus_node, guid_hash> buses;

  // strings bank data
  std::vector<guid16> string_guids;
  std::vector<std::pair<u32, u32>> string_nodes;  // (keyinfo, childinfo)
  std::vector<u8> string_blob;
  std::vector<u32> string_leaf;
  std::vector<u32> string_parent;
  std::unordered_map<std::string, guid16> path_to_guid;
  std::unordered_map<guid16, std::string, guid_hash> guid_to_path_map;

  std::vector<std::pair<u32, u32>> fsb_offsets;  // SNDH (offset, length)
  std::vector<fsb_bank> fsbs;

  int loading_state = TRT_STUDIO_LOADING_UNLOADED;
  int sample_loading_state = TRT_STUDIO_LOADING_UNLOADED;

  std::string guid_to_path(const guid16& g) const {
    auto it = guid_to_path_map.find(g);
    return it != guid_to_path_map.end() ? it->second : std::string();
  }
};

// Read `len` bytes at absolute file offset `off` into `dst`. Returns false
// on short read.
bool file_read_at(FILE* f, u64 off, u8* dst, size_t len) {
  if (file_seek(f, off) != 0) return false;
  return len == 0 || fread(dst, 1, len, f) == len;
}

// Skip an element list in a reader (count + payload size + elements).
bool skip_elem_list(reader* r) {
  u32 count = 0;
  if (!r->elem_list_begin(&count)) return false;
  // elem_list_begin consumed the payload-size u16; re-read it to know the
  // element size. (We consumed it already; recompute from position.)
  // Simpler: the payload size is the u16 just before the current position.
  u16 payload = 0;
  if (count == 0) return true;
  memcpy(&payload, r->p + r->pos - 2, 2);
  if (!r->ok((size_t)count * payload)) return false;
  r->pos += (size_t)count * payload;
  return true;
}

struct bank_walker {
  bank_data* b;
  FILE* f;
  guid16 pending_inst{};
  bool has_pending_inst = false;
  guid16 pending_multi{};
  bool has_pending_multi = false;
  guid16 pending_bus{};
  bool has_pending_bus = false;

  void walk(u64 start, u64 end) {
    u64 pos = start;
    while (pos + 8 <= end) {
      u64 node_start = pos;
      u8 hdr[8];
      if (!file_read_at(f, pos, hdr, 8)) return;
      u32 raw = 0;
      memcpy(&raw, hdr, 4);
      if ((raw & 0xFFu) == 0) {
        // FModReader quirk: a NUL low byte means the chunk id is
        // misaligned by 3 bytes; back up and re-read.
        if (pos < 3) return;
        pos -= 3;
        node_start = pos;
        if (!file_read_at(f, pos, hdr, 8)) return;
        memcpy(&raw, hdr, 4);
      }
      u32 size = 0;
      memcpy(&size, hdr + 4, 4);
      u64 body = pos + 8;
      // RIFF chunks are word-aligned: an odd-sized chunk is followed by
      // one pad byte. chunk_end bounds the recursion into LISTs; the
      // step to the next chunk includes the pad (missing it desyncs the
      // walk on the first odd-sized chunk — found 2026-11-20).
      u64 chunk_end = node_start + 8ull + size;
      u64 next_node = chunk_end + (size & 1ull);
      if (chunk_end > end) return;  // malformed chunk size: stop the walk
      if (size > 0) handle_chunk(hdr, body, size, chunk_end);
      pos = next_node;
    }
  }

  // Read a whole (small) chunk body into memory for parsing.
  bool read_body(u64 body, u32 size, std::vector<u8>* out) {
    out->resize(size);
    return size == 0 || file_read_at(f, body, out->data(), size);
  }

  void handle_chunk(const u8* hdr, u64 body, u32 size, u64 chunk_end) {
    const u32 id = (u32)hdr[0] | ((u32)hdr[1] << 8) | ((u32)hdr[2] << 16) |
                   ((u32)hdr[3] << 24);
    switch (id) {
      case 0x5453494cu: {  // LIST
        u8 type[4];
        if (file_read_at(f, body, type, 4)) walk(body + 4, chunk_end);
        return;
      }
      case 0x544e434cu:  // LCNT: count only, no body to parse
        return;
      case 0x20544d46: {  // FMT
        std::vector<u8> buf;
        if (read_body(body, size, &buf) && buf.size() >= 4) {
          reader r{buf.data(), buf.size()};
          b->version = r.i32v();
        }
        return;
      }
      case 0x494b4e42: {  // BNKI
        std::vector<u8> buf;
        if (read_body(body, size, &buf) && buf.size() >= 32) {
          reader r{buf.data(), buf.size()};
          r.guid(b->bank_guid.b);
          r.u64v();  // hash (>=0x37)
          b->top_level_event_count = r.i32v();  // >=0x41
        }
        return;
      }
      case 0x54445453:  // STDT
        parse_stdt(body, size);
        return;
      case 0x48444e53: {  // SNDH
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        u32 count = 0;
        if (!r.elem_list_begin(&count)) return;
        for (u32 i = 0; i < count; ++i) {
          u32 off = r.u32v(), len = r.u32v();
          b->fsb_offsets.push_back({off, len});
        }
        return;
      }
      case 0x20444e53:  // SND
        parse_snd(body, size);
        return;
      case 0x42545645: {  // EVTB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::event_node ev;
        r.guid(ev.snapshot.b);
        r.guid(ev.timeline.b);
        r.guid(ev.input_bus.b);
        guid16 master;
        r.guid(master.b);
        r.i32v();  // max polyphony
        r.i32v();  // priority
        r.u8v();   // polyphony limit behavior (byte at <0x34... at 0x67 the
                   // byte form is still read first; the i32 follows later)
        r.i32v();  // scheduling mode
        u32 nlayouts = 0;
        if (!r.elem_list_begin(&nlayouts)) return;
        for (u32 i = 0; i < nlayouts; ++i) {
          guid16 lg;
          r.guid(lg.b);
          ev.layouts.push_back(lg);
        }
        if (!r.skip_versioned_list()) return;  // user property floats
        if (!r.skip_versioned_list()) return;  // user property strings
        r.f32v();                              // doppler scale (>=0x30)
        r.i32v();  // polyphony limit behavior i32 (>=0x34)
        r.f32v();  // trigger cooldown (>=0x4e)
        r.u32v();  // flags (>=0x61)
        b->events[g] = std::move(ev);
        return;
      }
      case 0x424e4c54: {  // TLNB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        guid16 legacy;
        r.guid(legacy.b);  // <0x6d
        std::vector<trigger_box> boxes;
        u32 n = 0;
        if (!r.elem_list_begin(&n)) return;
        // Trigger-box times are SAMPLES at 48 kHz, not milliseconds
        // (verified: the worldmap box length 1617920 is exactly the
        // sample's frame count). Convert to ms at parse time; the
        // mixer runs at 48 kHz, so /48 is exact.
        for (u32 i = 0; i < n; ++i) {
          trigger_box tb;
          r.guid(tb.instrument.b);
          tb.start_ms = r.u32v() / 48;
          tb.length_ms = r.u32v() / 48;
          boxes.push_back(tb);
        }
        if (!r.elem_list_begin(&n)) return;  // time-locked trigger boxes
        for (u32 i = 0; i < n; ++i) {
          trigger_box tb;
          r.guid(tb.instrument.b);
          tb.start_ms = r.u32v() / 48;
          tb.length_ms = r.u32v() / 48;
          boxes.push_back(tb);
        }
        b->timelines[g] = std::move(boxes);
        // Legacy uint array (<0x84): skip.
        if (!skip_elem_list(&r)) return;
        // Named markers (versioned list): count them for HasCue, then skip.
        {
          u32 raw = r.x16();
          u32 mcount = raw >> 1;
          b->timeline_has_markers[g] = mcount > 0;
          for (u32 i = 0; i < mcount; ++i) {
            if (!r.ok(2)) return;
            u16 sz = r.u16v();
            if (!r.ok(sz)) return;
            r.pos += sz;
          }
        }
        // Tempo markers: elem list, skip.
        skip_elem_list(&r);
        return;
      }
      case 0x424c4d50: {  // PMLB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::layout_node ln;
        r.guid(ln.param.b);
        guid16 legacy;
        r.guid(legacy.b);  // <0x6d
        u32 n = 0;
        if (!r.elem_list_begin(&n)) return;
        for (u32 i = 0; i < n; ++i) {
          layout_box lb;
          r.guid(lb.instrument.b);
          lb.range_lo = r.f32v();
          lb.range_hi = r.f32v();
          lb.include_end = r.u8v() != 0;
          ln.boxes.push_back(lb);
        }
        b->layouts[g] = std::move(ln);
        return;
      }
      case 0x42494157: {  // WAIB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 32) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::instrument in;
        in.kind = 0;
        r.guid(in.wav.b);
        b->instruments[g] = in;
        pending_inst = g;
        has_pending_inst = true;
        return;
      }
      case 0x4249554d:  // MUIB
      case 0x42495053: {  // SPIB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 16) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::instrument in;
        in.kind = (id == 0x4249554du) ? 1 : 2;
        b->instruments[g] = in;
        pending_inst = g;
        has_pending_inst = true;
        pending_multi = g;
        has_pending_multi = true;
        return;
      }
      case 0x42495250:  // PRIB
      case 0x42495645:  // EVIB
      case 0x42444d43:  // CMDB
      case 0x424e4c53:  // SLNB
      case 0x42494645: {  // EFIB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 16) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::instrument in;
        in.kind = 3;
        b->instruments[g] = in;
        pending_inst = g;
        has_pending_inst = true;
        return;
      }
      case 0x54534e49: {  // INST
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 16) return;
        reader r{buf.data(), buf.size()};
        guid16 tl;
        r.guid(tl.b);
        if (has_pending_inst) {
          auto it = b->instruments.find(pending_inst);
          if (it != b->instruments.end()) it->second.timeline = tl;
        }
        has_pending_inst = false;
        return;
      }
      case 0x54534c50: {  // PLST
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        int play_mode = r.i32v();
        int selection_mode = r.i32v();
        u32 n = 0;
        if (!r.elem_list_begin(&n)) return;
        std::vector<playlist_entry> entries;
        for (u32 i = 0; i < n; ++i) {
          playlist_entry e;
          r.guid(e.instrument.b);
          e.weight = r.f32v();
          entries.push_back(e);
        }
        if (has_pending_multi) {
          auto it = b->instruments.find(pending_multi);
          if (it != b->instruments.end()) {
            it->second.playlist = std::move(entries);
            it->second.play_mode = play_mode;
            it->second.selection_mode = selection_mode;
          }
        }
        has_pending_multi = false;
        return;
      }
      case 0x20564157: {  // WAV
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 26) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        r.u16v();  // payload size
        bank_data::wav_res w;
        w.fsb_index = r.i32v();
        w.subsound = r.i32v();
        b->wavs[g] = w;
        return;
      }
      case 0x424d5250: {  // PRMB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        r.u8v();  // bool (version < 0x70)
        bank_data::param_node pn;
        pn.type = (int)r.u32v();
        pn.name = r.str();
        pn.min_v = r.f32v();
        pn.max_v = r.f32v();
        pn.def_v = r.f32v();
        r.f32v();  // velocity
        r.f32v();  // seek speed (<0x8f)
        r.u8v();   // bool (<0x70)
        r.f32v();  // seek speed down (0x52 < v <= 0x8e)
        r.u8v();   // bool (0x52 < v <= 0x6f)
        b->params[g] = std::move(pn);
        return;
      }
      case 0x42414356: {  // VCAB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        guid16 g;
        r.guid(g.b);
        bank_data::vca_node vn;
        u32 n = 0;
        if (!r.elem_list_begin(&n)) return;
        for (u32 i = 0; i < n; ++i) {
          guid16 s;
          r.guid(s.b);
          vn.strips.push_back(s);
        }
        r.u16v();  // mixer strip payload size
        vn.volume_db = r.f32v();
        r.f32v();  // pitch
        b->vcas[g] = std::move(vn);
        return;
      }
      case 0x42534249:  // IBSB
      case 0x42534247:  // GBSB
      case 0x42534252:  // RBSB
      case 0x4253424d: {  // MBSB
        std::vector<u8> buf;
        if (!read_body(body, size, &buf) || buf.size() < 16) return;
        reader r{buf.data(), buf.size()};
        r.guid(pending_bus.b);
        has_pending_bus = true;
        return;
      }
      case 0x20535542: {  // BUS
        std::vector<u8> buf;
        if (!read_body(body, size, &buf)) return;
        reader r{buf.data(), buf.size()};
        r.u8v();   // flags (version < 0x8c)
        r.u32v();  // input channel layout
        if (!skip_elem_list(&r)) return;  // pre-fader effects
        if (!skip_elem_list(&r)) return;  // post-fader effects
        r.u16v();                         // mixer strip payload size
        float vol_db = r.f32v();
        if (has_pending_bus) {
          bank_data::bus_node bn;
          bn.volume_db = vol_db;
          b->buses[pending_bus] = bn;
        }
        has_pending_bus = false;
        return;
      }
      default:
        return;  // unknown chunk: skipped by size
    }
  }

  void parse_stdt(u64 body, u32 size) {
    std::vector<u8> buf;
    if (!read_body(body, size, &buf)) return;
    reader r{buf.data(), buf.size()};
    u32 type = r.u32v();
    if (type != 1) {
      logf("fmod-studio: strings bank uses string-table type %u (want 1, "
           "radix-tree 24-bit); paths will not resolve",
           type);
      return;
    }
    b->is_strings_bank = true;
    u32 nnodes = 0;
    if (!r.elem_list_begin(&nnodes)) return;
    for (u32 i = 0; i < nnodes; ++i) {
      u32 key = r.u32v(), child = r.u32v();
      b->string_nodes.push_back({key, child});
    }
    u32 nguids = 0;
    if (!r.elem_list_begin(&nguids)) return;
    for (u32 i = 0; i < nguids; ++i) {
      guid16 g;
      r.guid(g.b);
      b->string_guids.push_back(g);
    }
    u32 blob_len = r.x16();
    if (!r.ok(blob_len)) return;
    b->string_blob.assign(r.p + r.pos, r.p + r.pos + blob_len);
    r.pos += blob_len;
    u32 nleaf = r.x16();
    for (u32 i = 0; i < nleaf; ++i) {
      if (!r.ok(3)) return;
      u32 v = r.p[r.pos] | ((u32)r.p[r.pos + 1] << 8) |
              ((u32)r.p[r.pos + 2] << 16);
      r.pos += 3;
      b->string_leaf.push_back(v);
    }
    u32 nparent = r.x16();
    for (u32 i = 0; i < nparent; ++i) {
      if (!r.ok(3)) return;
      u32 v = r.p[r.pos] | ((u32)r.p[r.pos + 1] << 8) |
              ((u32)r.p[r.pos + 2] << 16);
      r.pos += 3;
      b->string_parent.push_back(v);
    }
    // Resolve every GUID -> path once (996 for Celeste; cheap).
    for (size_t i = 0; i < b->string_guids.size(); ++i) {
      std::string path = resolve_string_path(i);
      b->path_to_guid[path] = b->string_guids[i];
      b->guid_to_path_map[b->string_guids[i]] = path;
    }
  }

  std::string resolve_string_path(size_t guid_idx) {
    if (guid_idx >= b->string_leaf.size()) return std::string();
    u32 node = b->string_leaf[guid_idx];
    if (node == 0xFFFFFFu) return std::string();
    std::vector<std::string> segs;
    int guard = 0;
    while (node != 0xFFFFFFu && node < b->string_nodes.size() &&
           guard++ < 10000) {
      u32 key = b->string_nodes[node].first;
      u32 off = key & 0xFFFFFFu;
      if (off != 0xFFFFFFu && off < b->string_blob.size()) {
        const char* s = (const char*)b->string_blob.data() + off;
        size_t maxlen = b->string_blob.size() - off;
        size_t len = 0;
        while (len < maxlen && s[len]) ++len;
        segs.push_back(std::string(s, len));
      }
      if (node >= b->string_parent.size()) break;
      node = b->string_parent[node];
    }
    std::string out;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) out += *it;
    return out;
  }

  void parse_snd(u64 body, u32 size) {
    // Preferred: SNDH offsets (relative to the SND chunk body, or
    // absolute — try both, verify the FSB5 magic). Fallback: scan.
    for (auto& off_len : b->fsb_offsets) {
      u64 candidates[2] = {body + off_len.first, (u64)off_len.first};
      for (int c = 0; c < 2; ++c) {
        u8 magic[4];
        if (file_read_at(f, candidates[c], magic, 4) &&
            memcmp(magic, "FSB5", 4) == 0) {
          fsb_bank fb;
          if (fsb_parse(f, candidates[c], b->file_size, &fb))
            b->fsbs.push_back(std::move(fb));
          break;
        }
      }
    }
    if (!b->fsbs.empty()) return;
    // Scan the SND chunk for FSB5 in 1 MB blocks.
    const u64 k_block = 1024ull * 1024ull;
    std::vector<u8> block((size_t)k_block + 4);
    u64 pos = body;
    u64 end = body + size;
    while (pos < end) {
      u64 want = end - pos < k_block ? end - pos : k_block;
      if (!file_read_at(f, pos, block.data(), (size_t)want)) return;
      for (u64 i = 0; i + 4 <= want; ++i) {
        if (memcmp(block.data() + i, "FSB5", 4) == 0) {
          fsb_bank fb;
          if (fsb_parse(f, pos + i, b->file_size, &fb))
            b->fsbs.push_back(std::move(fb));
        }
      }
      pos += want;
    }
  }
};

// Load + parse a bank file. On failure returns nullptr and logs the reason.
bank_data* bank_load(const char* path) {
  trt_trace("TRACE bank_load enter");
  FILE* f = fopen(path, "rb");
  if (!f) {
    logf("fmod-studio: LoadBankFile(%s) -> file not found", path);
    return nullptr;
  }
  file_seek(f, 0);
  u8 riff[12];
  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "FEV ", 4) != 0) {
    logf("fmod-studio: LoadBankFile(%s) -> not an FMOD Studio bank "
         "(missing RIFF/FEV header)",
         path);
    fclose(f);
    return nullptr;
  }
  // File size (seek to end).
#if defined(_WIN32)
  _fseeki64(f, 0, SEEK_END);
#else
  fseeko(f, 0, SEEK_END);
#endif
  u64 size = file_tell(f);
  bank_data* b = new bank_data();
  b->path = path;
  b->f = f;
  b->file_size = size;
  bank_walker w{b, f};
  w.walk(12, size);
  if (b->version != 0x67) {
    logf("fmod-studio: LoadBankFile(%s) -> bank format version 0x%x, "
         "only 0x67 is supported (refusing to misparse)",
         path, b->version);
    fclose(f);
    delete b;
    return nullptr;
  }
  b->loading_state = TRT_STUDIO_LOADING_LOADED;
  b->sample_loading_state = TRT_STUDIO_LOADING_LOADED;
  logf("fmod-studio: loaded bank %s (events=%d timelines=%d instruments=%d "
       "wavs=%d fsb=%d strings=%d)",
       path, (int)b->events.size(), (int)b->timelines.size(),
       (int)b->instruments.size(), (int)b->wavs.size(), (int)b->fsbs.size(),
       (int)b->string_guids.size());
  // Memory instrumentation (oom-analysis.md section 6.2): log app memory
  // + PCM cache bytes after each bank load. Requires patch 06 (the
  // trt_fmod_studio_pcm_cache_bytes accessor). On Linux (no WinRT) only
  // the PCM cache bytes are logged.
#if defined(_WIN32)
  {
    char mem[256];
    try {
      using winrt::Windows::System::MemoryManager;
      const u64 usage = MemoryManager::AppMemoryUsage();
      const u64 limit = MemoryManager::AppMemoryUsageLimit();
      const u64 pcm = trt_fmod_studio_pcm_cache_bytes();
      std::snprintf(mem, sizeof(mem),
                    "fmod-studio: mem after bank %s: app %llu/%llu MiB, "
                    "pcm cache %llu MiB",
                    path, (unsigned long long)(usage / (1024 * 1024)),
                    (unsigned long long)(limit / (1024 * 1024)),
                    (unsigned long long)(pcm / (1024 * 1024)));
    } catch (const winrt::hresult_error&) {
      std::snprintf(mem, sizeof(mem),
                    "fmod-studio: mem after bank %s: app usage query failed",
                    path);
    }
    logf("%s", mem);
  }
#else
  logf("fmod-studio: mem after bank %s: pcm cache %llu MiB", path,
       (unsigned long long)(trt_fmod_studio_pcm_cache_bytes() /
                            (1024 * 1024)));
#endif
  return b;
}

void bank_free(bank_data* b) {
  if (!b) return;
  if (b->f) fclose(b->f);
  delete b;
}

namespace {

// ---- Vorbis decode: FSB sample -> Ogg rebuild -> stb_vorbis -> PCM ----
struct decoded_sample {
  std::vector<short> pcm;  // s16 interleaved
  u32 rate = 0;
  u32 channels = 0;
  u32 frames = 0;  // frames per channel
  bool loop = false;
  u32 loop_start_frame = 0;
  u32 loop_end_frame = 0;
};

// Rebuild a valid Ogg Vorbis stream from one FSB Vorbis sample (TriMO
// algorithm, reimplemented: id + comment + setup headers, then the
// u16-length-prefixed audio packets, one Ogg page per packet, granulepos
// from the blocksize of each packet).
bool fsb_vorbis_to_ogg(const fsb_sample& s, const u8* data, u32 data_len,
                       std::vector<u8>* ogg_out) {
  size_t setup_len = 0;
  const u8* setup = find_vorbis_setup(s.vorbis_crc, &setup_len);
  if (!setup) {
    logf("fmod-studio: no Vorbis setup packet for crc32 0x%08x "
         "(sample '%s') — decoder pending",
         s.vorbis_crc, s.name.c_str());
    return false;
  }
  // Setup header packet: the table entries are COMPLETE setup packets
  // (they already start with 0x05 "vorbis") — use them as-is. Do NOT
  // prepend another prefix (double-prefixing broke stb_vorbis, err=20;
  // found by diffing against TriMO's output 2026-11-20).
  if (setup_len < 7 || setup[0] != 0x05 || memcmp(setup + 1, "vorbis", 6) != 0) {
    logf("fmod-studio: Vorbis setup packet for crc32 0x%08x is malformed "
         "(missing 0x05 'vorbis' prefix) — decoder pending",
         s.vorbis_crc);
    return false;
  }

  u8 id_header[30];
  vorbis_id_header(id_header, (int)s.channels, s.frequency);
  ogg_out->clear();
  u32 seq = 0;
  ogg_page_append(ogg_out, &seq, id_header, 30, 0, 0x02);  // BOS
  ogg_page_append(ogg_out, &seq, k_vorbis_comment_header,
                  sizeof(k_vorbis_comment_header), 0, 0x00);
  ogg_page_append(ogg_out, &seq, setup, setup_len, 0, 0x00);
  // Audio packets: u16 length + packet bytes, terminated by u16 0.
  // Blocksize from the packet's second bit (LSB-first): 0 = short (256),
  // 1 = long (2048). Granulepos accumulates (blocksize+prev)/4.
  size_t pos = 0;
  i64 granulepos = 0;
  int prev_blocksize = 0;
  int packet_count = 0;
  while (pos + 2 <= data_len) {
    u16 plen = 0;
    memcpy(&plen, data + pos, 2);
    pos += 2;
    if (plen == 0) break;
    if (pos + plen > data_len) return false;
    const u8* pkt = data + pos;
    int blocksize = (pkt[0] & 0x02) ? 2048 : 256;
    // TriMO's rule: every packet (including the first, with prev=0)
    // contributes (blocksize+prev)/4 to the granulepos.
    granulepos += (blocksize + prev_blocksize) / 4;
    prev_blocksize = blocksize;
    bool last = (pos + plen + 2 > data_len) ||
                (pos + plen + 2 <= data_len &&
                 data[pos + plen] == 0 && data[pos + plen + 1] == 0);
    ogg_page_append(ogg_out, &seq, pkt, plen, granulepos,
                    last ? 0x04 : 0x00);
    pos += plen;
    ++packet_count;
  }
  return packet_count > 0;
}

// Decode one FSB sample to PCM. Returns false for unsupported codecs or
// decode failure (the caller logs the named reason).
bool decode_fsb_sample(bank_data* bank, u32 fsb_mode, const fsb_sample& s,
                       decoded_sample* out) {
  if (s.data_len == 0) return false;
#ifdef _WIN32
  // Console: the unhardened decode corrupted the heap here (AV in
  // get_decoded), which is why Celeste is silent on an Xbox. The envelope
  // checks below now run on every platform and reject the shapes that caused
  // it, but decode still stays off by default until a console run proves it:
  // `fsb_decode=1` (settings.ini or a per-game tiramisu.profile.ini, pushed
  // through the file API) or TIRAMISU_FSB_DECODE=1 enables it for one run.
  static const bool env_wants_decode = [] {
    const char* env = getenv("TIRAMISU_FSB_DECODE");
    return env && *env && *env != '0';
  }();
  if (!env_wants_decode && !g_fsb5_console_decode.load()) {
    static std::atomic<int> once{0};
    log_once(&once,
             "fmod-studio: FSB decode off on console (audio silent); set "
             "fsb_decode=1 in settings.ini or tiramisu.profile.ini to try "
             "the hardened path");
    return false;
  }
#endif
  // Guard against corrupt parsed offsets/sizes: a bogus data_len made
  // stb_vorbis (and the vector heap) corrupt on console. Fail visible.
  if (!bank || s.data_len > (1u << 28) ||
      s.file_offset > bank->file_size ||
      s.data_len > bank->file_size - s.file_offset) {
    logf("fmod-studio: sample '%s' has invalid range (off=%llu len=%u "
         "filesize=%llu)", s.name.c_str(),
         (unsigned long long)s.file_offset, (unsigned)s.data_len,
         (unsigned long long)bank->file_size);
    return false;
  }
  // The frame count comes straight from a bitfield in the bank; it is what
  // reserve() used to trust. Refuse the absurd ones by name.
  {
    const char* why = nullptr;
    if (!trt_fsb5_sample_sane(s.samples, s.channels ? s.channels : 1, &why)) {
      logf("fmod-studio: sample '%s' rejected: %s (frames=%u channels=%u)",
           s.name.c_str(), why ? why : "unspecified", s.samples, s.channels);
      return false;
    }
  }
  try {
    std::vector<u8> compressed(s.data_len);
    if (!file_read_at(bank->f, s.file_offset, compressed.data(), s.data_len))
      return false;
    if (s.vorbis_crc == 0 && fsb_mode != TRT_FSB5_VORBIS) {
      trt_decoded_pcm dec_pcm{};
      if (trt_fsb5_decode_payload(fsb_mode, compressed.data(), s.data_len,
                                  s.frequency ? s.frequency : 44100u,
                                  s.channels ? s.channels : 1u, s.samples,
                                  &dec_pcm) == 0 &&
          dec_pcm.samples && dec_pcm.frames > 0) {
        const size_t n_samps = static_cast<size_t>(dec_pcm.frames) * dec_pcm.channels;
        out->pcm.assign(dec_pcm.samples, dec_pcm.samples + n_samps);
        free(dec_pcm.samples);
        out->rate = dec_pcm.rate;
        out->channels = dec_pcm.channels;
        out->frames = dec_pcm.frames;
        out->loop = s.has_loop;
        out->loop_start_frame = s.loop_start;
        out->loop_end_frame = s.loop_end && s.loop_end <= out->frames
                                  ? s.loop_end
                                  : out->frames;
        return true;
      }
      logf("fmod-studio: sample '%s' codec mode=%u could not be decoded — "
           "silent",
           s.name.c_str(), fsb_mode);
      return false;
    }
    if (s.vorbis_crc == 0) {
      logf("fmod-studio: sample '%s' is missing VORBISDATA chunk — silent",
           s.name.c_str());
      return false;
    }
    std::vector<u8> ogg;
    if (!fsb_vorbis_to_ogg(s, compressed.data(), s.data_len, &ogg))
      return false;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err,
                                           nullptr);
    if (!v) {
      logf("fmod-studio: stb_vorbis failed to open rebuilt Ogg for '%s' "
           "(err=%d)",
           s.name.c_str(), err);
      return false;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    if (!info.channels || !info.sample_rate) {
      stb_vorbis_close(v);
      return false;
    }
    std::vector<short> pcm;
    pcm.reserve((size_t)s.samples * info.channels);
    short frame_buf[8192 * 2];
    int n;
    while ((n = stb_vorbis_get_frame_short_interleaved(
                v, info.channels, frame_buf, 8192 * 2)) > 0) {
      pcm.insert(pcm.end(), frame_buf, frame_buf + (size_t)n * info.channels);
    }
    stb_vorbis_close(v);
    if (pcm.empty()) {
      logf("fmod-studio: stb_vorbis decoded 0 frames for '%s'",
           s.name.c_str());
      return false;
    }
    out->pcm = std::move(pcm);
    out->rate = info.sample_rate;
    out->channels = (u32)info.channels;
    out->frames = (u32)(out->pcm.size() / out->channels);
    out->loop = s.has_loop;
    out->loop_start_frame = s.loop_start;
    out->loop_end_frame = s.loop_end && s.loop_end <= out->frames
                              ? s.loop_end
                              : out->frames;
    return true;
  } catch (const std::bad_alloc&) {
    // A decode that cannot fit the budget is a named skip, not a termination
    // (an uncaught bad_alloc took the whole app down on console).
    logf("fmod-studio: sample '%s' exceeds the decode memory budget "
         "(%u frames x %u ch) — silent",
         s.name.c_str(), s.samples, s.channels);
    return false;
  }
}

// ---- PCM cache (decoded samples shared across instances) ----
struct cache_key {
  bank_data* bank;
  int fsb_idx;
  int subsound;
  bool operator==(const cache_key& o) const {
    return bank == o.bank && fsb_idx == o.fsb_idx && subsound == o.subsound;
  }
};
struct cache_key_hash {
  size_t operator()(const cache_key& k) const {
    return (size_t)(uintptr_t)k.bank * 1315423911u ^
           (size_t)(k.fsb_idx * 2654435761u + k.subsound);
  }
};
std::mutex g_cache_mu;
std::unordered_map<cache_key, std::shared_ptr<decoded_sample>, cache_key_hash>
    g_pcm_cache;
std::deque<cache_key> g_cache_order;
size_t g_cache_bytes = 0;
const size_t k_pcm_cache_cap = 512ull * 1024ull * 1024ull;

std::shared_ptr<decoded_sample> get_decoded(bank_data* bank, int fsb_idx,
                                            int subsound) {
  cache_key key{bank, fsb_idx, subsound};
  {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    auto it = g_pcm_cache.find(key);
    if (it != g_pcm_cache.end()) return it->second;
  }
  if (fsb_idx < 0 || fsb_idx >= (int)bank->fsbs.size()) return nullptr;
  const fsb_bank& fb = bank->fsbs[(size_t)fsb_idx];
  if (subsound < 0 || subsound >= (int)fb.samples.size()) return nullptr;
  std::shared_ptr<decoded_sample> dec;
  try {
    dec = std::make_shared<decoded_sample>();
  } catch (const std::bad_alloc&) {
    logf("fmod-studio: no memory for a decode slot (bank '%s' sample %d/%d)",
         bank->path.c_str(), fsb_idx, subsound);
    return nullptr;
  }
  if (!decode_fsb_sample(bank, fb.mode, fb.samples[(size_t)subsound], dec.get()))
    return nullptr;
  {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    // Evict oldest entries while over the cap.
    size_t bytes = dec->pcm.size() * sizeof(short);
    while (!g_cache_order.empty() && g_cache_bytes + bytes > k_pcm_cache_cap) {
      cache_key old = g_cache_order.front();
      g_cache_order.pop_front();
      auto it = g_pcm_cache.find(old);
      if (it != g_pcm_cache.end()) {
        g_cache_bytes -= it->second->pcm.size() * sizeof(short);
        g_pcm_cache.erase(it);
      }
    }
    g_pcm_cache[key] = dec;
    g_cache_order.push_back(key);
    g_cache_bytes += bytes;
  }
  return dec;
}

// ---- engine objects ----
enum obj_type {
  OT_SYSTEM = 0x53595354,  // 'SYST'
  OT_BANK = 0x42414e4b,    // 'BANK'
  OT_EVENT = 0x45564e54,   // 'EVNT'
  OT_INSTANCE = 0x494e5354,  // 'INST'
  OT_BUS = 0x42555320,     // 'BUS '
  OT_VCA = 0x56434120,     // 'VCA '
  OT_PARAM = 0x5041524d,   // 'PARM'
};
struct obj_head {
  u32 magic;
  u32 type;
};
bool obj_check(void* h, u32 type) {
  if (!h) return false;
  obj_head* o = (obj_head*)h;
  return o->magic == 0xF00D1234u && o->type == type;
}

struct studio_system;
struct studio_event;
struct studio_instance;

struct studio_bank {
  obj_head head{0xF00D1234u, OT_BANK};
  studio_system* sys = nullptr;
  bank_data* data = nullptr;
  void* user_data = nullptr;
  // EventDescription objects created for this bank's events (by GUID).
  std::unordered_map<guid16, studio_event*, guid_hash> event_objs;
};

struct studio_event {
  obj_head head{0xF00D1234u, OT_EVENT};
  studio_system* sys = nullptr;
  studio_bank* bank = nullptr;  // the bank that owns the event node
  guid16 guid{};
  std::string path;
  void* user_data = nullptr;
  trt_fmod_event_callback callback = nullptr;
  int callback_mask = 0;
  std::vector<studio_instance*> instances;
};

struct studio_bus {
  obj_head head{0xF00D1234u, OT_BUS};
  studio_system* sys = nullptr;
  guid16 guid{};
  std::string path;
  float volume = 1.0f;  // runtime volume (bank default applied at creation)
  bool paused = false;
  bool mute = false;
};

struct studio_vca {
  obj_head head{0xF00D1234u, OT_VCA};
  studio_system* sys = nullptr;
  guid16 guid{};
  std::string path;
  float volume = 1.0f;
};

struct studio_param_inst {
  obj_head head{0xF00D1234u, OT_PARAM};
  studio_instance* inst = nullptr;
  guid16 param_guid{};
  std::string name;
};

// A playing voice: one decoded sample feeding the mixer in chunks.
struct studio_voice {
  u64 voice_id = 0;
  studio_instance* inst = nullptr;
  std::shared_ptr<decoded_sample> sample;
  u64 next_frame = 0;       // playhead for the next chunk
  u64 frames_submitted = 0;  // total frames ever submitted (audio clock)
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point pause_start;
  bool playing = true;
  bool paused = false;
  bool done = false;  // all PCM submitted (loop voices never finish)
  bool loop = false;
};

struct studio_instance {
  obj_head head{0xF00D1234u, OT_INSTANCE};
  studio_system* sys = nullptr;
  studio_event* event = nullptr;
  int state = TRT_STUDIO_PLAYBACK_STOPPED;
  bool paused = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  trt_fmod_3d_attrs attrs_3d{};
  bool has_3d_attrs = false;
  u32 listener_mask = 0xFFFFFFFFu;
  float properties[6] = {0, 0, 0, 1.0f, 20.0f, 0};  // min/max distance defaults
  float reverb_levels[4] = {0, 0, 0, 0};
  void* user_data = nullptr;
  trt_fmod_event_callback callback = nullptr;
  int callback_mask = 0;
  // parameter values by parameter GUID (defaults from the bank)
  std::unordered_map<guid16, float, guid_hash> param_values;
  // scheduled timeline items: (fire time ms, instrument guid, fired?)
  // Layout items (is_layout) fire when their parameter enters range
  // instead of at a time; they are checked at Start() and on every
  // SetParameterValue. v1 gap: a layout instrument is NOT stopped when
  // its parameter leaves range (it plays out); FMOD would stop it.
  struct sched_item {
    u32 start_ms;
    guid16 instrument;
    bool fired = false;
    bool is_layout = false;
    guid16 layout_param;
    float range_lo = 0;
    float range_hi = 0;
    bool include_end = false;
  };
  std::vector<sched_item> schedule;
  std::vector<studio_voice*> voices;
  std::vector<studio_param_inst*> param_insts;  // owned; freed on release
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point pause_start;
  bool start_time_set = false;
  // multi/scatterer selection state: instrument -> last playlist index
  std::unordered_map<guid16, int, guid_hash> playlist_cursor;
  std::unordered_map<guid16, int, guid_hash> playlist_once;
  int timeline_position_ms = 0;  // set by SetTimelinePosition (seek)
  bool callback_fired_stopped = false;
};

struct studio_system {
  obj_head head{0xF00D1234u, OT_SYSTEM};
  bool initialized = false;
  void* low_level = nullptr;  // core FMOD system (fmod_System_Create)
  std::vector<studio_bank*> banks;
  std::vector<studio_event*> events;  // all descriptions (owned by banks)
  std::vector<studio_bus*> buses;
  std::vector<studio_vca*> vcas;
  std::vector<studio_instance*> instances;
  int num_listeners = 1;
  trt_fmod_3d_attrs listeners[8]{};
  void* user_data = nullptr;
  // feeder thread (chunked playback)
  std::thread feeder_thread;
  std::atomic<int> feeder_run{0};
  std::mutex mu;  // guards banks/events/instances/voices
  std::atomic<u64> next_voice_id{1};
  std::atomic<u64> pcm_bytes_submitted{0};
  // xorshift RNG state (deterministic per process; playlist random picks)
  u64 rng_state = 0x853c49e6748fea9bull;
  u32 rng_next() {
    u64 x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (u32)(x >> 32);
  }
};

}  // namespace

// Memory instrumentation for the Celeste OOM investigation
// (oom-analysis.md): current PCM decode cache size in bytes. The Mono
// watchdog (patch 06) and bank_load (patch 07) log this. g_cache_bytes
// lives in the anonymous namespace above; this file-scope accessor is
// how the rest of the host reads it.
extern "C" u64 trt_fmod_studio_pcm_cache_bytes() {
  return (u64)g_cache_bytes;
}

namespace {

// ---- cross-bank lookups ----
bank_data* strings_bank_of(studio_system* sys) {
  for (auto* b : sys->banks)
    if (b->data && b->data->is_strings_bank) return b->data;
  return nullptr;
}

const bank_data::event_node* find_event_node(studio_system* sys,
                                             const guid16& g,
                                             bank_data** bank_out) {
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->events.find(g);
    if (it != b->data->events.end()) {
      if (bank_out) *bank_out = b->data;
      return &it->second;
    }
  }
  return nullptr;
}

const bank_data::instrument* find_instrument(studio_system* sys,
                                             const guid16& g) {
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->instruments.find(g);
    if (it != b->data->instruments.end()) return &it->second;
  }
  return nullptr;
}

bool resolve_wav(studio_system* sys, const guid16& wav_guid,
                 bank_data** bank_out, int* fsb_idx, int* subsound) {
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->wavs.find(wav_guid);
    if (it != b->data->wavs.end()) {
      *bank_out = b->data;
      *fsb_idx = it->second.fsb_index;
      *subsound = it->second.subsound;
      return true;
    }
  }
  return false;
}

const bank_data::param_node* find_param(studio_system* sys, const guid16& g) {
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->params.find(g);
    if (it != b->data->params.end()) return &it->second;
  }
  return nullptr;
}

// Resolve a path (event:/..., bus:/..., vca:/..., bank:/...) via the
// strings bank.
bool resolve_path(studio_system* sys, const std::string& path, guid16* out) {
  bank_data* sb = strings_bank_of(sys);
  if (!sb) return false;
  auto it = sb->path_to_guid.find(path);
  if (it == sb->path_to_guid.end()) return false;
  *out = it->second;
  return true;
}

std::string guid_path(studio_system* sys, const guid16& g) {
  bank_data* sb = strings_bank_of(sys);
  if (!sb) return std::string();
  return sb->guid_to_path(g);
}

// ---- event graph walk: collect every sample an event can play ----
// Used for GetLength / IsOneshot (all playlist entries, all layouts).
void collect_event_samples(studio_system* sys, const guid16& event_guid,
                           std::vector<std::tuple<bank_data*, int, int>>* out,
                           u32* timeline_extent_ms, bool* has_markers) {
  bank_data* ev_bank = nullptr;
  const auto* ev = find_event_node(sys, event_guid, &ev_bank);
  if (!ev) return;
  std::vector<guid16> stack;
  stack.push_back(ev->timeline);
  for (auto& lg : ev->layouts) stack.push_back(lg);
  std::unordered_map<guid16, bool, guid_hash> visited;
  if (timeline_extent_ms) *timeline_extent_ms = 0;
  if (has_markers) *has_markers = false;
  while (!stack.empty()) {
    guid16 g = stack.back();
    stack.pop_back();
    if (visited[g]) continue;
    visited[g] = true;
    // Timeline?
    for (auto* b : sys->banks) {
      if (!b->data) continue;
      auto tit = b->data->timelines.find(g);
      if (tit != b->data->timelines.end()) {
        for (auto& tb : tit->second) {
          stack.push_back(tb.instrument);
          if (timeline_extent_ms) {
            u32 end = tb.start_ms + tb.length_ms;
            if (end > *timeline_extent_ms) *timeline_extent_ms = end;
          }
        }
        auto mit = b->data->timeline_has_markers.find(g);
        if (has_markers && mit != b->data->timeline_has_markers.end() &&
            mit->second)
          *has_markers = true;
      }
      auto lit = b->data->layouts.find(g);
      if (lit != b->data->layouts.end()) {
        for (auto& lb : lit->second.boxes) stack.push_back(lb.instrument);
      }
      auto iit = b->data->instruments.find(g);
      if (iit != b->data->instruments.end()) {
        const auto& in = iit->second;
        if (!in.timeline.is_zero()) stack.push_back(in.timeline);
        if (in.kind == 0 && !in.wav.is_zero()) {
          bank_data* wb = nullptr;
          int fsb = 0, sub = 0;
          if (resolve_wav(sys, in.wav, &wb, &fsb, &sub))
            out->push_back({wb, fsb, sub});
        }
        for (auto& e : in.playlist) stack.push_back(e.instrument);
      }
    }
  }
}

int event_length_ms(studio_system* sys, const guid16& event_guid) {
  std::vector<std::tuple<bank_data*, int, int>> samples;
  u32 extent = 0;
  collect_event_samples(sys, event_guid, &samples, &extent, nullptr);
  u32 longest_ms = extent;
  for (auto& t : samples) {
    bank_data* b = std::get<0>(t);
    int fsb = std::get<1>(t), sub = std::get<2>(t);
    if (fsb < 0 || fsb >= (int)b->fsbs.size()) continue;
    const fsb_bank& fb = b->fsbs[(size_t)fsb];
    if (sub < 0 || sub >= (int)fb.samples.size()) continue;
    const fsb_sample& s = fb.samples[(size_t)sub];
    if (s.frequency) {
      u32 ms = (u32)((u64)s.samples * 1000ull / s.frequency);
      if (ms > longest_ms) longest_ms = ms;
    }
  }
  return (int)longest_ms;
}

bool event_is_oneshot(studio_system* sys, const guid16& event_guid) {
  std::vector<std::tuple<bank_data*, int, int>> samples;
  collect_event_samples(sys, event_guid, &samples, nullptr, nullptr);
  for (auto& t : samples) {
    bank_data* b = std::get<0>(t);
    int fsb = std::get<1>(t), sub = std::get<2>(t);
    if (fsb < 0 || fsb >= (int)b->fsbs.size()) continue;
    const fsb_bank& fb = b->fsbs[(size_t)fsb];
    if (sub < 0 || sub >= (int)fb.samples.size()) continue;
    if (fb.samples[(size_t)sub].has_loop) return false;
  }
  return true;
}

// ---- buses / VCAs: find-or-create runtime objects ----
studio_bus* find_or_create_bus(studio_system* sys, const guid16& g) {
  for (auto* bus : sys->buses)
    if (bus->guid == g) return bus;
  auto* bus = new studio_bus();
  bus->sys = sys;
  bus->guid = g;
  bus->path = guid_path(sys, g);
  // Bank default volume (dB -> linear); 1.0 when the bus has no node.
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->buses.find(g);
    if (it != b->data->buses.end()) {
      bus->volume = db_to_linear(it->second.volume_db);
      break;
    }
  }
  sys->buses.push_back(bus);
  return bus;
}

studio_vca* find_or_create_vca(studio_system* sys, const guid16& g) {
  for (auto* vca : sys->vcas)
    if (vca->guid == g) return vca;
  auto* vca = new studio_vca();
  vca->sys = sys;
  vca->guid = g;
  vca->path = guid_path(sys, g);
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto it = b->data->vcas.find(g);
    if (it != b->data->vcas.end()) {
      vca->volume = db_to_linear(it->second.volume_db);
      break;
    }
  }
  sys->vcas.push_back(vca);
  return vca;
}

// Ancestor bus paths for a bus path (bus:/a/b/c -> bus:/a/b, bus:/a,
// bus:/). The bus hierarchy IS the path hierarchy.
std::vector<std::string> bus_ancestors(const std::string& path) {
  std::vector<std::string> out;
  std::string p = path;
  while (p.size() > 5) {  // longer than "bus:/"
    size_t slash = p.rfind('/');
    if (slash == std::string::npos || slash < 5) break;
    p = p.substr(0, slash);
    out.push_back(p);
  }
  out.push_back("bus:/");
  return out;
}

// Product of the bus volumes along the event's bus chain (input bus +
// ancestors), times the VCA volumes claiming any bus in the chain.
float bus_chain_volume(studio_system* sys, const guid16& input_bus) {
  float v = 1.0f;
  std::string bus_path = guid_path(sys, input_bus);
  std::vector<guid16> chain;
  chain.push_back(input_bus);
  if (!bus_path.empty()) {
    for (auto& anc : bus_ancestors(bus_path)) {
      guid16 g;
      if (resolve_path(sys, anc, &g)) chain.push_back(g);
    }
  }
  for (auto& g : chain) {
    studio_bus* bus = find_or_create_bus(sys, g);
    if (bus->mute || bus->paused) return 0.0f;
    v *= bus->volume;
    // VCAs claiming this bus (VCAB Strips hold bus GUIDs).
    for (auto* b : sys->banks) {
      if (!b->data) continue;
      for (auto& kv : b->data->vcas) {
        for (auto& strip : kv.second.strips) {
          if (strip == g) {
            studio_vca* vca = find_or_create_vca(sys, kv.first);
            v *= vca->volume;
          }
        }
      }
    }
  }
  return v;
}

float attenuation_3d(studio_instance* inst, studio_system* sys) {
  if (!inst->has_3d_attrs) return 1.0f;
  const trt_fmod_3d_attrs& l = sys->listeners[0];
  float dx = inst->attrs_3d.position[0] - l.position[0];
  float dy = inst->attrs_3d.position[1] - l.position[1];
  float dz = inst->attrs_3d.position[2] - l.position[2];
  float dist = sqrtf(dx * dx + dy * dy + dz * dz);
  float min_d = inst->properties[3] > 0 ? inst->properties[3] : 1.0f;
  float max_d = inst->properties[4] > min_d ? inst->properties[4] : 20.0f;
  if (dist <= min_d) return 1.0f;
  float att = min_d / (dist < max_d ? dist : max_d);
  return att;
}

float effective_volume(studio_instance* inst) {
  studio_system* sys = inst->sys;
  float v = inst->volume;
  bank_data* ev_bank = nullptr;
  const auto* ev = find_event_node(sys, inst->event->guid, &ev_bank);
  if (ev && !ev->input_bus.is_zero()) v *= bus_chain_volume(sys, ev->input_bus);
  v *= attenuation_3d(inst, sys);
  return v;
}

// ---- callbacks ----
void invoke_callback(studio_instance* inst, int type) {
  if (inst->callback && (inst->callback_mask & type))
    inst->callback(type, inst, nullptr);
  studio_event* ev = inst->event;
  if (ev && ev->callback && (ev->callback_mask & type))
    ev->callback(type, inst, nullptr);
}

// ---- playlist selection ----
int select_playlist_entry(studio_system* sys, studio_instance* inst,
                          const guid16& instrument_guid,
                          const bank_data::instrument& in) {
  if (in.playlist.empty()) return -1;
  if (in.selection_mode == 0) {  // SelectOnce: pick once per instance
    auto it = inst->playlist_once.find(instrument_guid);
    if (it != inst->playlist_once.end()) return it->second;
  }
  int idx = 0;
  switch (in.play_mode) {
    case 1:  // Random (weighted)
    case 2: {  // SmartRandom (weighted, excluding the last pick)
      float total = 0;
      int last = -1;
      auto cit = inst->playlist_cursor.find(instrument_guid);
      if (cit != inst->playlist_cursor.end()) last = cit->second;
      for (size_t i = 0; i < in.playlist.size(); ++i) {
        if (in.play_mode == 2 && (int)i == last) continue;
        total += in.playlist[i].weight > 0 ? in.playlist[i].weight : 1.0f;
      }
      if (total <= 0) {
        idx = (int)(sys->rng_next() % in.playlist.size());
        break;
      }
      float pick = (float)(sys->rng_next() % 1000000) / 1000000.0f * total;
      float acc = 0;
      idx = (int)in.playlist.size() - 1;
      for (size_t i = 0; i < in.playlist.size(); ++i) {
        if (in.play_mode == 2 && (int)i == last) continue;
        acc += in.playlist[i].weight > 0 ? in.playlist[i].weight : 1.0f;
        if (pick <= acc) {
          idx = (int)i;
          break;
        }
      }
      break;
    }
    default: {  // Sequential / GlobalSequential / InstanceSequential
      int cur = 0;
      auto cit = inst->playlist_cursor.find(instrument_guid);
      if (cit != inst->playlist_cursor.end()) cur = cit->second + 1;
      idx = cur % (int)in.playlist.size();
      break;
    }
  }
  inst->playlist_cursor[instrument_guid] = idx;
  if (in.selection_mode == 0) inst->playlist_once[instrument_guid] = idx;
  return idx;
}

// Fire one instrument on an instance: resolve it to a sample and create a
// voice. Multi/scatterer instruments select one playlist entry and fire
// it; nested instruments (INST timeline) are scheduled, not fired here.
void fire_instrument(studio_system* sys, studio_instance* inst,
                     const guid16& instrument_guid, int depth) {
  if (depth > 16) return;  // cycle guard
  const auto* in = find_instrument(sys, instrument_guid);
  if (!in) return;
  if (in->kind == 0) {  // waveform: decode + voice
    bank_data* wb = nullptr;
    int fsb = 0, sub = 0;
    if (!resolve_wav(sys, in->wav, &wb, &fsb, &sub)) return;
    auto dec = get_decoded(wb, fsb, sub);
    if (!dec) {
      static std::atomic<int> once{0};
      log_once(&once,
               "fmod-studio: a sample failed to decode; its event plays "
               "silently (see earlier decoder-pending lines)");
      return;
    }
    auto* voice = new studio_voice();
    voice->voice_id = sys->next_voice_id.fetch_add(1);
    voice->inst = inst;
    voice->sample = dec;
    voice->start_time = std::chrono::steady_clock::now();
    voice->loop = dec->loop;
    inst->voices.push_back(voice);
    return;
  }
  if (in->kind == 1 || in->kind == 2) {  // multi / scatterer
    int idx = select_playlist_entry(sys, inst, instrument_guid, *in);
    if (idx < 0) return;
    fire_instrument(sys, inst, in->playlist[(size_t)idx].instrument,
                    depth + 1);
    return;
  }
  // Other instrument kinds (parameter/trigger/event/command): no audio of
  // their own. Event instruments reference nested events — follow the
  // nested timeline if the instrument has one.
  if (!in->timeline.is_zero()) {
    for (auto* b : sys->banks) {
      if (!b->data) continue;
      auto tit = b->data->timelines.find(in->timeline);
      if (tit != b->data->timelines.end()) {
        for (auto& tb : tit->second) {
          studio_instance::sched_item si;
          si.start_ms = tb.start_ms;
          si.instrument = tb.instrument;
          si.fired = false;
          inst->schedule.push_back(si);
        }
      }
    }
  }
}

// ---- feeder: chunked PCM submission (the host mixer caps each voice's
// FIFO at 2s, so long samples stream in 0.5s chunks) ----
void feeder_submit_chunk(studio_system* sys, studio_voice* voice) {
  decoded_sample* s = voice->sample.get();
  if (!s || s->frames == 0 || s->rate == 0) {
    voice->done = true;
    return;
  }
  const u64 chunk_frames = s->rate / 2;  // 0.5s chunks
  u64 remaining = s->frames - voice->next_frame;
  if (voice->loop && s->loop_end_frame > 0 &&
      s->loop_end_frame < s->frames)
    remaining = s->loop_end_frame - voice->next_frame;
  u64 take = remaining < chunk_frames ? remaining : chunk_frames;
  if (take == 0) {
    if (voice->loop) {
      voice->next_frame = voice->sample->loop_start_frame;
      return;
    }
    voice->done = true;
    return;
  }
  const short* pcm_ptr = s->pcm.data() + voice->next_frame * s->channels;
  u32 bytes = (u32)(take * s->channels * sizeof(short));
  float vol = effective_volume(voice->inst);
  // Pitch: resampling is the mixer's job (it resamples by rate); a pitch
  // change is a rate change. v1 applies pitch as a rate scale.
  u32 rate = s->rate;
  if (voice->inst->pitch != 1.0f && voice->inst->pitch > 0.01f &&
      voice->inst->pitch < 100.0f)
    rate = (u32)(s->rate * voice->inst->pitch);
  trt_audio_submit_voice(voice->voice_id, pcm_ptr, bytes, rate, s->channels,
                         2, 0, vol);
  sys->pcm_bytes_submitted.fetch_add(bytes);
  voice->next_frame += take;
  voice->frames_submitted += take;
  if (voice->next_frame >= s->frames ||
      (voice->loop && s->loop_end_frame > 0 &&
       voice->next_frame >= s->loop_end_frame)) {
    if (voice->loop) {
      voice->next_frame = voice->sample->loop_start_frame;
    } else {
      voice->done = true;
    }
  }
}

void update_instance(studio_system* sys, studio_instance* inst);

void feeder_fn(studio_system* sys) {
  using namespace std::chrono;
  while (sys->feeder_run.load()) {
    bool active = false;
    {
      std::lock_guard<std::mutex> lk(sys->mu);
      auto now = steady_clock::now();
      for (auto* inst : sys->instances) {
        if (inst->state != TRT_STUDIO_PLAYBACK_PLAYING || inst->paused)
          continue;
        update_instance(sys, inst);
        for (auto* voice : inst->voices) {
          if (!voice->playing || voice->paused || voice->done) continue;
          decoded_sample* s = voice->sample.get();
          if (!s || s->rate == 0) continue;
          double elapsed =
              duration<double>(now - voice->start_time).count();
          double submitted = (double)voice->frames_submitted / s->rate;
          double buffered = submitted - elapsed;
          if (buffered < 1.0) {
            feeder_submit_chunk(sys, voice);
            active = true;
          }
        }
      }
    }
    std::this_thread::sleep_for(milliseconds(active ? 50 : 100));
  }
}

void feeder_ensure(studio_system* sys) {
  if (sys->feeder_run.load()) return;
  sys->feeder_run.store(1);
  try {
    sys->feeder_thread = std::thread(feeder_fn, sys);
  } catch (...) {
    sys->feeder_run.store(0);
  }
}

void feeder_stop(studio_system* sys) {
  if (!sys->feeder_run.load()) return;
  sys->feeder_run.store(0);
  if (sys->feeder_thread.joinable()) sys->feeder_thread.join();
}

// Elapsed ms for an instance (pause-aware).
int instance_elapsed_ms(studio_instance* inst) {
  if (!inst->start_time_set) return 0;
  auto now = std::chrono::steady_clock::now();
  auto end = inst->paused ? inst->pause_start : now;
  int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
               end - inst->start_time)
               .count();
  return ms < 0 ? 0 : ms;
}

// Update one playing instance: fire due trigger boxes, detect finish.
void update_instance(studio_system* sys, studio_instance* inst) {
  if (inst->state != TRT_STUDIO_PLAYBACK_PLAYING) return;
  int elapsed = instance_elapsed_ms(inst);
  // Fire due timeline items (layout items fire on parameter changes).
  for (auto& si : inst->schedule) {
    if (!si.fired && !si.is_layout && (int)si.start_ms <= elapsed) {
      si.fired = true;
      fire_instrument(sys, inst, si.instrument, 0);
    }
  }
  // Snapshot events sustain until stopped (they are mixer state).
  bank_data* ev_bank = nullptr;
  const auto* ev = find_event_node(sys, inst->event->guid, &ev_bank);
  bool is_snapshot = ev && !ev->snapshot.is_zero();
  if (is_snapshot) return;
  // Finish: every TIME schedule item fired, every voice done, and the
  // audio clock has run past the last submitted frame. Layout items
  // that never entered range do not block finishing. Looping voices
  // never finish on their own.
  bool all_fired = true;
  for (auto& si : inst->schedule)
    if (!si.fired && !si.is_layout) all_fired = false;
  if (!all_fired) return;
  bool any_loop = false;
  bool all_done = true;
  double voice_end_s = 0;
  for (auto* voice : inst->voices) {
    if (voice->loop && voice->playing) any_loop = true;
    if (!voice->done) all_done = false;
    decoded_sample* s = voice->sample.get();
    if (s && s->rate) {
      double end = (double)voice->frames_submitted / s->rate;
      if (end > voice_end_s) voice_end_s = end;
    }
  }
  if (any_loop) return;
  if (inst->voices.empty()) {
    // No audio (decode failure / empty event): run out the event length.
    int len = event_length_ms(sys, inst->event->guid);
    if (elapsed < len) return;
  } else {
    if (!all_done) return;
    if (elapsed / 1000.0 < voice_end_s) return;
  }
  inst->state = TRT_STUDIO_PLAYBACK_STOPPED;
  if (!inst->callback_fired_stopped) {
    inst->callback_fired_stopped = true;
    invoke_callback(inst, 0x20);  // STOPPED
  }
}

// Fire layout instruments whose parameter is currently in range.
void check_layout_items(studio_system* sys, studio_instance* inst) {
  for (auto& si : inst->schedule) {
    if (!si.is_layout || si.fired) continue;
    float value = 0;
    auto vit = inst->param_values.find(si.layout_param);
    if (vit != inst->param_values.end()) {
      value = vit->second;
    } else {
      const auto* pn = find_param(sys, si.layout_param);
      value = pn ? pn->def_v : 0;
    }
    bool in_range =
        value >= si.range_lo &&
        (si.include_end ? value <= si.range_hi : value < si.range_hi);
    if (in_range) {
      si.fired = true;
      fire_instrument(sys, inst, si.instrument, 0);
    }
  }
}

}  // namespace

extern "C" {

// ---- helpers ----
namespace {
studio_system* check_sys(void* h) {
  return obj_check(h, OT_SYSTEM) ? (studio_system*)h : nullptr;
}
studio_bank* check_bank(void* h) {
  return obj_check(h, OT_BANK) ? (studio_bank*)h : nullptr;
}
studio_event* check_event(void* h) {
  return obj_check(h, OT_EVENT) ? (studio_event*)h : nullptr;
}
studio_instance* check_inst(void* h) {
  return obj_check(h, OT_INSTANCE) ? (studio_instance*)h : nullptr;
}
studio_bus* check_bus(void* h) {
  return obj_check(h, OT_BUS) ? (studio_bus*)h : nullptr;
}
studio_vca* check_vca(void* h) {
  return obj_check(h, OT_VCA) ? (studio_vca*)h : nullptr;
}
studio_param_inst* check_param(void* h) {
  return obj_check(h, OT_PARAM) ? (studio_param_inst*)h : nullptr;
}

// Copy a path into the caller's buffer (FMOD getPath semantics):
// writes up to size-1 chars + NUL, sets *retrieved to the full length
// INCLUDING the NUL (the stub's critical fix: retrieved must be > 0).
int copy_path(const std::string& path, char* out, int size, int* retrieved) {
  if (!out || size <= 0 || !retrieved) return TRT_FMOD_ERR_INVALID_PARAM;
  int need = (int)path.size() + 1;
  int copy = need < size ? need : size;
  if (copy > 0) {
    memcpy(out, path.c_str(), (size_t)(copy - 1));
    out[copy - 1] = '\0';
  }
  *retrieved = need;
  return TRT_FMOD_OK;
}

studio_event* find_or_create_event(studio_system* sys, const guid16& g) {
  bank_data* ev_bank = nullptr;
  if (!find_event_node(sys, g, &ev_bank)) return nullptr;
  studio_bank* owner = nullptr;
  for (auto* b : sys->banks)
    if (b->data == ev_bank) owner = b;
  if (!owner) return nullptr;
  auto it = owner->event_objs.find(g);
  if (it != owner->event_objs.end()) return it->second;
  auto* ev = new studio_event();
  ev->sys = sys;
  ev->bank = owner;
  ev->guid = g;
  ev->path = guid_path(sys, g);
  owner->event_objs[g] = ev;
  sys->events.push_back(ev);
  return ev;
}

void free_instance_locked(studio_instance* inst) {
  studio_system* sys = inst->sys;
  for (auto* v : inst->voices) {
    v->playing = false;
    v->done = true;
    delete v;
  }
  inst->voices.clear();
  for (auto* p : inst->param_insts) {
    p->head.magic = 0;
    delete p;
  }
  inst->param_insts.clear();
  if (inst->event) {
    auto& vec = inst->event->instances;
    vec.erase(std::remove(vec.begin(), vec.end(), inst), vec.end());
  }
  auto& svec = sys->instances;
  svec.erase(std::remove(svec.begin(), svec.end(), inst), svec.end());
  inst->head.magic = 0;
  delete inst;
}

// Global registry of live Studio systems, for handle-free test
// introspection: the harness test cannot know the game's system handle
// (it lives inside managed code), so the global event count sums across
// every live system. Registered in system_create, removed in
// system_release (before the banks are freed).
std::mutex g_systems_mu;
std::vector<studio_system*> g_systems;

void registry_add(studio_system* sys) {
  std::lock_guard<std::mutex> lk(g_systems_mu);
  g_systems.push_back(sys);
}
void registry_remove(studio_system* sys) {
  std::lock_guard<std::mutex> lk(g_systems_mu);
  g_systems.erase(std::remove(g_systems.begin(), g_systems.end(), sys),
                  g_systems.end());
}
int registry_event_count() {
  std::lock_guard<std::mutex> lk(g_systems_mu);
  int total = 0;
  for (auto* sys : g_systems) {
    std::lock_guard<std::mutex> lk2(sys->mu);
    for (auto* b : sys->banks)
      if (b->data) total += (int)b->data->events.size();
  }
  return total;
}
}  // namespace

// ---- system ----
int trt_fmod_studio_system_create(void** out, u32 header_version) {
  (void)header_version;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  auto* sys = new studio_system();
  void* ll = nullptr;
  if (fmod_System_Create(nullptr, &ll) == 0) sys->low_level = ll;
  *out = sys;
  registry_add(sys);
  log_line("fmod-studio: System_Create -> real Studio engine (bank "
           "parsing + FSB5/Vorbis decode + host mixer)");
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_init(void* h, int maxchannels, u32 studio_flags,
                                u32 core_flags, void* extra) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  (void)studio_flags;
  if (sys->low_level)
    fmod_System_Init(sys->low_level, (u32)(maxchannels > 0 ? maxchannels : 64),
                     core_flags, extra);
  sys->initialized = true;
  feeder_ensure(sys);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_is_valid(void* h) {
  return obj_check(h, OT_SYSTEM) ? 1 : 0;
}

int trt_fmod_studio_system_release(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  feeder_stop(sys);
  // Remove from the global registry BEFORE taking sys->mu (lock order:
  // g_systems_mu -> sys->mu, as in registry_event_count).
  registry_remove(sys);
  {
    std::lock_guard<std::mutex> lk(sys->mu);
    while (!sys->instances.empty()) free_instance_locked(sys->instances[0]);
    for (auto* b : sys->banks) {
      for (auto& kv : b->event_objs) {
        kv.second->head.magic = 0;
        delete kv.second;
      }
      bank_free(b->data);
      b->head.magic = 0;
      delete b;
    }
    sys->banks.clear();
    for (auto* bus : sys->buses) {
      bus->head.magic = 0;
      delete bus;
    }
    for (auto* vca : sys->vcas) {
      vca->head.magic = 0;
      delete vca;
    }
  }
  if (sys->low_level) {
    fmod_System_Close(sys->low_level);
    fmod_System_Release(sys->low_level);
  }
  sys->head.magic = 0;
  delete sys;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_update(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(sys->mu);
  for (auto* inst : sys->instances) update_instance(sys, inst);
  if (sys->low_level) fmod_System_Update(sys->low_level);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_get_low_level(void* h, void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  if (!sys->low_level) return TRT_FMOD_ERR_STUDIO_UNINITIALIZED;
  *out = sys->low_level;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_get_num_listeners(void* h, int* out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = sys->num_listeners;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_set_num_listeners(void* h, int count) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (count < 1 || count > 8) return TRT_FMOD_ERR_INVALID_PARAM;
  sys->num_listeners = count;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_get_listener_attrs(void* h, int index,
                                              trt_fmod_3d_attrs* out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out || index < 0 || index >= sys->num_listeners)
    return TRT_FMOD_ERR_INVALID_PARAM;
  *out = sys->listeners[index];
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_set_listener_attrs(void* h, int index,
                                              const trt_fmod_3d_attrs* attrs) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!attrs || index < 0 || index >= 8) return TRT_FMOD_ERR_INVALID_PARAM;
  sys->listeners[index] = *attrs;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_unload_all(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(sys->mu);
  while (!sys->instances.empty()) free_instance_locked(sys->instances[0]);
  for (auto* b : sys->banks) {
    for (auto& kv : b->event_objs) {
      kv.second->head.magic = 0;
      delete kv.second;
    }
    bank_free(b->data);
    b->head.magic = 0;
    delete b;
  }
  sys->banks.clear();
  sys->events.clear();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_flush_commands(void* h) {
  if (!check_sys(h)) return TRT_FMOD_ERR_INVALID_HANDLE;
  return TRT_FMOD_OK;  // commands execute synchronously; nothing to flush
}

int trt_fmod_studio_system_flush_sample_loading(void* h) {
  if (!check_sys(h)) return TRT_FMOD_ERR_INVALID_HANDLE;
  return TRT_FMOD_OK;  // sample loading is synchronous (demand decode)
}

int trt_fmod_studio_system_get_bank_count(void* h, int* out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = (int)sys->banks.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_get_bank_list(void* h, void** out, int capacity,
                                         int* count) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!count) return TRT_FMOD_ERR_INVALID_PARAM;
  int n = (int)sys->banks.size();
  int fill = n < capacity ? n : capacity;
  if (out) {
    for (int i = 0; i < fill; ++i) out[i] = sys->banks[(size_t)i];
  }
  *count = fill;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_get_user_data(void* h, void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = sys->user_data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_system_set_user_data(void* h, void* data) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  sys->user_data = data;
  return TRT_FMOD_OK;
}

// ---- banks ----
int trt_fmod_studio_load_bank_file(void* h, const char* path, u32 flags,
                                   void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!path || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  (void)flags;  // NONBLOCKING/DECOMPRESS_SAMPLES: loading is synchronous
  std::lock_guard<std::mutex> lk(sys->mu);
  for (auto* b : sys->banks) {
    if (b->data && b->data->path == path) {
      *out = b;
      return TRT_FMOD_ERR_EVENT_ALREADY_LOADED;
    }
  }
  bank_data* data = bank_load(path);
  if (!data) return TRT_FMOD_ERR_FILE_NOTFOUND;
  auto* bank = new studio_bank();
  bank->sys = sys;
  bank->data = data;
  sys->banks.push_back(bank);
  *out = bank;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_is_valid(void* h) {
  return obj_check(h, OT_BANK) ? 1 : 0;
}

int trt_fmod_studio_bank_unload(void* h) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  studio_system* sys = bank->sys;
  std::lock_guard<std::mutex> lk(sys->mu);
  // Stop + free instances of this bank's events first.
  std::vector<studio_instance*> doomed;
  for (auto* inst : sys->instances)
    if (inst->event && inst->event->bank == bank) doomed.push_back(inst);
  for (auto* inst : doomed) free_instance_locked(inst);
  for (auto& kv : bank->event_objs) {
    kv.second->head.magic = 0;
    auto& evec = sys->events;
    evec.erase(std::remove(evec.begin(), evec.end(), kv.second), evec.end());
    delete kv.second;
  }
  bank_free(bank->data);
  auto& bvec = sys->banks;
  bvec.erase(std::remove(bvec.begin(), bvec.end(), bank), bvec.end());
  bank->head.magic = 0;
  delete bank;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_load_sample_data(void* h) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  // Sample data is demand-decoded from disk (decoding a whole bank
  // eagerly would take minutes + GBs for music.bank). The data IS
  // available, so LOADED is the honest state; see the header comment.
  bank->data->sample_loading_state = TRT_STUDIO_LOADING_LOADED;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_unload_sample_data(void* h) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  // Evict this bank's decoded samples from the PCM cache.
  std::lock_guard<std::mutex> lk(g_cache_mu);
  for (auto it = g_pcm_cache.begin(); it != g_pcm_cache.end();) {
    if (it->first.bank == bank->data) {
      g_cache_bytes -= it->second->pcm.size() * sizeof(short);
      it = g_pcm_cache.erase(it);
    } else {
      ++it;
    }
  }
  g_cache_order.erase(
      std::remove_if(g_cache_order.begin(), g_cache_order.end(),
                     [&](const cache_key& k) { return k.bank == bank->data; }),
      g_cache_order.end());
  bank->data->sample_loading_state = TRT_STUDIO_LOADING_UNLOADED;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_loading_state(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = bank->data->loading_state;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_sample_loading_state(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = bank->data->sample_loading_state;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_id(void* h, u8 out_guid[16]) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_guid) return TRT_FMOD_ERR_INVALID_PARAM;
  memcpy(out_guid, bank->data->bank_guid.b, 16);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_path(void* h, char* out, int size,
                                  int* retrieved) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::string path = guid_path(bank->sys, bank->data->bank_guid);
  if (path.empty()) {
    // No strings bank: build bank:/<name> from the file name.
    std::string base = bank->data->path;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    path = "bank:/" + base;
  }
  return copy_path(path, out, size, retrieved);
}

int trt_fmod_studio_bank_get_event_count(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  // Real count: parsed event definitions. (The BNKI header's
  // TopLevelEventCount is smaller — it excludes nested sub-events — but
  // GetEventList must agree with GetEventCount, so both use the parsed
  // count. sfx.bank: 587 parsed vs 527 top-level.)
  *out = (int)bank->data->events.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_event_list(void* h, void** out, int capacity,
                                        int* count) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!count) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(bank->sys->mu);
  std::vector<guid16> guids;
  guids.reserve(bank->data->events.size());
  for (auto& kv : bank->data->events) guids.push_back(kv.first);
  std::sort(guids.begin(), guids.end(), guid_less);
  int fill = (int)guids.size() < capacity ? (int)guids.size() : capacity;
  if (out) {
    for (int i = 0; i < fill; ++i)
      out[i] = find_or_create_event(bank->sys, guids[(size_t)i]);
  }
  *count = fill;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_bus_count(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = (int)bank->data->buses.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_bus_list(void* h, void** out, int capacity,
                                      int* count) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!count) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(bank->sys->mu);
  std::vector<guid16> guids;
  for (auto& kv : bank->data->buses) guids.push_back(kv.first);
  std::sort(guids.begin(), guids.end(), guid_less);
  int fill = (int)guids.size() < capacity ? (int)guids.size() : capacity;
  if (out) {
    for (int i = 0; i < fill; ++i)
      out[i] = find_or_create_bus(bank->sys, guids[(size_t)i]);
  }
  *count = fill;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_vca_count(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = (int)bank->data->vcas.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_vca_list(void* h, void** out, int capacity,
                                      int* count) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!count) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(bank->sys->mu);
  std::vector<guid16> guids;
  for (auto& kv : bank->data->vcas) guids.push_back(kv.first);
  std::sort(guids.begin(), guids.end(), guid_less);
  int fill = (int)guids.size() < capacity ? (int)guids.size() : capacity;
  if (out) {
    for (int i = 0; i < fill; ++i)
      out[i] = find_or_create_vca(bank->sys, guids[(size_t)i]);
  }
  *count = fill;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_string_count(void* h, int* out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = (int)bank->data->string_guids.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_get_string_info(void* h, int index,
                                         u8 out_guid[16], char* path,
                                         int path_size, int* retrieved) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_guid || !path || !retrieved) return TRT_FMOD_ERR_INVALID_PARAM;
  if (index < 0 || index >= (int)bank->data->string_guids.size())
    return TRT_FMOD_ERR_INVALID_PARAM;
  guid16 g = bank->data->string_guids[(size_t)index];
  memcpy(out_guid, g.b, 16);
  return copy_path(bank->data->guid_to_path(g), path, path_size, retrieved);
}

int trt_fmod_studio_bank_get_user_data(void* h, void** out) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = bank->user_data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bank_set_user_data(void* h, void* data) {
  studio_bank* bank = check_bank(h);
  if (!bank) return TRT_FMOD_ERR_INVALID_HANDLE;
  bank->user_data = data;
  return TRT_FMOD_OK;
}

// ---- events (lookup + description) ----
int trt_fmod_studio_get_event(void* h, const char* path, void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!path || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  guid16 g;
  if (!resolve_path(sys, path, &g)) {
    *out = nullptr;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  studio_event* ev = find_or_create_event(sys, g);
  if (!ev) {
    *out = nullptr;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  *out = ev;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_get_event_by_id(void* h, const u8 guid[16], void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!guid || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  studio_event* ev = find_or_create_event(sys, guid_from(guid));
  if (!ev) {
    *out = nullptr;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  *out = ev;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_id(void* h, u8 out_guid[16]) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_guid) return TRT_FMOD_ERR_INVALID_PARAM;
  memcpy(out_guid, ev->guid.b, 16);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_path(void* h, char* out, int size,
                                   int* retrieved) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (ev->path.empty()) {
    if (retrieved) *retrieved = 0;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  return copy_path(ev->path, out, size, retrieved);
}

int trt_fmod_studio_event_get_length(void* h, int* out_ms) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_ms) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  *out_ms = event_length_ms(ev->sys, ev->guid);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_is_3d(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  static std::atomic<int> once{0};
  log_once(&once,
           "fmod-studio: Is3D detection not implemented (spatializer data "
           "lives in the effect chain, not parsed yet) — events report 2D; "
           "positional audio plays centered");
  *out = 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_is_snapshot(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  *out = (node && !node->snapshot.is_zero()) ? 1 : 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_is_oneshot(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  *out = event_is_oneshot(ev->sys, ev->guid) ? 1 : 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_is_stream(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = 0;  // all samples are memory-resident after decode, never streamed
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_has_cue(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  *out = 0;
  if (node) {
    for (auto* bank : ev->sys->banks) {
      if (!bank->data) continue;
      auto it = bank->data->timeline_has_markers.find(node->timeline);
      if (it != bank->data->timeline_has_markers.end() && it->second) {
        *out = 1;
        break;
      }
    }
  }
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_min_distance(void* h, float* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = 1.0f;  // FMOD default; v0x67 banks do not store min/max distance
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_max_distance(void* h, float* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = 20.0f;  // FMOD default; v0x67 banks do not store min/max distance
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_sound_size(void* h, float* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = 0.0f;  // point source default
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_parameter_count(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  *out = node ? (int)node->layouts.size() : 0;
  return TRT_FMOD_OK;
}

namespace {
int fill_param_desc(studio_system* sys, const guid16& layout_guid,
                    trt_fmod_param_desc* out) {
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    auto lit = b->data->layouts.find(layout_guid);
    if (lit == b->data->layouts.end()) continue;
    const auto* pn = find_param(sys, lit->second.param);
    if (!pn) return TRT_FMOD_ERR_EVENT_NOTFOUND;
    out->name = pn->name.c_str();  // owned by the bank; valid while loaded
    memcpy(&out->id_data1, lit->second.param.b, 4);
    memcpy(&out->id_data2, lit->second.param.b + 4, 4);
    out->minimum = pn->min_v;
    out->maximum = pn->max_v;
    out->default_value = pn->def_v;
    out->type = pn->type;
    out->flags = pn->type == 0 ? 0 : 2;  // AUTOMATIC for non-game params
    return TRT_FMOD_OK;
  }
  return TRT_FMOD_ERR_EVENT_NOTFOUND;
}
}  // namespace

int trt_fmod_studio_event_get_parameter_by_index(void* h, int index,
                                                 trt_fmod_param_desc* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  if (!node || index < 0 || index >= (int)node->layouts.size())
    return TRT_FMOD_ERR_INVALID_PARAM;
  return fill_param_desc(ev->sys, node->layouts[(size_t)index], out);
}

int trt_fmod_studio_event_get_parameter(void* h, const char* name,
                                        trt_fmod_param_desc* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!name || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  if (!node) return TRT_FMOD_ERR_EVENT_NOTFOUND;
  for (auto& lg : node->layouts) {
    for (auto* bank : ev->sys->banks) {
      if (!bank->data) continue;
      auto lit = bank->data->layouts.find(lg);
      if (lit == bank->data->layouts.end()) continue;
      const auto* pn = find_param(ev->sys, lit->second.param);
      if (pn && pn->name == name)
        return fill_param_desc(ev->sys, lg, out);
    }
  }
  return TRT_FMOD_ERR_EVENT_NOTFOUND;
}

int trt_fmod_studio_event_create_instance(void* h, void** out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  auto* inst = new studio_instance();
  inst->sys = ev->sys;
  inst->event = ev;
  // Seed parameter values from the bank defaults.
  bank_data* b = nullptr;
  const auto* node = find_event_node(ev->sys, ev->guid, &b);
  if (node) {
    for (auto& lg : node->layouts) {
      for (auto* bank : ev->sys->banks) {
        if (!bank->data) continue;
        auto lit = bank->data->layouts.find(lg);
        if (lit == bank->data->layouts.end()) continue;
        const auto* pn = find_param(ev->sys, lit->second.param);
        if (pn) inst->param_values[lit->second.param] = pn->def_v;
      }
    }
  }
  ev->instances.push_back(inst);
  ev->sys->instances.push_back(inst);
  invoke_callback(inst, 0x01);  // CREATED
  *out = inst;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_instance_count(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = (int)ev->instances.size();
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_instance_list(void* h, void** out, int capacity,
                                            int* count) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!count) return TRT_FMOD_ERR_INVALID_PARAM;
  int n = (int)ev->instances.size();
  int fill = n < capacity ? n : capacity;
  if (out) {
    for (int i = 0; i < fill; ++i) out[i] = ev->instances[(size_t)i];
  }
  *count = fill;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_load_sample_data(void* h) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  // Demand-decoded like the bank path; pre-decode the event's samples
  // into the cache so Start() has no first-play hitch.
  std::vector<std::tuple<bank_data*, int, int>> samples;
  {
    std::lock_guard<std::mutex> lk(ev->sys->mu);
    collect_event_samples(ev->sys, ev->guid, &samples, nullptr, nullptr);
  }
  for (auto& t : samples)
    get_decoded(std::get<0>(t), std::get<1>(t), std::get<2>(t));
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_unload_sample_data(void* h) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::vector<std::tuple<bank_data*, int, int>> samples;
  {
    std::lock_guard<std::mutex> lk(ev->sys->mu);
    collect_event_samples(ev->sys, ev->guid, &samples, nullptr, nullptr);
  }
  std::lock_guard<std::mutex> lk(g_cache_mu);
  for (auto& t : samples) {
    cache_key key{std::get<0>(t), std::get<1>(t), std::get<2>(t)};
    auto it = g_pcm_cache.find(key);
    if (it != g_pcm_cache.end()) {
      g_cache_bytes -= it->second->pcm.size() * sizeof(short);
      g_pcm_cache.erase(it);
    }
  }
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_sample_loading_state(void* h, int* out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = TRT_STUDIO_LOADING_LOADED;  // samples are demand-decodable
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_release_all_instances(void* h) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(ev->sys->mu);
  while (!ev->instances.empty()) free_instance_locked(ev->instances[0]);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_set_callback(void* h, trt_fmod_event_callback cb,
                                       int mask) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  ev->callback = cb;
  ev->callback_mask = mask;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_get_user_data(void* h, void** out) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = ev->user_data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_set_user_data(void* h, void* data) {
  studio_event* ev = check_event(h);
  if (!ev) return TRT_FMOD_ERR_INVALID_HANDLE;
  ev->user_data = data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_event_is_valid(void* h) {
  return obj_check(h, OT_EVENT) ? 1 : 0;
}

}  // extern "C"

extern "C" {

// ---- event instances ----
namespace {
void build_schedule_locked(studio_system* sys, studio_instance* inst) {
  inst->schedule.clear();
  bank_data* b = nullptr;
  const auto* ev = find_event_node(sys, inst->event->guid, &b);
  if (!ev) return;
  // Timeline trigger boxes (time-based).
  for (auto* bank : sys->banks) {
    if (!bank->data) continue;
    auto tit = bank->data->timelines.find(ev->timeline);
    if (tit != bank->data->timelines.end()) {
      for (auto& tb : tit->second) {
        studio_instance::sched_item si;
        si.start_ms = tb.start_ms;
        si.instrument = tb.instrument;
        inst->schedule.push_back(si);
      }
    }
  }
  // Parameter-layout instruments (range-based).
  for (auto& lg : ev->layouts) {
    for (auto* bank : sys->banks) {
      if (!bank->data) continue;
      auto lit = bank->data->layouts.find(lg);
      if (lit == bank->data->layouts.end()) continue;
      for (auto& lb : lit->second.boxes) {
        studio_instance::sched_item si;
        si.start_ms = 0;
        si.instrument = lb.instrument;
        si.is_layout = true;
        si.layout_param = lit->second.param;
        si.range_lo = lb.range_lo;
        si.range_hi = lb.range_hi;
        si.include_end = lb.include_end;
        inst->schedule.push_back(si);
      }
    }
  }
}

void stop_voices_locked(studio_instance* inst) {
  for (auto* v : inst->voices) {
    v->playing = false;
    v->done = true;
    delete v;
  }
  inst->voices.clear();
}
}  // namespace

int trt_fmod_studio_instance_start(void* h) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  studio_system* sys = inst->sys;
  std::lock_guard<std::mutex> lk(sys->mu);
  if (inst->state == TRT_STUDIO_PLAYBACK_PLAYING) {
    // FMOD restarts a playing instance on Start().
    stop_voices_locked(inst);
  }
  invoke_callback(inst, 0x04);  // STARTING
  inst->state = TRT_STUDIO_PLAYBACK_STARTING;
  build_schedule_locked(sys, inst);
  inst->start_time = std::chrono::steady_clock::now();
  inst->start_time_set = true;
  inst->paused = false;
  inst->callback_fired_stopped = false;
  inst->timeline_position_ms = 0;
  // Fire t=0 timeline items + in-range layout items immediately.
  for (auto& si : inst->schedule) {
    if (!si.fired && !si.is_layout && si.start_ms == 0) {
      si.fired = true;
      fire_instrument(sys, inst, si.instrument, 0);
    }
  }
  check_layout_items(sys, inst);
  bank_data* b = nullptr;
  const auto* ev = find_event_node(sys, inst->event->guid, &b);
  if (ev && !ev->snapshot.is_zero()) {
    static std::atomic<int> once{0};
    log_once(&once,
             "fmod-studio: snapshot events start as state but their mixer "
             "changes are not applied (SnapshotNode data not parsed yet)");
  }
  inst->state = TRT_STUDIO_PLAYBACK_PLAYING;
  invoke_callback(inst, 0x08);  // STARTED
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_stop(void* h, int mode) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  (void)mode;  // ALLOWFADEOUT vs IMMEDIATE: no fadeout implemented — the
               // ~1s of already-queued PCM plays out (host mixer has no
               // per-voice stop). Named gap.
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  if (inst->state == TRT_STUDIO_PLAYBACK_STOPPED) return TRT_FMOD_OK;
  stop_voices_locked(inst);
  inst->state = TRT_STUDIO_PLAYBACK_STOPPED;
  if (!inst->callback_fired_stopped) {
    inst->callback_fired_stopped = true;
    invoke_callback(inst, 0x20);  // STOPPED
  }
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_release(void* h) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  invoke_callback(inst, 0x02);  // DESTROYED
  free_instance_locked(inst);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_playback_state(void* h, int* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->state;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_paused(void* h, int* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->paused ? 1 : 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_paused(void* h, int paused) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  auto now = std::chrono::steady_clock::now();
  if (paused && !inst->paused) {
    inst->paused = true;
    inst->pause_start = now;
    for (auto* v : inst->voices) {
      v->paused = true;
      v->pause_start = now;
    }
  } else if (!paused && inst->paused) {
    inst->paused = false;
    auto delta = now - inst->pause_start;
    inst->start_time += delta;
    for (auto* v : inst->voices) {
      v->paused = false;
      v->start_time += delta;
    }
  }
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_volume(void* h, float* volume,
                                        float* final_volume) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  // Either out-param may be NULL (real FMOD tolerates that).
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  if (volume) *volume = inst->volume;
  if (final_volume) *final_volume = effective_volume(inst);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_volume(void* h, float volume) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  inst->volume = volume;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_pitch(void* h, float* pitch,
                                       float* final_pitch) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (pitch) *pitch = inst->pitch;
  if (final_pitch) *final_pitch = inst->pitch;  // no pitch automation in v1
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_pitch(void* h, float pitch) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  inst->pitch = pitch;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_3d_attrs(void* h, trt_fmod_3d_attrs* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->attrs_3d;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_3d_attrs(void* h,
                                          const trt_fmod_3d_attrs* attrs) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!attrs) return TRT_FMOD_ERR_INVALID_PARAM;
  inst->attrs_3d = *attrs;
  inst->has_3d_attrs = true;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_listener_mask(void* h, u32* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->listener_mask;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_listener_mask(void* h, u32 mask) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  inst->listener_mask = mask;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_property(void* h, int property, float* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out || property < 0 || property > 5) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->properties[property];
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_property(void* h, int property, float value) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (property < 0 || property > 5) return TRT_FMOD_ERR_INVALID_PARAM;
  // SCHEDULE_DELAY / SCHEDULE_LOOKAHEAD are stored but not applied to
  // scheduling in v1 (named gap); the rest are live state.
  inst->properties[property] = value;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_reverb_level(void* h, int index,
                                              float* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out || index < 0 || index > 3) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->reverb_levels[index];
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_reverb_level(void* h, int index,
                                              float level) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (index < 0 || index > 3) return TRT_FMOD_ERR_INVALID_PARAM;
  // Stored; no reverb DSP exists in the host mixer (named gap).
  inst->reverb_levels[index] = level;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_timeline_position(void* h, int* out_ms) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_ms) return TRT_FMOD_ERR_INVALID_PARAM;
  *out_ms = instance_elapsed_ms(inst) + inst->timeline_position_ms;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_timeline_position(void* h, int pos_ms) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (pos_ms < 0) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  if (inst->state != TRT_STUDIO_PLAYBACK_PLAYING) {
    inst->timeline_position_ms = pos_ms;
    return TRT_FMOD_OK;
  }
  // Seek: drop current voices, skip schedule items before the position,
  // re-arm items at/after it, and rebase the clock.
  stop_voices_locked(inst);
  for (auto& si : inst->schedule) {
    if (si.is_layout) continue;
    si.fired = (int)si.start_ms < pos_ms;
  }
  inst->start_time = std::chrono::steady_clock::now() -
                     std::chrono::milliseconds(pos_ms);
  inst->start_time_set = true;
  inst->timeline_position_ms = 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_channel_group(void* h, void** out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  studio_system* sys = inst->sys;
  if (!sys->low_level) return TRT_FMOD_ERR_STUDIO_UNINITIALIZED;
  // Instances route through the master channel group (the bridge has no
  // per-instance channel groups).
  void* grp = nullptr;
  int rc = fmod_System_GetMasterChannelGroup(sys->low_level, &grp);
  if (rc != 0 || !grp) return TRT_FMOD_ERR_INVALID_HANDLE;
  *out = grp;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_is_virtual(void* h, int* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = 0;  // no virtualization: every started instance is real
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_trigger_cue(void* h) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  // Cues fire at named timeline markers; TriggerCue on an event without
  // markers is a no-op in FMOD. Marker-triggered behavior is not
  // implemented in v1 (named gap).
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_callback(void* h, trt_fmod_event_callback cb,
                                          int mask) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  inst->callback = cb;
  inst->callback_mask = mask;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_description(void* h, void** out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->event;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_user_data(void* h, void** out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = inst->user_data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_user_data(void* h, void* data) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  inst->user_data = data;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_is_valid(void* h) {
  return obj_check(h, OT_INSTANCE) ? 1 : 0;
}

namespace {
// Find a parameter GUID on an event by name.
bool event_param_guid(studio_system* sys, studio_event* ev, const char* name,
                      guid16* out) {
  bank_data* b = nullptr;
  const auto* node = find_event_node(sys, ev->guid, &b);
  if (!node) return false;
  for (auto& lg : node->layouts) {
    for (auto* bank : sys->banks) {
      if (!bank->data) continue;
      auto lit = bank->data->layouts.find(lg);
      if (lit == bank->data->layouts.end()) continue;
      const auto* pn = find_param(sys, lit->second.param);
      if (pn && pn->name == name) {
        *out = lit->second.param;
        return true;
      }
    }
  }
  return false;
}
}  // namespace

int trt_fmod_studio_instance_set_parameter_value(void* h, const char* name,
                                                 float value) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!name) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  guid16 pg;
  if (!event_param_guid(inst->sys, inst->event, name, &pg))
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  inst->param_values[pg] = value;
  // Newly in-range layout instruments fire now.
  check_layout_items(inst->sys, inst);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_parameter_value(void* h, const char* name,
                                                 float* value,
                                                 float* final_value) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!name) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  guid16 pg;
  if (!event_param_guid(inst->sys, inst->event, name, &pg))
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  auto it = inst->param_values.find(pg);
  float v = 0;
  if (it != inst->param_values.end()) {
    v = it->second;
  } else {
    const auto* pn = find_param(inst->sys, pg);
    v = pn ? pn->def_v : 0;
  }
  if (value) *value = v;
  // no automation curves in v1: final == set value
  if (final_value) *final_value = v;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_set_parameter_value_by_index(void* h, int index,
                                                          float value) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(inst->sys, inst->event->guid, &b);
  if (!node || index < 0 || index >= (int)node->layouts.size())
    return TRT_FMOD_ERR_INVALID_PARAM;
  guid16 lg = node->layouts[(size_t)index];
  for (auto* bank : inst->sys->banks) {
    if (!bank->data) continue;
    auto lit = bank->data->layouts.find(lg);
    if (lit == bank->data->layouts.end()) continue;
    inst->param_values[lit->second.param] = value;
    check_layout_items(inst->sys, inst);
    return TRT_FMOD_OK;
  }
  return TRT_FMOD_ERR_EVENT_NOTFOUND;
}

int trt_fmod_studio_instance_get_parameter_value_by_index(void* h, int index,
                                                          float* value,
                                                          float* final_value) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(inst->sys, inst->event->guid, &b);
  if (!node || index < 0 || index >= (int)node->layouts.size())
    return TRT_FMOD_ERR_INVALID_PARAM;
  guid16 lg = node->layouts[(size_t)index];
  for (auto* bank : inst->sys->banks) {
    if (!bank->data) continue;
    auto lit = bank->data->layouts.find(lg);
    if (lit == bank->data->layouts.end()) continue;
    auto it = inst->param_values.find(lit->second.param);
    float v = 0;
    if (it != inst->param_values.end()) {
      v = it->second;
    } else {
      const auto* pn = find_param(inst->sys, lit->second.param);
      v = pn ? pn->def_v : 0;
    }
    if (value) *value = v;
    if (final_value) *final_value = v;
    return TRT_FMOD_OK;
  }
  return TRT_FMOD_ERR_EVENT_NOTFOUND;
}

int trt_fmod_studio_instance_get_parameter(void* h, const char* name,
                                           void** out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!name || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  guid16 pg;
  if (!event_param_guid(inst->sys, inst->event, name, &pg))
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  auto* p = new studio_param_inst();
  p->inst = inst;
  p->param_guid = pg;
  p->name = name;
  inst->param_insts.push_back(p);
  *out = p;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_instance_get_parameter_by_index(void* h, int index,
                                                    void** out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(inst->sys->mu);
  bank_data* b = nullptr;
  const auto* node = find_event_node(inst->sys, inst->event->guid, &b);
  if (!node || index < 0 || index >= (int)node->layouts.size())
    return TRT_FMOD_ERR_INVALID_PARAM;
  guid16 lg = node->layouts[(size_t)index];
  for (auto* bank : inst->sys->banks) {
    if (!bank->data) continue;
    auto lit = bank->data->layouts.find(lg);
    if (lit == bank->data->layouts.end()) continue;
    const auto* pn = find_param(inst->sys, lit->second.param);
    auto* p = new studio_param_inst();
    p->inst = inst;
    p->param_guid = lit->second.param;
    p->name = pn ? pn->name : std::string();
    inst->param_insts.push_back(p);
    *out = p;
    return TRT_FMOD_OK;
  }
  return TRT_FMOD_ERR_EVENT_NOTFOUND;
}

int trt_fmod_studio_instance_get_parameter_count(void* h, int* out) {
  studio_instance* inst = check_inst(h);
  if (!inst) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  return trt_fmod_studio_event_get_parameter_count(inst->event, out);
}

int trt_fmod_studio_parameter_get_value(void* h, float* out) {
  studio_param_inst* p = check_param(h);
  if (!p) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  if (!obj_check(p->inst, OT_INSTANCE)) return TRT_FMOD_ERR_INVALID_HANDLE;
  float v = 0, fv = 0;
  int rc = trt_fmod_studio_instance_get_parameter_value(
      p->inst, p->name.c_str(), &v, &fv);
  if (rc != TRT_FMOD_OK) return rc;
  *out = v;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_parameter_set_value(void* h, float value) {
  studio_param_inst* p = check_param(h);
  if (!p) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!obj_check(p->inst, OT_INSTANCE)) return TRT_FMOD_ERR_INVALID_HANDLE;
  return trt_fmod_studio_instance_set_parameter_value(p->inst,
                                                      p->name.c_str(), value);
}

int trt_fmod_studio_parameter_get_description(void* h,
                                              trt_fmod_param_desc* out) {
  studio_param_inst* p = check_param(h);
  if (!p) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  if (!obj_check(p->inst, OT_INSTANCE)) return TRT_FMOD_ERR_INVALID_HANDLE;
  return trt_fmod_studio_event_get_parameter(p->inst->event,
                                             p->name.c_str(), out);
}

int trt_fmod_studio_parameter_is_valid(void* h) {
  studio_param_inst* p = check_param(h);
  if (!p) return 0;
  return obj_check(p->inst, OT_INSTANCE) ? 1 : 0;
}

// ---- buses / VCAs ----
int trt_fmod_studio_get_bus(void* h, const char* path, void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!path || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  guid16 g;
  if (!resolve_path(sys, path, &g)) {
    *out = nullptr;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  *out = find_or_create_bus(sys, g);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_get_bus_by_id(void* h, const u8 guid[16], void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!guid || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  *out = find_or_create_bus(sys, guid_from(guid));
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_get_id(void* h, u8 out_guid[16]) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_guid) return TRT_FMOD_ERR_INVALID_PARAM;
  memcpy(out_guid, bus->guid.b, 16);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_get_path(void* h, char* out, int size,
                                 int* retrieved) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (bus->path.empty()) {
    if (retrieved) *retrieved = 0;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  return copy_path(bus->path, out, size, retrieved);
}

int trt_fmod_studio_bus_get_volume(void* h, float* volume,
                                   float* final_volume) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (volume) *volume = bus->volume;
  if (final_volume) *final_volume = bus->mute ? 0.0f : bus->volume;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_set_volume(void* h, float volume) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  bus->volume = volume;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_get_paused(void* h, int* out) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = bus->paused ? 1 : 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_set_paused(void* h, int paused) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  bus->paused = paused != 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_get_mute(void* h, int* out) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  *out = bus->mute ? 1 : 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_set_mute(void* h, int mute) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  bus->mute = mute != 0;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_stop_all_events(void* h, int mode) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  studio_system* sys = bus->sys;
  std::lock_guard<std::mutex> lk(sys->mu);
  // Stop instances whose event routes through this bus or a descendant
  // (path-prefix match on the event's input bus path).
  std::string prefix = bus->path;
  for (auto* inst : sys->instances) {
    bank_data* b = nullptr;
    const auto* ev = find_event_node(sys, inst->event->guid, &b);
    if (!ev || ev->input_bus.is_zero()) continue;
    std::string ipath = guid_path(sys, ev->input_bus);
    if (ipath == prefix ||
        (ipath.size() > prefix.size() &&
         ipath.compare(0, prefix.size(), prefix) == 0 &&
         ipath[prefix.size()] == '/')) {
      if (inst->state == TRT_STUDIO_PLAYBACK_PLAYING) {
        stop_voices_locked(inst);
        inst->state = TRT_STUDIO_PLAYBACK_STOPPED;
        if (!inst->callback_fired_stopped) {
          inst->callback_fired_stopped = true;
          invoke_callback(inst, 0x20);
        }
      }
    }
  }
  (void)mode;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_get_channel_group(void* h, void** out) {
  studio_bus* bus = check_bus(h);
  if (!bus) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out) return TRT_FMOD_ERR_INVALID_PARAM;
  studio_system* sys = bus->sys;
  if (!sys->low_level) return TRT_FMOD_ERR_STUDIO_UNINITIALIZED;
  void* grp = nullptr;
  int rc = fmod_System_GetMasterChannelGroup(sys->low_level, &grp);
  if (rc != 0 || !grp) return TRT_FMOD_ERR_INVALID_HANDLE;
  *out = grp;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_bus_is_valid(void* h) {
  return obj_check(h, OT_BUS) ? 1 : 0;
}

int trt_fmod_studio_get_vca(void* h, const char* path, void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!path || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  guid16 g;
  if (!resolve_path(sys, path, &g)) {
    *out = nullptr;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  *out = find_or_create_vca(sys, g);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_get_vca_by_id(void* h, const u8 guid[16], void** out) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!guid || !out) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  *out = find_or_create_vca(sys, guid_from(guid));
  return TRT_FMOD_OK;
}

int trt_fmod_studio_vca_get_id(void* h, u8 out_guid[16]) {
  studio_vca* vca = check_vca(h);
  if (!vca) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!out_guid) return TRT_FMOD_ERR_INVALID_PARAM;
  memcpy(out_guid, vca->guid.b, 16);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_vca_get_path(void* h, char* out, int size,
                                 int* retrieved) {
  studio_vca* vca = check_vca(h);
  if (!vca) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (vca->path.empty()) {
    if (retrieved) *retrieved = 0;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  return copy_path(vca->path, out, size, retrieved);
}

int trt_fmod_studio_vca_get_volume(void* h, float* volume,
                                   float* final_volume) {
  studio_vca* vca = check_vca(h);
  if (!vca) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (volume) *volume = vca->volume;
  if (final_volume) *final_volume = vca->volume;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_vca_set_volume(void* h, float volume) {
  studio_vca* vca = check_vca(h);
  if (!vca) return TRT_FMOD_ERR_INVALID_HANDLE;
  vca->volume = volume;
  return TRT_FMOD_OK;
}

int trt_fmod_studio_vca_is_valid(void* h) {
  return obj_check(h, OT_VCA) ? 1 : 0;
}

// ---- GUID / path lookup ----
int trt_fmod_studio_lookup_id(void* h, const char* path, u8 out_guid[16]) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!path || !out_guid) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  guid16 g;
  if (!resolve_path(sys, path, &g)) return TRT_FMOD_ERR_EVENT_NOTFOUND;
  memcpy(out_guid, g.b, 16);
  return TRT_FMOD_OK;
}

int trt_fmod_studio_lookup_path(void* h, const u8 guid[16], char* out,
                                int size, int* retrieved) {
  studio_system* sys = check_sys(h);
  if (!sys) return TRT_FMOD_ERR_INVALID_HANDLE;
  if (!guid || !out || !retrieved) return TRT_FMOD_ERR_INVALID_PARAM;
  std::lock_guard<std::mutex> lk(sys->mu);
  std::string path = guid_path(sys, guid_from(guid));
  if (path.empty()) {
    *retrieved = 0;
    return TRT_FMOD_ERR_EVENT_NOTFOUND;
  }
  return copy_path(path, out, size, retrieved);
}

// ---- test introspection ----
int trt_fmod_studio_total_event_count(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return 0;
  int total = 0;
  for (auto* b : sys->banks)
    if (b->data) total += (int)b->data->events.size();
  return total;
}

// Handle-free version: sums parsed events across ALL live Studio
// systems. The harness test uses this — the game's system handle lives
// inside managed code and the test cannot know it.
int trt_fmod_studio_global_event_count() { return registry_event_count(); }

int trt_fmod_studio_total_sample_count(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return 0;
  int total = 0;
  for (auto* b : sys->banks) {
    if (!b->data) continue;
    for (auto& fb : b->data->fsbs) total += (int)fb.samples.size();
  }
  return total;
}

u64 trt_fmod_studio_total_pcm_bytes_submitted(void* h) {
  studio_system* sys = check_sys(h);
  if (!sys) return 0;
  return sys->pcm_bytes_submitted.load();
}

int trt_fmod_studio_bank_sample_count(void* h) {
  studio_bank* bank = check_bank(h);
  if (!bank) return 0;
  int total = 0;
  for (auto& fb : bank->data->fsbs) total += (int)fb.samples.size();
  return total;
}

}  // extern "C"
