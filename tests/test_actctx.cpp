// tiramisu/tests/test_actctx.cpp
// Activation contexts + .lnk reader, standalone (no libtrt): the bridge
// translation unit is included directly and its few runtime dependencies are
// stubbed, so the real k_* entry points are exercised exactly as a guest's IAT
// would call them (TRT_STDCALL = Win64 ABI).
#include "../src/trt/bridge_actctx.cpp"
#include "../src/trt/shell_link.cpp"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// ---- stubs for the runtime services the bridge consumes --------------------
static u32 g_last_error = 0;
static std::vector<std::string> g_registered;
extern "C" {
void TRT_STDCALL trt_SetLastError(u32 e) { g_last_error = e; }
void trt_core_log(const char*) {}
void trt_module_register(const char* name, trt_route, const trt_named_fn* fns, int n) {
  for (int i = 0; i < n; ++i) g_registered.push_back(std::string(name) + "!" + fns[i].name);
}
int trt_vfs_resolve_utf16(const u16* p, char* out, size_t cap) {
  std::string s;
  for (; *p; ++p) s += (char)*p;
  if (s.rfind("Z:\\", 0) == 0) s = "/tmp/" + s.substr(3);
  for (auto& c : s) if (c == '\\') c = '/';
  if (s.size() + 1 > cap) return -1;
  strcpy(out, s.c_str());
  return 0;
}
trt_image* trt_images_find(const char*) { return nullptr; }
void* trt_images_handle(trt_image*) { return nullptr; }
}

static const char* kManifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
    "  <assemblyIdentity type=\"win32\" name=\"Contoso.App\" version=\"1.2.3.4\" processorArchitecture=\"amd64\"/>\n"
    "  <dependency><dependentAssembly>\n"
    "    <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
    "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>\n"
    "  </dependentAssembly></dependency>\n"
    "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>\n"
    "    <requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"false\"/>\n"
    "  </requestedPrivileges></security></trustInfo>\n"
    "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\"><application>\n"
    "    <supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\n"
    "  </application></compatibility>\n"
    "  <application xmlns=\"urn:schemas-microsoft-com:asm.v3\"><windowsSettings>\n"
    "    <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">true/pm</dpiAware>\n"
    "    <longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>\n"
    "  </windowsSettings></application>\n"
    "</assembly>\n";

// resource stubs: module 0x1234 carries the manifest as RT_MANIFEST id 1
extern "C" {
void* TRT_STDCALL trt_FindResourceExW(void* hmod, const u16* type, const u16* name, u16) {
  if (hmod == (void*)0x1234 && (uintptr_t)type == 24 && (uintptr_t)name == 1) return (void*)0x77;
  return nullptr;
}
u32 TRT_STDCALL trt_SizeofResource(void*, void*) { return (u32)strlen(kManifest); }
void* TRT_STDCALL trt_LoadResource(void*, void*) { return (void*)kManifest; }
void* TRT_STDCALL trt_LockResource(void* g) { return g; }
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (lasterr=%u)\n", __LINE__, #c, g_last_error); ++g_fail; } } while (0)

static std::vector<u8> make_lnk() {
  std::vector<u8> b(0x4C, 0);
  u32 hs = 0x4C; memcpy(&b[0], &hs, 4);
  static const u8 clsid[16] = {1, 0x14, 2, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
  memcpy(&b[4], clsid, 16);
  u32 flags = 0x2 /*LinkInfo*/ | 0x4 /*name*/ | 0x20 /*args*/ | 0x10 /*workdir*/ | 0x80 /*unicode*/;
  memcpy(&b[20], &flags, 4);
  u32 show = 3; memcpy(&b[60], &show, 4);
  // LinkInfo: header 0x1C, then local base path "C:\\App\\run.exe\0", suffix "\0"
  const char* base = "C:\\App\\run.exe";
  u32 info_size = 0x1C + (u32)strlen(base) + 1 + 1;
  std::vector<u8> info(info_size, 0);
  u32 hdr = 0x1C, iflags = 1, off_base = 0x1C, off_suffix = 0x1C + (u32)strlen(base) + 1;
  memcpy(&info[0], &info_size, 4); memcpy(&info[4], &hdr, 4); memcpy(&info[8], &iflags, 4);
  memcpy(&info[16], &off_base, 4); memcpy(&info[24], &off_suffix, 4);
  memcpy(&info[0x1C], base, strlen(base) + 1);
  b.insert(b.end(), info.begin(), info.end());
  auto put = [&](const char16_t* s) {
    u16 n = 0; while (s[n]) ++n;
    b.insert(b.end(), (u8*)&n, (u8*)&n + 2);
    b.insert(b.end(), (const u8*)s, (const u8*)s + n * 2);
  };
  put(u"My App");        // name
  put(u"C:\\App");       // working dir (flag 0x10)
  put(u"--fullscreen");  // arguments (flag 0x20)
  return b;
}

int main() {
  trt_bridge_register_actctx();
  CHECK(g_registered.size() == 26);  // 13 fns x (kernel32 + kernelbase)

  // resource-backed context via HMODULE
  ActCtxW w{}; w.cbSize = sizeof(w); w.dwFlags = kFlagModule | kFlagResName;
  w.hModule = (void*)0x1234; w.lpResourceName = (const u16*)(uintptr_t)1;
  void* h = k_CreateActCtxW(&w);
  CHECK(h && h != (void*)~0ull);
  CHECK(trt_actctx_current_info(nullptr) == 0);

  u64 ck = 0;
  CHECK(k_ActivateActCtx(h, &ck) == 1);
  trt_actctx_info ai;
  CHECK(trt_actctx_current_info(&ai) == 1);
  CHECK(ai.common_controls_v6 == 1 && ai.long_path_aware == 1);
  CHECK(ai.run_level == 3 && ai.dependency_count == 1 && ai.dpi_aware == 1);

  void* cur = nullptr;
  CHECK(k_GetCurrentActCtx(&cur) == 1 && cur == h);
  k_ReleaseActCtx(cur);

  u32 run[3];
  u64 req = 0;
  CHECK(k_QueryActCtxW(kQueryUseActive, nullptr, nullptr, 5, run, sizeof(run), &req) == 1);
  CHECK(run[1] == 3 && run[2] == 0);
  CHECK(k_QueryActCtxW(kQueryUseActive, nullptr, nullptr, 5, run, 4, &req) == 0 &&
        g_last_error == kErrInsufficient && req == 12);
  u8 compat[8 + 32];
  CHECK(k_QueryActCtxW(kQueryUseActive, nullptr, nullptr, 6, compat, sizeof(compat), &req) == 1);
  CHECK(compat[0] == 1 && compat[8] == 0x12 && compat[9] == 0x7a && compat[10] == 0x0f && compat[11] == 0x8e);
  CHECK(k_QueryActCtxW(kQueryUseActive, nullptr, nullptr, 99, run, sizeof(run), &req) == 0 &&
        g_last_error == kErrNotSupported);

  u16 val[16];
  u64 wr = 0;
  const u16 key[] = {'l','o','n','g','P','a','t','h','A','w','a','r','e',0};
  CHECK(k_QueryActCtxSettingsW(0, nullptr, nullptr, key, val, 16, &wr) == 1 && wr == 5 && val[0] == 't');
  const u16 nokey[] = {'n','o','p','e',0};
  CHECK(k_QueryActCtxSettingsW(0, nullptr, nullptr, nokey, val, 16, &wr) == 0 &&
        g_last_error == kErrSxsKeyNotFound);

  // activation stack discipline
  u64 ck2 = 0;
  CHECK(k_ActivateActCtx(nullptr, &ck2) == 1);          // empty activation
  CHECK(trt_actctx_current_info(&ai) == 0);
  CHECK(k_DeactivateActCtx(0, ck) == 0 && g_last_error == kErrSxsEarlyDeact);
  CHECK(k_DeactivateActCtx(0, ck2) == 1);
  CHECK(k_DeactivateActCtx(0, ck) == 1);
  CHECK(k_DeactivateActCtx(0, ck) == 0 && g_last_error == kErrSxsInvalidDeact);
  k_ReleaseActCtx(h);
  CHECK(k_ActivateActCtx(h, &ck) == 0 && g_last_error == kErrInvalidHandle);

  // file-backed (UTF-8 manifest through the VFS) + failure modes
  FILE* f = fopen("/tmp/tiramisu_actctx_test.manifest", "wb");
  fwrite(kManifest, 1, strlen(kManifest), f); fclose(f);
  const u16 src[] = {'Z',':','\\','t','i','r','a','m','i','s','u','_','a','c','t','c','t','x','_','t','e','s','t','.','m','a','n','i','f','e','s','t',0};
  ActCtxW fw{}; fw.cbSize = sizeof(fw); fw.dwFlags = kFlagSetProcessDefault; fw.lpSource = src;
  void* fh = k_CreateActCtxW(&fw);
  CHECK(fh && fh != (void*)~0ull);
  CHECK(trt_actctx_current_info(&ai) == 1 && ai.common_controls_v6);  // process default
  const u16 missing[] = {'Z',':','\\','n','o','p','e','.','m','a','n','i','f','e','s','t',0};
  fw.dwFlags = 0; fw.lpSource = missing;
  CHECK(k_CreateActCtxW(&fw) == (void*)~0ull && g_last_error == kErrFileNotFound);
  CHECK(k_CreateActCtxW(nullptr) == (void*)~0ull && g_last_error == kErrInvalidParam);
  CHECK(k_FindActCtxSectionStringW(0, nullptr, 0, nullptr, nullptr) == 0 &&
        g_last_error == kErrSxsKeyNotFound);

  // process default via the loader helper
  trt_actctx_reset();
  CHECK(trt_actctx_install_process_default((void*)0x1234) != nullptr);
  CHECK(trt_actctx_install_process_default((void*)0x9999) == nullptr);
  CHECK(trt_actctx_current_info(&ai) == 1 && ai.long_path_aware);

  // .lnk
  std::vector<u8> lnk = make_lnk();
  trt_shelllink sl;
  CHECK(trt_shelllink_parse(lnk.data(), lnk.size(), &sl));
  CHECK(sl.target == u"C:\\App\\run.exe");
  CHECK(sl.description == u"My App" && sl.arguments == u"--fullscreen");
  CHECK(sl.working_dir == u"C:\\App" && sl.show_command == 3);
  CHECK(trt_shelllink_launch_target(sl, u"C:\\Users\\Player\\x.lnk") == u"C:\\App\\run.exe");
  std::vector<u8> bad = lnk; bad[5] ^= 0xFF;
  CHECK(!trt_shelllink_parse(bad.data(), bad.size(), &sl));
  std::vector<u8> trunc(lnk.begin(), lnk.begin() + 0x60);
  CHECK(!trt_shelllink_parse(trunc.data(), trunc.size(), &sl));
  trt_shelllink rel; rel.relative_path = u".\\bin\\a.exe";
  CHECK(trt_shelllink_launch_target(rel, u"C:\\Menu\\a.lnk") == u"C:\\Menu\\bin\\a.exe");

  printf(g_fail ? "test_actctx: %d FAILED\n" : "test_actctx: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
