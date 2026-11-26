// tiramisu/src/trt/bridge_prng.cpp
// bcryptprimitives.dll!ProcessPrng.
//
// Modern runtimes (Bun/JavaScriptCore, V8, .NET) seed their per-process hash
// seeds and ASLR cookies through ProcessPrng instead of the legacy CryptoAPI.
// The loader used to synthesise a return-0 thunk for it, so every seed was
// zero and the runtime either collided on its own locks or aborted.  Forward
// to the host's system RNG (advapi32!SystemFunction036 = RtlGenRandom).
#include "trt.h"

#if defined(_WIN32)
extern "C" unsigned char __stdcall SystemFunction036(void* buf,
                                                     unsigned long len);
#endif

namespace {

using u8 = unsigned char;
using u64 = unsigned long long;

u8 TRT_STDCALL prng_ProcessPrng(u8* data, u64 len) {
  if (!data || len == 0) return 1;
#if defined(_WIN32)
  return SystemFunction036(data, (unsigned long)len) ? 1 : 0;
#else
  // Offline test build: xorshift is enough to keep seeds distinct.
  static u64 state = 0x9E3779B97F4A7C15ull;
  for (u64 i = 0; i < len; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    data[i] = (u8)(state >> 24);
  }
  return 1;
#endif
}

}  // namespace

extern "C" int trt_bridge_register_prng(void) {
  static const trt_named_fn k_fns[] = {
      {"ProcessPrng", (void*)&prng_ProcessPrng},
  };
  trt_module_register("bcryptprimitives.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}
