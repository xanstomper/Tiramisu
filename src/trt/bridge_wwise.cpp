// tiramisu/src/trt/bridge_wwise.cpp
// Audiokinetic Wwise SoundEngine bridge — real .bnk (BKHD/DIDX/DATA/HIRC)
// parsing, .wem PCM/IMA-ADPCM/Ogg/MP3 decoding via audio_codecs, RTPC/State/
// Switch/GameObject tracking, and real PCM voice submission to the host mixer.
#include "bridge_wwise.h"
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
#include <unordered_map>
#include <vector>

namespace {

// Preserved legacy return contract (AK_Success = 0) expected by existing tests
// and thunked Wwise shims.
constexpr int AK_Success = 0;
constexpr int AK_Fail = 2;

struct wwise_media_blob {
  u32 media_id = 0;
  std::vector<u8> bytes;
};

struct wwise_action {
  u32 action_id = 0;
  u8 scope = 0;
  u8 type = 0;       // 1 = Stop, 2 = Pause, 3 = Resume, 4 = Play
  u32 target_id = 0; // Sound / Container ID
};

struct wwise_bank {
  u32 bank_id = 0;
  std::string name;
  std::unordered_map<u32, wwise_media_blob> media;       // media_id -> .wem bytes
  std::unordered_map<u32, u32> sound_to_media;           // sound_obj_id -> media_id
  std::unordered_map<u32, wwise_action> actions;         // action_id -> action
  std::unordered_map<u32, std::vector<u32>> events;      // event_id -> action_ids
};

struct wwise_pending_voice {
  u64 voice_id = 0;
  u32 game_obj = 0;
  u32 media_id = 0;
  float volume = 1.0f;
  std::vector<int16_t> pcm;
  u32 rate = 44100;
  u32 channels = 2;
};

std::mutex g_wwise_mu;
std::atomic<bool> g_wwise_init{false};
std::atomic<u32> g_next_playing_id{1};
std::atomic<u32> g_next_bank_id{100};
std::atomic<u64> g_total_samples_submitted{0};

std::string g_base_path;
std::unordered_map<u32, wwise_bank> g_banks;
std::unordered_map<u32, std::string> g_game_objs;
std::unordered_map<u32, float> g_rtpcs;
std::unordered_map<u32, u32> g_states;
std::unordered_map<u64, u32> g_switches;  // (game_obj << 32) | group_id -> state_id
std::vector<wwise_pending_voice> g_pending_voices;

inline u32 rd32(const u8* p) {
  u32 v = 0;
  memcpy(&v, p, 4);
  return v;
}

// Wwise FNV-1 32-bit lowercase hash (official AkSoundEngine::GetIDFromString)
u32 wwise_fnv1(const char* str) {
  if (!str || !*str) return 0;
  u32 h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    unsigned char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
    h = (h * 16777619u) ^ c;
  }
  return h;
}

void log_wwise_summary(const char* verb, u32 banks_cnt, u64 samples_cnt) {
  char buf[192];
  snprintf(buf, sizeof(buf),
           "wwise: %s, %u banks loaded, %llu samples submitted",
           verb, banks_cnt, static_cast<unsigned long long>(samples_cnt));
  trt_core_log(buf);
}

// Parse a Wwise SoundBank (.bnk) binary buffer: BKHD, DIDX, DATA, HIRC.
bool parse_wwise_bank(const u8* data, size_t size, wwise_bank* out) {
  if (!data || size < 16u || !out) return false;
  if (memcmp(data, "BKHD", 4) != 0) return false;

  struct didx_entry { u32 id, off, sz; };
  std::vector<didx_entry> didx;
  const u8* data_chunk = nullptr;
  size_t data_chunk_sz = 0;
  const u8* hirc_chunk = nullptr;
  size_t hirc_chunk_sz = 0;

  size_t pos = 0;
  while (pos + 8u <= size) {
    const u8* cid = data + pos;
    const u32 clen = rd32(data + pos + 4u);
    pos += 8u;
    if (clen > size - pos) break;
    const u8* body = data + pos;

    if (memcmp(cid, "BKHD", 4) == 0 && clen >= 8u) {
      // body[0..3] = bank generator version, body[4..7] = bank_id
      out->bank_id = rd32(body + 4u);
    } else if (memcmp(cid, "DIDX", 4) == 0) {
      const size_t n = clen / 12u;
      didx.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        didx.push_back({rd32(body + i * 12u),
                        rd32(body + i * 12u + 4u),
                        rd32(body + i * 12u + 8u)});
      }
    } else if (memcmp(cid, "DATA", 4) == 0) {
      data_chunk = body;
      data_chunk_sz = clen;
    } else if (memcmp(cid, "HIRC", 4) == 0) {
      hirc_chunk = body;
      hirc_chunk_sz = clen;
    }
    pos += clen;
  }

  if (data_chunk && !didx.empty()) {
    for (const auto& e : didx) {
      if (e.sz == 0 || e.off > data_chunk_sz || e.sz > data_chunk_sz - e.off)
        continue;
      if (e.sz > (64u << 20)) continue;
      wwise_media_blob blob;
      blob.media_id = e.id;
      blob.bytes.assign(data_chunk + e.off, data_chunk + e.off + e.sz);
      out->media[e.id] = std::move(blob);
    }
  }

  if (hirc_chunk && hirc_chunk_sz >= 4u) {
    const u32 num_objs = rd32(hirc_chunk);
    size_t hp = 4u;
    for (u32 i = 0; i < num_objs && hp + 5u <= hirc_chunk_sz; ++i) {
      const u8 type = hirc_chunk[hp];
      const u32 obj_sz = rd32(hirc_chunk + hp + 1u);
      hp += 5u;
      if (obj_sz < 4u || obj_sz > hirc_chunk_sz - hp) break;
      const u32 obj_id = rd32(hirc_chunk + hp);
      const u8* ob = hirc_chunk + hp + 4u;
      const u32 ob_len = obj_sz - 4u;

      if (type == 2u && ob_len >= 9u) {
        // Sound SFX/Voice: AkBankSourceData starts with u32 plugin_id + u8 stream_type + u32 source_id
        u32 source_id = rd32(ob + 5u);
        if (out->media.find(source_id) == out->media.end() && ob_len >= 13u) {
          // Some Wwise versions prepend 4 bytes of plugin/source flags
          const u32 alt = rd32(ob + 9u);
          if (out->media.find(alt) != out->media.end()) source_id = alt;
        }
        out->sound_to_media[obj_id] = source_id;
      } else if (type == 3u && ob_len >= 6u) {
        // Action: u8 scope, u8 action_type, u32 target_id
        wwise_action act;
        act.action_id = obj_id;
        act.scope = ob[0];
        act.type = ob[1];
        act.target_id = rd32(ob + 2u);
        out->actions[obj_id] = act;
      } else if (type == 4u && ob_len >= 5u) {
        // Event: either u32 count (v<=120) or u8 count (v>=122) followed by action IDs
        std::vector<u32> aids;
        const u32 cnt32 = rd32(ob);
        if (cnt32 <= 256u && ob_len == 4u + cnt32 * 4u) {
          for (u32 k = 0; k < cnt32; ++k) aids.push_back(rd32(ob + 4u + k * 4u));
        } else {
          const u32 cnt8 = ob[0];
          if (1u + cnt8 * 4u <= ob_len) {
            for (u32 k = 0; k < cnt8; ++k) aids.push_back(rd32(ob + 1u + k * 4u));
          }
        }
        if (!aids.empty()) out->events[obj_id] = std::move(aids);
      }
      hp += obj_sz;
    }
  }
  return true;
}

bool read_file_vfs_or_host(const char* name, std::vector<u8>* out) {
  if (!name || !name[0] || !out) return false;
  std::string path = name;
  if (!g_base_path.empty() && path.find(':') == std::string::npos &&
      path[0] != '/' && path[0] != '\\') {
    std::string sep = (g_base_path.back() == '\\' || g_base_path.back() == '/') ? "" : "\\";
    path = g_base_path + sep + path;
  }
  u16 wpath[512] = {};
  size_t n = 0;
  for (; n < path.size() && n < 511u; ++n) wpath[n] = static_cast<u16>(static_cast<unsigned char>(path[n]));
  wpath[n] = 0;

  char resolved[1024] = {};
  const char* open_path = path.c_str();
  if (trt_vfs_resolve_utf16_rw(wpath, 0, resolved, sizeof(resolved)) == 0 && resolved[0]) {
    open_path = resolved;
  }
  FILE* f = fopen(open_path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsz <= 0 || fsz > (128L << 20)) {
    fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(fsz));
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  out->resize(got);
  return got > 0;
}

void queue_media_playback_locked(u32 media_id, u32 game_obj, u32 playing_id) {
  const std::vector<u8>* blob = nullptr;
  std::vector<u8> external_wem;
  for (const auto& kv : g_banks) {
    auto it = kv.second.media.find(media_id);
    if (it != kv.second.media.end() && !it->second.bytes.empty()) {
      blob = &it->second.bytes;
      break;
    }
  }
  if (!blob) {
    char wem_name[64];
    snprintf(wem_name, sizeof(wem_name), "%u.wem", media_id);
    if (read_file_vfs_or_host(wem_name, &external_wem) && !external_wem.empty()) {
      blob = &external_wem;
    }
  }
  if (!blob) return;

  trt_decoded_pcm dec{};
  if (trt_decode_audio_buffer(blob->data(), blob->size(), &dec) != 0 ||
      !dec.samples || dec.frames == 0) {
    return;
  }
  wwise_pending_voice v;
  v.voice_id = (0x57574953ull << 32) | static_cast<u64>(playing_id);
  v.game_obj = game_obj;
  v.media_id = media_id;
  v.volume = 1.0f;
  v.rate = dec.rate ? dec.rate : 44100u;
  v.channels = dec.channels ? dec.channels : 2u;
  const size_t n_samps = static_cast<size_t>(dec.frames) * v.channels;
  v.pcm.assign(dec.samples, dec.samples + n_samps);
  free(dec.samples);
  g_pending_voices.push_back(std::move(v));
}

void trigger_event_locked(u32 event_id, u32 game_obj, u32 playing_id) {
  bool matched = false;
  for (const auto& kv : g_banks) {
    const auto& bk = kv.second;
    auto eit = bk.events.find(event_id);
    if (eit != bk.events.end()) {
      matched = true;
      for (u32 aid : eit->second) {
        auto ait = bk.actions.find(aid);
        if (ait == bk.actions.end()) continue;
        const auto& act = ait->second;
        if (act.type == 1u) {
          // Stop action
          g_pending_voices.erase(
              std::remove_if(g_pending_voices.begin(), g_pending_voices.end(),
                             [&](const wwise_pending_voice& v) {
                               return game_obj == 0 || v.game_obj == game_obj;
                             }),
              g_pending_voices.end());
        } else if (act.type == 4u) {
          // Play action: target_id -> Sound -> media_id (or direct media_id)
          u32 media_id = act.target_id;
          auto sit = bk.sound_to_media.find(act.target_id);
          if (sit != bk.sound_to_media.end()) media_id = sit->second;
          queue_media_playback_locked(media_id, game_obj, playing_id);
        }
      }
    }
  }
  if (!matched) {
    // Direct sound ID or media ID fallback
    for (const auto& kv : g_banks) {
      auto sit = kv.second.sound_to_media.find(event_id);
      if (sit != kv.second.sound_to_media.end()) {
        queue_media_playback_locked(sit->second, game_obj, playing_id);
        return;
      }
      if (kv.second.media.find(event_id) != kv.second.media.end()) {
        queue_media_playback_locked(event_id, game_obj, playing_id);
        return;
      }
    }
  }
}

void flush_pending_voices_locked() {
  if (g_pending_voices.empty()) return;
  auto voices = std::move(g_pending_voices);
  g_pending_voices.clear();
  for (const auto& v : voices) {
    if (v.pcm.empty()) continue;
    const u32 bytes = static_cast<u32>(v.pcm.size() * sizeof(int16_t));
    trt_audio_submit_voice(v.voice_id, v.pcm.data(), bytes, v.rate, v.channels,
                           2u, 0u, v.volume);
    g_total_samples_submitted.fetch_add(v.pcm.size() / v.channels);
  }
}

}  // namespace

extern "C" {

// Memory Manager
int TRT_GUEST_ABI wwise_AK_MemoryMgr_Init(void* settings) { (void)settings; return AK_Success; }
void TRT_GUEST_ABI wwise_AK_MemoryMgr_Term(void) {}
void* TRT_GUEST_ABI wwise_AK_MemoryMgr_Malloc(u32 size) { return malloc(size ? size : 1); }
void TRT_GUEST_ABI wwise_AK_MemoryMgr_Free(void* p) { free(p); }

// Stream Manager
void* TRT_GUEST_ABI wwise_AK_StreamMgr_Create(void* settings) {
  (void)settings;
  static int dummy_stream_mgr = 1;
  return &dummy_stream_mgr;
}
void TRT_GUEST_ABI wwise_AK_StreamMgr_SetCurrentLanguage(const void* lang) { (void)lang; }
int TRT_GUEST_ABI wwise_AK_StreamMgr_SetBasePath(const char* path) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_base_path = path ? path : "";
  return AK_Success;
}

// Sound Engine Lifecycle
int TRT_GUEST_ABI wwise_AK_SoundEngine_Init(void* settings, void* platform) {
  (void)settings; (void)platform;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_wwise_init.store(true);
  log_wwise_summary("audio initialized", static_cast<u32>(g_banks.size()),
                    g_total_samples_submitted.load());
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_Term(void) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  flush_pending_voices_locked();
  log_wwise_summary("audio terminated", static_cast<u32>(g_banks.size()),
                    g_total_samples_submitted.load());
  g_wwise_init.store(false);
  g_banks.clear();
  g_game_objs.clear();
  g_pending_voices.clear();
  return AK_Success;
}
u32 TRT_GUEST_ABI wwise_AK_SoundEngine_IsInitialized(void) {
  return g_wwise_init.load() ? 1u : 0u;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_RenderAudio(int allow_sync) {
  (void)allow_sync;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  flush_pending_voices_locked();
  return AK_Success;
}
void TRT_GUEST_ABI wwise_AK_SoundEngine_GetDefaultInitSettings(void* s) {
  if (s) memset(s, 0, 64);
}
void TRT_GUEST_ABI wwise_AK_SoundEngine_GetDefaultPlatformInitSettings(void* s) {
  if (s) memset(s, 0, 64);
}

// Game Objects
int TRT_GUEST_ABI wwise_AK_SoundEngine_RegisterGameObj(u32 id, const char* name) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_game_objs[id] = name ? name : "";
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_UnregisterGameObj(u32 id) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_game_objs.erase(id);
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_UnregisterAllGameObj(void) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_game_objs.clear();
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetPosition(u32 id, const void* pos) {
  (void)id; (void)pos; return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetDefaultListeners(const u32* ids, u32 count) {
  (void)ids; (void)count; return AK_Success;
}

// Banks
int TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBank(const char* name, u32* out_bank_id) {
  if (!name || !name[0]) return AK_Fail;
  std::vector<u8> raw;
  wwise_bank bk;
  bk.name = name;
  bk.bank_id = wwise_fnv1(name);
  if (!bk.bank_id) bk.bank_id = g_next_bank_id.fetch_add(1);

  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (read_file_vfs_or_host(name, &raw) && !raw.empty()) {
    parse_wwise_bank(raw.data(), raw.size(), &bk);
  }
  const u32 bid = bk.bank_id;
  g_banks[bid] = std::move(bk);
  if (out_bank_id) *out_bank_id = bid;
  log_wwise_summary("bank loaded", static_cast<u32>(g_banks.size()),
                    g_total_samples_submitted.load());
  return AK_Success;
}

int TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBankMemoryView(const void* buf, u32 size, u32* out_bank_id) {
  if (!buf || size < 16u) return AK_Fail;
  wwise_bank bk;
  bk.name = "(memory)";
  bk.bank_id = g_next_bank_id.fetch_add(1);
  if (!parse_wwise_bank(static_cast<const u8*>(buf), size, &bk)) return AK_Fail;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  const u32 bid = bk.bank_id;
  g_banks[bid] = std::move(bk);
  if (out_bank_id) *out_bank_id = bid;
  log_wwise_summary("memory bank loaded", static_cast<u32>(g_banks.size()),
                    g_total_samples_submitted.load());
  return AK_Success;
}

int TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBankID(u32 bank_id) {
  char fn[64];
  snprintf(fn, sizeof(fn), "%u.bnk", bank_id);
  u32 out_id = bank_id;
  std::vector<u8> raw;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  wwise_bank bk;
  bk.bank_id = bank_id;
  bk.name = fn;
  if (read_file_vfs_or_host(fn, &raw) && !raw.empty()) {
    parse_wwise_bank(raw.data(), raw.size(), &bk);
  }
  g_banks[out_id] = std::move(bk);
  return AK_Success;
}

int TRT_GUEST_ABI wwise_AK_SoundEngine_UnloadBank(const char* name, const void* mem) {
  (void)mem;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (!name) return AK_Success;
  const u32 bid = wwise_fnv1(name);
  g_banks.erase(bid);
  for (auto it = g_banks.begin(); it != g_banks.end(); ++it) {
    if (it->second.name == name) {
      g_banks.erase(it);
      break;
    }
  }
  return AK_Success;
}

int TRT_GUEST_ABI wwise_AK_SoundEngine_UnloadBankID(u32 bank_id, const void* mem) {
  (void)mem;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_banks.erase(bank_id);
  return AK_Success;
}

int TRT_GUEST_ABI wwise_AK_SoundEngine_ClearBanks(void) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_banks.clear();
  return AK_Success;
}

// Events & Playback
u32 TRT_GUEST_ABI wwise_AK_SoundEngine_PostEvent(const char* event_name, u32 game_obj,
                                                 u32 flags, void* cb, void* cookie) {
  (void)flags; (void)cb; (void)cookie;
  const u32 pid = g_next_playing_id.fetch_add(1);
  if (event_name && event_name[0]) {
    const u32 eid = wwise_fnv1(event_name);
    std::lock_guard<std::mutex> lk(g_wwise_mu);
    trigger_event_locked(eid, game_obj, pid);
  }
  return pid;
}

u32 TRT_GUEST_ABI wwise_AK_SoundEngine_PostEventID(u32 event_id, u32 game_obj,
                                                   u32 flags, void* cb, void* cookie) {
  (void)flags; (void)cb; (void)cookie;
  const u32 pid = g_next_playing_id.fetch_add(1);
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  trigger_event_locked(event_id, game_obj, pid);
  return pid;
}

void TRT_GUEST_ABI wwise_AK_SoundEngine_StopAll(u32 game_obj) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (game_obj == 0 || game_obj == 0xFFFFFFFFu) {
    g_pending_voices.clear();
  } else {
    g_pending_voices.erase(
        std::remove_if(g_pending_voices.begin(), g_pending_voices.end(),
                       [&](const wwise_pending_voice& v) { return v.game_obj == game_obj; }),
        g_pending_voices.end());
  }
}

void TRT_GUEST_ABI wwise_AK_SoundEngine_StopPlayingID(u32 playing_id, int fade_ms, int curve) {
  (void)fade_ms; (void)curve;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  const u64 vid = (0x57574953ull << 32) | static_cast<u64>(playing_id);
  g_pending_voices.erase(
      std::remove_if(g_pending_voices.begin(), g_pending_voices.end(),
                     [&](const wwise_pending_voice& v) { return v.voice_id == vid; }),
      g_pending_voices.end());
}

// RTPCs, States, Switches
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetRTPCValue(const char* name, float val, u32 game_obj, int dur, int curve) {
  (void)game_obj; (void)dur; (void)curve;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (name) g_rtpcs[wwise_fnv1(name)] = val;
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetRTPCValueID(u32 id, float val, u32 game_obj, int dur, int curve) {
  (void)game_obj; (void)dur; (void)curve;
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_rtpcs[id] = val;
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetState(const char* group, const char* state) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (group && state) g_states[wwise_fnv1(group)] = wwise_fnv1(state);
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetStateID(u32 group, u32 state) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_states[group] = state;
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetSwitch(const char* group, const char* state, u32 game_obj) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  if (group && state) {
    g_switches[(static_cast<u64>(game_obj) << 32) | wwise_fnv1(group)] = wwise_fnv1(state);
  }
  return AK_Success;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetSwitchID(u32 group, u32 state, u32 game_obj) {
  std::lock_guard<std::mutex> lk(g_wwise_mu);
  g_switches[(static_cast<u64>(game_obj) << 32) | group] = state;
  return AK_Success;
}

// Music Engine
int TRT_GUEST_ABI wwise_AK_MusicEngine_Init(void* settings) { (void)settings; return AK_Success; }
void TRT_GUEST_ABI wwise_AK_MusicEngine_Term(void) {}
void TRT_GUEST_ABI wwise_AK_MusicEngine_GetDefaultInitSettings(void* s) { if (s) memset(s, 0, 32); }

// Hashing
u32 TRT_GUEST_ABI wwise_AK_SoundEngine_GetIDFromString(const char* str) {
  return wwise_fnv1(str);
}

// Diagnostics
u64 wwise_get_total_samples_submitted(void) {
  return g_total_samples_submitted.load();
}

static const trt_named_fn k_wwise_fns[] = {
  {"AK_MemoryMgr_Init", (void*)&wwise_AK_MemoryMgr_Init},
  {"AK_MemoryMgr_Term", (void*)&wwise_AK_MemoryMgr_Term},
  {"AK_MemoryMgr_Malloc", (void*)&wwise_AK_MemoryMgr_Malloc},
  {"AK_MemoryMgr_Free", (void*)&wwise_AK_MemoryMgr_Free},
  {"AK_StreamMgr_Create", (void*)&wwise_AK_StreamMgr_Create},
  {"AK_StreamMgr_SetCurrentLanguage", (void*)&wwise_AK_StreamMgr_SetCurrentLanguage},
  {"AK_StreamMgr_SetBasePath", (void*)&wwise_AK_StreamMgr_SetBasePath},
  {"AK_SoundEngine_Init", (void*)&wwise_AK_SoundEngine_Init},
  {"AK_SoundEngine_Term", (void*)&wwise_AK_SoundEngine_Term},
  {"AK_SoundEngine_IsInitialized", (void*)&wwise_AK_SoundEngine_IsInitialized},
  {"AK_SoundEngine_RenderAudio", (void*)&wwise_AK_SoundEngine_RenderAudio},
  {"AK_SoundEngine_GetDefaultInitSettings", (void*)&wwise_AK_SoundEngine_GetDefaultInitSettings},
  {"AK_SoundEngine_GetDefaultPlatformInitSettings", (void*)&wwise_AK_SoundEngine_GetDefaultPlatformInitSettings},
  {"AK_SoundEngine_RegisterGameObj", (void*)&wwise_AK_SoundEngine_RegisterGameObj},
  {"AK_SoundEngine_UnregisterGameObj", (void*)&wwise_AK_SoundEngine_UnregisterGameObj},
  {"AK_SoundEngine_UnregisterAllGameObj", (void*)&wwise_AK_SoundEngine_UnregisterAllGameObj},
  {"AK_SoundEngine_SetPosition", (void*)&wwise_AK_SoundEngine_SetPosition},
  {"AK_SoundEngine_SetDefaultListeners", (void*)&wwise_AK_SoundEngine_SetDefaultListeners},
  {"AK_SoundEngine_LoadBank", (void*)&wwise_AK_SoundEngine_LoadBank},
  {"AK_SoundEngine_LoadBankMemoryView", (void*)&wwise_AK_SoundEngine_LoadBankMemoryView},
  {"AK_SoundEngine_LoadBankMemoryCopy", (void*)&wwise_AK_SoundEngine_LoadBankMemoryView},
  {"AK_SoundEngine_LoadBankID", (void*)&wwise_AK_SoundEngine_LoadBankID},
  {"AK_SoundEngine_UnloadBank", (void*)&wwise_AK_SoundEngine_UnloadBank},
  {"AK_SoundEngine_UnloadBankID", (void*)&wwise_AK_SoundEngine_UnloadBankID},
  {"AK_SoundEngine_ClearBanks", (void*)&wwise_AK_SoundEngine_ClearBanks},
  {"AK_SoundEngine_PostEvent", (void*)&wwise_AK_SoundEngine_PostEvent},
  {"AK_SoundEngine_PostEventID", (void*)&wwise_AK_SoundEngine_PostEventID},
  {"AK_SoundEngine_StopAll", (void*)&wwise_AK_SoundEngine_StopAll},
  {"AK_SoundEngine_StopPlayingID", (void*)&wwise_AK_SoundEngine_StopPlayingID},
  {"AK_SoundEngine_SetRTPCValue", (void*)&wwise_AK_SoundEngine_SetRTPCValue},
  {"AK_SoundEngine_SetRTPCValueID", (void*)&wwise_AK_SoundEngine_SetRTPCValueID},
  {"AK_SoundEngine_SetState", (void*)&wwise_AK_SoundEngine_SetState},
  {"AK_SoundEngine_SetStateID", (void*)&wwise_AK_SoundEngine_SetStateID},
  {"AK_SoundEngine_SetSwitch", (void*)&wwise_AK_SoundEngine_SetSwitch},
  {"AK_SoundEngine_SetSwitchID", (void*)&wwise_AK_SoundEngine_SetSwitchID},
  {"AK_MusicEngine_Init", (void*)&wwise_AK_MusicEngine_Init},
  {"AK_MusicEngine_Term", (void*)&wwise_AK_MusicEngine_Term},
  {"AK_MusicEngine_GetDefaultInitSettings", (void*)&wwise_AK_MusicEngine_GetDefaultInitSettings},
  {"AK_SoundEngine_GetIDFromString", (void*)&wwise_AK_SoundEngine_GetIDFromString},
  {"CSharp_Init", (void*)&wwise_AK_SoundEngine_Init},
  {"CSharp_Term", (void*)&wwise_AK_SoundEngine_Term},
  {"CSharp_RenderAudio", (void*)&wwise_AK_SoundEngine_RenderAudio},
  {"CSharp_PostEvent", (void*)&wwise_AK_SoundEngine_PostEvent},
  {"CSharp_LoadBank", (void*)&wwise_AK_SoundEngine_LoadBank},
  {"CSharp_LoadBankMemoryView", (void*)&wwise_AK_SoundEngine_LoadBankMemoryView},
};

int trt_bridge_register_wwise(void) {
  static const char* const k_dlls[] = {
    "AkSoundEngine.dll",
    "AkSoundEngineP.dll",
    "AkMemoryMgr.dll",
    "AkStreamMgr.dll",
    "AkMusicEngine.dll",
    "AK::SoundEngine.dll",
  };
  for (size_t i = 0; i < sizeof(k_dlls) / sizeof(k_dlls[0]); ++i) {
    trt_module_register(k_dlls[i], TRT_ROUTE_BUILTIN, k_wwise_fns,
                        static_cast<u32>(sizeof(k_wwise_fns) / sizeof(k_wwise_fns[0])));
  }
  return 0;
}

}  // extern "C"
