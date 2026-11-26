// tiramisu/src/trt/bridge_img.cpp
// PNG + DDS parsers (M7). See bridge_img.h.
//
// PNG: signature, IHDR (w/h/bitdepth/colortype), concatenate IDAT
// chunks, inflate with miniz's tinfl (raw deflate after the 2-byte
// zlib header), then unfilter each scanline (the 5 filter types).
// Only bitdepth 8 is supported (games use 8); 16/1/2/4 fail named.
//
// DDS: 'DDS ' magic, header height/width/mipcount at fixed offsets,
// pixel format fourCC at +84: 0 = uncompressed RGB (bitmasks at +92..),
// 'DXT1'/'DXT3'/'DXT5' = BC1/2/3 blocks. Mip 0 is decoded; BC blocks
// are expanded to RGBA8 (the standard table-based decoder).
#include "bridge_img.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// vendored miniz (zlib license) — tinfl is the raw-inflate core
#include "../host/vendor/miniz.h"
// vendored stb_image (public domain) — the fallback loader for JPG/GIF/
// PSD and every container variant the hand parsers reject
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_PSD
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../host/vendor/stb_image.h"

extern "C" {

namespace {

void log_line(const char* s) { trt_core_log(s); }

// ---- CRC table for PNG chunk verification (not enforced; parse only) ----

u32 be32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// ============================ PNG ============================
constexpr u8 k_png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

int parse_png(const u8* d, u32 bytes, trt_img* out) {
  if (bytes < 57 || memcmp(d, k_png_sig, 8) != 0) return (int)0x8007000D;  // corrupt
  // IHDR at +8: len(4) 'IHDR'(4) w(4) h(4) depth(1) color(1) comp(1)
  //             filter(1) interlace(1) crc(4)
  const u32 w = be32(d + 16);
  const u32 h = be32(d + 20);
  const u8 depth = d[24];
  const u8 color = d[25];
  const u8 interlace = d[28];
  if (!w || !h || h > 16384 || w > 16384) return (int)0x8007000D;
  if (depth != 8) {
    char line[80];
    snprintf(line, sizeof(line), "img: PNG bitdepth %u unsupported", depth);
    log_line(line);
    return (int)0x80004001;
  }
  if (interlace != 0) {
    log_line("img: PNG interlaced unsupported");
    return (int)0x80004001;
  }
  // channels by color type: 0=gray, 2=RGB, 3=palette(fail), 4=grayA, 6=RGBA
  u32 ch = 0;
  if (color == 0) ch = 1;
  else if (color == 2) ch = 3;
  else if (color == 4) ch = 2;
  else if (color == 6) ch = 4;
  else {
    log_line("img: PNG palette/indexed unsupported (convert to RGBA)");
    return (int)0x80004001;
  }

  // concatenate IDAT payloads
  const u8* p = d + 8;
  const u8* end = d + bytes;
  u32 idat_total = 0;
  // first pass: size
  for (const u8* q = p; q + 12 <= end;) {
    const u32 len = be32(q);
    if (q + 12 + len > end) break;
    if (memcmp(q + 4, "IDAT", 4) == 0) idat_total += len;
    q += 12 + len;
  }
  if (!idat_total) {
    log_line("img: PNG has no IDAT");
    return (int)0x8007000D;
  }
  u8* idat = (u8*)malloc(idat_total);
  u32 off = 0;
  for (const u8* q = p; q + 12 <= end;) {
    const u32 len = be32(q);
    if (q + 12 + len > end) break;
    if (memcmp(q + 4, "IDAT", 4) == 0) {
      memcpy(idat + off, q + 8, len);
      off += len;
    }
    q += 12 + len;
  }

  // zlib stream: 2-byte header, raw deflate, 4-byte adler
  if (idat_total < 6) { free(idat); return (int)0x8007000D; }
  const size_t raw_len_expect = (size_t)h * ((size_t)w * ch + 1);
  size_t raw_len = raw_len_expect;
  // tinfl with TINFL_FLAG_PARSE_ZLIB_HEADER consumes the zlib header itself
  u8* raw = (u8*)tinfl_decompress_mem_to_heap(
      idat, idat_total, &raw_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
  free(idat);
  if (!raw || raw_len != raw_len_expect) {
    if (raw) free(raw);
    log_line("img: PNG inflate size mismatch (truncated IDAT?)");
    return (int)0x8007000D;
  }

  // unfilter: per scanline, filter byte + w*ch bytes; bpp = ceil(ch/8)
  const size_t stride = (size_t)w * ch;
  // PNG filter bpp = ceil(bitdepth * channels / 8); bitdepth is 8 here,
  // so bpp == channels. (Was: a ternary yielding 2 for RGB, which
  // scrambled every RGB PNG's Sub/Average/Paeth unfilter.)
  const int bpp_f = ch;
  u8* prev = (u8*)calloc(1, stride);
  u8* cur = (u8*)malloc(stride);
  u8* rgba = (u8*)calloc(1, (size_t)w * h * 4);
  for (u32 y = 0; y < h; ++y) {
    const u8* line_in = raw + (size_t)y * (stride + 1);
    const u8 ft = line_in[0];
    const u8* data = line_in + 1;
    for (size_t x = 0; x < stride; ++x) {
      const i32 a = x >= (size_t)bpp_f ? cur[x - bpp_f] : 0;
      const i32 b = prev[x];
      const i32 c = x >= (size_t)bpp_f ? prev[x - bpp_f] : 0;
      i32 v = data[x];
      if (ft == 1) v += a;
      else if (ft == 2) v += b;
      else if (ft == 3) v += (a + b) / 2;
      else if (ft == 4) {
        const i32 p_ = a + b - c;
        const i32 pa = p_ > a ? p_ - a : a - p_;
        const i32 pb = p_ > b ? p_ - b : b - p_;
        const i32 pc = p_ > c ? p_ - c : c - p_;
        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      }
      cur[x] = (u8)v;
    }
    // expand to RGBA8
    u8* dst = rgba + (size_t)y * w * 4;
    if (ch == 4) {
      memcpy(dst, cur, stride);
    } else if (ch == 3) {
      for (u32 x = 0; x < w; ++x) {
        dst[x*4+0] = cur[x*3+0];
        dst[x*4+1] = cur[x*3+1];
        dst[x*4+2] = cur[x*3+2];
        dst[x*4+3] = 0xFF;
      }
    } else if (ch == 2) {
      for (u32 x = 0; x < w; ++x) {
        dst[x*4+0] = dst[x*4+1] = dst[x*4+2] = cur[x*2+0];
        dst[x*4+3] = cur[x*2+1];
      }
    } else {
      for (u32 x = 0; x < w; ++x) {
        dst[x*4+0] = dst[x*4+1] = dst[x*4+2] = cur[x];
        dst[x*4+3] = 0xFF;
      }
    }
    memcpy(prev, cur, stride);
  }
  free(prev);
  free(cur);
  free(raw);
  out->w = w;
  out->h = h;
  out->rgba = rgba;
  return 0;
}

// ============================ DDS ============================
u32 fourcc(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// BC decode tables
struct bc_col { u8 r, g, b, a; };

void bc1_block(const u8* in, bc_col* px /* 16 out */) {
  const u16 c0 = (u16)in[0] | ((u16)in[1] << 8);
  const u16 c1 = (u16)in[2] | ((u16)in[3] << 8);
  // 565 -> 8: bit replication (v<<3 | v>>2) gives full 0..255 range
  bc_col p0 = {(u8)(((c0 >> 11) & 31) * 255 / 31), (u8)(((c0 >> 5) & 63) * 255 / 63),
               (u8)((c0 & 31) * 255 / 31), 0xFF};
  bc_col p1 = {(u8)(((c1 >> 11) & 31) * 255 / 31), (u8)(((c1 >> 5) & 63) * 255 / 63),
               (u8)((c1 & 31) * 255 / 31), 0xFF};
  bc_col lut[4];
  lut[0] = p0;
  lut[1] = p1;
  if (c0 > c1) {
    lut[2].r = (2 * p0.r + p1.r + 1) / 3;  // approximate: (p0*2+p1)/3
    lut[2].g = (2 * p0.g + p1.g + 1) / 3;
    lut[2].b = (2 * p0.b + p1.b + 1) / 3;
    lut[2].a = 0xFF;
    lut[3].r = (p0.r + 2 * p1.r + 1) / 3;
    lut[3].g = (p0.g + 2 * p1.g + 1) / 3;
    lut[3].b = (p0.b + 2 * p1.b + 1) / 3;
    lut[3].a = 0xFF;
  } else {
    lut[2].r = (p0.r + p1.r) / 2; lut[2].g = (p0.g + p1.g) / 2;
    lut[2].b = (p0.b + p1.b) / 2; lut[2].a = 0xFF;
    lut[3].r = lut[3].g = lut[3].b = lut[3].a = 0;  // transparent black
  }
  for (u32 i = 0; i < 16; ++i) {
    const u32 bits = (u32)in[4 + i / 4] >> ((i % 4) * 2) & 3;
    px[i] = lut[bits];
  }
}

void bc23_block(const u8* in, bc_col* px, int is_bc2) {
  if (is_bc2) {
    // BC2: 16 alpha nibbles first, then a BC1 color block
    bc1_block(in + 8, px);
    for (u32 i = 0; i < 16; ++i)
      px[i].a = (u8)((in[i / 2] >> ((i % 2) * 4)) & 15) * 17;
  } else {
    // BC3: alpha0/alpha1 + 3-bit indices, then BC1 color
    const u8 a0 = in[0], a1 = in[1];
    u8 alut[8];
    alut[0] = a0;
    alut[1] = a1;
    if (a0 > a1) {
      for (u32 i = 0; i < 6; ++i)
        alut[2 + i] = (u8)(((6 - i) * a0 + (1 + i) * a1) / 7);
    } else {
      for (u32 i = 0; i < 4; ++i)
        alut[2 + i] = (u8)(((4 - i) * a0 + (1 + i) * a1) / 5);
      alut[6] = 0;
      alut[7] = 255;
    }
    bc1_block(in + 8, px);
    // 48-bit alpha index stream
    u64 aidx = 0;
    for (u32 i = 0; i < 6; ++i)
      aidx |= (u64)in[2 + i] << (8 * i);
    for (u32 i = 0; i < 16; ++i)
      px[i].a = alut[(aidx >> (3 * i)) & 7];
  }
}

int parse_dds(const u8* d, u32 bytes, trt_img* out) {
  if (bytes < 128 || memcmp(d, "DDS ", 4) != 0) return (int)0x8007000D;
  const u32 h = *(const u32*)(d + 12);
  const u32 w = *(const u32*)(d + 16);
  if (!w || !h || w > 16384 || h > 16384) return (int)0x8007000D;
  const u32 fourcc_ = *(const u32*)(d + 84);
  const u32* pfd = (const u32*)(d + 76);  // DDPIXELFORMAT starts at +76
  (void)pfd;
  u8* rgba = (u8*)calloc(1, (size_t)w * h * 4);

  if (fourcc_ == 0 || fourcc_ == 0x31385441 /* 'ATI1' no; uncompressed */) {
    // uncompressed: bitmasks at d+92 (R), +96 (G), +100 (B), +104 (A)
    const u32 bpp = *(const u32*)(d + 88);
    const u32 rm = *(const u32*)(d + 92);
    const u32 gm = *(const u32*)(d + 96);
    const u32 bm = *(const u32*)(d + 100);
    const u32 am = *(const u32*)(d + 104);
    const u32 bytes_pp = bpp / 8;
    if (!bytes_pp || bytes_pp > 4) { free(rgba); return (int)0x8007000D; }
    const u8* px = d + 128;
    if ((u64)w * h * bytes_pp > bytes - 128) { free(rgba); return (int)0x8007000D; }
    for (u32 y = 0; y < h; ++y) {
      u8* dst = rgba + (size_t)y * w * 4;
      for (u32 x = 0; x < w; ++x) {
        const u8* s = px + ((size_t)y * w + x) * bytes_pp;
        u32 v = 0;
        memcpy(&v, s, bytes_pp);
        dst[x*4+0] = (u8)((v & rm) ? 1 : 0);  // refined below
        // proper shift extraction
        u32 sh = 0, bits = 0, t = rm;
        while (t && !(t & 1)) { ++sh; t >>= 1; }
        t = rm >> sh; while (t) { ++bits; t >>= 1; }
        dst[x*4+0] = (u8)(((v & rm) >> sh) * 255 / ((1 << bits) - 1));
        sh = 0; bits = 0; t = gm;
        while (t && !(t & 1)) { ++sh; t >>= 1; }
        t = gm >> sh; while (t) { ++bits; t >>= 1; }
        dst[x*4+1] = (u8)(((v & gm) >> sh) * 255 / ((1 << bits) - 1));
        sh = 0; bits = 0; t = bm;
        while (t && !(t & 1)) { ++sh; t >>= 1; }
        t = bm >> sh; while (t) { ++bits; t >>= 1; }
        dst[x*4+2] = (u8)(((v & bm) >> sh) * 255 / ((1 << bits) - 1));
        sh = 0; bits = 0; t = am;
        while (t && !(t & 1)) { ++sh; t >>= 1; }
        t = am >> sh; while (t) { ++bits; t >>= 1; }
        dst[x*4+3] = am ? (u8)(((v & am) >> sh) * 255 / ((1 << bits) - 1))
                        : 0xFF;
      }
    }
  } else if (fourcc_ == fourcc((const u8*)"DXT1") ||
             fourcc_ == fourcc((const u8*)"DXT3") ||
             fourcc_ == fourcc((const u8*)"DXT5")) {
    const u32 bw = (w + 3) / 4;
    const u32 bh = (h + 3) / 4;
    const u32 block_bytes = fourcc_ == fourcc((const u8*)"DXT1") ? 8 : 16;
    const u8* px = d + 128;
    if ((u64)bw * bh * block_bytes > bytes - 128) {
      free(rgba);
      return (int)0x8007000D;
    }
    bc_col blk[16];
    for (u32 by = 0; by < bh; ++by) {
      for (u32 bx = 0; bx < bw; ++bx) {
        const u8* src = px + ((size_t)by * bw + bx) * block_bytes;
        if (fourcc_ == fourcc((const u8*)"DXT1")) bc1_block(src, blk);
        else bc23_block(src, blk, fourcc_ == fourcc((const u8*)"DXT3"));
        for (u32 i = 0; i < 16; ++i) {
          const u32 x = bx * 4 + (i & 3);
          const u32 y = by * 4 + (i >> 2);
          if (x >= w || y >= h) continue;
          u8* dst = rgba + ((size_t)y * w + x) * 4;
          dst[0] = blk[i].r;
          dst[1] = blk[i].g;
          dst[2] = blk[i].b;
          dst[3] = blk[i].a;
        }
      }
    }
  } else {
    char line[96];
    snprintf(line, sizeof(line), "img: DDS fourCC %08x unsupported",
             fourcc_);
    log_line(line);
    free(rgba);
    return (int)0x80004001;
  }
  out->w = w;
  out->h = h;
  out->rgba = rgba;
  return 0;
}

}  // namespace

int trt_img_parse(const void* data, u32 bytes, trt_img* out) {
  if (!data || !bytes || !out) return (int)0x80070057;
  const u8* d = (const u8*)data;
  if (bytes >= 8 && memcmp(d, k_png_sig, 8) == 0) {
    const int rc = parse_png(d, bytes, out);
    if (rc == 0) {
      char line[80];
      snprintf(line, sizeof(line), "img: PNG %ux%u parsed", out->w, out->h);
      log_line(line);
    }
    return rc;
  }
  if (bytes >= 4 && memcmp(d, "DDS ", 4) == 0) {
    const int rc = parse_dds(d, bytes, out);
    if (rc == 0) {
      char line[80];
      snprintf(line, sizeof(line), "img: DDS %ux%u parsed", out->w, out->h);
      log_line(line);
    }
    return rc;
  }
  // everything else (JPG, GIF, PSD, PIC, PNM, robust PNG/TGA/BMP variants
  // the hand parsers reject): vendored stb_image (public domain). RGBA
  // request forces 4-channel output regardless of source channels.
  int w = 0, h = 0, channels = 0;
  u8* rgba = stbi_load_from_memory(d, (int)bytes, &w, &h, &channels, 4);
  if (!rgba) {
    char line[160];
    snprintf(line, sizeof(line), "img: stb_image also failed: %s",
             stbi_failure_reason());
    log_line(line);
    return (int)0x8007000D;  // corrupt/unsupported
  }
  char line[96];
  snprintf(line, sizeof(line),
           "img: stb %ux%u (%d src channels -> RGBA) parsed", w, h, channels);
  log_line(line);
  out->w = (u32)w;
  out->h = (u32)h;
  out->rgba = rgba;
  return 0;
}

}  // extern "C"
