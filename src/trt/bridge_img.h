// tiramisu/src/trt/bridge_img.h
// Image-container parsers for the d3dx9 bridge (M7).
//
// Games store UI/world textures as PNG (UI, fonts), DDS (world
// textures), and TGA/BMP (older engines). This file parses the
// containers into raw RGBA8 rows the texture path can upload:
//   PNG  — zlib stream, inflate via vendored miniz (zlib license);
//          supports 8-bit gray / gray+alpha / RGB / RGBA, no interlace
//   DDS  — uncompressed A8R8G8B8/X8R8G8B8/R5G6B5 and block-compressed
//          BC1/BC2/BC3 (DXT1/3/5) decoded to RGBA8
//   JPG  — baseline JPEG via vendored miniz? miniz has no JPG decoder;
//          fails visibly (rare in games' own textures: engines use PNG/DDS)
//
// Every parser is pure: (data, bytes) -> (w, h, malloc'd RGBA8 buffer)
// or a named failure. No exceptions, no silent paths.
#ifndef TIRAMISU_BRIDGE_IMG_H
#define TIRAMISU_BRIDGE_IMG_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// parsed image: RGBA8, top-down, rows tightly packed (w*4 bytes/row)
typedef struct trt_img {
  u32 w, h;
  u8* rgba;  // malloc'd; caller frees
} trt_img;

// Decode one 16-byte BC3 block into sixteen row-major RGBA8 pixels.
void trt_img_decode_bc3_block(const u8 block[16], u8 rgba[64]);

// parse a PNG or DDS from memory; returns 0 and fills *out on success,
// nonzero (a named HRESULT) on failure. Log line names the failure.
int trt_img_parse(const void* data, u32 bytes, trt_img* out);

#ifdef __cplusplus
}
#endif

#endif
