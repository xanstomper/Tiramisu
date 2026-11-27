// tiramisu/src/trt/audio_codecs.h
// Shared audio decoders for FMOD Core, FMOD Studio, and Wwise bridges:
//   - FMOD FADPCM (4-bit predictive ADPCM, 140-byte frames -> 256 samples/ch)
//   - IMA-ADPCM (WAV 0x0011, Wwise ADPCM, FSB5 IMA-ADPCM)
//   - FSB5 non-Vorbis payload decode (PCM8/16/24/32/float, IMA-ADPCM, MPEG, FADPCM)
//   - RIFF/WAVE & Wwise .wem container decode (PCM, Float, IMA-ADPCM, Ogg, MP3)
#ifndef TIRAMISU_AUDIO_CODECS_H
#define TIRAMISU_AUDIO_CODECS_H

#include "trt.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FSB5 codec mode identifiers (FMOD 5 standard enum)
enum trt_fsb5_mode : u32 {
  TRT_FSB5_PCM8     = 1,
  TRT_FSB5_PCM16    = 2,
  TRT_FSB5_PCM24    = 3,
  TRT_FSB5_PCM32    = 4,
  TRT_FSB5_PCMFLOAT = 5,
  TRT_FSB5_IMAADPCM = 7,
  TRT_FSB5_MPEG     = 11,
  TRT_FSB5_FADPCM   = 14,
  TRT_FSB5_VORBIS   = 15,
};

struct trt_decoded_pcm {
  int16_t* samples;   // heap-allocated interleaved s16 PCM (caller frees via free())
  u32 frames;         // sample frames per channel
  u32 rate;           // Hz
  u32 channels;       // 1..8
  u32 loop;           // 1 if container specifies loop
  u32 loop_start;     // frame
  u32 loop_end;       // frame
};

// Decode interleaved FADPCM frames (140 bytes per channel per 256-frame block).
// Returns 0 on success, -1 on malformed input or allocation failure.
int trt_decode_fadpcm(const u8* data, size_t data_len, u32 channels,
                      u32 num_frames, int16_t** out_pcm, u32* out_frames);

// Decode WAV / Wwise / FSB5 IMA-ADPCM blocks.
// block_align must be >= 4 * channels (typically 36 * channels for FSB5/Wwise,
// or fmt.nBlockAlign for WAV 0x0011).
int trt_decode_ima_adpcm(const u8* data, size_t data_len, u32 channels,
                         u32 block_align, u32 max_frames,
                         int16_t** out_pcm, u32* out_frames);

// Decode a single FSB5 sample's compressed/raw payload (all modes except Vorbis,
// which requires Ogg remuxing with VORBISDATA setup CRC in bridge_fmod_studio).
// Returns 0 on success (out->samples heap-allocated), -1 on unsupported/corrupt.
int trt_fsb5_decode_payload(u32 fsb_mode, const u8* payload, size_t payload_len,
                            u32 rate, u32 channels, u32 expected_frames,
                            trt_decoded_pcm* out);

// Parse a standalone FSB5 container buffer and decode its first sample (for
// FMOD Core CreateSound on .fsb files/memory). Guarded by trt_fsb5_header_sane
// and trt_fsb5_sample_sane. Returns 0 on success, -1 on error/unsupported.
int trt_fsb5_decode_first_sample(const u8* fsb_data, size_t fsb_len,
                                 trt_decoded_pcm* out);

// Decode a RIFF/WAVE, Wwise .wem (PCM/Float/IMA-ADPCM), Ogg Vorbis, or MP3
// memory buffer into interleaved s16 PCM. Returns 0 on success, -1 on error.
int trt_decode_audio_buffer(const u8* data, size_t len, trt_decoded_pcm* out);

#ifdef __cplusplus
}
#endif

#endif
