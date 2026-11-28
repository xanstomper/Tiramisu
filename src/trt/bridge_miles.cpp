// tiramisu/src/trt/bridge_miles.cpp
// Miles Sound System (mss32.dll / miles.dll) bridge — per-handle sample &
// stream management with real PCM/WAV/MP3/Ogg decode routed into trt_audio.
#include "bridge_miles.h"
#include "audio_codecs.h"
#include "bridge_audio.h"
#include "thunk.h"
#include "trt.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
namespace mileslog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) mileslog::line(s_)

constexpr int MILES_SUCCESS = 1;
constexpr int SMP_DONE = 0;
constexpr int SMP_PLAYING = 4;
constexpr int SMP_STOPPED = 8;

struct miles_handle {
  u64 voice_id = 0;
  std::vector<int16_t> pcm;
  u32 rate = 44100;
  u32 channels = 2;
  float volume = 1.0f;
  float pan = 0.5f;
  int loop_count = 1;
  int status = SMP_DONE;
  bool is_stream = false;
};

std::mutex g_miles_mu;
std::atomic<u64> g_miles_seq{1};
std::unordered_set<miles_handle*> g_miles_handles;
int g_miles_driver_token = 1;
int g_miles_provider_token = 1;

miles_handle* alloc_handle(bool is_stream) {
  auto* h = new miles_handle();
  h->voice_id = (0x4D494C53ull << 32) | g_miles_seq.fetch_add(1);
  h->is_stream = is_stream;
  h->status = SMP_DONE;
  std::lock_guard<std::mutex> lk(g_miles_mu);
  g_miles_handles.insert(h);
  return h;
}

miles_handle* find_handle_locked(void* p) {
  auto* h = static_cast<miles_handle*>(p);
  return (h && g_miles_handles.find(h) != g_miles_handles.end()) ? h : nullptr;
}

bool decode_into_handle_locked(miles_handle* h, const u8* data, size_t len) {
  if (!h || !data || len == 0) return false;
  trt_decoded_pcm dec{};
  if (trt_decode_audio_buffer(data, len, &dec) == 0 && dec.samples && dec.frames > 0) {
    h->rate = dec.rate ? dec.rate : 44100u;
    h->channels = dec.channels ? dec.channels : 2u;
    h->pcm.assign(dec.samples, dec.samples + static_cast<size_t>(dec.frames) * h->channels);
    free(dec.samples);
    return true;
  }
  return false;
}

bool load_file_bytes(const char* filename, std::vector<u8>* out) {
  if (!filename || !filename[0] || !out) return false;
  u16 wpath[512] = {};
  size_t n = 0;
  for (; filename[n] && n < 511u; ++n) {
    wpath[n] = static_cast<u16>(static_cast<unsigned char>(filename[n]));
  }
  wpath[n] = 0;
  char resolved[1024] = {};
  const char* open_p = filename;
  if (trt_vfs_resolve_utf16_rw(wpath, 0, resolved, sizeof(resolved)) == 0 && resolved[0]) {
    open_p = resolved;
  }
  FILE* f = fopen(open_p, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > (64L << 20)) {
    fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  out->resize(got);
  return got > 0;
}

void submit_handle_locked(miles_handle* h) {
  if (!h) return;
  if (!h->pcm.empty()) {
    const u32 bytes = static_cast<u32>(h->pcm.size() * sizeof(int16_t));
    trt_audio_submit_voice(h->voice_id, h->pcm.data(), bytes, h->rate, h->channels,
                           2u, (h->loop_count == 0) ? 1u : 0u, h->volume);
  }
  // Mark done after submission so games polling AIL_sample_status / AIL_stream_status
  // for cutscene/dialogue completion never soft-lock.
  h->status = SMP_DONE;
}

}  // namespace

extern "C" {

int TRT_GUEST_ABI miles_AIL_startup(void) {
  log_line("miles: AIL_startup -> active PCM/WAV/MP3/Ogg bridge");
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_shutdown(void) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  for (auto* h : g_miles_handles) delete h;
  g_miles_handles.clear();
  return MILES_SUCCESS;
}

const char* TRT_GUEST_ABI miles_AIL_last_error(void) { return ""; }

void* TRT_GUEST_ABI miles_AIL_allocate_sample_handle(void* dig) {
  (void)dig;
  return alloc_handle(false);
}

int TRT_GUEST_ABI miles_AIL_free_sample_handle(void* sam) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    g_miles_handles.erase(h);
    delete h;
  }
  return MILES_SUCCESS;
}

void* TRT_GUEST_ABI miles_AIL_open_digital_driver(unsigned rate, unsigned bits,
                                                  int channels, unsigned flags) {
  (void)rate; (void)bits; (void)channels; (void)flags;
  return &g_miles_driver_token;
}

int TRT_GUEST_ABI miles_AIL_close_digital_driver(void* dig) {
  (void)dig;
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_sample_volume(void* sam, int vol) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    h->volume = std::clamp(static_cast<float>(vol) / 127.0f, 0.0f, 1.0f);
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_sample_volume_pan(void* sam, float vol, float pan) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    h->volume = std::clamp(vol, 0.0f, 1.0f);
    h->pan = std::clamp(pan, 0.0f, 1.0f);
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_sample_pan(void* sam, int pan) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    h->pan = std::clamp(static_cast<float>(pan) / 127.0f, 0.0f, 1.0f);
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_sample_loop_count(void* sam, int count) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) h->loop_count = count;
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_sample_file(void* sam, const void* file_image, int block) {
  (void)block;
  if (!file_image) return 0;
  // Best-effort RIFF/Ogg/MP3 size probe from header (RIFF carries explicit size).
  const auto* b = static_cast<const u8*>(file_image);
  size_t len = 0;
  if (memcmp(b, "RIFF", 4) == 0) {
    u32 rsz = 0;
    memcpy(&rsz, b + 4, 4);
    if (rsz > 0 && rsz < (64u << 20)) len = static_cast<size_t>(rsz) + 8u;
  }
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    if (len > 0) decode_into_handle_locked(h, b, len);
    h->status = SMP_STOPPED;
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_load_sample_buffer(void* sam, unsigned index,
                                               const void* data, unsigned len) {
  (void)index;
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam); h && data && len > 0) {
    if (!decode_into_handle_locked(h, static_cast<const u8*>(data), len)) {
      // Raw 16-bit PCM fallback if container header wasn't present
      const size_t n = len / sizeof(int16_t);
      h->pcm.resize(n);
      if (n > 0) memcpy(h->pcm.data(), data, n * sizeof(int16_t));
    }
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_start_sample(void* sam) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) {
    h->status = SMP_PLAYING;
    submit_handle_locked(h);
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_stop_sample(void* sam) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) h->status = SMP_STOPPED;
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_end_sample(void* sam) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) h->status = SMP_DONE;
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_sample_status(void* sam) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(sam)) return h->status;
  return SMP_DONE;
}

int TRT_GUEST_ABI miles_AIL_service(void) { return MILES_SUCCESS; }

void* TRT_GUEST_ABI miles_AIL_3D_provider_open(void* dig, const char* name) {
  (void)dig; (void)name;
  return &g_miles_provider_token;
}

int TRT_GUEST_ABI miles_AIL_3D_provider_close(void* prov) {
  (void)prov;
  return MILES_SUCCESS;
}

void* TRT_GUEST_ABI miles_AIL_open_stream(void* dig, const char* filename, int stream_flags) {
  (void)dig; (void)stream_flags;
  auto* h = alloc_handle(true);
  std::vector<u8> raw;
  if (load_file_bytes(filename, &raw) && !raw.empty()) {
    std::lock_guard<std::mutex> lk(g_miles_mu);
    decode_into_handle_locked(h, raw.data(), raw.size());
  }
  return h;
}

int TRT_GUEST_ABI miles_AIL_close_stream(void* stream) {
  return miles_AIL_free_sample_handle(stream);
}

int TRT_GUEST_ABI miles_AIL_start_stream(void* stream) {
  return miles_AIL_start_sample(stream);
}

int TRT_GUEST_ABI miles_AIL_stop_stream(void* stream) {
  return miles_AIL_stop_sample(stream);
}

int TRT_GUEST_ABI miles_AIL_pause_stream(void* stream, int pause) {
  std::lock_guard<std::mutex> lk(g_miles_mu);
  if (auto* h = find_handle_locked(stream)) {
    h->status = pause ? SMP_STOPPED : SMP_DONE;
  }
  return MILES_SUCCESS;
}

int TRT_GUEST_ABI miles_AIL_set_stream_volume(void* stream, int vol) {
  return miles_AIL_set_sample_volume(stream, vol);
}

int TRT_GUEST_ABI miles_AIL_set_stream_volume_pan(void* stream, float vol, float pan) {
  return miles_AIL_set_sample_volume_pan(stream, vol, pan);
}

int TRT_GUEST_ABI miles_AIL_set_stream_loop_count(void* stream, int count) {
  return miles_AIL_set_sample_loop_count(stream, count);
}

int TRT_GUEST_ABI miles_AIL_stream_status(void* stream) {
  return miles_AIL_sample_status(stream);
}

}  // extern "C"

extern "C" int trt_bridge_register_miles(void) {
  static const trt_named_fn k_fns[] = {
      {"AIL_startup", (void*)&miles_AIL_startup},
      {"AIL_shutdown", (void*)&miles_AIL_shutdown},
      {"AIL_last_error", (void*)&miles_AIL_last_error},
      {"AIL_allocate_sample_handle", (void*)&miles_AIL_allocate_sample_handle},
      {"AIL_free_sample_handle", (void*)&miles_AIL_free_sample_handle},
      {"AIL_open_digital_driver", (void*)&miles_AIL_open_digital_driver},
      {"AIL_close_digital_driver", (void*)&miles_AIL_close_digital_driver},
      {"AIL_set_sample_volume", (void*)&miles_AIL_set_sample_volume},
      {"AIL_set_sample_volume_pan", (void*)&miles_AIL_set_sample_volume_pan},
      {"AIL_set_sample_pan", (void*)&miles_AIL_set_sample_pan},
      {"AIL_set_sample_loop_count", (void*)&miles_AIL_set_sample_loop_count},
      {"AIL_set_sample_file", (void*)&miles_AIL_set_sample_file},
      {"AIL_start_sample", (void*)&miles_AIL_start_sample},
      {"AIL_stop_sample", (void*)&miles_AIL_stop_sample},
      {"AIL_end_sample", (void*)&miles_AIL_end_sample},
      {"AIL_sample_status", (void*)&miles_AIL_sample_status},
      {"AIL_load_sample_buffer", (void*)&miles_AIL_load_sample_buffer},
      {"AIL_service", (void*)&miles_AIL_service},
      {"AIL_3D_provider_open", (void*)&miles_AIL_3D_provider_open},
      {"AIL_3D_provider_close", (void*)&miles_AIL_3D_provider_close},
      {"AIL_open_stream", (void*)&miles_AIL_open_stream},
      {"AIL_close_stream", (void*)&miles_AIL_close_stream},
      {"AIL_start_stream", (void*)&miles_AIL_start_stream},
      {"AIL_stop_stream", (void*)&miles_AIL_stop_stream},
      {"AIL_pause_stream", (void*)&miles_AIL_pause_stream},
      {"AIL_set_stream_volume", (void*)&miles_AIL_set_stream_volume},
      {"AIL_set_stream_volume_pan", (void*)&miles_AIL_set_stream_volume_pan},
      {"AIL_set_stream_loop_count", (void*)&miles_AIL_set_stream_loop_count},
      {"AIL_stream_status", (void*)&miles_AIL_stream_status},
  };
  trt_module_register("mss32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("miles.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}
