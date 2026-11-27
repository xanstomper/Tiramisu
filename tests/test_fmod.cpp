// tiramisu/tests/test_fmod.cpp
// FMOD bridge gate: the lifecycle a game exercises (create -> init ->
// createSound -> play -> channel control -> release), with a REAL wav
// decoded through the VFS and submitted to the mixer hook, plus the
// compressed named-silent path and the handle-model edge cases.
#include "../src/trt/bridge_fmod.h"
#include "../src/trt/bridge_audio.h"
#include "../src/trt/trt.h"

// kernel32 file surface (bridge_kernel32.cpp) for staging the test wav
extern "C" {
void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access, u32 share,
                                  void* sa, u32 disposition, u32 flags,
                                  void* tmpl);
int TRT_STDCALL trt_WriteFile(void* f, const void* buf, u32 bytes, u32* written,
                              void* overlapped);
int TRT_STDCALL trt_CloseHandle(void* h);
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name)                                     \
  do {                                                        \
    if (cond) {                                               \
      printf("  PASS %s\n", name);                            \
    } else {                                                  \
      printf("  FAIL %s\n", name);                            \
      ++failures;                                             \
    }                                                         \
  } while (0)

// ---- mixer capture hook: records every submit ----
struct capture {
  u64 voice_id = 0;
  u32 bytes = 0, rate = 0, channels = 0, bps = 0, is_float = 0;
  float volume = 0.f;
  int count = 0;
} g_cap;

int cap_submit(u64 voice_id, const void* pcm, u32 bytes, u32 rate,
               u32 channels, u32 bytes_per_sample, u32 is_float, float volume,
               void* user) {
  (void)pcm;
  (void)user;
  g_cap.voice_id = voice_id;
  g_cap.bytes = bytes;
  g_cap.rate = rate;
  g_cap.channels = channels;
  g_cap.bps = bytes_per_sample;
  g_cap.is_float = is_float;
  g_cap.volume = volume;
  ++g_cap.count;
  return 0;
}

// ---- build a real 16-bit stereo WAV in memory (440Hz, 0.5s @ 44.1k) ----
static u8 g_wav[44100 * 4 + 64];
static u32 g_wav_size = 0;

void make_wav(void) {
  const u32 rate = 44100, frames = 22050, channels = 2;
  u8* p = g_wav;
  memcpy(p, "RIFF", 4); p += 4;
  u32 riff_sz = 36 + frames * channels * 2;
  memcpy(p, &riff_sz, 4); p += 4;
  memcpy(p, "WAVEfmt ", 8); p += 8;
  u32 fmt_sz = 16; memcpy(p, &fmt_sz, 4); p += 4;
  u16 tag = 1, ch = (u16)channels; u32 rr = rate;
  u16 bits = 16;
  u32 byte_rate = rate * channels * 2; u16 align = (u16)(channels * 2);
  memcpy(p, &tag, 2); p += 2;
  memcpy(p, &ch, 2); p += 2;
  memcpy(p, &rr, 4); p += 4;
  memcpy(p, &byte_rate, 4); p += 4;
  memcpy(p, &align, 2); p += 2;
  memcpy(p, &bits, 2); p += 2;
  memcpy(p, "data", 4); p += 4;
  u32 data_sz = frames * channels * 2;
  memcpy(p, &data_sz, 4); p += 4;
  for (u32 i = 0; i < frames; ++i) {
    for (u32 c = 0; c < channels; ++c) {
      // 440 Hz sine, signed 16-bit
      double t = (double)i / (double)rate;
      double v = 0.4 * 32767.0 * __builtin_sin(2.0 * 3.141592653589793 * 440.0 * t);
      short s = (short)v;
      memcpy(p, &s, 2); p += 2;
    }
  }
  g_wav_size = (u32)(p - g_wav);
}

// stage the wav into the VFS as c:\fmod_test\tone.wav via the runtime's own
// file surface (CreateFileW), so the bridge's VFS path is exercised too.
static void stage_wav(void) {
  u16 path[64];
  const char* p8 = "c:\\fmod_test\\tone.wav";
  u32 n = 0;
  for (; p8[n]; ++n) path[n] = (u16)(u8)p8[n];
  path[n] = 0;
  void* f = trt_CreateFileW(path, 0x40000000 /*GENERIC_WRITE*/, 0, nullptr,
                            2 /*CREATE_ALWAYS*/, 0x80, nullptr);
  if (f) {
    u32 written = 0;
    trt_WriteFile(f, g_wav, g_wav_size, &written, nullptr);
    trt_CloseHandle(f);
  }
}

int main(void) {
  printf("=== test_fmod: FMOD bridge lifecycle ===\n");
  trt_backend be{};  // default backend: host alloc/protect
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_fmod();
  trt_audio_set_voice_hook(&cap_submit, nullptr);
  // mount a scratch c: like the corpus runner does (the VFS needs the drive)
  char tmpl[] = "/tmp/tfm-XXXXXX";
  char* tmpdir = mkdtemp(tmpl);
  if (tmpdir) trt_vfs_mount("c:", tmpdir);
  make_wav();
  stage_wav();

  void* sys = nullptr;
  CHECK(fmod_System_Create(nullptr, &sys) == 0 && sys,
        "FMOD_System_Create -> handle");
  CHECK(fmod_System_Init(sys, 64, 0, nullptr) == 0,
        "FMOD_System_Init -> FMOD_OK");
  CHECK(fmod_System_SetOutput(sys, 3) == 0 &&
            fmod_System_SetSpeakerMode(sys, 1) == 0 &&
            fmod_System_SetDSPBufferSize(sys, 512, 4) == 0,
        "configuration setters accepted");

  // --- real wav through the VFS ---
  void* snd = nullptr;
  CHECK(fmod_System_CreateSound(sys, "c:\\fmod_test\\tone.wav", 0x00000002u,
                                    nullptr, &snd) == 0 &&
            snd != nullptr,
        "CreateSound(wav path via VFS) -> handle");
  u32 len = 0;
  CHECK(fmod_Sound_GetLength(snd, &len, 1) == 0 && len >= 480 && len <= 560,
        "Sound_GetLength(ms) decodes to ~500ms");
  void* ch = nullptr;
  CHECK(fmod_System_PlaySound(sys, snd, nullptr, 0, &ch) == 0 && ch,
        "PlaySound -> channel");
  CHECK(g_cap.count == 1 && g_cap.bytes == 22050u * 4u && g_cap.rate == 44100 &&
            g_cap.channels == 2 && g_cap.bps == 2,
        "mixer received the decoded PCM (22050 frames, 44.1k stereo s16)");
  float vol = 0.f;
  CHECK(fmod_Channel_SetVolume(ch, 0.5f) == 0 &&
            fmod_Channel_GetVolume(ch, &vol) == 0 && vol == 0.5f,
        "channel volume set/get");
  int playing = 0;
  CHECK(fmod_Channel_IsPlaying(ch, &playing) == 0 && playing == 1,
        "channel IsPlaying -> true");
  CHECK(fmod_Channel_Stop(ch) == 0, "Channel_Stop");
  CHECK(fmod_Channel_IsPlaying(ch, &playing) == 0 && playing == 0,
        "channel stopped");

  // --- looping sound: play -> feeder re-pushes ---
  u32 loop_mode = 0x00000002u;  // FMOD_LOOP_NORMAL
  CHECK(fmod_Sound_SetMode(snd, loop_mode) == 0, "Sound_SetMode(LOOP_NORMAL)");
  void* ch2 = nullptr;
  CHECK(fmod_System_PlaySound(sys, snd, nullptr, 0, &ch2) == 0 && ch2,
        "looping PlaySound -> channel");
  CHECK(g_cap.count >= 2, "feeder re-pushed the loop");
  fmod_Channel_Stop(ch2);

  // --- real MP3: fixture decoded through the bridge, PCM on the mixer ---
  {
    const char* paths[] = {"tests/fixtures/audio/sine22050.mp3",
                           "../tests/fixtures/audio/sine22050.mp3"};
    FILE* mf = nullptr;
    for (int i = 0; i < 2 && !mf; ++i) mf = fopen(paths[i], "rb");
    if (!mf) {
      printf("  FAIL mp3 fixture missing (tests/fixtures/audio/sine22050.mp3)\n");
      ++failures;
    } else {
      static u8 mp3buf[64 * 1024];
      const size_t got = fread(mp3buf, 1, sizeof(mp3buf), mf);
      fclose(mf);
      void* msnd = nullptr;
      u32 mex[2] = {64, (u32)got};
      CHECK(got > 0 &&
                fmod_System_CreateSound(sys, mp3buf, 0x800u /*OPENMEMORY*/, mex,
                                        &msnd) == 0 &&
                msnd != nullptr,
            "CreateSound(mp3 memory) -> handle");
      const u32 pre_mp3 = g_cap.count;
      void* mch = nullptr;
      CHECK(fmod_System_PlaySound(sys, msnd, nullptr, 0, &mch) == 0 && mch,
            "mp3 sound plays a real channel");
      CHECK(g_cap.count > pre_mp3 && g_cap.bytes > 0 && g_cap.rate == 22050 &&
                g_cap.channels == 1 && g_cap.bps == 2,
            "mp3 decoded to PCM (22050 Hz mono s16, real audio)");
      fmod_Channel_Stop(mch);
      fmod_Sound_Release(msnd);
    }
  }

  // --- real OGG/Vorbis: ffmpeg-generated fixture, decoded by stb_vorbis ---
  {
    const char* paths[] = {"tests/fixtures/audio/sine22050.ogg",
                           "../tests/fixtures/audio/sine22050.ogg"};
    FILE* of = nullptr;
    for (int i = 0; i < 2 && !of; ++i) of = fopen(paths[i], "rb");
    if (!of) {
      printf("  FAIL ogg fixture missing (tests/fixtures/audio/sine22050.ogg)\n");
      ++failures;
    } else {
      static u8 oggbuf[64 * 1024];
      const size_t oggot = fread(oggbuf, 1, sizeof(oggbuf), of);
      fclose(of);
      void* osnd = nullptr;
      u32 oex[2] = {64, (u32)oggot};
      CHECK(oggot > 0 &&
                fmod_System_CreateSound(sys, oggbuf, 0x800u /*OPENMEMORY*/, oex,
                                        &osnd) == 0 &&
                osnd != nullptr,
            "CreateSound(ogg memory) -> handle");
      const u32 pre_ogg2 = g_cap.count;
      void* och = nullptr;
      CHECK(fmod_System_PlaySound(sys, osnd, nullptr, 0, &och) == 0 && och,
            "ogg sound plays a real channel");
      CHECK(g_cap.count > pre_ogg2 && g_cap.bytes > 0 &&
                g_cap.rate == 22050 && g_cap.channels == 1 && g_cap.bps == 2,
            "ogg decoded to PCM via stb_vorbis (22050 Hz mono s16)");
      fmod_Channel_Stop(och);
      fmod_Sound_Release(osnd);
    }
  }

  // --- real FSB5 FADPCM & PCM16 containers: decoded via audio_codecs ---
  {
    // Build a valid 60-byte FSB5 header + 8-byte sample header + 140-byte FADPCM frame (256 samples, mono 44.1kHz)
    static u8 fsb[60 + 8 + 140];
    memset(fsb, 0, sizeof(fsb));
    memcpy(fsb, "FSB5", 4);
    u32 ver = 1, num_s = 1, shdr_sz = 8, name_sz = 0, data_sz = 140, mode_fadpcm = 14;
    memcpy(fsb + 4, &ver, 4);
    memcpy(fsb + 8, &num_s, 4);
    memcpy(fsb + 12, &shdr_sz, 4);
    memcpy(fsb + 16, &name_sz, 4);
    memcpy(fsb + 20, &data_sz, 4);
    memcpy(fsb + 24, &mode_fadpcm, 4);
    // sample header: next=0, freq_idx=8 (44100Hz), ch=0 (mono), data_off=0, samples=256
    u64 sh0 = (8ull << 1) | (256ull << 34);
    memcpy(fsb + 60, &sh0, 8);
    // FADPCM block: coefs=0x11111111 (idx 1 -> coef1=60), shifts=0x44444444, hist1=1200, hist2=600, alternating nibbles
    u8* blk = fsb + 68;
    u32 coefs = 0x11111111u, shifts = 0x44444444u;
    short h1 = 1200, h2 = 600;
    memcpy(blk + 0, &coefs, 4);
    memcpy(blk + 4, &shifts, 4);
    memcpy(blk + 8, &h1, 2);
    memcpy(blk + 10, &h2, 2);
    for (int i = 12; i < 140; ++i) blk[i] = (u8)((i & 1) ? 0x35 : 0xCA);

    void* fsnd = nullptr;
    u32 fex[2] = {64, (u32)sizeof(fsb)};
    CHECK(fmod_System_CreateSound(sys, fsb, 0x800u /*OPENMEMORY*/, fex, &fsnd) == 0 &&
              fsnd != nullptr,
          "CreateSound(FSB5 FADPCM memory) -> handle");
    const u32 pre_fsb = g_cap.count;
    void* fch = nullptr;
    CHECK(fmod_System_PlaySound(sys, fsnd, nullptr, 0, &fch) == 0 && fch,
          "FSB5 FADPCM sound plays a real channel");
    CHECK(g_cap.count > pre_fsb && g_cap.bytes == 256u * 2u &&
              g_cap.rate == 44100 && g_cap.channels == 1 && g_cap.bps == 2,
          "FSB5 FADPCM decoded to real s16 PCM (256 frames, 44.1kHz mono)");
    fmod_Channel_Stop(fch);
    fmod_Sound_Release(fsnd);

    // Build a WAV IMA-ADPCM (format 0x0011, 36-byte mono block -> 65 samples)
    static u8 ima_wav[44 + 36];
    memset(ima_wav, 0, sizeof(ima_wav));
    memcpy(ima_wav, "RIFF", 4);
    u32 rsz = 36 + 36; memcpy(ima_wav + 4, &rsz, 4);
    memcpy(ima_wav + 8, "WAVEfmt ", 8);
    u32 fsz = 16; memcpy(ima_wav + 16, &fsz, 4);
    u16 itag = 0x0011u, ich = 1, iba = 36, ibits = 4;
    u32 irate = 22050, ibr = 11025;
    memcpy(ima_wav + 20, &itag, 2);
    memcpy(ima_wav + 22, &ich, 2);
    memcpy(ima_wav + 24, &irate, 4);
    memcpy(ima_wav + 28, &ibr, 4);
    memcpy(ima_wav + 32, &iba, 2);
    memcpy(ima_wav + 34, &ibits, 2);
    memcpy(ima_wav + 36, "data", 4);
    u32 dsz = 36; memcpy(ima_wav + 40, &dsz, 4);
    short pred = 1000;
    memcpy(ima_wav + 44, &pred, 2);
    ima_wav[46] = 20;  // step index
    for (int i = 48; i < 80; ++i) ima_wav[i] = 0x29;

    void* isnd = nullptr;
    u32 iex[2] = {64, (u32)sizeof(ima_wav)};
    CHECK(fmod_System_CreateSound(sys, ima_wav, 0x800u /*OPENMEMORY*/, iex, &isnd) == 0 &&
              isnd != nullptr,
          "CreateSound(WAV IMA-ADPCM memory) -> handle");
    const u32 pre_ima = g_cap.count;
    void* ich_h = nullptr;
    CHECK(fmod_System_PlaySound(sys, isnd, nullptr, 0, &ich_h) == 0 && ich_h,
          "WAV IMA-ADPCM sound plays a real channel");
    CHECK(g_cap.count > pre_ima && g_cap.bytes == 65u * 2u &&
              g_cap.rate == 22050 && g_cap.channels == 1 && g_cap.bps == 2,
          "WAV IMA-ADPCM decoded to real s16 PCM (65 frames, 22.05kHz mono)");
    fmod_Channel_Stop(ich_h);
    fmod_Sound_Release(isnd);
  }

  // --- compressed: named-silent, handle still real ---
  static u8 ogg[8] = {'O', 'g', 'g', 'S', 0, 0, 0, 0};
  void* osnd = nullptr;
  u32 omode = 0x800u;  // FMOD_OPENMEMORY
  u32 ex[2] = {64, 8};  // cbsize, length
  CHECK(fmod_System_CreateSound(sys, ogg, omode, ex, &osnd) == 0 &&
            osnd != nullptr,
        "CreateSound(ogg memory) -> handle (named-silent path)");
  void* och = nullptr;
  const u32 pre_ogg = g_cap.count;
  CHECK(fmod_System_PlaySound(sys, osnd, nullptr, 0, &och) == 0 && och,
        "silent sound plays a real channel");
  CHECK(g_cap.count == pre_ogg, "no PCM submitted for the compressed sound");
  fmod_Channel_Stop(och);

  // --- handle discipline ---
  CHECK(fmod_Sound_Release(snd) == 0, "Sound_Release");
  CHECK(fmod_System_PlaySound(sys, snd, nullptr, 0, &ch) != 0,
        "released sound handle is rejected (no use-after-free)");
  CHECK(fmod_System_PlaySound(sys, (void*)0x1234, nullptr, 0, &ch) ==
            27, /* FMOD_ERR_INVALID_HANDLE (real SDK value) */
        "garbage handle rejected with FMOD_ERR_INVALID_HANDLE");
  CHECK(fmod_System_Release(sys) == 0, "System_Release");

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}
