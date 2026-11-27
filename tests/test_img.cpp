// tiramisu/tests/test_img.cpp
// bridge_img: PNG + DDS parsers, standalone (no guest PE needed —
// trt_img_parse is a pure host-side function the d3dx9 bridge calls).
//
// Vectors:
//   PNG  — a minimal valid 2x2 RGBA PNG built byte-by-byte in this file
//          (fixed zlib stream, filter type 0), plus corrupt-input checks
//   DDS  — uncompressed X8R8G8B8 2x2 and a DXT5 4x4 block, exact pixels
#include <initializer_list>
#include "../src/trt/bridge_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// bridge_img logs through trt_core_log (thunk.cpp); standalone binary
// defines it here.
extern "C" void trt_core_log(const char* line) { printf("  [trt] %s\n", line); }

namespace {

int failures = 0;
#define CHECK(cond, name)                                          \
  do {                                                             \
    if (cond) printf("  PASS %s\n", name);                         \
    else { printf("  FAIL %s\n", name); ++failures; }              \
  } while (0)

u32 be32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
void le32store(void* p, u32 v) {
  u8* q = (u8*)p;
  q[0] = (u8)v; q[1] = (u8)(v >> 8); q[2] = (u8)(v >> 16); q[3] = (u8)(v >> 24);
}

// ---- build a minimal PNG: 2x2, color type 6 (RGBA), depth 8, filter 0 ----
// zlib stream for raw bytes [filter0][4 px][filter0][4 px] where each px =
// R,G,B,A. Deflate: stored block (BFINAL=1, BTYPE=00), len, nlen, data.
// Then adler32 of the raw data.
u32 adler32(const u8* d, u32 n) {
  u32 a = 1, b = 0;
  for (u32 i = 0; i < n; ++i) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}

// png chunk: len, type, payload, crc (crc not verified by parser — zeros ok)
void put_chunk(u8* o, u32& off, const char* type, const u8* payload, u32 len) {
  const u32 bl = len;
  o[off++] = (u8)(bl >> 24); o[off++] = (u8)(bl >> 16);
  o[off++] = (u8)(bl >> 8); o[off++] = (u8)bl;
  memcpy(o + off, type, 4); off += 4;
  memcpy(o + off, payload, len); off += len;
  // CRC over type+payload — parser doesn't verify; store real CRC anyway
  // via the standard table-less bitwise CRC32
  u32 crc = 0xFFFFFFFF;
  for (u32 i = 0; i < 4 + len; ++i) {
    const u8 byte = i < 4 ? (u8)type[i] : payload[i - 4];
    crc ^= byte;
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  }
  crc ^= 0xFFFFFFFF;
  o[off++] = (u8)(crc >> 24); o[off++] = (u8)(crc >> 16);
  o[off++] = (u8)(crc >> 8); o[off++] = (u8)crc;
}

void build_png(u8* o, u32& off, u32 w, u32 h, u8 color, const u8* raw,
               u32 raw_len) {
  static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  memcpy(o + off, sig, 8); off += 8;
  u8 ihdr[13];
  ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16);
  ihdr[2] = (u8)(w >> 8); ihdr[3] = (u8)w;
  ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16);
  ihdr[6] = (u8)(h >> 8); ihdr[7] = (u8)h;
  ihdr[8] = 8;    // depth
  ihdr[9] = color;
  ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  put_chunk(o, off, "IHDR", ihdr, 13);
  // zlib stream: 0x78 0x01 header, stored deflate blocks, adler
  u32 zoff = 0;
  u8* z = (u8*)malloc(raw_len + 16);
  z[zoff++] = 0x78; z[zoff++] = 0x01;
  // single stored block (max 65535 — our vectors are tiny)
  z[zoff++] = 0x01;  // BFINAL=1, BTYPE=00
  z[zoff++] = (u8)raw_len; z[zoff++] = (u8)(raw_len >> 8);
  z[zoff++] = (u8)(~raw_len & 0xFF); z[zoff++] = (u8)((~raw_len >> 8) & 0xFF);
  memcpy(z + zoff, raw, raw_len); zoff += raw_len;
  const u32 ad = adler32(raw, raw_len);
  z[zoff++] = (u8)(ad >> 24); z[zoff++] = (u8)(ad >> 16);
  z[zoff++] = (u8)(ad >> 8); z[zoff++] = (u8)ad;
  put_chunk(o, off, "IDAT", z, zoff);
  free(z);
  put_chunk(o, off, "IEND", (const u8*)"", 0);
}

}  // namespace

int main() {
  printf("tiramisu bridge_img test (PNG + DDS parsers)\n");

  // ==================== PNG 2x2 RGBA ====================
  // raw scanlines: filter0 + (R,G,B,A)x2 per row
  const u8 raw[2 * (2 * 4 + 1)] = {
      0, 255, 0, 0, 255, 0, 255, 0, 255,   // row0: red, green
      0, 0, 0, 255, 255, 255, 0, 255, 0,   // row1: blue(0,0,255), white-ish
  };
  u8* png = (u8*)malloc(4096);
  u32 png_len = 0;
  build_png(png, png_len, 2, 2, 6, raw, sizeof(raw));

  trt_img img{};
  int rc = trt_img_parse(png, png_len, &img);
  CHECK(rc == 0, "PNG 2x2 RGBA parses");
  if (rc == 0) {
    CHECK(img.w == 2 && img.h == 2, "PNG dims exact");
    const u8* px = img.rgba;
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255,
          "PNG px(0,0) = red");
    CHECK(px[8] == 0 && px[9] == 0 && px[10] == 255 && px[11] == 255,
          "PNG px(0,1) = blue");
    free(img.rgba);
  }

  // ==================== PNG gray ====================
  const u8 rawg[2 * (2 * 1 + 1)] = {0, 200, 100, 0, 50, 25};
  u8 png2[256];
  u32 png2_len = 0;
  build_png(png2, png2_len, 2, 2, 0, rawg, sizeof(rawg));
  rc = trt_img_parse(png2, png2_len, &img);
  CHECK(rc == 0, "PNG 2x2 gray parses");
  if (rc == 0) {
    CHECK(img.rgba[0] == 200 && img.rgba[1] == 200 && img.rgba[3] == 255,
          "PNG gray expands to RGBA");
    free(img.rgba);
  }

  // ==================== PNG corruption fail-visible ====================
  rc = trt_img_parse(png, 20, &img);  // truncated
  CHECK(rc != 0, "PNG truncated fails named");
  png[1] = 'X';  // break signature
  rc = trt_img_parse(png, png_len, &img);
  CHECK(rc != 0, "PNG bad signature fails named");
  png[1] = 'P';

  // ==================== DDS uncompressed X8R8G8B8 2x2 ====================
  u8 dds[128 + 16];
  memset(dds, 0, sizeof(dds));
  memcpy(dds, "DDS ", 4);
  le32store(dds + 12, 2);   // height
  le32store(dds + 16, 2);   // width
  le32store(dds + 88, 32);  // bpp
  // pixel format: flags DDPF_RGB(0x40) at +80, fourCC 0 at +84
  le32store(dds + 80, 0x40);
  le32store(dds + 92, 0x00FF0000);  // R mask
  le32store(dds + 96, 0x0000FF00);  // G mask
  le32store(dds + 100, 0x000000FF); // B mask
  le32store(dds + 104, 0xFF000000); // A mask
  // pixels: (255,0,0), (0,255,0) / (0,0,255), (255,255,255) as A8R8G8B8 LE
  le32store(dds + 128 + 0, 0xFFFF0000);      // red
  le32store(dds + 128 + 4, 0xFF00FF00);      // green
  le32store(dds + 128 + 8, 0xFF0000FF);      // blue
  le32store(dds + 128 + 12, 0xFFFFFFFF);     // white
  rc = trt_img_parse(dds, sizeof(dds), &img);
  CHECK(rc == 0, "DDS X8R8G8B8 parses");
  if (rc == 0) {
    CHECK(img.w == 2 && img.h == 2, "DDS dims exact");
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0,
          "DDS px(0,0) = red");
    CHECK(img.rgba[4] == 0 && img.rgba[5] == 255 && img.rgba[6] == 0,
          "DDS px(1,0) = green");
    free(img.rgba);
  }

  // ==================== DDS DXT5 4x4 ====================
  u8 dxt[128 + 16];
  memset(dxt, 0, sizeof(dxt));
  memcpy(dxt, "DDS ", 4);
  le32store(dxt + 12, 4); le32store(dxt + 16, 4);   // 4x4
  le32store(dxt + 80, 4);                            // DDPF_FOURCC
  memcpy(dxt + 84, "DXT5", 4);
  // alpha block: a0=0 a1=255 -> indices all pick... a0<a1: lut = 0,255,0..,
  // idx 1 = 255. color block: c0=red(31,0,0)->565, c1=blue, idx all 0 = c0
  const u16 red565 = (31 << 11) | (0 << 5) | 0;    // 0xF800
  const u16 blue565 = (0 << 11) | (0 << 5) | 31;   // 0x001F
  dxt[128 + 0] = 0; dxt[128 + 1] = 255;            // alpha endpoints
  memset(dxt + 128 + 2, 0x49, 6);                  // alpha idx: 001 001 ... =
                                                   // picks alut[1]=255 every
                                                   // 3 bits? 0x49 = 0b1001001
  // alpha indices: index 1 (001) everywhere = 48 bits of 001 repeating;
  // the byte pattern has period 3: 0x49 0x92 0x24
  dxt[128 + 2] = 0x49; dxt[128 + 3] = 0x92; dxt[128 + 4] = 0x24;
  dxt[128 + 5] = 0x49; dxt[128 + 6] = 0x92; dxt[128 + 7] = 0x24;
  dxt[128 + 8] = (u8)(red565 & 0xFF); dxt[128 + 9] = (u8)(red565 >> 8);
  dxt[128 + 10] = (u8)(blue565 & 0xFF); dxt[128 + 11] = (u8)(blue565 >> 8);
  memset(dxt + 128 + 12, 0, 4);                    // color idx all 0 = red
  rc = trt_img_parse(dxt, sizeof(dxt), &img);
  CHECK(rc == 0, "DDS DXT5 4x4 parses");
  if (rc == 0) {
    CHECK(img.w == 4 && img.h == 4, "DXT5 dims exact");
    // all pixels red (color idx 0), alpha 255 (alpha idx 1)
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 &&
              img.rgba[3] == 255,
          "DXT5 px(0,0) = opaque red");
    CHECK(img.rgba[(3 * 4 + 3) * 4] == 255 &&
              img.rgba[(3 * 4 + 3) * 4 + 3] == 255,
          "DXT5 px(3,3) = opaque red");
    free(img.rgba);
  }

  // In BC2 and BC3 even ascending 565 endpoints use the four-color palette.
  // Selector 3 between black and white must yield opaque light gray.
  for (const char* tag : {"DXT3", "DXT5", "DXT1"}) {
    memcpy(dxt + 84, tag, 4);
    const bool bc1 = tag[3] == '1';
    memset(dxt + 128, 0xff, 16);
    u8* colors = dxt + 128 + (bc1 ? 0 : 8);
    colors[0] = colors[1] = 0;
    rc = trt_img_parse(dxt, sizeof(dxt), &img);
    CHECK(rc == 0, "ascending BC endpoints parse");
    if (rc == 0) {
      const u8 gray = bc1 ? 0 : 170;
      CHECK(img.rgba[0] == gray && img.rgba[1] == gray &&
            img.rgba[2] == gray && img.rgba[3] == (bc1 ? 0 : 255),
            "BC1 transparency is exclusive to BC1");
      free(img.rgba);
    }
  }

  // ==================== unknown container fails named ====================
  rc = trt_img_parse("GIF89a", 6, &img);
  CHECK(rc != 0, "GIF fails named (not PNG/DDS)");

  free(png);
  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", failures);
  return 1;
}
