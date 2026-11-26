// tiramisu/src/host/shader_cache_util.h
// On-disk D3DCompile bytecode caching for GL, Vulkan, and D3D9 backends.
// Eliminates repetitive shader compile stutters on AMD Jaguar CPU cores.
#pragma once

#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "settings.h"

namespace longan {

inline uint64_t fnv1a_shader_key(const char* target, const char* src, size_t len) {
  uint64_t h = 0xcbf29ce484222325ull;
  if (target) {
    for (const char* p = target; *p; ++p) {
      h ^= (uint8_t)*p;
      h *= 0x100000001b3ull;
    }
  }
  if (src && len > 0) {
    for (size_t i = 0; i < len; ++i) {
      h ^= (uint8_t)src[i];
      h *= 0x100000001b3ull;
    }
  }
  return h;
}

inline bool d3d_compile_cached(const char* src, size_t len, const char* target,
                               const char* entry, ID3DBlob** out_code) {
  if (!src || !len || !out_code) return false;
  *out_code = nullptr;

  const char* cache_dir = trt_settings_shader_cache_dir();
  const bool cache_enabled = trt_settings_shader_cache();

  wchar_t cso_path[512] = {0};
  if (cache_enabled && cache_dir && *cache_dir) {
    uint64_t key = fnv1a_shader_key(target, src, len);
    swprintf(cso_path, 512, L"%hs\\hlsl_%016llx.cso", cache_dir, (unsigned long long)key);

    if (SUCCEEDED(D3DReadFileToBlob(cso_path, out_code)) && *out_code) {
      return true; // Cache hit: instant microsecond load from disk
    }
  }

  ID3DBlob* code = nullptr;
  ID3DBlob* errs = nullptr;
  const char* ep = (entry && *entry) ? entry : "main";
  HRESULT hr = D3DCompile(src, len, nullptr, nullptr, nullptr, ep, target, 0, 0, &code, &errs);
  if (errs) errs->Release();

  if (SUCCEEDED(hr) && code) {
    if (cache_enabled && cso_path[0]) {
      D3DWriteBlobToFile(code, cso_path, FALSE);
    }
    *out_code = code;
    return true;
  }
  return false;
}

} // namespace longan
