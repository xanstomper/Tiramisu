// tiramisu/tests/test_engine_synthesizer.cpp
// Verification of the Autonomous Engine Synthesizer & Dynamic API Synthesizer.
#include "../src/trt/engine_synthesizer.h"
#include "../src/host/steam_install.h"
#include "../src/trt/bridge_opengl32.h"
#include "../src/trt/dynamic_api.h"
#include "../src/trt/thunk.h"
#include "../src/trt/trt.h"
#include "../src/trt/pe.h"
#include "../src/trt/images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <filesystem>
#include <algorithm>

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      exit(1); \
    } else { \
      printf("  PASS: %s\n", msg); \
    } \
  } while (0)

extern "C" {
void* TRT_STDCALL trt_GetProcAddress(void* handle, const char* name);
u32 TRT_STDCALL trt_GetLastError();
}

namespace {

void* test_alloc(size_t bytes, u64 preferred_base) {
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? nullptr : p;
}

int test_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}

void test_release(void* base, size_t bytes) {
  munmap(base, bytes);
}

void make_fake_file(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
}

// Builds a minimal but structurally real PE whose import directory names the
// given modules, so the scanner is exercised on actual import tables rather
// than on file names.
void make_pe_with_imports(const std::string& path, uint16_t magic,
                          const std::vector<std::string>& modules,
                          uint32_t raw_ptr = 0x400,
                          const char* delay_module = nullptr) {
  const bool pe32 = (magic == 0x10b);
  const uint32_t name_rva = 0x1050;
  std::vector<unsigned char> buf(0x600, 0);

  buf[0] = 'M';
  buf[1] = 'Z';
  const uint32_t e_lfanew = 0x80;
  memcpy(&buf[0x3C], &e_lfanew, 4);

  buf[e_lfanew] = 'P';
  buf[e_lfanew + 1] = 'E';

  const uint16_t machine = pe32 ? TRT_PE_MACHINE_I386 : TRT_PE_MACHINE_X64;
  memcpy(&buf[e_lfanew + 4], &machine, 2);
  const uint16_t nsec = 1;
  const uint16_t opt_size = pe32 ? 224 : 240;
  memcpy(&buf[e_lfanew + 4 + 2], &nsec, 2);
  memcpy(&buf[e_lfanew + 4 + 16], &opt_size, 2);

  const size_t opt = e_lfanew + 24;
  memcpy(&buf[opt], &magic, 2);

  // data directory 1 = import table (RVA/size), at +96 (PE32) / +112 (PE64)
  const size_t dirs = opt + (pe32 ? 96 : 112);
  const uint32_t dir_count = 16;
  memcpy(&buf[dirs - 4], &dir_count, 4);
  const uint32_t imp_rva = 0x1000, imp_size = 3 * 20;
  memcpy(&buf[dirs + 1 * 8], &imp_rva, 4);
  memcpy(&buf[dirs + 1 * 8 + 4], &imp_size, 4);

  // one section mapping RVA 0x1000 -> file offset 0x400
  const size_t sec = opt + opt_size;
  memcpy(&buf[sec], ".idata", 6);
  const uint32_t vsize = 0x200, va = 0x1000, raw_size = 0x200;
  memcpy(&buf[sec + 8], &vsize, 4);
  memcpy(&buf[sec + 12], &va, 4);
  memcpy(&buf[sec + 16], &raw_size, 4);
  memcpy(&buf[sec + 20], &raw_ptr, 4);

  // import descriptors: module name RVAs advance by one 16-byte slot
  for (size_t i = 0; i < modules.size() && i < 8; ++i) {
    const size_t d = 0x400 + i * 20;
    const uint32_t n = (uint32_t)(name_rva + i * 16);
    memcpy(&buf[d + 12], &n, 4);
    const size_t noff = 0x400 + (n - va);
    if (noff + modules[i].size() + 1 < buf.size()) {
      memcpy(&buf[noff], modules[i].c_str(), modules[i].size());
    }
  }

  if (delay_module) {
    const uint32_t delay_rva = 0x1100;
    const uint32_t delay_size = 2 * sizeof(trt_delayload_descriptor);
    memcpy(&buf[dirs + TRT_PE_DIR_DELAY_IMPORT * 8], &delay_rva, 4);
    memcpy(&buf[dirs + TRT_PE_DIR_DELAY_IMPORT * 8 + 4], &delay_size, 4);
    trt_delayload_descriptor delay{};
    delay.attributes = 1;
    delay.name_rva = 0x1180;
    memcpy(&buf[0x500], &delay, sizeof(delay));
    CHECK(strlen(delay_module) < 0x80, "delay import fits in the fixture");
    memcpy(&buf[0x580], delay_module, strlen(delay_module));
  }

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(buf.data(), 1, 0x400, f);
  if (fseek(f, raw_ptr, SEEK_SET) == 0)
    fwrite(buf.data() + 0x400, 1, buf.size() - 0x400, f);
  fclose(f);
}

void make_fake_pe(const std::string& path, uint16_t magic) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;

  uint8_t buf[1024];
  memset(buf, 0, sizeof(buf));

  // DOS header
  buf[0] = 'M'; buf[1] = 'Z';
  uint32_t e_lfanew = 0x80;
  memcpy(buf + 0x3C, &e_lfanew, 4);

  // PE signature
  buf[e_lfanew] = 'P'; buf[e_lfanew + 1] = 'E';

  const uint16_t machine = magic == TRT_PE_OPT32_MAGIC ? TRT_PE_MACHINE_I386
                                                       : TRT_PE_MACHINE_X64;
  memcpy(buf + e_lfanew + 4, &machine, 2);
  const uint16_t opt_size = magic == TRT_PE_OPT32_MAGIC ? 224 : 240;
  memcpy(buf + e_lfanew + 4 + 16, &opt_size, 2);
  // Optional header starts at e_lfanew + 24
  memcpy(buf + e_lfanew + 24, &magic, 2);

  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
}

} // namespace

int main() {
  trt_backend be = {};
  be.alloc = test_alloc;
  be.protect = test_protect;
  be.release = test_release;
  be.log = [](const char* s) { printf("  [trt] %s\n", s); };
  trt_core_set_backend(&be);

  printf("== 1. Dynamic API Return Value & Arity Deduction ==\n");
  {
    uint64_t h = trt_dynamic_deduce_return_val("kernel32.dll", "CreateCustomDevice");
    CHECK(h != 0 && h != (uint64_t)-1, "CreateCustomDevice deduces valid pseudo-handle");
    CHECK((h & 0x77000000ULL) == 0x77000000ULL, "pseudo-handle is in 0x77000000 range");

    uint64_t b = trt_dynamic_deduce_return_val("user32.dll", "SetCustomFeatureEnabled");
    CHECK(b == 1, "SetCustomFeatureEnabled deduces TRUE (1)");

    uint64_t hr = trt_dynamic_deduce_return_val("d3d11.dll", "D3D11CustomCall");
    CHECK(hr == 0, "D3D11CustomCall deduces S_OK (0)");

    uint64_t v = trt_dynamic_deduce_return_val("kernel32.dll", "GetVersionExW");
    CHECK(v == 0x0A00, "GetVersionExW deduces Windows 10 version 0x0A00");

    int a1 = trt_dynamic_deduce_arg_count("kernel32.dll", "CloseHandle");
    CHECK(a1 == 1, "CloseHandle deduced arity 1");

    int a2 = trt_dynamic_deduce_arg_count("kernel32.dll", "GetProcAddress");
    CHECK(a2 == 2, "GetProcAddress deduced arity 2");

    int a7 = trt_dynamic_deduce_arg_count("kernel32.dll", "CreateFileW");
    CHECK(a7 == 7, "CreateFileW deduced arity 7");

    int a12 = trt_dynamic_deduce_arg_count("user32.dll", "CreateWindowExW");
    CHECK(a12 == 12, "CreateWindowExW deduced arity 12");
  }

  printf("== 2. Dynamic Thunk Synthesis & Invocation ==\n");
  {
    void* thunk = trt_dynamic_synthesize_api("mycustom.dll", "CreateMagicPortal", 0);
    CHECK(thunk != nullptr, "synthesized thunk is non-null");
    CHECK(trt_dynamic_is_synthesized("mycustom.dll", "CreateMagicPortal") == 1,
          "symbol marked as synthesized");

    // Call the thunk through a generic Win64 callable
    typedef uint64_t (TRT_STDCALL *portal_fn)(void);
    portal_fn pfn = (portal_fn)thunk;
    uint64_t res = pfn();
    CHECK((res & 0x77000000ULL) == 0x77000000ULL,
          "invoking synthesized thunk returns inferred pseudo-handle");
  }

  printf("== 2b. Dynamic lookup stays honest for missing exports ==\n");
  {
    CHECK(trt_resolve_module(nullptr, "mystery_middleware.dll", "MysteryInit", 0) == nullptr,
          "an unknown module lookup returns NULL (GetProcAddress contract)");

    trt_module_register("mystery_builtin.dll", TRT_ROUTE_BUILTIN, nullptr, 0);
    CHECK(trt_resolve_module(nullptr, "mystery_builtin.dll", "MissingExport", 0) == nullptr,
          "an unknown BUILTIN export lookup returns NULL");
  }

  printf("== 2c. Synthesized shims.txt is consumed by the loader ==\n");
  {
    std::string shim_dir = "tests/out/test_synth_shims";
    mkdir(shim_dir.c_str(), 0755);
    mkdir((shim_dir + "/.tiramisu").c_str(), 0755);
    make_fake_file(shim_dir + "/.tiramisu/shims.txt",
                   "# generated by engine synthesizer\nmycustom.dll:bridge_custom\n");
    trt_runtime_init(&be, shim_dir.c_str());
    CHECK(trt_module_route("mycustom.dll") == TRT_ROUTE_STUB,
          "module:bridge shim registers the module before load");
    void* shim = trt_resolve_module(nullptr, "mycustom.dll", "CreateMagicPortal", 0);
    CHECK(shim != nullptr, "a synthesized shim module binds instead of failing");
    CHECK(trt_dynamic_is_synthesized("mycustom.dll", "CreateMagicPortal") == 1,
          "the shim module is serviced by the dynamic synthesizer");
    const auto handle = trt_images_synthetic_handle("mycustom.dll");
    CHECK(handle && !trt_GetProcAddress(handle, "CreateMagicPortal") &&
              trt_GetLastError() == 127,
          "GetProcAddress fails a generated-only export and sets ERROR_PROC_NOT_FOUND");
    CHECK(!trt_runtime_export_resolver()->resolve(nullptr, "mycustom.dll",
                                                   "CreateMagicPortal", 0),
          "the forwarder resolver also refuses a generated-only export");

    const trt_named_fn exports[] = {{"#23", reinterpret_cast<void*>(&trt_stub_thunk)}};
    trt_module_register("ordinal_fixture.dll", TRT_ROUTE_BUILTIN, exports, 1);
    const auto ordinal_handle = trt_images_synthetic_handle("ordinal_fixture.dll");
    CHECK(trt_GetProcAddress(ordinal_handle, reinterpret_cast<const char*>(23)) ==
              reinterpret_cast<void*>(&trt_stub_thunk),
          "a registered export can be looked up by ordinal on a synthetic module");
  }

  printf("== 3. Autonomous Engine Introspection & Profile Synthesis ==\n");
  {
    std::string test_dir = "tests/out/test_synth_unity";
    mkdir("tests/out", 0755);
    mkdir(test_dir.c_str(), 0755);
    unlink((test_dir + "/steam_appid.txt").c_str());
    unlink((test_dir + "/steam_emu.ini").c_str());

    // Create Unity IL2CPP file signatures
    make_fake_pe(test_dir + "/Game.exe", 0x20b); // PE64
    make_fake_file(test_dir + "/UnityPlayer.dll", "fake unity player");
    make_fake_file(test_dir + "/GameAssembly.dll", "fake il2cpp assembly");
    make_fake_file(test_dir + "/steam_api64.dll", "fake steam");

    trt_engine_profile prof;
    bool ok = trt_synthesize_engine(test_dir.c_str(), nullptr, &prof);
    CHECK(ok, "engine synthesis succeeded for Unity IL2CPP game");
    CHECK(prof.engine_type == TRT_ENGINE_UNITY_IL2CPP, "classified as Unity IL2CPP");
    CHECK(prof.is_pe64 && !prof.is_pe32, "detected PE64 architecture");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D11, "preferred graphics is D3D11");
    CHECK(prof.has_steam, "detected Steam API dependency");
    CHECK(prof.steam_appid == 0, "Steam integration does not invent an AppID");
    const std::string appid_file = test_dir + "/steam_appid.txt";
    CHECK(access(appid_file.c_str(), F_OK) != 0, "synthesis does not create an AppID file");
    make_fake_file(test_dir + "/steam_emu.ini", "AppId=123456\n");
    CHECK(trt_synthesize_engine(test_dir.c_str(), nullptr, &prof) && prof.steam_appid == 0,
          "emulator configuration cannot establish the Steam identity");
    make_fake_file(appid_file, "480\n");
    CHECK(trt_synthesize_engine(test_dir.c_str(), nullptr, &prof) && prof.steam_appid == 0,
          "legacy Spacewar placeholder does not authorize another title");
    make_fake_file(appid_file, "526870\r\n");
    CHECK(trt_synthesize_engine(test_dir.c_str(), nullptr, &prof) && prof.steam_appid == 526870,
          "explicit install metadata supplies the Steam AppID");
    unlink(appid_file.c_str());
    unlink((test_dir + "/steam_emu.ini").c_str());

    // Check that .tiramisu/profile.json was created
    FILE* pf = fopen((test_dir + "/.tiramisu/profile.json").c_str(), "r");
    CHECK(pf != nullptr, ".tiramisu/profile.json was auto-generated");
    if (pf) fclose(pf);

    // Verify command line arguments
    bool has_force_d3d11 = false;
    for (const auto& arg : prof.cmdline_args) {
      if (arg == "-force-d3d11") has_force_d3d11 = true;
    }
    CHECK(has_force_d3d11, "synthesized -force-d3d11 command line argument");
    CHECK(trt_autonomous_provision_runtime(&be, &prof) == 0,
          "autonomous runtime provisioning succeeds for the synthesized profile");
  }

  printf("== 4. Engine Synthesis on 32-bit GameMaker Game ==\n");
  {
    std::string gm_dir = "tests/out/test_synth_gm";
    mkdir(gm_dir.c_str(), 0755);

    make_fake_pe(gm_dir + "/RetroGame.exe", 0x10b); // PE32
    make_fake_file(gm_dir + "/data.win", "gamemaker bytecode data");
    make_fake_file(gm_dir + "/audiogroup1.dat", "audio data");

    trt_engine_profile prof;
    bool ok = trt_synthesize_engine(gm_dir.c_str(), nullptr, &prof);
    CHECK(ok, "engine synthesis succeeded for GameMaker game");
    CHECK(prof.engine_type == TRT_ENGINE_GAMEMAKER, "classified as GameMaker");
    CHECK(prof.is_pe32 && !prof.is_pe64, "detected 32-bit PE32 architecture");
  }

  printf("== 5. Real PE import-table scanning ==\n");
  {
    std::string imp_dir = "tests/out/test_synth_imports";
    mkdir(imp_dir.c_str(), 0755);
    const std::string exe = imp_dir + "/Game.exe";
    make_pe_with_imports(exe, 0x10b, {"D3D11.dll", "steam_api.dll"});

    char csv[512] = {};
    int n = trt_pe_scan_import_modules(exe.c_str(), csv, sizeof(csv));
    CHECK(n == 2, "import scanner found exactly 2 modules");
    CHECK(strcmp(csv, "d3d11.dll,steam_api.dll") == 0,
          "scanner reports lower-case module CSV");
    CHECK(trt_pe_scan_import_modules("tests/out/does_not_exist.exe", csv, sizeof(csv)) == -1,
          "missing file reports -1, never a fake success");

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(imp_dir.c_str(), nullptr, &prof),
          "synthesis succeeded from a real import table");
    CHECK(prof.is_pe32 && !prof.is_pe64, "import fixture is PE32");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D11, "d3d11.dll import selects D3D11");
    CHECK(prof.has_steam, "steam_api.dll import flags Steam");
    CHECK(prof.imported_modules.size() == 2, "profile carries the imported modules");

    bool has_d3d11_shim = false, has_steam_shim = false;
    for (const auto& s : prof.synthetic_shims) {
      if (s == "d3d11.dll:bridge_d3d") has_d3d11_shim = true;
      if (s == "steam_api64.dll:bridge_steam") has_steam_shim = true;
    }
    CHECK(has_d3d11_shim, "synthesized the D3D11 bridge shim");
    CHECK(has_steam_shim, "synthesized the Steam bridge shim");

    std::ifstream pj(imp_dir + "/.tiramisu/profile.json");
    std::string js((std::istreambuf_iterator<char>(pj)), std::istreambuf_iterator<char>());
    CHECK(js.find("\"imports\"") != std::string::npos &&
              js.find("d3d11.dll") != std::string::npos,
          "profile.json records the real imports");
    CHECK(js.find("\"shims\"") != std::string::npos, "profile.json records the shims");
    CHECK(js.find("\"architecture\": \"PE32 (x86)\"") != std::string::npos,
          "profile.json records the architecture");

    std::ifstream sh(imp_dir + "/.tiramisu/shims.txt");
    CHECK(sh.good(), "shims.txt was written");
  }

  printf("== Bundled engine DLL import scanning ==\n");
  {
    const std::string root = "tests/out/test_synth_bundle";
    const std::string bin = root + "/Bin";
    mkdir(root.c_str(), 0755);
    mkdir(bin.c_str(), 0755);
    const auto exe = bin + "/Game.exe";
    make_pe_with_imports(exe, 0x20b, {"UnityPlayer.dll", "WrongArch.dll"});
    make_pe_with_imports(bin + "/UnityPlayer.dll", 0x20b, {"gamegfx.dll", "OpenAL32.dll"});
    make_pe_with_imports(root + "/GameGfx.dll", 0x20b,
                         {"UnityPlayer.dll", "d3d12.dll", "dxgi.dll"}, 160u << 20);
    make_pe_with_imports(bin + "/WrongArch.dll", 0x10b, {"d3d11.dll"});
    make_pe_with_imports(bin + "/Unused.dll", 0x20b, {"vulkan-1.dll"});

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(root.c_str(), "Bin/Game.exe", &prof),
          "synthesis follows DLL imports across the executable and game directories");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D12 && prof.preferred_audio == TRT_AUDIO_OPENAL,
          "DLL imports select D3D12 and OpenAL without treating DXGI as D3D11");
    const auto has_import = [&](const std::string& name) {
      return std::binary_search(prof.imported_modules.begin(), prof.imported_modules.end(), name);
    };
    CHECK(has_import("d3d12.dll") && has_import("openal32.dll") &&
              !has_import("d3d11.dll") && !has_import("vulkan-1.dll"),
          "wrong architecture and unreferenced DLLs do not affect launch imports");
    CHECK(std::count(prof.imported_modules.begin(), prof.imported_modules.end(),
                     "unityplayer.dll") == 1,
          "cyclic imports are recorded once");
    CHECK(std::find(prof.cmdline_args.begin(), prof.cmdline_args.end(),
                    "-force-d3d11") == prof.cmdline_args.end(),
          "Unity launch does not force a renderer absent from the dependency chain");
  }

  printf("== Steam hint outside the executable discovery scan ==\n");
  {
    const std::string root = "tests/out/test_synth_deep_hint";
    const std::string bin = "Content/Windows/Engine/Runtime/Build/Client/Win64";
    std::filesystem::create_directories(root + "/" + bin);
    std::filesystem::create_directories(root + "/Unrelated");
    make_pe_with_imports(root + "/Helper.exe", 0x20b, {"d3d11.dll"});
    make_pe_with_imports(root + "/" + bin + "/Game.exe", 0x20b, {"Render.dll"});
    make_pe_with_imports(root + "/" + bin + "/Render.DLL", 0x20b,
                         {"d3d12.dll", "Audio.dll"});
    make_pe_with_imports(root + "/Audio.dll", 0x20b, {"openal32.dll"});
    make_pe_with_imports(root + "/Unrelated/Render.dll", 0x20b, {"vulkan-1.dll"});

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(root.c_str(), (bin + "/Game.exe").c_str(), &prof),
          "Steam launch hint selects a valid executable beyond discovery depth");
    CHECK(prof.main_exe_rel == bin + "/Game.exe", "root helper is not selected");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D12 && prof.preferred_audio == TRT_AUDIO_OPENAL,
          "deep executable finds its renderer DLL and the game-root audio DLL");
    CHECK(!std::binary_search(prof.imported_modules.begin(), prof.imported_modules.end(),
                              "vulkan-1.dll"),
          "unrelated DLL of the same name does not affect launch imports");
  }

  printf("== 6. D3D9-only title resolves to the D3D9 bridge ==\n");
  {
    std::string d9_dir = "tests/out/test_synth_d3d9";
    mkdir(d9_dir.c_str(), 0755);
    make_pe_with_imports(d9_dir + "/OldGame.exe", 0x10b, {"d3d9.dll", "dsound.dll"});

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(d9_dir.c_str(), nullptr, &prof), "D3D9 synthesis succeeded");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D9, "d3d9.dll import selects the D3D9 bridge");
  }

  printf("== 7. options.ini alone is not GameMaker ==\n");
  {
    std::string opt_dir = "tests/out/test_synth_options";
    mkdir(opt_dir.c_str(), 0755);
    make_fake_pe(opt_dir + "/PlainGame.exe", 0x20b);
    make_fake_file(opt_dir + "/options.ini", "[video]\nwidth=1280\n");

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(opt_dir.c_str(), nullptr, &prof), "plain game synthesis succeeded");
    CHECK(prof.engine_type != TRT_ENGINE_GAMEMAKER,
          "a generic options.ini no longer misclassifies as GameMaker");
    // Policy (host/main.cpp has a dedicated TRT_ENGINE_UNIVERSAL launch path):
    // an unmanaged binary with no engine evidence is the universal guest.
    // Custom Native is for binaries that carry evidence of an in-house engine,
    // not for "the scanner found nothing".
    CHECK(prof.engine_type == TRT_ENGINE_UNIVERSAL,
          "no engine evidence resolves to the universal guest");
    CHECK((prof.universal_flags & TRT_UNIV_CLEAR_PC) != 0,
          "universal classification sets the PC clear flag");
  }

  printf("== 8. Engine byte-signature detection (stripped imports) ==\n");
  {
    std::string ue_dir = "tests/out/test_synth_ue";
    mkdir(ue_dir.c_str(), 0755);
    make_fake_pe(ue_dir + "/Shipped.exe", 0x20b);
    // A shipping exe whose engine is only visible as its own build tag, the
    // case that file names and imports both miss.
    FILE* af = fopen((ue_dir + "/Shipped.exe").c_str(), "ab");
    CHECK(af != nullptr, "marker exe opened for append");
    if (af) {
      const char* tag = "++UE4+Release-4.27-CL-18319896\0";
      fwrite(tag, 1, strlen(tag) + 1, af);
      fclose(af);
    }

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(ue_dir.c_str(), nullptr, &prof),
          "signature-only synthesis succeeded");
    CHECK(prof.engine_type == TRT_ENGINE_UNREAL,
          "++UE4+Release tag classifies as Unreal");
    bool has_windowed = false;
    for (const auto& a : prof.cmdline_args) {
      if (a == "-windowed") has_windowed = true;
    }
    CHECK(has_windowed, "Unreal profile carries -windowed");
  }

  printf("== 9. Universal COM Object Interface Dispatch ==\n");
  {
    void* com_obj = trt_dynamic_universal_com_object();
    CHECK(com_obj != nullptr, "universal COM object is non-null");

    void** vtbl = *(void***)com_obj;
    CHECK(vtbl != nullptr, "universal COM object has valid vtable pointer");

    // Slot 0: QueryInterface
    typedef int (TRT_GUEST_ABI *qi_fn)(void*, const void*, void**);
    qi_fn qi = (qi_fn)vtbl[0];
    void* queried = nullptr;
    int rc_qi = qi(com_obj, nullptr, &queried);
    CHECK(rc_qi == 0, "QueryInterface returned S_OK (0)");
    CHECK(queried == com_obj, "QueryInterface returned universal COM object pointer");

    // Slot 1: AddRef
    typedef uint32_t (TRT_GUEST_ABI *addref_fn)(void*);
    addref_fn addref = (addref_fn)vtbl[1];
    CHECK(addref(com_obj) == 2, "AddRef returned 2");

    // Slot 2: Release
    typedef uint32_t (TRT_GUEST_ABI *release_fn)(void*);
    release_fn release = (release_fn)vtbl[2];
    CHECK(release(com_obj) == 1, "Release returned 1");

    // Slot 3 & 255: Arbitrary interface method slots
    typedef int (TRT_GUEST_ABI *method_fn)(void*, void*, void*, void*);
    method_fn m3 = (method_fn)vtbl[3];
    CHECK(m3(com_obj, nullptr, nullptr, nullptr) == 0, "method slot 3 returned S_OK (0)");

    method_fn m255 = (method_fn)vtbl[255];
    CHECK(m255(com_obj, nullptr, nullptr, nullptr) == 0, "method slot 255 returned S_OK (0)");
  }

  printf("== 10. Extended Game Middleware Return Value Deduction ==\n");
  {
    CHECK(trt_dynamic_deduce_return_val("fmod.dll", "FMOD_System_Create") == 0, "FMOD_System_Create -> FMOD_OK (0)");
    CHECK(trt_dynamic_deduce_return_val("AkSoundEngine.dll", "AK::SoundEngine::Init") == 1, "AK::SoundEngine::Init -> AK_Success (1)");
    CHECK(trt_dynamic_deduce_return_val("steam_api64.dll", "SteamAPI_Init") == 1, "SteamAPI_Init -> true (1)");
    CHECK(trt_dynamic_deduce_return_val("steam_api64.dll", "SteamAPI_RestartAppIfNecessary") == 0, "SteamAPI_RestartAppIfNecessary -> false (0)");
    CHECK(trt_dynamic_deduce_return_val("steam_api64.dll", "SteamInternal_CreateInterface") == (uint64_t)(uintptr_t)trt_dynamic_universal_com_object(), "SteamInternal_CreateInterface -> COM object pointer");
    CHECK(trt_dynamic_deduce_return_val("steam_api64.dll", "SteamFriends") == (uint64_t)(uintptr_t)trt_dynamic_universal_com_object(), "SteamFriends -> COM object pointer");
    CHECK(trt_dynamic_deduce_return_val("amd_ags_x64.dll", "agsInitialize") == 0, "agsInitialize -> AGS_SUCCESS (0)");
    CHECK(trt_dynamic_deduce_return_val("GFSDK_SSAO_D3D11.win64.dll", "GFSDK_SSAO_D3D11_CreateContext") == 0, "GFSDK_SSAO_D3D11_CreateContext -> GFSDK_OK (0)");
    CHECK(trt_dynamic_deduce_return_val("crashpad.dll", "Crashpad_Initialize") == 0, "Crashpad_Initialize -> 0");
    CHECK(trt_dynamic_deduce_return_val("EOSSDK-Win64-Shipping.dll", "EOS_Initialize") == 0, "EOS_Initialize -> EOS_Success (0)");
    CHECK(trt_dynamic_deduce_return_val("discord_game_sdk.dll", "DiscordCreate") == 0, "DiscordCreate -> DiscordResult_Ok (0)");
    CHECK(trt_dynamic_deduce_return_val("nvapi64.dll", "NvAPI_Initialize") == 0, "NvAPI_Initialize -> NVAPI_OK (0)");
    CHECK(trt_dynamic_deduce_return_val("kernel32.dll", "VirtualAlloc") != 0, "VirtualAlloc -> valid memory handle");
    CHECK(trt_dynamic_deduce_return_val("kernel32.dll", "VirtualFree") == 1, "VirtualFree -> TRUE (1)");
  }

  printf("== 11. Runtime Provisioning & Provider Lookup ==\n");
  {
    trt_engine_profile test_prof;
    test_prof.game_name = "SyntheticGame";
    test_prof.engine_type = TRT_ENGINE_CUSTOM_NATIVE;
    test_prof.preferred_gfx = TRT_GFX_VULKAN;
    test_prof.preferred_audio = TRT_AUDIO_WWISE;
    test_prof.imported_modules = {"unknown_physics_engine.dll", "aksoundengine.dll", "my_custom_telemetry.dll"};

    int rc = trt_autonomous_provision_runtime(&be, &test_prof);
    CHECK(rc == 0, "trt_autonomous_provision_runtime succeeded");

    static int host_symbol;
    trt_set_system_loader(+[](const char* module, const char* symbol) -> void* {
      if (symbol && strcmp(module, "unknown_physics_engine.dll") == 0 &&
          strcmp(symbol, "Init") == 0)
        return &host_symbol;
      return nullptr;
    });
    trt_resolver resolver{};
    trt_resolver_default(&resolver);
    void* resolved = resolver.resolve(resolver.user, "unknown_physics_engine.dll", "Init", 0);
    trt_set_system_loader(nullptr);
    CHECK(resolved == &host_symbol, "the static binder can find an available host provider");
    CHECK(trt_module_route("unknown_physics_engine.dll") == TRT_ROUTE_MISSING,
          "an uncataloged import does not register a global placeholder");
    CHECK(trt_module_route("aksoundengine.dll") == TRT_ROUTE_BUILTIN,
          "known Wwise module stays on the real bridge, never downgraded to STUB");

    // A BUILTIN route is not the same as a real implementation. These three
    // bridges return success but decode nothing; the coverage report must say
    // NAMED_SILENT or a title can read as fully supported while silent.
    int impl_cls = -1;
    CHECK(trt_symbol_class("aksoundengine.dll", "AK_SoundEngine_Init", &impl_cls) == 1 &&
              impl_cls == TRT_IMPL_NAMED_SILENT,
          "Wwise bridge is classified NAMED_SILENT, not REAL");
    CHECK(trt_symbol_class("bink2w64.dll", "BinkOpen", &impl_cls) == 1 &&
              impl_cls == TRT_IMPL_NAMED_SILENT,
          "Bink fallback is classified NAMED_SILENT, not REAL");
    CHECK(trt_symbol_class("mss32.dll", "AIL_startup", &impl_cls) == 1 &&
              impl_cls == TRT_IMPL_NAMED_SILENT,
          "Miles fallback is classified NAMED_SILENT, not REAL");

    CHECK(trt_module_route("my_custom_telemetry.dll") == TRT_ROUTE_MISSING,
          "an unavailable module stays missing");

    // These are the modules a profile can select before its imports bind.
    const char* kProvisionedModules[] = {
        "d3d11.dll", "d3d12.dll", "d3d9.dll", "vulkan-1.dll",
        "opengl32.dll", "xaudio2.dll", "fmod.dll", "dsound.dll",
        "openal32.dll", "aksoundengine.dll"};
    for (const char* mod : kProvisionedModules) {
      CHECK(trt_module_route(mod) != TRT_ROUTE_MISSING,
            "provisioning leaves the graphics/audio module bound");
    }

    // Dynamic lookup must still report missing modules and exports.
    CHECK(trt_resolve_module(nullptr, "never_provisioned_middleware.dll", "Init", 0) == nullptr,
          "unprovisioned module lookup stays NULL for GetProcAddress");
    CHECK(trt_resolve_module(nullptr, "aksoundengine.dll", "AK::SoundEngine::Init", 0) == nullptr,
          "missing Wwise export lookup stays NULL for GetProcAddress");
  }

  printf("== 12. Single-File Engine Synthesis & Extended API Deductions ==\n");
  {
    std::string test_dir = "tests/out/test_synth_single";
    mkdir(test_dir.c_str(), 0755);
    std::string fake_game = test_dir + "/SingleGame.exe";
    make_pe_with_imports(fake_game, 0x20b, {"d3d12.dll", "xaudio2_9.dll", "steam_api64.dll"});

    trt_engine_profile sf_prof;
    bool sf_ok = trt_synthesize_engine_for_file(fake_game.c_str(), &sf_prof);
    CHECK(sf_ok, "trt_synthesize_engine_for_file succeeded");
    CHECK(sf_prof.is_pe64, "detected PE64 binary");
    CHECK(sf_prof.preferred_gfx == TRT_GFX_D3D12, "preferred gfx is D3D12");
    CHECK(sf_prof.has_steam, "detected Steam integration");

    CHECK(!trt_synthesize_engine_for_file((test_dir + "/missing.exe").c_str(), &sf_prof),
          "single-file synthesis does not substitute a different executable");
    mkdir((test_dir + "/Bin").c_str(), 0755);
    make_fake_pe(test_dir + "/Bin/SingleGame.exe", TRT_PE_OPT32_MAGIC);
    CHECK(trt_synthesize_engine(test_dir.c_str(), "Bin\\SingleGame.exe", &sf_prof) &&
              sf_prof.is_pe32 && sf_prof.main_exe_rel == "Bin/SingleGame.exe",
          "the complete Steam path wins over a root executable with the same name");

    CHECK(trt_dynamic_deduce_return_val("kernel32.dll", "QueryPerformanceCounter") == 1,
          "QueryPerformanceCounter -> TRUE (1)");
    CHECK(trt_dynamic_deduce_return_val("kernel32.dll", "QueryPerformanceFrequency") == 1,
          "QueryPerformanceFrequency -> TRUE (1)");
    CHECK(trt_dynamic_deduce_return_val("user32.dll", "GetSystemMetrics") == 1080,
          "GetSystemMetrics -> 1080");
    CHECK(trt_dynamic_deduce_return_val("user32.dll", "GetClientRect") == 1,
          "GetClientRect -> TRUE (1)");
    CHECK(trt_dynamic_deduce_return_val("user32.dll", "AdjustWindowRectEx") == 1,
          "AdjustWindowRectEx -> TRUE (1)");
  }

  printf("== 13. Middleware Routes & Module Alias Tests ==\n");
  {
    trt_engine_profile prof;
    prof.engine_type = TRT_ENGINE_UNIVERSAL;
    prof.preferred_gfx = TRT_GFX_D3D12;
    prof.preferred_audio = TRT_AUDIO_WASAPI;
    trt_autonomous_provision_runtime(nullptr, &prof);

    for (const char* module : {"discord_game_sdk.dll", "nvngx_dlss.dll", "sentry.dll"}) {
      CHECK(trt_module_route(module) == TRT_ROUTE_MISSING,
            "unimplemented middleware does not preempt a real provider");
    }

    // Verify expanded Oodle, Bink, Wwise, EOS, and GOG Galaxy built-in modules
    CHECK(trt_module_route("oo2core_9_win64.dll") == TRT_ROUTE_BUILTIN,
          "oo2core_9_win64.dll is provisioned as BUILTIN");
    CHECK(trt_module_route("oo2core_7_win64.dll") == TRT_ROUTE_BUILTIN,
          "oo2core_7_win64.dll is provisioned as BUILTIN");
    CHECK(trt_module_route("binkw32.dll") == TRT_ROUTE_BUILTIN,
          "binkw32.dll is provisioned as BUILTIN");
    CHECK(trt_module_route("AkSoundEngineP.dll") == TRT_ROUTE_BUILTIN,
          "AkSoundEngineP.dll is provisioned as BUILTIN");
    CHECK(trt_module_route("eossdk-win64-shipping.dll") == TRT_ROUTE_BUILTIN,
          "EOSSDK-Win64-Shipping.dll is provisioned as BUILTIN");
    CHECK(trt_module_route("Galaxy64.dll") == TRT_ROUTE_BUILTIN,
          "Galaxy64.dll is provisioned as BUILTIN");

    // Test image alias registration
    trt_image fake_img = {};
    fake_img.base = (u8*)0x140000000ull;
    fake_img.size_of_image = 0x100000;
    trt_images_register_main(&fake_img, "Binaries\\Win64\\ShippingGame.exe");
    trt_images_register_alias(&fake_img, "Launcher.exe");

    CHECK(trt_images_find("ShippingGame.exe") == &fake_img,
          "trt_images_find resolves primary image name");
    CHECK(trt_images_find("Launcher.exe") == &fake_img,
          "trt_images_find resolves registered alias");
  }

  printf("== 14. Native Game DLL Preservation (No Blanket Stubbing) ==\n");
  {
    std::string cup_dir = "tests/out/test_synth_cuphead";
    mkdir(cup_dir.c_str(), 0755);
    make_pe_with_imports(cup_dir + "/Cuphead.exe", 0x20b,
                         {"UnityPlayer.dll", "d3d11.dll", "missing_middleware.dll"});
    make_fake_file(cup_dir + "/UnityPlayer.dll", "fake unity player binary");

    trt_engine_profile cup_prof;
    CHECK(trt_synthesize_engine(cup_dir.c_str(), "Cuphead.exe", &cup_prof),
          "synthesized engine profile for Cuphead fixture");
    CHECK(cup_prof.engine_type == TRT_ENGINE_UNITY_MONO,
          "classified as Unity Mono");
    CHECK(trt_autonomous_provision_runtime(&be, &cup_prof) == 0,
          "provisioned runtime for Cuphead fixture");

    // UnityPlayer.dll exists physically on disk: MUST NOT be stubbed!
    CHECK(trt_module_route("unityplayer.dll") != TRT_ROUTE_STUB,
          "UnityPlayer.dll is preserved as native DLL and NOT stubbed out");
    CHECK(trt_module_route("missing_middleware.dll") == TRT_ROUTE_MISSING,
          "an unavailable external module stays missing");
  }

  printf("== 15. Payload Closure & Launch Preflight ==\n");
  {
    // A Unity Mono game whose engine data sits beside the exe: payload closes.
    std::string ok_dir = "tests/out/test_synth_payload_ok";
    mkdir(ok_dir.c_str(), 0755);
    std::string ok_data = ok_dir + "/Payload_Data";
    mkdir(ok_data.c_str(), 0755);
    std::string ok_managed = ok_data + "/Managed";
    mkdir(ok_managed.c_str(), 0755);
    make_pe_with_imports(ok_dir + "/Payload.exe", 0x20b,
                         {"unityplayer.dll", "d3d11.dll"});
    make_fake_file(ok_managed + "/Assembly-CSharp.dll", "fake managed assembly");

    trt_engine_profile ok_prof;
    CHECK(trt_synthesize_engine(ok_dir.c_str(), "Payload.exe", &ok_prof),
          "synthesized payload fixture");
    CHECK(ok_prof.payload_complete && ok_prof.missing_payload.empty(),
          "engine payload closure: complete when *_Data/Managed is present");

    // The same exe with the data folder removed: classified, but refused by
    // name before the engine's own "Failed to load mono".
    std::string bare_dir = "tests/out/test_synth_payload_bare";
    mkdir(bare_dir.c_str(), 0755);
    make_pe_with_imports(bare_dir + "/Payload.exe", 0x20b,
                         {"unityplayer.dll", "d3d11.dll"});
    trt_engine_profile bare_prof;
    CHECK(trt_synthesize_engine(bare_dir.c_str(), "Payload.exe", &bare_prof),
          "synthesized exe-only fixture (classification still succeeds)");
    CHECK(!bare_prof.payload_complete && !bare_prof.missing_payload.empty(),
          "payload closure: incomplete without *_Data");
    bool names_data = false;
    for (const auto& m : bare_prof.missing_payload)
      if (m.find("Payload_Data") != std::string::npos) names_data = true;
    CHECK(names_data, "missing payload entry names the expected *_Data folder");

    // Preflight: no caps = named blockers, all caps = runnable, and the
    // verdict is persisted next to the profile.
    trt_host_caps none{};
    trt_preflight_report rep;
    const int nblock = trt_engine_launch_preflight(&ok_prof, &none, &rep);
    CHECK(nblock > 0 && !rep.runnable, "preflight reports blockers with no host caps");
    bool names_device = false, names_payload = false;
    for (const auto& l : rep.lines) {
      if (l.find("no_gfx_device") != std::string::npos) names_device = true;
      if (l.find("payload_missing") != std::string::npos) names_payload = true;
    }
    CHECK(names_device, "preflight names the missing D3D11 host device");
    CHECK(!names_payload, "preflight does not invent a payload blocker when the data is present");

    trt_host_caps full{};
    full.d3d11_device = 1;
    full.d3d12_device = 1;
    full.mono_host = 1;
    full.compositor = 1;
    full.audio_device = 1;
    trt_preflight_report rep2;
    CHECK(trt_engine_launch_preflight(&ok_prof, &full, &rep2) == 0 && rep2.runnable,
          "preflight reports launchable when the host has every capability");

    auto gl_prof = ok_prof;
    gl_prof.preferred_gfx = TRT_GFX_OPENGL;
    trt_preflight_report gl_rep;
    trt_gl_set_hooks(nullptr);
    full.opengl_renderer = trt_gl_render_hooks_present();
    CHECK(trt_engine_launch_preflight(&gl_prof, &full, &gl_rep) > 0 &&
              (gl_rep.blockers & TRT_PRE_GFX_NOT_EXECUTED),
          "OpenGL preflight reports the missing render backend");
    trt_gl_hooks gl_hooks{};
    gl_hooks.draw = [](const trt_gl_draw_cmd*, void*) {};
    gl_hooks.swap_buffers = [](u64, void*) {};
    trt_gl_set_hooks(&gl_hooks);
    full.opengl_renderer = trt_gl_render_hooks_present();
    CHECK(trt_engine_launch_preflight(&gl_prof, &full, &gl_rep) == 0 &&
              gl_rep.runnable,
          "OpenGL preflight accepts a live render and present backend");
    trt_gl_set_hooks(nullptr);

    trt_preflight_report rep3;
    CHECK(trt_engine_launch_preflight(&bare_prof, &full, &rep3) > 0,
          "preflight blocks the exe-only fixture");
    bool bare_named = false;
    for (const auto& l : rep3.lines)
      if (l.find("payload_missing") != std::string::npos &&
          l.find("Payload_Data") != std::string::npos)
        bare_named = true;
    CHECK(bare_named, "preflight line names the missing engine data");

    std::ifstream pj(ok_dir + "/.tiramisu/profile.json");
    std::string json((std::istreambuf_iterator<char>(pj)),
                     std::istreambuf_iterator<char>());
    CHECK(json.find("\"preflight\"") != std::string::npos,
          "profile.json carries the preflight verdict");
    CHECK(json.find("\"payload_complete\": true") != std::string::npos,
          "profile.json records payload completeness");
  }

  printf("== 16. Screen Synthesis & Engine User Settings ==\n");
  {
    // Unreal: args carry the size, and the engine's own settings file does too
    // (Unreal is the engine that reads GameUserSettings.ini back on start).
    std::string ue_dir = "tests/out/test_synth_ue_screen";
    mkdir(ue_dir.c_str(), 0755);
    make_pe_with_imports(ue_dir + "/Game-Win64-Shipping.exe", 0x20b,
                         {"d3d11.dll", "xaudio2_9.dll"});
    make_fake_file(ue_dir + "/pakchunk0-WindowsNoEditor.pak", "fake pak");

    trt_engine_profile ue;
    CHECK(trt_synthesize_engine(ue_dir.c_str(), "Game-Win64-Shipping.exe", &ue),
          "synthesized Unreal fixture for screen synthesis");
    CHECK(ue.engine_type == TRT_ENGINE_UNREAL, "classified the fixture as Unreal");
    CHECK(ue.payload_complete, "Unreal payload closes on a .pak");
    CHECK(ue.screen_width == 1280 && ue.screen_height == 720,
          "default screen is 1280x720 (console budget)");
    bool has_resx = false, has_resy = false, has_windowed = false;
    for (const auto& a : ue.cmdline_args) {
      if (a == "-ResX=1280") has_resx = true;
      if (a == "-ResY=720") has_resy = true;
      if (a == "-windowed") has_windowed = true;
    }
    CHECK(has_resx && has_resy && has_windowed,
          "Unreal args carry -ResX/-ResY/-windowed");

    char drive_template[] = "tests/out/test_synth_drive_c_XXXXXX";
    const char* drive = mkdtemp(drive_template);
    CHECK(drive != nullptr, "created an isolated guest drive");
    char ini_path[1200] = {};
    CHECK(trt_engine_write_user_settings(&ue, drive, ini_path,
                                         sizeof(ini_path)) == 1,
          "wrote the Unreal user settings file");
    CHECK(strstr(ini_path, "GameUserSettings.ini") != nullptr,
          "settings path names GameUserSettings.ini");
    std::ifstream uf(ini_path);
    std::string body((std::istreambuf_iterator<char>(uf)),
                     std::istreambuf_iterator<char>());
    CHECK(body.find("ResolutionSizeX=1280") != std::string::npos &&
              body.find("ResolutionSizeY=720") != std::string::npos,
          "settings file carries the synthesized resolution");
    CHECK(body.find("FullscreenMode=2") != std::string::npos,
          "settings file asks for windowed mode");

    constexpr const char* saved = "[CustomSettings]\nSavedPreference=42\n";
    {
      std::ofstream settings(ini_path, std::ios::trunc);
      settings << saved;
      CHECK(settings.good(), "wrote the game's saved preferences");
    }
    CHECK(trt_engine_write_user_settings(&ue, drive, nullptr, 0) == 0,
          "later Play preserves both existing Unreal settings files");
    std::ifstream persisted(ini_path);
    CHECK(std::string(std::istreambuf_iterator<char>(persisted), {}) == saved,
          "later Play preserves the game's preferences byte for byte");

    // Unity: same screen on the command line, and TRT_SCREEN overrides it.
    std::string u_dir = "tests/out/test_synth_unity_screen";
    mkdir(u_dir.c_str(), 0755);
    make_pe_with_imports(u_dir + "/U.exe", 0x20b,
                         {"unityplayer.dll", "d3d11.dll"});
    std::string u_data = u_dir + "/U_Data";
    mkdir(u_data.c_str(), 0755);
    std::string u_managed = u_data + "/Managed";
    mkdir(u_managed.c_str(), 0755);
    make_fake_file(u_managed + "/Assembly-CSharp.dll", "fake managed assembly");

    trt_engine_profile un;
    CHECK(trt_synthesize_engine(u_dir.c_str(), "U.exe", &un),
          "synthesized Unity fixture for screen synthesis");
    bool has_w = false, has_h = false, has_fs = false;
    for (const auto& a : un.cmdline_args) {
      if (a == "-screen-width") has_w = true;
      if (a == "-screen-height") has_h = true;
      if (a == "-screen-fullscreen") has_fs = true;
    }
    CHECK(has_w && has_h && has_fs,
          "Unity args carry -screen-width/-screen-height/-screen-fullscreen");

    setenv("TRT_SCREEN", "1920x1080", 1);
    trt_engine_profile un2;
    CHECK(trt_synthesize_engine(u_dir.c_str(), "U.exe", &un2) &&
              un2.screen_width == 1920 && un2.screen_height == 1080,
          "TRT_SCREEN=1920x1080 overrides the synthesized screen");
    unsetenv("TRT_SCREEN");

    char no_ini[256] = {};
    CHECK(trt_engine_write_user_settings(&un2, drive, no_ini,
                                         sizeof(no_ini)) == 0,
          "no user-settings file is invented for an engine that has none");
  }

  printf("== 17. Steam Install Readiness ==\n");
  {
    using longan::inspect_steam_install;
    using longan::steam_install_state;
    namespace fs = std::filesystem;
    char install_template[] = "tests/out/test_steam_install_XXXXXX";
    const char* install = mkdtemp(install_template);
    CHECK(install != nullptr, "created isolated Steam install");
    const fs::path root(install);
    const fs::path metadata = root / ".tiramisu";
    const fs::path launch = metadata / "steam-launch-exe.txt";
    const fs::path pending = metadata / "steam-install-pending";
    CHECK(fs::create_directories(root / "Binaries" / "Win64"), "created nested executable directory");
    CHECK(fs::create_directory(metadata), "created Steam metadata directory");

    make_fake_file((root / "steam_appid.txt").string(), "987654\n");
    CHECK(inspect_steam_install(install).state == steam_install_state::missing_executable,
          "an AppID alone does not make a downloaded title ready to play");
    make_pe_with_imports((root / "dxsetup.exe").string(), 0x20b, {"kernel32.dll"});
    CHECK(inspect_steam_install(install).state == steam_install_state::missing_executable,
          "a redistributable installer does not make the title playable");
    make_pe_with_imports((root / "Binaries" / "Win64" / "Title.exe").string(), 0x20b,
                         {"d3d11.dll"});
    const auto legacy = inspect_steam_install(install);
    CHECK(legacy.app_id == 987654 && legacy.state == steam_install_state::ready &&
              legacy.executable == "Binaries\\Win64\\Title.exe",
          "a completed legacy install discovers its real nested executable");
    CHECK(!fs::exists(metadata / "profile.json"), "readiness never writes an engine profile");

    make_fake_file(launch.string(), "Missing.exe\n.\n-option\n");
    CHECK(inspect_steam_install(install).state == steam_install_state::missing_executable,
          "stale published launch metadata cannot fall back to other executables");
    make_fake_file((root / "Truncated.exe").string(), "MZ");
    make_fake_file(launch.string(), "Truncated.exe\n");
    CHECK(inspect_steam_install(install).state == steam_install_state::missing_executable,
          "a partial executable does not make an install ready");
    make_fake_file(launch.string(), "..\\OtherGame.exe\n");
    CHECK(inspect_steam_install(install).state == steam_install_state::missing_executable,
          "launch metadata cannot escape the install");
    make_fake_file(launch.string(), "Binaries\\Win64\\Title.exe\r\n.\r\n-option\r\n");
    make_fake_file(pending.string(), "987654\n");
    CHECK(inspect_steam_install(install).state == steam_install_state::incomplete,
          "a pending install remains hidden even after its executable arrives");
    CHECK(fs::remove(pending), "simulated final publication of the Steam install");
    const auto published = inspect_steam_install(install);
    CHECK(published.app_id == 987654 && published.state == steam_install_state::ready &&
              published.executable == "Binaries\\Win64\\Title.exe",
          "the published launch metadata becomes ready once");
  }

  printf("== 18. Startup and delay-loaded graphics ==\n");
  {
    char directory[] = "tests/out/test_optional_graphics_XXXXXX";
    CHECK(mkdtemp(directory), "created isolated delay import fixture");
    const std::string root = directory;
    make_pe_with_imports(root + "/Game.exe", 0x10b, {"d3d9.dll"}, 0x400, "d3d12.dll");
    trt_engine_profile profile;
    CHECK(trt_synthesize_engine(root.c_str(), "Game.exe", &profile) &&
              profile.preferred_gfx == TRT_GFX_D3D9,
          "a delayed D3D12 import cannot override the startup D3D9 API");

    make_pe_with_imports(root + "/Game.exe", 0x10b, {"RenderCore.dll"}, 0x400, "OptionalFx.dll");
    make_pe_with_imports(root + "/RenderCore.dll", 0x10b, {"d3d9.dll"});
    make_pe_with_imports(root + "/OptionalFx.dll", 0x10b, {"d3d12.dll"});
    CHECK(trt_synthesize_engine(root.c_str(), "Game.exe", &profile) &&
              profile.preferred_gfx == TRT_GFX_D3D9,
          "ordinary imports of a delay-loaded DLL stay optional");
    CHECK(std::binary_search(profile.imported_modules.begin(), profile.imported_modules.end(),
                             std::string("d3d12.dll")),
          "optional capabilities remain visible for diagnosis");
    CHECK(profile.import_roles_scanned &&
              !std::binary_search(profile.startup_modules.begin(), profile.startup_modules.end(),
                                  std::string("d3d12.dll")),
          "optional capabilities are absent from the startup import closure");

    make_pe_with_imports(root + "/RenderCore.dll", 0x10b, {"d3d9.dll", "OptionalFx.dll"});
    CHECK(trt_synthesize_engine(root.c_str(), "Game.exe", &profile) &&
              std::binary_search(profile.startup_modules.begin(), profile.startup_modules.end(),
                                 std::string("d3d12.dll")),
          "a dependency referenced by both graphs remains in the startup graph");

    make_pe_with_imports(root + "/Game.exe", 0x10b, {"kernel32.dll"}, 0x400, "d3d12.dll");
    CHECK(trt_synthesize_engine(root.c_str(), "Game.exe", &profile) &&
              profile.preferred_gfx == TRT_GFX_D3D12 && profile.graphics_from_delay_imports,
          "a delay-only renderer is preserved as a hint");
    trt_host_caps caps{};
    caps.compositor = caps.d3d11_device = caps.audio_device = 1;
    trt_preflight_report report;
    CHECK(trt_engine_launch_preflight(&profile, &caps, &report) == 0 && report.runnable &&
              std::any_of(report.notes.begin(), report.notes.end(), [](const std::string& note) {
                return note.find("Delay-loaded graphics hint") != std::string::npos;
              }),
          "an unavailable delay-only renderer yields a preflight note");

  }

  printf("ALL ENGINE SYNTHESIZER CHECKS PASSED CLEANLY.\n");
  return 0;
}
