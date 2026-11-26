// tiramisu/tests/test_version.cpp
// version.dll bridge, standalone (no libtrt): the bridge TU is included
// directly and its runtime services are stubbed, so the real TRT_STDCALL
// entry points are exercised the way a guest's IAT would call them.
#include "../src/trt/bridge_version.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- stubs for the runtime services the bridge consumes --------------------
static u32 g_last_error = 0;
extern "C" {
void TRT_STDCALL trt_SetLastError(u32 e) { g_last_error = e; }
u32 TRT_STDCALL trt_GetLastError(void) { return g_last_error; }
void trt_core_log(const char*) {}
void trt_module_register(const char* name, trt_route, const trt_named_fn* fns, int n) {
  (void)name; (void)fns; (void)n;
}
// Map every guest path to the host file passed on argv[1] (set before run).
static const char* g_host_path = "corpus-local/ortmm/OrTMM.exe";
int trt_vfs_resolve_utf16(const u16* p, char* out, size_t cap) {
  (void)p;
  const size_t n = std::strlen(g_host_path);
  if (n + 1 > cap) return -1;
  std::memcpy(out, g_host_path, n + 1);
  return 0;
}
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (lasterr=%u)\n", __LINE__, #c, g_last_error); ++g_fail; } } while (0)

static u32 test_rd32(const unsigned char* p) { u32 v; std::memcpy(&v, p, 4); return v; }

int main(int argc, char** argv) {
  if (argc > 1) g_host_path = argv[1];

  {
    FILE* probe = std::fopen(g_host_path, "rb");
    if (!probe) {
      printf("test_version: SKIP (fixture not found: %s)\n", g_host_path);
      return 0;
    }
    std::fclose(probe);
  }

  trt_bridge_register_version();

  u32 handle = 123;
  u32 size = trt_GetFileVersionInfoSizeW((const u16*)u"Z:\\test.exe", &handle);
  printf("test_version: resource size = %u (host=%s)\n", size, g_host_path);
  CHECK(size > 0);
  CHECK(handle == 0);

  if (size == 0) { printf("test_version: %d FAILED (no version resource)\n", g_fail); return 1; }

  std::vector<unsigned char> buf(size + 16, 0);
  CHECK(trt_GetFileVersionInfoW((const u16*)u"Z:\\test.exe", 0, (u32)buf.size(), buf.data()) == 1);

  // VS_FIXEDFILEINFO is the value of the root block.
  void* value = nullptr;
  u32 len = 0;
  CHECK(trt_VerQueryValueW(buf.data(), (const u16*)u"\\", &value, &len) == 1);
  CHECK(value != nullptr);
  CHECK(len == 52);
  if (value) CHECK(test_rd32((const unsigned char*)value) == 0xFEEF04BDu);  // dwSignature

  // Missing subblock fails visibly.
  value = nullptr; len = 0;
  CHECK(trt_VerQueryValueW(buf.data(), (const u16*)u"\\NoSuchThing", &value, &len) == 0);

  // ANSI path variant.
  value = nullptr; len = 0;
  CHECK(trt_VerQueryValueA(buf.data(), "\\", &value, &len) == 1 && len == 52);

  // A missing file reports size 0 and a named error.
  g_last_error = 0;
  g_host_path = "/nonexistent/tiramisu_version_test.exe";
  CHECK(trt_GetFileVersionInfoSizeW((const u16*)u"Z:\\gone.exe", nullptr) == 0);

  // VerLanguageNameW fills a buffer.
  u16 lang[64] = {0};
  u32 n = trt_VerLanguageNameW(0x0409, lang, 64);
  CHECK(n > 0 && lang[0] != 0);

  printf(g_fail ? "test_version: %d FAILED\n" : "test_version: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
