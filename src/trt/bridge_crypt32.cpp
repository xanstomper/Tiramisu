// tiramisu/src/trt/bridge_crypt32.cpp
// crypt32 bridge. See bridge_crypt32.h.
//
// No certificate store on console. Open verbs fail with ERROR-class codes
// and name the reason; Free/Close verbs succeed (freeing nothing is not a
// lie). UnityPlayer probes the cert surface at boot and an honest FALSE
// sends it down the no-cert path.
#ifndef _CRT_RAND_S
#  define _CRT_RAND_S
#endif
#include "bridge_crypt32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {
[[maybe_unused]] constexpr u32 k_err_invalid_parameter = 87;
[[maybe_unused]] constexpr u32 k_err_no_data = 232;

namespace cryptlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) cryptlog::line(s_)

static inline u32 rotl32(u32 x, u32 n) { return (x << n) | (x >> (32 - n)); }
static inline u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }
static inline u32 load_le32(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u32 load_be32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static inline void store_le32(u8* p, u32 v) {
  p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static inline void store_be32(u8* p, u32 v) {
  p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

void md5_block(u32 s[4], const u8 blk[64]) {
  static const u32 k[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
  };
  static const u32 r[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
  };
  u32 w[16];
  for (int i = 0; i < 16; ++i) w[i] = load_le32(blk + i * 4);
  u32 a = s[0], b = s[1], c = s[2], d = s[3];
  for (int i = 0; i < 64; ++i) {
    u32 f = 0, g = 0;
    if (i < 16)      { f = (b & c) | ((~b) & d); g = (u32)i; }
    else if (i < 32) { f = (d & b) | ((~d) & c); g = (u32)((5 * i + 1) & 15); }
    else if (i < 48) { f = b ^ c ^ d;            g = (u32)((3 * i + 5) & 15); }
    else             { f = c ^ (b | (~d));       g = (u32)((7 * i) & 15); }
    const u32 tmp = d;
    d = c; c = b;
    b = b + rotl32(a + f + k[i] + w[g], r[i]);
    a = tmp;
  }
  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

void sha1_block(u32 s[5], const u8 blk[64]) {
  u32 w[80];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(blk + i * 4);
  for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  u32 a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
  for (int i = 0; i < 80; ++i) {
    u32 f = 0, k = 0;
    if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else             { f = b ^ c ^ d;            k = 0xCA62C1D6u; }
    const u32 t = rotl32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl32(b, 30); b = a; a = t;
  }
  s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}

void sha256_block(u32 s[8], const u8 blk[64]) {
  static const u32 k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
  };
  u32 w[64];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(blk + i * 4);
  for (int i = 16; i < 64; ++i) {
    const u32 s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
    const u32 s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  u32 a=s[0], b=s[1], c=s[2], d=s[3], e=s[4], f=s[5], g=s[6], h=s[7];
  for (int i = 0; i < 64; ++i) {
    const u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const u32 ch = (e & f) ^ ((~e) & g);
    const u32 t1 = h + S1 + ch + k[i] + w[i];
    const u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const u32 maj = (a & b) ^ (a & c) ^ (b & c);
    const u32 t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
}

void hash_block(trt_hash_ctx* ctx, const u8 blk[64]) {
  if (ctx->alg == TRT_HASH_MD5) md5_block(ctx->state, blk);
  else if (ctx->alg == TRT_HASH_SHA1) sha1_block(ctx->state, blk);
  else if (ctx->alg == TRT_HASH_SHA256) sha256_block(ctx->state, blk);
}

constexpr u32 BCRYPT_ALG_RNG = 0xBC010001u;
constexpr u32 BCRYPT_ALG_SHA256 = 0xBC010002u;
constexpr u32 BCRYPT_ALG_SHA1 = 0xBC010003u;
constexpr u32 BCRYPT_ALG_MD5 = 0xBC010004u;

u32 alg_handle_to_hash(u32 h) {
  switch (h) {
    case BCRYPT_ALG_SHA256: return TRT_HASH_SHA256;
    case BCRYPT_ALG_SHA1:   return TRT_HASH_SHA1;
    case BCRYPT_ALG_MD5:    return TRT_HASH_MD5;
    default:                return TRT_HASH_NONE;
  }
}

bool utf16_ieq_ascii(const u16* w, const char* a) {
  if (!w || !a) return false;
  while (*w && *a) {
    u16 cw = *w;
    u16 ca = (u16)(u8)*a;
    if (cw >= 'a' && cw <= 'z') cw = (u16)(cw - 'a' + 'A');
    if (ca >= 'a' && ca <= 'z') ca = (u16)(ca - 'a' + 'A');
    if (cw != ca) return false;
    ++w; ++a;
  }
  return *w == 0 && *a == 0;
}

struct bcrypt_hash_slot {
  bool used;
  trt_hash_ctx ctx;
};
bcrypt_hash_slot g_bcrypt_hashes[64];
}  // namespace

extern "C" {

// DXBC uses MD5's compression function with a different final block layout.
// The layout is documented by ComputeHashRetail in DirectXShaderCompiler.
void trt_dxbc_checksum(const u8* bytes, u32 size, u8 digest[16]) {
  u32 state[] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  u32 remaining = size;
  while (remaining >= 64) {
    md5_block(state, bytes);
    bytes += 64;
    remaining -= 64;
  }

  u8 block[64]{};
  if (remaining >= 56) {
    memcpy(block, bytes, remaining);
    block[remaining] = 0x80;
    md5_block(state, block);
    memset(block, 0, sizeof(block));
  } else {
    memcpy(block + 4, bytes, remaining);
    block[4 + remaining] = 0x80;
  }
  store_le32(block, size << 3);
  store_le32(block + 60, 1u | (size << 1));
  md5_block(state, block);
  for (unsigned i = 0; i < 4; ++i) store_le32(digest + 4 * i, state[i]);
}

void trt_csprng_fill(u8* out, u32 len) {
  if (!out || !len) return;
#ifdef _WIN32
  u32 pos = 0;
  while (pos < len) {
    unsigned int val = 0;
    if (rand_s(&val) != 0) val = (unsigned int)clock() ^ 0x9E3779B9u;
    const u32 n = (len - pos < 4) ? (len - pos) : 4;
    memcpy(out + pos, &val, n);
    pos += n;
  }
#else
  FILE* f = fopen("/dev/urandom", "rb");
  u32 got = 0;
  if (f) {
    got = (u32)fread(out, 1, len, f);
    fclose(f);
  }
  if (got < len) {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 st = (u64)ts.tv_nsec ^ ((u64)ts.tv_sec << 32) ^ 0xA5A55A5Au;
    for (u32 i = got; i < len; ++i) {
      st = st * 6364136223846793005ull + 1442695040888963407ull;
      out[i] = (u8)(st >> 56);
    }
  }
#endif
}

u32 trt_hash_digest_len(u32 alg) {
  switch (alg) {
    case TRT_HASH_MD5:    return 16;
    case TRT_HASH_SHA1:   return 20;
    case TRT_HASH_SHA256: return 32;
    default:              return 0;
  }
}

void trt_hash_init(trt_hash_ctx* ctx, u32 alg) {
  if (!ctx) return;
  memset(ctx, 0, sizeof(*ctx));
  ctx->alg = alg;
  if (alg == TRT_HASH_MD5) {
    ctx->state[0] = 0x67452301u; ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu; ctx->state[3] = 0x10325476u;
  } else if (alg == TRT_HASH_SHA1) {
    ctx->state[0] = 0x67452301u; ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu; ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xc3d2e1f0u;
  } else if (alg == TRT_HASH_SHA256) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
  }
}

void trt_hash_update(trt_hash_ctx* ctx, const u8* data, u32 len) {
  if (!ctx || !data || !len) return;
  ctx->total_bytes += len;
  u32 pos = 0;
  if (ctx->buf_len > 0) {
    const u32 room = 64 - ctx->buf_len;
    const u32 take = len < room ? len : room;
    memcpy(ctx->buf + ctx->buf_len, data, take);
    ctx->buf_len += take;
    pos += take;
    if (ctx->buf_len == 64) {
      hash_block(ctx, ctx->buf);
      ctx->buf_len = 0;
    }
  }
  while (pos + 64 <= len) {
    hash_block(ctx, data + pos);
    pos += 64;
  }
  if (pos < len) {
    memcpy(ctx->buf, data + pos, len - pos);
    ctx->buf_len = len - pos;
  }
}

void trt_hash_final(trt_hash_ctx* ctx, u8* out) {
  if (!ctx || !out) return;
  const u64 bits = ctx->total_bytes * 8ull;
  u8 pad[72];
  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  const u32 rem = ctx->buf_len;
  const u32 pad_len = (rem < 56) ? (56 - rem) : (120 - rem);
  trt_hash_update(ctx, pad, pad_len);
  u8 len_blk[8];
  if (ctx->alg == TRT_HASH_MD5) {
    store_le32(len_blk, (u32)(bits & 0xFFFFFFFFu));
    store_le32(len_blk + 4, (u32)(bits >> 32));
  } else {
    store_be32(len_blk, (u32)(bits >> 32));
    store_be32(len_blk + 4, (u32)(bits & 0xFFFFFFFFu));
  }
  trt_hash_update(ctx, len_blk, 8);
  if (ctx->alg == TRT_HASH_MD5) {
    for (int i = 0; i < 4; ++i) store_le32(out + i * 4, ctx->state[i]);
  } else if (ctx->alg == TRT_HASH_SHA1) {
    for (int i = 0; i < 5; ++i) store_be32(out + i * 4, ctx->state[i]);
  } else if (ctx->alg == TRT_HASH_SHA256) {
    for (int i = 0; i < 8; ++i) store_be32(out + i * 4, ctx->state[i]);
  }
}

u32 TRT_GUEST_ABI trt_BCryptOpenAlgorithmProvider(void** out_alg,
                                                 const u16* alg_id,
                                                 const u16* impl, u32 flags) {
  (void)impl; (void)flags;
  if (!out_alg || !alg_id) return 0xC000000Du;
  u32 h = 0;
  if (utf16_ieq_ascii(alg_id, "RNG")) h = BCRYPT_ALG_RNG;
  else if (utf16_ieq_ascii(alg_id, "SHA256")) h = BCRYPT_ALG_SHA256;
  else if (utf16_ieq_ascii(alg_id, "SHA1")) h = BCRYPT_ALG_SHA1;
  else if (utf16_ieq_ascii(alg_id, "MD5")) h = BCRYPT_ALG_MD5;
  if (!h) {
    char utf8[64];
    trt_utf16_to_utf8(alg_id, utf8, sizeof(utf8));
    char line[128];
    snprintf(line, sizeof(line),
             "bcrypt: BCryptOpenAlgorithmProvider('%s') -> STATUS_NOT_FOUND",
             utf8);
    log_line(line);
    *out_alg = nullptr;
    return 0xC0000225u;
  }
  *out_alg = (void*)(uintptr_t)h;
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptCloseAlgorithmProvider(void* alg, u32 flags) {
  (void)alg; (void)flags;
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptGenRandom(void* alg, u8* buf, u32 len, u32 flags) {
  const u32 h = (u32)(uintptr_t)alg;
  if (!buf && len > 0) return 0xC000000Du;
  if (alg && h != BCRYPT_ALG_RNG && !(flags & 0x2u)) return 0xC0000008u;
  trt_csprng_fill(buf, len);
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptGetProperty(void* obj, const u16* prop, u8* out,
                                        u32 cb_out, u32* cb_res, u32 flags) {
  (void)flags;
  if (!prop) return 0xC000000Du;
  u32 val = 0;
  const u32 h = (u32)(uintptr_t)obj;
  u32 alg = alg_handle_to_hash(h);
  if (!alg && (h >> 16) == 0xBC02u) {
    const u32 idx = h & 0xFFFFu;
    if (idx < 64 && g_bcrypt_hashes[idx].used) alg = g_bcrypt_hashes[idx].ctx.alg;
  }
  if (utf16_ieq_ascii(prop, "HashDigestLength")) {
    val = trt_hash_digest_len(alg);
    if (!val) return 0xC00000BBu;
  } else if (utf16_ieq_ascii(prop, "ObjectLength")) {
    val = (u32)sizeof(trt_hash_ctx);
  } else {
    return 0xC00000BBu;
  }
  if (cb_res) *cb_res = 4;
  if (!out) return 0;
  if (cb_out < 4) return 0xC0000023u;
  memcpy(out, &val, 4);
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptCreateHash(void* alg, void** out_hash,
                                       u8* hash_obj, u32 cb_hash_obj,
                                       u8* secret, u32 cb_secret, u32 flags) {
  (void)hash_obj; (void)cb_hash_obj; (void)secret; (void)cb_secret; (void)flags;
  const u32 ha = alg_handle_to_hash((u32)(uintptr_t)alg);
  if (!out_hash || !ha) return 0xC000000Du;
  for (u32 i = 0; i < 64; ++i) {
    if (!g_bcrypt_hashes[i].used) {
      g_bcrypt_hashes[i].used = true;
      trt_hash_init(&g_bcrypt_hashes[i].ctx, ha);
      *out_hash = (void*)(uintptr_t)(0xBC020000u | i);
      return 0;
    }
  }
  return 0xC000009Au;
}

u32 TRT_GUEST_ABI trt_BCryptHashData(void* hash, const u8* in, u32 cb_in,
                                     u32 flags) {
  (void)flags;
  const u32 h = (u32)(uintptr_t)hash;
  if ((h >> 16) != 0xBC02u || (h & 0xFFFFu) >= 64 ||
      !g_bcrypt_hashes[h & 0xFFFFu].used)
    return 0xC0000008u;
  trt_hash_update(&g_bcrypt_hashes[h & 0xFFFFu].ctx, in, cb_in);
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptFinishHash(void* hash, u8* out, u32 cb_out,
                                       u32 flags) {
  (void)flags;
  const u32 h = (u32)(uintptr_t)hash;
  if ((h >> 16) != 0xBC02u || (h & 0xFFFFu) >= 64 ||
      !g_bcrypt_hashes[h & 0xFFFFu].used)
    return 0xC0000008u;
  trt_hash_ctx* ctx = &g_bcrypt_hashes[h & 0xFFFFu].ctx;
  const u32 need = trt_hash_digest_len(ctx->alg);
  if (!out || cb_out < need) return 0xC0000023u;
  trt_hash_final(ctx, out);
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptDestroyHash(void* hash) {
  const u32 h = (u32)(uintptr_t)hash;
  if ((h >> 16) != 0xBC02u || (h & 0xFFFFu) >= 64) return 0xC0000008u;
  g_bcrypt_hashes[h & 0xFFFFu].used = false;
  return 0;
}

u32 TRT_GUEST_ABI trt_BCryptHash(void* alg, u8* secret, u32 cb_secret,
                                 const u8* in, u32 cb_in, u8* out, u32 cb_out) {
  (void)secret; (void)cb_secret;
  const u32 ha = alg_handle_to_hash((u32)(uintptr_t)alg);
  if (!ha || !out || cb_out < trt_hash_digest_len(ha)) return 0xC000000Du;
  trt_hash_ctx ctx;
  trt_hash_init(&ctx, ha);
  trt_hash_update(&ctx, in, cb_in);
  trt_hash_final(&ctx, out);
  return 0;
}

int trt_bridge_register_bcrypt(void) {
  static const trt_named_fn k_bcrypt_fns[] = {
      {"BCryptOpenAlgorithmProvider", (void*)&trt_BCryptOpenAlgorithmProvider},
      {"BCryptCloseAlgorithmProvider", (void*)&trt_BCryptCloseAlgorithmProvider},
      {"BCryptGenRandom", (void*)&trt_BCryptGenRandom},
      {"BCryptGetProperty", (void*)&trt_BCryptGetProperty},
      {"BCryptCreateHash", (void*)&trt_BCryptCreateHash},
      {"BCryptHashData", (void*)&trt_BCryptHashData},
      {"BCryptFinishHash", (void*)&trt_BCryptFinishHash},
      {"BCryptDestroyHash", (void*)&trt_BCryptDestroyHash},
      {"BCryptHash", (void*)&trt_BCryptHash},
  };
  trt_module_register("bcrypt.dll", TRT_ROUTE_BUILTIN, k_bcrypt_fns,
                      (int)(sizeof(k_bcrypt_fns) / sizeof(k_bcrypt_fns[0])));
  return 0;
}

void* TRT_GUEST_ABI crypt_CertOpenSystemStoreW(void* hprov, const u16* name) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenSystemStoreW -> NULL (no cert store on console; "
           "store=%s)", name ? "named" : "default");
  log_line(line);
  return nullptr;
}

void* TRT_GUEST_ABI crypt_CertOpenSystemStoreA(void* hprov, const char* name) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenSystemStoreA -> NULL (no cert store on console; "
           "store=%s)", name ? name : "default");
  log_line(line);
  return nullptr;
}

void* TRT_GUEST_ABI crypt_CertOpenStore(const void* provider, u32 encoding,
                                        void* hprov, u32 flags, const void* para) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenStore -> NULL (no cert store on console)");
  log_line(line);
  (void)provider; (void)encoding; (void)hprov; (void)flags; (void)para;
  return nullptr;
}

int TRT_GUEST_ABI crypt_CertCloseStore(void* store, u32 flags) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertCloseStore -> TRUE (no store was ever open)");
  log_line(line);
  (void)store; (void)flags;
  return 1;
}

int TRT_GUEST_ABI crypt_CertAddEncodedCertificateToStore(void* store,
                                                         u32 encoding,
                                                         const u8* cert,
                                                         u32 clen, u32 add,
                                                         void** ctx) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertAddEncodedCertificateToStore -> FALSE (no cert store "
           "on console)");
  log_line(line);
  (void)store; (void)encoding; (void)cert; (void)clen; (void)add;
  if (ctx) *ctx = nullptr;
  return 0;
}

void* TRT_GUEST_ABI crypt_CertGetCertificateChain(void* engine, void* leaf,
                                                  void* time, void* store,
                                                  void* para, u32 flags,
                                                  void* rsv) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertGetCertificateChain -> FALSE (no cert engine on "
           "console)");
  log_line(line);
  (void)engine; (void)leaf; (void)time; (void)store; (void)para; (void)flags;
  (void)rsv;
  return nullptr;
}

int TRT_GUEST_ABI crypt_CertVerifyCertificateChainPolicy(void* policy,
                                                         void* chain,
                                                         void* para,
                                                         void* status) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertVerifyCertificateChainPolicy -> FALSE (no chain was "
           "built: no cert engine)");
  log_line(line);
  (void)policy; (void)chain; (void)para; (void)status;
  return 0;
}

void TRT_GUEST_ABI crypt_CertFreeCertificateContext(void* ctx) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertFreeCertificateContext -> freed (nothing allocated)");
  log_line(line);
  (void)ctx;
}

void TRT_GUEST_ABI crypt_CertFreeCertificateChain(void* chain) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertFreeCertificateChain -> freed (nothing allocated)");
  log_line(line);
  (void)chain;
}

extern void TRT_STDCALL trt_SetLastError(u32 err);

void* TRT_GUEST_ABI crypt_CertEnumCertificatesInStore(void* store,
                                                     void* prev_ctx) {
  (void)store; (void)prev_ctx;
  log_line("crypt32: CertEnumCertificatesInStore -> NULL (empty store)");
  trt_SetLastError(0x80092004u);  // CRYPT_E_NOT_FOUND
  return nullptr;
}

int TRT_GUEST_ABI crypt_CertGetCertificateContextProperty(void* ctx, u32 prop,
                                                          void* data,
                                                          u32* cb_data) {
  (void)ctx; (void)prop; (void)data;
  if (cb_data) *cb_data = 0;
  log_line("crypt32: CertGetCertificateContextProperty -> FALSE (no context)");
  trt_SetLastError(0x80092004u);  // CRYPT_E_NOT_FOUND
  return 0;
}

int TRT_GUEST_ABI crypt_CryptBinaryToStringA(const u8* bin, u32 cb_bin,
                                           u32 flags, char* str, u32* pcch) {
  if (!pcch || (!bin && cb_bin > 0)) {
    trt_SetLastError(87);
    return 0;
  }
  const u32 fmt = flags & 0x0FFFFFFFu;
  const bool nocrlf = (flags & 0x40000000u) != 0 || (flags & 0x80000000u) != 0;
  if (fmt == 0x04u /*CRYPT_STRING_HEX*/ || fmt == 0x0Cu /*CRYPT_STRING_HEXRAW*/) {
    static const char k_hex[] = "0123456789abcdef";
    const bool spaces = (fmt == 0x04u);
    const u32 chars = cb_bin ? (cb_bin * (spaces ? 3u : 2u) - (spaces ? 1u : 0u)) : 0u;
    const u32 need = chars + (nocrlf ? 1u : 3u);
    if (!str) { *pcch = need; return 1; }
    if (*pcch < need) { *pcch = need; trt_SetLastError(234); return 0; }
    u32 pos = 0;
    for (u32 i = 0; i < cb_bin; ++i) {
      if (spaces && i > 0) str[pos++] = ' ';
      str[pos++] = k_hex[(bin[i] >> 4) & 0xFu];
      str[pos++] = k_hex[bin[i] & 0xFu];
    }
    if (!nocrlf) { str[pos++] = '\r'; str[pos++] = '\n'; }
    str[pos] = 0;
    *pcch = pos;
    return 1;
  }
  if (fmt == 0x01u /*CRYPT_STRING_BASE64*/ || fmt == 0x00u /*BASE64HEADER*/) {
    static const char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const u32 body_len = ((cb_bin + 2u) / 3u) * 4u;
    const char* hdr = (fmt == 0x00u) ? "-----BEGIN CERTIFICATE-----\r\n" : "";
    const char* ftr = (fmt == 0x00u) ? "-----END CERTIFICATE-----\r\n" : "";
    const u32 hl = (u32)strlen(hdr), fl = (u32)strlen(ftr);
    const u32 crlf_len = (nocrlf || fmt == 0x00u) ? (fmt == 0x00u ? 2u : 0u) : 2u;
    const u32 need = hl + body_len + crlf_len + fl + 1u;
    if (!str) { *pcch = need; return 1; }
    if (*pcch < need) { *pcch = need; trt_SetLastError(234); return 0; }
    u32 pos = 0;
    if (hl) { memcpy(str + pos, hdr, hl); pos += hl; }
    for (u32 i = 0; i < cb_bin; i += 3u) {
      const u32 b0 = bin[i];
      const u32 b1 = (i + 1u < cb_bin) ? bin[i + 1u] : 0u;
      const u32 b2 = (i + 2u < cb_bin) ? bin[i + 2u] : 0u;
      const u32 t = (b0 << 16) | (b1 << 8) | b2;
      str[pos++] = k_b64[(t >> 18) & 0x3Fu];
      str[pos++] = k_b64[(t >> 12) & 0x3Fu];
      str[pos++] = (i + 1u < cb_bin) ? k_b64[(t >> 6) & 0x3Fu] : '=';
      str[pos++] = (i + 2u < cb_bin) ? k_b64[t & 0x3Fu] : '=';
    }
    if (crlf_len) { str[pos++] = '\r'; str[pos++] = '\n'; }
    if (fl) { memcpy(str + pos, ftr, fl); pos += fl; }
    str[pos] = 0;
    *pcch = pos;
    return 1;
  }
  log_line("crypt32: CryptBinaryToStringA unsupported format -> FALSE");
  trt_SetLastError(87);
  return 0;
}

// CertFindCertificateInStore: no cert store exists on the console, so no
// certificate can ever match. NULL = "not found" (real Win32 semantics for an
// empty store); OpenSSL treats it as untrusted and fails TLS by name.
void* TRT_GUEST_ABI crypt_CertFindCertificateInStore(void* hCertStore,
                                                     u32 dwCertEncodingType,
                                                     u32 dwFindFlags,
                                                     u32 dwFindType,
                                                     const void* pvFindPara,
                                                     void* pPrevCertContext) {
  log_line("crypt32: CertFindCertificateInStore -> NULL (store is empty on "
           "console; TLS cert paths fail named)");
  return nullptr;
}

// CertDuplicateCertificateContext: no refcounted store exists on console; treat
// the input context as already owned and return it (identity duplicate).
void* TRT_GUEST_ABI crypt_CertDuplicateCertificateContext(void* pCertContext) {
  return pCertContext;
}

int trt_bridge_register_crypt32(void) {
  static const trt_named_fn k_fns[] = {
      {"CertOpenSystemStoreW", (void*)&crypt_CertOpenSystemStoreW},
      {"CertOpenSystemStoreA", (void*)&crypt_CertOpenSystemStoreA},
      {"CertOpenStore", (void*)&crypt_CertOpenStore},
      {"CertCloseStore", (void*)&crypt_CertCloseStore},
      {"CertAddEncodedCertificateToStore",
       (void*)&crypt_CertAddEncodedCertificateToStore},
      {"CertGetCertificateChain", (void*)&crypt_CertGetCertificateChain},
      {"CertVerifyCertificateChainPolicy",
       (void*)&crypt_CertVerifyCertificateChainPolicy},
      {"CertFreeCertificateContext", (void*)&crypt_CertFreeCertificateContext},
      {"CertDuplicateCertificateContext", (void*)&crypt_CertDuplicateCertificateContext},
      {"CertFindCertificateInStore",
       (void*)&crypt_CertFindCertificateInStore},
      {"CertFreeCertificateChain", (void*)&crypt_CertFreeCertificateChain},
      {"CertEnumCertificatesInStore",
       (void*)&crypt_CertEnumCertificatesInStore},
      {"CertGetCertificateContextProperty",
       (void*)&crypt_CertGetCertificateContextProperty},
      {"CryptBinaryToStringA", (void*)&crypt_CryptBinaryToStringA},
  };
  trt_module_register("crypt32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
