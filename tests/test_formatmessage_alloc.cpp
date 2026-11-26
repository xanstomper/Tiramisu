#include "../src/trt/trt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 TRT_STDCALL trt_FormatMessageA(u32, const void*, u32, u32, char*, u32, void*);
extern "C" u32 TRT_STDCALL trt_FormatMessageW(u32, const void*, u32, u32, u16*, u32, void*);
extern "C" void* TRT_STDCALL trt_LocalFree(void*);
extern "C" u32 TRT_STDCALL trt_GetLastError(void);

int main() {
  trt_backend backend{
      [](size_t n, u64) { return calloc(1, n); },
      [](void*, size_t, unsigned) { return 0; },
      [](void* p, size_t) { free(p); },
      [](const char*) {}, nullptr};
  trt_bridge_boot(&backend);

  constexpr u32 system = 0x1000;
  constexpr u32 allocate = 0x0100;
  constexpr unsigned long long guard = 0xa1b2c3d4e5f60718ULL;
  const char expected_a[] = "The system cannot find the file specified.\r\n";
  const char16_t expected_w[] = u"The system cannot find the file specified.\r\n";
  constexpr size_t length = sizeof(expected_a) - 1;
  int failures = 0;
  const auto check = [&](bool ok, const char* name) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
  };

  struct { u16* message; unsigned long long guard; } wide{nullptr, guard};
  const u32 wide_length = trt_FormatMessageW(
      system | allocate, nullptr, 2, 0, reinterpret_cast<u16*>(&wide.message), 0, nullptr);
  check(wide_length == length && wide.message &&
        memcmp(wide.message, expected_w, sizeof(expected_w)) == 0 && wide.guard == guard,
        "wide allocate-buffer preserves the output pointer and adjacent storage");
  if (wide.message) check(trt_LocalFree(wide.message) == nullptr, "wide result uses LocalAlloc");

  struct { char* message; unsigned long long guard; } ansi{nullptr, guard};
  const u32 ansi_length = trt_FormatMessageA(
      system | allocate, nullptr, 2, 0, reinterpret_cast<char*>(&ansi.message), 128, nullptr);
  check(ansi_length == length && ansi.message &&
        memcmp(ansi.message, expected_a, sizeof(expected_a)) == 0 && ansi.guard == guard,
        "ANSI allocate-buffer writes the result through the output pointer");
  if (ansi.message) check(trt_LocalFree(ansi.message) == nullptr, "ANSI result uses LocalAlloc");

  u16 fixed_w[sizeof(expected_a)]{};
  char fixed_a[sizeof(expected_a)]{};
  check(trt_FormatMessageW(system, nullptr, 2, 0, fixed_w, sizeof(expected_a), nullptr) == length &&
        memcmp(fixed_w, expected_w, sizeof(expected_w)) == 0, "wide fixed length fits exactly");
  check(trt_FormatMessageA(system, nullptr, 2, 0, fixed_a, sizeof(fixed_a), nullptr) == length &&
        memcmp(fixed_a, expected_a, sizeof(expected_a)) == 0, "ANSI fixed length fits exactly");

  u16 small_w[] = {9, 9, 9};
  char small_a[] = {9, 9, 9};
  check(trt_FormatMessageW(system, nullptr, 2, 0, small_w, 3, nullptr) == 0 &&
        trt_GetLastError() == 122 && small_w[0] == 9, "wide short buffer is rejected");
  check(trt_FormatMessageA(system, nullptr, 2, 0, small_a, 3, nullptr) == 0 &&
        trt_GetLastError() == 122 && small_a[0] == 9, "ANSI short buffer is rejected");
  check(trt_FormatMessageW(system | allocate, nullptr, 2, 0, nullptr, 0, nullptr) == 0 &&
        trt_GetLastError() == 87, "null output pointer is rejected");
  return failures ? 1 : 0;
}
