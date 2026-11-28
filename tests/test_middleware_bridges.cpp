// tiramisu/tests/test_middleware_bridges.cpp
// Unit tests for expanded audio and video middleware bridges:
// Wwise, Bink Video, Miles Sound System, and CriWare.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_audio.h"
#include "../src/trt/bridge_wwise.h"
#include "../src/trt/bridge_eos.h"
#include "../src/trt/bridge_bink.h"
#include "../src/trt/bridge_miles.h"
#include "../src/trt/bridge_criware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <thread>

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
      exit(1); \
    } else { \
      printf("  PASS %s\n", msg); \
    } \
  } while (0)

extern "C" {
// Declare bridge functions with TRT_GUEST_ABI (ms_abi on Linux)
int TRT_GUEST_ABI wwise_AK_SoundEngine_Init(void* settings, void* platform);
int TRT_GUEST_ABI wwise_AK_SoundEngine_Term(void);
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_IsInitialized(void);
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_GetIDFromString(const char* str);
void* TRT_GUEST_ABI wwise_AK_MemoryMgr_Malloc(unsigned size);
void TRT_GUEST_ABI wwise_AK_MemoryMgr_Free(void* p);
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_PostEvent(const char* event_name, unsigned game_obj, unsigned flags, void* cb, void* cookie);
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_PostEventID(unsigned event_id, unsigned game_obj, unsigned flags, void* cb, void* cookie);
int TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBankMemoryView(const void* buf, unsigned size, unsigned* out_bank_id);
int TRT_GUEST_ABI wwise_AK_SoundEngine_RenderAudio(int allow_sync);
int TRT_GUEST_ABI wwise_AK_SoundEngine_SetRTPCValue(const char* name, float val, unsigned game_obj, int dur, int curve);

int TRT_GUEST_ABI eos_Initialize(const void* options);
int TRT_GUEST_ABI eos_Shutdown(void);
const char* TRT_GUEST_ABI eos_GetVersion(void);
void* TRT_GUEST_ABI eos_Platform_Create(const void* options);
void TRT_GUEST_ABI eos_Platform_Release(void* handle);
void TRT_GUEST_ABI eos_Platform_Tick(void* handle);
int TRT_GUEST_ABI eos_Platform_CheckForLauncherAndRestart(void* handle);
void* TRT_GUEST_ABI eos_Platform_GetAuthInterface(void* handle);
void* TRT_GUEST_ABI eos_Platform_GetAntiCheatClientInterface(void* handle);
void TRT_GUEST_ABI eos_Auth_Login(void* handle, const void* opts, void* client_data, void* cb);
void TRT_GUEST_ABI galaxy_Init(const void* opts);
void TRT_GUEST_ABI galaxy_Shutdown(void);
int TRT_GUEST_ABI galaxy_IsSignedIn(void);

struct BinkHeader {
  unsigned int Width;
  unsigned int Height;
  unsigned int Frames;
  unsigned int FrameNum;
  unsigned int LastFrameNum;
  unsigned int FrameRate;
  unsigned int FrameRateDiv;
  unsigned int ReadError;
  unsigned int OpenFlags;
  unsigned int BinkType;
  unsigned int Size;
  unsigned int FrameSize;
  unsigned int SndSize;
  unsigned int NumTracks;
};
void* TRT_GUEST_ABI bink_BinkOpen(const void* file, unsigned flags);
void* TRT_GUEST_ABI bink_BinkOpenDirect(const char* name, unsigned flags);
int TRT_GUEST_ABI bink_BinkClose(void* bnk);
int TRT_GUEST_ABI bink_BinkDoFrame(void* bnk);
int TRT_GUEST_ABI bink_BinkNextFrame(void* bnk);
int TRT_GUEST_ABI bink_BinkWait(void* bnk);
int TRT_GUEST_ABI bink_BinkShouldSkip(void* bnk);
unsigned TRT_GUEST_ABI bink_BinkGetFrameNum(void* bnk);
int TRT_GUEST_ABI bink_BinkGoto(void* bnk, unsigned frame, int flags);
int TRT_GUEST_ABI bink_BinkCopyToBuffer(void* bnk, void* buf, int pitch, int desth, int destx, int desty, unsigned flags);

int TRT_GUEST_ABI miles_AIL_startup(void);
int TRT_GUEST_ABI miles_AIL_shutdown(void);
void* TRT_GUEST_ABI miles_AIL_allocate_sample_handle(void* dig);
int TRT_GUEST_ABI miles_AIL_free_sample_handle(void* sam);
void* TRT_GUEST_ABI miles_AIL_open_stream(void* dig, const char* filename, int stream_flags);
int TRT_GUEST_ABI miles_AIL_sample_status(void* sam);
int TRT_GUEST_ABI miles_AIL_load_sample_buffer(void* sam, unsigned index, const void* data, unsigned len);
int TRT_GUEST_ABI miles_AIL_start_sample(void* sam);

int TRT_GUEST_ABI cri_CriAtom_Initialize(const void* config);
int TRT_GUEST_ABI cri_CriAtom_Finalize(void);
void* TRT_GUEST_ABI cri_criAtomExPlayer_Create(const void* config, void* work, unsigned work_size);
int TRT_GUEST_ABI cri_criAtomExPlayer_Destroy(void* player);
int TRT_GUEST_ABI cri_CriMana_Initialize(const void* config);
int TRT_GUEST_ABI cri_CriMana_Finalize(void);
void* TRT_GUEST_ABI cri_criManaPlayer_Create(const void* config, void* work, unsigned work_size);

// DirectStorage
int trt_bridge_register_dstorage(void);
long TRT_GUEST_ABI dstorage_DStorageGetFactory(const void* riid, void** ppv);
long TRT_GUEST_ABI dstorage_DStorageSetConfiguration(const void* config);
long TRT_GUEST_ABI dstorage_DStorageSetConfiguration1(const void* config);

// D3D9 PERF
int trt_bridge_register_d3d9(void);
void TRT_GUEST_ABI trt_D3DPERF_SetOptions(unsigned int opt);
int TRT_GUEST_ABI trt_D3DPERF_QueryRepeatFrame(void);
unsigned int TRT_GUEST_ABI trt_D3DPERF_GetStatus(void* dev);
}

int main() {
  printf("test_middleware_bridges: audio & video middleware bridges\n");

  // 1. Wwise tests
  printf("-- Wwise (AK::SoundEngine)\n");
  CHECK(trt_bridge_register_wwise() == 0, "trt_bridge_register_wwise succeeds");
  CHECK(wwise_AK_SoundEngine_Init(nullptr, nullptr) == 0, "wwise Init succeeds");
  CHECK(wwise_AK_SoundEngine_IsInitialized() == 1, "wwise IsInitialized returns 1");

  // Memory manager
  void* mem = wwise_AK_MemoryMgr_Malloc(256);
  CHECK(mem != nullptr, "wwise MemoryMgr_Malloc returns valid memory");
  memset(mem, 0xAA, 256);
  CHECK(((unsigned char*)mem)[0] == 0xAA && ((unsigned char*)mem)[255] == 0xAA,
        "wwise allocated memory is writable");
  wwise_AK_MemoryMgr_Free(mem);
  CHECK(true, "wwise MemoryMgr_Free succeeds without fault");

  // FNV-1a hash
  unsigned hash_null = wwise_AK_SoundEngine_GetIDFromString(nullptr);
  unsigned hash_empty = wwise_AK_SoundEngine_GetIDFromString("");
  CHECK(hash_null == 0 && hash_empty == 0, "GetIDFromString returns 0 for null/empty");

  unsigned hash1 = wwise_AK_SoundEngine_GetIDFromString("Play_Footstep");
  unsigned hash2 = wwise_AK_SoundEngine_GetIDFromString("play_footstep");
  unsigned hash3 = wwise_AK_SoundEngine_GetIDFromString("Stop_Music");
  CHECK(hash1 != 0, "GetIDFromString returns non-zero hash for valid string");
  CHECK(hash1 == hash2, "GetIDFromString is case-insensitive (Wwise convention)");
  CHECK(hash1 != hash3, "GetIDFromString generates distinct hashes for distinct events");

  // PostEvent
  unsigned play_id1 = wwise_AK_SoundEngine_PostEvent("Play_BGM", 1, 0, nullptr, nullptr);
  unsigned play_id2 = wwise_AK_SoundEngine_PostEvent("Play_SFX", 1, 0, nullptr, nullptr);
  CHECK(play_id1 != 0 && play_id2 != 0, "PostEvent returns valid non-zero playing ID");
  CHECK(play_id1 != play_id2, "PostEvent returns unique playing IDs");

  unsigned play_id3 = wwise_AK_SoundEngine_PostEventID(hash1, 1, 0, nullptr, nullptr);
  CHECK(play_id3 != 0, "PostEventID returns valid non-zero playing ID");

  // Real binary Wwise .bnk (BKHD + DIDX + DATA + HIRC) -> PCM submission test
  {
    static int wwise_cap_calls = 0;
    static u32 wwise_cap_bytes = 0, wwise_cap_rate = 0, wwise_cap_ch = 0;
    trt_audio_set_voice_hook(
        [](u64, const void*, u32 bytes, u32 rate, u32 ch, u32, u32, float, void*) -> int {
          ++wwise_cap_calls;
          wwise_cap_bytes = bytes;
          wwise_cap_rate = rate;
          wwise_cap_ch = ch;
          return 0;
        },
        nullptr);

    // Build a 16-bit mono WAV payload (128 frames @ 44100 Hz)
    u8 wav[44 + 256] = {};
    memcpy(wav, "RIFF", 4);
    u32 rsz = 36 + 256; memcpy(wav + 4, &rsz, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    u32 fsz = 16; memcpy(wav + 16, &fsz, 4);
    u16 tag = 1, ch = 1, align = 2, bits = 16;
    u32 rate = 44100, br = 88200;
    memcpy(wav + 20, &tag, 2); memcpy(wav + 22, &ch, 2);
    memcpy(wav + 24, &rate, 4); memcpy(wav + 28, &br, 4);
    memcpy(wav + 32, &align, 2); memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    u32 dsz = 256; memcpy(wav + 40, &dsz, 4);
    for (int i = 0; i < 128; ++i) {
      short s = (short)(i * 120);
      memcpy(wav + 44 + i * 2, &s, 2);
    }

    // Build .bnk: BKHD + DIDX(media=9001) + DATA(wav) + HIRC(Sound 7001->9001, Action 8001->Play 7001, Event hash("Play_WwiseTest")->[8001])
    u8 bnk[1024] = {};
    size_t bp = 0;
    auto push4 = [&](const char* s) { memcpy(bnk + bp, s, 4); bp += 4; };
    auto push_u32 = [&](u32 v) { memcpy(bnk + bp, &v, 4); bp += 4; };
    auto push_u8 = [&](u8 v) { bnk[bp++] = v; };

    push4("BKHD"); push_u32(8); push_u32(134); push_u32(4242);
    push4("DIDX"); push_u32(12); push_u32(9001); push_u32(0); push_u32(sizeof(wav));
    push4("DATA"); push_u32(sizeof(wav)); memcpy(bnk + bp, wav, sizeof(wav)); bp += sizeof(wav);

    // HIRC chunk: 3 objects
    // Obj 1 (Sound, type=2): obj_sz = 4(id) + 9(body) = 13
    // Obj 2 (Action, type=3): obj_sz = 4(id) + 6(body) = 10
    // Obj 3 (Event, type=4): obj_sz = 4(id) + 5(u8 count=1 + u32 action_id) = 9
    const u32 hirc_sz = 4 + (5 + 13) + (5 + 10) + (5 + 9);
    const u32 ev_id = wwise_AK_SoundEngine_GetIDFromString("Play_WwiseTest");
    push4("HIRC"); push_u32(hirc_sz); push_u32(3);
    push_u8(2); push_u32(13); push_u32(7001); push_u32(0x00010001u); push_u8(0); push_u32(9001);
    push_u8(3); push_u32(10); push_u32(8001); push_u8(2); push_u8(4 /*Play*/); push_u32(7001);
    push_u8(4); push_u32(9); push_u32(ev_id); push_u8(1); push_u32(8001);

    u32 loaded_bid = 0;
    CHECK(wwise_AK_SoundEngine_LoadBankMemoryView(bnk, (u32)bp, &loaded_bid) == 0 && loaded_bid == 4242,
          "Wwise LoadBankMemoryView parses BKHD/DIDX/DATA/HIRC");
    CHECK(wwise_AK_SoundEngine_PostEvent("Play_WwiseTest", 10, 0, nullptr, nullptr) != 0,
          "Wwise PostEvent resolves HIRC Event->Action->Sound->Media");
    CHECK(wwise_AK_SoundEngine_RenderAudio(1) == 0, "Wwise RenderAudio flushes voices");
    CHECK(wwise_cap_calls == 1 && wwise_cap_bytes == 256 && wwise_cap_rate == 44100 && wwise_cap_ch == 1,
          "Wwise decoded embedded .wem WAV and submitted PCM to host mixer");

    // HIRC RanSeqCntr (type=5) container + RTPC MasterVolume scaling check
    static float wwise_cap_vol = 0.0f;
    trt_audio_set_voice_hook(
        [](u64, const void*, u32 bytes, u32, u32, u32, u32, float vol, void*) -> int {
          ++wwise_cap_calls;
          wwise_cap_bytes = bytes;
          wwise_cap_vol = vol;
          return 0;
        },
        nullptr);
    u8 bnk2[1024] = {};
    size_t bp2 = 0;
    auto p4 = [&](const char* s) { memcpy(bnk2 + bp2, s, 4); bp2 += 4; };
    auto p32 = [&](u32 v) { memcpy(bnk2 + bp2, &v, 4); bp2 += 4; };
    auto p8 = [&](u8 v) { bnk2[bp2++] = v; };
    p4("BKHD"); p32(8); p32(134); p32(4243);
    p4("DIDX"); p32(12); p32(9002); p32(0); p32(sizeof(wav));
    p4("DATA"); p32(sizeof(wav)); memcpy(bnk2 + bp2, wav, sizeof(wav)); bp2 += sizeof(wav);
    // HIRC: Sound 7002->9002, RanSeqCntr 6001->[7002], Action 8002->Play 6001, Event Play_Cntr->[8002]
    const u32 ev2 = wwise_AK_SoundEngine_GetIDFromString("Play_Cntr");
    const u32 hsz2 = 4 + (5 + 13) + (5 + 12) + (5 + 10) + (5 + 9);
    p4("HIRC"); p32(hsz2); p32(4);
    p8(2); p32(13); p32(7002); p32(0x00010001u); p8(0); p32(9002);
    p8(5); p32(12); p32(6001); p32(1); p32(7002);
    p8(3); p32(10); p32(8002); p8(2); p8(4); p32(6001);
    p8(4); p32(9); p32(ev2); p8(1); p32(8002);
    u32 bid2 = 0;
    CHECK(wwise_AK_SoundEngine_LoadBankMemoryView(bnk2, (u32)bp2, &bid2) == 0 && bid2 == 4243,
          "Wwise parses HIRC RanSeqCntr container bank");
    wwise_AK_SoundEngine_SetRTPCValue("MasterVolume", 50.0f, 0, 0, 0);
    wwise_AK_SoundEngine_PostEvent("Play_Cntr", 10, 0, nullptr, nullptr);
    wwise_AK_SoundEngine_RenderAudio(1);
    CHECK(wwise_cap_calls == 2 && wwise_cap_bytes == 256 &&
              wwise_cap_vol > 0.49f && wwise_cap_vol < 0.51f,
          "Wwise resolves Event->Action->RanSeqCntr->Sound->Media and applies MasterVolume RTPC (50% -> 0.5)");
    wwise_AK_SoundEngine_SetRTPCValue("MasterVolume", 100.0f, 0, 0, 0);
    trt_audio_set_voice_hook(nullptr, nullptr);
  }

  CHECK(wwise_AK_SoundEngine_Term() == 0, "wwise Term succeeds");

  // 2. Bink Video tests
  printf("-- Bink 2 Video\n");
  CHECK(trt_bridge_register_bink() == 0, "trt_bridge_register_bink succeeds");
  void* bnk = bink_BinkOpen("fake.bik", 0);
  CHECK(bnk != nullptr, "BinkOpen returns non-null dummy structure");
  BinkHeader* hdr = (BinkHeader*)bnk;
  CHECK(hdr->Width == 1920 && hdr->Height == 1080, "Bink dummy header has 1080p resolution");
  CHECK(hdr->Frames >= 1 && hdr->FrameNum == 1, "Bink dummy header indicates frame ready");

  CHECK(bink_BinkShouldSkip(bnk) == 1, "BinkShouldSkip indicates skip to main menu");
  CHECK(bink_BinkDoFrame(bnk) == 0, "BinkDoFrame succeeds");
  CHECK(bink_BinkNextFrame(bnk) == 0, "BinkNextFrame succeeds");
  CHECK(bink_BinkWait(bnk) == 0, "BinkWait returns ready (0)");
  CHECK(bink_BinkGetFrameNum(bnk) == 1, "BinkGetFrameNum returns 1");
  // Verify BinkGoto + BinkNextFrame anti-softlock progression & buffer zeroing
  bink_BinkGoto(bnk, 0, 0);
  CHECK(bink_BinkGetFrameNum(bnk) == 0, "BinkGoto(0) resets FrameNum to 0");
  bink_BinkNextFrame(bnk);
  CHECK(bink_BinkGetFrameNum(bnk) == 1, "BinkNextFrame advances 0 -> 1 so cutscene loops never hang");
  unsigned char bink_fb[64];
  memset(bink_fb, 0xAA, sizeof(bink_fb));
  bink_BinkCopyToBuffer(bnk, bink_fb, 16, 4, 0, 0, 0);
  CHECK(bink_fb[0] == 0 && bink_fb[63] == 0, "BinkCopyToBuffer clears target buffer cleanly to black");
  CHECK(bink_BinkClose(bnk) == 0, "BinkClose succeeds");

  void* bnk_dir = bink_BinkOpenDirect("intro.bik", 0);
  CHECK(bnk_dir != nullptr, "BinkOpenDirect returns non-null dummy structure");
  CHECK(bink_BinkClose(bnk_dir) == 0, "BinkClose succeeds for direct open");

  // 3. Miles Sound System tests
  printf("-- Miles Sound System\n");
  CHECK(trt_bridge_register_miles() == 0, "trt_bridge_register_miles succeeds");
  CHECK(miles_AIL_startup() == 1, "AIL_startup returns success (1)");
  void* sample = miles_AIL_allocate_sample_handle(nullptr);
  CHECK(sample != nullptr, "AIL_allocate_sample_handle returns valid non-null handle");
  CHECK(miles_AIL_sample_status(sample) == 0, "AIL_sample_status returns done (0)");
  {
    static int miles_calls = 0;
    static u32 miles_bytes = 0;
    trt_audio_set_voice_hook(
        [](u64, const void*, u32 bytes, u32, u32, u32, u32, float, void*) -> int {
          ++miles_calls;
          miles_bytes = bytes;
          return 0;
        },
        nullptr);
    short pcm[64];
    for (int i = 0; i < 64; ++i) pcm[i] = (short)(i * 100);
    miles_AIL_load_sample_buffer(sample, 0, pcm, sizeof(pcm));
    miles_AIL_start_sample(sample);
    CHECK(miles_calls == 1 && miles_bytes == sizeof(pcm) && miles_AIL_sample_status(sample) == 0,
          "Miles AIL_load_sample_buffer + AIL_start_sample submits PCM and marks SMP_DONE");
    trt_audio_set_voice_hook(nullptr, nullptr);
  }
  CHECK(miles_AIL_free_sample_handle(sample) == 1, "AIL_free_sample_handle succeeds");

  void* stream = miles_AIL_open_stream(nullptr, "music.wav", 0);
  CHECK(stream != nullptr, "AIL_open_stream returns valid non-null handle");
  CHECK(miles_AIL_shutdown() == 1, "AIL_shutdown returns success (1)");

  // 4. CriWare tests
  printf("-- CriWare (Atom + Mana)\n");
  CHECK(trt_bridge_register_criware() == 0, "trt_bridge_register_criware succeeds");
  CHECK(cri_CriAtom_Initialize(nullptr) == 0, "CriAtom_Initialize succeeds");
  void* atom_player = cri_criAtomExPlayer_Create(nullptr, nullptr, 0);
  CHECK(atom_player != nullptr, "criAtomExPlayer_Create returns valid non-null handle");
  CHECK(cri_criAtomExPlayer_Destroy(atom_player) == 0, "criAtomExPlayer_Destroy succeeds");
  CHECK(cri_CriAtom_Finalize() == 0, "CriAtom_Finalize succeeds");

  CHECK(cri_CriMana_Initialize(nullptr) == 0, "CriMana_Initialize succeeds");
  void* mana_player = cri_criManaPlayer_Create(nullptr, nullptr, 0);
  CHECK(mana_player != nullptr, "criManaPlayer_Create returns valid non-null handle");
  CHECK(cri_CriMana_Finalize() == 0, "CriMana_Finalize succeeds");

  // 5. DirectStorage (real async IDStorageFactory / File / Queue / StatusArray)
  printf("-- DirectStorage\n");
  CHECK(trt_bridge_register_dstorage() == 0, "trt_bridge_register_dstorage succeeds");
  CHECK(dstorage_DStorageSetConfiguration(nullptr) == 0, "DStorageSetConfiguration succeeds");
  CHECK(dstorage_DStorageSetConfiguration1(nullptr) == 0, "DStorageSetConfiguration1 succeeds");
  void* factory = (void*)0x1234;
  CHECK(dstorage_DStorageGetFactory(nullptr, &factory) != 0 && factory == nullptr,
        "DStorageGetFactory rejects null IID cleanly and nulls ppv");

  {
    struct test_guid { unsigned d1; unsigned short d2, d3; unsigned char d4[8]; };
    static constexpr test_guid k_factory_iid = {0x6924ea0cu, 0xc3cdu, 0x4826u, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
    static constexpr test_guid k_file_iid = {0x5de95e7bu, 0x955au, 0x4868u, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
    static constexpr test_guid k_status_iid = {0x82397587u, 0x7cd5u, 0x453bu, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
    static constexpr test_guid k_queue1_iid = {0xdd2f482cu, 0x5effu, 0x41e8u, {0x9c, 0x9e, 0xd2, 0x37, 0x4b, 0x27, 0x81, 0x28}};

    CHECK(dstorage_DStorageGetFactory(&k_factory_iid, &factory) == 0 && factory != nullptr,
          "DStorageGetFactory returns IDStorageFactory");

    char tmpl[] = "/tmp/trt_dstorage_XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0, "created temp test payload");
    const char payload[] = "DIRECTSTORAGE_TIRAMISU_PAYLOAD_123456";
    CHECK(write(fd, payload, sizeof(payload) - 1) == (ssize_t)(sizeof(payload) - 1),
          "wrote test payload");
    close(fd);

    u16 wpath[256] = {};
    for (size_t i = 0; tmpl[i] && i + 1 < 256; ++i) wpath[i] = (u16)(unsigned char)tmpl[i];

    auto** fvt = *(void***)factory;
    typedef long(TRT_GUEST_ABI* create_queue_fn)(void*, const void*, const void*, void**);
    typedef long(TRT_GUEST_ABI* open_file_fn)(void*, const u16*, const void*, void**);
    typedef long(TRT_GUEST_ABI* create_status_fn)(void*, u32, const char*, const void*, void**);
    typedef u32(TRT_GUEST_ABI* release_fn)(void*);

    void* file = nullptr;
    CHECK(((open_file_fn)fvt[4])(factory, wpath, &k_file_iid, &file) == 0 && file != nullptr,
          "IDStorageFactory::OpenFile opens real file");

    void* status = nullptr;
    CHECK(((create_status_fn)fvt[5])(factory, 4, "test_status", &k_status_iid, &status) == 0 &&
              status != nullptr,
          "IDStorageFactory::CreateStatusArray creates status array");

    struct { u64 source_type; u16 capacity; int8_t priority; const char* name; void* device; } qdesc = {
        0, 0x80u, 0, "test_q", nullptr};
    void* queue = nullptr;
    CHECK(((create_queue_fn)fvt[3])(factory, &qdesc, &k_queue1_iid, &queue) == 0 && queue != nullptr,
          "IDStorageFactory::CreateQueue creates async IDStorageQueue1");

    u8 dst[64] = {};
    struct {
      u8 options[16];
      u8 source[24];
      u8 destination[40];
      u32 uncompressed_size;
      u64 cancellation_tag;
      const char* name;
    } req = {};
    u32 read_offset_sz = (u32)(sizeof(payload) - 1);
    memcpy(req.source + 0, &file, 8);
    u64 off0 = 0;
    memcpy(req.source + 8, &off0, 8);
    memcpy(req.source + 16, &read_offset_sz, 4);
    void* dst_ptr = dst;
    memcpy(req.destination + 0, &dst_ptr, 8);
    memcpy(req.destination + 8, &read_offset_sz, 4);
    req.uncompressed_size = read_offset_sz;

    auto** qvt = *(void***)queue;
    typedef void(TRT_GUEST_ABI* enqueue_req_fn)(void*, const void*);
    typedef void(TRT_GUEST_ABI* enqueue_stat_fn)(void*, void*, u32);
    typedef void(TRT_GUEST_ABI* submit_fn)(void*);
    typedef void(TRT_GUEST_ABI* close_fn)(void*);
    ((enqueue_req_fn)qvt[3])(queue, &req);
    ((enqueue_stat_fn)qvt[4])(queue, status, 0);
    ((submit_fn)qvt[6])(queue);

    auto** svt = *(void***)status;
    typedef u8(TRT_GUEST_ABI* is_complete_fn)(void*, u32);
    typedef long(TRT_GUEST_ABI* get_hr_fn)(void*, u32);
    bool done = false;
    for (int i = 0; i < 200; ++i) {
      if (((is_complete_fn)svt[3])(status, 0)) {
        done = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done && ((get_hr_fn)svt[4])(status, 0) == 0,
          "IDStorageQueue async read completed with S_OK");
    CHECK(memcmp(dst, payload, sizeof(payload) - 1) == 0,
          "DirectStorage read transferred exact file bytes into memory");

    ((close_fn)qvt[8])(queue);
    ((release_fn)qvt[2])(queue);
    ((release_fn)svt[2])(status);
    auto** filevt = *(void***)file;
    ((close_fn)filevt[3])(file);
    ((release_fn)filevt[2])(file);
    ((release_fn)fvt[2])(factory);
    unlink(tmpl);
  }

  // 6. D3D9 PERF
  printf("-- D3D9 PERF\n");
  CHECK(trt_bridge_register_d3d9() == 0, "trt_bridge_register_d3d9 succeeds");
  trt_D3DPERF_SetOptions(1);
  CHECK(trt_D3DPERF_QueryRepeatFrame() == 0, "D3DPERF_QueryRepeatFrame returns 0");
  CHECK(trt_D3DPERF_GetStatus(nullptr) == 0, "D3DPERF_GetStatus returns 0");

  // 7. Epic Online Services (EOS) & GOG Galaxy (honest offline-safe)
  printf("-- Epic Online Services & GOG Galaxy\n");
  CHECK(trt_bridge_register_eos() == 0, "trt_bridge_register_eos succeeds");
  CHECK(eos_Initialize(nullptr) == 0, "EOS_Initialize succeeds");
  CHECK(eos_GetVersion() != nullptr && strstr(eos_GetVersion(), "tiramisu") != nullptr,
        "EOS_GetVersion reports version string");
  void* eos_plat = eos_Platform_Create(nullptr);
  CHECK(eos_plat != nullptr, "EOS_Platform_Create returns handle");
  CHECK(eos_Platform_CheckForLauncherAndRestart(eos_plat) == 24 /*EOS_NoChange*/,
        "EOS_Platform_CheckForLauncherAndRestart returns EOS_NoChange (does not exit game)");
  void* eos_auth = eos_Platform_GetAuthInterface(eos_plat);
  CHECK(eos_auth != nullptr, "EOS_Platform_GetAuthInterface returns handle");
  CHECK(eos_Platform_GetAntiCheatClientInterface(eos_plat) == nullptr,
        "EOS_Platform_GetAntiCheatClientInterface honestly returns NULL (no AC bypass)");
  {
    static int eos_cb_res = -1;
    static void* eos_cb_cd = nullptr;
    struct cb_info { int res; int pad; void* cd; };
    auto cb = +[](const void* p) TRT_GUEST_ABI {
      const auto* ci = static_cast<const cb_info*>(p);
      eos_cb_res = ci->res;
      eos_cb_cd = ci->cd;
    };
    int marker = 77;
    eos_Auth_Login(eos_auth, nullptr, &marker, (void*)cb);
    eos_Platform_Tick(eos_plat);
    CHECK(eos_cb_res == 1 /*EOS_NoConnection*/ && eos_cb_cd == &marker,
          "EOS_Auth_Login + Tick delivers honest EOS_NoConnection callback");
  }
  eos_Platform_Release(eos_plat);
  CHECK(eos_Shutdown() == 0, "EOS_Shutdown succeeds");
  galaxy_Init(nullptr);
  CHECK(galaxy_IsSignedIn() == 0, "GOG Galaxy reports offline (not signed in)");
  galaxy_Shutdown();

  printf("RESULT: ALL PASS\n");
  return 0;
}
