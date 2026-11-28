// tiramisu/src/trt/bridge_crypt32.h
// crypt32 + bcrypt + shared CSPRNG / hash engine for the guest (bridge_crypt32.cpp).
//
// The console has no certificate store. CertOpenSystemStore/CertOpenStore
// fail (ERROR_INVALID_PARAMETER-class, named log: no cert store on console),
// chain verbs fail honestly, the *Free* verbs succeed (freeing nothing is
// not a lie). UnityPlayer probes the cert surface at boot; an honest FALSE
// sends it down its no-cert path instead of dying at load.
// bcrypt.dll provides BCryptGenRandom and MD5/SHA-1/SHA-256 hashing backed
// by OS CSPRNG (/dev/urandom on Linux, rand_s on UWP) + pure-C digest impls.
#ifndef TIRAMISU_BRIDGE_CRYPT32_H
#define TIRAMISU_BRIDGE_CRYPT32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register crypt32.dll as BUILTIN.
int trt_bridge_register_crypt32(void);
// Register bcrypt.dll as BUILTIN (also routes api-ms-win-security-basecrypt).
int trt_bridge_register_bcrypt(void);

// Retail DXBC container checksum of the bytes following the magic and digest.
void trt_dxbc_checksum(const u8* bytes, u32 size, u8 digest[16]);

// Shared CSPRNG + hash primitives reused by advapi32 (SystemFunction036 / CryptoAPI).
enum trt_hash_alg {
  TRT_HASH_NONE = 0,
  TRT_HASH_MD5 = 1,
  TRT_HASH_SHA1 = 2,
  TRT_HASH_SHA256 = 3,
};

struct trt_hash_ctx {
  u32 alg;
  u64 total_bytes;
  u32 state[8];
  u8  buf[64];
  u32 buf_len;
};

void trt_csprng_fill(u8* out, u32 len);
u32 trt_hash_digest_len(u32 alg);
void trt_hash_init(trt_hash_ctx* ctx, u32 alg);
void trt_hash_update(trt_hash_ctx* ctx, const u8* data, u32 len);
void trt_hash_final(trt_hash_ctx* ctx, u8* out);

#ifdef __cplusplus
}
#endif

#endif
